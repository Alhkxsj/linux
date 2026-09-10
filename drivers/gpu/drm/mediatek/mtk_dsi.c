// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 MediaTek Inc.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/component.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/reset.h>
#include <linux/units.h>

#include <video/mipi_display.h>
#include <video/videomode.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_bridge.h>
#include <drm/drm_bridge_connector.h>
#include <drm/display/drm_dsc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_of.h>
#include <drm/drm_panel.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#include "mtk_ddp_comp.h"
#include "mtk_disp_drv.h"
#include "mtk_drm_drv.h"

#define DSI_START		0x00

#define DSI_INTEN		0x08

#define DSI_INTSTA		0x0c
#define LPRX_RD_RDY_INT_FLAG		BIT(0)
#define CMD_DONE_INT_FLAG		BIT(1)
#define TE_RDY_INT_FLAG			BIT(2)
#define VM_DONE_INT_FLAG		BIT(3)
#define EXT_TE_RDY_INT_FLAG		BIT(4)
#define DSI_BUSY			BIT(31)

#define DSI_CON_CTRL		0x10
#define DSI_RESET			BIT(0)
#define DSI_EN				BIT(1)
#define DPHY_RESET			BIT(2)

#define DSI_MODE_CTRL		0x14
#define MODE				(3)
#define CMD_MODE			0
#define SYNC_PULSE_MODE			1
#define SYNC_EVENT_MODE			2
#define BURST_MODE			3
#define FRM_MODE			BIT(16)
#define MIX_MODE			BIT(17)

#define DSI_TXRX_CTRL		0x18
#define VC_NUM				BIT(1)
#define LANE_NUM			GENMASK(5, 2)
#define DIS_EOT				BIT(6)
#define NULL_EN				BIT(7)
#define TE_FREERUN			BIT(8)
#define EXT_TE_EN			BIT(9)
#define EXT_TE_EDGE			BIT(10)
#define MAX_RTN_SIZE			GENMASK(15, 12)
#define HSTX_CKLP_EN			BIT(16)

#define DSI_PSCTRL		0x1c
#define DSI_PS_WC			GENMASK(13, 0)
#define DSI_PS_SEL			GENMASK(19, 16)
#define PACKED_PS_16BIT_RGB565		0
#define PACKED_PS_18BIT_RGB666		1
#define LOOSELY_PS_24BIT_RGB666		2
#define PACKED_PS_24BIT_RGB888		3
#define PACKED_PS_DSC			5

#define DSI_VSA_NL		0x20
#define DSI_VBP_NL		0x24
#define DSI_VFP_NL		0x28
#define DSI_VACT_NL		0x2C
#define VACT_NL				GENMASK(14, 0)
#define DSI_SIZE_CON		0x38
#define DSI_HEIGHT				GENMASK(30, 16)
#define DSI_WIDTH				GENMASK(14, 0)
#define DSI_HSA_WC		0x50
#define DSI_HBP_WC		0x54
#define DSI_HFP_WC		0x58
#define HFP_HS_VB_PS_WC		GENMASK(30, 16)
#define HFP_HS_EN			BIT(31)

#define DSI_CMDQ_SIZE		0x60
#define CMDQ_SIZE			0x3f
#define CMDQ_SIZE_SEL		BIT(15)

/* MT6895 (mt6983-gen) DSI TX buffer control */
#define DSI_DEBUG_SEL		0x170
#define MM_RST_SEL			BIT(10)
#define DSI_BUF_CON0		0x400
#define BUF_BUF_EN			BIT(0)
#define DSI_BUF_CON1		0x404
#define DSI_TX_BUF_RW_TIMES	0x410
#define DSI_BUF_SODI_HIGH	0x414
#define DSI_BUF_SODI_LOW	0x418
#define DSI_BUF_PREULTRA_HIGH	0x424
#define DSI_BUF_PREULTRA_LOW	0x428
#define DSI_BUF_ULTRA_HIGH	0x42c
#define DSI_BUF_ULTRA_LOW	0x430
#define DSI_BUF_URGENT_HIGH	0x434
#define DSI_BUF_URGENT_LOW	0x438

#define DSI_HSTX_CKL_WC		0x64
#define HSTX_CKL_WC			GENMASK(15, 2)

#define DSI_RX_DATA0		0x74
#define DSI_RX_DATA1		0x78
#define DSI_RX_DATA2		0x7c
#define DSI_RX_DATA3		0x80

#define DSI_RACK		0x84
#define RACK				BIT(0)

#define DSI_PHY_LCCON		0x104
#define LC_HS_TX_EN			BIT(0)
#define LC_ULPM_EN			BIT(1)
#define LC_WAKEUP_EN			BIT(2)

#define DSI_PHY_LD0CON		0x108
#define LD0_HS_TX_EN			BIT(0)
#define LD0_ULPM_EN			BIT(1)
#define LD0_WAKEUP_EN			BIT(2)

#define DSI_PHY_TIMECON0	0x110
#define LPX				GENMASK(7, 0)
#define HS_PREP				GENMASK(15, 8)
#define HS_ZERO				GENMASK(23, 16)
#define HS_TRAIL			GENMASK(31, 24)

#define DSI_PHY_TIMECON1	0x114
#define TA_GO				GENMASK(7, 0)
#define TA_SURE				GENMASK(15, 8)
#define TA_GET				GENMASK(23, 16)
#define DA_HS_EXIT			GENMASK(31, 24)

#define DSI_PHY_TIMECON2	0x118
#define CONT_DET			GENMASK(7, 0)
#define DA_HS_SYNC			GENMASK(15, 8)
#define CLK_ZERO			GENMASK(23, 16)
#define CLK_TRAIL			GENMASK(31, 24)

#define DSI_PHY_TIMECON3	0x11c
#define CLK_HS_PREP			GENMASK(7, 0)
#define CLK_HS_POST			GENMASK(15, 8)
#define CLK_HS_EXIT			GENMASK(23, 16)

/* DSI_VM_CMD_CON */
#define VM_CMD_EN			BIT(0)
#define VM_LONG_PACKET			BIT(1)
#define TS_VFP_EN			BIT(5)
#define VM_CMD_START			BIT(16)
#define VM_CMD_DONE_INT			BIT(5)

/* DSI_SHADOW_DEBUG */
#define FORCE_COMMIT			BIT(0)
#define BYPASS_SHADOW			BIT(1)

/* CMDQ related bits */
#define CONFIG				GENMASK(7, 0)
#define SHORT_PACKET			0
#define LONG_PACKET			2
#define BTA				BIT(2)
#define DATA_ID				GENMASK(15, 8)
#define DATA_0				GENMASK(23, 16)
#define DATA_1				GENMASK(31, 24)

#define NS_TO_CYCLE(n, c)    ((n) / (c) + (((n) % (c)) ? 1 : 0))

#define MTK_DSI_HOST_IS_READ(type) \
	((type == MIPI_DSI_GENERIC_READ_REQUEST_0_PARAM) || \
	(type == MIPI_DSI_GENERIC_READ_REQUEST_1_PARAM) || \
	(type == MIPI_DSI_GENERIC_READ_REQUEST_2_PARAM) || \
	(type == MIPI_DSI_DCS_READ))

struct mtk_phy_timing {
	u32 lpx;
	u32 da_hs_prepare;
	u32 da_hs_zero;
	u32 da_hs_trail;

	u32 ta_go;
	u32 ta_sure;
	u32 ta_get;
	u32 da_hs_exit;

	u32 clk_hs_zero;
	u32 clk_hs_trail;

	u32 clk_hs_prepare;
	u32 clk_hs_post;
	u32 clk_hs_exit;
};

struct phy;

struct mtk_dsi_driver_data {
	const u32 reg_cmdq_off;
	const u32 reg_vm_cmd_off;
	const u32 reg_shadow_dbg_off;
	bool has_shadow_ctl;
	bool has_size_ctl;
	bool cmdq_long_packet_ctl;
	bool support_per_frame_lp;
};

struct mtk_dsi {
	struct device *dev;
	struct mipi_dsi_host host;
	struct drm_encoder encoder;
	struct drm_bridge bridge;
	struct drm_bridge *next_bridge;
	struct drm_connector *connector;
	struct phy *phy;

	void __iomem *regs;

	struct clk *engine_clk;
	struct clk *digital_clk;
	struct clk *hs_clk;

	u32 data_rate;

	unsigned long mode_flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;
	struct videomode vm;
	struct mtk_phy_timing phy_timing;
	int refcount;
	bool enabled;
	bool lanes_ready;
	u32 irq_data;
	wait_queue_head_t irq_wait_queue;
	const struct mtk_dsi_driver_data *driver_data;
	struct mtk_dsc_config_params dsc_params;
};

