// SPDX-License-Identifier: GPL-2.0-only
/*
 * Allwinner sun55i (A523/A527/T527) DSI/LVDS combo D-PHY driver.
 *
 * Copyright (C) 2026 Midgy BALON
 *
 * Ported from the Allwinner AIOT BSP (sunxi-drm framework):
 *   drivers/drm/phy/sunxi_dsi_combophy.c
 *   drivers/drm/phy/sunxi_dsi_combophy_reg.{c,h}
 * (gitlab.com/tina5.0_aiot/lichee/bsp, branch product-aiot-stable).
 *
 * The A523 combo-PHY is the classic sun6i MIPI D-PHY front-end (gctl/tx_ctl/
 * tx_time/ana, 0x00-0x5c) plus an integrated display PLL ("DISPLL", 0x104+)
 * that generates the high-speed lane clock, and a combo LVDS/MIPI mux
 * (0x110+). Because the PLL is internal, this driver programs it directly from
 * the hs_clk_rate handed in via phy_configure() (the mainline sun6i_mipi_dsi
 * host already does phy_mipi_dphy_get_default_config()+phy_configure()), so no
 * separate clock provider is needed.
 *
 * MIPI-DSI mode only for now; LVDS is stubbed (the Trimui panel is 4-lane DSI).
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/phy/phy-mipi-dphy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

/* --- D-PHY front-end (same IP as sun6i a31/a100 dphy) --- */
#define SUN55I_DPHY_GCTL		0x00
#define SUN55I_DPHY_GCTL_LANE_NUM(n)		(((n) & 0x3) << 4)
#define SUN55I_DPHY_GCTL_MODULE_EN		BIT(0)

#define SUN55I_DPHY_TX_CTL		0x04
#define SUN55I_DPHY_TX_CTL_HS_CLK_CONT		BIT(26)

#define SUN55I_DPHY_TX_TIME0		0x10
#define SUN55I_DPHY_TX_TIME0_HS_TRAIL(n)	(((n) & 0xff) << 24)
#define SUN55I_DPHY_TX_TIME0_HS_PREPARE(n)	(((n) & 0xff) << 16)
#define SUN55I_DPHY_TX_TIME0_LP_CLK_DIV(n)	((n) & 0xff)

#define SUN55I_DPHY_TX_TIME1		0x14
#define SUN55I_DPHY_TX_TIME1_CK_POST(n)		(((n) & 0xff) << 24)
#define SUN55I_DPHY_TX_TIME1_CK_PRE(n)		(((n) & 0xff) << 16)
#define SUN55I_DPHY_TX_TIME1_CK_ZERO(n)		(((n) & 0xff) << 8)
#define SUN55I_DPHY_TX_TIME1_CK_PREPARE(n)	((n) & 0xff)

#define SUN55I_DPHY_TX_TIME2		0x18
#define SUN55I_DPHY_TX_TIME2_CK_TRAIL(n)	((n) & 0xff)

#define SUN55I_DPHY_TX_TIME3		0x1c
#define SUN55I_DPHY_TX_TIME4		0x20
#define SUN55I_DPHY_TX_TIME4_HS_TX_ANA1(n)	(((n) & 0xff) << 8)
#define SUN55I_DPHY_TX_TIME4_HS_TX_ANA0(n)	((n) & 0xff)

#define SUN55I_DPHY_ANA0		0x4c
#define SUN55I_DPHY_ANA1		0x50
#define SUN55I_DPHY_ANA1_VTTMODE		BIT(31)
#define SUN55I_DPHY_ANA2		0x54
#define SUN55I_DPHY_ANA2_ENIB			BIT(1)
#define SUN55I_DPHY_ANA2_ENCK_CPU		BIT(4)
#define SUN55I_DPHY_ANA2_ENP2S_CPU(n)		(((n) & 0xf) << 24)
#define SUN55I_DPHY_ANA3		0x58
#define SUN55I_DPHY_ANA3_ENVTTD(n)		(((u32)(n) & 0xf) << 28)
#define SUN55I_DPHY_ANA3_ENVTTC			BIT(27)
#define SUN55I_DPHY_ANA3_ENDIV			BIT(26)
#define SUN55I_DPHY_ANA3_ENLDOD			BIT(24)
#define SUN55I_DPHY_ANA3_ENLDOC			BIT(25)
#define SUN55I_DPHY_ANA3_ENLDOR			BIT(18)	/* was BIT(15): a reserved bit -> LDO never on -> DISPLL dead */
#define SUN55I_DPHY_ANA4		0x5c

