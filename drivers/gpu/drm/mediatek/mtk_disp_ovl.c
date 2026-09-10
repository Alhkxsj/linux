// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 MediaTek Inc.
 */

#include <drm/drm_blend.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/soc/mediatek/mtk-cmdq.h>

#include "mtk_crtc.h"

void xaga_pq_config(void);
void xaga_measure_fps(void);
#include "mtk_ddp_comp.h"
#include "mtk_disp_drv.h"
#include "mtk_drm_drv.h"
#include "mtk_gem.h"

static bool xaga_ovl1_2l_done;

#define DISP_REG_OVL_INTEN			0x0004
#define OVL_FME_CPL_INT					BIT(1)
#define DISP_REG_OVL_INTSTA			0x0008
#define DISP_REG_OVL_EN				0x000c
#define DISP_REG_OVL_RST			0x0014
#define DISP_REG_OVL_ROI_SIZE			0x0020
#define DISP_REG_OVL_DATAPATH_CON		0x0024
#define OVL_LAYER_SMI_ID_EN				BIT(0)
#define OVL_BGCLR_SEL_IN				BIT(2)
#define OVL_LAYER_AFBC_EN(n)				BIT(4+n)
#define DISP_REG_OVL_ROI_BGCLR			0x0028
#define DISP_REG_OVL_SRC_CON			0x002c
#define DISP_OVL_FORCE_RELAY_MODE		BIT(8)
#define DISP_REG_OVL_CON(n)			(0x0030 + 0x20 * (n))
#define DISP_REG_OVL_SRC_SIZE(n)		(0x0038 + 0x20 * (n))
#define DISP_REG_OVL_OFFSET(n)			(0x003c + 0x20 * (n))
#define DISP_REG_OVL_PITCH_MSB(n)		(0x0040 + 0x20 * (n))
#define OVL_PITCH_MSB_2ND_SUBBUF			BIT(16)
#define DISP_REG_OVL_PITCH(n)			(0x0044 + 0x20 * (n))
#define OVL_CONST_BLEND					BIT(28)
#define DISP_REG_OVL_RDMA_CTRL(n)		(0x00c0 + 0x20 * (n))
#define DISP_REG_OVL_RDMA_GMC(n)		(0x00c8 + 0x20 * (n))
#define DISP_REG_OVL_ADDR_MT2701		0x0040
#define DISP_REG_OVL_CLRFMT_EXT			0x02d0
#define OVL_CON_CLRFMT_BIT_DEPTH_MASK(n)		(GENMASK(1, 0) << (4 * (n)))
#define OVL_CON_CLRFMT_BIT_DEPTH(depth, n)		((depth) << (4 * (n)))
#define OVL_CON_CLRFMT_8_BIT				(0)
#define OVL_CON_CLRFMT_10_BIT				(1)
#define DISP_REG_OVL_ADDR_MT8173		0x0f40
#define DISP_REG_OVL_ADDR(ovl, n)		((ovl)->data->addr + 0x20 * (n))
#define DISP_REG_OVL_HDR_ADDR(ovl, n)		((ovl)->data->addr + 0x20 * (n) + 0x04)
#define DISP_REG_OVL_HDR_PITCH(ovl, n)		((ovl)->data->addr + 0x20 * (n) + 0x08)

#define GMC_THRESHOLD_BITS	16
#define GMC_THRESHOLD_HIGH	((1 << GMC_THRESHOLD_BITS) / 4)
#define GMC_THRESHOLD_LOW	((1 << GMC_THRESHOLD_BITS) / 8)

#define OVL_CON_CLRFMT_MAN	BIT(23)
#define OVL_CON_BYTE_SWAP	BIT(24)

/* OVL_CON_RGB_SWAP works only if OVL_CON_CLRFMT_MAN is enabled */
#define OVL_CON_RGB_SWAP	BIT(25)

#define OVL_CON_CLRFMT_RGB	(1 << 12)
#define OVL_CON_CLRFMT_ARGB8888	(2 << 12)
#define OVL_CON_CLRFMT_RGBA8888	(3 << 12)
#define OVL_CON_CLRFMT_ABGR8888	(OVL_CON_CLRFMT_ARGB8888 | OVL_CON_BYTE_SWAP)
#define OVL_CON_CLRFMT_BGRA8888	(OVL_CON_CLRFMT_RGBA8888 | OVL_CON_BYTE_SWAP)
#define OVL_CON_CLRFMT_UYVY	(4 << 12)
#define OVL_CON_CLRFMT_YUYV	(5 << 12)
#define OVL_CON_MTX_YUV_TO_RGB	(6 << 16)
#define OVL_CON_CLRFMT_PARGB8888 ((3 << 12) | OVL_CON_CLRFMT_MAN)
#define OVL_CON_CLRFMT_PABGR8888 (OVL_CON_CLRFMT_PARGB8888 | OVL_CON_RGB_SWAP)
#define OVL_CON_CLRFMT_PBGRA8888 (OVL_CON_CLRFMT_PARGB8888 | OVL_CON_BYTE_SWAP)
#define OVL_CON_CLRFMT_PRGBA8888 (OVL_CON_CLRFMT_PABGR8888 | OVL_CON_BYTE_SWAP)
#define OVL_CON_CLRFMT_RGB565(ovl)	((ovl)->data->fmt_rgb565_is_0 ? \
					0 : OVL_CON_CLRFMT_RGB)
