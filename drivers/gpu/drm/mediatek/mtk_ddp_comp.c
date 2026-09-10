// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 MediaTek Inc.
 * Authors:
 *	YT Shen <yt.shen@mediatek.com>
 *	CK Hu <ck.hu@mediatek.com>
 */

#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mtk-cmdq.h>
#include <drm/drm_print.h>

#include "mtk_crtc.h"
#include "mtk_ddp_comp.h"
#include "mtk_disp_drv.h"
#include "mtk_drm_drv.h"
#include "mtk_plane.h"


#define DISP_REG_DITHER_EN			0x0000
#define DITHER_EN				BIT(0)
#define DISP_REG_DITHER_CFG			0x0020
#define DITHER_RELAY_MODE			BIT(0)
#define DITHER_ENGINE_EN			BIT(1)
#define DISP_DITHERING				BIT(2)
#define DISP_REG_DITHER_SIZE			0x0030
#define DISP_REG_DITHER_5			0x0114
#define DISP_REG_DITHER_7			0x011c
#define DISP_REG_DITHER_15			0x013c
#define DITHER_LSB_ERR_SHIFT_R(x)		(((x) & 0x7) << 28)
#define DITHER_ADD_LSHIFT_R(x)			(((x) & 0x7) << 20)
#define DITHER_NEW_BIT_MODE			BIT(0)
#define DISP_REG_DITHER_16			0x0140
#define DITHER_LSB_ERR_SHIFT_B(x)		(((x) & 0x7) << 28)
#define DITHER_ADD_LSHIFT_B(x)			(((x) & 0x7) << 20)
#define DITHER_LSB_ERR_SHIFT_G(x)		(((x) & 0x7) << 12)
#define DITHER_ADD_LSHIFT_G(x)			(((x) & 0x7) << 4)

#define DISP_REG_DSC_CON			0x0000
#define DISP_REG_DSC_INTSTA			0x0008
#define DSC_EN					BIT(0)
#define DSC_DUAL_INOUT				BIT(2)
#define DSC_IN_SRC_SEL				BIT(3)
#define DSC_BYPASS				BIT(4)
#define DSC_RELAY				BIT(5)
#define DSC_UFOE_SEL				BIT(16)
#define DISP_REG_DSC_PIC_W			0x0018
#define DISP_REG_DSC_PIC_H			0x001C
#define DISP_REG_DSC_SLICE_W			0x0020
#define DISP_REG_DSC_SLICE_H			0x0024
#define DISP_REG_DSC_CHUNK_SIZE			0x0028
#define DISP_REG_DSC_BUF_SIZE			0x002C
#define DISP_REG_DSC_MODE			0x0030
#define DISP_REG_DSC_CFG			0x0034
#define DISP_REG_DSC_PAD			0x0038
#define DISP_REG_DSC_ENC_WIDTH			0x003C
#define DISP_REG_DSC_SPR			0x0014
#define DISP_REG_DSC_DBG_CON			0x0060
#define DSC_CKSM_CAL_EN				BIT(9)
#define DISP_REG_DSC_PPS0			0x0080
#define DISP_REG_DSC_PPS1			0x0084
#define DISP_REG_DSC_PPS2			0x0088
#define DISP_REG_DSC_PPS3			0x008C
#define DISP_REG_DSC_PPS4			0x0090
#define DISP_REG_DSC_PPS5			0x0094
#define DISP_REG_DSC_PPS6			0x0098
#define DISP_REG_DSC_PPS7			0x009C
#define DISP_REG_DSC_PPS8			0x00A0
#define DISP_REG_DSC_PPS9			0x00A4
#define DISP_REG_DSC_PPS10			0x00A8
#define DISP_REG_DSC_PPS11			0x00AC
#define DISP_REG_DSC_PPS12			0x00B0
#define DISP_REG_DSC_PPS13			0x00B4
#define DISP_REG_DSC_PPS14			0x00B8
#define DISP_REG_DSC_PPS15			0x00BC
#define DISP_REG_DSC_PPS16			0x00C0
#define DISP_REG_DSC_PPS17			0x00C4
#define DISP_REG_DSC_PPS18			0x00C8
#define DISP_REG_DSC_PPS19			0x00CC
#define DISP_REG_DSC_SHADOW			0x0200
#define DSC_FORCE_COMMIT			BIT(0)
#define DSC_BYPASS_SHADOW			BIT(1)
#define MT6983_DISP_REG_SHADOW_CTRL		0x0228

#define DISP_REG_OD_EN				0x0000
#define DISP_REG_OD_CFG				0x0020
#define OD_RELAYMODE				BIT(0)
#define DISP_REG_OD_SIZE			0x0030

#define DISP_REG_POSTMASK_EN			0x0000
#define POSTMASK_EN					BIT(0)
#define DISP_REG_POSTMASK_CFG			0x0020
#define POSTMASK_RELAY_MODE				BIT(0)
#define DISP_REG_POSTMASK_SIZE			0x0030

#define DISP_REG_UFO_START			0x0000
#define UFO_BYPASS				BIT(2)

struct mtk_ddp_comp_dev {
	struct clk *clk;
	void __iomem *regs;
	struct cmdq_client_reg cmdq_reg;
	const struct mtk_dsc_config_params *dsc;
	bool dsc_enable;
	/* mt6895: LK already programmed the DSC encoder to match the panel;
	 * do not re-write the PPS/size registers (see mtk_dsc_config) */
	bool preserve_lk_dsc;
};

void mtk_ddp_write(struct cmdq_pkt *cmdq_pkt, unsigned int value,
		   struct cmdq_client_reg *cmdq_reg, void __iomem *regs,
		   unsigned int offset)
{
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	if (cmdq_pkt)
		cmdq_pkt_write(cmdq_pkt, cmdq_reg->subsys,
			       cmdq_reg->offset + offset, value);
	else
#endif
		writel(value, regs + offset);
}

void mtk_ddp_write_relaxed(struct cmdq_pkt *cmdq_pkt, unsigned int value,
			   struct cmdq_client_reg *cmdq_reg, void __iomem *regs,
			   unsigned int offset)
{
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	if (cmdq_pkt)
		cmdq_pkt_write(cmdq_pkt, cmdq_reg->subsys,
			       cmdq_reg->offset + offset, value);
	else
#endif
		writel_relaxed(value, regs + offset);
}