static inline struct mtk_dsi *bridge_to_dsi(struct drm_bridge *b)
{
	return container_of(b, struct mtk_dsi, bridge);
}

static inline struct mtk_dsi *host_to_dsi(struct mipi_dsi_host *h)
{
	return container_of(h, struct mtk_dsi, host);
}

static void mtk_dsi_mask(struct mtk_dsi *dsi, u32 offset, u32 mask, u32 data)
{
	u32 temp = readl(dsi->regs + offset);

	writel((temp & ~mask) | (data & mask), dsi->regs + offset);
}

/*
 * Fill the DISP_DSC block parameters from the panel's drm_dsc_config. The
 * xaga panel is a fixed 8bpc/8bpp DSC panel; the register-level PPS (PPS0-19)
 * is written by the DSC ddp component from these values.
 */
static void mtk_dsi_fill_dsc_params(struct mtk_dsi *dsi,
				    const struct drm_dsc_config *dsc)
{
	struct mtk_dsc_config_params *p = &dsi->dsc_params;

	memset(p, 0, sizeof(*p));
	p->enable = true;
	p->ver = 17;
	p->slice_mode = dsc->slice_count > 1 ? 1 : 0;
	p->rgb_swap = 0;
	p->dsc_cfg = 34;
	p->rct_on = 1;
	p->bit_per_channel = dsc->bits_per_component;
	p->line_buf_depth = dsc->line_buf_depth;
	p->bp_enable = dsc->block_pred_enable;
	p->bit_per_pixel = dsc->bits_per_pixel;
	p->slice_width = dsc->slice_width;
	p->slice_height = dsc->slice_height;
	p->chunk_size = dsc->slice_width * dsc->bits_per_pixel / 8 / 16;
	p->xmit_delay = dsc->initial_xmit_delay;
	p->dec_delay = dsc->initial_dec_delay;
	p->scale_value = dsc->initial_scale_value;
	p->increment_interval = dsc->scale_increment_interval;
	p->decrement_interval = dsc->scale_decrement_interval;
	p->line_bpg_offset = dsc->first_line_bpg_offset;
	p->nfl_bpg_offset = dsc->nfl_bpg_offset;
	p->slice_bpg_offset = dsc->slice_bpg_offset;
	p->initial_offset = dsc->initial_offset;
	p->final_offset = dsc->final_offset;
	p->flatness_minqp = dsc->flatness_min_qp;
	p->flatness_maxqp = dsc->flatness_max_qp;
	p->rc_model_size = dsc->rc_model_size;
}

static void __maybe_unused mtk_dsi_phy_timconfig(struct mtk_dsi *dsi)
{
	u32 timcon0, timcon1, timcon2, timcon3;
	u32 data_rate_mhz = DIV_ROUND_UP(dsi->data_rate, HZ_PER_MHZ);
	struct mtk_phy_timing *timing = &dsi->phy_timing;

	timing->lpx = (60 * data_rate_mhz / (8 * 1000)) + 1;
	timing->da_hs_prepare = (80 * data_rate_mhz + 4 * 1000) / 8000;
	timing->da_hs_zero = (170 * data_rate_mhz + 10 * 1000) / 8000 + 1 -
			     timing->da_hs_prepare;
	timing->da_hs_trail = timing->da_hs_prepare + 1;

	timing->ta_go = 4 * timing->lpx - 2;
	timing->ta_sure = timing->lpx + 2;
	timing->ta_get = 4 * timing->lpx;
	timing->da_hs_exit = 2 * timing->lpx + 1;

	timing->clk_hs_prepare = 70 * data_rate_mhz / (8 * 1000);
	timing->clk_hs_post = timing->clk_hs_prepare + 8;
	timing->clk_hs_trail = timing->clk_hs_prepare;
	timing->clk_hs_zero = timing->clk_hs_trail * 4;
	timing->clk_hs_exit = 2 * timing->clk_hs_trail;

	timcon0 = FIELD_PREP(LPX, timing->lpx) |
		  FIELD_PREP(HS_PREP, timing->da_hs_prepare) |
		  FIELD_PREP(HS_ZERO, timing->da_hs_zero) |
		  FIELD_PREP(HS_TRAIL, timing->da_hs_trail);

	timcon1 = FIELD_PREP(TA_GO, timing->ta_go) |
		  FIELD_PREP(TA_SURE, timing->ta_sure) |
		  FIELD_PREP(TA_GET, timing->ta_get) |
		  FIELD_PREP(DA_HS_EXIT, timing->da_hs_exit);

	timcon2 = FIELD_PREP(DA_HS_SYNC, 1) |
		  FIELD_PREP(CLK_ZERO, timing->clk_hs_zero) |
		  FIELD_PREP(CLK_TRAIL, timing->clk_hs_trail);

	timcon3 = FIELD_PREP(CLK_HS_PREP, timing->clk_hs_prepare) |
		  FIELD_PREP(CLK_HS_POST, timing->clk_hs_post) |
		  FIELD_PREP(CLK_HS_EXIT, timing->clk_hs_exit);

	writel(timcon0, dsi->regs + DSI_PHY_TIMECON0);
	writel(timcon1, dsi->regs + DSI_PHY_TIMECON1);
	writel(timcon2, dsi->regs + DSI_PHY_TIMECON2);
	writel(timcon3, dsi->regs + DSI_PHY_TIMECON3);
}

static void mtk_dsi_enable(struct mtk_dsi *dsi)
{
	mtk_dsi_mask(dsi, DSI_CON_CTRL, DSI_EN, DSI_EN);
}

static void mtk_dsi_disable(struct mtk_dsi *dsi)
{
	mtk_dsi_mask(dsi, DSI_CON_CTRL, DSI_EN, 0);
}

static void mtk_dsi_reset_engine(struct mtk_dsi *dsi)
{
	mtk_dsi_mask(dsi, DSI_CON_CTRL, DSI_RESET, DSI_RESET);
	mtk_dsi_mask(dsi, DSI_CON_CTRL, DSI_RESET, 0);
}

static void mtk_dsi_reset_dphy(struct mtk_dsi *dsi)
{
	mtk_dsi_mask(dsi, DSI_CON_CTRL, DPHY_RESET, DPHY_RESET);
	mtk_dsi_mask(dsi, DSI_CON_CTRL, DPHY_RESET, 0);
}

static void mtk_dsi_clk_ulp_mode_enter(struct mtk_dsi *dsi)
{
	mtk_dsi_mask(dsi, DSI_PHY_LCCON, LC_HS_TX_EN, 0);
	mtk_dsi_mask(dsi, DSI_PHY_LCCON, LC_ULPM_EN, 0);
}

static void mtk_dsi_clk_ulp_mode_leave(struct mtk_dsi *dsi)
{
	mtk_dsi_mask(dsi, DSI_PHY_LCCON, LC_ULPM_EN, 0);
	mtk_dsi_mask(dsi, DSI_PHY_LCCON, LC_WAKEUP_EN, LC_WAKEUP_EN);
	mtk_dsi_mask(dsi, DSI_PHY_LCCON, LC_WAKEUP_EN, 0);
}

static void mtk_dsi_lane0_ulp_mode_enter(struct mtk_dsi *dsi)
{
	mtk_dsi_mask(dsi, DSI_PHY_LD0CON, LD0_HS_TX_EN, 0);
	mtk_dsi_mask(dsi, DSI_PHY_LD0CON, LD0_ULPM_EN, 0);
}

static void mtk_dsi_lane0_ulp_mode_leave(struct mtk_dsi *dsi)
{
	mtk_dsi_mask(dsi, DSI_PHY_LD0CON, LD0_ULPM_EN, 0);
	mtk_dsi_mask(dsi, DSI_PHY_LD0CON, LD0_WAKEUP_EN, LD0_WAKEUP_EN);
	mtk_dsi_mask(dsi, DSI_PHY_LD0CON, LD0_WAKEUP_EN, 0);
}

static bool mtk_dsi_clk_hs_state(struct mtk_dsi *dsi)
{
	return readl(dsi->regs + DSI_PHY_LCCON) & LC_HS_TX_EN;
}

static void mtk_dsi_clk_hs_mode(struct mtk_dsi *dsi, bool enter)
{
	if (enter && !mtk_dsi_clk_hs_state(dsi))
		mtk_dsi_mask(dsi, DSI_PHY_LCCON, LC_HS_TX_EN, LC_HS_TX_EN);
	else if (!enter && mtk_dsi_clk_hs_state(dsi))
		mtk_dsi_mask(dsi, DSI_PHY_LCCON, LC_HS_TX_EN, 0);
}