#define SUN55I_DPHY_DBG0		0xe0
#define SUN55I_DPHY_DBG0_PLL_LOCK		BIT(26)

/* --- integrated DISPLL PLL --- */
#define SUN55I_DPHY_PLL_REG0		0x104
#define SUN55I_DPHY_PLL_REG0_UPDATE		BIT(31)
#define SUN55I_DPHY_PLL_REG0_M2(n)		(((n) & 0x3) << 28)
#define SUN55I_DPHY_PLL_REG0_M3(n)		(((n) & 0xf) << 24)
#define SUN55I_DPHY_PLL_REG0_LDO_EN		BIT(22)
#define SUN55I_DPHY_PLL_REG0_EN_LVS		BIT(21)
#define SUN55I_DPHY_PLL_REG0_PLL_EN		BIT(20)
#define SUN55I_DPHY_PLL_REG0_P(n)		(((n) & 0xf) << 16)
#define SUN55I_DPHY_PLL_REG0_N(n)		(((n) & 0xff) << 8)
#define SUN55I_DPHY_PLL_REG0_M0(n)		(((n) & 0x3) << 4)
#define SUN55I_DPHY_PLL_REG0_M1(n)		((n) & 0xf)
#define SUN55I_DPHY_PLL_REG0_M_MASK		(GENMASK(5, 0) | GENMASK(29, 24))
#define SUN55I_DPHY_PLL_REG0_NP_MASK		GENMASK(19, 8)

#define SUN55I_DPHY_PLL_REG1		0x108
#define SUN55I_DPHY_PLL_REG1_HS_GATING		BIT(22)
#define SUN55I_DPHY_PLL_REG1_LS_GATING		BIT(21)
#define SUN55I_DPHY_PLL_REG1_LOCKDET_EN		BIT(12)

#define SUN55I_DPHY_PLL_REG2		0x10c

/* --- combo LVDS/MIPI mux --- */
#define SUN55I_COMBO_PHY_REG0		0x110
#define SUN55I_COMBO_PHY_REG1		0x114
#define SUN55I_COMBO_PHY_REG2		0x118
#define SUN55I_COMBO_PHY_REG2_HS_STOP_DLY(n)	((n) & 0xff)

#define SUN55I_DPHY_REF_CLK		24000000UL

/*
 * Analog power-up values for MIPI-DSI mode, decoded field-by-field from the BSP
 * dsi_dphy_open() (de_dsi.c) against dphy_ana{0,4}_reg_t. The earlier port took
 * these from the BSP phy_config[] *table*, whose ANA0/ANA4 layout does NOT match
 * this SoC (it even mislabelled ANA0 as lptx_setr/setc) -> wrong trim/LDO bias,
 * which together with the ENLDOR misdefine above left the DISPLL analog dead.
 *   ana0: reg_plr=4 [7:4], reg_sfb=1 [3:2]                          -> 0x44
 *   ana4: txpusd=2 txpusc=3 txdnsd=2 txdnsc=3 tmsd=1 tmsc=1 ckdv=3
 *         vtt_set=3 dmplvd=4 ib=2 en_mipi=1                         -> 0x844635ee
 *   combo0: en_cp | en_comboldo | en_mipi                           -> 0x0b (correct)
 */
#define SUN55I_DPHY_CFG_TX_TIME0	0x0406000e
#define SUN55I_DPHY_CFG_ANA0		0x00000044
#define SUN55I_DPHY_CFG_ANA4		0x844635ee
#define SUN55I_DPHY_CFG_COMBO0		0x0000000b
#define SUN55I_DPHY_CFG_COMBO1		0x00000043

struct sun55i_dphy {
	void __iomem		*base;
	struct clk		*bus_clk;	/* APB register clock (CLK_BUS_MIPI_DSI1) */
	struct clk		*mod_clk;	/* PHY functional clock (CLK_COMBOPHY_DSI1) */
	struct reset_control	*reset;
	struct phy		*phy;
	unsigned long		hs_clk_rate;
	unsigned int		lanes;
};

