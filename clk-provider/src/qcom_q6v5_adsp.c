// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm Technology Inc. ADSP Peripheral Image Loader for SDM845.
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 */

#include <linux/of_address.h>
#include <linux/firmware/qcom/qcom_pas.h>
#include <linux/elf.h>
#include <linux/arm-smccc.h>
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>
#include <linux/soc/qcom/mdt_loader.h>
#include <linux/soc/qcom/smem.h>
#include <linux/soc/qcom/smem_state.h>
#include <linux/soc/qcom/socinfo.h>

#include "qcom_common.h"
#include "qcom_pil_info.h"
#include "qcom_q6v5.h"
#include "remoteproc_internal.h"

/* time out value */
#define ACK_TIMEOUT			1000
#define ACK_TIMEOUT_US			1000000
#define BOOT_FSM_TIMEOUT		10000
/* mask values */
#define EVB_MASK			GENMASK(27, 4)
/*QDSP6SS register offsets*/
#define RST_EVB_REG			0x10
#define CORE_START_REG			0x400
#define BOOT_CMD_REG			0x404
#define BOOT_STATUS_REG			0x408
#define RET_CFG_REG			0x1C
/*TCSR register offsets*/
#define LPASS_MASTER_IDLE_REG		0x8
#define LPASS_HALTACK_REG		0x4
#define LPASS_PWR_ON_REG		0x10
#define LPASS_HALTREQ_REG		0x0

#define SID_MASK_DEFAULT        0xF

#define QDSP6SS_XO_CBCR		0x38
#define QDSP6SS_CORE_CBCR	0x20
#define QDSP6SS_SLEEP_CBCR	0x3c

#define LPASS_BOOT_CORE_START	BIT(0)
#define LPASS_BOOT_CMD_START	BIT(0)
#define LPASS_EFUSE_Q6SS_EVB_SEL 0x0

#define X1E_NR_BLK_MAX		20

/* X1E80100 at EL2: second (DTB) image and the PAS ids UEFI may have used */
struct x1e_dsp_data {
	bool is_cdsp;
	u32 core_rcg;		/* core RCG in the Q6SS clock controller */
	int pas_id;
	int dtb_pas_id;
	int lite_pas_id;
	int lite_dtb_pas_id;
	const char *dtb_firmware_name;
};

struct adsp_pil_data {
	int crash_reason_smem;
	const char *firmware_name;

	const char *ssr_name;
	const char *sysmon_name;
	int ssctl_id;
	bool is_wpss;
	bool has_iommu;
	bool auto_boot;

	const char **clk_ids;
	int num_clks;
	const char **pd_names;
	unsigned int num_pds;
	const char *load_state;

	const struct x1e_dsp_data *x1e;
};

struct qcom_adsp {
	struct device *dev;
	struct rproc *rproc;

	struct qcom_q6v5 q6v5;

	struct clk *xo;
	struct clk *core_clk;	/* X1E: Q6 core clock */
	bool core_clk_on;

	int num_clks;
	struct clk_bulk_data *clks;

	void __iomem *qdsp6ss_base;
	void __iomem *lpass_efuse;

	struct reset_control *pdc_sync_reset;
	struct reset_control *restart;

	struct regmap *halt_map;
	unsigned int halt_lpass;

	int crash_reason_smem;
	const char *info_name;

	struct completion start_done;
	struct completion stop_done;

	phys_addr_t mem_phys;
	unsigned long iova;
	phys_addr_t mem_reloc;
	void __iomem *mem_region;
	size_t mem_size;
	bool has_iommu;

	struct dev_pm_domain_list *pd_list;

	struct qcom_rproc_glink glink_subdev;
	struct qcom_rproc_pdm pdm_subdev;
	struct qcom_rproc_ssr ssr_subdev;
	struct qcom_sysmon *sysmon;

	int (*shutdown)(struct qcom_adsp *adsp);

	/* X1E80100, see qcom_q6v5_adsp_x1e.h */
	const struct x1e_dsp_data *x1e;
	const struct firmware *firmware;
	const struct firmware *dtb_firmware;
	const char *dtb_firmware_name;
	phys_addr_t dtb_mem_phys;
	phys_addr_t dtb_mem_reloc;
	void __iomem *dtb_mem_region;
	size_t dtb_mem_size;
	u32 dtb_chip_id;
	u32 dtb_chip_version;
	u32 dtb_platform_info;
	unsigned long extra_mapped;
	DECLARE_BITMAP(qhee_mapped, 128);
	void __iomem *x1e_base[X1E_NR_BLK_MAX];
};

static int qcom_rproc_pds_attach(struct qcom_adsp *adsp, const char **pd_names,
				 unsigned int num_pds)
{
	struct device *dev = adsp->dev;
	struct dev_pm_domain_attach_data pd_data = {
		.pd_names = pd_names,
		.num_pd_names = num_pds,
	};
	int ret;

	/* Handle single power domain */
	if (dev->pm_domain)
		goto out;

	if (!pd_names)
		return 0;

	ret = dev_pm_domain_attach_list(dev, &pd_data, &adsp->pd_list);
	if (ret < 0)
		return ret;

out:
	pm_runtime_enable(dev);
	return 0;
}

static void qcom_rproc_pds_detach(struct qcom_adsp *adsp)
{
	struct device *dev = adsp->dev;
	struct dev_pm_domain_list *pds = adsp->pd_list;

	dev_pm_domain_detach_list(pds);

	if (dev->pm_domain || pds)
		pm_runtime_disable(adsp->dev);
}

static int qcom_rproc_pds_enable(struct qcom_adsp *adsp)
{
	struct device *dev = adsp->dev;
	struct dev_pm_domain_list *pds = adsp->pd_list;
	int ret, i = 0;

	if (!dev->pm_domain && !pds)
		return 0;

	if (dev->pm_domain)
		dev_pm_genpd_set_performance_state(dev, INT_MAX);

	while (pds && i < pds->num_pds) {
		dev_pm_genpd_set_performance_state(pds->pd_devs[i], INT_MAX);
		i++;
	}

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		while (pds && i > 0) {
			i--;
			dev_pm_genpd_set_performance_state(pds->pd_devs[i], 0);
		}

		if (dev->pm_domain)
			dev_pm_genpd_set_performance_state(dev, 0);
	}

	return ret;
}

static void qcom_rproc_pds_disable(struct qcom_adsp *adsp)
{
	struct device *dev = adsp->dev;
	struct dev_pm_domain_list *pds = adsp->pd_list;
	int i = 0;

	if (!dev->pm_domain && !pds)
		return;

	if (dev->pm_domain)
		dev_pm_genpd_set_performance_state(dev, 0);

	while (pds && i < pds->num_pds) {
		dev_pm_genpd_set_performance_state(pds->pd_devs[i], 0);
		i++;
	}

	pm_runtime_put(dev);
}

static int qcom_wpss_shutdown(struct qcom_adsp *adsp)
{
	unsigned int val;

	regmap_write(adsp->halt_map, adsp->halt_lpass + LPASS_HALTREQ_REG, 1);

	/* Wait for halt ACK from QDSP6 */
	regmap_read_poll_timeout(adsp->halt_map,
				 adsp->halt_lpass + LPASS_HALTACK_REG, val,
				 val, 1000, ACK_TIMEOUT_US);

	/* Assert the WPSS PDC Reset */
	reset_control_assert(adsp->pdc_sync_reset);

	/* Place the WPSS processor into reset */
	reset_control_assert(adsp->restart);

	/* wait after asserting subsystem restart from AOSS */
	usleep_range(200, 205);

	/* Remove the WPSS reset */
	reset_control_deassert(adsp->restart);

	/* De-assert the WPSS PDC Reset */
	reset_control_deassert(adsp->pdc_sync_reset);

	usleep_range(100, 105);

	clk_bulk_disable_unprepare(adsp->num_clks, adsp->clks);

	regmap_write(adsp->halt_map, adsp->halt_lpass + LPASS_HALTREQ_REG, 0);

	/* Wait for halt ACK from QDSP6 */
	regmap_read_poll_timeout(adsp->halt_map,
				 adsp->halt_lpass + LPASS_HALTACK_REG, val,
				 !val, 1000, ACK_TIMEOUT_US);

	return 0;
}

static int qcom_adsp_shutdown(struct qcom_adsp *adsp)
{
	unsigned long timeout;
	unsigned int val;
	int ret;

	/* Reset the retention logic */
	val = readl(adsp->qdsp6ss_base + RET_CFG_REG);
	val |= 0x1;
	writel(val, adsp->qdsp6ss_base + RET_CFG_REG);

	clk_bulk_disable_unprepare(adsp->num_clks, adsp->clks);

	/* QDSP6 master port needs to be explicitly halted */
	ret = regmap_read(adsp->halt_map,
			adsp->halt_lpass + LPASS_PWR_ON_REG, &val);
	if (ret || !val)
		goto reset;

	ret = regmap_read(adsp->halt_map,
			adsp->halt_lpass + LPASS_MASTER_IDLE_REG,
			&val);
	if (ret || val)
		goto reset;

	regmap_write(adsp->halt_map,
			adsp->halt_lpass + LPASS_HALTREQ_REG, 1);

	/* Wait for halt ACK from QDSP6 */
	timeout = jiffies + msecs_to_jiffies(ACK_TIMEOUT);
	for (;;) {
		ret = regmap_read(adsp->halt_map,
			adsp->halt_lpass + LPASS_HALTACK_REG, &val);
		if (ret || val || time_after(jiffies, timeout))
			break;

		usleep_range(1000, 1100);
	}

	ret = regmap_read(adsp->halt_map,
			adsp->halt_lpass + LPASS_MASTER_IDLE_REG, &val);
	if (ret || !val)
		dev_err(adsp->dev, "port failed halt\n");

reset:
	/* Assert the LPASS PDC Reset */
	reset_control_assert(adsp->pdc_sync_reset);
	/* Place the LPASS processor into reset */
	reset_control_assert(adsp->restart);
	/* wait after asserting subsystem restart from AOSS */
	usleep_range(200, 300);

	/* Clear the halt request for the AXIM and AHBM for Q6 */
	regmap_write(adsp->halt_map, adsp->halt_lpass + LPASS_HALTREQ_REG, 0);

	/* De-assert the LPASS PDC Reset */
	reset_control_deassert(adsp->pdc_sync_reset);
	/* Remove the LPASS reset */
	reset_control_deassert(adsp->restart);
	/* wait after de-asserting subsystem restart from AOSS */
	usleep_range(200, 300);

	return 0;
}

static int adsp_load(struct rproc *rproc, const struct firmware *fw)
{
	struct qcom_adsp *adsp = rproc->priv;
	int ret;

	ret = qcom_mdt_load_no_init(adsp->dev, fw, rproc->firmware,
				    (__force void *)adsp->mem_region, adsp->mem_phys,
				    adsp->mem_size, &adsp->mem_reloc);
	if (ret)
		return ret;

	qcom_pil_info_store(adsp->info_name, adsp->mem_phys, adsp->mem_size);

	return 0;
}

static void adsp_unmap_carveout(struct rproc *rproc)
{
	struct qcom_adsp *adsp = rproc->priv;

	if (adsp->has_iommu)
		iommu_unmap(rproc->domain, adsp->iova, adsp->mem_size);
}