void mtk_ddp_write_mask(struct cmdq_pkt *cmdq_pkt, unsigned int value,
			struct cmdq_client_reg *cmdq_reg, void __iomem *regs,
			unsigned int offset, unsigned int mask)
{
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	if (cmdq_pkt) {
		cmdq_pkt_write_mask(cmdq_pkt, cmdq_reg->subsys,
				    cmdq_reg->offset + offset, value, mask);
	} else {
#endif
		u32 tmp = readl(regs + offset);

		tmp = (tmp & ~mask) | (value & mask);
		writel(tmp, regs + offset);
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	}
#endif
}

static int mtk_ddp_clk_enable(struct device *dev)
{
	struct mtk_ddp_comp_dev *priv = dev_get_drvdata(dev);

	return clk_prepare_enable(priv->clk);
}

static void mtk_ddp_clk_disable(struct device *dev)
{
	struct mtk_ddp_comp_dev *priv = dev_get_drvdata(dev);

	clk_disable_unprepare(priv->clk);
}

void mtk_dither_set_common(void __iomem *regs, struct cmdq_client_reg *cmdq_reg,
			   unsigned int bpc, unsigned int cfg,
			   unsigned int dither_en, struct cmdq_pkt *cmdq_pkt)
{
	/* If bpc equal to 0, the dithering function didn't be enabled */
	if (bpc == 0)
		return;

	if (bpc >= MTK_MIN_BPC) {
		mtk_ddp_write(cmdq_pkt, 0, cmdq_reg, regs, DISP_REG_DITHER_5);
		mtk_ddp_write(cmdq_pkt, 0, cmdq_reg, regs, DISP_REG_DITHER_7);
		mtk_ddp_write(cmdq_pkt,
			      DITHER_LSB_ERR_SHIFT_R(MTK_MAX_BPC - bpc) |
			      DITHER_ADD_LSHIFT_R(MTK_MAX_BPC - bpc) |
			      DITHER_NEW_BIT_MODE,
			      cmdq_reg, regs, DISP_REG_DITHER_15);
		mtk_ddp_write(cmdq_pkt,
			      DITHER_LSB_ERR_SHIFT_B(MTK_MAX_BPC - bpc) |
			      DITHER_ADD_LSHIFT_B(MTK_MAX_BPC - bpc) |
			      DITHER_LSB_ERR_SHIFT_G(MTK_MAX_BPC - bpc) |
			      DITHER_ADD_LSHIFT_G(MTK_MAX_BPC - bpc),
			      cmdq_reg, regs, DISP_REG_DITHER_16);
		mtk_ddp_write(cmdq_pkt, dither_en, cmdq_reg, regs, cfg);
	}
}

static void mtk_dither_config(struct device *dev, unsigned int w,
			      unsigned int h, unsigned int vrefresh,
			      unsigned int bpc, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_ddp_comp_dev *priv = dev_get_drvdata(dev);

	mtk_ddp_write(cmdq_pkt, w << 16 | h, &priv->cmdq_reg, priv->regs, DISP_REG_DITHER_SIZE);
	mtk_ddp_write(cmdq_pkt, DITHER_RELAY_MODE, &priv->cmdq_reg, priv->regs,
		      DISP_REG_DITHER_CFG);
	mtk_dither_set_common(priv->regs, &priv->cmdq_reg, bpc, DISP_REG_DITHER_CFG,
			      DITHER_ENGINE_EN, cmdq_pkt);
}

static void mtk_dither_start(struct device *dev)
{
	struct mtk_ddp_comp_dev *priv = dev_get_drvdata(dev);

	writel(DITHER_EN, priv->regs + DISP_REG_DITHER_EN);
}

static void mtk_dither_stop(struct device *dev)
{
	struct mtk_ddp_comp_dev *priv = dev_get_drvdata(dev);

	writel_relaxed(0x0, priv->regs + DISP_REG_DITHER_EN);
}

static void mtk_dither_set(struct device *dev, unsigned int bpc,
			   unsigned int cfg, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_ddp_comp_dev *priv = dev_get_drvdata(dev);

	mtk_dither_set_common(priv->regs, &priv->cmdq_reg, bpc, cfg,
			      DISP_DITHERING, cmdq_pkt);
}