static void mtk_dsi_set_mode(struct mtk_dsi *dsi)
{
	u32 vid_mode = CMD_MODE;

	if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO) {
		if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO_BURST)
			vid_mode = BURST_MODE;
		else if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE)
			vid_mode = SYNC_PULSE_MODE;
		else
			vid_mode = SYNC_EVENT_MODE;
	}

	writel(vid_mode, dsi->regs + DSI_MODE_CTRL);
}

static void __maybe_unused mtk_dsi_set_vm_cmd(struct mtk_dsi *dsi)
{
	mtk_dsi_mask(dsi, dsi->driver_data->reg_vm_cmd_off, VM_CMD_EN, VM_CMD_EN);
	mtk_dsi_mask(dsi, dsi->driver_data->reg_vm_cmd_off, TS_VFP_EN, TS_VFP_EN);
}

static void mtk_dsi_rxtx_control(struct mtk_dsi *dsi)
{
	u32 regval, tmp_reg = 0;
	u8 i;

	/* Number of DSI lanes (max 4 lanes), each bit enables one DSI lane. */
	for (i = 0; i < dsi->lanes; i++)
		tmp_reg |= BIT(i);

	regval = FIELD_PREP(LANE_NUM, tmp_reg);

	if (dsi->mode_flags & MIPI_DSI_CLOCK_NON_CONTINUOUS)
		regval |= HSTX_CKLP_EN;

	if (dsi->mode_flags & MIPI_DSI_MODE_NO_EOT_PACKET)
		regval |= DIS_EOT;

	writel(regval, dsi->regs + DSI_TXRX_CTRL);
}

static void mtk_dsi_ps_control(struct mtk_dsi *dsi, bool config_vact)
{
	u32 dsi_buf_bpp, ps_val, ps_wc, vact_nl;

	if (dsi->format == MIPI_DSI_FMT_RGB565)
		dsi_buf_bpp = 2;
	else
		dsi_buf_bpp = 3;

	if (dsi->dsc_params.enable) {
		/* compressed word count per line */
		ps_wc = FIELD_PREP(DSI_PS_WC,
				   ((dsi->dsc_params.chunk_size + 2) / 3) * 3 *
				   (dsi->dsc_params.slice_mode + 1));
		ps_val = ps_wc | FIELD_PREP(DSI_PS_SEL, PACKED_PS_DSC);
	} else {
		/* Word count */
		ps_wc = FIELD_PREP(DSI_PS_WC, dsi->vm.hactive * dsi_buf_bpp);
		ps_val = ps_wc;

		/* Pixel Stream type */
		switch (dsi->format) {
		default:
			fallthrough;
		case MIPI_DSI_FMT_RGB888:
			ps_val |= FIELD_PREP(DSI_PS_SEL, PACKED_PS_24BIT_RGB888);
			break;
		case MIPI_DSI_FMT_RGB666:
			ps_val |= FIELD_PREP(DSI_PS_SEL, LOOSELY_PS_24BIT_RGB666);
			break;
		case MIPI_DSI_FMT_RGB666_PACKED:
			ps_val |= FIELD_PREP(DSI_PS_SEL, PACKED_PS_18BIT_RGB666);
			break;
		case MIPI_DSI_FMT_RGB565:
			ps_val |= FIELD_PREP(DSI_PS_SEL, PACKED_PS_16BIT_RGB565);
			break;
		}
	}

	if (config_vact) {
		vact_nl = FIELD_PREP(VACT_NL, dsi->vm.vactive);
		writel(vact_nl, dsi->regs + DSI_VACT_NL);
		writel(ps_wc, dsi->regs + DSI_HSTX_CKL_WC);
	}
	/*
	 * Preserve the upper PSCTRL bits (CUSTOM_HEADER[31:26] etc.) that LK
	 * programmed - the working device reads PSCTRL=0x2c050438 with a
	 * nonzero CUSTOM_HEADER. Downstream does a read-modify-write on just
	 * the PS_WC/PS_SEL fields instead of a full write.
	 */
	{
		u32 val = readl(dsi->regs + DSI_PSCTRL);

		val = (val & ~(DSI_PS_WC | DSI_PS_SEL)) | (ps_val & (DSI_PS_WC | DSI_PS_SEL));
		writel(val, dsi->regs + DSI_PSCTRL);
	}
}

static void mtk_dsi_config_vdo_timing_per_frame_lp(struct mtk_dsi *dsi)
{
	u32 horizontal_sync_active_byte;
	u32 horizontal_backporch_byte;
	u32 horizontal_frontporch_byte;
	u32 hfp_byte_adjust, v_active_adjust;
	u32 cklp_wc_min_adjust, cklp_wc_max_adjust;
	u32 dsi_tmp_buf_bpp;
	unsigned int da_hs_trail;
	unsigned int ps_wc, hs_vb_ps_wc;
	u32 v_active_roundup, hstx_cklp_wc;
	u32 hstx_cklp_wc_max, hstx_cklp_wc_min;
	struct videomode *vm = &dsi->vm;

	if (dsi->format == MIPI_DSI_FMT_RGB565)
		dsi_tmp_buf_bpp = 2;
	else
		dsi_tmp_buf_bpp = 3;

	da_hs_trail = dsi->phy_timing.da_hs_trail;
	if (dsi->dsc_params.enable)
		ps_wc = ((dsi->dsc_params.chunk_size + 2) / 3) * 3 *
			(dsi->dsc_params.slice_mode + 1);
	else
		ps_wc = vm->hactive * dsi_tmp_buf_bpp;

	if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE) {
		horizontal_sync_active_byte =
			vm->hsync_len * dsi_tmp_buf_bpp - 10;
		horizontal_backporch_byte =
			vm->hback_porch * dsi_tmp_buf_bpp - 10;
		hfp_byte_adjust = 12;
		v_active_adjust = 32 + horizontal_sync_active_byte;
		cklp_wc_min_adjust = 12 + 2 + 4 + horizontal_sync_active_byte;
		cklp_wc_max_adjust = 20 + 6 + 4 + horizontal_sync_active_byte;
	} else {
		horizontal_sync_active_byte = vm->hsync_len * dsi_tmp_buf_bpp - 4;
		horizontal_backporch_byte = (vm->hback_porch + vm->hsync_len) *
			dsi_tmp_buf_bpp - 10;
		cklp_wc_min_adjust = 4;
		cklp_wc_max_adjust = 12 + 4 + 4;
		if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO_BURST) {
			hfp_byte_adjust = 18;
			v_active_adjust = 28;
		} else {
			hfp_byte_adjust = 12;
			v_active_adjust = 22;
		}
	}
	horizontal_frontporch_byte = vm->hfront_porch * dsi_tmp_buf_bpp - hfp_byte_adjust;
	v_active_roundup = (v_active_adjust + horizontal_backporch_byte + ps_wc +
			   horizontal_frontporch_byte) % dsi->lanes;
	if (v_active_roundup)
		horizontal_backporch_byte += dsi->lanes - v_active_roundup;
	hstx_cklp_wc_min = (DIV_ROUND_UP(cklp_wc_min_adjust, dsi->lanes) + da_hs_trail + 1)
			   * dsi->lanes / 6 - 1;
	hstx_cklp_wc_max = (DIV_ROUND_UP((cklp_wc_max_adjust + horizontal_backporch_byte +
			   ps_wc), dsi->lanes) + da_hs_trail + 1) * dsi->lanes / 6 - 1;

	hstx_cklp_wc = FIELD_PREP(HSTX_CKL_WC, (hstx_cklp_wc_min + hstx_cklp_wc_max) / 2);
	writel(hstx_cklp_wc, dsi->regs + DSI_HSTX_CKL_WC);

	hs_vb_ps_wc = ps_wc - (dsi->phy_timing.lpx + dsi->phy_timing.da_hs_exit +
		      dsi->phy_timing.da_hs_prepare + dsi->phy_timing.da_hs_zero + 2) * dsi->lanes;
	horizontal_frontporch_byte |= FIELD_PREP(HFP_HS_EN, 1) |
				      FIELD_PREP(HFP_HS_VB_PS_WC, hs_vb_ps_wc);

	writel(horizontal_sync_active_byte, dsi->regs + DSI_HSA_WC);
	writel(horizontal_backporch_byte, dsi->regs + DSI_HBP_WC);
	writel(horizontal_frontporch_byte, dsi->regs + DSI_HFP_WC);
}