static void sun55i_dphy_update(struct sun55i_dphy *dphy, u32 reg,
			       u32 mask, u32 val)
{
	u32 tmp = readl(dphy->base + reg);

	tmp &= ~mask;
	tmp |= val & mask;
	writel(tmp, dphy->base + reg);
}

/*
 * Program the integrated DISPLL for the requested per-lane bit rate.
 * Single-link MIPI-DSI dividers, ported from sunxi_dsi_comb_dphy_pll_set():
 *   clk_hs = 24MHz * n / (p+1) / (m0+1) / (m1+1)
 */
static void sun55i_dphy_pll_set(struct sun55i_dphy *dphy, unsigned long hs_rate)
{
	u32 div_p, div_m0, n, reg;
	u64 vco = hs_rate;

	/*
	 * Match the BSP dsi_comb_dphy_pll_set() exactly: the DISPLL VCO must
	 * run high (24 MHz * n, n ~= 124 for 372 Mbps) and is then divided by
	 * (p+1)*(m0+1). m1 is the LP-clock divider (constant 2) and does NOT
	 * affect the HS rate. The earlier port halved every VCO multiplier and
	 * forced p=0, which parked the VCO below the PLL lock range (dbg0 reads
	 * all-zero, pll_enable returns -110). M2/M3 are left at reset (the BSP
	 * pll_set never touches them).
	 */
	if (hs_rate <= 264000000) {
		vco *= 16; div_p = 7; div_m0 = 1;
	} else if (hs_rate <= 536000000) {
		vco *= 8;  div_p = 7; div_m0 = 0;
	} else if (hs_rate <= 1072000000) {
		vco *= 4;  div_p = 3; div_m0 = 0;
	} else if (hs_rate <= 2144000000UL) {
		vco *= 2;  div_p = 1; div_m0 = 0;
	} else {
		div_p = 0; div_m0 = 0;
	}
	n = div_u64(vco, SUN55I_DPHY_REF_CLK);

	reg = readl(dphy->base + SUN55I_DPHY_PLL_REG0);
	reg &= ~(SUN55I_DPHY_PLL_REG0_N(0xff) | SUN55I_DPHY_PLL_REG0_P(0xf) |
		 SUN55I_DPHY_PLL_REG0_M0(0x3) | SUN55I_DPHY_PLL_REG0_M1(0xf));
	reg |= SUN55I_DPHY_PLL_REG0_N(n) | SUN55I_DPHY_PLL_REG0_P(div_p) |
	       SUN55I_DPHY_PLL_REG0_M0(div_m0) | SUN55I_DPHY_PLL_REG0_M1(2);
	writel(reg, dphy->base + SUN55I_DPHY_PLL_REG0);

	writel(0, dphy->base + SUN55I_DPHY_PLL_REG2);	/* disable SDM */

	sun55i_dphy_update(dphy, SUN55I_DPHY_PLL_REG0,
			   SUN55I_DPHY_PLL_REG0_PLL_EN |
			   SUN55I_DPHY_PLL_REG0_LDO_EN,
			   SUN55I_DPHY_PLL_REG0_PLL_EN |
			   SUN55I_DPHY_PLL_REG0_LDO_EN);
	sun55i_dphy_update(dphy, SUN55I_DPHY_PLL_REG1,
			   SUN55I_DPHY_PLL_REG1_LOCKDET_EN,
			   SUN55I_DPHY_PLL_REG1_LOCKDET_EN);
	sun55i_dphy_update(dphy, SUN55I_DPHY_PLL_REG0,
			   SUN55I_DPHY_PLL_REG0_UPDATE,
			   SUN55I_DPHY_PLL_REG0_UPDATE);
}