static void mtk_dsc_config(struct device *dev, unsigned int w,
			   unsigned int h, unsigned int vrefresh,
			   unsigned int bpc, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_ddp_comp_dev *priv = dev_get_drvdata(dev);
	const struct mtk_dsc_config_params *dsc = priv->dsc;
	unsigned int pic_group_width, slice_width, slice_height;
	unsigned int enc_slice_width, enc_pic_width;
	unsigned int pic_height_ext_num, slice_group_width;
	unsigned int bit_per_pixel, chunk_size, pad_num;
	unsigned int dsc_con = 0, reg_val;

	/* dsc bypass mode when no panel params are programmed */
	if (!dsc || !dsc->enable) {
		mtk_ddp_write_mask(cmdq_pkt, DSC_BYPASS, &priv->cmdq_reg, priv->regs,
				   DISP_REG_DSC_CON, DSC_BYPASS);
		mtk_ddp_write_mask(cmdq_pkt, DSC_UFOE_SEL, &priv->cmdq_reg, priv->regs,
				   DISP_REG_DSC_CON, DSC_UFOE_SEL);
		mtk_ddp_write_mask(cmdq_pkt, DSC_DUAL_INOUT, &priv->cmdq_reg, priv->regs,
				   DISP_REG_DSC_CON, DSC_DUAL_INOUT);
		return;
	}

	/*
	 * MT6983/6895-gen DSC uses shadow registers; bypass them so the
	 * CON/PPS/size writes below land in the active registers (matches
	 * downstream mtk_dsc_prepare). Otherwise the config stays in shadow
	 * and the encoder runs with stale defaults.
	 */
	mtk_ddp_write_mask(cmdq_pkt, DSC_BYPASS_SHADOW, &priv->cmdq_reg,
			   priv->regs, MT6983_DISP_REG_SHADOW_CTRL,
			   DSC_BYPASS_SHADOW);

	/*
	 * XAGA: LK already programmed the DISP_DSC encoder to match the
	 * panel's decoder (it initialized the whole pipeline, including the
	 * panel DSC PPS via the 0xC1 init command). The stock mediatek_v2
	 * driver does NOT reprogram the DSC block on this SoC - our dumps
	 * showed mtk_dsc_config is never called there. Re-writing the PPS/
	 * size registers here produces a stream the panel's LK-programmed
	 * decoder cannot decode (grey + purple dots). Preserve LK's DSC
	 * encoder state; mtk_dsc_start still keeps the block enabled.
	 * (Verified against the working Tianma 5.10/recovery state: DSC
	 * CON stays 0x10089 and DSC INTSTA=0x1 clean.)
	 */
	if (priv->preserve_lk_dsc)
		return;

	pr_info("XAGA-STAGE dsc_config: w=%d h=%d slice_mode=%d slice(%d,%d) bpp=%d ver=%d\n",
		w, h, dsc->slice_mode, dsc->slice_width, dsc->slice_height,
		dsc->bit_per_pixel, dsc->ver);

	pic_group_width = (w + 2) / 3;
	slice_width = dsc->slice_width;
	slice_height = dsc->slice_height;
	pic_height_ext_num = DIV_ROUND_UP(h, slice_height);
	slice_group_width = (slice_width + 2) / 3;
	bit_per_pixel = dsc->bit_per_pixel;
	chunk_size = slice_width * bit_per_pixel / 8 / 16;
	enc_slice_width = slice_width;
	enc_pic_width = enc_slice_width * (dsc->slice_mode + 1);
	pad_num = (chunk_size * (dsc->slice_mode + 1) + 2) / 3 * 3
		- chunk_size * (dsc->slice_mode + 1);

	mtk_ddp_write_relaxed(cmdq_pkt, 0x0, &priv->cmdq_reg, priv->regs,
			      DISP_REG_DSC_SPR);
	mtk_ddp_write_relaxed(cmdq_pkt, enc_pic_width << 16 | enc_slice_width,
			      &priv->cmdq_reg, priv->regs, DISP_REG_DSC_ENC_WIDTH);

	if (dsc->ver == 2)
		dsc_con = 0x4080;
	else
		dsc_con = 0x4000;
	dsc_con |= DSC_UFOE_SEL;
	mtk_ddp_write_relaxed(cmdq_pkt, dsc_con, &priv->cmdq_reg, priv->regs,
			      DISP_REG_DSC_CON);

	mtk_ddp_write_relaxed(cmdq_pkt,
			      (pic_group_width - 1) << 16 | w,
			      &priv->cmdq_reg, priv->regs, DISP_REG_DSC_PIC_W);
	mtk_ddp_write_relaxed(cmdq_pkt,
			      (pic_height_ext_num * slice_height - 1) << 16 |
			      (h - 1),
			      &priv->cmdq_reg, priv->regs, DISP_REG_DSC_PIC_H);
	mtk_ddp_write_relaxed(cmdq_pkt,
			      (slice_group_width - 1) << 16 | slice_width,
			      &priv->cmdq_reg, priv->regs, DISP_REG_DSC_SLICE_W);
	mtk_ddp_write_relaxed(cmdq_pkt,
			      (slice_width % 3) << 30 |
			      (pic_height_ext_num - 1) << 16 |
			      (slice_height - 1),
			      &priv->cmdq_reg, priv->regs, DISP_REG_DSC_SLICE_H);
	mtk_ddp_write_relaxed(cmdq_pkt, chunk_size, &priv->cmdq_reg, priv->regs,
			      DISP_REG_DSC_CHUNK_SIZE);
	mtk_ddp_write_relaxed(cmdq_pkt, pad_num, &priv->cmdq_reg, priv->regs,
			      DISP_REG_DSC_PAD);
	mtk_ddp_write_relaxed(cmdq_pkt, chunk_size * slice_height,
			      &priv->cmdq_reg, priv->regs, DISP_REG_DSC_BUF_SIZE);

	reg_val = (!!dsc->slice_mode) | (!!dsc->rgb_swap << 2);
	mtk_ddp_write_mask(cmdq_pkt, reg_val, &priv->cmdq_reg, priv->regs,
			   DISP_REG_DSC_MODE, 0xFFFF);

	mtk_ddp_write_relaxed(cmdq_pkt,
			      (dsc->dsc_cfg == 40) ? 0x0828 : 0x0022,
			      &priv->cmdq_reg, priv->regs, DISP_REG_DSC_CFG);

	mtk_ddp_write_mask(cmdq_pkt, DSC_CKSM_CAL_EN, &priv->cmdq_reg,
			   priv->regs, DISP_REG_DSC_DBG_CON, DSC_CKSM_CAL_EN);

	mtk_ddp_write_mask(cmdq_pkt,
			   (((dsc->ver & 0xf) == 2) ? 0x40 : 0x20),
			   &priv->cmdq_reg, priv->regs,
			   DISP_REG_DSC_SHADOW, 0x60);

	reg_val = dsc->line_buf_depth;
	reg_val |= (dsc->bit_per_channel ? dsc->bit_per_channel : 8) << 4;
	reg_val |= (dsc->bit_per_pixel ? dsc->bit_per_pixel : 0x80) << 8;
	reg_val |= dsc->rct_on << 18;
	reg_val |= dsc->bp_enable << 19;
	mtk_ddp_write_relaxed(cmdq_pkt, reg_val, &priv->cmdq_reg, priv->regs,
			      DISP_REG_DSC_PPS0);

	reg_val = dsc->xmit_delay ? dsc->xmit_delay : 0x200;
	reg_val |= (dsc->dec_delay ? dsc->dec_delay : 0x268) << 16;
	mtk_ddp_write_relaxed(cmdq_pkt, reg_val, &priv->cmdq_reg, priv->regs,
			      DISP_REG_DSC_PPS1);

	reg_val = dsc->scale_value ? dsc->scale_value : 0x20;
	reg_val |= (dsc->increment_interval ? dsc->increment_interval : 0x387) << 16;
	mtk_ddp_write_relaxed(cmdq_pkt, reg_val, &priv->cmdq_reg, priv->regs,
			      DISP_REG_DSC_PPS2);

	reg_val = dsc->decrement_interval ? dsc->decrement_interval : 0xa;
	reg_val |= (dsc->line_bpg_offset ? dsc->line_bpg_offset : 0xc) << 16;
	mtk_ddp_write_relaxed(cmdq_pkt, reg_val, &priv->cmdq_reg, priv->regs,
			      DISP_REG_DSC_PPS3);

	reg_val = dsc->nfl_bpg_offset ? dsc->nfl_bpg_offset : 0x319;
	reg_val |= (dsc->slice_bpg_offset ? dsc->slice_bpg_offset : 0x263) << 16;
	mtk_ddp_write_relaxed(cmdq_pkt, reg_val, &priv->cmdq_reg, priv->regs,
			      DISP_REG_DSC_PPS4);

	reg_val = dsc->initial_offset ? dsc->initial_offset : 0x1800;
	reg_val |= (dsc->final_offset ? dsc->final_offset : 0x10f0) << 16;
	mtk_ddp_write_relaxed(cmdq_pkt, reg_val, &priv->cmdq_reg, priv->regs,
			      DISP_REG_DSC_PPS5);

	/*
	 * PPS6..PPS19: 8bpc flatness/RC table (matches the "8bpc_to_8bpp"
	 * config used by the downstream DSC panel driver for 8bpc panels).
	 */
	mtk_ddp_write(cmdq_pkt, 0x20000c03, &priv->cmdq_reg, priv->regs,
		      DISP_REG_DSC_PPS6);
	mtk_ddp_write(cmdq_pkt, 0x330b0b06, &priv->cmdq_reg, priv->regs,
		      DISP_REG_DSC_PPS7);
	mtk_ddp_write(cmdq_pkt, 0x382a1c0e, &priv->cmdq_reg, priv->regs,
		      DISP_REG_DSC_PPS8);
	mtk_ddp_write(cmdq_pkt, 0x69625446, &priv->cmdq_reg, priv->regs,
		      DISP_REG_DSC_PPS9);
	mtk_ddp_write(cmdq_pkt, 0x7b797770, &priv->cmdq_reg, priv->regs,
		      DISP_REG_DSC_PPS10);
	mtk_ddp_write(cmdq_pkt, 0x00007e7d, &priv->cmdq_reg, priv->regs,
		      DISP_REG_DSC_PPS11);
	mtk_ddp_write(cmdq_pkt, 0x00800880, &priv->cmdq_reg, priv->regs,
		      DISP_REG_DSC_PPS12);
	mtk_ddp_write(cmdq_pkt, 0xf8c100a1, &priv->cmdq_reg, priv->regs,
		      DISP_REG_DSC_PPS13);
	mtk_ddp_write(cmdq_pkt, 0xe8e3f0e3, &priv->cmdq_reg, priv->regs,
		      DISP_REG_DSC_PPS14);
	mtk_ddp_write(cmdq_pkt, 0xe103e0e3, &priv->cmdq_reg, priv->regs,
		      DISP_REG_DSC_PPS15);
	mtk_ddp_write(cmdq_pkt, 0xd943e123, &priv->cmdq_reg, priv->regs,
		      DISP_REG_DSC_PPS16);
	mtk_ddp_write(cmdq_pkt, 0xd185d965, &priv->cmdq_reg, priv->regs,
		      DISP_REG_DSC_PPS17);
	mtk_ddp_write(cmdq_pkt, 0xd1a7d1a5, &priv->cmdq_reg, priv->regs,
		      DISP_REG_DSC_PPS18);
	mtk_ddp_write(cmdq_pkt, 0x0000d1ed, &priv->cmdq_reg, priv->regs,
		      DISP_REG_DSC_PPS19);

	priv->dsc_enable = true;
}

