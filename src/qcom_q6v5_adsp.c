// SPDX-License-Identifier: GPL-2.0
/*
 * Qualcomm Technology Inc. ADSP Peripheral Image Loader for SDM845.
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 */

#include <linux/of_address.h>
#include <linux/firmware/qcom/qcom_pas.h>
#include <linux/elf.h>
#include <linux/arm-smccc.h>
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

/* X1E80100 at EL2: second (DTB) image and the PAS ids UEFI may have used */
struct x1e_dsp_data {
	bool is_cdsp;
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
	bool test_channel_mapped;
	unsigned long extra_mapped;
	DECLARE_BITMAP(qhee_mapped, 128);
	bool clk_saved;
	u32 clk_saved_val[11];
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
 * X1E80100 ADSP/CDSP for Linux running at EL2 without a hypervisor. The clock/PLL bring-up, boot and stop sequences were recovered from
 * the Qualcomm hypervisor (QHEE .367) that normally runs them, and are replayed as plain
 * MMIO accesses on the absolute addresses QHEE uses.
 *
 * SLOPWARE: written largely by AI agents from reverse-engineered firmware and tested on
 * one ASUS Vivobook S15. Several of these registers belong to clock controllers that
 * have (or should have) their own drivers.
 */

#define QHEE_CDSP_MAP_MAX	128
#define QHEE_CLK_SAVE_COUNT	11

/* TEST ONLY: QHEE .367 E-069 CDSP clock recipe, with bounded waits. */

enum cdsp_test_op { CT_RMW, CT_WRITE, CT_PRESERVE, CT_DELAY };
struct cdsp_test_step {
	phys_addr_t addr;
	u32 clear, set, poll_mask, poll_value, timeout_us;
	enum cdsp_test_op op;
	bool branch_poll;
};
#define CT(a,c,s) { .addr=(a), .clear=(c), .set=(s) }
#define CW(a,s) { .addr=(a), .set=(s), .op=CT_WRITE }
#define CP(a,c,s,m,v,t,b) { .addr=(a), .clear=(c), .set=(s), \
	.poll_mask=(m), .poll_value=(v), .timeout_us=(t), .branch_poll=(b) }
#define CD(d) { .set=(d), .op=CT_DELAY }

static const struct cdsp_test_step cdsp_setup_steps[] = {
	CP(0x00145008, 0, 1, BIT(31), 0, 10000, true),
	CT(0x32008404, 0, 2), CT(0x32008220, 0, 2), CT(0x3200822c, 0, 2),
	CP(0x32008418, 0, 1, BIT(31), 0, 10000, true),
	CT(0x32018034, 1, 0), CT(0x32008418, 1, 0),
	CP(0x32008414, 0, 1, BIT(31), 0, 10000, true),
	CP(0x32008080, 1, 0, BIT(31), BIT(31), 100000, false),
	CT(0x32008080, 0, 0x800), CT(0x32008080, 0, 1),
	{ .addr=0x32300018, .op=CT_PRESERVE }, /* QHEE RAM253c10 unknown */
	CT(0x32348040, 0, 1),
};
static const struct cdsp_test_step cdsp_pll_steps[] = {
	CT(0x32340010, 0xff, 0x32),
	CT(0x32340018, ~0xee3c03ffU, 0), CW(0x32340014, 0),
	CT(0x32340000, 0, 4),
	CP(0x32340000, 0, 0x4000, BIT(13), BIT(13), 100000, false),
	CP(0x32340000, 0x4000, 0, BIT(13), 0, 100000, false), CD(1),
	CW(0x32340020, 0x20485699), CW(0x32340024, 0x00182261),
	CW(0x32340028, 0x82aa299c), CW(0x3234002c, 0),
	CW(0x32340030, 3), CW(0x32340034, 0x9000),
	CW(0x32340038, 0x34), CW(0x32340018, 0), CW(0x3234001c, 5),
	CT(0x32340010, 0x00ff0000, 0x00440000),
	CW(0x32340004, 1), CT(0x32340000, 0, 4),
	CT(0x32340000, 0, 1), CT(0x32340018, 0, 1),
	CW(0x32348024, 0x00100201),
	CP(0x32348020, 0, 1, BIT(0), 0, 100000, false), CD(5),
	CT(0x32300400, 0, 1),
};

static int cdsp_test_read(struct qcom_adsp *pas, phys_addr_t addr, u32 *val)
{
	void __iomem *p;
	/* Hard exclusion, even if future table entries are accidentally added. */
	if (addr >= 0xb220000 && addr < 0xb230000)
		return -EPERM;
	dev_dbg(pas->dev, "read before addr=%pa\n", &addr);
	p = ioremap(addr, 4);
	if (!p)
		return -ENOMEM;
	*val = readl(p);
	iounmap(p);
	dev_dbg(pas->dev, "read after addr=%pa value=%#010x\n", &addr, *val);
	return 0;
}