#define OVL_CON_CLRFMT_RGB888(ovl)	((ovl)->data->fmt_rgb565_is_0 ? \
					OVL_CON_CLRFMT_RGB : 0)
#define	OVL_CON_AEN		BIT(8)
#define	OVL_CON_ALPHA		0xff
#define	OVL_CON_VIRT_FLIP	BIT(9)
#define	OVL_CON_HORZ_FLIP	BIT(10)

#define OVL_COLOR_ALPHA		GENMASK(31, 24)

static inline bool is_10bit_rgb(u32 fmt)
{
	switch (fmt) {
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_RGBX1010102:
	case DRM_FORMAT_RGBA1010102:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010:
	case DRM_FORMAT_BGRX1010102:
	case DRM_FORMAT_BGRA1010102:
		return true;
	}
	return false;
}

static const u32 mt8173_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_BGRX8888,
	DRM_FORMAT_BGRA8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_YUYV,
};

static const u32 mt8195_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XRGB2101010,
	DRM_FORMAT_ARGB2101010,
	DRM_FORMAT_BGRX8888,
	DRM_FORMAT_BGRA8888,
	DRM_FORMAT_BGRX1010102,
	DRM_FORMAT_BGRA1010102,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_XBGR2101010,
	DRM_FORMAT_ABGR2101010,
	DRM_FORMAT_RGBX8888,
	DRM_FORMAT_RGBA8888,
	DRM_FORMAT_RGBX1010102,
	DRM_FORMAT_RGBA1010102,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_YUYV,
};

struct mtk_disp_ovl_data {
	unsigned int addr;
	unsigned int gmc_bits;
	unsigned int layer_nr;
	bool fmt_rgb565_is_0;
	bool smi_id_en;
	bool supports_afbc;
	const u32 blend_modes;
	const u32 *formats;
	size_t num_formats;
	bool supports_clrfmt_ext;
	/* XAGA dual-pipe: mt6895 main display = 2 x 540-wide OVLs (OVL1_2L +
	 * OVL0) feeding a 2-slice DSC. When set, the layer config is mirrored
	 * onto the second OVL and each OVL gets half the plane width.
	 */
	bool is_dual_pipe;
	unsigned int pipe2_base;
};

/*
 * struct mtk_disp_ovl - DISP_OVL driver structure
 * @crtc: associated crtc to report vblank events to
 * @data: platform data
 */
struct mtk_disp_ovl {
	struct drm_crtc			*crtc;
	struct clk			*clk;
	void __iomem			*regs;
	void __iomem			*pipe2_regs;
	struct cmdq_client_reg		cmdq_reg;
	const struct mtk_disp_ovl_data	*data;
	void				(*vblank_cb)(void *data);
	void				*vblank_cb_data;
};

static irqreturn_t mtk_disp_ovl_irq_handler(int irq, void *dev_id)
{
	struct mtk_disp_ovl *priv = dev_id;

	/* Clear frame completion interrupt */
	writel(0x0, priv->regs + DISP_REG_OVL_INTSTA);

	if (!priv->vblank_cb)
		return IRQ_NONE;

	priv->vblank_cb(priv->vblank_cb_data);

	return IRQ_HANDLED;
}

void mtk_ovl_register_vblank_cb(struct device *dev,
				void (*vblank_cb)(void *),
				void *vblank_cb_data)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	ovl->vblank_cb = vblank_cb;
	ovl->vblank_cb_data = vblank_cb_data;
}

void mtk_ovl_unregister_vblank_cb(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	ovl->vblank_cb = NULL;
	ovl->vblank_cb_data = NULL;
}

void mtk_ovl_enable_vblank(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	writel(0x0, ovl->regs + DISP_REG_OVL_INTSTA);
	writel_relaxed(OVL_FME_CPL_INT, ovl->regs + DISP_REG_OVL_INTEN);
}

void mtk_ovl_disable_vblank(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	writel_relaxed(0x0, ovl->regs + DISP_REG_OVL_INTEN);
}

u32 mtk_ovl_get_blend_modes(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	return ovl->data->blend_modes;
}

const u32 *mtk_ovl_get_formats(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	return ovl->data->formats;
}

size_t mtk_ovl_get_num_formats(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	return ovl->data->num_formats;
}

bool mtk_ovl_is_afbc_supported(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	return ovl->data->supports_afbc;
}

int mtk_ovl_clk_enable(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	return clk_prepare_enable(ovl->clk);
}

void mtk_ovl_clk_disable(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	clk_disable_unprepare(ovl->clk);
}

