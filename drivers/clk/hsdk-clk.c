/*
 * Synopsys HSDK SDP Generic PLL clock driver
 *
 * Copyright (C) 2017 Synopsys
 * Author: Eugeniy Paltsev <Eugeniy.Paltsev@synopsys.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <common.h>
#include <clk-uclass.h>
#include <div64.h>
#include <dm.h>
#include <linux/io.h>

#define CGU_PLL_CTRL	0x000 /* ARC PLL control register */
#define CGU_PLL_STATUS	0x004 /* ARC PLL status register */
#define CGU_PLL_FMEAS	0x008 /* ARC PLL frequency measurement register */
#define CGU_PLL_MON	0x00C /* ARC PLL monitor register */

#define CGU_PLL_CTRL_ODIV_SHIFT		2
#define CGU_PLL_CTRL_IDIV_SHIFT		4
#define CGU_PLL_CTRL_FBDIV_SHIFT	9
#define CGU_PLL_CTRL_BAND_SHIFT		20

#define CGU_PLL_CTRL_ODIV_MASK		GENMASK(3, CGU_PLL_CTRL_ODIV_SHIFT)
#define CGU_PLL_CTRL_IDIV_MASK		GENMASK(8, CGU_PLL_CTRL_IDIV_SHIFT)
#define CGU_PLL_CTRL_FBDIV_MASK		GENMASK(15, CGU_PLL_CTRL_FBDIV_SHIFT)

#define CGU_PLL_CTRL_PD			BIT(0)
#define CGU_PLL_CTRL_BYPASS		BIT(1)

#define CGU_PLL_STATUS_LOCK		BIT(0)
#define CGU_PLL_STATUS_ERR		BIT(1)

#define HSDK_PLL_MAX_LOCK_TIME		100 /* 100 us */

#define CREG_CORE_IF_DIV		0x000 /* ARC CORE interface divider */
#define CORE_IF_CLK_THRESHOLD_HZ	500000000
#define CREG_CORE_IF_CLK_DIV_1		0x0
#define CREG_CORE_IF_CLK_DIV_2		0x1

#define PARENT_RATE			33333333 /* fixed clock - xtal */

struct hsdk_pll_cfg {
	u32 rate;
	u32 idiv;
	u32 fbdiv;
	u32 odiv;
	u32 band;
};

static const struct hsdk_pll_cfg asdt_pll_cfg[] = {
	{ 100000000,  0, 11, 3, 0 },
	{ 133000000,  0, 15, 3, 0 },
	{ 200000000,  1, 47, 3, 0 },
	{ 233000000,  1, 27, 2, 0 },
	{ 300000000,  1, 35, 2, 0 },
	{ 333000000,  1, 39, 2, 0 },
	{ 400000000,  1, 47, 2, 0 },
	{ 500000000,  0, 14, 1, 0 },
	{ 600000000,  0, 17, 1, 0 },
	{ 700000000,  0, 20, 1, 0 },
	{ 800000000,  0, 23, 1, 0 },
	{ 900000000,  1, 26, 0, 0 },
	{ 1000000000, 1, 29, 0, 0 },
	{ 1100000000, 1, 32, 0, 0 },
	{ 1200000000, 1, 35, 0, 0 },
	{ 1300000000, 1, 38, 0, 0 },
	{ 1400000000, 1, 41, 0, 0 },
	{ 1500000000, 1, 44, 0, 0 },
	{ 1600000000, 1, 47, 0, 0 },
	{}
};

static const struct hsdk_pll_cfg hdmi_pll_cfg[] = {
	{ 297000000,  0, 21, 2, 0 },
	{ 540000000,  0, 19, 1, 0 },
	{ 594000000,  0, 21, 1, 0 },
	{}
};

struct hsdk_pll_clk {
	void __iomem *regs;
	void __iomem *spec_regs;
	const struct hsdk_pll_devdata *pll_devdata;
};

struct hsdk_pll_devdata {
	const struct hsdk_pll_cfg *pll_cfg;
	int (*update_rate)(struct hsdk_pll_clk *clk, unsigned long rate,
			   const struct hsdk_pll_cfg *cfg);
};

static int hsdk_pll_core_update_rate(struct hsdk_pll_clk *, unsigned long,
				     const struct hsdk_pll_cfg *);
static int hsdk_pll_comm_update_rate(struct hsdk_pll_clk *, unsigned long,
				     const struct hsdk_pll_cfg *);