static void mtk_dsi_config_vdo_timing_per_line_lp(struct mtk_dsi *dsi)
{
	u32 horizontal_sync_active_byte;
	u32 horizontal_backporch_byte;
	u32 horizontal_frontporch_byte;
	u32 dsi_tmp_buf_bpp;
	struct videomode *vm = &dsi->vm;

	if (dsi->format == MIPI_DSI_FMT_RGB565)
		dsi_tmp_buf_bpp = 2;
	else
		dsi_tmp_buf_bpp = 3;

	/*
	 * Per-line video mode word counts, matching the downstream
	 * mtk_dsi_calc_vdo_timing() exactly (non-cphy path). The panel
	 * expects these byte counts; any other formula (e.g. the mainline
	 * per_line_lp data_phy_cycles adjustment) writes wrong HSA/HBP/HFP
	 * and the panel never locks onto the stream.
	 */
	if (dsi->mode_flags & MIPI_DSI_MODE_VIDEO_SYNC_PULSE) {
		horizontal_sync_active_byte =
			ALIGN((vm->hsync_len * dsi_tmp_buf_bpp - 10), 4);
		horizontal_backporch_byte =
			ALIGN((vm->hback_porch * dsi_tmp_buf_bpp - 10), 4);
	} else {
		horizontal_sync_active_byte =
			ALIGN((vm->hsync_len * dsi_tmp_buf_bpp - 4), 4);
		horizontal_backporch_byte =
			ALIGN(((vm->hback_porch + vm->hsync_len) *
			       dsi_tmp_buf_bpp - 10), 4);
	}

	horizontal_frontporch_byte =
		ALIGN((vm->hfront_porch * dsi_tmp_buf_bpp - 12), 4);

	writel(horizontal_sync_active_byte, dsi->regs + DSI_HSA_WC);
	writel(horizontal_backporch_byte, dsi->regs + DSI_HBP_WC);
	writel(horizontal_frontporch_byte, dsi->regs + DSI_HFP_WC);
}

static void __maybe_unused mtk_dsi_config_vdo_timing(struct mtk_dsi *dsi)
{
	struct videomode *vm = &dsi->vm;

	writel(vm->vsync_len, dsi->regs + DSI_VSA_NL);
	writel(vm->vback_porch, dsi->regs + DSI_VBP_NL);
	writel(vm->vfront_porch, dsi->regs + DSI_VFP_NL);
	writel(vm->vactive, dsi->regs + DSI_VACT_NL);

	if (dsi->driver_data->has_size_ctl) {
		/*
		 * DSI_SIZE_CON low word = active line length in DSI-buffer
		 * bpp units. With DSC this is ps_wc / dsi_buf_bpp (1080/3=360),
		 * NOT the raw hactive. Downstream mtk_dsi_ps_control_vact
		 * writes (height<<16) + (ps_wc/dsi_buf_bpp). A wrong value makes
		 * the DSI expect the wrong amount of active data per line ->
		 * INP_UNFINISH -> the TX buffer fills but never drains.
		 */
		if (dsi->dsc_params.enable) {
			u32 ps_wc = ((dsi->dsc_params.chunk_size + 2) / 3) * 3;
			u32 dsi_buf_bpp = 3;

			if (dsi->dsc_params.slice_mode == 1)
				ps_wc *= 2;
			writel(FIELD_PREP(DSI_HEIGHT, vm->vactive) |
			       (ps_wc / dsi_buf_bpp),
			       dsi->regs + DSI_SIZE_CON);
		} else {
			writel(FIELD_PREP(DSI_HEIGHT, vm->vactive) |
				FIELD_PREP(DSI_WIDTH, vm->hactive),
				dsi->regs + DSI_SIZE_CON);
		}
	}

	if (dsi->driver_data->support_per_frame_lp)
		mtk_dsi_config_vdo_timing_per_frame_lp(dsi);
	else
		mtk_dsi_config_vdo_timing_per_line_lp(dsi);

	mtk_dsi_ps_control(dsi, false);
}

static void mtk_dsi_start(struct mtk_dsi *dsi)
{
	writel(0, dsi->regs + DSI_START);
	writel(1, dsi->regs + DSI_START);
}

static void mtk_dsi_stop(struct mtk_dsi *dsi)
{
	writel(0, dsi->regs + DSI_START);
}

static void mtk_dsi_set_cmd_mode(struct mtk_dsi *dsi)
{
	writel(CMD_MODE, dsi->regs + DSI_MODE_CTRL);
}

static void __maybe_unused mtk_dsi_set_interrupt_enable(struct mtk_dsi *dsi)
{
	u32 inten = LPRX_RD_RDY_INT_FLAG | CMD_DONE_INT_FLAG | VM_DONE_INT_FLAG;

	writel(inten, dsi->regs + DSI_INTEN);
}

static void mtk_dsi_irq_data_set(struct mtk_dsi *dsi, u32 irq_bit)
{
	dsi->irq_data |= irq_bit;
}

static void mtk_dsi_irq_data_clear(struct mtk_dsi *dsi, u32 irq_bit)
{
	dsi->irq_data &= ~irq_bit;
}

static s32 mtk_dsi_wait_for_irq_done(struct mtk_dsi *dsi, u32 irq_flag,
				     unsigned int timeout)
{
	s32 ret = 0;
	unsigned long jiffies = msecs_to_jiffies(timeout);

	ret = wait_event_interruptible_timeout(dsi->irq_wait_queue,
					       dsi->irq_data & irq_flag,
					       jiffies);
	if (ret == 0) {
		DRM_WARN("Wait DSI IRQ(0x%08x) Timeout\n", irq_flag);

		mtk_dsi_enable(dsi);
		mtk_dsi_reset_engine(dsi);
	}

	return ret;
}

static irqreturn_t mtk_dsi_irq(int irq, void *dev_id)
{
	struct mtk_dsi *dsi = dev_id;
	u32 status, tmp;
	u32 flag = LPRX_RD_RDY_INT_FLAG | CMD_DONE_INT_FLAG | VM_DONE_INT_FLAG;
	int spin = 0;

	status = readl(dsi->regs + DSI_INTSTA) & flag;

	if (status) {
		/*
		 * XAGA: BOUND the busy spin. On grey boots the DSI can be
		 * stuck BUSY forever; the original unbounded loop spins in
		 * IRQ context indefinitely -> the CPU never services the
		 * watchdog ping -> 31s reboot. Give up after ~10ms and still
		 * clear the interrupt so the line does not re-fire.
		 */
		do {
			mtk_dsi_mask(dsi, DSI_RACK, RACK, RACK);
			tmp = readl(dsi->regs + DSI_INTSTA);
			if (++spin > 10000)
				break;
		} while (tmp & DSI_BUSY);

		mtk_dsi_mask(dsi, DSI_INTSTA, status, 0);
		mtk_dsi_irq_data_set(dsi, status);
		wake_up_interruptible(&dsi->irq_wait_queue);
	}

	return IRQ_HANDLED;
}

static s32 mtk_dsi_switch_to_cmd_mode(struct mtk_dsi *dsi, u8 irq_flag, u32 t)
{
	mtk_dsi_irq_data_clear(dsi, irq_flag);
	mtk_dsi_set_cmd_mode(dsi);

	if (!mtk_dsi_wait_for_irq_done(dsi, irq_flag, t)) {
		DRM_ERROR("failed to switch cmd mode\n");
		return -ETIME;
	} else {
		return 0;
	}
}

/*
 * MT6895 (mt6983-gen) DSI TX buffer / flow-control config. Without this the
 * DSI TX FIFO has no fill thresholds and BUFFER_UNDERRUN stalls the whole
 * display pipeline (OVL stops producing vblank after a few frames). Ported
 * from the stock xaga kernel mtk_dsi_tx_buf_rw() for the dsi_buffer=true
 * (mt6895) configuration, single-pipe video mode.
 */