static int adsp_map_carveout(struct rproc *rproc)
{
	struct qcom_adsp *adsp = rproc->priv;
	struct of_phandle_args args;
	long long sid;
	int ret;

	if (!adsp->has_iommu)
		return 0;

	if (!rproc->domain)
		return -EINVAL;

	ret = of_parse_phandle_with_args(adsp->dev->of_node, "iommus", "#iommu-cells", 0, &args);
	if (ret < 0)
		return ret;

	sid = args.args[0] & SID_MASK_DEFAULT;
	of_node_put(args.np);

	/* Add SID configuration for ADSP Firmware to SMMU */
	adsp->iova = adsp->mem_phys | (sid << 32);

	ret = iommu_map(rproc->domain, adsp->iova, adsp->mem_phys,
			adsp->mem_size,	IOMMU_READ | IOMMU_WRITE,
			GFP_KERNEL);
	if (ret) {
		dev_err(adsp->dev, "Unable to map ADSP Physical Memory\n");
		return ret;
	}

	return 0;
}

static int adsp_start(struct rproc *rproc)
{
	struct qcom_adsp *adsp = rproc->priv;
	int ret;
	unsigned int val;

	ret = qcom_q6v5_prepare(&adsp->q6v5);
	if (ret)
		return ret;

	ret = adsp_map_carveout(rproc);
	if (ret) {
		dev_err(adsp->dev, "ADSP smmu mapping failed\n");
		goto disable_irqs;
	}

	ret = clk_prepare_enable(adsp->xo);
	if (ret)
		goto adsp_smmu_unmap;

	ret = qcom_rproc_pds_enable(adsp);
	if (ret < 0)
		goto disable_xo_clk;

	ret = clk_bulk_prepare_enable(adsp->num_clks, adsp->clks);
	if (ret) {
		dev_err(adsp->dev, "adsp clk_enable failed\n");
		goto disable_power_domain;
	}

	/* Enable the XO clock */
	writel(1, adsp->qdsp6ss_base + QDSP6SS_XO_CBCR);

	/* Enable the QDSP6SS sleep clock */
	writel(1, adsp->qdsp6ss_base + QDSP6SS_SLEEP_CBCR);

	/* Enable the QDSP6 core clock */
	writel(1, adsp->qdsp6ss_base + QDSP6SS_CORE_CBCR);

	/* Program boot address */
	writel(adsp->mem_phys >> 4, adsp->qdsp6ss_base + RST_EVB_REG);

	if (adsp->lpass_efuse)
		writel(LPASS_EFUSE_Q6SS_EVB_SEL, adsp->lpass_efuse);

	/* De-assert QDSP6 stop core. QDSP6 will execute after out of reset */
	writel(LPASS_BOOT_CORE_START, adsp->qdsp6ss_base + CORE_START_REG);

	/* Trigger boot FSM to start QDSP6 */
	writel(LPASS_BOOT_CMD_START, adsp->qdsp6ss_base + BOOT_CMD_REG);

	/* Wait for core to come out of reset */
	ret = readl_poll_timeout(adsp->qdsp6ss_base + BOOT_STATUS_REG,
			val, (val & BIT(0)) != 0, 10, BOOT_FSM_TIMEOUT);
	if (ret) {
		dev_err(adsp->dev, "failed to bootup adsp\n");
		goto disable_adsp_clks;
	}

	ret = qcom_q6v5_wait_for_start(&adsp->q6v5, msecs_to_jiffies(5 * HZ));
	if (ret == -ETIMEDOUT) {
		dev_err(adsp->dev, "start timed out\n");
		goto disable_adsp_clks;
	}

	return 0;

disable_adsp_clks:
	clk_bulk_disable_unprepare(adsp->num_clks, adsp->clks);
disable_power_domain:
	qcom_rproc_pds_disable(adsp);
disable_xo_clk:
	clk_disable_unprepare(adsp->xo);
adsp_smmu_unmap:
	adsp_unmap_carveout(rproc);
disable_irqs:
	qcom_q6v5_unprepare(&adsp->q6v5);

	return ret;
}

static void qcom_adsp_pil_handover(struct qcom_q6v5 *q6v5)
{
	struct qcom_adsp *adsp = container_of(q6v5, struct qcom_adsp, q6v5);

	clk_disable_unprepare(adsp->xo);
	qcom_rproc_pds_disable(adsp);
}

static int adsp_stop(struct rproc *rproc)
{
	struct qcom_adsp *adsp = rproc->priv;
	int handover;
	int ret;

	ret = qcom_q6v5_request_stop(&adsp->q6v5, adsp->sysmon);
	if (ret == -ETIMEDOUT)
		dev_err(adsp->dev, "timed out on wait\n");

	ret = adsp->shutdown(adsp);
	if (ret)
		dev_err(adsp->dev, "failed to shutdown: %d\n", ret);

	adsp_unmap_carveout(rproc);

	handover = qcom_q6v5_unprepare(&adsp->q6v5);
	if (handover)
		qcom_adsp_pil_handover(&adsp->q6v5);

	return ret;
}

static void *adsp_da_to_va(struct rproc *rproc, u64 da, size_t len, bool *is_iomem)
{
	struct qcom_adsp *adsp = rproc->priv;
	int offset;

	offset = da - adsp->mem_reloc;
	if (offset < 0 || offset + len > adsp->mem_size)
		return NULL;

	return (__force void *)adsp->mem_region + offset;
}

static int adsp_parse_firmware(struct rproc *rproc, const struct firmware *fw)
{
	struct qcom_adsp *adsp = rproc->priv;
	int ret;

	ret = qcom_register_dump_segments(rproc, fw);
	if (ret) {
		dev_err(&rproc->dev, "Error in registering dump segments\n");
		return ret;
	}

	if (adsp->has_iommu) {
		ret = rproc_elf_load_rsc_table(rproc, fw);
		if (ret) {
			dev_err(&rproc->dev, "Error in loading resource table\n");
			return ret;
		}
	}
	return 0;
}

static unsigned long adsp_panic(struct rproc *rproc)
{
	struct qcom_adsp *adsp = rproc->priv;

	return qcom_q6v5_panic(&adsp->q6v5);
}

static const struct rproc_ops adsp_ops = {
	.start = adsp_start,
	.stop = adsp_stop,
	.da_to_va = adsp_da_to_va,
	.parse_fw = adsp_parse_firmware,
	.load = adsp_load,
	.panic = adsp_panic,
};

/*
 * X1E80100 ADSP/CDSP for Linux running at EL2 without a hypervisor. The clock/PLL
 * bring-up, boot and stop sequences are the ones the Qualcomm hypervisor (QHEE .367)
 * normally runs. The Q6 core PLL and RCG come from the q6sscc clock provider; several
 * of the other registers belong to clock controllers that have, or should have, their
 * own drivers.
 */

#define QHEE_CDSP_MAP_MAX	128

static bool x1e_has_domain(struct rproc *rproc)
{
	return rproc->has_iommu && rproc->domain;
}

static void x1e_unmap_carveout(struct rproc *rproc, phys_addr_t mem_phys, size_t size)
{
	if (x1e_has_domain(rproc))
		iommu_unmap(rproc->domain, mem_phys, size);
}

/* Identity-map shared regions a hypervisor would normally grant the DSP */
static const char * const pas_extra_regions[] = {
	"qcom,smem",
	"qcom,cmd-db",
};

static int x1e_extra_resource(int i, struct resource *res)
{
	struct device_node *np;
	int ret;

	np = of_find_compatible_node(NULL, NULL, pas_extra_regions[i]);
	if (!np)
		return -ENOENT;
	ret = of_address_to_resource(np, 0, res);
	of_node_put(np);
	return ret;
}