static void mtk_dsc_start(struct device *dev)
{
	struct mtk_ddp_comp_dev *priv = dev_get_drvdata(dev);

	/*
	 * MT6983/M6895-gen DSC: bypass the shadow register so the CON/PPS/
	 * size registers written by mtk_dsc_config take effect immediately.
	 * Downstream mtk_dsc_prepare writes DSC_BYPASS_SHADOW to
	 * MT6983_DISP_REG_SHADOW_CTRL (0x228); without it the config stays
	 * in shadow and the encoder stops after a few frames (DSI
	 * INP_UNFINISH -> pipeline stall).
	 */
	mtk_ddp_write_mask(NULL, DSC_BYPASS_SHADOW, &priv->cmdq_reg, priv->regs,
			   MT6983_DISP_REG_SHADOW_CTRL, DSC_BYPASS_SHADOW);

	/* write with mask to reserve the value set in mtk_dsc_config */
	mtk_ddp_write_mask(NULL, DSC_FORCE_COMMIT, &priv->cmdq_reg, priv->regs,
			   DISP_REG_DSC_SHADOW, DSC_FORCE_COMMIT);
	if (priv->dsc_enable || priv->preserve_lk_dsc)
		mtk_ddp_write_mask(NULL, DSC_EN, &priv->cmdq_reg, priv->regs,
				   DISP_REG_DSC_CON, DSC_EN);

	/*
	 * XAGA diag: clear DSC INTSTA via INTACK (0xC) and report. If it
	 * re-latches to 0x9 (ABN_EOF) immediately, the DSC input is
	 * live-malformed; if it stays 0x0/0x1 the ABN_EOF was a stale
	 * boot-time latch and the encoder is actually clean.
	 */
	{
		void __iomem *b = priv->regs;
		u32 v = readl(b + DISP_REG_DSC_INTSTA);

		writel_relaxed(v, b + 0xC);
		writel_relaxed(0x0, b + 0xC);
		pr_info("XAGA-DSC start: CON=0x%08x INTSTA(was)=0x%08x INTSTA(now)=0x%08x\n",
			readl(b + DISP_REG_DSC_CON), v,
			readl(b + DISP_REG_DSC_INTSTA));
		pr_info("XAGA-DSC FULL: MODE=0x%08x ENC_W=0x%08x PIC_W=0x%08x PIC_H=0x%08x SLICE_W=0x%08x SLICE_H=0x%08x CHUNK=0x%08x BUF=0x%08x\n",
			readl(b + DISP_REG_DSC_MODE), readl(b + DISP_REG_DSC_ENC_WIDTH),
			readl(b + DISP_REG_DSC_PIC_W), readl(b + DISP_REG_DSC_PIC_H),
			readl(b + DISP_REG_DSC_SLICE_W), readl(b + DISP_REG_DSC_SLICE_H),
			readl(b + DISP_REG_DSC_CHUNK_SIZE), readl(b + DISP_REG_DSC_BUF_SIZE));
		pr_info("XAGA-DSC FULL: PPS0=0x%08x PPS1=0x%08x PPS2=0x%08x PPS3=0x%08x PPS4=0x%08x PPS5=0x%08x SHADOW=0x%08x\n",
			readl(b + DISP_REG_DSC_PPS0), readl(b + DISP_REG_DSC_PPS1),
			readl(b + DISP_REG_DSC_PPS2), readl(b + DISP_REG_DSC_PPS3),
			readl(b + DISP_REG_DSC_PPS4), readl(b + DISP_REG_DSC_PPS5),
			readl(b + MT6983_DISP_REG_SHADOW_CTRL));
		pr_info("XAGA-DSC FULL: SPR=0x%08x CFG=0x%08x PAD=0x%08x DBG=0x%08x\n",
			readl(b + DISP_REG_DSC_SPR), readl(b + DISP_REG_DSC_CFG),
			readl(b + DISP_REG_DSC_PAD), readl(b + DISP_REG_DSC_DBG_CON));
		pr_info("XAGA-DSC FULL: PPS6=0x%08x PPS7=0x%08x PPS8=0x%08x PPS9=0x%08x PPS10=0x%08x PPS11=0x%08x\n",
			readl(b + DISP_REG_DSC_PPS6), readl(b + DISP_REG_DSC_PPS7),
			readl(b + DISP_REG_DSC_PPS8), readl(b + DISP_REG_DSC_PPS9),
			readl(b + DISP_REG_DSC_PPS10), readl(b + DISP_REG_DSC_PPS11));
		pr_info("XAGA-DSC FULL: PPS12=0x%08x PPS13=0x%08x PPS14=0x%08x PPS15=0x%08x PPS16=0x%08x PPS17=0x%08x PPS18=0x%08x PPS19=0x%08x\n",
			readl(b + DISP_REG_DSC_PPS12), readl(b + DISP_REG_DSC_PPS13),
			readl(b + DISP_REG_DSC_PPS14), readl(b + DISP_REG_DSC_PPS15),
			readl(b + DISP_REG_DSC_PPS16), readl(b + DISP_REG_DSC_PPS17),
			readl(b + DISP_REG_DSC_PPS18), readl(b + DISP_REG_DSC_PPS19));
	}
}