static void __maybe_unused mtk_dsi_tx_buf_rw(struct mtk_dsi *dsi)
{
	u32 mmsys_clk = 208;
	u32 ps_wc, width, height, tmp, rw_times;
	u32 sodi_hi, sodi_lo, preultra_hi, preultra_lo;
	u32 ultra_hi, ultra_lo, urgent_hi, urgent_lo;
	u32 fill_rate, dsi_buf_bpp = 3;

	width = dsi->vm.hactive;
	height = dsi->vm.vactive;

	if (dsi->dsc_params.enable) {
		ps_wc = ((dsi->dsc_params.chunk_size + 2) / 3) * 3;
		if (dsi->dsc_params.slice_mode == 1)
			ps_wc *= 2;
	} else {
		ps_wc = width * dsi_buf_bpp;
	}

	if ((ps_wc * height % 9) == 0)
		rw_times = ps_wc * height / 9;
	else
		rw_times = ps_wc * height / 9 + 1;

	/* video mode: not frame-trigger, so no per-line burst tuning */
	{
		u32 rate_mhz = dsi->data_rate / 1000000;

		tmp = 25 * rate_mhz * dsi->lanes / 8 / 18;
	}
	mtk_dsi_mask(dsi, DSI_BUF_CON1, 0x7fff, tmp);

	mtk_dsi_mask(dsi, DSI_DEBUG_SEL, MM_RST_SEL, MM_RST_SEL);

	fill_rate = mmsys_clk * 3 / 18;
	tmp = readl(dsi->regs + DSI_BUF_CON1) >> 16;

	{
		u32 rate_mhz = dsi->data_rate / 1000000;

		sodi_hi = tmp - (12 * (fill_rate - rate_mhz * dsi->lanes / 8 / 18) / 10);
		sodi_lo = (23 + 5) * rate_mhz * dsi->lanes / 8 / 18;
		preultra_hi = 26 * rate_mhz * dsi->lanes / 8 / 18;
		preultra_lo = 25 * rate_mhz * dsi->lanes / 8 / 18;
		ultra_hi = 25 * rate_mhz * dsi->lanes / 8 / 18;
		ultra_lo = 23 * rate_mhz * dsi->lanes / 8 / 18;
		urgent_hi = 12 * rate_mhz * dsi->lanes / 8 / 18;
		urgent_lo = 11 * rate_mhz * dsi->lanes / 8 / 18;
	}

	writel(sodi_hi & 0xfffff, dsi->regs + DSI_BUF_SODI_HIGH);
	writel(sodi_lo & 0xfffff, dsi->regs + DSI_BUF_SODI_LOW);
	writel(preultra_hi & 0xfffff, dsi->regs + DSI_BUF_PREULTRA_HIGH);
	writel(preultra_lo & 0xfffff, dsi->regs + DSI_BUF_PREULTRA_LOW);
	writel(ultra_hi & 0xfffff, dsi->regs + DSI_BUF_ULTRA_HIGH);
	writel(ultra_lo & 0xfffff, dsi->regs + DSI_BUF_ULTRA_LOW);
	writel(urgent_hi & 0xfffff, dsi->regs + DSI_BUF_URGENT_HIGH);
	writel(urgent_lo & 0xfffff, dsi->regs + DSI_BUF_URGENT_LOW);
	writel(rw_times, dsi->regs + DSI_TX_BUF_RW_TIMES);
	mtk_dsi_mask(dsi, DSI_BUF_CON0, BUF_BUF_EN, BUF_BUF_EN);
}

static int mtk_dsi_poweron(struct mtk_dsi *dsi)
{
	if (++dsi->refcount != 1)
		return 0;

	/*
	 * XAGA: minimal poweron for the panel-init path. The PHY/PLL stay
	 * as LK left them (re-running clk_set_rate/phy_power_on on the live
	 * stream disrupted it). But the panel re-init (reset + init table,
	 * see panel-xiaomi-l16-36-02-0b.c) needs DSI command transfers,
	 * which wait on CMD_DONE/VM_DONE IRQs - arm those (the mtk_dsi_irq
	 * handler clears exactly these bits, so no storm). INTEN with
	 * FRAME_DONE/UNDERRUN must NOT be armed (handler never clears them
	 * -> 144Hz interrupt storm -> RCU stall -> watchdog reboot).
	 */
	mtk_dsi_set_interrupt_enable(dsi);
	writel(0, dsi->regs + DSI_INTSTA);

	if (dsi->driver_data->has_shadow_ctl)
		writel(FORCE_COMMIT | BYPASS_SHADOW,
		       dsi->regs + dsi->driver_data->reg_shadow_dbg_off);

	return 0;
}

static void mtk_dsi_poweroff(struct mtk_dsi *dsi)
{
	if (WARN_ON(dsi->refcount == 0))
		return;

	if (--dsi->refcount != 0)
		return;

	/*
	 * XAGA: NO-OP for the mt6895 boot path, mirroring the poweron
	 * no-op. We never clk_prepare_enable'd engine_clk/digital_clk and
	 * never phy_power_on'd, so the original body's
	 * clk_disable_unprepare/phy_power_off trips prepare-count WARNs
	 * and crashes the disable path (observed: fbdev modeset-disable
	 * at ~2-3s -> clk WARN traces -> panic -> MTK panic handler ->
	 * reboot with command 'recovery' -> LK recovery bootloop =
	 * xaga recovery brick). LK's PHY/clocks stay running; the stream
	 * restart logic lives in hw_init instead.
	 */
}

static void mtk_dsi_lane_ready(struct mtk_dsi *dsi)
{
	if (!dsi->lanes_ready) {
		dsi->lanes_ready = true;
		mtk_dsi_rxtx_control(dsi);
		usleep_range(30, 100);
		mtk_dsi_reset_dphy(dsi);
		mtk_dsi_clk_ulp_mode_leave(dsi);
		mtk_dsi_lane0_ulp_mode_leave(dsi);
		mtk_dsi_clk_hs_mode(dsi, 0);
		usleep_range(1000, 3000);
		/* The reaction time after pulling up the mipi signal for dsi_rx */
	}
}

static void mtk_output_dsi_enable(struct mtk_dsi *dsi)
{
	if (dsi->enabled)
		return;

	/*
	 * XAGA: no-op for the mt6895 boot path. LK already left the DSI
	 * streaming in video mode. mtk_dsi_lane_ready() (rxtx_control, dphy
	 * reset, ULP leave) + set_mode + clk_hs_mode + DSI_START toggle would
	 * re-drive the PHY/lanes on the live stream and can disrupt it ->
	 * panel grey test pattern. The DRM commit path takes over later.
	 * (Verified against the working Tianma 5.10/recovery state: LK's DSI
	 * timing VFP=0x532/HFP=0x3cc/HSA=0x14/HBP=0x1c is preserved and the
	 * DSC runs clean with it.)
	 */
	dsi->enabled = true;
}

static void mtk_output_dsi_disable(struct mtk_dsi *dsi)
{
	if (!dsi->enabled)
		return;

	dsi->enabled = false;
}

static int mtk_dsi_bridge_attach(struct drm_bridge *bridge,
				 struct drm_encoder *encoder,
				 enum drm_bridge_attach_flags flags)
{
	struct mtk_dsi *dsi = bridge_to_dsi(bridge);

	/* Attach the panel or bridge to the dsi bridge */
	return drm_bridge_attach(encoder, dsi->next_bridge,
				 &dsi->bridge, flags);
}

static void mtk_dsi_bridge_mode_set(struct drm_bridge *bridge,
				    const struct drm_display_mode *mode,
				    const struct drm_display_mode *adjusted)
{
	struct mtk_dsi *dsi = bridge_to_dsi(bridge);

	drm_display_mode_to_videomode(adjusted, &dsi->vm);
}

static void mtk_dsi_bridge_atomic_disable(struct drm_bridge *bridge,
					  struct drm_atomic_state *state)
{
	struct mtk_dsi *dsi = bridge_to_dsi(bridge);

	mtk_output_dsi_disable(dsi);
}

static void mtk_dsi_bridge_atomic_enable(struct drm_bridge *bridge,
					 struct drm_atomic_state *state)
{
	struct mtk_dsi *dsi = bridge_to_dsi(bridge);

	pr_info("XAGA-STAGE dsi enable: DSI START=0x%08x CON=0x%08x MODE=0x%08x TXRX=0x%08x PSCTRL=0x%08x SIZE_CON=0x%08x\n",
		readl(dsi->regs + 0x00), readl(dsi->regs + 0x10),
		readl(dsi->regs + 0x14), readl(dsi->regs + 0x18),
		readl(dsi->regs + 0x1c), readl(dsi->regs + 0x38));
	if (dsi->refcount == 0)
		return;

	mtk_output_dsi_enable(dsi);
}

static void mtk_dsi_bridge_atomic_pre_enable(struct drm_bridge *bridge,
					     struct drm_atomic_state *state)
{
	struct mtk_dsi *dsi = bridge_to_dsi(bridge);
	int ret;