static void x1e_map_extra(struct qcom_adsp *pas, struct rproc *rproc, bool map)
{
	struct resource res;
	int i, ret;

	if (!x1e_has_domain(rproc))
		return;
	for (i = 0; i < ARRAY_SIZE(pas_extra_regions); i++) {
		if (map == !!(pas->extra_mapped & BIT(i)))
			continue;
		if (x1e_extra_resource(i, &res)) {
			if (map)
				dev_warn(&rproc->dev, "no %s region to map for the DSP\n",
					 pas_extra_regions[i]);
			continue;
		}
		if (map) {
			ret = iommu_map(rproc->domain, res.start, res.start, resource_size(&res),
					IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
			if (ret)
				dev_warn(&rproc->dev, "failed to map %s %pR: %d\n",
					 pas_extra_regions[i], &res, ret);
			else
				pas->extra_mapped |= BIT(i);
		} else {
			iommu_unmap(rproc->domain, res.start, resource_size(&res));
			pas->extra_mapped &= ~BIT(i);
		}
	}
}

/*
 * CDSP (VMID 0x1e) static apertures granted by QHEE: the 103 of the ASUS .367 build
 * (table at ELF VA 0xccc08) plus the four read-only SPMI windows that newer builds
 * (Surface UEFI 175.222.235) add, 107 in all.
 * {pa, size, perm}, perm bit2 = read, bit1 = write. Names come from the hardware
 * region directory in the CDSP firmware. "(boot)" marks the entries the CDSP cannot
 * boot without (bisected on hardware); the others are used at run time, e.g. the
 * LLCC registers as soon as an HTP session opens, so the whole table is kept.
 */
static const struct { u64 pa; u64 size; u32 perm; } qhee_cdsp_map[] = {
	{ 0x100000, 0x1000, 0x4 },	/*   0 GCC_GPLL0_CM_PLL_LUCID_OLE */
	{ 0x101000, 0x1000, 0x4 },	/*   1 GCC_GPLL1_CM_PLL_LUCID_OLE */
	{ 0x102000, 0x1000, 0x4 },	/*   2 GCC_GPLL2_CM_PLL_LUCID_OLE */
	{ 0x103000, 0x1000, 0x4 },	/*   3 GCC_GPLL3_CM_PLL_LUCID_OLE */
	{ 0x104000, 0x1000, 0x4 },	/*   4 GCC_GPLL4_CM_PLL_LUCID_OLE */
	{ 0x105000, 0x1000, 0x4 },	/*   5 GCC_GPLL5_CM_PLL_LUCID_OLE */
	{ 0x106000, 0x1000, 0x4 },	/*   6 GCC_GPLL6_CM_PLL_LUCID_OLE */
	{ 0x107000, 0x1000, 0x4 },	/*   7 GCC_GPLL7_CM_PLL_LUCID_OLE */
	{ 0x108000, 0x1000, 0x4 },	/*   8 GCC_GPLL8_CM_PLL_LUCID_OLE */
	{ 0x109000, 0x1000, 0x4 },	/*   9 GCC_GPLL9_CM_PLL_LUCID_OLE */
	{ 0x10a000, 0x1000, 0x4 },	/*  10 GCC_GPLL10_CM_PLL_ZONDA_OLE */
	{ 0x10b000, 0x1000, 0x4 },	/*  11 GCC_GPLL11_CM_PLL_ZONDA_OLE */
	{ 0x118000, 0x1000, 0x6 },	/*  12 GCC QUPv3 wrapper 1 (boot) */
	{ 0x11e000, 0x1000, 0x6 },	/*  13 GCC QUPv3 wrapper 2 (boot) */
	{ 0x123000, 0x1000, 0x6 },	/*  14 GCC_CLK_CTL_REG */
	{ 0x138000, 0x1000, 0x4 },	/*  15 GCC_CLK_CTL_REG */
	{ 0x142000, 0x1000, 0x6 },	/*  16 GCC QUPv3 wrapper 0 (boot) */
	{ 0x145000, 0x1000, 0x6 },	/*  17 GCC_CLK_CTL_REG */
	{ 0x15b000, 0x1000, 0x6 },	/*  18 GCC_CLK_CTL_REG */
	{ 0x162000, 0x1000, 0x6 },	/*  19 GCC_CLK_CTL_REG */
	{ 0x1a6000, 0x1000, 0x6 },	/*  20 GCC_CLK_CTL_REG */
	{ 0x406000, 0x1000, 0x6 },	/*  21 IPC_CORE (boot) */
	{ 0x446000, 0x1000, 0x6 },	/*  22 IPC_CORE */
	{ 0x486000, 0x1000, 0x6 },	/*  23 IPC_CORE */
	{ 0x4fc000, 0x1000, 0x4 },	/*  24 IPC_CONFIG */
	{ 0x526000, 0x7000, 0x6 },	/*  25 unnamed */
	{ 0x63c000, 0x4000, 0x6 },	/*  26 NSP_CX_CPR3 (boot) */
	{ 0x640000, 0x9000, 0x2 },	/*  27 unnamed */
	{ 0x650000, 0x9000, 0x2 },	/*  28 unnamed */
	{ 0x10ca000, 0x1000, 0x4 },	/*  29 RNG_EE10_EE10_PRNG_SUB (boot) */
	{ 0x17af000, 0x1000, 0x6 },	/*  30 MMSS NoC */
	{ 0x1f40000, 0x40000, 0x6 },	/*  31 TCSR_TCSR_MUTEX (boot) */
	{ 0x1fc0000, 0x10000, 0x4 },	/*  32 TCSR_TCSR_REGS (boot) */
	{ 0x1fd1000, 0x1a000, 0x4 },	/*  33 TCSR_TCSR_REGS (boot) */
	{ 0x1feb000, 0x1000, 0x6 },	/*  34 TCSR_TCSR_REGS (boot) */
	{ 0x1fec000, 0x4000, 0x4 },	/*  35 TCSR_TCSR_REGS */
	{ 0xb2a0000, 0x10000, 0x6 },	/*  36 RPMH_PDC_COMPUTE_COMPUTE_PDC (boot) */
	{ 0xb5f0000, 0x1000, 0x4 },	/*  37 RPMH_PDC_PDC_WRAPPER (boot) */
	{ 0xbbf0000, 0x2000, 0x4 },	/*  38 RPMH_BCM_BCM_TOP (boot) */
	{ 0xc20a000, 0x1000, 0x6 },	/*  39 RPMH_CPRF_CPRF (boot) */
	{ 0xc210000, 0x80000, 0x4 },	/*  40 AOSS_PERIPHERALS */
	{ 0xc330000, 0x1000, 0x6 },	/*  41 AOP_SS_MSG_RAM (boot) */
	{ 0xc370000, 0x1000, 0x4 },	/*  42 AOP_SS_MSG_RAM */
	{ 0xc380000, 0x1000, 0x4 },	/*  43 AOP_SS_MSG_RAM */
	{ 0xc390000, 0x1000, 0x4 },	/*  44 AOP_SS_MSG_RAM */
	{ 0xc3a0000, 0x1000, 0x4 },	/*  45 AOP_SS_MSG_RAM */
	{ 0xc3b0000, 0x1000, 0x4 },	/*  46 AOP_SS_MSG_RAM */
	{ 0xc3c0000, 0x1000, 0x4 },	/*  47 AOP_SS_MSG_RAM */
	{ 0xc3d0000, 0x1000, 0x4 },	/*  48 AOP_SS_MSG_RAM */
	{ 0xc3e0000, 0x1000, 0x4 },	/*  49 AOP_SS_MSG_RAM */
	{ 0xc3f0000, 0x1000, 0x4 },	/*  50 AOP_SS_MSG_RAM (boot) */
	{ 0xc400000, 0x3000, 0x6 },	/*  51 PMIC_ARB_CORE */
	{ 0xc406000, 0x11000, 0x4 },	/*  52 PMIC_ARB_MGPI */
	{ 0xc440000, 0x80000, 0x6 },	/*  53 PMIC_ARB_CORE_REGISTERS_OBS */
	{ 0xf000000, 0x1000000, 0x6 },	/*  54 TLMM */
	{ 0x10001000, 0x1000, 0x6 },	/*  55 QDSS_CSR */
	{ 0x10002000, 0x1000, 0x6 },	/*  56 QDSS_CXSTM_8_32_32_TRUE */
	{ 0x10041000, 0x1000, 0x6 },	/*  57 QDSS_IN_FUN0_IN_FUN0_CXATBFUNNEL_128W8SP */
	{ 0x10042000, 0x1000, 0x6 },	/*  58 QDSS_IN_FUN1_IN_FUN1_CXATBFUNNEL_128W8SP */
	{ 0x10045000, 0x1000, 0x6 },	/*  59 QDSS_MERG_FUN_MERG_FUN_CXATBFUNNEL_128W2SP */
	{ 0x10046000, 0x1000, 0x6 },	/*  60 QDSS_REPL64_REPL64_CXATBREPLICATOR_64WP */
	{ 0x10048000, 0x1000, 0x6 },	/*  61 QDSS_ETR_ETR_CXTMC_R64W32D */
	{ 0x10980000, 0x1000, 0x6 },	/*  62 QDSS_TURING_APB_TURING_TPDM0 */
	{ 0x10981000, 0x1000, 0x6 },	/*  63 QDSS_TURING_APB_TURING_TPDM1 */
	{ 0x146aa000, 0x1000, 0x6 },	/*  64 boot IMEM (shared) */
	{ 0x16000000, 0x1000000, 0x2 },	/*  65 QDSS STM stimulus */
	{ 0x20c21000, 0x1000, 0x4 },	/*  66 security/boot policy status */
	{ 0x221c2000, 0x1000, 0x4 },	/*  67 TME_FUSECONTROLLER */
	{ 0x221c8000, 0x1000, 0x4 },	/*  68 QFPROM (boot) */
	{ 0x24080000, 0x1000, 0x4 },	/*  69 DDR_REG_DDR_SS_REGS */
	{ 0x240b8000, 0x1000, 0x6 },	/*  70 CDSP_BWMON_THROTTLE_BWMON_WRAPPER_2 (boot) */
	{ 0x2500a000, 0x1000, 0x6 },	/*  71 LLCC0_TRP */
	{ 0x25011000, 0x1000, 0x6 },	/*  72 LLCC0_TRP */
	{ 0x25021000, 0x1000, 0x4 },	/*  73 LLCC0_TRP */
	{ 0x2520a000, 0x1000, 0x6 },	/*  74 LLCC1_TRP */
	{ 0x25211000, 0x1000, 0x6 },	/*  75 LLCC1_TRP */
	{ 0x25221000, 0x1000, 0x4 },	/*  76 LLCC1_TRP */
	{ 0x2540a000, 0x1000, 0x6 },	/*  77 LLCC2_TRP */
	{ 0x25411000, 0x1000, 0x6 },	/*  78 LLCC2_TRP */
	{ 0x25421000, 0x1000, 0x4 },	/*  79 LLCC2_TRP */
	{ 0x2560a000, 0x1000, 0x6 },	/*  80 LLCC3_TRP */
	{ 0x25611000, 0x1000, 0x6 },	/*  81 LLCC3_TRP */
	{ 0x25621000, 0x1000, 0x4 },	/*  82 LLCC3_TRP */
	{ 0x2580a000, 0x1000, 0x6 },	/*  83 LLCC4_TRP */
	{ 0x25811000, 0x1000, 0x6 },	/*  84 LLCC4_TRP */
	{ 0x25821000, 0x1000, 0x4 },	/*  85 LLCC4_TRP */
	{ 0x25a0a000, 0x1000, 0x6 },	/*  86 LLCC5_TRP */
	{ 0x25a11000, 0x1000, 0x6 },	/*  87 LLCC5_TRP */
	{ 0x25a21000, 0x1000, 0x4 },	/*  88 LLCC5_TRP */
	{ 0x25c0a000, 0x1000, 0x6 },	/*  89 LLCC6_TRP */
	{ 0x25c11000, 0x1000, 0x6 },	/*  90 LLCC6_TRP */
	{ 0x25c21000, 0x1000, 0x4 },	/*  91 LLCC6_TRP */
	{ 0x25e0a000, 0x1000, 0x6 },	/*  92 LLCC7_TRP */
	{ 0x25e11000, 0x1000, 0x6 },	/*  93 LLCC7_TRP */
	{ 0x25e21000, 0x1000, 0x4 },	/*  94 LLCC7_TRP */
	{ 0x2600a000, 0x1000, 0x6 },	/*  95 LLCC_BROADCAST_ORTRP */
	{ 0x26011000, 0x1000, 0x6 },	/*  96 LLCC_BROADCAST_ORTRP */
	{ 0x26021000, 0x1000, 0x4 },	/*  97 LLCC_BROADCAST_ORTRP */
	{ 0x2620a000, 0x1000, 0x6 },	/*  98 LLCC_BROADCAST_ANDTRP (boot) */
	{ 0x26211000, 0x1000, 0x6 },	/*  99 LLCC_BROADCAST_ANDTRP */
	{ 0x26221000, 0x1000, 0x4 },	/* 100 LLCC_BROADCAST_ANDTRP */
	{ 0x320cc000, 0x3000, 0x6 },	/* 101 NSP NoC */
	{ 0x322fa000, 0x1000, 0x6 },	/* 102 TURING_VAPSS_NOC_VAPSS_NOC_VAPSS_NOC */
	{ 0x0c42c000, 0x5000, 0x4 },	/* 103 SPMI0_SPMI_CFG_TOP */
	{ 0x0c431000, 0x5000, 0x4 },	/* 104 SPMI1_SPMI_CFG_TOP */
	{ 0x0c4c0000, 0x10000, 0x4 },	/* 105 SPMI0_SPMI_PIC_OWNER */
	{ 0x0c4d0000, 0x10000, 0x4 },	/* 106 SPMI1_SPMI_PIC_OWNER */
};

static_assert(ARRAY_SIZE(qhee_cdsp_map) <= QHEE_CDSP_MAP_MAX);

static void x1e_map_qhee(struct qcom_adsp *pas, struct rproc *rproc)
{
	int i, ret, fails = 0;

	if (!x1e_has_domain(rproc))
		return;
	x1e_map_extra(pas, rproc, true);	/* SMEM + cmd-db as normal memory */
	for (i = 0; i < ARRAY_SIZE(qhee_cdsp_map); i++) {
		int prot = IOMMU_MMIO;

		if (test_bit(i, pas->qhee_mapped))
			continue;
		if (qhee_cdsp_map[i].perm & 4)
			prot |= IOMMU_READ;
		if (qhee_cdsp_map[i].perm & 2)
			prot |= IOMMU_WRITE;
		ret = iommu_map(rproc->domain, qhee_cdsp_map[i].pa, qhee_cdsp_map[i].pa,
				qhee_cdsp_map[i].size, prot, GFP_KERNEL);
		if (ret) {
			fails++;
			dev_warn(&rproc->dev, "qhee map %llx+%llx: %d\n",
				 qhee_cdsp_map[i].pa, qhee_cdsp_map[i].size, ret);
		} else {
			set_bit(i, pas->qhee_mapped);
		}
	}
	if (fails)
		dev_warn(&rproc->dev, "%d of %zu CDSP apertures not mapped\n", fails,
			 ARRAY_SIZE(qhee_cdsp_map));
}

/* Undo x1e_map_qhee() */
static void x1e_unmap_qhee(struct qcom_adsp *pas, struct rproc *rproc)
{
	int i;

	if (!x1e_has_domain(rproc)) {
		pas->extra_mapped = 0;
		bitmap_zero(pas->qhee_mapped, QHEE_CDSP_MAP_MAX);
		return;
	}

	for_each_set_bit(i, pas->qhee_mapped, ARRAY_SIZE(qhee_cdsp_map)) {
		iommu_unmap(rproc->domain, qhee_cdsp_map[i].pa, qhee_cdsp_map[i].size);
		clear_bit(i, pas->qhee_mapped);
	}

	x1e_map_extra(pas, rproc, false);
}

/*
 * X1E80100 at EL2: the bring-up and stop sequences the hypervisor (QHEE) runs for the
 * ADSP and the CDSP, done by Linux. Register names come from the hardware region
 * directories in the DSP firmware; "OFF_xxxx" marks registers whose block is known but
 * whose name is not.
 */

/* Register blocks touched by the sequences, mapped once at probe */
enum x1e_blk {
	X1E_QDSP6SS,		/* QDSP6 subsystem (the DT reg): EVB, boot FSM, DTB descriptor */
	X1E_Q6SS_CC,		/* Q6 core clock controller: PLL, core RCG and branches */
	X1E_TCSR_SOC,		/* TCSR SoC hardware version */
	/* CDSP only */
	X1E_TURING_CC,
	X1E_ACD_MND,
	X1E_GCC_TURING,
	/* ADSP only */
	X1E_LPASS_MCC,
	X1E_LPASS_TCM,
	X1E_LPASS_CORE_GDSC,
	X1E_LPASS_CORE_CC,
	X1E_LPASS_AON_CC,
	X1E_LPASS_AUDIO_PLL,
	X1E_LPASS_LPMLA,
	X1E_SSC_SCC,
	X1E_LPASS_HOST_LINK,
	X1E_NR_BLK
};

static_assert(X1E_NR_BLK <= X1E_NR_BLK_MAX);

struct x1e_blk_desc {
	phys_addr_t base;
	size_t size;
};

/* QDSP6SS, besides RST_EVB_REG, RET_CFG_REG, CORE_START_REG, BOOT_CMD_REG, BOOT_STATUS_REG */
#define QDSP6SS_DTB_ENTRY_LO		0x60
#define QDSP6SS_DTB_ENTRY_HI		0x64
#define QDSP6SS_DTB_CHIP_ID		0x68
#define QDSP6SS_DTB_CHIP_VERSION	0x6c
#define QDSP6SS_DTB_PLATFORM_INFO	0x70
#define QDSP6SS_DTB_SIZE		0x74
#define RST_EVB_KEEP			(GENMASK(31, 28) | GENMASK(3, 0))
#define RET_CFG_RESET_RETENTION		BIT(0)
#define RET_CFG_BIT1			BIT(1)

/* Lucid/Zonda PLL MODE bits, for the audio PLLs the ADSP stop turns off */
#define PLL_MODE_OUTCTRL		BIT(0)
#define PLL_MODE_RESET_N		BIT(2)

/* Q6 core RCG and branches, offsets in the Q6SS clock controller */
#define RCG_CMD				0x0
#define RCG_CMD_UPDATE			BIT(0)	/* also for LPASS_CORE_CMD_RCGR */
#define RCG_CMD_ROOT_EN			BIT(1)
#define CDSP_CORE_CBCR			0x8040
#define CDSP_Q6SS_CC_OFF_A004		0xa004

/* Branch clock control register bits */
#define CBCR_CLK_ENABLE			BIT(0)
#define CBCR_HW_CTL			BIT(1)
#define CBCR_FORCE_MEM_CORE_ON		BIT(14)
#define CBCR_BIT22			BIT(22)
#define CBCR_CLK_OFF			BIT(31)

/* TCSR halt block (qcom,halt-regs): LPASS_HALTREQ_REG etc. plus these on X1E */
#define HALT_PWR_ON			0x0c
#define HALT_OFF_14			0x14

/* TURING_CC, ACD_MND and the GCC Turing branch (CDSP stop) */
#define TURING_CC_OFF_8410		0x8410
#define TURING_CC_OFF_8414		0x8414
#define TURING_CC_OFF_8418		0x8418
#define TURING_CC_OFF_18034		0x18034
#define ACD_MND_OFF_3C			0x3c
#define GCC_TURING_CFG_AHB_CBCR		0x8

/* LPASS (ADSP) */
#define LPASS_MCC_RSC_WAIT_EVENT_OVRD_MASK	0x18
#define LPI_TCM_BLOCK_CBCR(n)		(0x4 * (n))
#define LPI_TCM_NR_BLOCKS		10
#define LPI_TCM_AXIS_HS_CBCR		0x300
#define LPI_TCM_CBCR_KEEP_MEM		0x81c0300f	/* QHEE clears all other bits */
#define LPI_TCM_CBCR_KEEP_EN		0x81c0700e
#define LPASS_CORE_HM_GDSCR		0x0
#define LPASS_CORE_GDSC_OFF_2000	0x2000
#define LPASS_CORE_GDSC_STATUS		0xa008
#define LPASS_CORE_HM_GDSCR_RETAIN	BIT(11)
#define LPASS_CORE_DIG_PLL_OPMODE	0x4
#define LPASS_CORE_CMD_RCGR		0x25000
#define LPASS_CORE_CFG_RCGR		0x25004
#define RCG_CFG_SRC_SEL			GENMASK(10, 8)
#define RCG_CFG_SRC_DIV			GENMASK(4, 0)
#define LPASS_AON_OFF_A000		0xa000
#define LPASS_AON_OFF_11014		0x11014
#define LPASS_AON_OFF_1108C		0x1108c
#define LPASS_AON_AUDIO_HM_GDSCR	0x11090
#define LPASS_AON_OFF_110AC		0x110ac
#define LPASS_AON_OFF_14000		0x14000
#define LPASS_AON_OFF_18004		0x18004
#define LPASS_AON_OFF_220D0		0x220d0
#define LPASS_AON_OFF_25100		0x25100
#define LPASS_AON_OFF_260AC		0x260ac
#define LPASS_AON_OFF_270AC		0x270ac
#define LPASS_AON_OFF_2900C		0x2900c
#define LPASS_AON_OFF_29010		0x29010
#define LPASS_AON_OFF_29020		0x29020
#define AUDIO_ZONDA_PLL_MODE		0x0
#define AUDIO_ZONDA_PLL_OPMODE		0x30
#define AUDIO_DIG_PLL_OPMODE		0x1004
/* n = 0: LPMLA, n = 1: AOC LPMLA */
#define LPMLA_CORE_CTL(n)		(0x1064 + 0x10000 * (n))
#define LPMLA_CORE_STATUS(n)		(0x1068 + 0x10000 * (n))
#define SSC_SCC_OFF_4C			0x4c
#define LPASS_HOST_LINK_CTL		0x0

#define X1E_SOC_HW_VERSION		0x0

static void __iomem *x1e_reg(struct qcom_adsp *pas, enum x1e_blk blk, u32 off)
{
	return pas->x1e_base[blk] + off;
}

static u32 x1e_rd(struct qcom_adsp *pas, enum x1e_blk blk, u32 off)
{
	return readl(x1e_reg(pas, blk, off));
}

static void x1e_wr(struct qcom_adsp *pas, enum x1e_blk blk, u32 off, u32 val)
{
	writel(val, x1e_reg(pas, blk, off));
}

static void x1e_rmw(struct qcom_adsp *pas, enum x1e_blk blk, u32 off, u32 clear, u32 set)
{
	void __iomem *reg = x1e_reg(pas, blk, off);

	writel((readl(reg) & ~clear) | set, reg);
}

static int x1e_poll(struct qcom_adsp *pas, enum x1e_blk blk, u32 off, u32 mask, u32 want,
		    u32 timeout_us)
{
	u32 val;

	return readl_poll_timeout(x1e_reg(pas, blk, off), val, (val & mask) == want, 1,
				  timeout_us);
}

static u32 x1e_halt_rd(struct qcom_adsp *pas, u32 reg)
{
	u32 val = 0;

	regmap_read(pas->halt_map, pas->halt_lpass + reg, &val);
	return val;
}

static void x1e_halt_wr(struct qcom_adsp *pas, u32 reg, u32 val)
{
	regmap_write(pas->halt_map, pas->halt_lpass + reg, val);
}

static int x1e_halt_wait_ack(struct qcom_adsp *pas)
{
	u32 val;

	return regmap_read_poll_timeout(pas->halt_map, pas->halt_lpass + LPASS_HALTACK_REG,
					val, val & 1, 5, 1000000);
}

/* Turn a branch clock on; under hardware control it may not report CLK_OFF = 0 */
static int x1e_branch_enable(struct qcom_adsp *pas, enum x1e_blk blk, u32 off)
{
	x1e_rmw(pas, blk, off, 0, CBCR_CLK_ENABLE);
	if (x1e_rd(pas, blk, off) & CBCR_HW_CTL)
		return 0;

	return x1e_poll(pas, blk, off, CBCR_CLK_OFF, 0, 1000);
}

/*
 * The Q6 core PLL and RCG (q6sscc) lose their state on every subsystem reset, so the
 * provider programs them on each prepare; on the CDSP only after the boot FSM ran.
 */
static int x1e_core_clk_enable(struct qcom_adsp *pas)
{
	int ret;

	ret = clk_prepare_enable(pas->core_clk);
	if (!ret)
		pas->core_clk_on = true;

	return ret;
}

static void x1e_core_clk_disable(struct qcom_adsp *pas)
{
	if (pas->core_clk_on)
		clk_disable_unprepare(pas->core_clk);
	pas->core_clk_on = false;
}

/* Boot address: the ELF entry point, the bits outside EVB_MASK stay as they are */
static void x1e_set_evb(struct qcom_adsp *pas, u64 entry)
{
	u32 evb = x1e_rd(pas, X1E_QDSP6SS, RST_EVB_REG) & RST_EVB_KEEP;

	x1e_wr(pas, X1E_QDSP6SS, RST_EVB_REG, evb | FIELD_PREP(EVB_MASK, entry >> 8));
}

/* Ask the boot FSM to power the core up; like QHEE, carry on if it doesn't report back */
static void x1e_boot_cmd(struct qcom_adsp *pas)
{
	u32 val;

	x1e_wr(pas, X1E_QDSP6SS, BOOT_CMD_REG, LPASS_BOOT_CMD_START);
	if (readl_poll_timeout(x1e_reg(pas, X1E_QDSP6SS, BOOT_STATUS_REG), val, val, 5,
			       1000000))
		dev_warn(pas->dev, "boot FSM did not report back\n");
	udelay(5);
}

static u64 x1e_entry(const struct firmware *fw, phys_addr_t phys, phys_addr_t reloc)
{
	const struct elf32_hdr *ehdr = (const struct elf32_hdr *)fw->data;

	return (u64)ehdr->e_entry + phys - reloc;
}

/* Size of the loaded DTB image, as the DSP reads it from the descriptor */
static int x1e_dtb_size(struct qcom_adsp *pas, u32 *size)
{
	const struct firmware *fw = pas->dtb_firmware;
	const struct elf32_hdr *ehdr = (const struct elf32_hdr *)fw->data;
	u64 low = U64_MAX, high = 0;
	int i;

	if (ehdr->e_phentsize != sizeof(struct elf32_phdr) ||
	    (u64)ehdr->e_phoff + (u64)ehdr->e_phnum * ehdr->e_phentsize > fw->size)
		return -EINVAL;

	for (i = 0; i < ehdr->e_phnum; i++) {
		const struct elf32_phdr *phdr = (const void *)(fw->data + ehdr->e_phoff +
							      i * ehdr->e_phentsize);

		if (phdr->p_type != PT_LOAD || !phdr->p_memsz ||
		    ((phdr->p_flags >> 24) & 7) == 2)	/* hash segment */
			continue;
		low = min_t(u64, low, phdr->p_paddr);
		high = max_t(u64, high, (u64)phdr->p_paddr + phdr->p_memsz);
	}
	if (low == U64_MAX || high <= low || ALIGN(high, SZ_4K) - low > pas->dtb_mem_size)
		return -EINVAL;

	*size = ALIGN(high, SZ_4K) - low;
	return 0;
}

/* SMEM keeps 16-bit major and minor numbers, the descriptor one byte of each */
static u32 x1e_dtb_version(u32 ver)
{
	return ((ver >> 8) & 0xff00) | (ver & 0xff);
}

/*
 * Each DSP picks its configuration out of its DTB image by chip and platform.
 * It reads them from the descriptor, packed the way the hypervisor packs the
 * socinfo fields when it starts the DSP itself.
 */
static int x1e_dtb_identity(struct qcom_adsp *pas)
{
	const struct socinfo *info;
	size_t size;

	info = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_HW_SW_BUILD_ID, &size);
	if (IS_ERR(info))
		return dev_err_probe(pas->dev, PTR_ERR(info), "unable to read socinfo\n");
	if (size < offsetofend(struct socinfo, chip_family) ||
	    le32_to_cpu(info->fmt) < SOCINFO_VERSION(0, 12))
		return dev_err_probe(pas->dev, -EINVAL, "socinfo has no chip family\n");

	pas->dtb_chip_id = (le32_to_cpu(info->chip_family) & 0xffff) << 16 |
			   (le32_to_cpu(info->id) & 0xffff);
	pas->dtb_chip_version = x1e_dtb_version(le32_to_cpu(info->ver));
	pas->dtb_platform_info = (le32_to_cpu(info->hw_plat) & 0xff) << 24 |
				 (le32_to_cpu(info->hw_plat_subtype) & 0xff) << 16 |
				 x1e_dtb_version(le32_to_cpu(info->plat_ver));
	return 0;
}