static void mtk_dsc_stop(struct device *dev)
{
	struct mtk_ddp_comp_dev *priv = dev_get_drvdata(dev);

	mtk_ddp_write_mask(NULL, 0x0, &priv->cmdq_reg, priv->regs,
			   DISP_REG_DSC_CON, DSC_EN);
}

void mtk_ddp_comp_dsc_set_config(struct device *dev,
				 const struct mtk_dsc_config_params *cfg)
{
	struct mtk_ddp_comp_dev *priv = dev_get_drvdata(dev);

	if (priv)
		priv->dsc = cfg;
}

static void mtk_od_config(struct device *dev, unsigned int w,
			  unsigned int h, unsigned int vrefresh,
			  unsigned int bpc, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_ddp_comp_dev *priv = dev_get_drvdata(dev);

	mtk_ddp_write(cmdq_pkt, w << 16 | h, &priv->cmdq_reg, priv->regs, DISP_REG_OD_SIZE);
	mtk_ddp_write(cmdq_pkt, OD_RELAYMODE, &priv->cmdq_reg, priv->regs, DISP_REG_OD_CFG);
	mtk_dither_set(dev, bpc, DISP_REG_OD_CFG, cmdq_pkt);
}

static void mtk_od_start(struct device *dev)
{
	struct mtk_ddp_comp_dev *priv = dev_get_drvdata(dev);

	writel(1, priv->regs + DISP_REG_OD_EN);
}

static void mtk_postmask_config(struct device *dev, unsigned int w,
				unsigned int h, unsigned int vrefresh,
				unsigned int bpc, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_ddp_comp_dev *priv = dev_get_drvdata(dev);

	mtk_ddp_write(cmdq_pkt, w << 16 | h, &priv->cmdq_reg, priv->regs,
		      DISP_REG_POSTMASK_SIZE);
	mtk_ddp_write(cmdq_pkt, POSTMASK_RELAY_MODE, &priv->cmdq_reg,
		      priv->regs, DISP_REG_POSTMASK_CFG);
}

static void mtk_postmask_start(struct device *dev)
{
	struct mtk_ddp_comp_dev *priv = dev_get_drvdata(dev);

	writel(POSTMASK_EN, priv->regs + DISP_REG_POSTMASK_EN);
}

static void mtk_postmask_stop(struct device *dev)
{
	struct mtk_ddp_comp_dev *priv = dev_get_drvdata(dev);

	writel_relaxed(0x0, priv->regs + DISP_REG_POSTMASK_EN);
}

static void mtk_ufoe_start(struct device *dev)
{
	struct mtk_ddp_comp_dev *priv = dev_get_drvdata(dev);

	writel(UFO_BYPASS, priv->regs + DISP_REG_UFO_START);
}

static const struct mtk_ddp_comp_funcs ddp_aal = {
	.clk_enable = mtk_aal_clk_enable,
	.clk_disable = mtk_aal_clk_disable,
	.gamma_get_lut_size = mtk_aal_gamma_get_lut_size,
	.gamma_set = mtk_aal_gamma_set,
	.config = mtk_aal_config,
	.start = mtk_aal_start,
	.stop = mtk_aal_stop,
};

static const struct mtk_ddp_comp_funcs ddp_ccorr = {
	.clk_enable = mtk_ccorr_clk_enable,
	.clk_disable = mtk_ccorr_clk_disable,
	.config = mtk_ccorr_config,
	.start = mtk_ccorr_start,
	.stop = mtk_ccorr_stop,
	.ctm_set = mtk_ccorr_ctm_set,
};

static const struct mtk_ddp_comp_funcs ddp_color = {
	.clk_enable = mtk_color_clk_enable,
	.clk_disable = mtk_color_clk_disable,
	.config = mtk_color_config,
	.start = mtk_color_start,
};

