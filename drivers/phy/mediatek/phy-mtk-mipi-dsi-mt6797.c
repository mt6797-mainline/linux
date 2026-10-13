// SPDX-License-Identifier: GPL-2.0-only
/* MT6797 MIPI TX: S2Q/POSDIV layout, PCW-change strobe and retained pad trim. */
#include "phy-mtk-io.h"
#include "phy-mtk-mipi-dsi.h"

#define TX_CORE 0x00
#define TX_CLOCK_LANE 0x04
#define TX_TOP 0x40
#define TX_BG 0x44
#define TX_PLL0 0x50
#define TX_PLL1 0x54
#define TX_PCW 0x58
#define TX_PCW_CHANGE 0x60
#define TX_POWER 0x68

#define PLL_ENABLE BIT(0)
#define PLL_PREDIV GENMASK(3, 2)
#define PLL_POSDIV GENMASK(6, 4)
#define PLL_S2QDIV GENMASK(13, 12)
#define PLL_POWER BIT(0)
#define PLL_ISOLATE BIT(1)
#define PAD_TIE_LOW BIT(11)

static int mt6797_mipi_pll_prepare(struct clk_hw *hw)
{
	struct mtk_mipi_tx *tx = mtk_mipi_tx_from_clk_hw(hw);
	void __iomem *base = tx->regs;
	u32 postdiv = 0;
	u64 pcw;

	if (tx->data_rate < 50000000 || tx->data_rate > 1250000000)
		return -EINVAL;
	while ((u64)tx->data_rate << postdiv < 500000000 && postdiv < 4)
		postdiv++;

	/* Leave firmware bandgap voltage selection and impedance trim intact. */
	mtk_phy_set_bits(base + TX_BG, BIT(0));
	usleep_range(1000, 1500);
	mtk_phy_set_bits(base + TX_CORE, BIT(0) | BIT(1));
	mtk_phy_update_bits(base + TX_POWER, PLL_POWER | PLL_ISOLATE, PLL_POWER);
	usleep_range(1000, 1500);
	mtk_phy_clear_bits(base + TX_PLL0, PLL_ENABLE);
	mtk_phy_update_bits(base + TX_PLL0, PLL_PREDIV | PLL_POSDIV | PLL_S2QDIV,
			    FIELD_PREP(PLL_POSDIV, postdiv) | FIELD_PREP(PLL_S2QDIV, 2));
	/* S2Q divider = 4; rate = 13 MHz * PCW / 2^24 / 2^POSDIV. */
	pcw = div_u64(((u64)tx->data_rate << postdiv) << 24, 13000000);
	writel(pcw, base + TX_PCW);
	mtk_phy_update_bits(base + TX_PLL1, BIT(0) | BIT(2), BIT(0));
	mtk_phy_set_bits(base + TX_PLL0, PLL_ENABLE);
	usleep_range(1000, 1500);
	writel(0, base + TX_PCW_CHANGE);
	writel(1, base + TX_PCW_CHANGE);
	return 0;
}

static void mt6797_mipi_pll_unprepare(struct clk_hw *hw)
{
	struct mtk_mipi_tx *tx = mtk_mipi_tx_from_clk_hw(hw);

	mtk_phy_clear_bits(tx->regs + TX_PLL0, PLL_ENABLE);
	mtk_phy_update_bits(tx->regs + TX_POWER, PLL_POWER | PLL_ISOLATE, PLL_ISOLATE);
	mtk_phy_clear_bits(tx->regs + TX_CORE, BIT(0) | BIT(1));
	mtk_phy_clear_bits(tx->regs + TX_BG, BIT(0));
}

static int mt6797_mipi_pll_is_prepared(struct clk_hw *hw)
{
	struct mtk_mipi_tx *tx = mtk_mipi_tx_from_clk_hw(hw);

	return !!(readl(tx->regs + TX_PLL0) & PLL_ENABLE);
}

static long mt6797_mipi_pll_round_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long *parent_rate)
{
	return clamp_val(rate, 50000000, 1250000000);
}

static void mt6797_mipi_enable_signal(struct phy *phy)
{
	struct mtk_mipi_tx *tx = phy_get_drvdata(phy);
	int i;

	for (i = 0; i < 5; i++)
		mtk_phy_set_bits(tx->regs + TX_CLOCK_LANE + i * 4, BIT(0));
	mtk_phy_clear_bits(tx->regs + TX_TOP, PAD_TIE_LOW);
	usleep_range(1000, 1500);
}

static void mt6797_mipi_disable_signal(struct phy *phy)
{
	struct mtk_mipi_tx *tx = phy_get_drvdata(phy);
	int i;

	mtk_phy_set_bits(tx->regs + TX_TOP, PAD_TIE_LOW);
	for (i = 0; i < 5; i++)
		mtk_phy_clear_bits(tx->regs + TX_CLOCK_LANE + i * 4, BIT(0));
}

static const struct clk_ops mt6797_mipi_pll_ops = {
	.prepare = mt6797_mipi_pll_prepare,
	.unprepare = mt6797_mipi_pll_unprepare,
	.is_prepared = mt6797_mipi_pll_is_prepared,
	.round_rate = mt6797_mipi_pll_round_rate,
	.set_rate = mtk_mipi_tx_pll_set_rate,
	.recalc_rate = mtk_mipi_tx_pll_recalc_rate,
};

const struct mtk_mipitx_data mt6797_mipitx_data = {
	.mipi_tx_clk_ops = &mt6797_mipi_pll_ops,
	.mipi_tx_enable_signal = mt6797_mipi_enable_signal,
	.mipi_tx_disable_signal = mt6797_mipi_disable_signal,
};