static int sun55i_dphy_pll_enable(struct sun55i_dphy *dphy)
{
	u32 val;

	sun55i_dphy_update(dphy, SUN55I_DPHY_PLL_REG0,
			   SUN55I_DPHY_PLL_REG0_LDO_EN |
			   SUN55I_DPHY_PLL_REG0_PLL_EN |
			   SUN55I_DPHY_PLL_REG0_EN_LVS,
			   SUN55I_DPHY_PLL_REG0_LDO_EN |
			   SUN55I_DPHY_PLL_REG0_PLL_EN |
			   SUN55I_DPHY_PLL_REG0_EN_LVS);
	sun55i_dphy_update(dphy, SUN55I_DPHY_PLL_REG1,
			   SUN55I_DPHY_PLL_REG1_HS_GATING |
			   SUN55I_DPHY_PLL_REG1_LS_GATING |
			   SUN55I_DPHY_PLL_REG1_LOCKDET_EN,
			   SUN55I_DPHY_PLL_REG1_HS_GATING |
			   SUN55I_DPHY_PLL_REG1_LS_GATING |
			   SUN55I_DPHY_PLL_REG1_LOCKDET_EN);
	sun55i_dphy_update(dphy, SUN55I_DPHY_PLL_REG0,
			   SUN55I_DPHY_PLL_REG0_UPDATE,
			   SUN55I_DPHY_PLL_REG0_UPDATE);

	return readl_poll_timeout(dphy->base + SUN55I_DPHY_DBG0, val,
				  val & SUN55I_DPHY_DBG0_PLL_LOCK, 5, 200000);
}

static void sun55i_dphy_pll_disable(struct sun55i_dphy *dphy)
{
	sun55i_dphy_update(dphy, SUN55I_DPHY_PLL_REG0,
			   SUN55I_DPHY_PLL_REG0_LDO_EN |
			   SUN55I_DPHY_PLL_REG0_PLL_EN |
			   SUN55I_DPHY_PLL_REG0_EN_LVS |
			   SUN55I_DPHY_PLL_REG0_UPDATE, 0);
	sun55i_dphy_update(dphy, SUN55I_DPHY_PLL_REG1,
			   SUN55I_DPHY_PLL_REG1_HS_GATING |
			   SUN55I_DPHY_PLL_REG1_LS_GATING |
			   SUN55I_DPHY_PLL_REG1_LOCKDET_EN, 0);
}

/* D-PHY HS clock/data lane timing (ported from sunxi_dsi_dphy_cfg) */
static void sun55i_dphy_timing(struct sun55i_dphy *dphy)
{
	sun55i_dphy_update(dphy, SUN55I_DPHY_TX_CTL,
			   SUN55I_DPHY_TX_CTL_HS_CLK_CONT,
			   SUN55I_DPHY_TX_CTL_HS_CLK_CONT);

	writel(SUN55I_DPHY_TX_TIME1_CK_PREPARE(7) |
	       SUN55I_DPHY_TX_TIME1_CK_ZERO(50) |
	       SUN55I_DPHY_TX_TIME1_CK_PRE(3) |
	       SUN55I_DPHY_TX_TIME1_CK_POST(10),
	       dphy->base + SUN55I_DPHY_TX_TIME1);
	writel(SUN55I_DPHY_TX_TIME2_CK_TRAIL(30),
	       dphy->base + SUN55I_DPHY_TX_TIME2);
	writel(0, dphy->base + SUN55I_DPHY_TX_TIME3);
	writel(SUN55I_DPHY_TX_TIME4_HS_TX_ANA0(3) |
	       SUN55I_DPHY_TX_TIME4_HS_TX_ANA1(3),
	       dphy->base + SUN55I_DPHY_TX_TIME4);
}

static void sun55i_dphy_lane_set(struct sun55i_dphy *dphy)
{
	u32 lane_den = GENMASK(dphy->lanes - 1, 0);

	sun55i_dphy_update(dphy, SUN55I_DPHY_ANA3, SUN55I_DPHY_ANA3_ENVTTD(0xf),
			   SUN55I_DPHY_ANA3_ENVTTD(lane_den));
	sun55i_dphy_update(dphy, SUN55I_DPHY_ANA2, SUN55I_DPHY_ANA2_ENP2S_CPU(0xf),
			   SUN55I_DPHY_ANA2_ENP2S_CPU(lane_den));
	sun55i_dphy_update(dphy, SUN55I_DPHY_GCTL, SUN55I_DPHY_GCTL_LANE_NUM(3),
			   SUN55I_DPHY_GCTL_LANE_NUM(dphy->lanes - 1));
}