static const struct mtk_ddp_comp_funcs ddp_dither = {
	.clk_enable = mtk_ddp_clk_enable,
	.clk_disable = mtk_ddp_clk_disable,
	.config = mtk_dither_config,
	.start = mtk_dither_start,
	.stop = mtk_dither_stop,
};

static const struct mtk_ddp_comp_funcs ddp_dpi = {
	.start = mtk_dpi_start,
	.stop = mtk_dpi_stop,
	.encoder_index = mtk_dpi_encoder_index,
};

static const struct mtk_ddp_comp_funcs ddp_dsc = {
	.clk_enable = mtk_ddp_clk_enable,
	.clk_disable = mtk_ddp_clk_disable,
	.config = mtk_dsc_config,
	.start = mtk_dsc_start,
	.stop = mtk_dsc_stop,
};

static const struct mtk_ddp_comp_funcs ddp_dsi = {
	.start = mtk_dsi_ddp_start,
	.stop = mtk_dsi_ddp_stop,
	.encoder_index = mtk_dsi_encoder_index,
};

static const struct mtk_ddp_comp_funcs ddp_gamma = {
	.clk_enable = mtk_gamma_clk_enable,
	.clk_disable = mtk_gamma_clk_disable,
	.gamma_get_lut_size = mtk_gamma_get_lut_size,
	.gamma_set = mtk_gamma_set,
	.config = mtk_gamma_config,
	.start = mtk_gamma_start,
	.stop = mtk_gamma_stop,
};

static const struct mtk_ddp_comp_funcs ddp_merge = {
	.clk_enable = mtk_merge_clk_enable,
	.clk_disable = mtk_merge_clk_disable,
	.start = mtk_merge_start,
	.stop = mtk_merge_stop,
	.config = mtk_merge_config,
};

static const struct mtk_ddp_comp_funcs ddp_od = {
	.clk_enable = mtk_ddp_clk_enable,
	.clk_disable = mtk_ddp_clk_disable,
	.config = mtk_od_config,
	.start = mtk_od_start,
};

static const struct mtk_ddp_comp_funcs ddp_ovl = {
	.clk_enable = mtk_ovl_clk_enable,
	.clk_disable = mtk_ovl_clk_disable,
	.config = mtk_ovl_config,
	.start = mtk_ovl_start,
	.stop = mtk_ovl_stop,
	.register_vblank_cb = mtk_ovl_register_vblank_cb,
	.unregister_vblank_cb = mtk_ovl_unregister_vblank_cb,
	.enable_vblank = mtk_ovl_enable_vblank,
	.disable_vblank = mtk_ovl_disable_vblank,
	.supported_rotations = mtk_ovl_supported_rotations,
	.layer_nr = mtk_ovl_layer_nr,
	.layer_check = mtk_ovl_layer_check,
	.layer_config = mtk_ovl_layer_config,
	.bgclr_in_on = mtk_ovl_bgclr_in_on,
	.bgclr_in_off = mtk_ovl_bgclr_in_off,
	.get_blend_modes = mtk_ovl_get_blend_modes,
	.get_formats = mtk_ovl_get_formats,
	.get_num_formats = mtk_ovl_get_num_formats,
	.is_afbc_supported = mtk_ovl_is_afbc_supported,
};

static const struct mtk_ddp_comp_funcs ddp_postmask = {
	.clk_enable = mtk_ddp_clk_enable,
	.clk_disable = mtk_ddp_clk_disable,
	.config = mtk_postmask_config,
	.start = mtk_postmask_start,
	.stop = mtk_postmask_stop,
};

static const struct mtk_ddp_comp_funcs ddp_rdma = {
	.clk_enable = mtk_rdma_clk_enable,
	.clk_disable = mtk_rdma_clk_disable,
	.config = mtk_rdma_config,
	.start = mtk_rdma_start,
	.stop = mtk_rdma_stop,
	.register_vblank_cb = mtk_rdma_register_vblank_cb,
	.unregister_vblank_cb = mtk_rdma_unregister_vblank_cb,
	.enable_vblank = mtk_rdma_enable_vblank,
	.disable_vblank = mtk_rdma_disable_vblank,
	.layer_nr = mtk_rdma_layer_nr,
	.layer_config = mtk_rdma_layer_config,
	.get_formats = mtk_rdma_get_formats,
	.get_num_formats = mtk_rdma_get_num_formats,
};

static const struct mtk_ddp_comp_funcs ddp_ufoe = {
	.clk_enable = mtk_ddp_clk_enable,
	.clk_disable = mtk_ddp_clk_disable,
	.start = mtk_ufoe_start,
};

static const struct mtk_ddp_comp_funcs ddp_ovl_adaptor = {
	.power_on = mtk_ovl_adaptor_power_on,
	.power_off = mtk_ovl_adaptor_power_off,
	.clk_enable = mtk_ovl_adaptor_clk_enable,
	.clk_disable = mtk_ovl_adaptor_clk_disable,
	.config = mtk_ovl_adaptor_config,
	.start = mtk_ovl_adaptor_start,
	.stop = mtk_ovl_adaptor_stop,
	.layer_nr = mtk_ovl_adaptor_layer_nr,
	.layer_config = mtk_ovl_adaptor_layer_config,
	.register_vblank_cb = mtk_ovl_adaptor_register_vblank_cb,
	.unregister_vblank_cb = mtk_ovl_adaptor_unregister_vblank_cb,
	.enable_vblank = mtk_ovl_adaptor_enable_vblank,
	.disable_vblank = mtk_ovl_adaptor_disable_vblank,
	.dma_dev_get = mtk_ovl_adaptor_dma_dev_get,
	.connect = mtk_ovl_adaptor_connect,
	.disconnect = mtk_ovl_adaptor_disconnect,
	.add = mtk_ovl_adaptor_add_comp,
	.remove = mtk_ovl_adaptor_remove_comp,
	.get_blend_modes = mtk_ovl_adaptor_get_blend_modes,
	.get_formats = mtk_ovl_adaptor_get_formats,
	.get_num_formats = mtk_ovl_adaptor_get_num_formats,
	.mode_valid = mtk_ovl_adaptor_mode_valid,
};