	/* XAGA: handoff-state check BEFORE the panel prepare runs - is the
	 * DSC already ABN_EOF at this point (grey handoff) or not? */
	{
		void __iomem *dsc = ioremap(0x14015000, 0x1000);

		if (dsc) {
		pr_info("XAGA-DSI pre_enable: DSC INTSTA(handoff)=0x%08x DSI INTSTA=0x%08x\n",
			readl(dsc + 0x08), readl(dsi->regs + DSI_INTSTA));
		pr_info("XAGA-DSI pre_enable: LCCON=0x%08x LD0CON=0x%08x BUF_CON1=0x%08x\n",
			readl(dsi->regs + 0x104), readl(dsi->regs + 0x108),
			readl(dsi->regs + 0x404));
			pr_info("XAGA-DSC HNDOFF: CON=0x%08x MODE=0x%08x ENC_W=0x%08x PIC_W=0x%08x PIC_H=0x%08x SLICE_W=0x%08x SLICE_H=0x%08x CHUNK=0x%08x BUF=0x%08x\n",
				readl(dsc + 0x00), readl(dsc + 0x30),
				readl(dsc + 0x3c), readl(dsc + 0x18),
				readl(dsc + 0x1c), readl(dsc + 0x20),
				readl(dsc + 0x24), readl(dsc + 0x28),
				readl(dsc + 0x2c));
			pr_info("XAGA-DSC HNDOFF: PPS0=0x%08x PPS1=0x%08x PPS2=0x%08x PPS3=0x%08x PPS4=0x%08x PPS5=0x%08x SHADOW=0x%08x\n",
				readl(dsc + 0x80), readl(dsc + 0x84),
				readl(dsc + 0x88), readl(dsc + 0x8c),
				readl(dsc + 0x90), readl(dsc + 0x94),
				readl(dsc + 0x228));
			pr_info("XAGA-DSC HNDOFF: SPR=0x%08x CFG=0x%08x PAD=0x%08x DBG=0x%08x\n",
				readl(dsc + 0x14), readl(dsc + 0x34),
				readl(dsc + 0x38), readl(dsc + 0x60));
			pr_info("XAGA-DSC HNDOFF: PPS6=0x%08x PPS7=0x%08x PPS8=0x%08x PPS9=0x%08x PPS10=0x%08x PPS11=0x%08x\n",
				readl(dsc + 0x98), readl(dsc + 0x9c),
				readl(dsc + 0xa0), readl(dsc + 0xa4),
				readl(dsc + 0xa8), readl(dsc + 0xac));
			pr_info("XAGA-DSC HNDOFF: PPS12=0x%08x PPS13=0x%08x PPS14=0x%08x PPS15=0x%08x PPS16=0x%08x PPS17=0x%08x PPS18=0x%08x PPS19=0x%08x\n",
				readl(dsc + 0xb0), readl(dsc + 0xb4),
				readl(dsc + 0xb8), readl(dsc + 0xbc),
				readl(dsc + 0xc0), readl(dsc + 0xc4),
				readl(dsc + 0xc8), readl(dsc + 0xcc));
			iounmap(dsc);
		}
	}
	pr_info("XAGA-STAGE dsi pre_enable: DSI START=0x%08x CON=0x%08x MODE=0x%08x TXRX=0x%08x\n",
		readl(dsi->regs + 0x00), readl(dsi->regs + 0x10),
		readl(dsi->regs + 0x14), readl(dsi->regs + 0x18));
	ret = mtk_dsi_poweron(dsi);
	if (ret < 0)
		DRM_ERROR("failed to power on dsi\n");
}

static void mtk_dsi_bridge_atomic_post_disable(struct drm_bridge *bridge,
					       struct drm_atomic_state *state)
{
	struct mtk_dsi *dsi = bridge_to_dsi(bridge);

	mtk_dsi_poweroff(dsi);
}

static enum drm_mode_status
mtk_dsi_bridge_mode_valid(struct drm_bridge *bridge,
			  const struct drm_display_info *info,
			  const struct drm_display_mode *mode)
{
	struct mtk_dsi *dsi = bridge_to_dsi(bridge);
	int bpp;

	/* Use the compressed bpp when the panel is DSC encoded */
	if (dsi->dsc_params.enable)
		bpp = dsi->dsc_params.bit_per_pixel / 16;
	else
		bpp = mipi_dsi_pixel_format_to_bpp(dsi->format);
	if (bpp < 0)
		return MODE_ERROR;

	if (mode->clock * bpp / dsi->lanes > 1500000)
		return MODE_CLOCK_HIGH;

	return MODE_OK;
}

static const struct drm_bridge_funcs mtk_dsi_bridge_funcs = {
	.attach = mtk_dsi_bridge_attach,
	.atomic_destroy_state = drm_atomic_helper_bridge_destroy_state,
	.atomic_disable = mtk_dsi_bridge_atomic_disable,
	.atomic_duplicate_state = drm_atomic_helper_bridge_duplicate_state,
	.atomic_enable = mtk_dsi_bridge_atomic_enable,
	.atomic_pre_enable = mtk_dsi_bridge_atomic_pre_enable,
	.atomic_post_disable = mtk_dsi_bridge_atomic_post_disable,
	.atomic_reset = drm_atomic_helper_bridge_reset,
	.mode_valid = mtk_dsi_bridge_mode_valid,
	.mode_set = mtk_dsi_bridge_mode_set,
};

void mtk_dsi_ddp_start(struct device *dev)
{
	struct mtk_dsi *dsi = dev_get_drvdata(dev);

	mtk_dsi_poweron(dsi);
}

void mtk_dsi_ddp_stop(struct device *dev)
{
	struct mtk_dsi *dsi = dev_get_drvdata(dev);

	mtk_dsi_poweroff(dsi);
}

static int mtk_dsi_encoder_init(struct drm_device *drm, struct mtk_dsi *dsi)
{
	int ret;

	ret = drm_simple_encoder_init(drm, &dsi->encoder,
				      DRM_MODE_ENCODER_DSI);
	if (ret) {
		DRM_ERROR("Failed to encoder init to drm\n");
		return ret;
	}

	ret = mtk_find_possible_crtcs(drm, dsi->host.dev);
	if (ret < 0)
		goto err_cleanup_encoder;
	dsi->encoder.possible_crtcs = ret;

	ret = drm_bridge_attach(&dsi->encoder, &dsi->bridge, NULL,
				DRM_BRIDGE_ATTACH_NO_CONNECTOR);
	if (ret)
		goto err_cleanup_encoder;

	dsi->connector = drm_bridge_connector_init(drm, &dsi->encoder);
	if (IS_ERR(dsi->connector)) {
		DRM_ERROR("Unable to create bridge connector\n");
		ret = PTR_ERR(dsi->connector);
		goto err_cleanup_encoder;
	}
	drm_connector_attach_encoder(dsi->connector, &dsi->encoder);

	return 0;

err_cleanup_encoder:
	drm_encoder_cleanup(&dsi->encoder);
	return ret;
}

unsigned int mtk_dsi_encoder_index(struct device *dev)
{
	struct mtk_dsi *dsi = dev_get_drvdata(dev);
	unsigned int encoder_index = drm_encoder_index(&dsi->encoder);

	dev_dbg(dev, "encoder index:%d\n", encoder_index);
	return encoder_index;
}

static int mtk_dsi_bind(struct device *dev, struct device *master, void *data)
{
	int ret;
	struct drm_device *drm = data;
	struct mtk_dsi *dsi = dev_get_drvdata(dev);
	struct mtk_drm_private *priv = drm->dev_private;
	struct mtk_ddp_comp *dsc_comp;

	ret = mtk_dsi_encoder_init(drm, dsi);
	if (ret)
		return ret;

	/* hand the panel's DSC params to the DSC0 ddp component (if present) */
	dsc_comp = &priv->ddp_comp[DDP_COMPONENT_DSC0];
	if (dsc_comp->dev && dsi->dsc_params.enable)
		mtk_ddp_comp_dsc_set_config(dsc_comp->dev, &dsi->dsc_params);

	return device_reset_optional(dev);
}

static void mtk_dsi_unbind(struct device *dev, struct device *master,
			   void *data)
{
	struct mtk_dsi *dsi = dev_get_drvdata(dev);

	drm_encoder_cleanup(&dsi->encoder);
}

static const struct component_ops mtk_dsi_component_ops = {
	.bind = mtk_dsi_bind,
	.unbind = mtk_dsi_unbind,
};

static int mtk_dsi_host_attach(struct mipi_dsi_host *host,
			       struct mipi_dsi_device *device)
{
	struct mtk_dsi *dsi = host_to_dsi(host);
	struct device *dev = host->dev;
	int ret;