void mtk_ovl_start(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);
	unsigned int reg;

	if (ovl->data->smi_id_en) {
		reg = readl(ovl->regs + DISP_REG_OVL_DATAPATH_CON);
		reg = reg | OVL_LAYER_SMI_ID_EN;
		writel_relaxed(reg, ovl->regs + DISP_REG_OVL_DATAPATH_CON);
	}
	/* XAGA: RMW bit0 only, preserve LK's EN bits (BYPASS_SHADOW etc.) */
	reg = readl(ovl->regs + DISP_REG_OVL_EN);
	writel_relaxed(reg | 0x1, ovl->regs + DISP_REG_OVL_EN);
	if (ovl->data->is_dual_pipe && ovl->pipe2_regs) {
		reg = readl(ovl->pipe2_regs + DISP_REG_OVL_EN);
		writel_relaxed(reg | 0x1, ovl->pipe2_regs + DISP_REG_OVL_EN);
	}
	/*
	 * XAGA: OVL0 is the RELAY pipe - it passes OVL1_2L's output through
	 * (its DATAPATH has BGCLR_IN_SEL bit2) so the DSC gets a complete
	 * 1080 stream. The working Tianma recovery state has OVL0
	 * SRC_CON=0x100 (FORCE_RELAY_MODE, NO layer bits). LK sometimes
	 * leaves bit1 (L1 sourced) set from its logo config -> OVL0 fetches
	 * L1 instead of relaying -> left slice black. Write 0x100 ABSOLUTE
	 * (clear layer bits, set relay) to match the working state in every
	 * boot. Verified: recovery OVL0 SRC_CON=0x00000100.
	 */
	if (ovl->data->is_dual_pipe) {
		writel_relaxed(DISP_OVL_FORCE_RELAY_MODE,
			       ovl->regs + DISP_REG_OVL_SRC_CON);
	}
}

void mtk_ovl_stop(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);
	unsigned int reg;

	/* XAGA: RMW bit0 only, preserve LK's EN bits */
	reg = readl(ovl->regs + DISP_REG_OVL_EN);
	writel_relaxed(reg & ~0x1, ovl->regs + DISP_REG_OVL_EN);
	if (ovl->data->is_dual_pipe && ovl->pipe2_regs) {
		reg = readl(ovl->pipe2_regs + DISP_REG_OVL_EN);
		writel_relaxed(reg & ~0x1, ovl->pipe2_regs + DISP_REG_OVL_EN);
	}
	if (ovl->data->smi_id_en) {
		reg = readl(ovl->regs + DISP_REG_OVL_DATAPATH_CON);
		reg = reg & ~OVL_LAYER_SMI_ID_EN;
		writel_relaxed(reg, ovl->regs + DISP_REG_OVL_DATAPATH_CON);
	}
}

static void mtk_ovl_set_afbc(struct mtk_disp_ovl *ovl, struct cmdq_pkt *cmdq_pkt,
			     int idx, bool enabled)
{
	mtk_ddp_write_mask(cmdq_pkt, enabled ? OVL_LAYER_AFBC_EN(idx) : 0,
			   &ovl->cmdq_reg, ovl->regs,
			   DISP_REG_OVL_DATAPATH_CON, OVL_LAYER_AFBC_EN(idx));
}

static void mtk_ovl_set_bit_depth(struct device *dev, int idx, u32 format,
				  struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);
	unsigned int bit_depth = OVL_CON_CLRFMT_8_BIT;

	if (!ovl->data->supports_clrfmt_ext)
		return;

	if (is_10bit_rgb(format))
		bit_depth = OVL_CON_CLRFMT_10_BIT;

	mtk_ddp_write_mask(cmdq_pkt, OVL_CON_CLRFMT_BIT_DEPTH(bit_depth, idx),
			   &ovl->cmdq_reg, ovl->regs, DISP_REG_OVL_CLRFMT_EXT,
			   OVL_CON_CLRFMT_BIT_DEPTH_MASK(idx));
}

void mtk_ovl_config(struct device *dev, unsigned int w,
		    unsigned int h, unsigned int vrefresh,
		    unsigned int bpc, struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	/*
	 * XAGA: do NOT rewrite OVL_ROI_SIZE. LK left it at 540 wide (DSC
	 * slice width) - the working device shows exactly this (OVL
	 * ROI=0x099c021c at ddp_prepare). Writing the DRM mode's full 1080
	 * here mismatches the DSC 540-slice pipeline -> grey test-pattern.
	 * Keep the background + reset but leave ROI at LK's value.
	 */

	/*
	 * The background color must be opaque black (ARGB),
	 * otherwise the alpha blending will have no effect
	 */
	mtk_ddp_write_relaxed(cmdq_pkt, OVL_COLOR_ALPHA, &ovl->cmdq_reg,
			      ovl->regs, DISP_REG_OVL_ROI_BGCLR);

	mtk_ddp_write(cmdq_pkt, 0x1, &ovl->cmdq_reg, ovl->regs, DISP_REG_OVL_RST);
	mtk_ddp_write(cmdq_pkt, 0x0, &ovl->cmdq_reg, ovl->regs, DISP_REG_OVL_RST);

	if (ovl->data->is_dual_pipe && ovl->pipe2_regs) {
		writel_relaxed(OVL_COLOR_ALPHA,
			       ovl->pipe2_regs + DISP_REG_OVL_ROI_BGCLR);
		writel_relaxed(0x1, ovl->pipe2_regs + DISP_REG_OVL_RST);
		writel_relaxed(0x0, ovl->pipe2_regs + DISP_REG_OVL_RST);
	}
}

unsigned int mtk_ovl_layer_nr(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	return ovl->data->layer_nr;
}

unsigned int mtk_ovl_supported_rotations(struct device *dev)
{
	return DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_180 |
	       DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y;
}