static const char * const mtk_ddp_comp_stem[MTK_DDP_COMP_TYPE_MAX] = {
	[MTK_DISP_AAL] = "aal",
	[MTK_DISP_BLS] = "bls",
	[MTK_DISP_CCORR] = "ccorr",
	[MTK_DISP_COLOR] = "color",
	[MTK_DISP_DITHER] = "dither",
	[MTK_DISP_DSC] = "dsc",
	[MTK_DISP_GAMMA] = "gamma",
	[MTK_DISP_MERGE] = "merge",
	[MTK_DISP_MUTEX] = "mutex",
	[MTK_DISP_OD] = "od",
	[MTK_DISP_OVL] = "ovl",
	[MTK_DISP_OVL_2L] = "ovl-2l",
	[MTK_DISP_OVL_ADAPTOR] = "ovl_adaptor",
	[MTK_DISP_POSTMASK] = "postmask",
	[MTK_DISP_PWM] = "pwm",
	[MTK_DISP_RDMA] = "rdma",
	[MTK_DISP_UFOE] = "ufoe",
	[MTK_DISP_WDMA] = "wdma",
	[MTK_DP_INTF] = "dp-intf",
	[MTK_DPI] = "dpi",
	[MTK_DSI] = "dsi",
};

struct mtk_ddp_comp_match {
	enum mtk_ddp_comp_type type;
	int alias_id;
	const struct mtk_ddp_comp_funcs *funcs;
};

static const struct mtk_ddp_comp_match mtk_ddp_matches[DDP_COMPONENT_DRM_ID_MAX] = {
	[DDP_COMPONENT_AAL0]		= { MTK_DISP_AAL,		0, &ddp_aal },
	[DDP_COMPONENT_AAL1]		= { MTK_DISP_AAL,		1, &ddp_aal },
	[DDP_COMPONENT_BLS]		= { MTK_DISP_BLS,		0, NULL },
	[DDP_COMPONENT_CCORR]		= { MTK_DISP_CCORR,		0, &ddp_ccorr },
	[DDP_COMPONENT_COLOR0]		= { MTK_DISP_COLOR,		0, &ddp_color },
	[DDP_COMPONENT_COLOR1]		= { MTK_DISP_COLOR,		1, &ddp_color },
	[DDP_COMPONENT_DITHER0]		= { MTK_DISP_DITHER,		0, &ddp_dither },
	[DDP_COMPONENT_DP_INTF0]	= { MTK_DP_INTF,		0, &ddp_dpi },
	[DDP_COMPONENT_DP_INTF1]	= { MTK_DP_INTF,		1, &ddp_dpi },
	[DDP_COMPONENT_DPI0]		= { MTK_DPI,			0, &ddp_dpi },
	[DDP_COMPONENT_DPI1]		= { MTK_DPI,			1, &ddp_dpi },
	[DDP_COMPONENT_DRM_OVL_ADAPTOR]	= { MTK_DISP_OVL_ADAPTOR,	0, &ddp_ovl_adaptor },
	[DDP_COMPONENT_DSC0]		= { MTK_DISP_DSC,		0, &ddp_dsc },
	[DDP_COMPONENT_DSC1]		= { MTK_DISP_DSC,		1, &ddp_dsc },
	[DDP_COMPONENT_DSI0]		= { MTK_DSI,			0, &ddp_dsi },
	[DDP_COMPONENT_DSI1]		= { MTK_DSI,			1, &ddp_dsi },
	[DDP_COMPONENT_DSI2]		= { MTK_DSI,			2, &ddp_dsi },
	[DDP_COMPONENT_DSI3]		= { MTK_DSI,			3, &ddp_dsi },
	[DDP_COMPONENT_GAMMA]		= { MTK_DISP_GAMMA,		0, &ddp_gamma },
	[DDP_COMPONENT_MERGE0]		= { MTK_DISP_MERGE,		0, &ddp_merge },
	[DDP_COMPONENT_MERGE1]		= { MTK_DISP_MERGE,		1, &ddp_merge },
	[DDP_COMPONENT_MERGE2]		= { MTK_DISP_MERGE,		2, &ddp_merge },
	[DDP_COMPONENT_MERGE3]		= { MTK_DISP_MERGE,		3, &ddp_merge },
	[DDP_COMPONENT_MERGE4]		= { MTK_DISP_MERGE,		4, &ddp_merge },
	[DDP_COMPONENT_MERGE5]		= { MTK_DISP_MERGE,		5, &ddp_merge },
	[DDP_COMPONENT_OD0]		= { MTK_DISP_OD,		0, &ddp_od },
	[DDP_COMPONENT_OD1]		= { MTK_DISP_OD,		1, &ddp_od },
	[DDP_COMPONENT_OVL0]		= { MTK_DISP_OVL,		0, &ddp_ovl },
	[DDP_COMPONENT_OVL1]		= { MTK_DISP_OVL,		1, &ddp_ovl },
	[DDP_COMPONENT_OVL_2L0]		= { MTK_DISP_OVL_2L,		0, &ddp_ovl },
	[DDP_COMPONENT_OVL_2L1]		= { MTK_DISP_OVL_2L,		1, &ddp_ovl },
	[DDP_COMPONENT_OVL_2L2]		= { MTK_DISP_OVL_2L,		2, &ddp_ovl },
	[DDP_COMPONENT_POSTMASK0]	= { MTK_DISP_POSTMASK,		0, &ddp_postmask },
	[DDP_COMPONENT_PWM0]		= { MTK_DISP_PWM,		0, NULL },
	[DDP_COMPONENT_PWM1]		= { MTK_DISP_PWM,		1, NULL },
	[DDP_COMPONENT_PWM2]		= { MTK_DISP_PWM,		2, NULL },
	[DDP_COMPONENT_RDMA0]		= { MTK_DISP_RDMA,		0, &ddp_rdma },
	[DDP_COMPONENT_RDMA1]		= { MTK_DISP_RDMA,		1, &ddp_rdma },
	[DDP_COMPONENT_RDMA2]		= { MTK_DISP_RDMA,		2, &ddp_rdma },
	[DDP_COMPONENT_RDMA4]		= { MTK_DISP_RDMA,		4, &ddp_rdma },
	[DDP_COMPONENT_UFOE]		= { MTK_DISP_UFOE,		0, &ddp_ufoe },
	[DDP_COMPONENT_WDMA0]		= { MTK_DISP_WDMA,		0, NULL },
	[DDP_COMPONENT_WDMA1]		= { MTK_DISP_WDMA,		1, NULL },
};

