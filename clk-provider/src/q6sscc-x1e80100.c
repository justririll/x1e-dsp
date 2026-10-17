// SPDX-License-Identifier: GPL-2.0-only
/*
 * X1E80100 QDSP6SS (ADSP and CDSP) Q6 core clock controller, for Linux at EL2.
 *
 * The Q6 core PLL (Lucid OLE) and the core RCG sit inside the QDSP6 subsystem.
 * They lose their state on every subsystem reset, and on the CDSP they can only
 * be accessed once the boot FSM has powered the core up. So this provider never
 * reads the hardware on its own: it reprograms the PLL in .prepare, every time,
 * reports the rate the PLL is programmed for, and leaves turning it off to the
 * subsystem reset the remoteproc driver does when it stops the DSP.
 */

#include <linux/bitfield.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/qcom,x1e80100-q6sscc.h>

#define Q6SSCC_NR_CLKS		(Q6SSCC_Q6_CORE_CLK_SRC + 1)

#define PLL_MODE		0x00
#define PLL_OPMODE		0x04
#define PLL_L_VAL		0x10
#define PLL_ALPHA_VAL		0x14
#define PLL_USER_CTL		0x18
#define PLL_USER_CTL_U		0x1c
#define PLL_CONFIG_CTL		0x20
#define PLL_CONFIG_CTL_U	0x24
#define PLL_CONFIG_CTL_U1	0x28
#define PLL_TEST_CTL		0x2c
#define PLL_TEST_CTL_U		0x30
#define PLL_TEST_CTL_U1		0x34
#define PLL_TEST_CTL_U2		0x38

#define PLL_MODE_OUTCTRL	BIT(0)
#define PLL_MODE_RESET_N	BIT(2)
#define PLL_MODE_CAL_DONE	BIT(13)
#define PLL_MODE_CAL_REQ	BIT(14)
#define PLL_OPMODE_RUN		1
#define PLL_USER_CTL_OUT_MAIN	BIT(0)
#define PLL_USER_CTL_KEEP	0xee3c03ff
#define PLL_L_VAL_L		GENMASK(7, 0)
#define PLL_L_VAL_CAL_L		GENMASK(23, 16)

#define Q6_PLL_L		0x32
#define Q6_PLL_CAL_L		0x44

#define RCG_CMD			0x0
#define RCG_CFG			0x4
#define RCG_CMD_UPDATE		BIT(0)

struct q6sscc_desc {
	u32 core_rcg;		/* core RCG offset */
	u32 core_rcg_cfg;	/* PLL main output, divide by 1 */
};

struct q6sscc {
	void __iomem *base;
	const struct q6sscc_desc *desc;
	struct clk_hw pll;
	struct clk_hw core;
	struct clk_hw_onecell_data *hw_data;
	bool pll_on;
	bool core_on;
};

/*
 * QHEE's sequence. It differs from clk_lucid_ole_pll_configure(): there is a
 * calibration handshake first, only CAL_L is written in the upper L_VAL bits,
 * and only the main output is enabled.
 */
static int q6sscc_pll_prepare(struct clk_hw *hw)
{
	struct q6sscc *cc = container_of(hw, struct q6sscc, pll);
	void __iomem *base = cc->base;
	u32 val;
	int ret;

	writel((readl(base + PLL_L_VAL) & ~PLL_L_VAL_L) | Q6_PLL_L, base + PLL_L_VAL);
	writel(readl(base + PLL_USER_CTL) & PLL_USER_CTL_KEEP, base + PLL_USER_CTL);
	writel(0, base + PLL_ALPHA_VAL);
	writel(readl(base + PLL_MODE) | PLL_MODE_RESET_N, base + PLL_MODE);

	writel(readl(base + PLL_MODE) | PLL_MODE_CAL_REQ, base + PLL_MODE);
	ret = readl_poll_timeout(base + PLL_MODE, val, val & PLL_MODE_CAL_DONE, 1, 100000);
	if (ret)
		return ret;
	writel(readl(base + PLL_MODE) & ~PLL_MODE_CAL_REQ, base + PLL_MODE);
	ret = readl_poll_timeout(base + PLL_MODE, val, !(val & PLL_MODE_CAL_DONE), 1, 100000);
	if (ret)
		return ret;
	udelay(1);

	writel(0x20485699, base + PLL_CONFIG_CTL);
	writel(0x00182261, base + PLL_CONFIG_CTL_U);
	writel(0x82aa299c, base + PLL_CONFIG_CTL_U1);
	writel(0, base + PLL_TEST_CTL);
	writel(0x3, base + PLL_TEST_CTL_U);
	writel(0x9000, base + PLL_TEST_CTL_U1);
	writel(0x34, base + PLL_TEST_CTL_U2);
	writel(0, base + PLL_USER_CTL);
	writel(0x5, base + PLL_USER_CTL_U);
	val = readl(base + PLL_L_VAL) & ~PLL_L_VAL_CAL_L;
	writel(val | FIELD_PREP(PLL_L_VAL_CAL_L, Q6_PLL_CAL_L), base + PLL_L_VAL);

	return 0;
}

static int q6sscc_pll_enable(struct clk_hw *hw)
{
	struct q6sscc *cc = container_of(hw, struct q6sscc, pll);
	void __iomem *base = cc->base;

	writel(PLL_OPMODE_RUN, base + PLL_OPMODE);
	writel(readl(base + PLL_MODE) | PLL_MODE_RESET_N, base + PLL_MODE);
	writel(readl(base + PLL_MODE) | PLL_MODE_OUTCTRL, base + PLL_MODE);
	writel(readl(base + PLL_USER_CTL) | PLL_USER_CTL_OUT_MAIN, base + PLL_USER_CTL);
	cc->pll_on = true;

	return 0;
}