/*
 * The DTB image descriptor; a reset clears it. Without it the DSP still boots,
 * but runs without the configuration from its DTB image.
 */
static int x1e_write_dtb_desc(struct qcom_adsp *pas)
{
	u64 dtb_entry;
	u32 dtb_size;
	int ret;

	ret = x1e_dtb_size(pas, &dtb_size);
	if (ret)
		return ret;

	dtb_entry = x1e_entry(pas->dtb_firmware, pas->dtb_mem_phys, pas->dtb_mem_reloc);
	x1e_wr(pas, X1E_QDSP6SS, QDSP6SS_DTB_ENTRY_LO, lower_32_bits(dtb_entry));
	x1e_wr(pas, X1E_QDSP6SS, QDSP6SS_DTB_ENTRY_HI, upper_32_bits(dtb_entry));
	x1e_wr(pas, X1E_QDSP6SS, QDSP6SS_DTB_CHIP_ID, pas->dtb_chip_id);
	x1e_wr(pas, X1E_QDSP6SS, QDSP6SS_DTB_CHIP_VERSION, pas->dtb_chip_version);
	x1e_wr(pas, X1E_QDSP6SS, QDSP6SS_DTB_PLATFORM_INFO, pas->dtb_platform_info);
	x1e_wr(pas, X1E_QDSP6SS, QDSP6SS_DTB_SIZE, dtb_size);

	return 0;
}