/* Analog power-up sequence (ported from sunxi_dsi_io_open) */
static void sun55i_dphy_analog_on(struct sun55i_dphy *dphy)
{
	writel(SUN55I_DPHY_CFG_TX_TIME0, dphy->base + SUN55I_DPHY_TX_TIME0);
	writel(SUN55I_DPHY_CFG_ANA4, dphy->base + SUN55I_DPHY_ANA4);

	sun55i_dphy_update(dphy, SUN55I_DPHY_ANA2,
			   SUN55I_DPHY_ANA2_ENCK_CPU | SUN55I_DPHY_ANA2_ENIB,
			   SUN55I_DPHY_ANA2_ENCK_CPU | SUN55I_DPHY_ANA2_ENIB);
	sun55i_dphy_update(dphy, SUN55I_DPHY_ANA3,
			   SUN55I_DPHY_ANA3_ENLDOR | SUN55I_DPHY_ANA3_ENLDOC |
			   SUN55I_DPHY_ANA3_ENLDOD,
			   SUN55I_DPHY_ANA3_ENLDOR | SUN55I_DPHY_ANA3_ENLDOC |
			   SUN55I_DPHY_ANA3_ENLDOD);
	writel(SUN55I_DPHY_CFG_ANA0, dphy->base + SUN55I_DPHY_ANA0);
	writel(SUN55I_DPHY_CFG_COMBO0, dphy->base + SUN55I_COMBO_PHY_REG0);
	writel(SUN55I_COMBO_PHY_REG2_HS_STOP_DLY(20),
	       dphy->base + SUN55I_COMBO_PHY_REG2);
	udelay(1);

	sun55i_dphy_update(dphy, SUN55I_DPHY_ANA3,
			   SUN55I_DPHY_ANA3_ENVTTC | SUN55I_DPHY_ANA3_ENDIV,
			   SUN55I_DPHY_ANA3_ENVTTC | SUN55I_DPHY_ANA3_ENDIV);
	sun55i_dphy_update(dphy, SUN55I_DPHY_ANA1, SUN55I_DPHY_ANA1_VTTMODE,
			   SUN55I_DPHY_ANA1_VTTMODE);
	sun55i_dphy_update(dphy, SUN55I_DPHY_GCTL, SUN55I_DPHY_GCTL_MODULE_EN,
			   SUN55I_DPHY_GCTL_MODULE_EN);
}

static void sun55i_dphy_analog_off(struct sun55i_dphy *dphy)
{
	writel(0, dphy->base + SUN55I_DPHY_TX_TIME0);
	sun55i_dphy_update(dphy, SUN55I_DPHY_ANA2, SUN55I_DPHY_ANA2_ENP2S_CPU(0xf), 0);
	sun55i_dphy_update(dphy, SUN55I_DPHY_ANA1, SUN55I_DPHY_ANA1_VTTMODE, 0);
	sun55i_dphy_update(dphy, SUN55I_DPHY_ANA2, SUN55I_DPHY_ANA2_ENCK_CPU, 0);
	sun55i_dphy_update(dphy, SUN55I_DPHY_ANA3,
			   SUN55I_DPHY_ANA3_ENDIV | SUN55I_DPHY_ANA3_ENVTTD(0xf) |
			   SUN55I_DPHY_ANA3_ENVTTC | SUN55I_DPHY_ANA3_ENLDOD |
			   SUN55I_DPHY_ANA3_ENLDOC | SUN55I_DPHY_ANA3_ENLDOR, 0);
	sun55i_dphy_update(dphy, SUN55I_DPHY_ANA2, SUN55I_DPHY_ANA2_ENIB, 0);
	writel(0, dphy->base + SUN55I_DPHY_ANA4);
}

static int sun55i_dphy_init(struct phy *phy)
{
	struct sun55i_dphy *dphy = phy_get_drvdata(phy);
	int ret;

	ret = reset_control_deassert(dphy->reset);
	if (ret)
		return ret;

	ret = clk_prepare_enable(dphy->bus_clk);
	if (ret) {
		reset_control_assert(dphy->reset);
		return ret;
	}

	ret = clk_prepare_enable(dphy->mod_clk);
	if (ret) {
		clk_disable_unprepare(dphy->bus_clk);
		reset_control_assert(dphy->reset);
	}

	return ret;
}