static int cdsp_test_step(struct qcom_adsp *pas, const char *phase, int n,
			  const struct cdsp_test_step *s)
{
	void __iomem *p;
	u32 old, val;
	int ret = 0;
	if (s->op == CT_DELAY) {
		dev_dbg(pas->dev, "%s[%d] delay before %uus\n", phase, n, s->set);
		udelay(s->set);
		dev_dbg(pas->dev, "%s[%d] delay after\n", phase, n);
		return 0;
	}
	if (s->addr >= 0xb220000 && s->addr < 0xb230000)
		return -EPERM;
	dev_dbg(pas->dev, "%s[%d] before addr=%pa op=%d clear=%#x set=%#x\n",
		 phase, n, &s->addr, s->op, s->clear, s->set);
	p = ioremap(s->addr, 4);
	if (!p)
		return -ENOMEM;
	old = 0;
	if (s->op != CT_WRITE) {
		dev_dbg(pas->dev, "%s[%d] RMW/preserve read before\n", phase, n);
		old = readl(p);
		dev_dbg(pas->dev, "%s[%d] read completed old=%#010x\n", phase, n, old);
	}
	if (s->op == CT_PRESERVE) {
		dev_dbg(pas->dev, "%s[%d] UNKNOWN QHEE RAM253c10: preserve reg=%#010x, no write\n",
			 phase, n, old);
		val = old;
		goto done;
	}
	val = s->op == CT_WRITE ? s->set : (old & ~s->clear) | s->set;
	dev_dbg(pas->dev, "%s[%d] write before value=%#010x\n", phase, n, val);
	writel(val, p);
	dev_dbg(pas->dev, "%s[%d] write issued; no extra readback\n", phase, n);
	if (s->poll_mask && !(s->branch_poll && (old & BIT(1)))) {
		dev_dbg(pas->dev, "%s[%d] poll before mask=%#x wanted=%#x limit=%uus\n",
			 phase, n, s->poll_mask, s->poll_value, s->timeout_us);
		ret = readl_poll_timeout(p, val, (val & s->poll_mask) == s->poll_value,
					1, s->timeout_us);
		dev_dbg(pas->dev, "%s[%d] poll after ret=%d value=%#010x\n",
			 phase, n, ret, val);
	}
done:
	dev_dbg(pas->dev, "%s[%d] after addr=%pa old=%#010x value=%#010x ret=%d\n",
		 phase, n, &s->addr, old, val, ret);
	iounmap(p);
	return ret;
}