	dsi->lanes = device->lanes;
	dsi->format = device->format;
	dsi->mode_flags = device->mode_flags;
	if (device->dsc)
		mtk_dsi_fill_dsc_params(dsi, device->dsc);
	dsi->next_bridge = devm_drm_of_get_bridge(dev, dev->of_node, 1, 0);
	if (IS_ERR(dsi->next_bridge)) {
		ret = PTR_ERR(dsi->next_bridge);
		if (ret == -EPROBE_DEFER)
			return ret;

		/* Old devicetree has only one endpoint */
		dsi->next_bridge = devm_drm_of_get_bridge(dev, dev->of_node, 0, 0);
		if (IS_ERR(dsi->next_bridge))
			return PTR_ERR(dsi->next_bridge);
	}
	DRM_INFO("XAGA-DSI: next_bridge=%p\n", dsi->next_bridge);

	/*
	 * set flag to request the DSI host bridge be pre-enabled before device bridge
	 * in the chain, so the DSI host is ready when the device bridge is pre-enabled
	 */
	dsi->next_bridge->pre_enable_prev_first = true;

	drm_bridge_add(&dsi->bridge);

	ret = component_add(host->dev, &mtk_dsi_component_ops);
	if (ret) {
		DRM_ERROR("failed to add dsi_host component: %d\n", ret);
		drm_bridge_remove(&dsi->bridge);
		return ret;
	}

	return 0;
}

static int mtk_dsi_host_detach(struct mipi_dsi_host *host,
			       struct mipi_dsi_device *device)
{
	struct mtk_dsi *dsi = host_to_dsi(host);

	component_del(host->dev, &mtk_dsi_component_ops);
	drm_bridge_remove(&dsi->bridge);
	return 0;
}

static void mtk_dsi_wait_for_idle(struct mtk_dsi *dsi)
{
	int ret;
	u32 val;

	ret = readl_poll_timeout(dsi->regs + DSI_INTSTA, val, !(val & DSI_BUSY),
				 4, 2000000);
	if (ret) {
		DRM_WARN("polling dsi wait not busy timeout!\n");

		mtk_dsi_enable(dsi);
		mtk_dsi_reset_engine(dsi);
	}
}

static u32 mtk_dsi_recv_cnt(u8 type, u8 *read_data)
{
	switch (type) {
	case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_1BYTE:
	case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_1BYTE:
		return 1;
	case MIPI_DSI_RX_GENERIC_SHORT_READ_RESPONSE_2BYTE:
	case MIPI_DSI_RX_DCS_SHORT_READ_RESPONSE_2BYTE:
		return 2;
	case MIPI_DSI_RX_GENERIC_LONG_READ_RESPONSE:
	case MIPI_DSI_RX_DCS_LONG_READ_RESPONSE:
		return read_data[1] + read_data[2] * 16;
	case MIPI_DSI_RX_ACKNOWLEDGE_AND_ERROR_REPORT:
		DRM_INFO("type is 0x02, try again\n");
		break;
	default:
		DRM_INFO("type(0x%x) not recognized\n", type);
		break;
	}

	return 0;
}

static void mtk_dsi_cmdq(struct mtk_dsi *dsi, const struct mipi_dsi_msg *msg)
{
	const char *tx_buf = msg->tx_buf;
	u8 config, cmdq_size, cmdq_off, type = msg->type;
	u32 reg_val, cmdq_mask, i;
	u32 reg_cmdq_off = dsi->driver_data->reg_cmdq_off;

	if (MTK_DSI_HOST_IS_READ(type))
		config = BTA;
	else
		config = (msg->tx_len > 2) ? LONG_PACKET : SHORT_PACKET;

	if (msg->tx_len > 2) {
		cmdq_size = 1 + (msg->tx_len + 3) / 4;
		cmdq_off = 4;
		cmdq_mask = CONFIG | DATA_ID | DATA_0 | DATA_1;
		reg_val = (msg->tx_len << 16) | (type << 8) | config;
	} else {
		cmdq_size = 1;
		cmdq_off = 2;
		cmdq_mask = CONFIG | DATA_ID;
		reg_val = (type << 8) | config;
	}

	for (i = 0; i < msg->tx_len; i++)
		mtk_dsi_mask(dsi, (reg_cmdq_off + cmdq_off + i) & (~0x3U),
			     (0xffUL << (((i + cmdq_off) & 3U) * 8U)),
			     tx_buf[i] << (((i + cmdq_off) & 3U) * 8U));

	mtk_dsi_mask(dsi, reg_cmdq_off, cmdq_mask, reg_val);
	mtk_dsi_mask(dsi, DSI_CMDQ_SIZE, CMDQ_SIZE, cmdq_size);
	if (dsi->driver_data->cmdq_long_packet_ctl) {
		/* Disable setting cmdq_size automatically for long packets */
		mtk_dsi_mask(dsi, DSI_CMDQ_SIZE, CMDQ_SIZE_SEL, CMDQ_SIZE_SEL);
	}
}

static ssize_t mtk_dsi_host_send_cmd(struct mtk_dsi *dsi,
				     const struct mipi_dsi_msg *msg, u8 flag)
{
	mtk_dsi_wait_for_idle(dsi);
	mtk_dsi_irq_data_clear(dsi, flag);
	mtk_dsi_cmdq(dsi, msg);
	mtk_dsi_start(dsi);

	if (!mtk_dsi_wait_for_irq_done(dsi, flag, 2000))
		return -ETIME;
	else
		return 0;
}

/*
 * XAGA: video-mode command via the VM_CMD window - ported from the
 * downstream mtk_dsi_host_send_vm_cmd (mt6895 regs: CON=0x200,
 * DATA0=0x208/DATA10=0x218/DATA20=0x228/DATA30=0x238, VM_CMD_START=
 * DSI_START bit16). The panel init runs while the video stream keeps
 * streaming; each command executes during the next VFP window.
 * Stopping the stream / switching to cmd mode on this SoC wedges
 * DSI_BUSY forever (see the "polling dsi wait not busy timeout"
 * failures), so this is THE way to send panel commands.
 */
static ssize_t mtk_dsi_host_send_vm_cmd(struct mtk_dsi *dsi,
					const struct mipi_dsi_msg *msg)
{
	const char *tx_buf = msg->tx_buf;
	unsigned int loop_cnt = 0;
	u32 reg_val, config;
	int i, j;

	config = (msg->tx_len > 2) ? VM_LONG_PACKET : 0;

	if (msg->tx_len > 2) {
		for (i = 0; i < msg->tx_len; i += 4) {
			u32 val = 0, addr;

			for (j = i; j < i + 4 && j < msg->tx_len; j++)
				val += tx_buf[j] << ((j - i) * 8);
			if (i / 16 == 0)
				addr = 0x208 + (i % 16);
			else if (i / 16 == 1)
				addr = 0x218 + (i % 16);
			else if (i / 16 == 2)
				addr = 0x228 + (i % 16);
			else
				addr = 0x238 + (i % 16);
			writel(val, dsi->regs + addr);
		}
		reg_val = (msg->tx_len << 16) | (msg->type << 8) | config;
	} else if (msg->tx_len == 2) {
		reg_val = (tx_buf[1] << 24) | (tx_buf[0] << 16) |
			  (msg->type << 8) | config;
	} else {
		reg_val = (tx_buf[0] << 16) | (msg->type << 8) | config;
	}

	reg_val |= VM_CMD_EN | TS_VFP_EN;
	writel(reg_val, dsi->regs + dsi->driver_data->reg_vm_cmd_off);

	/* clear the done latch, trigger, then wait for the latch to SET =
	 * command executed (observed: bit5 sets ~100us after the trigger
	 * and stays latched until cleared). Waiting for it to clear is
	 * wrong: it never auto-clears, and without pacing we blast the
	 * whole init table into the 1-deep VM_CMD queue in ~1ms, losing
	 * most commands -> truncated panel init -> black screen. */
	mtk_dsi_mask(dsi, DSI_INTSTA, VM_CMD_DONE_INT, 0);
	mtk_dsi_mask(dsi, DSI_START, VM_CMD_START, 0);
	mtk_dsi_mask(dsi, DSI_START, VM_CMD_START, VM_CMD_START);

	while (loop_cnt < 100000) {
		if (readl(dsi->regs + DSI_INTSTA) & VM_CMD_DONE_INT) {
			if (loop_cnt > 100)
				pr_info("XAGA-DSI vmcmd done after %dus\n",
					loop_cnt);
			return 0;
		}
		loop_cnt++;
		udelay(1);
	}
	DRM_WARN("XAGA-DSI vm cmd type=0x%02x timeout, INTSTA=0x%08x\n",
		 msg->type, readl(dsi->regs + DSI_INTSTA));
	return -ETIME;
}