int mtk_ovl_layer_check(struct device *dev, unsigned int idx,
			struct mtk_plane_state *mtk_state)
{
	struct drm_plane_state *state = &mtk_state->base;

	/* check if any unsupported rotation is set */
	if (state->rotation & ~mtk_ovl_supported_rotations(dev))
		return -EINVAL;

	/*
	 * TODO: Rotating/reflecting YUV buffers is not supported at this time.
	 *	 Only RGB[AX] variants are supported.
	 *	 Since DRM_MODE_ROTATE_0 means "no rotation", we should not
	 *	 reject layers with this property.
	 */
	if (state->fb->format->is_yuv && (state->rotation & ~DRM_MODE_ROTATE_0))
		return -EINVAL;

	return 0;
}

static void mtk_ovl_pipe_layer_on(struct mtk_disp_ovl *ovl, void __iomem *regs,
				  unsigned int idx, struct cmdq_pkt *cmdq_pkt)
{
	unsigned int gmc_thrshd_l;
	unsigned int gmc_thrshd_h;
	unsigned int gmc_value;
	unsigned int reg;

	/* XAGA: downstream mtk_ovl_layer_on writes RDMA_CTRL = 0x1 (full
	 * write, mask ~0). The 0xc20001-style values in the recovery dumps
	 * are LK's leftovers before the kernel ever configured a layer;
	 * once the downstream kernel programs the layer, the value IS 0x1.
	 * Match that exactly. */
	writel_relaxed(0x1, regs + DISP_REG_OVL_RDMA_CTRL(idx));
	gmc_thrshd_l = GMC_THRESHOLD_LOW >>
		      (GMC_THRESHOLD_BITS - ovl->data->gmc_bits);
	gmc_thrshd_h = GMC_THRESHOLD_HIGH >>
		      (GMC_THRESHOLD_BITS - ovl->data->gmc_bits);
	if (ovl->data->gmc_bits == 10)
		gmc_value = gmc_thrshd_h | gmc_thrshd_h << 16;
	else
		gmc_value = gmc_thrshd_l | gmc_thrshd_l << 8 |
			    gmc_thrshd_h << 16 | gmc_thrshd_h << 24;
	writel_relaxed(gmc_value, regs + DISP_REG_OVL_RDMA_GMC(idx));
	reg = readl(regs + DISP_REG_OVL_SRC_CON);
	writel_relaxed(reg | BIT(idx), regs + DISP_REG_OVL_SRC_CON);
}

void mtk_ovl_layer_on(struct device *dev, unsigned int idx,
		      struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);
	unsigned int gmc_thrshd_l;
	unsigned int gmc_thrshd_h;
	unsigned int gmc_value;

	mtk_ddp_write(cmdq_pkt, 0x1, &ovl->cmdq_reg, ovl->regs,
		      DISP_REG_OVL_RDMA_CTRL(idx));
	gmc_thrshd_l = GMC_THRESHOLD_LOW >>
		      (GMC_THRESHOLD_BITS - ovl->data->gmc_bits);
	gmc_thrshd_h = GMC_THRESHOLD_HIGH >>
		      (GMC_THRESHOLD_BITS - ovl->data->gmc_bits);
	if (ovl->data->gmc_bits == 10)
		gmc_value = gmc_thrshd_h | gmc_thrshd_h << 16;
	else
		gmc_value = gmc_thrshd_l | gmc_thrshd_l << 8 |
			    gmc_thrshd_h << 16 | gmc_thrshd_h << 24;
	mtk_ddp_write(cmdq_pkt, gmc_value,
		      &ovl->cmdq_reg, ovl->regs, DISP_REG_OVL_RDMA_GMC(idx));
	mtk_ddp_write_mask(cmdq_pkt, BIT(idx), &ovl->cmdq_reg, ovl->regs,
			   DISP_REG_OVL_SRC_CON, BIT(idx));
}

static void mtk_ovl_pipe_layer_off(void __iomem *regs, unsigned int idx)
{
	unsigned int reg;

	reg = readl(regs + DISP_REG_OVL_SRC_CON);
	writel_relaxed(reg & ~BIT(idx), regs + DISP_REG_OVL_SRC_CON);
	/* XAGA: downstream mtk_ovl_layer_off writes RDMA_CTRL = 0 (full). */
	writel_relaxed(0x0, regs + DISP_REG_OVL_RDMA_CTRL(idx));
}

void mtk_ovl_layer_off(struct device *dev, unsigned int idx,
		       struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);

	mtk_ddp_write_mask(cmdq_pkt, 0, &ovl->cmdq_reg, ovl->regs,
			   DISP_REG_OVL_SRC_CON, BIT(idx));
	mtk_ddp_write(cmdq_pkt, 0, &ovl->cmdq_reg, ovl->regs,
		      DISP_REG_OVL_RDMA_CTRL(idx));
}

static unsigned int mtk_ovl_fmt_convert(struct mtk_disp_ovl *ovl,
					struct mtk_plane_state *state)
{
	unsigned int fmt = state->pending.format;
	unsigned int blend_mode = DRM_MODE_BLEND_COVERAGE;