static const struct hsdk_pll_devdata core_pll_devdata = {
	.pll_cfg = asdt_pll_cfg,
	.update_rate = hsdk_pll_core_update_rate,
};

static const struct hsdk_pll_devdata sdt_pll_devdata = {
	.pll_cfg = asdt_pll_cfg,
	.update_rate = hsdk_pll_comm_update_rate,
};

static const struct hsdk_pll_devdata hdmi_pll_devdata = {
	.pll_cfg = hdmi_pll_cfg,
	.update_rate = hsdk_pll_comm_update_rate,
};

static inline void hsdk_pll_write(struct hsdk_pll_clk *clk, u32 reg, u32 val)
{
	iowrite32(val, clk->regs + reg);
}

static inline u32 hsdk_pll_read(struct hsdk_pll_clk *clk, u32 reg)
{
	return ioread32(clk->regs + reg);
}

static inline void hsdk_pll_spcwrite(struct hsdk_pll_clk *clk, u32 reg, u32 val)
{
	iowrite32(val, clk->spec_regs + reg);
}

static inline u32 hsdk_pll_spcread(struct hsdk_pll_clk *clk, u32 reg)
{
	return ioread32(clk->spec_regs + reg);
}

static inline void hsdk_pll_set_cfg(struct hsdk_pll_clk *clk,
				    const struct hsdk_pll_cfg *cfg)
{
	u32 val = 0;

	/* Powerdown and Bypass bits should be cleared */
	val |= cfg->idiv << CGU_PLL_CTRL_IDIV_SHIFT;
	val |= cfg->fbdiv << CGU_PLL_CTRL_FBDIV_SHIFT;
	val |= cfg->odiv << CGU_PLL_CTRL_ODIV_SHIFT;
	val |= cfg->band << CGU_PLL_CTRL_BAND_SHIFT;

	pr_debug("write configurarion: %#x\n", val);

	hsdk_pll_write(clk, CGU_PLL_CTRL, val);
}

static inline bool hsdk_pll_is_locked(struct hsdk_pll_clk *clk)
{
	return !!(hsdk_pll_read(clk, CGU_PLL_STATUS) & CGU_PLL_STATUS_LOCK);
}

static inline bool hsdk_pll_is_err(struct hsdk_pll_clk *clk)
{
	return !!(hsdk_pll_read(clk, CGU_PLL_STATUS) & CGU_PLL_STATUS_ERR);
}

static ulong hsdk_pll_get_rate(struct clk *sclk)
{
	u32 val;
	u64 rate;
	u32 idiv, fbdiv, odiv;
	struct hsdk_pll_clk *clk = dev_get_priv(sclk->dev);

	val = hsdk_pll_read(clk, CGU_PLL_CTRL);

	pr_debug("current configurarion: %#x\n", val);

	/* Check if PLL is disabled */
	if (val & CGU_PLL_CTRL_PD)
		return 0;

	/* Check if PLL is bypassed */
	if (val & CGU_PLL_CTRL_BYPASS)
		return PARENT_RATE;

	/* input divider = reg.idiv + 1 */
	idiv = 1 + ((val & CGU_PLL_CTRL_IDIV_MASK) >> CGU_PLL_CTRL_IDIV_SHIFT);
	/* fb divider = 2*(reg.fbdiv + 1) */
	fbdiv = 2 * (1 + ((val & CGU_PLL_CTRL_FBDIV_MASK) >> CGU_PLL_CTRL_FBDIV_SHIFT));
	/* output divider = 2^(reg.odiv) */
	odiv = 1 << ((val & CGU_PLL_CTRL_ODIV_MASK) >> CGU_PLL_CTRL_ODIV_SHIFT);

	rate = (u64)PARENT_RATE * fbdiv;
	do_div(rate, idiv * odiv);

	return rate;
}

static unsigned long hsdk_pll_round_rate(struct clk *sclk, unsigned long rate)
{
	int i;
	unsigned long best_rate;
	struct hsdk_pll_clk *clk = dev_get_priv(sclk->dev);
	const struct hsdk_pll_cfg *pll_cfg = clk->pll_devdata->pll_cfg;

	if (pll_cfg[0].rate == 0)
		return -EINVAL;

	best_rate = pll_cfg[0].rate;

	for (i = 1; pll_cfg[i].rate != 0; i++) {
		if (abs(rate - pll_cfg[i].rate) < abs(rate - best_rate))
			best_rate = pll_cfg[i].rate;
	}

	pr_debug("chosen best rate: %lu\n", best_rate);

	return best_rate;
}