static int x1e_adsp_bringup(struct qcom_adsp *pas)
{
	int ret, i;

	/* TCM clocks */
	x1e_wr(pas, X1E_LPASS_MCC, LPASS_MCC_RSC_WAIT_EVENT_OVRD_MASK, 0x8);
	ret = x1e_branch_enable(pas, X1E_LPASS_TCM, LPI_TCM_AXIS_HS_CBCR);
	if (ret)
		return ret;
	for (i = 0; i < LPI_TCM_NR_BLOCKS; i++) {
		x1e_rmw(pas, X1E_LPASS_TCM, LPI_TCM_BLOCK_CBCR(i), ~LPI_TCM_CBCR_KEEP_MEM,
			CBCR_FORCE_MEM_CORE_ON);
		x1e_rmw(pas, X1E_LPASS_TCM, LPI_TCM_BLOCK_CBCR(i), ~LPI_TCM_CBCR_KEEP_EN,
			CBCR_CLK_ENABLE);
	}

	ret = x1e_core_clk_enable(pas);
	if (ret)
		return ret;

	ret = x1e_write_dtb_desc(pas);
	if (ret)
		return ret;

	x1e_set_evb(pas, x1e_entry(pas->firmware, pas->mem_phys, pas->mem_reloc));
	x1e_wr(pas, X1E_QDSP6SS, RET_CFG_REG, 0);
	x1e_rmw(pas, X1E_QDSP6SS, CORE_START_REG, 0, LPASS_BOOT_CORE_START);
	x1e_boot_cmd(pas);

	return 0;
}

static int x1e_cdsp_bringup(struct qcom_adsp *pas)
{
	u32 hw = x1e_rd(pas, X1E_TCSR_SOC, X1E_SOC_HW_VERSION);
	int ret;

	if ((hw >> 28) != 0xa || (((hw >> 16) & 0xfff) != 0x9 && ((hw >> 16) & 0xfff) != 0x16))
		return -ENODEV;

	ret = x1e_write_dtb_desc(pas);
	if (ret)
		return ret;

	x1e_set_evb(pas, x1e_entry(pas->firmware, pas->mem_phys, pas->mem_reloc));
	x1e_wr(pas, X1E_QDSP6SS, RET_CFG_REG, 0);
	x1e_boot_cmd(pas);

	ret = x1e_core_clk_enable(pas);
	if (ret)
		return ret;
	udelay(5);

	x1e_rmw(pas, X1E_QDSP6SS, CORE_START_REG, 0, LPASS_BOOT_CORE_START);

	return 0;
}

static int x1e_cdsp_shutdown(struct qcom_adsp *pas)
{
	static const u32 bit22_branches[] = { 0x8060, 0x8080, 0x80a0, 0x80c0, 0x8100, 0x81c8,
					      CDSP_CORE_CBCR };
	int ret, i;

	ret = x1e_branch_enable(pas, X1E_TURING_CC, TURING_CC_OFF_8418);
	if (ret)
		return ret;
	x1e_rmw(pas, X1E_TURING_CC, TURING_CC_OFF_18034, 0, BIT(0));
	x1e_rmw(pas, X1E_QDSP6SS, RET_CFG_REG, 0, RET_CFG_RESET_RETENTION);
	x1e_rmw(pas, X1E_GCC_TURING, GCC_TURING_CFG_AHB_CBCR, CBCR_CLK_ENABLE, 0);

	/* Like QHEE: halt the NSP NoC port only if it is up and not idle yet */
	if ((x1e_halt_rd(pas, HALT_PWR_ON) & 1) &&
	    (!(x1e_halt_rd(pas, LPASS_MASTER_IDLE_REG) & 1) ||
	     !(x1e_halt_rd(pas, HALT_OFF_14) & 1))) {
		x1e_halt_wr(pas, LPASS_HALTREQ_REG, 1);
		if (x1e_halt_wait_ack(pas))
			dev_warn(pas->dev, "NSP port did not halt\n");
	}

	/* Gate the core clocks */
	ret = x1e_branch_enable(pas, X1E_GCC_TURING, GCC_TURING_CFG_AHB_CBCR);
	if (ret)
		return ret;
	x1e_rmw(pas, X1E_ACD_MND, ACD_MND_OFF_3C, BIT(0), 0);
	x1e_rmw(pas, X1E_Q6SS_CC, CDSP_Q6SS_CC_OFF_A004, 0, 0xff);
	x1e_rmw(pas, X1E_Q6SS_CC, pas->x1e->core_rcg + RCG_CMD, RCG_CMD_ROOT_EN, 0);
	for (i = 0; i < ARRAY_SIZE(bit22_branches); i++)
		x1e_rmw(pas, X1E_Q6SS_CC, bit22_branches[i], 0, CBCR_BIT22);
	x1e_rmw(pas, X1E_TURING_CC, TURING_CC_OFF_8410, 0, CBCR_BIT22);
	ret = x1e_poll(pas, X1E_TURING_CC, TURING_CC_OFF_8410, CBCR_CLK_OFF, CBCR_CLK_OFF, 1000);
	if (ret)
		return ret;
	x1e_rmw(pas, X1E_TURING_CC, TURING_CC_OFF_8414, 0, CBCR_BIT22);
	ret = x1e_poll(pas, X1E_TURING_CC, TURING_CC_OFF_8414, CBCR_CLK_OFF, CBCR_CLK_OFF, 1000);
	if (ret)
		return ret;

	/* As in qcom_adsp_shutdown(), with QHEE's longer wait before the release */
	reset_control_assert(pas->pdc_sync_reset);
	reset_control_assert(pas->restart);
	x1e_halt_wr(pas, LPASS_HALTREQ_REG, 0);
	usleep_range(10000, 11000);
	reset_control_deassert(pas->pdc_sync_reset);
	reset_control_deassert(pas->restart);

	return 0;
}

/* Collapse the LPASS core power domain; QHEE retries up to ten times */
static void x1e_lpass_core_collapse(struct qcom_adsp *pas)
{
	int i;

	x1e_rmw(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_A000, 0, BIT(4));
	x1e_rmw(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_A000, BIT(0), 0);
	for (i = 0; ; i++) {
		if (!x1e_poll(pas, X1E_LPASS_CORE_GDSC, LPASS_CORE_GDSC_STATUS, ~0U, 1, 500))
			return;
		if (i == 10)
			break;
		x1e_rmw(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_A000, 0, BIT(0));
		udelay(5);
		x1e_rmw(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_A000, BIT(0), 0);
	}
	dev_warn(pas->dev, "LPASS core did not collapse\n");
}