	/*
	 * For the platforms where OVL_CON_CLRFMT_MAN is defined in the hardware data sheet
	 * and supports premultiplied color formats, such as OVL_CON_CLRFMT_PARGB8888.
	 *
	 * Check blend_modes in the driver data to see if premultiplied mode is supported.
	 * If not, use coverage mode instead to set it to the supported color formats.
	 *
	 * Current DRM assumption is that alpha is default premultiplied, so the bitmask of
	 * blend_modes must include BIT(DRM_MODE_BLEND_PREMULTI). Otherwise, mtk_plane_init()
	 * will get an error return from drm_plane_create_blend_mode_property() and
	 * state->base.pixel_blend_mode should not be used.
	 */
	if (ovl->data->blend_modes & BIT(DRM_MODE_BLEND_PREMULTI))
		blend_mode = state->base.pixel_blend_mode;

	switch (fmt) {
	default:
	case DRM_FORMAT_RGB565:
		return OVL_CON_CLRFMT_RGB565(ovl);
	case DRM_FORMAT_BGR565:
		return OVL_CON_CLRFMT_RGB565(ovl) | OVL_CON_BYTE_SWAP;
	case DRM_FORMAT_RGB888:
		return OVL_CON_CLRFMT_RGB888(ovl);
	case DRM_FORMAT_BGR888:
		return OVL_CON_CLRFMT_RGB888(ovl) | OVL_CON_BYTE_SWAP;
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
	case DRM_FORMAT_RGBX1010102:
	case DRM_FORMAT_RGBA1010102:
		return blend_mode == DRM_MODE_BLEND_COVERAGE ?
		       OVL_CON_CLRFMT_RGBA8888 :
		       OVL_CON_CLRFMT_PRGBA8888;
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_BGRA8888:
	case DRM_FORMAT_BGRX1010102:
	case DRM_FORMAT_BGRA1010102:
		return blend_mode == DRM_MODE_BLEND_COVERAGE ?
		       OVL_CON_CLRFMT_BGRA8888 :
		       OVL_CON_CLRFMT_PBGRA8888;
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
		return blend_mode == DRM_MODE_BLEND_COVERAGE ?
		       OVL_CON_CLRFMT_ARGB8888 :
		       OVL_CON_CLRFMT_PARGB8888;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010:
		return blend_mode == DRM_MODE_BLEND_COVERAGE ?
		       OVL_CON_CLRFMT_ABGR8888 :
		       OVL_CON_CLRFMT_PABGR8888;
	case DRM_FORMAT_UYVY:
		return OVL_CON_CLRFMT_UYVY | OVL_CON_MTX_YUV_TO_RGB;
	case DRM_FORMAT_YUYV:
		return OVL_CON_CLRFMT_YUYV | OVL_CON_MTX_YUV_TO_RGB;
	}
}

static void mtk_ovl_afbc_layer_config(struct mtk_disp_ovl *ovl,
				      unsigned int idx,
				      struct mtk_plane_pending_state *pending,
				      struct cmdq_pkt *cmdq_pkt)
{
	unsigned int pitch_msb = pending->pitch >> 16;
	unsigned int hdr_pitch = pending->hdr_pitch;
	unsigned int hdr_addr = pending->hdr_addr;

	if (pending->modifier != DRM_FORMAT_MOD_LINEAR) {
		mtk_ddp_write_relaxed(cmdq_pkt, hdr_addr, &ovl->cmdq_reg, ovl->regs,
				      DISP_REG_OVL_HDR_ADDR(ovl, idx));
		mtk_ddp_write_relaxed(cmdq_pkt,
				      OVL_PITCH_MSB_2ND_SUBBUF | pitch_msb,
				      &ovl->cmdq_reg, ovl->regs, DISP_REG_OVL_PITCH_MSB(idx));
		mtk_ddp_write_relaxed(cmdq_pkt, hdr_pitch, &ovl->cmdq_reg, ovl->regs,
				      DISP_REG_OVL_HDR_PITCH(ovl, idx));
	} else {
		mtk_ddp_write_relaxed(cmdq_pkt, pitch_msb,
				      &ovl->cmdq_reg, ovl->regs, DISP_REG_OVL_PITCH_MSB(idx));
	}
}