/* The subsystem reset on DSP stop turns the PLL off; the block may be unpowered by then */
static void q6sscc_pll_disable(struct clk_hw *hw)
{
	container_of(hw, struct q6sscc, pll)->pll_on = false;
}

static int q6sscc_pll_is_enabled(struct clk_hw *hw)
{
	return container_of(hw, struct q6sscc, pll)->pll_on;
}

static unsigned long q6sscc_pll_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	return parent_rate * Q6_PLL_L;
}

static const struct clk_ops q6sscc_pll_ops = {
	.prepare = q6sscc_pll_prepare,
	.enable = q6sscc_pll_enable,
	.disable = q6sscc_pll_disable,
	.is_enabled = q6sscc_pll_is_enabled,
	.recalc_rate = q6sscc_pll_recalc_rate,
};

/* Core RCG: switch to the PLL main output at divide by 1 */
static int q6sscc_core_enable(struct clk_hw *hw)
{
	struct q6sscc *cc = container_of(hw, struct q6sscc, core);
	void __iomem *rcg = cc->base + cc->desc->core_rcg;
	u32 val;
	int ret;

	writel(cc->desc->core_rcg_cfg, rcg + RCG_CFG);
	writel(readl(rcg + RCG_CMD) | RCG_CMD_UPDATE, rcg + RCG_CMD);
	ret = readl_poll_timeout_atomic(rcg + RCG_CMD, val, !(val & RCG_CMD_UPDATE), 1, 100000);
	if (!ret)
		cc->core_on = true;

	return ret;
}

static void q6sscc_core_disable(struct clk_hw *hw)
{
	container_of(hw, struct q6sscc, core)->core_on = false;
}

static int q6sscc_core_is_enabled(struct clk_hw *hw)
{
	return container_of(hw, struct q6sscc, core)->core_on;
}

static unsigned long q6sscc_core_recalc_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	return parent_rate;
}

static const struct clk_ops q6sscc_core_ops = {
	.enable = q6sscc_core_enable,
	.disable = q6sscc_core_disable,
	.is_enabled = q6sscc_core_is_enabled,
	.recalc_rate = q6sscc_core_recalc_rate,
};

static int q6sscc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_parent_data xo = { .index = 0 };
	struct clk_init_data init = {};
	const char *name;
	struct q6sscc *cc;
	int ret;

	cc = devm_kzalloc(dev, sizeof(*cc), GFP_KERNEL);
	if (!cc)
		return -ENOMEM;

	cc->desc = device_get_match_data(dev);
	cc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cc->base))
		return PTR_ERR(cc->base);

	cc->hw_data = devm_kzalloc(dev, struct_size(cc->hw_data, hws, Q6SSCC_NR_CLKS), GFP_KERNEL);
	if (!cc->hw_data)
		return -ENOMEM;
	cc->hw_data->num = Q6SSCC_NR_CLKS;

	/* Never gate them as unused: their registers may be unpowered */
	name = devm_kasprintf(dev, GFP_KERNEL, "%s_q6_pll", dev_name(dev));
	if (!name)
		return -ENOMEM;
	init.name = name;
	init.ops = &q6sscc_pll_ops;
	init.parent_data = &xo;
	init.num_parents = 1;
	init.flags = CLK_IGNORE_UNUSED;
	cc->pll.init = &init;
	ret = devm_clk_hw_register(dev, &cc->pll);
	if (ret)
		return ret;

	name = devm_kasprintf(dev, GFP_KERNEL, "%s_q6_core_clk_src", dev_name(dev));
	if (!name)
		return -ENOMEM;
	init.name = name;
	init.ops = &q6sscc_core_ops;
	init.parent_data = NULL;
	init.parent_hws = (const struct clk_hw *[]){ &cc->pll };
	init.flags = CLK_IGNORE_UNUSED | CLK_SET_RATE_PARENT;
	cc->core.init = &init;
	ret = devm_clk_hw_register(dev, &cc->core);
	if (ret)
		return ret;

	cc->hw_data->hws[Q6SSCC_Q6_PLL] = &cc->pll;
	cc->hw_data->hws[Q6SSCC_Q6_CORE_CLK_SRC] = &cc->core;

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, cc->hw_data);
}

static const struct q6sscc_desc adsp_q6sscc_desc = {
	.core_rcg = 0x8000,
	.core_rcg_cfg = 0x201,
};

static const struct q6sscc_desc cdsp_q6sscc_desc = {
	.core_rcg = 0x8020,
	.core_rcg_cfg = 0x100201,	/* same, under hardware clock control */
};

static const struct of_device_id q6sscc_match[] = {
	{ .compatible = "qcom,x1e80100-adsp-q6sscc", .data = &adsp_q6sscc_desc },
	{ .compatible = "qcom,x1e80100-cdsp-q6sscc", .data = &cdsp_q6sscc_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, q6sscc_match);

static struct platform_driver q6sscc_driver = {
	.probe = q6sscc_probe,
	.driver = {
		.name = "q6sscc-x1e80100",
		.of_match_table = q6sscc_match,
	},
};
module_platform_driver(q6sscc_driver);

MODULE_DESCRIPTION("Qualcomm X1E80100 QDSP6SS Q6 core clock controller");
MODULE_LICENSE("GPL");