static int x1e_adsp_shutdown(struct qcom_adsp *pas)
{
	struct arm_smccc_res res;
	int ret, n;

	/* QUP/GPI channel cleanup before the clock stop: SiP svc 4 cmd 5, one value argument (0) */
	arm_smccc_smc(0xc2000405, 1, 0, 0, 0, 0, 0, 0, &res);
	/* QHEE only logs a failure here and still runs the clock stop */
	if (res.a0)
		dev_warn(pas->dev, "BLSP cleanup SMC failed: %lx\n", res.a0);

	/* LPASS core GDSC under hardware control: move the core RCG off its PLL and let go */
	if (x1e_rd(pas, X1E_LPASS_CORE_GDSC, LPASS_CORE_HM_GDSCR) & BIT(31)) {
		if (x1e_rd(pas, X1E_LPASS_CORE_GDSC, LPASS_CORE_GDSC_OFF_2000) & BIT(0))
			x1e_wr(pas, X1E_LPASS_CORE_GDSC, LPASS_CORE_GDSC_OFF_2000, 0);
		x1e_rmw(pas, X1E_LPASS_CORE_GDSC, LPASS_CORE_HM_GDSCR,
			LPASS_CORE_HM_GDSCR_RETAIN, 0);
		x1e_rmw(pas, X1E_LPASS_CORE_CC, LPASS_CORE_CFG_RCGR, RCG_CFG_SRC_SEL, 0);
		x1e_rmw(pas, X1E_LPASS_CORE_CC, LPASS_CORE_CFG_RCGR, RCG_CFG_SRC_DIV, 0);
		x1e_rmw(pas, X1E_LPASS_CORE_CC, LPASS_CORE_CMD_RCGR, 0, RCG_CMD_UPDATE);
		ret = x1e_poll(pas, X1E_LPASS_CORE_CC, LPASS_CORE_CMD_RCGR, RCG_CMD_UPDATE, 0,
			       100000);
		if (ret)
			return ret;
		x1e_rmw(pas, X1E_LPASS_CORE_CC, LPASS_CORE_DIG_PLL_OPMODE, 0x7, 0);
		x1e_rmw(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_14000, 0, BIT(0));
		x1e_rmw(pas, X1E_LPASS_CORE_GDSC, LPASS_CORE_HM_GDSCR, 0, BIT(0));
		ret = x1e_poll(pas, X1E_LPASS_CORE_GDSC, LPASS_CORE_HM_GDSCR, BIT(31), 0, 100000);
		if (ret)
			return ret;
	}
	x1e_rmw(pas, X1E_LPASS_CORE_GDSC, LPASS_CORE_GDSC_OFF_2000, BIT(0), 0);
	x1e_rmw(pas, X1E_LPASS_CORE_GDSC, LPASS_CORE_HM_GDSCR, LPASS_CORE_HM_GDSCR_RETAIN, 0);

	/* Core not collapsed yet: switch the audio clocks off and enable Q6 retention */
	if (!(x1e_rd(pas, X1E_LPASS_CORE_GDSC, LPASS_CORE_GDSC_STATUS) & BIT(0))) {
		ret = x1e_branch_enable(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_18004);
		if (ret)
			return ret;
		if (!(x1e_rd(pas, X1E_LPASS_AON_CC, LPASS_AON_AUDIO_HM_GDSCR) & BIT(31))) {
			x1e_rmw(pas, X1E_LPASS_AON_CC, LPASS_AON_AUDIO_HM_GDSCR, BIT(0), 0);
			ret = x1e_poll(pas, X1E_LPASS_AON_CC, LPASS_AON_AUDIO_HM_GDSCR, BIT(31),
				       BIT(31), 12000);
			if (!ret)
				ret = x1e_poll(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_110AC, BIT(31),
					       BIT(31), 12000);
			if (ret)
				return ret;
		}
		ret = x1e_branch_enable(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_11014);
		if (ret)
			return ret;
		x1e_rmw(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_1108C, 0, BIT(0));
		if (x1e_poll(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_1108C, BIT(31), BIT(31), 300)) {
			x1e_rmw(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_1108C, 0, BIT(2));
			usleep_range(250, 300);
			x1e_rmw(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_1108C, BIT(2), 0);
		}
		x1e_rmw(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_1108C, BIT(0), 0);
		usleep_range(150, 200);
		/* Audio PLLs off */
		x1e_rmw(pas, X1E_LPASS_AUDIO_PLL, AUDIO_ZONDA_PLL_MODE, PLL_MODE_OUTCTRL, 0);
		x1e_rmw(pas, X1E_LPASS_AUDIO_PLL, AUDIO_ZONDA_PLL_OPMODE, 0x7, 0);
		x1e_rmw(pas, X1E_LPASS_AUDIO_PLL, AUDIO_ZONDA_PLL_MODE, PLL_MODE_RESET_N, 0);
		x1e_rmw(pas, X1E_LPASS_AUDIO_PLL, AUDIO_ZONDA_PLL_MODE, BIT(1), 0);
		x1e_rmw(pas, X1E_LPASS_AUDIO_PLL, AUDIO_DIG_PLL_OPMODE, 0x7, 0);
		x1e_rmw(pas, X1E_LPASS_AON_CC, LPASS_AON_AUDIO_HM_GDSCR, 0, BIT(0));
		ret = x1e_poll(pas, X1E_LPASS_AON_CC, LPASS_AON_AUDIO_HM_GDSCR, BIT(31), 0, 100000);
		if (ret)
			return ret;
		x1e_rmw(pas, X1E_QDSP6SS, RET_CFG_REG, 0, RET_CFG_RESET_RETENTION);
		x1e_rmw(pas, X1E_QDSP6SS, RET_CFG_REG, 0, RET_CFG_BIT1);
	}

	x1e_halt_wr(pas, LPASS_HALTREQ_REG, 1);
	if (x1e_halt_wait_ack(pas))
		dev_warn(pas->dev, "LPASS port did not halt\n");

	if (!(x1e_rd(pas, X1E_LPASS_CORE_GDSC, LPASS_CORE_GDSC_STATUS) & BIT(0))) {
		x1e_rmw(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_29020, BIT(0), 0);
		x1e_rmw(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_29020, 0, BIT(3));
		ret = x1e_poll(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_29020, BIT(31), BIT(31),
			       100000);
		if (ret)
			return ret;
		x1e_rmw(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_29010, 0, BIT(2));
		x1e_rmw(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_2900C, 0, BIT(2));
		udelay(1);
		x1e_rmw(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_29020, 0, BIT(0));
		if (x1e_rd(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_25100) & BIT(31))
			x1e_rmw(pas, X1E_SSC_SCC, SSC_SCC_OFF_4C, 0, BIT(22));
		x1e_rmw(pas, X1E_LPASS_AON_CC, LPASS_AON_OFF_220D0, BIT(0), 0);
		/* Both LPMLA cores off, if they are clocked */
		for (n = 0; n < 2; n++) {
			if (!(x1e_rd(pas, X1E_LPASS_AON_CC, n ? LPASS_AON_OFF_270AC :
							       LPASS_AON_OFF_260AC) & BIT(31)))
				continue;
			x1e_rmw(pas, X1E_LPASS_LPMLA, LPMLA_CORE_CTL(n), BIT(0), 0);
			x1e_rmw(pas, X1E_LPASS_LPMLA, LPMLA_CORE_CTL(n), BIT(1), 0);
			ret = x1e_poll(pas, X1E_LPASS_LPMLA, LPMLA_CORE_STATUS(n), 0x3, 0, 100000);
			if (ret)
				return ret;
		}
		x1e_lpass_core_collapse(pas);
	}

	/* Reset, with the host link isolated around the pulse */
	x1e_rmw(pas, X1E_LPASS_HOST_LINK, LPASS_HOST_LINK_CTL, 0, BIT(1));
	reset_control_assert(pas->pdc_sync_reset);
	usleep_range(10000, 11000);
	reset_control_assert(pas->restart);
	x1e_rmw(pas, X1E_LPASS_HOST_LINK, LPASS_HOST_LINK_CTL, 0, BIT(0));
	usleep_range(10, 20);
	x1e_rmw(pas, X1E_LPASS_HOST_LINK, LPASS_HOST_LINK_CTL, BIT(0), 0);
	reset_control_deassert(pas->pdc_sync_reset);
	reset_control_deassert(pas->restart);
	x1e_rmw(pas, X1E_LPASS_HOST_LINK, LPASS_HOST_LINK_CTL, BIT(1), 0);
	x1e_halt_wr(pas, LPASS_HALTREQ_REG, 0);
	usleep_range(100, 150);

	return 0;
}

static const struct x1e_blk_desc x1e_adsp_blks[X1E_NR_BLK] = {
	[X1E_Q6SS_CC]		= { 0x06840000, 0x10000 },
	[X1E_TCSR_SOC]		= { 0x01fc8000, 0x4 },
	[X1E_LPASS_MCC]		= { 0x07250000, 0x1000 },
	[X1E_LPASS_TCM]		= { 0x0726a000, 0x1000 },
	[X1E_LPASS_CORE_GDSC]	= { 0x07e00000, 0x10000 },
	[X1E_LPASS_CORE_CC]	= { 0x07b00000, 0x30000 },
	[X1E_LPASS_AON_CC]	= { 0x06e00000, 0x30000 },
	[X1E_LPASS_AUDIO_PLL]	= { 0x06bc0000, 0x2000 },
	[X1E_LPASS_LPMLA]	= { 0x06900000, 0x20000 },
	[X1E_SSC_SCC]		= { 0x07a01000, 0x1000 },
	[X1E_LPASS_HOST_LINK]	= { 0x0ef01000, 0x1000 },
};

static const struct x1e_blk_desc x1e_cdsp_blks[X1E_NR_BLK] = {
	[X1E_Q6SS_CC]		= { 0x32340000, 0x10000 },
	[X1E_TCSR_SOC]		= { 0x01fc8000, 0x4 },
	[X1E_TURING_CC]		= { 0x32000000, 0x20000 },
	[X1E_ACD_MND]		= { 0x32350000, 0x1000 },
	[X1E_GCC_TURING]	= { 0x00145000, 0x1000 },
};

static int x1e_map_blocks(struct qcom_adsp *pas)
{
	const struct x1e_blk_desc *blks = pas->x1e->is_cdsp ? x1e_cdsp_blks : x1e_adsp_blks;
	int i;

	pas->x1e_base[X1E_QDSP6SS] = pas->qdsp6ss_base;
	for (i = 0; i < X1E_NR_BLK; i++) {
		if (!blks[i].size)
			continue;
		pas->x1e_base[i] = devm_ioremap(pas->dev, blks[i].base, blks[i].size);
		if (!pas->x1e_base[i])
			return -ENOMEM;
	}

	return 0;
}

/*
 * rproc glue for X1E. Linux copies both images (main + DTB) into their carveouts and the
 * sequences above start the core through the QDSP6SS boot FSM: no TZ PAS. TZ is only used
 * once, to stop the ADSP image the UEFI firmware leaves running (that also drops the TZ
 * protection of its carveout).
 */