void mtk_ovl_layer_config(struct device *dev, unsigned int idx,
			  struct mtk_plane_state *state,
			  struct cmdq_pkt *cmdq_pkt)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);
	struct mtk_plane_pending_state *pending = &state->pending;
	unsigned int addr = pending->addr;
	unsigned int fmt = pending->format;
	unsigned int offset = (pending->y << 16) | pending->x;

	/*
	 * XAGA: OVL1_2L (pipe2) is the single content pipe; OVL0 stays a
	 * pure relay (never programmed, keeps FORCE_RELAY_MODE). The working
	 * Tianma recovery state has OVL1_2L fully configured (RDMA_CTRL
	 * incl. memory-fetch/GMC bits, PITCH, SRC_SIZE, ADDR). Program it
	 * here with our fbcon buffer so the OVL actually fetches memory.
	 */
	if (!pending->enable) {
		mtk_ovl_layer_off(dev, idx, cmdq_pkt);
		if (ovl->data->is_dual_pipe && ovl->pipe2_regs)
			mtk_ovl_pipe_layer_off(ovl->pipe2_regs, idx);
		return;
	}

	if (ovl->data->supports_clrfmt_ext)
		mtk_ovl_set_bit_depth(dev, idx, fmt, cmdq_pkt);

	if (ovl->data->is_dual_pipe && ovl->pipe2_regs) {
		unsigned int half_w = pending->width / 2;

		if (!half_w)
			half_w = pending->width;
		/*
		 * XAGA: configure OVL1_2L only ONCE. The fbcon buffer is
		 * fixed (0xfb300000, same pitch/format), so re-writing the
		 * layer on every DRM commit mid-frame glitches the frame
		 * boundary -> DSC ABN_EOF latches -> panel grey test pattern.
		 * The working recovery kernel never re-touches OVL1_2L after
		 * LK set it up. (DSC confirmed clean right after the first
		 * config, ABN_EOF latches during later commits.)
		 */
		if (!xaga_ovl1_2l_done) {
			/* XAGA: OVL1_2L reads the RESERVED framebuffer region
			 * (0xff5c3000) - the only memory the OVL's m4u (LK's
			 * page tables) can fetch. PITCH=0x1100 (4352): the
			 * stride LK's logo buffer uses (4320+32, 64-byte
			 * aligned). The OVL misreads rows with 0x10e0 ->
			 * deterministic striping. The fbcon GEM (4320 stride)
			 * is BLITTED into a 4352-stride layout below. */
			writel_relaxed(0x21ff, ovl->pipe2_regs + DISP_REG_OVL_CON(idx));
			writel_relaxed(0x1100,
				       ovl->pipe2_regs + DISP_REG_OVL_PITCH(idx));
			writel_relaxed((pending->height << 16) | half_w,
				       ovl->pipe2_regs + DISP_REG_OVL_SRC_SIZE(idx));
			writel_relaxed(offset, ovl->pipe2_regs + DISP_REG_OVL_OFFSET(idx));
			writel_relaxed(0xff5c3000,
				       ovl->pipe2_regs + DISP_REG_OVL_ADDR(ovl, idx));
			mtk_ovl_pipe_layer_on(ovl, ovl->pipe2_regs, idx, cmdq_pkt);
			pr_info("XAGA-OVL: pipe2 layer -> 0xff5c3000 (blit target) PITCH=0x10e0\n");
			xaga_ovl1_2l_done = true;
		}

		/* XAGA double-buffered blit: copy the fbcon GEM (pitch 4320)
		 * ROW BY ROW into a 4352-stride layout (the OVL's expected
		 * stride) in the buffer the OVL is NOT scanning, then flip
		 * the OVL ADDR (a single register write). Buffers:
		 *   A = 0xff5c3000, B = 0xfeb83000 (same reserved mblock,
		 *   m4u-fetchable; B+4352*2460 ends just before A). */
		if (state->base.fb && state->base.fb->obj[0]) {
			struct mtk_gem_obj *g =
				to_mtk_gem_obj(state->base.fb->obj[0]);
			static void __iomem *xaga_buf[2];
			static const u32 xaga_addr[2] = { 0xff5c3000, 0xfeb83000 };
			static int xaga_cur;
			static unsigned int xaga_blit_count;
			int y;

			if (!xaga_buf[0]) {
				/* WC (write-combining, non-cached): the OVL1_2L
				 * reads this via DMA/m4u, so a WB mapping would
				 * leave dirty cache lines the OVL never sees ->
				 * stale/garbage content. */
				xaga_buf[0] = memremap(xaga_addr[0], 0xa3c000,
						       MEMREMAP_WC);
				xaga_buf[1] = memremap(xaga_addr[1], 0xa3c000,
						       MEMREMAP_WC);
			}
			if (xaga_buf[0] && xaga_buf[1] && g->cookie) {
				int next = xaga_cur ^ 1;
				void __iomem *dst = xaga_buf[next];
				u8 *src = g->cookie;

				for (y = 0; y < 2460; y++)
					memcpy_toio(dst + y * 4352,
						    src + y * 4320, 4320);
				/* make the blit visible to the OVL before the
				 * ADDR flip (wmb = dsb(st) flushes the WC buf) */
				wmb();
				writel_relaxed(xaga_addr[next],
					       ovl->pipe2_regs +
					       DISP_REG_OVL_ADDR(ovl, idx));
				xaga_cur = next;
				if ((xaga_blit_count++ & 0xff) == 0)
					pr_info("XAGA-OVL blit: %u frames, cur=0x%08x\n",
						xaga_blit_count,
						xaga_addr[xaga_cur]);
			}
		}

		/* XAGA: re-drive the PQ chain (left slice) on every commit so it
		 * doesn't freeze. */
		xaga_pq_config();

		/* XAGA diag: report DSC state every commit to catch when
		 * ABN_EOF re-latches */
		{
			void __iomem *dsc = ioremap(0x14015000, 0x1000);
			void __iomem *ovl12 = ioremap(0x14004000, 0x1000);
			void __iomem *rd1 = ioremap(0x1401c000, 0x1000);

			if (dsc) {
				u32 st = readl(dsc + 0x8);

				pr_info("XAGA-DSC commit: CON=0x%08x INTSTA=0x%08x\n",
					readl(dsc + 0x0), st);
				/* clear via INTACK so the NEXT commit shows a fresh
				 * value: distinguishes ongoing ABN_EOF from a one-time
				 * startup latch */
				writel_relaxed(st, dsc + 0xc);
				writel_relaxed(0x0, dsc + 0xc);
				iounmap(dsc);
			}
			if (ovl12) {
				/* L0 RDMA_CTRL bits16:27 = FIFO_USED_SZ (live fetch
				 * level). 0 = not fetching, ~0x100+ = actively
				 * draining (downstream kms_init_done shows 0x01040001).
				 * INTSTA bit2=FME_UND, bits5:8=RDMA EOF_ABNORMAL,
				 * bit9=RDMA0_SMI_UNDERFLOW. Clear via W1C (~val, as
				 * downstream) so the next commit shows a fresh value -
				 * tells us if SMI underflow is ongoing or one-time. */
				u32 st = readl(ovl12 + 0x08);

				pr_info("XAGA-OVL12 commit: EN=0x%08x INTSTA=0x%08x L0_RDMA_CTRL=0x%08x L0_ADDR=0x%08x\n",
					readl(ovl12 + 0x0c), st,
					readl(ovl12 + 0xc0), readl(ovl12 + 0xf40));
				writel_relaxed(~st, ovl12 + 0x08);
				iounmap(ovl12);
			}
			if (rd1) {
				pr_info("XAGA-RDMA1 commit: GLOBAL=0x%08x SIZE0=0x%08x SIZE1=0x%08x FIFO=0x%08x\n",
					readl(rd1 + 0x10), readl(rd1 + 0x14),
					readl(rd1 + 0x18), readl(rd1 + 0x40));
				iounmap(rd1);
			}
		}
	}
}