static int hsdk_pll_comm_update_rate(struct hsdk_pll_clk *clk,
				     unsigned long rate,
				     const struct hsdk_pll_cfg *cfg)
{
	hsdk_pll_set_cfg(clk, cfg);

	/*
	 * Wait until CGU relocks and check error status.
	 * If after timeout CGU is unlocked yet return error.
	 */
	udelay(HSDK_PLL_MAX_LOCK_TIME);
	if (!hsdk_pll_is_locked(clk))
		return -ETIMEDOUT;

	if (hsdk_pll_is_err(clk))
		return -EINVAL;

	return 0;
}

static int hsdk_pll_core_update_rate(struct hsdk_pll_clk *clk,
				     unsigned long rate,
				     const struct hsdk_pll_cfg *cfg)
{
	/*
	 * When core clock exceeds 500MHz, the divider for the interface
	 * clock must be programmed to div-by-2.
	 */
	if (rate > CORE_IF_CLK_THRESHOLD_HZ)
		hsdk_pll_spcwrite(clk, CREG_CORE_IF_DIV, CREG_CORE_IF_CLK_DIV_2);

	hsdk_pll_set_cfg(clk, cfg);

	/*
	 * Wait until CGU relocks and check error status.
	 * If after timeout CGU is unlocked yet return error.
	 */
	udelay(HSDK_PLL_MAX_LOCK_TIME);
	if (!hsdk_pll_is_locked(clk))
		return -ETIMEDOUT;

	if (hsdk_pll_is_err(clk))
		return -EINVAL;

	/*
	 * Program divider to div-by-1 if we succesfuly set core clock below
	 * 500MHz threshold.
	 */
	if (rate <= CORE_IF_CLK_THRESHOLD_HZ)
		hsdk_pll_spcwrite(clk, CREG_CORE_IF_DIV, CREG_CORE_IF_CLK_DIV_1);

	return 0;
}

static ulong hsdk_pll_set_rate(struct clk *sclk, ulong rate)
{
	int i;
	unsigned long best_rate;
	struct hsdk_pll_clk *clk = dev_get_priv(sclk->dev);
	const struct hsdk_pll_cfg *pll_cfg = clk->pll_devdata->pll_cfg;

	best_rate = hsdk_pll_round_rate(sclk, rate);

	for (i = 0; pll_cfg[i].rate != 0; i++) {
		if (pll_cfg[i].rate == best_rate) {
			return clk->pll_devdata->update_rate(clk, best_rate,
							     &pll_cfg[i]);
		}
	}

	pr_err("invalid rate=%ld, parent_rate=%d\n", best_rate, PARENT_RATE);

	return -EINVAL;
}

static const struct clk_ops hsdk_pll_ops = {
	.set_rate = hsdk_pll_set_rate,
	.get_rate = hsdk_pll_get_rate,
};

static int hsdk_pll_clk_probe(struct udevice *dev)
{
	struct hsdk_pll_clk *pll_clk = dev_get_priv(dev);

	pll_clk->pll_devdata = (const struct hsdk_pll_devdata *)
					dev_get_driver_data(dev);

	pll_clk->regs = (void __iomem *)devfdt_get_addr_index(dev, 0);
	pll_clk->spec_regs = (void __iomem *)devfdt_get_addr_index(dev, 1);
	if ((pll_clk->pll_devdata == &core_pll_devdata) && !pll_clk->spec_regs)
		return -ENOENT;

	return 0;
}

static const struct udevice_id hsdk_pll_clk_id[] = {
	{ .compatible = "snps,hsdk-gp-pll-clock", .data = (ulong)&sdt_pll_devdata},
	{ .compatible = "snps,hsdk-hdmi-pll-clock", .data = (ulong)&hdmi_pll_devdata},
	{ .compatible = "snps,hsdk-core-pll-clock", .data = (ulong)&core_pll_devdata},
	{ }
};

U_BOOT_DRIVER(hsdk_pll_clk) = {
	.name = "hsdk-pll-clk",
	.id = UCLASS_CLK,
	.of_match = hsdk_pll_clk_id,
	.probe = hsdk_pll_clk_probe,
	.platdata_auto_alloc_size = sizeof(struct hsdk_pll_clk),
	.ops = &hsdk_pll_ops,
};