static int x1e_load(struct rproc *rproc, const struct firmware *fw)
{
	struct qcom_adsp *pas = rproc->priv;
	int ret;

	/* The bring-up reads the ELF headers in ->start(); the core keeps fw until then */
	pas->firmware = fw;

	ret = qcom_mdt_load_no_init(pas->dev, fw, rproc->firmware,
				    (__force void *)pas->mem_region, pas->mem_phys,
				    pas->mem_size, &pas->mem_reloc);
	if (ret)
		return ret;

	qcom_pil_info_store(pas->info_name, pas->mem_phys, pas->mem_size);

	ret = request_firmware(&pas->dtb_firmware, pas->dtb_firmware_name, pas->dev);
	if (ret) {
		dev_err(pas->dev, "request_firmware failed for %s: %d\n",
			pas->dtb_firmware_name, ret);
		return ret;
	}

	ret = qcom_mdt_load_no_init(pas->dev, pas->dtb_firmware, pas->dtb_firmware_name,
				    (__force void *)pas->dtb_mem_region, pas->dtb_mem_phys,
				    pas->dtb_mem_size, &pas->dtb_mem_reloc);
	if (ret) {
		release_firmware(pas->dtb_firmware);
		pas->dtb_firmware = NULL;
	}

	return ret;
}

static void x1e_release_fw(struct qcom_adsp *pas)
{
	release_firmware(pas->dtb_firmware);
	pas->dtb_firmware = NULL;
	pas->firmware = NULL;
}

/* X1E images carry no resource table; the carveouts are mapped in ->start() */
static int x1e_parse_firmware(struct rproc *rproc, const struct firmware *fw)
{
	return qcom_register_dump_segments(rproc, fw);
}

static int x1e_map_carveout(struct rproc *rproc, phys_addr_t phys, size_t size)
{
	if (!x1e_has_domain(rproc))
		return 0;
	return iommu_map(rproc->domain, phys, phys, size, IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
}

static void x1e_unmap_all(struct qcom_adsp *pas, struct rproc *rproc)
{
	x1e_unmap_qhee(pas, rproc);
	x1e_unmap_carveout(rproc, pas->mem_phys, pas->mem_size);
	x1e_unmap_carveout(rproc, pas->dtb_mem_phys, pas->dtb_mem_size);
}

static int x1e_start(struct rproc *rproc)
{
	struct qcom_adsp *pas = rproc->priv;
	bool cdsp = pas->x1e->is_cdsp;
	int ret;

	ret = qcom_q6v5_prepare(&pas->q6v5);
	if (ret)
		goto release_fw;

	ret = clk_prepare_enable(pas->xo);
	if (ret)
		goto disable_irqs;

	ret = qcom_rproc_pds_enable(pas);
	if (ret < 0)
		goto disable_xo;

	ret = x1e_map_carveout(rproc, pas->dtb_mem_phys, pas->dtb_mem_size);
	if (ret)
		goto unmap;
	ret = x1e_map_carveout(rproc, pas->mem_phys, pas->mem_size);
	if (ret)
		goto unmap;

	if (cdsp) {
		x1e_map_qhee(pas, rproc);
		ret = x1e_cdsp_bringup(pas);
	} else {
		ret = x1e_adsp_bringup(pas);
	}
	if (ret)
		goto unmap;

	ret = qcom_q6v5_wait_for_start(&pas->q6v5, msecs_to_jiffies(5000));
	if (ret == -ETIMEDOUT) {
		dev_err(pas->dev, "start timed out\n");
		goto unmap;
	}

	x1e_release_fw(pas);
	return 0;

unmap:
	x1e_core_clk_disable(pas);
	x1e_unmap_all(pas, rproc);
	qcom_rproc_pds_disable(pas);
disable_xo:
	clk_disable_unprepare(pas->xo);
disable_irqs:
	qcom_q6v5_unprepare(&pas->q6v5);
release_fw:
	x1e_release_fw(pas);
	return ret;
}

/* Runs the hardware stop with the proxy votes held so the subsystem stays powered */
static int x1e_hw_stop(struct qcom_adsp *pas)
{
	int ret;

	ret = qcom_rproc_pds_enable(pas);
	if (ret < 0)
		return ret;
	ret = clk_prepare_enable(pas->xo);
	if (!ret) {
		ret = pas->x1e->is_cdsp ? x1e_cdsp_shutdown(pas) : x1e_adsp_shutdown(pas);
		x1e_core_clk_disable(pas);
		clk_disable_unprepare(pas->xo);
	}
	qcom_rproc_pds_disable(pas);

	return ret;
}

static int x1e_stop(struct rproc *rproc)
{
	struct qcom_adsp *pas = rproc->priv;
	int handover, ret;

	ret = qcom_q6v5_request_stop(&pas->q6v5, pas->sysmon);
	if (ret == -ETIMEDOUT)
		dev_err(pas->dev, "timed out on wait\n");

	ret = x1e_hw_stop(pas);
	if (ret)
		dev_err(pas->dev, "hardware stop failed: %d\n", ret);

	x1e_unmap_all(pas, rproc);

	handover = qcom_q6v5_unprepare(&pas->q6v5);
	if (handover)
		qcom_adsp_pil_handover(&pas->q6v5);

	return ret;
}

/*
 * Stop the ADSP image the UEFI firmware left running: ask it to stop over SMP2P like
 * qcom_q6v5_request_stop() does, PAS-shut down every id it may run as, then run the
 * hardware stop. Must happen before rproc_add() attaches the IOMMU domain.
 */
static int x1e_stop_fw_instance(struct qcom_adsp *pas)
{
	const struct x1e_dsp_data *d = pas->x1e;
	struct qcom_q6v5 *q6v5 = &pas->q6v5;
	unsigned long left;
	int ret;

	reinit_completion(&q6v5->stop_done);
	qcom_smem_state_update_bits(q6v5->state, BIT(q6v5->stop_bit), BIT(q6v5->stop_bit));
	left = wait_for_completion_timeout(&q6v5->stop_done, 2 * HZ);
	qcom_smem_state_update_bits(q6v5->state, BIT(q6v5->stop_bit), 0);
	dev_info(pas->dev, "UEFI-started instance: stop %s\n",
		 left ? "acknowledged" : "not acknowledged");

	/* Not knowing which image UEFI ran, shut down both; one of them is enough */
	ret = qcom_pas_shutdown(d->pas_id);
	if (ret && d->lite_pas_id)
		ret = qcom_pas_shutdown(d->lite_pas_id);
	if (ret)
		return ret;

	ret = x1e_hw_stop(pas);
	if (ret)
		return ret;

	ret = qcom_pas_shutdown(d->dtb_pas_id);
	if (ret && d->lite_dtb_pas_id)
		ret = qcom_pas_shutdown(d->lite_dtb_pas_id);

	return ret;
}

static const struct rproc_ops x1e_ops = {
	.start = x1e_start,
	.stop = x1e_stop,
	.da_to_va = adsp_da_to_va,
	.parse_fw = x1e_parse_firmware,
	.load = x1e_load,
	.panic = adsp_panic,
};

/* Probe part specific to X1E: DTB carveout and DTB firmware name */
static int x1e_probe(struct qcom_adsp *pas)
{
	struct device_node *np = pas->dev->of_node;
	struct resource res;
	int ret;

	ret = of_reserved_mem_region_to_resource(np, 1, &res);
	if (ret)
		return dev_err_probe(pas->dev, ret, "unable to resolve dtb memory-region\n");

	pas->dtb_mem_phys = res.start;
	pas->dtb_mem_reloc = res.start;
	pas->dtb_mem_size = resource_size(&res);
	pas->dtb_mem_region = devm_ioremap_resource_wc(pas->dev, &res);
	if (IS_ERR(pas->dtb_mem_region))
		return PTR_ERR(pas->dtb_mem_region);

	ret = x1e_map_blocks(pas);
	if (ret)
		return ret;

	pas->core_clk = devm_clk_get(pas->dev, "core");
	if (IS_ERR(pas->core_clk))
		return dev_err_probe(pas->dev, PTR_ERR(pas->core_clk), "no core clock\n");

	pas->dtb_firmware_name = pas->x1e->dtb_firmware_name;
	ret = of_property_read_string_index(np, "firmware-name", 1, &pas->dtb_firmware_name);
	if (ret < 0 && ret != -EINVAL)
		return ret;

	return x1e_dtb_identity(pas);
}

static int adsp_init_clock(struct qcom_adsp *adsp, const char **clk_ids)
{
	int num_clks = 0;
	int i;

	adsp->xo = devm_clk_get(adsp->dev, "xo");
	if (IS_ERR(adsp->xo))
		return dev_err_probe(adsp->dev, PTR_ERR(adsp->xo), "failed to get xo clock");

	for (i = 0; clk_ids[i]; i++)
		num_clks++;

	adsp->num_clks = num_clks;
	adsp->clks = devm_kcalloc(adsp->dev, adsp->num_clks,
				sizeof(*adsp->clks), GFP_KERNEL);
	if (!adsp->clks)
		return -ENOMEM;

	for (i = 0; i < adsp->num_clks; i++)
		adsp->clks[i].id = clk_ids[i];

	return devm_clk_bulk_get(adsp->dev, adsp->num_clks, adsp->clks);
}

static int adsp_init_reset(struct qcom_adsp *adsp)
{
	adsp->pdc_sync_reset = devm_reset_control_get_optional_exclusive(adsp->dev,
			"pdc_sync");
	if (IS_ERR(adsp->pdc_sync_reset)) {
		dev_err(adsp->dev, "failed to acquire pdc_sync reset\n");
		return PTR_ERR(adsp->pdc_sync_reset);
	}

	adsp->restart = devm_reset_control_get_optional_exclusive(adsp->dev, "restart");

	/* Fall back to the  old "cc_lpass" if "restart" is absent */
	if (!adsp->restart)
		adsp->restart = devm_reset_control_get_exclusive(adsp->dev, "cc_lpass");

	if (IS_ERR(adsp->restart)) {
		dev_err(adsp->dev, "failed to acquire restart\n");
		return PTR_ERR(adsp->restart);
	}

	return 0;
}

static int adsp_init_mmio(struct qcom_adsp *adsp,
				struct platform_device *pdev)
{
	struct resource *efuse_region;
	struct device_node *syscon;
	int ret;

	adsp->qdsp6ss_base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(adsp->qdsp6ss_base)) {
		dev_err(adsp->dev, "failed to map QDSP6SS registers\n");
		return PTR_ERR(adsp->qdsp6ss_base);
	}

	efuse_region = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!efuse_region) {
		adsp->lpass_efuse = NULL;
		dev_dbg(adsp->dev, "failed to get efuse memory region\n");
	} else {
		adsp->lpass_efuse = devm_ioremap_resource(&pdev->dev, efuse_region);
		if (IS_ERR(adsp->lpass_efuse)) {
			dev_err(adsp->dev, "failed to map efuse registers\n");
			return PTR_ERR(adsp->lpass_efuse);
		}
	}
	syscon = of_parse_phandle(pdev->dev.of_node, "qcom,halt-regs", 0);
	if (!syscon) {
		dev_err(&pdev->dev, "failed to parse qcom,halt-regs\n");
		return -EINVAL;
	}

	adsp->halt_map = syscon_node_to_regmap(syscon);
	of_node_put(syscon);
	if (IS_ERR(adsp->halt_map))
		return PTR_ERR(adsp->halt_map);

	ret = of_property_read_u32_index(pdev->dev.of_node, "qcom,halt-regs",
			1, &adsp->halt_lpass);
	if (ret < 0) {
		dev_err(&pdev->dev, "no offset in syscon\n");
		return ret;
	}

	return 0;
}