static int sun55i_dphy_exit(struct phy *phy)
{
	struct sun55i_dphy *dphy = phy_get_drvdata(phy);

	clk_disable_unprepare(dphy->mod_clk);
	clk_disable_unprepare(dphy->bus_clk);
	reset_control_assert(dphy->reset);

	return 0;
}

static int sun55i_dphy_configure(struct phy *phy,
				 union phy_configure_opts *opts)
{
	struct sun55i_dphy *dphy = phy_get_drvdata(phy);
	int ret;

	ret = phy_mipi_dphy_config_validate(&opts->mipi_dphy);
	if (ret)
		return ret;

	dphy->hs_clk_rate = opts->mipi_dphy.hs_clk_rate;
	dphy->lanes = opts->mipi_dphy.lanes;

	sun55i_dphy_pll_set(dphy, dphy->hs_clk_rate);
	sun55i_dphy_timing(dphy);
	sun55i_dphy_lane_set(dphy);

	return 0;
}

static int sun55i_dphy_power_on(struct phy *phy)
{
	struct sun55i_dphy *dphy = phy_get_drvdata(phy);

	sun55i_dphy_analog_on(dphy);

	return sun55i_dphy_pll_enable(dphy);
}

static int sun55i_dphy_power_off(struct phy *phy)
{
	struct sun55i_dphy *dphy = phy_get_drvdata(phy);

	sun55i_dphy_pll_disable(dphy);
	sun55i_dphy_analog_off(dphy);

	return 0;
}

static int sun55i_dphy_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	if (mode != PHY_MODE_MIPI_DPHY)
		return -EINVAL;	/* LVDS not yet supported */

	return 0;
}

static const struct phy_ops sun55i_dphy_ops = {
	.init		= sun55i_dphy_init,
	.exit		= sun55i_dphy_exit,
	.configure	= sun55i_dphy_configure,
	.power_on	= sun55i_dphy_power_on,
	.power_off	= sun55i_dphy_power_off,
	.set_mode	= sun55i_dphy_set_mode,
	.owner		= THIS_MODULE,
};

static int sun55i_dphy_probe(struct platform_device *pdev)
{
	struct phy_provider *phy_provider;
	struct sun55i_dphy *dphy;

	dphy = devm_kzalloc(&pdev->dev, sizeof(*dphy), GFP_KERNEL);
	if (!dphy)
		return -ENOMEM;

	dphy->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dphy->base))
		return PTR_ERR(dphy->base);

	dphy->bus_clk = devm_clk_get(&pdev->dev, "bus");
	if (IS_ERR(dphy->bus_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(dphy->bus_clk),
				     "failed to get bus clock\n");

	dphy->mod_clk = devm_clk_get_optional(&pdev->dev, "mod");
	if (IS_ERR(dphy->mod_clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(dphy->mod_clk),
				     "failed to get mod clock\n");

	dphy->reset = devm_reset_control_get_shared(&pdev->dev, NULL);
	if (IS_ERR(dphy->reset))
		return dev_err_probe(&pdev->dev, PTR_ERR(dphy->reset),
				     "failed to get reset control\n");

	dphy->phy = devm_phy_create(&pdev->dev, NULL, &sun55i_dphy_ops);
	if (IS_ERR(dphy->phy))
		return dev_err_probe(&pdev->dev, PTR_ERR(dphy->phy),
				     "failed to create PHY\n");

	phy_set_drvdata(dphy->phy, dphy);
	phy_provider = devm_of_phy_provider_register(&pdev->dev,
						     of_phy_simple_xlate);

	return PTR_ERR_OR_ZERO(phy_provider);
}

static const struct of_device_id sun55i_dphy_of_table[] = {
	{ .compatible = "allwinner,sun55i-a523-dsi-combo-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, sun55i_dphy_of_table);

static struct platform_driver sun55i_dphy_platform_driver = {
	.probe		= sun55i_dphy_probe,
	.driver		= {
		.name		= "sun55i-dsi-combo-phy",
		.of_match_table	= sun55i_dphy_of_table,
	},
};
module_platform_driver(sun55i_dphy_platform_driver);

MODULE_AUTHOR("Midgy BALON");
MODULE_DESCRIPTION("Allwinner sun55i DSI/LVDS combo D-PHY driver");
MODULE_LICENSE("GPL");