static ssize_t mtk_dsi_host_transfer(struct mipi_dsi_host *host,
				     const struct mipi_dsi_msg *msg)
{
	struct mtk_dsi *dsi = host_to_dsi(host);
	ssize_t recv_cnt;
	u8 read_data[16];
	void *src_addr;
	u8 irq_flag = CMD_DONE_INT_FLAG;
	u32 dsi_mode;
	int ret, i;

	dsi_mode = readl(dsi->regs + DSI_MODE_CTRL);
	if (dsi_mode & MODE) {
		/*
		 * XAGA: video mode - send the command via the VM_CMD
		 * window (downstream mtk_dsi_host_send_vm_cmd), executed
		 * during the next VFP. The stream is NOT stopped: stopping
		 * and switching modes wedges DSI_BUSY on mt6895 (verified
		 * with the "polling dsi wait not busy timeout" failures).
		 */
		return mtk_dsi_host_send_vm_cmd(dsi, msg);
	}

	if (MTK_DSI_HOST_IS_READ(msg->type))
		irq_flag |= LPRX_RD_RDY_INT_FLAG;

	mtk_dsi_lane_ready(dsi);

	ret = mtk_dsi_host_send_cmd(dsi, msg, irq_flag);
	if (ret)
		goto restore_dsi_mode;

	if (!MTK_DSI_HOST_IS_READ(msg->type)) {
		recv_cnt = 0;
		goto restore_dsi_mode;
	}

	if (!msg->rx_buf) {
		DRM_ERROR("dsi receive buffer size may be NULL\n");
		ret = -EINVAL;
		goto restore_dsi_mode;
	}

	for (i = 0; i < 16; i++)
		*(read_data + i) = readb(dsi->regs + DSI_RX_DATA0 + i);

	recv_cnt = mtk_dsi_recv_cnt(read_data[0], read_data);

	if (recv_cnt > 2)
		src_addr = &read_data[4];
	else
		src_addr = &read_data[1];

	if (recv_cnt > 10)
		recv_cnt = 10;

	if (recv_cnt > msg->rx_len)
		recv_cnt = msg->rx_len;

	if (recv_cnt)
		memcpy(msg->rx_buf, src_addr, recv_cnt);

	DRM_INFO("dsi get %zd byte data from the panel address(0x%x)\n",
		 recv_cnt, *((u8 *)(msg->tx_buf)));

restore_dsi_mode:
	if (dsi_mode & MODE) {
		mtk_dsi_set_mode(dsi);
		mtk_dsi_start(dsi);
	}

	return ret < 0 ? ret : recv_cnt;
}

static const struct mipi_dsi_host_ops mtk_dsi_ops = {
	.attach = mtk_dsi_host_attach,
	.detach = mtk_dsi_host_detach,
	.transfer = mtk_dsi_host_transfer,
};

static int mtk_dsi_probe(struct platform_device *pdev)
{
	struct mtk_dsi *dsi;
	struct device *dev = &pdev->dev;
	int irq_num;
	int ret;

	dsi = devm_drm_bridge_alloc(dev, struct mtk_dsi, bridge,
				    &mtk_dsi_bridge_funcs);
	if (IS_ERR(dsi))
		return PTR_ERR(dsi);

	dsi->driver_data = of_device_get_match_data(dev);

	dsi->engine_clk = devm_clk_get(dev, "engine");
	if (IS_ERR(dsi->engine_clk))
		return dev_err_probe(dev, PTR_ERR(dsi->engine_clk),
				     "Failed to get engine clock\n");


	dsi->digital_clk = devm_clk_get(dev, "digital");
	if (IS_ERR(dsi->digital_clk))
		return dev_err_probe(dev, PTR_ERR(dsi->digital_clk),
				     "Failed to get digital clock\n");

	dsi->hs_clk = devm_clk_get(dev, "hs");
	if (IS_ERR(dsi->hs_clk))
		return dev_err_probe(dev, PTR_ERR(dsi->hs_clk), "Failed to get hs clock\n");

	dsi->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(dsi->regs))
		return dev_err_probe(dev, PTR_ERR(dsi->regs), "Failed to ioremap memory\n");

	dsi->phy = devm_phy_get(dev, "dphy");
	if (IS_ERR(dsi->phy))
		return dev_err_probe(dev, PTR_ERR(dsi->phy), "Failed to get MIPI-DPHY\n");

	irq_num = platform_get_irq(pdev, 0);
	if (irq_num < 0)
		return irq_num;

	dsi->host.ops = &mtk_dsi_ops;
	dsi->host.dev = dev;
	ret = mipi_dsi_host_register(&dsi->host);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to register DSI host\n");

	ret = devm_request_irq(&pdev->dev, irq_num, mtk_dsi_irq,
			       IRQF_TRIGGER_NONE, dev_name(&pdev->dev), dsi);
	if (ret) {
		mipi_dsi_host_unregister(&dsi->host);
		return dev_err_probe(&pdev->dev, ret, "Failed to request DSI irq\n");
	}

	init_waitqueue_head(&dsi->irq_wait_queue);

	platform_set_drvdata(pdev, dsi);

	dsi->bridge.of_node = dev->of_node;
	dsi->bridge.type = DRM_MODE_CONNECTOR_DSI;

	return 0;
}

static void mtk_dsi_remove(struct platform_device *pdev)
{
	struct mtk_dsi *dsi = platform_get_drvdata(pdev);

	mtk_output_dsi_disable(dsi);
	mipi_dsi_host_unregister(&dsi->host);
}

static const struct mtk_dsi_driver_data mt8173_dsi_driver_data = {
	.reg_cmdq_off = 0x200,
	.reg_vm_cmd_off = 0x130,
	.reg_shadow_dbg_off = 0x190
};

static const struct mtk_dsi_driver_data mt2701_dsi_driver_data = {
	.reg_cmdq_off = 0x180,
	.reg_vm_cmd_off = 0x130,
	.reg_shadow_dbg_off = 0x190
};

static const struct mtk_dsi_driver_data mt8183_dsi_driver_data = {
	.reg_cmdq_off = 0x200,
	.reg_vm_cmd_off = 0x130,
	.reg_shadow_dbg_off = 0x190,
	.has_shadow_ctl = true,
	.has_size_ctl = true,
};

static const struct mtk_dsi_driver_data mt8186_dsi_driver_data = {
	.reg_cmdq_off = 0xd00,
	.reg_vm_cmd_off = 0x200,
	.reg_shadow_dbg_off = 0xc00,
	.has_shadow_ctl = true,
	.has_size_ctl = true,
};

static const struct mtk_dsi_driver_data mt8188_dsi_driver_data = {
	.reg_cmdq_off = 0xd00,
	.reg_vm_cmd_off = 0x200,
	.reg_shadow_dbg_off = 0xc00,
	.has_shadow_ctl = true,
	.has_size_ctl = true,
	.cmdq_long_packet_ctl = true,
	.support_per_frame_lp = true,
};

/*
 * mt6895 uses the same register layout as mt8188 but the downstream driver
 * (mediatek_v2) programs per-LINE vdo timing, not per-frame-lp. Using
 * per_frame_lp here wrote wrong HFP/HBP/HSA word counts (with HS_VB_PS_WC in
 * the high bits) -> the DSI couldn't build valid video lines and the panel
 * never consumed a stream, stalling the pipeline after a few OVL frames.
 */
static const struct mtk_dsi_driver_data mt6895_dsi_driver_data = {
	.reg_cmdq_off = 0xd00,
	.reg_vm_cmd_off = 0x200,
	.reg_shadow_dbg_off = 0xc00,
	.has_shadow_ctl = true,
	.has_size_ctl = true,
	.cmdq_long_packet_ctl = true,
};

static const struct of_device_id mtk_dsi_of_match[] = {
	{ .compatible = "mediatek,mt2701-dsi", .data = &mt2701_dsi_driver_data },
	{ .compatible = "mediatek,mt8173-dsi", .data = &mt8173_dsi_driver_data },
	{ .compatible = "mediatek,mt8183-dsi", .data = &mt8183_dsi_driver_data },
	{ .compatible = "mediatek,mt8186-dsi", .data = &mt8186_dsi_driver_data },
	{ .compatible = "mediatek,mt8188-dsi", .data = &mt8188_dsi_driver_data },
	{ .compatible = "mediatek,mt6895-dsi", .data = &mt6895_dsi_driver_data },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_dsi_of_match);

struct platform_driver mtk_dsi_driver = {
	.probe = mtk_dsi_probe,
	.remove = mtk_dsi_remove,
	.driver = {
		.name = "mtk-dsi",
		.of_match_table = mtk_dsi_of_match,
	},
};