void mtk_ovl_bgclr_in_on(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);
	unsigned int reg;

	reg = readl(ovl->regs + DISP_REG_OVL_DATAPATH_CON);
	reg = reg | OVL_BGCLR_SEL_IN;
	writel(reg, ovl->regs + DISP_REG_OVL_DATAPATH_CON);
}

void mtk_ovl_bgclr_in_off(struct device *dev)
{
	struct mtk_disp_ovl *ovl = dev_get_drvdata(dev);
	unsigned int reg;

	reg = readl(ovl->regs + DISP_REG_OVL_DATAPATH_CON);
	reg = reg & ~OVL_BGCLR_SEL_IN;
	writel(reg, ovl->regs + DISP_REG_OVL_DATAPATH_CON);
}

static int mtk_disp_ovl_bind(struct device *dev, struct device *master,
			     void *data)
{
	return 0;
}

static void mtk_disp_ovl_unbind(struct device *dev, struct device *master,
				void *data)
{
}

static const struct component_ops mtk_disp_ovl_component_ops = {
	.bind	= mtk_disp_ovl_bind,
	.unbind = mtk_disp_ovl_unbind,
};

static int mtk_disp_ovl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_disp_ovl *priv;
	int irq;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	priv->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(priv->clk))
		return dev_err_probe(dev, PTR_ERR(priv->clk),
				     "failed to get ovl clk\n");

	priv->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->regs))
		return dev_err_probe(dev, PTR_ERR(priv->regs),
				     "failed to ioremap ovl\n");
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	ret = cmdq_dev_get_client_reg(dev, &priv->cmdq_reg, 0);
	if (ret)
		dev_dbg(dev, "get mediatek,gce-client-reg fail!\n");
#endif

	priv->data = of_device_get_match_data(dev);
	platform_set_drvdata(pdev, priv);

	if (priv->data->is_dual_pipe) {
		priv->pipe2_regs = devm_ioremap(dev, priv->data->pipe2_base, SZ_4K);
		if (!priv->pipe2_regs)
			return -ENOMEM;
		dev_info(dev, "XAGA dual-pipe: second OVL at 0x%x mapped to %p\n",
			 priv->data->pipe2_base, priv->pipe2_regs);
	}

	ret = devm_request_irq(dev, irq, mtk_disp_ovl_irq_handler,
			       IRQF_TRIGGER_NONE, dev_name(dev), priv);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to request irq %d\n", irq);

	pm_runtime_enable(dev);

	ret = component_add(dev, &mtk_disp_ovl_component_ops);
	if (ret) {
		pm_runtime_disable(dev);
		return dev_err_probe(dev, ret, "Failed to add component\n");
	}

	return 0;
}

static void mtk_disp_ovl_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &mtk_disp_ovl_component_ops);
	pm_runtime_disable(&pdev->dev);
}

static const struct mtk_disp_ovl_data mt2701_ovl_driver_data = {
	.addr = DISP_REG_OVL_ADDR_MT2701,
	.gmc_bits = 8,
	.layer_nr = 4,
	.fmt_rgb565_is_0 = false,
	.formats = mt8173_formats,
	.num_formats = ARRAY_SIZE(mt8173_formats),
};

static const struct mtk_disp_ovl_data mt8173_ovl_driver_data = {
	.addr = DISP_REG_OVL_ADDR_MT8173,
	.gmc_bits = 8,
	.layer_nr = 4,
	.fmt_rgb565_is_0 = true,
	.formats = mt8173_formats,
	.num_formats = ARRAY_SIZE(mt8173_formats),
};