static bool mtk_ddp_comp_find(struct device *dev,
			      const unsigned int *path,
			      unsigned int path_len,
			      struct mtk_ddp_comp *ddp_comp)
{
	unsigned int i;

	if (path == NULL)
		return false;

	for (i = 0U; i < path_len; i++)
		if (dev == ddp_comp[path[i]].dev)
			return true;

	return false;
}

static int mtk_ddp_comp_find_in_route(struct device *dev,
				      const struct mtk_drm_route *routes,
				      unsigned int num_routes,
				      struct mtk_ddp_comp *ddp_comp)
{
	unsigned int i;

	if (!routes)
		return -EINVAL;

	for (i = 0; i < num_routes; i++)
		if (dev == ddp_comp[routes[i].route_ddp].dev)
			return BIT(routes[i].crtc_id);

	return -ENODEV;
}

static bool mtk_ddp_path_available(const unsigned int *path,
				   unsigned int path_len,
				   struct device_node **comp_node)
{
	unsigned int i;

	if (!path || !path_len)
		return false;

	for (i = 0U; i < path_len; i++) {
		/* OVL_ADAPTOR doesn't have a device node */
		if (path[i] == DDP_COMPONENT_DRM_OVL_ADAPTOR)
			continue;

		if (!comp_node[path[i]])
			return false;
	}

	return true;
}

int mtk_ddp_comp_get_id(struct device_node *node,
			enum mtk_ddp_comp_type comp_type)
{
	int id = of_alias_get_id(node, mtk_ddp_comp_stem[comp_type]);
	int i;

	for (i = 0; i < ARRAY_SIZE(mtk_ddp_matches); i++) {
		if (comp_type == mtk_ddp_matches[i].type &&
		    (id < 0 || id == mtk_ddp_matches[i].alias_id))
			return i;
	}

	return -EINVAL;
}

int mtk_find_possible_crtcs(struct drm_device *drm, struct device *dev)
{
	struct mtk_drm_private *private = drm->dev_private;
	const struct mtk_mmsys_driver_data *data;
	struct mtk_drm_private *priv_n;
	int i = 0, j;
	int ret;

	for (j = 0; j < private->data->mmsys_dev_num; j++) {
		priv_n = private->all_drm_private[j];
		data = priv_n->data;

		if (mtk_ddp_path_available(data->main_path, data->main_len,
					   priv_n->comp_node)) {
			if (mtk_ddp_comp_find(dev, data->main_path,
					      data->main_len,
					      priv_n->ddp_comp))
				return BIT(i);
			i++;
		}

		if (mtk_ddp_path_available(data->ext_path, data->ext_len,
					   priv_n->comp_node)) {
			if (mtk_ddp_comp_find(dev, data->ext_path,
					      data->ext_len,
					      priv_n->ddp_comp))
				return BIT(i);
			i++;
		}

		if (mtk_ddp_path_available(data->third_path, data->third_len,
					   priv_n->comp_node)) {
			if (mtk_ddp_comp_find(dev, data->third_path,
					      data->third_len,
					      priv_n->ddp_comp))
				return BIT(i);
			i++;
		}
	}

	ret = mtk_ddp_comp_find_in_route(dev,
					 private->data->conn_routes,
					 private->data->num_conn_routes,
					 private->ddp_comp);

	if (ret < 0)
		DRM_INFO("Failed to find comp in ddp table, ret = %d\n", ret);

	return ret;
}

int mtk_ddp_comp_init(struct device_node *node, struct mtk_ddp_comp *comp,
		      unsigned int comp_id)
{
	struct platform_device *comp_pdev;
	enum mtk_ddp_comp_type type;
	struct mtk_ddp_comp_dev *priv;
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	int ret;
#endif

	if (comp_id >= DDP_COMPONENT_DRM_ID_MAX)
		return -EINVAL;

	type = mtk_ddp_matches[comp_id].type;

	comp->id = comp_id;
	comp->funcs = mtk_ddp_matches[comp_id].funcs;
	/* Not all drm components have a DTS device node, such as ovl_adaptor,
	 * which is the drm bring up sub driver
	 */
	if (!node)
		return 0;

	comp_pdev = of_find_device_by_node(node);
	if (!comp_pdev) {
		DRM_INFO("Waiting for device %s\n", node->full_name);
		return -EPROBE_DEFER;
	}
	comp->dev = &comp_pdev->dev;

	if (type == MTK_DISP_AAL ||
	    type == MTK_DISP_BLS ||
	    type == MTK_DISP_CCORR ||
	    type == MTK_DISP_COLOR ||
	    type == MTK_DISP_GAMMA ||
	    type == MTK_DISP_MERGE ||
	    type == MTK_DISP_OVL ||
	    type == MTK_DISP_OVL_2L ||
	    type == MTK_DISP_PWM ||
	    type == MTK_DISP_RDMA ||
	    type == MTK_DPI ||
	    type == MTK_DP_INTF ||
	    type == MTK_DSI)
		return 0;

	priv = devm_kzalloc(comp->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regs = of_iomap(node, 0);
	priv->clk = of_clk_get(node, 0);
	if (IS_ERR(priv->clk))
		return PTR_ERR(priv->clk);

	/*
	 * mt6895: LK initializes the whole display pipeline (panel + DSC
	 * encoder + DSI) and the kernel must preserve it (the stock driver
	 * never reprograms the DSC block - "Assume DSI0 enable already in
	 * LK"). Reprogramming DISP_DSC here desyncs the encoder from the
	 * panel's LK-programmed decoder (grey + purple dots).
	 */
	if (type == MTK_DISP_DSC && of_device_is_compatible(node,
				"mediatek,mt6895-disp-dsc"))
		priv->preserve_lk_dsc = true;

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	ret = cmdq_dev_get_client_reg(comp->dev, &priv->cmdq_reg, 0);
	if (ret)
		dev_dbg(comp->dev, "get mediatek,gce-client-reg fail!\n");
#endif

	platform_set_drvdata(comp_pdev, priv);

	return 0;
}