static int cdsp_test_run(struct qcom_adsp *pas, const char *phase,
			 const struct cdsp_test_step *steps, size_t count)
{
	int i, ret;
	for (i = 0; i < count; i++) {
		ret = cdsp_test_step(pas, phase, i, &steps[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int cdsp_test_calibration(struct qcom_adsp *pas)
{
	u32 control, lo, hi, t = 0;
	int ret;
	struct cdsp_test_step steps[] = {
		CW(0x32350008, 1), CW(0x32350004, 0x8028550d),
		CW(0x32350028, 0x9f1), CW(0x32350030, 0x020f9ac0),
		CW(0x32350038, 0x037d2002), CW(0x323500a4, 0x30),
		CW(0x323500ac, 0x10000), CW(0x3235003c, 1),
		CW(0x32350014, 0), CD(1),
		CW(0x32350014, 0), CD(10), CW(0x32350014, 0), CD(1),
	};
	ret = cdsp_test_read(pas, 0x221c2bec, &control);
	if (ret || !(control & 0x1fe000))
		return ret;
	if (control & BIT(16)) {
		ret = cdsp_test_read(pas, 0x221c2be0, &lo);
		if (ret)
			return ret;
		ret = cdsp_test_read(pas, 0x221c2be4, &hi);
		if (ret)
			return ret;
		t = (lo >> 13) | ((hi & 0x1f) << 19);
	}
	steps[8].set = t;
	steps[10].set = t | BIT(31);
	steps[12].set = t;
	return cdsp_test_run(pas, "calibration", steps, ARRAY_SIZE(steps));
}

static int cdsp_test_enable(struct qcom_adsp *pas)
{
	static const struct cdsp_test_step initial[] = {
		CW(0x3230001c, 0), CW(0x32300404, 1),
	};
	void __iomem *p;
	u32 status;
	int ret;
	ret = cdsp_test_run(pas, "enable-reset", initial, ARRAY_SIZE(initial));
	if (ret)
		return ret;
	dev_dbg(pas->dev, "status before 32300408 nonzero limit=1000000us\n");
	p = ioremap(0x32300408, 4);
	if (!p)
		return -ENOMEM;
	ret = readl_poll_timeout(p, status, status != 0, 5, 1000000);
	iounmap(p);
	dev_dbg(pas->dev, "status after value=%#010x ret=%d\n", status, ret);
	/* E-069 continues on status timeout; record it, preserve that behavior. */
	udelay(5);
	return cdsp_test_run(pas, "enable-pll", cdsp_pll_steps, ARRAY_SIZE(cdsp_pll_steps));
}

static int x1pas_qhee_bringup(struct qcom_adsp *pas, struct rproc *rproc)
{
	const struct elf32_hdr *ehdr = (const struct elf32_hdr *)pas->firmware->data;
	struct cdsp_test_step boot = CW(0x32300010, 0);
	u32 version, val;
	u64 entry;
	int ret;
	ret = cdsp_test_read(pas, 0x01fc8000, &version);
	if (ret)
		return ret;
	if ((version >> 28) != 0xa ||
	    (((version >> 16) & 0xfff) != 9 && ((version >> 16) & 0xfff) != 0x16))
		return -ENODEV;
	dev_dbg(pas->dev, "ClockSetup(8,3) before selector=%#x\n", version);
	ret = cdsp_test_run(pas, "setup", cdsp_setup_steps, ARRAY_SIZE(cdsp_setup_steps));
	if (!ret && ((version >> 16) & 0xfff) == 9)
		ret = cdsp_test_calibration(pas);
	dsb(sy);
	dev_dbg(pas->dev, "ClockSetup(8,3) after ret=%d\n", ret);
	if (ret)
		return ret;
	entry = (u64)ehdr->e_entry + pas->mem_phys - pas->mem_reloc;
	ret = cdsp_test_read(pas, 0x32300010, &val);
	if (ret)
		return ret;
	boot.set = (val & 0xf000000f) | ((((u32)(entry >> 8)) & 0xffffff) << 4);
	ret = cdsp_test_step(pas, "boot-address", 0, &boot);
	dsb(sy);
	if (ret)
		return ret;
	dev_dbg(pas->dev, "ClockEnable(8,3) before\n");
	ret = cdsp_test_enable(pas);
	dev_dbg(pas->dev, "ClockEnable(8,3) after ret=%d\n", ret);
	if (ret)
		return ret;
	/* QHEE private channel is not a Linux reserved-memory region. */
	dev_dbg(pas->dev, "channel clear SKIPPED: Linux ownership unproven\n");
	return 0;
}

/* The IOMMU domain only exists while the rproc framework has it attached */
static bool x1pas_has_domain(struct rproc *rproc)
{
	return rproc->has_iommu && rproc->domain;
}

static void x1pas_unmap_carveout(struct rproc *rproc, phys_addr_t mem_phys, size_t size)
{
	if (x1pas_has_domain(rproc))
		iommu_unmap(rproc->domain, mem_phys, size);
}

/* Test: identity-map shared regions a hypervisor would normally grant the DSP */
static const char * const pas_extra_regions[] = {
	"/reserved-memory/smem@ffe00000",
	"/reserved-memory/aop-cmd-db@81c60000",
};

static int x1pas_extra_resource(int i, struct resource *res)
{
	struct device_node *np;
	int ret;

	np = of_find_node_by_path(pas_extra_regions[i]);
	if (!np)
		return -ENOENT;
	ret = of_address_to_resource(np, 0, res);
	of_node_put(np);
	return ret;
}

static void x1pas_map_extra(struct qcom_adsp *pas, struct rproc *rproc, bool map)
{
	struct resource res;
	int i, ret;

	if (!x1pas_has_domain(rproc))
		return;
	for (i = 0; i < ARRAY_SIZE(pas_extra_regions); i++) {
		if (map == !!(pas->extra_mapped & BIT(i)))
			continue;
		if (x1pas_extra_resource(i, &res))
			continue;
		if (map) {
			ret = iommu_map(rproc->domain, res.start, res.start, resource_size(&res),
					IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
			dev_dbg(&rproc->dev, "extra map %pR: %d\n", &res, ret);
			if (!ret)
				pas->extra_mapped |= BIT(i);
		} else {
			iommu_unmap(rproc->domain, res.start, resource_size(&res));
			pas->extra_mapped &= ~BIT(i);
		}
	}
}

/* CDSP (VMID 0x1e) static apertures from QHEE .367, ELF VA 0xccc08: {pa, size, perm} */
static const struct { u64 pa; u64 size; u32 perm; } qhee_cdsp_map[] = {
	{ 0x100000, 0x1000, 0x4 },
	{ 0x101000, 0x1000, 0x4 },
	{ 0x102000, 0x1000, 0x4 },
	{ 0x103000, 0x1000, 0x4 },
	{ 0x104000, 0x1000, 0x4 },
	{ 0x105000, 0x1000, 0x4 },
	{ 0x106000, 0x1000, 0x4 },
	{ 0x107000, 0x1000, 0x4 },
	{ 0x108000, 0x1000, 0x4 },
	{ 0x109000, 0x1000, 0x4 },
	{ 0x10a000, 0x1000, 0x4 },
	{ 0x10b000, 0x1000, 0x4 },
	{ 0x118000, 0x1000, 0x6 },
	{ 0x11e000, 0x1000, 0x6 },
	{ 0x123000, 0x1000, 0x6 },
	{ 0x138000, 0x1000, 0x4 },
	{ 0x142000, 0x1000, 0x6 },
	{ 0x145000, 0x1000, 0x6 },
	{ 0x15b000, 0x1000, 0x6 },
	{ 0x162000, 0x1000, 0x6 },
	{ 0x1a6000, 0x1000, 0x6 },
	{ 0x406000, 0x1000, 0x6 },
	{ 0x446000, 0x1000, 0x6 },
	{ 0x486000, 0x1000, 0x6 },
	{ 0x4fc000, 0x1000, 0x4 },
	{ 0x526000, 0x7000, 0x6 },
	{ 0x63c000, 0x4000, 0x6 },
	{ 0x640000, 0x9000, 0x2 },
	{ 0x650000, 0x9000, 0x2 },
	{ 0x10ca000, 0x1000, 0x4 },
	{ 0x17af000, 0x1000, 0x6 },
	{ 0x1f40000, 0x40000, 0x6 },
	{ 0x1fc0000, 0x10000, 0x4 },
	{ 0x1fd1000, 0x1a000, 0x4 },
	{ 0x1feb000, 0x1000, 0x6 },
	{ 0x1fec000, 0x4000, 0x4 },
	{ 0xb2a0000, 0x10000, 0x6 },
	{ 0xb5f0000, 0x1000, 0x4 },
	{ 0xbbf0000, 0x2000, 0x4 },
	{ 0xc20a000, 0x1000, 0x6 },
	{ 0xc210000, 0x80000, 0x4 },
	{ 0xc330000, 0x1000, 0x6 },
	{ 0xc370000, 0x1000, 0x4 },
	{ 0xc380000, 0x1000, 0x4 },
	{ 0xc390000, 0x1000, 0x4 },
	{ 0xc3a0000, 0x1000, 0x4 },
	{ 0xc3b0000, 0x1000, 0x4 },
	{ 0xc3c0000, 0x1000, 0x4 },
	{ 0xc3d0000, 0x1000, 0x4 },
	{ 0xc3e0000, 0x1000, 0x4 },
	{ 0xc3f0000, 0x1000, 0x4 },
	{ 0xc400000, 0x3000, 0x6 },
	{ 0xc406000, 0x11000, 0x4 },
	{ 0xc440000, 0x80000, 0x6 },
	{ 0xf000000, 0x1000000, 0x6 },
	{ 0x10001000, 0x1000, 0x6 },
	{ 0x10002000, 0x1000, 0x6 },
	{ 0x10041000, 0x1000, 0x6 },
	{ 0x10042000, 0x1000, 0x6 },
	{ 0x10045000, 0x1000, 0x6 },
	{ 0x10046000, 0x1000, 0x6 },
	{ 0x10048000, 0x1000, 0x6 },
	{ 0x10980000, 0x1000, 0x6 },
	{ 0x10981000, 0x1000, 0x6 },
	{ 0x146aa000, 0x1000, 0x6 },
	{ 0x16000000, 0x1000000, 0x2 },
	{ 0x20c21000, 0x1000, 0x4 },
	{ 0x221c2000, 0x1000, 0x4 },
	{ 0x221c8000, 0x1000, 0x4 },
	{ 0x24080000, 0x1000, 0x4 },
	{ 0x240b8000, 0x1000, 0x6 },
	{ 0x2500a000, 0x1000, 0x6 },
	{ 0x25011000, 0x1000, 0x6 },
	{ 0x25021000, 0x1000, 0x4 },
	{ 0x2520a000, 0x1000, 0x6 },
	{ 0x25211000, 0x1000, 0x6 },
	{ 0x25221000, 0x1000, 0x4 },
	{ 0x2540a000, 0x1000, 0x6 },
	{ 0x25411000, 0x1000, 0x6 },
	{ 0x25421000, 0x1000, 0x4 },
	{ 0x2560a000, 0x1000, 0x6 },
	{ 0x25611000, 0x1000, 0x6 },
	{ 0x25621000, 0x1000, 0x4 },
	{ 0x2580a000, 0x1000, 0x6 },
	{ 0x25811000, 0x1000, 0x6 },
	{ 0x25821000, 0x1000, 0x4 },
	{ 0x25a0a000, 0x1000, 0x6 },
	{ 0x25a11000, 0x1000, 0x6 },
	{ 0x25a21000, 0x1000, 0x4 },
	{ 0x25c0a000, 0x1000, 0x6 },
	{ 0x25c11000, 0x1000, 0x6 },
	{ 0x25c21000, 0x1000, 0x4 },
	{ 0x25e0a000, 0x1000, 0x6 },
	{ 0x25e11000, 0x1000, 0x6 },
	{ 0x25e21000, 0x1000, 0x4 },
	{ 0x2600a000, 0x1000, 0x6 },
	{ 0x26011000, 0x1000, 0x6 },
	{ 0x26021000, 0x1000, 0x4 },
	{ 0x2620a000, 0x1000, 0x6 },
	{ 0x26211000, 0x1000, 0x6 },
	{ 0x26221000, 0x1000, 0x4 },
	{ 0x320cc000, 0x3000, 0x6 },
	{ 0x322fa000, 0x1000, 0x6 },
};

static_assert(ARRAY_SIZE(qhee_cdsp_map) <= QHEE_CDSP_MAP_MAX);

static void x1pas_map_qhee(struct qcom_adsp *pas, struct rproc *rproc)
{
	int i, ret, fails = 0;

	if (!x1pas_has_domain(rproc))
		return;
	x1pas_map_extra(pas, rproc, true);	/* SMEM + cmd-db as normal memory */
	for (i = 0; i < ARRAY_SIZE(qhee_cdsp_map); i++) {
		int prot = IOMMU_MMIO;

		if (test_bit(i, pas->qhee_mapped))
			continue;
		if (qhee_cdsp_map[i].perm & 4)
			prot |= IOMMU_READ;
		if (qhee_cdsp_map[i].perm & 2)
			prot |= IOMMU_WRITE;
		dev_dbg(&rproc->dev, "map[%d] before pa=%llx size=%llx\n", i, qhee_cdsp_map[i].pa, qhee_cdsp_map[i].size);
		ret = iommu_map(rproc->domain, qhee_cdsp_map[i].pa, qhee_cdsp_map[i].pa,
				qhee_cdsp_map[i].size, prot, GFP_KERNEL);
		dev_dbg(&rproc->dev, "map[%d] after ret=%d\n", i, ret);
		if (ret) {
			fails++;
			dev_warn(&rproc->dev, "qhee map %llx+%llx: %d\n",
				 qhee_cdsp_map[i].pa, qhee_cdsp_map[i].size, ret);
		} else {
			set_bit(i, pas->qhee_mapped);
		}
	}
	dev_info(&rproc->dev, "QHEE CDSP apertures mapped: %zu, failed %d\n",
		 ARRAY_SIZE(qhee_cdsp_map), fails);
}

/* Undo x1pas_map_qhee() and the test channel mapping done in start() */
static void x1pas_unmap_qhee(struct qcom_adsp *pas, struct rproc *rproc)
{
	int i;

	if (!x1pas_has_domain(rproc)) {
		pas->test_channel_mapped = false;
		pas->extra_mapped = 0;
		bitmap_zero(pas->qhee_mapped, QHEE_CDSP_MAP_MAX);
		return;
	}

	if (pas->test_channel_mapped) {
		iommu_unmap(rproc->domain, 0x827dc000, 0x2000);
		pas->test_channel_mapped = false;
	}

	for_each_set_bit(i, pas->qhee_mapped, ARRAY_SIZE(qhee_cdsp_map)) {
		iommu_unmap(rproc->domain, qhee_cdsp_map[i].pa, qhee_cdsp_map[i].size);
		clear_bit(i, pas->qhee_mapped);
	}

	x1pas_map_extra(pas, rproc, false);
}

static bool clk_teardown = true;
module_param(clk_teardown, bool, 0644);
MODULE_PARM_DESC(clk_teardown, "Turn off clocks/PLL enabled by the QHEE bring-up on stop");

/*
 * Clock enables left on by the QHEE bring-up (qhee_cdsp_test.h). Only the
 * bits in .mask are put back to their pre-bring-up value on teardown.
 */
enum {
	QCLK_GCC_BRANCH,	/* 0x00145008 CBCR, enabled in setup[0] */
	QCLK_8414,		/* 0x32008414 CBCR, enabled in setup */
	QCLK_8080,		/* 0x32008080 CBCR enable + bit 11 */
	QCLK_CORE_CBCR,		/* 0x32348040 enabled in setup */
	QCLK_PLL_MODE,		/* 0x32340000 OUTCTRL/BYPASSNL/RESET_N */
	QCLK_PLL_OPMODE,	/* 0x32340004 */
	QCLK_PLL_USER,		/* 0x32340018 main output enable */
	QCLK_RCG_CFG,		/* 0x32348024 core RCG source/divider */
	QCLK_8404_HW,		/* 0x32008404 CBCR, HW_CTL set in setup */
	QCLK_8220_HW,		/* 0x32008220 CBCR, HW_CTL set in setup */
	QCLK_822C_HW,		/* 0x3200822c CBCR, HW_CTL set in setup */
};

static const struct { phys_addr_t addr; u32 mask; } qhee_clk_save[] = {
	[QCLK_GCC_BRANCH]	= { 0x00145008, BIT(0) },
	[QCLK_8414]		= { 0x32008414, BIT(0) },
	[QCLK_8080]		= { 0x32008080, BIT(11) | BIT(0) },
	[QCLK_CORE_CBCR]	= { 0x32348040, BIT(0) },
	[QCLK_PLL_MODE]		= { 0x32340000, BIT(2) | BIT(1) | BIT(0) },
	[QCLK_PLL_OPMODE]	= { 0x32340004, ~0U },
	[QCLK_PLL_USER]		= { 0x32340018, BIT(0) },
	[QCLK_RCG_CFG]		= { 0x32348024, ~0U },
	[QCLK_8404_HW]		= { 0x32008404, BIT(1) | BIT(0) },
	[QCLK_8220_HW]		= { 0x32008220, BIT(1) | BIT(0) },
	[QCLK_822C_HW]		= { 0x3200822c, BIT(1) | BIT(0) },
};
static_assert(ARRAY_SIZE(qhee_clk_save) == QHEE_CLK_SAVE_COUNT);

#define QCLK_RCG_CMD		0x32348020

/* Snapshot the registers the bring-up is about to turn on */
static int x1pas_clk_save(struct qcom_adsp *pas)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(qhee_clk_save); i++) {
		ret = cdsp_test_read(pas, qhee_clk_save[i].addr, &pas->clk_saved_val[i]);
		if (ret)
			return ret;
	}
	pas->clk_saved = true;

	return 0;
}

/* Put the @mask bits of saved register @idx back, returns true if written */
static bool x1pas_clk_restore(struct qcom_adsp *pas, int idx, u32 mask)
{
	void __iomem *p;
	u32 cur, val;

	p = ioremap(qhee_clk_save[idx].addr, 4);
	if (!p)
		return false;
	cur = readl(p);
	val = (cur & ~mask) | (pas->clk_saved_val[idx] & mask);
	if (val != cur)
		writel(val, p);
	iounmap(p);
	dev_dbg(pas->dev, "teardown %pa %#010x -> %#010x\n",
		&qhee_clk_save[idx].addr, cur, val);

	return val != cur;
}

/*
 * Undo the clock part of x1pas_qhee_bringup() in reverse order. Must run
 * after PAS shutdown (core held in reset) with the proxy votes held, so the
 * NSP register space is powered.
 */
static void x1pas_clk_teardown(struct qcom_adsp *pas)
{
	void __iomem *cmd;
	u32 val;
	int ret;

	if (!pas->clk_saved)
		return;
	if (!clk_teardown) {
		dev_dbg(pas->dev, "clock teardown disabled\n");
		return;
	}

	/* Core RCG back to its original source before the PLL goes away */
	if (x1pas_clk_restore(pas, QCLK_RCG_CFG, qhee_clk_save[QCLK_RCG_CFG].mask)) {
		cmd = ioremap(QCLK_RCG_CMD, 4);
		if (cmd) {
			writel(readl(cmd) | BIT(0), cmd);
			ret = readl_poll_timeout(cmd, val, !(val & BIT(0)), 1, 100000);
			if (ret)
				dev_warn(pas->dev, "core RCG update timed out: %#x\n", val);
			iounmap(cmd);
		}
	}

	/* PLL: outputs off, main output off, standby, then reset */
	x1pas_clk_restore(pas, QCLK_PLL_MODE, BIT(0));
	x1pas_clk_restore(pas, QCLK_PLL_USER, qhee_clk_save[QCLK_PLL_USER].mask);
	x1pas_clk_restore(pas, QCLK_PLL_OPMODE, qhee_clk_save[QCLK_PLL_OPMODE].mask);
	x1pas_clk_restore(pas, QCLK_PLL_MODE, qhee_clk_save[QCLK_PLL_MODE].mask);

	/* Branch clocks, last enabled first */
	x1pas_clk_restore(pas, QCLK_CORE_CBCR, qhee_clk_save[QCLK_CORE_CBCR].mask);
	x1pas_clk_restore(pas, QCLK_8080, BIT(0));
	x1pas_clk_restore(pas, QCLK_8080, qhee_clk_save[QCLK_8080].mask);
	x1pas_clk_restore(pas, QCLK_8414, qhee_clk_save[QCLK_8414].mask);
	x1pas_clk_restore(pas, QCLK_822C_HW, qhee_clk_save[QCLK_822C_HW].mask);
	x1pas_clk_restore(pas, QCLK_8220_HW, qhee_clk_save[QCLK_8220_HW].mask);
	x1pas_clk_restore(pas, QCLK_8404_HW, qhee_clk_save[QCLK_8404_HW].mask);
	x1pas_clk_restore(pas, QCLK_GCC_BRANCH, qhee_clk_save[QCLK_GCC_BRANCH].mask);
	dsb(sy);

	pas->clk_saved = false;
}

/* Live test: QHEE selector a,9 ADSP backend, bounded polls. */
/* Values independently read before reset on this exact laptop, not defaults for other systems. */
static unsigned int adsp_chip_ids = 0x0088022b;
module_param(adsp_chip_ids, uint, 0444);
static unsigned int adsp_chip_version = 0x201;
module_param(adsp_chip_version, uint, 0444);
static unsigned int adsp_platform_info = 0x28000200;
module_param(adsp_platform_info, uint, 0444);
static int adsp_op(struct qcom_adsp *pas, u32 a, u32 clear, u32 set)
{
	struct cdsp_test_step s = CT(a, clear, set);
	return cdsp_test_step(pas, "ADSP-rmw", 0, &s);
}
static int adsp_wr(struct qcom_adsp *pas, u32 a, u32 v)
{
	struct cdsp_test_step s = CW(a, v);
	return cdsp_test_step(pas, "ADSP-write", 0, &s);
}
static int adsp_rd(struct qcom_adsp *pas, u32 a, u32 *v)
{
	return cdsp_test_read(pas, a, v);
}
static int adsp_wait(struct qcom_adsp *pas, u32 a, u32 mask, u32 wanted, u32 us)
{
	void __iomem *p = ioremap(a, 4);
	u32 v;
	int ret;
	if (!p) return -ENOMEM;
	dev_dbg(pas->dev, "ADSP-LIVE wait %08x mask=%x value=%x limit=%u\n", a, mask, wanted, us);
	ret = readl_poll_timeout(p, v, (v & mask) == wanted, 1, us);
	iounmap(p);
	dev_dbg(pas->dev, "ADSP-LIVE wait result=%d status=%x\n", ret, v);
	return ret;
}
static int adsp_branch(struct qcom_adsp *pas, u32 a)
{
	u32 v;
	int ret = adsp_op(pas, a, 0, 1);
	if (ret) return ret;
	ret = adsp_rd(pas, a, &v);
	if (ret || (v & BIT(1))) return ret;
	return adsp_wait(pas, a, BIT(31), 0, 1000);
}
#define AO(a,c,s) do { ret=adsp_op(pas,a,c,s); if(ret) return ret; } while(0)
#define AW(a,v) do { ret=adsp_wr(pas,a,v); if(ret) return ret; } while(0)
#define AR(a,v) do { ret=adsp_rd(pas,a,&(v)); if(ret) return ret; } while(0)
#define AP(a,m,v,t) do { ret=adsp_wait(pas,a,m,v,t); if(ret) return ret; } while(0)
#define AB(a) do { ret=adsp_branch(pas,a); if(ret) return ret; } while(0)

static int live_adsp_shutdown(struct qcom_adsp *pas)
{
	struct arm_smccc_res res;
	u32 v;
	int ret, i;
	dev_dbg(pas->dev, "ADSP-LIVE hardware shutdown begin\n");
	/* Original a3e58 -> 7d714(0), BLSP permission cleanup before ClockStop. */
	arm_smccc_smc(0xc2000405, 1, 0, 0, 0, 0, 0, 0, &res);
	dev_dbg(pas->dev, "ADSP-LIVE BLSP SMC c2000405(1,0)=%lx,%lx\n", res.a0, res.a1);
	if (res.a0) return -EIO;
	AR(0x07e00000,v);
	if (v & BIT(31)) {
		AR(0x07e02000,v); if(v&1) AW(0x07e02000,0);
		AO(0x07e00000,BIT(11),0);
		AO(0x07b25004,0x700,0); AO(0x07b25004,0x1f,0);
		AO(0x07b25000,0,1); AP(0x07b25000,1,0,100000);
		AO(0x07b00004,7,0); AO(0x06e14000,0,1);
		AR(0x07e00000,v); if(!(v&1)) AO(0x07e00000,0,1);
		AP(0x07e00000,BIT(31),0,100000);
	}
	AO(0x07e02000,1,0); AO(0x07e00000,BIT(11),0);
	AR(0x07e0a008,v);
	if (!(v&1)) {
		AB(0x06e18004);
		AR(0x06e11090,v);
		if (!(v&BIT(31))) {
			AO(0x06e11090,1,0);
			AP(0x06e11090,BIT(31),BIT(31),12000);
			AP(0x06e110ac,BIT(31),BIT(31),12000);
		}
		AB(0x06e11014);
		AO(0x06e1108c,0,1);
		ret=adsp_wait(pas,0x06e1108c,BIT(31),BIT(31),300);
		if(ret) { AO(0x06e1108c,0,4); udelay(250); AO(0x06e1108c,4,0); }
		AO(0x06e1108c,1,0); udelay(150);
		AO(0x06bc0000,1,0); AO(0x06bc0030,7,0);
		AO(0x06bc0000,4,0); AO(0x06bc0000,2,0); AO(0x06bc1004,7,0);
		AO(0x06e11090,0,1); AP(0x06e11090,BIT(31),0,100000);
		AO(0x0680001c,0,1); AO(0x0680001c,0,2);
	}
	AW(0x01f62000,1);
	ret=adsp_wait(pas,0x01f62004,1,1,1000000); /* original timeout continues */
	AR(0x07e0a008,v);
	if (!(v&1)) {
		AO(0x06e29020,1,0); AO(0x06e29020,0,8);
		AP(0x06e29020,BIT(31),BIT(31),100000);
		AO(0x06e29010,0,4); AO(0x06e2900c,0,4); udelay(1);
		AO(0x06e29020,0,1);
		AR(0x06e25100,v); if(v&BIT(31)) AO(0x07a0104c,0,BIT(22));
		AO(0x06e220d0,1,0);
		AR(0x06e260ac,v);
		if(v&BIT(31)) { AO(0x06901064,1,0); AO(0x06901064,2,0); AP(0x06901068,3,0,100000); }
		AR(0x06e270ac,v);
		if(v&BIT(31)) { AO(0x06911064,1,0); AO(0x06911064,2,0); AP(0x06911068,3,0,100000); }
		AO(0x06e0a000,0,16); AO(0x06e0a000,1,0);
		ret=adsp_wait(pas,0x07e0a008,~0U,1,500);
		for(i=0;ret && i<10;i++) {
			AO(0x06e0a000,0,1); udelay(5); AO(0x06e0a000,1,0);
			ret=adsp_wait(pas,0x07e0a008,~0U,1,500);
		}
	}
	AO(0x0ef01000,0,2); AO(0x0b5e1000,0,4); udelay(10000);
	AO(0x0c2f7014,0,1); udelay(200);
	AO(0x0ef01000,0,1); udelay(10); AO(0x0ef01000,1,0);
	AO(0x0b5e1000,4,0); AO(0x0c2f7014,1,0); udelay(200);
	AO(0x0ef01000,2,0); AW(0x01f62000,0); udelay(100);
	dsb(sy);
	dev_dbg(pas->dev, "ADSP-LIVE hardware shutdown complete\n");
	return 0;
}

static int live_adsp_bringup(struct qcom_adsp *pas)
{
	const struct elf32_hdr *ehdr=(const struct elf32_hdr *)pas->firmware->data;
	const struct elf32_hdr *dh=(const struct elf32_hdr *)pas->dtb_firmware->data;
	u64 entry=(u64)ehdr->e_entry+pas->mem_phys-pas->mem_reloc;
	u64 dtb_entry=(u64)dh->e_entry+pas->dtb_mem_phys-pas->dtb_mem_reloc;
	u64 low=U64_MAX,high=0;
	u32 v, boot_config;
	int ret, i;
	dev_dbg(pas->dev,"ADSP-LIVE ClockSetup(0,3) begin entry=%llx\n",entry);
	AR(0x01fc8000,v); if(v != 0xa0090201) return -ENODEV;
	/* QHEE RAM253c08 unavailable: preserve UEFI's live boot configuration. */
	AR(0x06800018,boot_config);
	AB(0x00147160);
	AR(0x07436090,v);
	if(v&4) AO(0x06e21008,1,0); else AB(0x06e21008);
	AB(0x06e21034);
	AO(0x00147010,0,BIT(20)); AO(0x06848020,0,1);
	AW(0x06800018,boot_config); AW(0x07250018,8); AB(0x0726a300);
	for(i=0;i<10;i++) {
		u32 a=0x0726a000+i*4;
		AO(a,~0x81c0300fU,0x4000);
		AO(a,~0x81c0700eU,1);
	}
	if(boot_config) {
		AO(0x06e12020,0,1); AO(0x06e12014,0,1);
		AO(0x07255014,0,4); AO(0x07255018,0,4); AO(0x07255020,0,4);
		ret=adsp_wait(pas,0x07255020,1,1,1500);
		dev_dbg(pas->dev,"ADSP-LIVE optional config wait=%d\n",ret);
	}
	/* Same PLL descriptor as b1820: L=50, output mask0xc, RCGR=0x201. */
	AO(0x06840010,0xff,0x32); AO(0x06840018,~0xee3c03ffU,0); AW(0x06840014,0);
	AO(0x06840000,0,4);
	AO(0x06840000,0,0x4000); AP(0x06840000,BIT(13),BIT(13),100000);
	AO(0x06840000,0x4000,0); AP(0x06840000,BIT(13),0,100000); udelay(1);
	AW(0x06840020,0x20485699); AW(0x06840024,0x00182261);
	AW(0x06840028,0x82aa299c); AW(0x0684002c,0); AW(0x06840030,3);
	AW(0x06840034,0x9000); AW(0x06840038,0x34);
	AW(0x06840018,0); AW(0x0684001c,5);
	AO(0x06840010,0xff0000,0x440000); AW(0x06840004,1);
	AO(0x06840000,0,4); AO(0x06840000,0,1); AO(0x06840018,0,1);
	AW(0x06848004,0x201); AO(0x06848000,0,1); AP(0x06848000,1,0,100000);
	/* a426c: reset clears these; reconstruct the actual loaded DTB descriptor. */
	if(dh->e_phentsize != sizeof(struct elf32_phdr) ||
	   (u64)dh->e_phoff+(u64)dh->e_phnum*dh->e_phentsize > pas->dtb_firmware->size)
		return -EINVAL;
	for(i=0;i<dh->e_phnum;i++) {
		const struct elf32_phdr *ph=(const void *)(pas->dtb_firmware->data+dh->e_phoff+i*dh->e_phentsize);
		if(ph->p_type != PT_LOAD || !ph->p_memsz || ((ph->p_flags>>24)&7)==2) continue;
		low=min_t(u64,low,ph->p_paddr);
		high=max_t(u64,high,(u64)ph->p_paddr+ph->p_memsz);
	}
	if(low==U64_MAX || high<=low || ALIGN(high,4096)-low>pas->dtb_mem_size)
		return -EINVAL;
	AW(0x06800060,lower_32_bits(dtb_entry)); AW(0x06800064,upper_32_bits(dtb_entry));
	AW(0x06800068,adsp_chip_ids); AW(0x0680006c,adsp_chip_version);
	AW(0x06800070,adsp_platform_info); AW(0x06800074,ALIGN(high,4096)-low);
	dev_dbg(pas->dev,"ADSP-LIVE restored DTB entry=%llx size=%llx chip=%x/%x/%x\n",
		dtb_entry,ALIGN(high,4096)-low,adsp_chip_ids,adsp_chip_version,adsp_platform_info);
	AR(0x06800010,v); AW(0x06800010,(v&0xf000000f)|(((entry>>8)&0xffffff)<<4));
	AO(0x0725b000,1,0); dsb(sy);
	AW(0x0680001c,0); AO(0x06800400,0,1); AW(0x06800404,1);
	{
		void __iomem *p=ioremap(0x06800408,4);
		if(!p) return -ENOMEM;
		ret=readl_poll_timeout(p,v,v!=0,5,1000000);
		iounmap(p);
	}
	dev_dbg(pas->dev,"ADSP-LIVE ClockEnable(0,3) ack=%x ret=%d\n",v,ret);
	udelay(5); dsb(sy);
	return 0; /* original continues on the ack timeout */
}
#undef AO
#undef AW
#undef AR
#undef AP
#undef AB

/* Test-only translation of QHEE b1114..b13f0; live status, separate RMW. */
static int live_cdsp_shutdown(struct qcom_adsp *pas)
{
	static const struct cdsp_test_step first[] = {
		CP(0x32008418, 0, 1, BIT(31), 0, 1000, true),
		CT(0x32018034, 0, 1), CT(0x3230001c, 0, 1),
		CT(0x00145008, 1, 0),
	};
	static const struct cdsp_test_step middle[] = {
		CP(0x00145008, 0, 1, BIT(31), 0, 1000, true),
		CT(0x3235003c, 1, 0), CT(0x3234a004, 0, 0xff),
		CT(0x32348020, 2, 0),
		CT(0x32348060, 0, BIT(22)), CT(0x32348080, 0, BIT(22)),
		CT(0x323480a0, 0, BIT(22)), CT(0x323480c0, 0, BIT(22)),
		CT(0x32348100, 0, BIT(22)), CT(0x323481c8, 0, BIT(22)),
		CT(0x32348040, 0, BIT(22)),
		CP(0x32008410, 0, BIT(22), BIT(31), BIT(31), 1000, false),
		CP(0x32008414, 0, BIT(22), BIT(31), BIT(31), 1000, false),
	};
	static const struct cdsp_test_step tail[] = {
		CT(0x0b5e1000, 0, BIT(7)), CW(0x0c2f7020, 1), CD(200),
		CT(0x01f74000, 1, 0), CD(10000),
		CT(0x0b5e1000, BIT(7), 0), CW(0x0c2f7020, 0),
	};
	void __iomem *p;
	u32 a, b, c, v;
	int ret;
	dev_info(pas->dev, "DSP-LIVE QHEE hardware stop begin\n");
	ret = cdsp_test_run(pas, "full-stop-first", first, ARRAY_SIZE(first));
	if (ret) return ret;
	ret = cdsp_test_read(pas, 0x01f7400c, &a);
	if (ret) return ret;
	ret = cdsp_test_read(pas, 0x01f74008, &b);
	if (ret) return ret;
	ret = cdsp_test_read(pas, 0x01f74014, &c);
	if (ret) return ret;
	if (!((a & b & c) & 1)) {
		p = ioremap(0x01f74000, 8);
		if (!p) return -ENOMEM;
		writel(readl(p) | 1, p);
		ret = readl_poll_timeout(p + 4, v, v & 1, 5, 1000000);
		iounmap(p);
		/* Original callback continues on this particular timeout. */
		dev_info(pas->dev, "DSP-LIVE stop handshake ret=%d status=%#x\n", ret, v);
	}
	ret = cdsp_test_run(pas, "full-stop-middle", middle, ARRAY_SIZE(middle));
	if (ret) return ret;
	ret = cdsp_test_run(pas, "full-stop-tail", tail, ARRAY_SIZE(tail));
	if (ret) return ret;
	dsb(sy);
	udelay(200);
	pas->clk_saved = false;
	dev_info(pas->dev, "DSP-LIVE QHEE hardware stop complete\n");
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
	if (!x1pas_has_domain(rproc))
		return 0;
	return iommu_map(rproc->domain, phys, phys, size, IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
}

static void x1e_unmap_all(struct qcom_adsp *pas, struct rproc *rproc)
{
	x1pas_unmap_qhee(pas, rproc);
	x1pas_unmap_carveout(rproc, pas->mem_phys, pas->mem_size);
	x1pas_unmap_carveout(rproc, pas->dtb_mem_phys, pas->dtb_mem_size);
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
		x1pas_map_qhee(pas, rproc);
		if (x1pas_has_domain(rproc)) {
			/* QHEE private channel page */
			ret = iommu_map(rproc->domain, 0x827dc000, 0x827dc000, 0x2000,
					IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
			if (ret)
				goto unmap;
			pas->test_channel_mapped = true;
		}
		ret = x1pas_clk_save(pas);
		if (!ret)
			ret = x1pas_qhee_bringup(pas, rproc);
	} else {
		x1pas_map_extra(pas, rproc, true);
		ret = live_adsp_bringup(pas);
	}
	if (ret)
		goto teardown;

	ret = qcom_q6v5_wait_for_start(&pas->q6v5, msecs_to_jiffies(5000));
	if (ret == -ETIMEDOUT) {
		dev_err(pas->dev, "start timed out\n");
		goto teardown;
	}

	x1e_release_fw(pas);
	return 0;

teardown:
	x1pas_clk_teardown(pas);
unmap:
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

/* Runs the QHEE hardware stop with the proxy votes held so the subsystem stays powered */
static int x1e_hw_stop(struct qcom_adsp *pas)
{
	int ret;

	ret = qcom_rproc_pds_enable(pas);
	if (ret < 0)
		return ret;
	ret = clk_prepare_enable(pas->xo);
	if (!ret) {
		ret = pas->x1e->is_cdsp ? live_cdsp_shutdown(pas) : live_adsp_shutdown(pas);
		x1pas_clk_teardown(pas);
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

	pas->dtb_mem_phys = pas->dtb_mem_reloc = res.start;
	pas->dtb_mem_size = resource_size(&res);
	pas->dtb_mem_region = devm_ioremap_resource_wc(pas->dev, &res);
	if (IS_ERR(pas->dtb_mem_region))
		return PTR_ERR(pas->dtb_mem_region);

	pas->dtb_firmware_name = pas->x1e->dtb_firmware_name;
	ret = of_property_read_string_index(np, "firmware-name", 1, &pas->dtb_firmware_name);
	if (ret < 0 && ret != -EINVAL)
		return ret;

	return 0;
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

	if (desc->x1e) {
		ret = x1e_probe(adsp);
		if (ret)
			goto disable_pm;
	} else {
		ret = adsp_init_reset(adsp);
		if (ret)
			goto disable_pm;

		ret = adsp_init_mmio(adsp, pdev);
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