static const struct mtk_disp_ovl_data mt8183_ovl_driver_data = {
	.addr = DISP_REG_OVL_ADDR_MT8173,
	.gmc_bits = 10,
	.layer_nr = 4,
	.fmt_rgb565_is_0 = true,
	.formats = mt8173_formats,
	.num_formats = ARRAY_SIZE(mt8173_formats),
};

static const struct mtk_disp_ovl_data mt8183_ovl_2l_driver_data = {
	.addr = DISP_REG_OVL_ADDR_MT8173,
	.gmc_bits = 10,
	.layer_nr = 2,
	.fmt_rgb565_is_0 = true,
	.formats = mt8173_formats,
	.num_formats = ARRAY_SIZE(mt8173_formats),
};

/*
 * XAGA: mt6895 OVL0 is the right pipe of a DUAL-PIPE display (2x540-wide
 * OVLs feeding a 2-slice DSC). The left pipe is OVL1_2L at 0x14004000.
 * The layer config is mirrored onto it; each OVL gets half the plane width.
 */
static const struct mtk_disp_ovl_data mt6895_ovl_driver_data = {
	.addr = DISP_REG_OVL_ADDR_MT8173,
	.gmc_bits = 10,
	.layer_nr = 4,
	.fmt_rgb565_is_0 = true,
	.smi_id_en = true,
	.supports_afbc = true,
	.blend_modes = BIT(DRM_MODE_BLEND_PREMULTI) |
		       BIT(DRM_MODE_BLEND_COVERAGE) |
		       BIT(DRM_MODE_BLEND_PIXEL_NONE),
	.formats = mt8195_formats,
	.num_formats = ARRAY_SIZE(mt8195_formats),
	.supports_clrfmt_ext = true,
	.is_dual_pipe = true,
	.pipe2_base = 0x14004000,
};

static const struct mtk_disp_ovl_data mt8192_ovl_driver_data = {
	.addr = DISP_REG_OVL_ADDR_MT8173,
	.gmc_bits = 10,
	.layer_nr = 4,
	.fmt_rgb565_is_0 = true,
	.smi_id_en = true,
	.blend_modes = BIT(DRM_MODE_BLEND_PREMULTI) |
		       BIT(DRM_MODE_BLEND_COVERAGE) |
		       BIT(DRM_MODE_BLEND_PIXEL_NONE),
	.formats = mt8173_formats,
	.num_formats = ARRAY_SIZE(mt8173_formats),
};

static const struct mtk_disp_ovl_data mt8192_ovl_2l_driver_data = {
	.addr = DISP_REG_OVL_ADDR_MT8173,
	.gmc_bits = 10,
	.layer_nr = 2,
	.fmt_rgb565_is_0 = true,
	.smi_id_en = true,
	.blend_modes = BIT(DRM_MODE_BLEND_PREMULTI) |
		       BIT(DRM_MODE_BLEND_COVERAGE) |
		       BIT(DRM_MODE_BLEND_PIXEL_NONE),
	.formats = mt8173_formats,
	.num_formats = ARRAY_SIZE(mt8173_formats),
};

static const struct mtk_disp_ovl_data mt8195_ovl_driver_data = {
	.addr = DISP_REG_OVL_ADDR_MT8173,
	.gmc_bits = 10,
	.layer_nr = 4,
	.fmt_rgb565_is_0 = true,
	.smi_id_en = true,
	.supports_afbc = true,
	.blend_modes = BIT(DRM_MODE_BLEND_PREMULTI) |
		       BIT(DRM_MODE_BLEND_COVERAGE) |
		       BIT(DRM_MODE_BLEND_PIXEL_NONE),
	.formats = mt8195_formats,
	.num_formats = ARRAY_SIZE(mt8195_formats),
	.supports_clrfmt_ext = true,
};

static const struct of_device_id mtk_disp_ovl_driver_dt_match[] = {
	{ .compatible = "mediatek,mt2701-disp-ovl",
	  .data = &mt2701_ovl_driver_data},
	{ .compatible = "mediatek,mt8173-disp-ovl",
	  .data = &mt8173_ovl_driver_data},
	{ .compatible = "mediatek,mt8183-disp-ovl",
	  .data = &mt8183_ovl_driver_data},
	{ .compatible = "mediatek,mt8183-disp-ovl-2l",
	  .data = &mt8183_ovl_2l_driver_data},
	{ .compatible = "mediatek,mt8192-disp-ovl",
	  .data = &mt8192_ovl_driver_data},
	{ .compatible = "mediatek,mt8192-disp-ovl-2l",
	  .data = &mt8192_ovl_2l_driver_data},
	{ .compatible = "mediatek,mt8195-disp-ovl",
	  .data = &mt8195_ovl_driver_data},
	{ .compatible = "mediatek,mt6895-disp-ovl",
	  .data = &mt6895_ovl_driver_data},
	{ .compatible = "mediatek,mt6895-disp-ovl-2l",
	  .data = &mt8195_ovl_driver_data},
	{},
};
MODULE_DEVICE_TABLE(of, mtk_disp_ovl_driver_dt_match);

struct platform_driver mtk_disp_ovl_driver = {
	.probe		= mtk_disp_ovl_probe,
	.remove		= mtk_disp_ovl_remove,
	.driver		= {
		.name	= "mediatek-disp-ovl",
		.of_match_table = mtk_disp_ovl_driver_dt_match,
	},
};