static int adsp_alloc_memory_region(struct qcom_adsp *adsp)
{
	int ret;
	struct resource res;

	ret = of_reserved_mem_region_to_resource(adsp->dev->of_node, 0, &res);
	if (ret) {
		dev_err(adsp->dev, "unable to resolve memory-region\n");
		return ret;
	}

	adsp->mem_phys = adsp->mem_reloc = res.start;
	adsp->mem_size = resource_size(&res);
	adsp->mem_region = devm_ioremap_resource_wc(adsp->dev, &res);
	if (IS_ERR(adsp->mem_region)) {
		dev_err(adsp->dev, "unable to map memory region: %pR\n", &res);
		return PTR_ERR(adsp->mem_region);

	}

	return 0;
}

static int adsp_probe(struct platform_device *pdev)
{
	const struct adsp_pil_data *desc;
	const char *firmware_name;
	struct qcom_adsp *adsp;
	struct rproc *rproc;
	int ret;

	desc = of_device_get_match_data(&pdev->dev);
	if (!desc)
		return -EINVAL;

	firmware_name = desc->firmware_name;
	ret = of_property_read_string(pdev->dev.of_node, "firmware-name",
				      &firmware_name);
	if (ret < 0 && ret != -EINVAL) {
		dev_err(&pdev->dev, "unable to read firmware-name\n");
		return ret;
	}

	if (desc->x1e && !desc->x1e->is_cdsp && !qcom_pas_is_available())
		return -EPROBE_DEFER;	/* TZ stops the image UEFI left running */

	if (desc->x1e)
		rproc = devm_rproc_alloc(&pdev->dev, desc->sysmon_name, &x1e_ops,
					 firmware_name, sizeof(*adsp));
	else
		rproc = devm_rproc_alloc(&pdev->dev, pdev->name, &adsp_ops,
					 firmware_name, sizeof(*adsp));
	if (!rproc) {
		dev_err(&pdev->dev, "unable to allocate remoteproc\n");
		return -ENOMEM;
	}

	rproc->auto_boot = desc->auto_boot;
	rproc->has_iommu = desc->has_iommu ||
			   (desc->x1e && of_property_present(pdev->dev.of_node, "iommus"));
	rproc_coredump_set_elf_info(rproc, ELFCLASS32, EM_NONE);

	adsp = rproc->priv;
	adsp->dev = &pdev->dev;
	adsp->rproc = rproc;
	adsp->info_name = desc->sysmon_name;
	adsp->has_iommu = desc->has_iommu;
	adsp->x1e = desc->x1e;

	platform_set_drvdata(pdev, adsp);

	if (desc->is_wpss)
		adsp->shutdown = qcom_wpss_shutdown;
	else
		adsp->shutdown = qcom_adsp_shutdown;

	ret = adsp_alloc_memory_region(adsp);
	if (ret)
		return ret;

	ret = adsp_init_clock(adsp, desc->clk_ids);
	if (ret)
		return ret;

	ret = qcom_rproc_pds_attach(adsp, desc->pd_names, desc->num_pds);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret,
				     "Failed to attach proxy power domains\n");

	ret = adsp_init_reset(adsp);
	if (ret)
		goto disable_pm;

	ret = adsp_init_mmio(adsp, pdev);
	if (ret)
		goto disable_pm;

	if (desc->x1e) {
		ret = x1e_probe(adsp);
		if (ret)
			goto disable_pm;
	}

	ret = qcom_q6v5_init(&adsp->q6v5, pdev, rproc, desc->crash_reason_smem,
			     desc->load_state, qcom_adsp_pil_handover);
	if (ret)
		goto disable_pm;

	qcom_add_glink_subdev(rproc, &adsp->glink_subdev, desc->ssr_name);
	qcom_add_pdm_subdev(rproc, &adsp->pdm_subdev);
	qcom_add_ssr_subdev(rproc, &adsp->ssr_subdev, desc->ssr_name);
	adsp->sysmon = qcom_add_sysmon_subdev(rproc,
					      desc->sysmon_name,
					      desc->ssctl_id);
	if (IS_ERR(adsp->sysmon)) {
		ret = PTR_ERR(adsp->sysmon);
		goto deinit_remove_glink_pdm_ssr;
	}

	if (desc->x1e && !desc->x1e->is_cdsp) {
		ret = x1e_stop_fw_instance(adsp);
		if (ret)
			goto remove_sysmon;
	}

	ret = rproc_add(rproc);
	if (ret)
		goto remove_sysmon;

	return 0;

remove_sysmon:
	qcom_remove_sysmon_subdev(adsp->sysmon);
deinit_remove_glink_pdm_ssr:
	qcom_q6v5_deinit(&adsp->q6v5);
	qcom_remove_glink_subdev(rproc, &adsp->glink_subdev);
	qcom_remove_pdm_subdev(rproc, &adsp->pdm_subdev);
	qcom_remove_ssr_subdev(rproc, &adsp->ssr_subdev);
disable_pm:
	qcom_rproc_pds_detach(adsp);

	return ret;
}

static void adsp_remove(struct platform_device *pdev)
{
	struct qcom_adsp *adsp = platform_get_drvdata(pdev);

	rproc_del(adsp->rproc);

	qcom_q6v5_deinit(&adsp->q6v5);
	qcom_remove_glink_subdev(adsp->rproc, &adsp->glink_subdev);
	qcom_remove_pdm_subdev(adsp->rproc, &adsp->pdm_subdev);
	qcom_remove_sysmon_subdev(adsp->sysmon);
	qcom_remove_ssr_subdev(adsp->rproc, &adsp->ssr_subdev);
	qcom_rproc_pds_detach(adsp);
}

static const struct adsp_pil_data adsp_resource_init = {
	.crash_reason_smem = 423,
	.firmware_name = "adsp.mdt",
	.ssr_name = "lpass",
	.sysmon_name = "adsp",
	.ssctl_id = 0x14,
	.is_wpss = false,
	.auto_boot = true,
	.clk_ids = (const char*[]) {
		"sway_cbcr", "lpass_ahbs_aon_cbcr", "lpass_ahbm_aon_cbcr",
		"qdsp6ss_xo", "qdsp6ss_sleep", "qdsp6ss_core", NULL
	},
	.num_clks = 7,
	.pd_names = (const char*[]) { "cx" },
	.num_pds = 1,
};

static const struct adsp_pil_data adsp_sc7280_resource_init = {
	.crash_reason_smem = 423,
	.firmware_name = "adsp.pbn",
	.load_state = "adsp",
	.ssr_name = "lpass",
	.sysmon_name = "adsp",
	.ssctl_id = 0x14,
	.has_iommu = true,
	.auto_boot = true,
	.clk_ids = (const char*[]) {
		"gcc_cfg_noc_lpass", NULL
	},
	.num_clks = 1,
};

static const struct adsp_pil_data cdsp_resource_init = {
	.crash_reason_smem = 601,
	.firmware_name = "cdsp.mdt",
	.ssr_name = "cdsp",
	.sysmon_name = "cdsp",
	.ssctl_id = 0x17,
	.is_wpss = false,
	.auto_boot = true,
	.clk_ids = (const char*[]) {
		"sway", "tbu", "bimc", "ahb_aon", "q6ss_slave", "q6ss_master",
		"q6_axim", NULL
	},
	.num_clks = 7,
	.pd_names = (const char*[]) { "cx" },
	.num_pds = 1,
};

static const struct adsp_pil_data wpss_resource_init = {
	.crash_reason_smem = 626,
	.firmware_name = "wpss.mdt",
	.ssr_name = "wpss",
	.sysmon_name = "wpss",
	.ssctl_id = 0x19,
	.is_wpss = true,
	.auto_boot = false,
	.load_state = "wpss",
	.clk_ids = (const char*[]) {
		"ahb_bdg", "ahb", "rscp", NULL
	},
	.num_clks = 3,
	.pd_names = (const char*[]) { "cx", "mx" },
	.num_pds = 2,
};

static const struct x1e_dsp_data x1e80100_adsp_x1e = {
	.pas_id = 1,
	.dtb_pas_id = 0x24,
	.lite_pas_id = 0x1f,
	.lite_dtb_pas_id = 0x29,
	.core_rcg = 0x8000,
	.dtb_firmware_name = "adsp_dtb.mdt",
};

static const struct adsp_pil_data x1e80100_adsp_resource = {
	.crash_reason_smem = 423,
	.firmware_name = "adsp.mdt",
	.load_state = "adsp",
	.ssr_name = "lpass",
	.sysmon_name = "adsp",
	.ssctl_id = 0x14,
	.auto_boot = true,
	.clk_ids = (const char*[]) { NULL },
	.pd_names = (const char*[]) { "lcx", "lmx" },
	.num_pds = 2,
	.x1e = &x1e80100_adsp_x1e,
};

static const struct x1e_dsp_data x1e80100_cdsp_x1e = {
	.is_cdsp = true,
	.pas_id = 18,
	.dtb_pas_id = 0x25,
	.core_rcg = 0x8020,
	.dtb_firmware_name = "cdsp_dtb.mdt",
};

static const struct adsp_pil_data x1e80100_cdsp_resource = {
	.crash_reason_smem = 601,
	.firmware_name = "cdsp.mdt",
	.load_state = "cdsp",
	.ssr_name = "cdsp",
	.sysmon_name = "cdsp",
	.ssctl_id = 0x17,
	.auto_boot = true,
	.clk_ids = (const char*[]) { NULL },
	.pd_names = (const char*[]) { "cx", "mxc", "nsp" },
	.num_pds = 3,
	.x1e = &x1e80100_cdsp_x1e,
};

static const struct of_device_id adsp_of_match[] = {
	{ .compatible = "qcom,qcs404-cdsp-pil", .data = &cdsp_resource_init },
	{ .compatible = "qcom,sc7280-adsp-pil", .data = &adsp_sc7280_resource_init },
	{ .compatible = "qcom,sc7280-wpss-pil", .data = &wpss_resource_init },
	{ .compatible = "qcom,sdm845-adsp-pil", .data = &adsp_resource_init },
	{ .compatible = "qcom,x1e80100-adsp-pil", .data = &x1e80100_adsp_resource },
	{ .compatible = "qcom,x1e80100-cdsp-pil", .data = &x1e80100_cdsp_resource },
	{ },
};
MODULE_DEVICE_TABLE(of, adsp_of_match);

static struct platform_driver adsp_pil_driver = {
	.probe = adsp_probe,
	.remove = adsp_remove,
	.driver = {
		.name = "qcom_q6v5_adsp",
		.of_match_table = adsp_of_match,
	},
};

module_platform_driver(adsp_pil_driver);
MODULE_DESCRIPTION("QTI SDM845 ADSP Peripheral Image Loader");
MODULE_LICENSE("GPL v2");
