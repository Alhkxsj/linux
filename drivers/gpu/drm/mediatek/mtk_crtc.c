// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015 MediaTek Inc.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/mailbox_controller.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/soc/mediatek/mtk-cmdq.h>
#include <linux/soc/mediatek/mtk-mmsys.h>
#include <linux/soc/mediatek/mtk-mutex.h>

#include <asm/barrier.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "mtk_crtc.h"
#include "mtk_ddp_comp.h"
#include "mtk_drm_drv.h"
#include "mtk_gem.h"
#include "mtk_plane.h"

/*
 * struct mtk_crtc - MediaTek specific crtc structure.
 * @base: crtc object.
 * @enabled: records whether crtc_enable succeeded
 * @planes: array of 4 drm_plane structures, one for each overlay plane
 * @pending_planes: whether any plane has pending changes to be applied
 * @mmsys_dev: pointer to the mmsys device for configuration registers
 * @mutex: handle to one of the ten disp_mutex streams
 * @ddp_comp_nr: number of components in ddp_comp
 * @ddp_comp: array of pointers the mtk_ddp_comp structures used by this crtc
 *
 * TODO: Needs update: this header is missing a bunch of member descriptions.
 */
struct mtk_crtc {
	struct drm_crtc			base;
	bool				enabled;

	bool				pending_needs_vblank;
	struct drm_pending_vblank_event	*event;

	struct drm_plane		*planes;
	unsigned int			layer_nr;
	bool				pending_planes;
	bool				pending_async_planes;

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	struct cmdq_client		cmdq_client;
	struct cmdq_pkt			cmdq_handle;
	u32				cmdq_event;
	u32				cmdq_vblank_cnt;
	wait_queue_head_t		cb_blocking_queue;
#endif

	struct device			*mmsys_dev;
	struct device			*dma_dev;
	struct mtk_mutex		*mutex;
	unsigned int			ddp_comp_nr;
	struct mtk_ddp_comp		**ddp_comp;
	unsigned int			num_conn_routes;
	const struct mtk_drm_route	*conn_routes;

	/* lock for display hardware access */
	struct mutex			hw_lock;
	bool				config_updating;
	/* lock for config_updating to cmd buffer */
	spinlock_t			config_lock;
};

struct mtk_crtc_state {
	struct drm_crtc_state		base;

	bool				pending_config;
	unsigned int			pending_width;
	unsigned int			pending_height;
	unsigned int			pending_vrefresh;
};

static inline struct mtk_crtc *to_mtk_crtc(struct drm_crtc *c)
{
	return container_of(c, struct mtk_crtc, base);
}

static inline struct mtk_crtc_state *to_mtk_crtc_state(struct drm_crtc_state *s)
{
	return container_of(s, struct mtk_crtc_state, base);
}

static void mtk_crtc_finish_page_flip(struct mtk_crtc *mtk_crtc)
{
	struct drm_crtc *crtc = &mtk_crtc->base;
	unsigned long flags;

	if (mtk_crtc->event) {
		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		drm_crtc_send_vblank_event(crtc, mtk_crtc->event);
		drm_crtc_vblank_put(crtc);
		mtk_crtc->event = NULL;
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	}
}

static void mtk_drm_finish_page_flip(struct mtk_crtc *mtk_crtc)
{
	unsigned long flags;

	drm_crtc_handle_vblank(&mtk_crtc->base);

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	if (mtk_crtc->cmdq_client.chan)
		return;
#endif

	spin_lock_irqsave(&mtk_crtc->config_lock, flags);
	if (!mtk_crtc->config_updating && mtk_crtc->pending_needs_vblank) {
		mtk_crtc_finish_page_flip(mtk_crtc);
		mtk_crtc->pending_needs_vblank = false;
	}
	spin_unlock_irqrestore(&mtk_crtc->config_lock, flags);
}

static void mtk_crtc_destroy(struct drm_crtc *crtc)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	int i;

	mtk_mutex_put(mtk_crtc->mutex);
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	if (mtk_crtc->cmdq_client.chan) {
		cmdq_pkt_destroy(&mtk_crtc->cmdq_client, &mtk_crtc->cmdq_handle);
		mbox_free_channel(mtk_crtc->cmdq_client.chan);
		mtk_crtc->cmdq_client.chan = NULL;
	}
#endif

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
		struct mtk_ddp_comp *comp;

		comp = mtk_crtc->ddp_comp[i];
		mtk_ddp_comp_unregister_vblank_cb(comp);
	}

	drm_crtc_cleanup(crtc);
}

static void mtk_crtc_reset(struct drm_crtc *crtc)
{
	struct mtk_crtc_state *state;

	if (crtc->state)
		__drm_atomic_helper_crtc_destroy_state(crtc->state);

	kfree(to_mtk_crtc_state(crtc->state));
	crtc->state = NULL;

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (state)
		__drm_atomic_helper_crtc_reset(crtc, &state->base);
}

static struct drm_crtc_state *mtk_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct mtk_crtc_state *state;

	state = kmalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_crtc_duplicate_state(crtc, &state->base);

	WARN_ON(state->base.crtc != crtc);
	state->base.crtc = crtc;
	state->pending_config = false;

	return &state->base;
}

static void mtk_crtc_destroy_state(struct drm_crtc *crtc,
				   struct drm_crtc_state *state)
{
	__drm_atomic_helper_crtc_destroy_state(state);
	kfree(to_mtk_crtc_state(state));
}

static enum drm_mode_status
mtk_crtc_mode_valid(struct drm_crtc *crtc, const struct drm_display_mode *mode)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	enum drm_mode_status status = MODE_OK;
	int i;

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
		status = mtk_ddp_comp_mode_valid(mtk_crtc->ddp_comp[i], mode);
		if (status != MODE_OK)
			break;
	}
	return status;
}

static bool mtk_crtc_mode_fixup(struct drm_crtc *crtc,
				const struct drm_display_mode *mode,
				struct drm_display_mode *adjusted_mode)
{
	/* Nothing to do here, but this callback is mandatory. */
	return true;
}

static void mtk_crtc_mode_set_nofb(struct drm_crtc *crtc)
{
	struct mtk_crtc_state *state = to_mtk_crtc_state(crtc->state);
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct drm_display_mode *mode = &crtc->state->adjusted_mode;
	int i;

	pr_info("XAGA-STAGE mode_set_nofb: crtc%d mode %dx%d@%d clock=%d\n",
		drm_crtc_index(crtc), mode->hdisplay, mode->vdisplay,
		drm_mode_vrefresh(mode), mode->clock);
	for (i = 0; i < mtk_crtc->layer_nr; i++) {
		struct drm_plane *plane = &mtk_crtc->planes[i];

		if (plane->state->fb)
			pr_info("XAGA-STAGE   plane%d fb=%dx%d pitch=%d\n", i,
				plane->state->fb->width, plane->state->fb->height,
				plane->state->fb->pitches[0]);
	}

	state->pending_width = crtc->mode.hdisplay;
	state->pending_height = crtc->mode.vdisplay;
	state->pending_vrefresh = drm_mode_vrefresh(&crtc->mode);
	wmb();	/* Make sure the above parameters are set before update */
	state->pending_config = true;
}

static int mtk_crtc_ddp_clk_enable(struct mtk_crtc *mtk_crtc)
{
	int ret;
	int i;

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
		ret = mtk_ddp_comp_clk_enable(mtk_crtc->ddp_comp[i]);
		if (ret) {
			DRM_ERROR("Failed to enable clock %d: %d\n", i, ret);
			goto err;
		}
	}

	return 0;
err:
	while (--i >= 0)
		mtk_ddp_comp_clk_disable(mtk_crtc->ddp_comp[i]);
	return ret;
}

static void mtk_crtc_ddp_clk_disable(struct mtk_crtc *mtk_crtc)
{
	int i;

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++)
		mtk_ddp_comp_clk_disable(mtk_crtc->ddp_comp[i]);
}

static
struct mtk_ddp_comp *mtk_ddp_comp_for_plane(struct drm_crtc *crtc,
					    struct drm_plane *plane,
					    unsigned int *local_layer)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_ddp_comp *comp;
	int i, count = 0;
	unsigned int local_index = plane - mtk_crtc->planes;

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
		comp = mtk_crtc->ddp_comp[i];
		if (local_index < (count + mtk_ddp_comp_layer_nr(comp))) {
			*local_layer = local_index - count;
			return comp;
		}
		count += mtk_ddp_comp_layer_nr(comp);
	}

	WARN(1, "Failed to find component for plane %d\n", plane->index);
	return NULL;
}

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
static void ddp_cmdq_cb(struct mbox_client *cl, void *mssg)
{
	struct cmdq_cb_data *data = mssg;
	struct cmdq_client *cmdq_cl = container_of(cl, struct cmdq_client, client);
	struct mtk_crtc *mtk_crtc = container_of(cmdq_cl, struct mtk_crtc, cmdq_client);
	struct mtk_crtc_state *state;
	unsigned int i;
	unsigned long flags;

	/* release GCE HW usage and start autosuspend */
	pm_runtime_mark_last_busy(cmdq_cl->chan->mbox->dev);
	pm_runtime_put_autosuspend(cmdq_cl->chan->mbox->dev);

	if (data->sta < 0)
		return;

	state = to_mtk_crtc_state(mtk_crtc->base.state);

	spin_lock_irqsave(&mtk_crtc->config_lock, flags);
	if (mtk_crtc->config_updating)
		goto ddp_cmdq_cb_out;

	state->pending_config = false;

	if (mtk_crtc->pending_planes) {
		for (i = 0; i < mtk_crtc->layer_nr; i++) {
			struct drm_plane *plane = &mtk_crtc->planes[i];
			struct mtk_plane_state *plane_state;

			plane_state = to_mtk_plane_state(plane->state);

			plane_state->pending.config = false;
		}
		mtk_crtc->pending_planes = false;
	}

	if (mtk_crtc->pending_async_planes) {
		for (i = 0; i < mtk_crtc->layer_nr; i++) {
			struct drm_plane *plane = &mtk_crtc->planes[i];
			struct mtk_plane_state *plane_state;

			plane_state = to_mtk_plane_state(plane->state);

			plane_state->pending.async_config = false;
		}
		mtk_crtc->pending_async_planes = false;
	}

ddp_cmdq_cb_out:

	if (mtk_crtc->pending_needs_vblank) {
		mtk_crtc_finish_page_flip(mtk_crtc);
		mtk_crtc->pending_needs_vblank = false;
	}

	spin_unlock_irqrestore(&mtk_crtc->config_lock, flags);

	mtk_crtc->cmdq_vblank_cnt = 0;
	wake_up(&mtk_crtc->cb_blocking_queue);
}
#endif

static void xaga_ovl_golden_setting(void __iomem *base)
{
	int i;

	/* MT6895 OVL RDMA golden settings (downstream mtk_ovl_golden_setting,
	 * non-DC path: is_dc=0). These program the SMI request pacing +
	 * FIFO watermarks so the RDMA fetch does not underflow. Without them
	 * the OVL1_2L RDMA latches SMI_UNDERFLOW (INTSTA bit9) -> the DSC
	 * input frame is truncated -> DSC ABN_EOF -> split-screen corruption.
	 */
	for (i = 0; i < 4; i++) {
		writel_relaxed(0x03ff03ff, base + 0xc8 + 0x20 * i); /* MEM_GMC_SETTING_1 */
		writel_relaxed(0x01800000, base + 0xd0 + 0x20 * i); /* FIFO_CTRL */
		writel_relaxed(0x207f00ff, base + 0x1e0 + 4 * i);   /* MEM_GMC_SETTING_2 */
		writel_relaxed(0x00000000, base + 0x210 + 4 * i);   /* BUF_LOW */
		writel_relaxed(0x80000000, base + 0x220 + 4 * i);   /* BUF_HIGH */
	}
	writel_relaxed(0xf1ff7777, base + 0x1f8);	/* GREQ_NUM */
	writel_relaxed(0x00007777, base + 0x1fc);	/* GREQ_URG_NUM */
	writel_relaxed(0x00008040, base + 0x20c);	/* ULTRA_SRC */
}

/*
 * XAGA: the mainline DT has no smi/larb node, so the SMI driver never
 * probes the display LARB0 (0x14021000) and it is left in LK's default
 * state (NONSEC_CON=0, OSTDL=0x7f7f). Downstream's mtk-smi driver tunes
 * these per port; 0x7f7f outstanding throttles the OVL1_2L fetch ->
 * SMI_UNDERFLOW -> DSC ABN_EOF -> structural corruption. Reproduce the
 * downstream LARB0 values (captured from the working recovery kernel).
 */
static void xaga_larb0_config(void)
{
	static const u32 nonsec[8] = {
		0x00000000, 0x00000000, 0x00000001, 0x00000000,
		0x00000001, 0x00000000, 0x00000000, 0x00000003,
	};
	static const u32 ostdl[8] = {
		0x00000202, 0x00000606, 0x00000606, 0x00000404,
		0x00000404, 0x00000202, 0x00000202, 0x00004040,
	};
	void __iomem *larb = ioremap(0x14021000, 0x1000);
	int i;

	if (!larb)
		return;
	for (i = 0; i < 8; i++) {
		writel_relaxed(nonsec[i], larb + 0x380 + 4 * i);
		writel_relaxed(ostdl[i], larb + 0x200 + 4 * i);
	}
	iounmap(larb);
}

/*
 * XAGA: minimal PQ-chain driver for the LEFT slice (OVL0 -> PQ -> DSC_L).
 * The mainline DDP path has no PQ components, so these blocks sit in LK's
 * one-time state and freeze -> the left slice never updates -> the DSC's
 * interleaved stream is corrupted. Re-assert EN + shadow-bypass + START on
 * the 12 PQ blocks (and sodi pacing) so they stay live. Called once at
 * hw_init and again on every commit (re-drive).
 */
void xaga_pq_config(void)
{
	static const struct {
		phys_addr_t base;
		u32 reg;
		u32 val;
		u32 mask;
	} pq[] = {
		{0x14007000, 0x67c, 1, 0x1},	/* TDSHP0 shadow bypass */
		{0x14007000, 0x100, 1, 0x1},	/* TDSHP0 CTRL EN */
		{0x14008000, 0x000, 1, 0x1},	/* C3D0 EN */
		{0x14009000, 0xcb0, 1, 0x1},	/* COLOR0 shadow bypass */
		{0x14009000, 0xc00, 1, 0x3},	/* COLOR0 START */
		{0x1400a000, 0x000, 1, 0x1},	/* CCORR0 EN */
		{0x1400b000, 0x000, 1, 0x1},	/* CCORR1 EN */
		{0x1400d000, 0x000, 1, 0x1},	/* AAL0 EN */
		{0x1400e000, 0x000, 1, 0x1},	/* GAMMA0 EN */
		{0x1400f000, 0x000, 1, 0x1},	/* POSTMASK0 EN */
		{0x14010000, 0x000, 1, 0x1},	/* DITHER0 EN */
		{0x14013000, 0x000, 1, 0x1},	/* CM0 EN */
		{0x14014000, 0x00c, 1, 0x1},	/* SPR0 EN */
	};
	void __iomem *mx = ioremap(0x14000000, 0x1000);
	int p;

	for (p = 0; p < ARRAY_SIZE(pq); p++) {
		void __iomem *b = ioremap(pq[p].base, 0x1000);
		u32 v;

		if (!b)
			continue;
		v = readl(b + pq[p].reg);
		v &= ~pq[p].mask;
		v |= pq[p].val & pq[p].mask;
		writel_relaxed(v, b + pq[p].reg);
		iounmap(b);
	}
	/* sodi: DSI-buffer SMI request source + ultra routing */
	if (mx) {
		writel_relaxed(0xF500, mx + 0xF4);
		writel_relaxed(0xdf, mx + 0xF8);
		writel_relaxed(0x7, mx + 0x400);
		{
			u32 v = readl(mx + 0xF0);

			v &= ~((0x3 << 10) | (0x3 << 2));
			writel_relaxed(v, mx + 0xF0);
		}
		iounmap(mx);
	}
}

/* XAGA: measure the ACTUAL frame rate by counting DSC frame-done (INTSTA
 * bit0) over a fixed window. Verifies the DSI pixel clock / PLL output is
 * really 60Hz (a solid fill showing tinted stripes = the DSC stream is
 * misaligned = clock/frame-rate drift). */
void xaga_measure_fps(void)
{
	void __iomem *dsc = ioremap(0x14015000, 0x1000);
	unsigned long deadline;
	unsigned int count = 0;

	if (!dsc)
		return;

	writel_relaxed(readl(dsc + 0x8), dsc + 0xc);
	writel_relaxed(0x0, dsc + 0xc);

	deadline = jiffies + msecs_to_jiffies(500);
	while (time_before(jiffies, deadline)) {
		u32 st = readl(dsc + 0x8);

		if (st & 0x1) {
			count++;
			writel_relaxed(st, dsc + 0xc);
			writel_relaxed(0x0, dsc + 0xc);
		}
		udelay(50);
	}

	pr_info("XAGA-FPS: %u frames in 500ms => ~%u Hz\n", count, count * 2);
	iounmap(dsc);
}

static void xaga_fps_worker(struct work_struct *work)
{
	xaga_measure_fps();
}

static int mtk_crtc_ddp_hw_init(struct mtk_crtc *mtk_crtc)
{
	struct drm_crtc *crtc = &mtk_crtc->base;
	struct drm_connector *connector;
	struct drm_encoder *encoder;
	struct drm_connector_list_iter conn_iter;
	unsigned int width, height, vrefresh, bpc = MTK_MAX_BPC;
	int ret;
	int i;

	if (WARN_ON(!crtc->state))
		return -EINVAL;

	width = crtc->state->adjusted_mode.hdisplay;
	height = crtc->state->adjusted_mode.vdisplay;
	vrefresh = drm_mode_vrefresh(&crtc->state->adjusted_mode);

	pr_info("XAGA-STAGE ddp_hw_init: %dx%d@%d comp_nr=%d\n",
		width, height, vrefresh, mtk_crtc->ddp_comp_nr);
	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++)
		pr_info("XAGA-STAGE   comp[%d] id=%d\n", i,
			mtk_crtc->ddp_comp[i]->id);

	drm_for_each_encoder(encoder, crtc->dev) {
		if (encoder->crtc != crtc)
			continue;

		drm_connector_list_iter_begin(crtc->dev, &conn_iter);
		drm_for_each_connector_iter(connector, &conn_iter) {
			if (connector->encoder != encoder)
				continue;
			if (connector->display_info.bpc != 0 &&
			    bpc > connector->display_info.bpc)
				bpc = connector->display_info.bpc;
		}
		drm_connector_list_iter_end(&conn_iter);
	}

	ret = pm_runtime_resume_and_get(crtc->dev->dev);
	if (ret < 0) {
		DRM_ERROR("Failed to enable power domain: %d\n", ret);
		return ret;
	}

	ret = mtk_mutex_prepare(mtk_crtc->mutex);
	if (ret < 0) {
		DRM_ERROR("Failed to enable mutex clock: %d\n", ret);
		goto err_pm_runtime_put;
	}

	ret = mtk_crtc_ddp_clk_enable(mtk_crtc);
	if (ret < 0) {
		DRM_ERROR("Failed to enable component clocks: %d\n", ret);
		goto err_mutex_unprepare;
	}

	for (i = 0; i < mtk_crtc->ddp_comp_nr - 1; i++) {
		if (!mtk_ddp_comp_connect(mtk_crtc->ddp_comp[i], mtk_crtc->mmsys_dev,
					  mtk_crtc->ddp_comp[i + 1]->id))
			mtk_mmsys_ddp_connect(mtk_crtc->mmsys_dev,
					      mtk_crtc->ddp_comp[i]->id,
					      mtk_crtc->ddp_comp[i + 1]->id);
		if (!mtk_ddp_comp_add(mtk_crtc->ddp_comp[i], mtk_crtc->mutex))
			mtk_mutex_add_comp(mtk_crtc->mutex,
					   mtk_crtc->ddp_comp[i]->id);
	}
	if (!mtk_ddp_comp_add(mtk_crtc->ddp_comp[i], mtk_crtc->mutex))
		mtk_mutex_add_comp(mtk_crtc->mutex, mtk_crtc->ddp_comp[i]->id);
	mtk_mutex_enable(mtk_crtc->mutex);

	/*
	 * XAGA: full pipeline stop before config, mirroring downstream's
	 * enable order (stop_vdo -> comp stop/soft-reset -> config+start ->
	 * DSI start last). On grey boots LK's video start raced: the DSC
	 * latched ABN_EOF at handoff, RDMA0 never completes a frame
	 * (INT_STA=0x0), DSI shows BUFFER_UNDERRUN+INP_UNFINISH. A DSI-only
	 * restart cannot revive that stalled chain; the RDMA0 must be soft
	 * reset too.
	 */
	{
		void __iomem *dsi = ioremap(0x14017000, 0x1000);
		void __iomem *rdma = ioremap(0x14006000, 0x1000);
		void __iomem *ovl = ioremap(0x14002000, 0x1000);
		/* XAGA: leave the DSI untouched. LK is streaming a healthy video
		 * stream (handoff DSC INTSTA=0x1) and the panel's DSC decoder is
		 * locked to it. Stopping/restarting the DSI (START=0->1) resets
		 * the panel decoder's sync -> permanent vertical-stripe corruption.
		 * The encoder itself stays clean (DSC INTSTA=0x1). Only reconfigure
		 * the OVL/RDMA source below. */

		/* 2. RDMA0 soft reset (downstream mtk_rdma_stop/start) */
		writel_relaxed(0x0, rdma + 0x00);	/* INT_EN = 0 */
		writel_relaxed(BIT(4), rdma + 0x10);	/* GLOBAL = SOFT_RESET */
		writel_relaxed(0x0, rdma + 0x10);	/* GLOBAL = 0 */
		writel_relaxed(0x0, rdma + 0x04);	/* INT_STA = 0 */

		/* 3. OVL0 reset + relay restore */
		writel_relaxed(0x1, ovl + 0x14);	/* OVL RST = 1 */
		writel_relaxed(0x0, ovl + 0x14);	/* OVL RST = 0 */
		writel_relaxed(0x100, ovl + 0x2c);	/* SRC_CON = FORCE_RELAY_MODE */
		writel_relaxed(0x07000005, ovl + 0x24); /* DATAPATH = LK value */
		writel_relaxed(0xe00001, ovl + 0x0c);	/* EN = LK value (bit22 BYPASS_SHADOW!) */
		/* Disable OVL0's leftover LK layers (matches the working device,
		 * where OVL0 L1 has RDMA_CTRL=0 after all_layer_off). */
		writel_relaxed(0x0, ovl + 0xc0);	/* L0 RDMA_CTRL = 0 */
		writel_relaxed(0x0, ovl + 0xe0);	/* L1 RDMA_CTRL = 0 */

		/* XAGA: apply the downstream OVL RDMA golden settings (SMI
		 * request pacing + FIFO watermarks) to BOTH OVLs. The OVL1_2L
		 * is the pipe-2 fetch source; without these it underflows. */
		xaga_ovl_golden_setting(ovl);
		{
			void __iomem *ovl12 = ioremap(0x14004000, 0x1000);

			if (ovl12) {
				xaga_ovl_golden_setting(ovl12);
				iounmap(ovl12);
			}
		}
		xaga_larb0_config();
		xaga_pq_config();
		pr_info("XAGA-stop: minimal restart + OVL golden settings + LARB0 + PQ\n");

		iounmap(dsi);
		iounmap(rdma);
		iounmap(ovl);
	}

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
		struct mtk_ddp_comp *comp = mtk_crtc->ddp_comp[i];

		if (i == 1)
			mtk_ddp_comp_bgclr_in_on(comp);

		mtk_ddp_comp_config(comp, width, height, vrefresh, bpc, NULL);
		mtk_ddp_comp_start(comp);
	}

	pr_info("XAGA-STAGE ddp_hw_init: after comp config+start\n");
	{
		void __iomem *mx = ioremap(0x14000000, 0x1000);
		void __iomem *mu = ioremap(0x14001000, 0x1000);
		void __iomem *ovl = ioremap(0x14002000, 0x1000);
		void __iomem *ovl12 = ioremap(0x14004000, 0x1000);
		void __iomem *dsc = ioremap(0x14015000, 0x1000);
		void __iomem *rdma = ioremap(0x14006000, 0x1000);
		void __iomem *dsi = ioremap(0x14017000, 0x1000);

		pr_info("XAGA[hw_init] MUTEX EN=0x%08x SOF=0x%08x MOD0=0x%08x MOD1=0x%08x\n",
			readl(mu + 0x20), readl(mu + 0x2c), readl(mu + 0x30),
			readl(mu + 0x34));
		pr_info("XAGA[hw_init] OVL0 EN=0x%08x INTSTA=0x%08x ROI=0x%08x SRC_CON=0x%08x DATAPATH=0x%08x BGCLR=0x%08x\n",
			readl(ovl + 0x0c), readl(ovl + 0x08), readl(ovl + 0x20),
			readl(ovl + 0x2c), readl(ovl + 0x24), readl(ovl + 0x28));
		pr_info("XAGA[hw_init] OVL0 L0 CON=0x%08x SRC_SIZE=0x%08x PITCH=0x%08x RDMA_CTRL=0x%08x ADDR=0x%08x\n",
			readl(ovl + 0x30), readl(ovl + 0x38), readl(ovl + 0x44),
			readl(ovl + 0xc0), readl(ovl + 0xf40));
		pr_info("XAGA[hw_init] OVL0 L1 CON=0x%08x SRC_SIZE=0x%08x PITCH=0x%08x RDMA_CTRL=0x%08x ADDR=0x%08x\n",
			readl(ovl + 0x50), readl(ovl + 0x58), readl(ovl + 0x64),
			readl(ovl + 0xe0), readl(ovl + 0xf60));
		pr_info("XAGA[hw_init] OVL1_2L EN=0x%08x INTSTA=0x%08x ROI=0x%08x SRC_CON=0x%08x DATAPATH=0x%08x BGCLR=0x%08x\n",
			readl(ovl12 + 0x0c), readl(ovl12 + 0x08),
			readl(ovl12 + 0x20), readl(ovl12 + 0x2c),
			readl(ovl12 + 0x24), readl(ovl12 + 0x28));
		pr_info("XAGA[hw_init] OVL1_2L L0 CON=0x%08x SRC_SIZE=0x%08x PITCH=0x%08x RDMA_CTRL=0x%08x ADDR=0x%08x\n",
			readl(ovl12 + 0x30), readl(ovl12 + 0x38),
			readl(ovl12 + 0x44), readl(ovl12 + 0xc0),
			readl(ovl12 + 0xf40));
		pr_info("XAGA[hw_init] OVL1_2L GOLD GREQ=0x%08x GREQ_URG=0x%08x ULTRA_SRC=0x%08x GMC0=0x%08x FIFO0=0x%08x GMC_S2_0=0x%08x BUF_LOW0=0x%08x BUF_HIGH0=0x%08x\n",
			readl(ovl12 + 0x1f8), readl(ovl12 + 0x1fc),
			readl(ovl12 + 0x20c), readl(ovl12 + 0xc8),
			readl(ovl12 + 0xd0), readl(ovl12 + 0x1e0),
			readl(ovl12 + 0x210), readl(ovl12 + 0x220));
		pr_info("XAGA[hw_init] AID_SEL OVL0=0x%08x OVL0_2L=0x%08x OVL1_2L=0x%08x\n",
			readl(mx + 0xb00), readl(mx + 0xb04), readl(mx + 0xb08));
		{
			void __iomem *larb = ioremap(0x14021000, 0x1000);

			if (larb) {
				pr_info("XAGA[hw_init] LARB0 NONSEC[0..7]=%08x %08x %08x %08x %08x %08x %08x %08x\n",
					readl(larb + 0x380), readl(larb + 0x384),
					readl(larb + 0x388), readl(larb + 0x38c),
					readl(larb + 0x390), readl(larb + 0x394),
					readl(larb + 0x398), readl(larb + 0x39c));
				pr_info("XAGA[hw_init] LARB0 OSTDL[0..7]=%08x %08x %08x %08x %08x %08x %08x %08x\n",
					readl(larb + 0x200), readl(larb + 0x204),
					readl(larb + 0x208), readl(larb + 0x20c),
					readl(larb + 0x210), readl(larb + 0x214),
					readl(larb + 0x218), readl(larb + 0x21c));
				iounmap(larb);
			}
		}
		pr_info("XAGA[hw_init] RDMA0 GLOBAL=0x%08x SIZE0=0x%08x SIZE1=0x%08x FIFO=0x%08x INT_EN=0x%08x INT_STA=0x%08x\n",
			readl(rdma + 0x10), readl(rdma + 0x14),
			readl(rdma + 0x18), readl(rdma + 0x40),
			readl(rdma + 0x00), readl(rdma + 0x04));
		pr_info("XAGA[hw_init] DSC CON=0x%08x INTSTA=0x%08x MODE=0x%08x ENC_W=0x%08x SLICE_W=0x%08x SHADOW=0x%08x\n",
			readl(dsc + 0x00), readl(dsc + 0x08), readl(dsc + 0x30),
			readl(dsc + 0x3c), readl(dsc + 0x20),
			readl(dsc + 0x228));
		pr_info("XAGA[hw_init] DSI START=0x%08x INTSTA=0x%08x CON=0x%08x MODE=0x%08x TXRX=0x%08x PSCTRL=0x%08x SIZE_CON=0x%08x VM_CMD=0x%08x\n",
			readl(dsi + 0x00), readl(dsi + 0x0c), readl(dsi + 0x10),
			readl(dsi + 0x14), readl(dsi + 0x18), readl(dsi + 0x1c),
			readl(dsi + 0x38), readl(dsi + 0x200));
		pr_info("XAGA[hw_init] XBAR F24=0x%08x F8C=0x%08x F34=0x%08x F38=0x%08x F3C=0x%08x F40=0x%08x FAC=0x%08x FB4=0x%08x\n",
			readl(mx + 0xf24), readl(mx + 0xf8c), readl(mx + 0xf34),
			readl(mx + 0xf38), readl(mx + 0xf3c), readl(mx + 0xf40),
			readl(mx + 0xfac), readl(mx + 0xfb4));
		pr_info("XAGA[hw_init] XBAR FCC=0x%08x FD4=0x%08x FD8=0x%08x FDC=0x%08x F50=0x%08x F68=0x%08x F4C=0x%08x\n",
			readl(mx + 0xfcc), readl(mx + 0xfd4), readl(mx + 0xfd8),
			readl(mx + 0xfdc), readl(mx + 0xf50), readl(mx + 0xf68),
			readl(mx + 0xf4c));
		/* DLI0 async module (right-slice path DLI0->DSC_R, downstream
		 * mtk_dli_async_dump reads mmsys 0x26C/0x2C8) + RSZ0 EN. */
		{
			void __iomem *rsz = ioremap(0x14005000, 0x1000);

			pr_info("XAGA[hw_init] DLI0 0x26C=0x%08x 0x2C8=0x%08x RSZ0_EN=0x%08x\n",
				readl(mx + 0x26c), readl(mx + 0x2c8),
				rsz ? readl(rsz + 0x00) : 0xdead);
			if (rsz)
				iounmap(rsz);
		}
		{
			void __iomem *p0 = ioremap(0x14007000, 0x1000);
			void __iomem *p1 = ioremap(0x14008000, 0x1000);
			void __iomem *p2 = ioremap(0x14009000, 0x1000);
			void __iomem *p3 = ioremap(0x1400a000, 0x1000);
			void __iomem *p4 = ioremap(0x1400d000, 0x1000);
			void __iomem *p5 = ioremap(0x1400e000, 0x1000);
			void __iomem *p6 = ioremap(0x1400f000, 0x1000);
			void __iomem *p7 = ioremap(0x14010000, 0x1000);

			pr_info("XAGA[hw_init] PQ TDSHP0(0x100)=0x%08x C3D0(0)=0x%08x COLOR0(0xc00)=0x%08x CCORR0(0)=0x%08x AAL0(0)=0x%08x GAMMA0(0)=0x%08x POSTMASK0(0)=0x%08x DITHER0(0)=0x%08x\n",
				readl(p0 + 0x100), readl(p1 + 0x0),
				readl(p2 + 0xc00), readl(p3 + 0x0),
				readl(p4 + 0x0), readl(p5 + 0x0),
				readl(p6 + 0x0), readl(p7 + 0x0));
			iounmap(p0); iounmap(p1); iounmap(p2); iounmap(p3);
			iounmap(p4); iounmap(p5); iounmap(p6); iounmap(p7);
		}
		{
			void __iomem *tx = ioremap(0x11f70000, 0x1000);

			pr_info("XAGA[hw_init] MIPITX PLL_CON0=0x%08x PLL_CON1=0x%08x PLL_CON4=0x%08x LANE_CON=0x%08x VOLTAGE_SEL=0x%08x\n",
				readl(tx + 0x2c), readl(tx + 0x30),
				readl(tx + 0x3c), readl(tx + 0x04),
				readl(tx + 0x08));
			pr_info("XAGA[hw_init] MIPITX PWR=0x%08x PLL_CON2=0x%08x PLL_CON3=0x%08x SW_CTL_EN=0x%08x\n",
				readl(tx + 0x28), readl(tx + 0x34),
				readl(tx + 0x38), readl(tx + 0x15c));
			iounmap(tx);
		}
		/*
		 * XAGA: DSI NOT restarted. LK's stream stays running so the
		 * panel decoder keeps its lock. The OVL source is reconfigured
		 * above; the running DSI just carries the new content.
		 */
		iounmap(mx);
		iounmap(mu);
		iounmap(ovl);
		iounmap(ovl12);
		iounmap(dsc);
		iounmap(rdma);
		iounmap(dsi);
	}

	/* Initially configure all planes */
	for (i = 0; i < mtk_crtc->layer_nr; i++) {
		struct drm_plane *plane = &mtk_crtc->planes[i];
		struct mtk_plane_state *plane_state;
		struct mtk_ddp_comp *comp;
		unsigned int local_layer;

		plane_state = to_mtk_plane_state(plane->state);

		/* should not enable layer before crtc enabled */
		plane_state->pending.enable = false;
		comp = mtk_ddp_comp_for_plane(crtc, plane, &local_layer);
		if (comp)
			mtk_ddp_comp_layer_config(comp, local_layer,
						  plane_state, NULL);
	}

	/* XAGA: measure the actual frame rate once, ~3s after enable */
	{
		static struct delayed_work fps_work;
		static bool fps_armed;

		if (!fps_armed) {
			fps_armed = true;
			INIT_DELAYED_WORK(&fps_work, xaga_fps_worker);
			schedule_delayed_work(&fps_work, msecs_to_jiffies(3000));
		}
	}

	return 0;

err_mutex_unprepare:
	mtk_mutex_unprepare(mtk_crtc->mutex);
err_pm_runtime_put:
	pm_runtime_put(crtc->dev->dev);
	return ret;
}

static void mtk_crtc_ddp_hw_fini(struct mtk_crtc *mtk_crtc)
{
	struct drm_device *drm = mtk_crtc->base.dev;
	struct drm_crtc *crtc = &mtk_crtc->base;
	unsigned long flags;
	int i;

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
		mtk_ddp_comp_stop(mtk_crtc->ddp_comp[i]);
		if (i == 1)
			mtk_ddp_comp_bgclr_in_off(mtk_crtc->ddp_comp[i]);
	}

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++)
		if (!mtk_ddp_comp_remove(mtk_crtc->ddp_comp[i], mtk_crtc->mutex))
			mtk_mutex_remove_comp(mtk_crtc->mutex,
					      mtk_crtc->ddp_comp[i]->id);
	mtk_mutex_disable(mtk_crtc->mutex);
	for (i = 0; i < mtk_crtc->ddp_comp_nr - 1; i++) {
		if (!mtk_ddp_comp_disconnect(mtk_crtc->ddp_comp[i], mtk_crtc->mmsys_dev,
					     mtk_crtc->ddp_comp[i + 1]->id))
			mtk_mmsys_ddp_disconnect(mtk_crtc->mmsys_dev,
						 mtk_crtc->ddp_comp[i]->id,
						 mtk_crtc->ddp_comp[i + 1]->id);
		if (!mtk_ddp_comp_remove(mtk_crtc->ddp_comp[i], mtk_crtc->mutex))
			mtk_mutex_remove_comp(mtk_crtc->mutex,
					      mtk_crtc->ddp_comp[i]->id);
	}
	if (!mtk_ddp_comp_remove(mtk_crtc->ddp_comp[i], mtk_crtc->mutex))
		mtk_mutex_remove_comp(mtk_crtc->mutex, mtk_crtc->ddp_comp[i]->id);
	mtk_crtc_ddp_clk_disable(mtk_crtc);
	mtk_mutex_unprepare(mtk_crtc->mutex);

	pm_runtime_put(drm->dev);

	if (crtc->state->event && !crtc->state->active) {
		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		drm_crtc_send_vblank_event(crtc, crtc->state->event);
		crtc->state->event = NULL;
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	}
}

static void mtk_crtc_ddp_config(struct drm_crtc *crtc,
				struct cmdq_pkt *cmdq_handle)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_crtc_state *state = to_mtk_crtc_state(mtk_crtc->base.state);
	struct mtk_ddp_comp *comp = mtk_crtc->ddp_comp[0];
	unsigned int i;
	unsigned int local_layer;

	if (!mtk_crtc->base.state)
		return;

	/*
	 * TODO: instead of updating the registers here, we should prepare
	 * working registers in atomic_commit and let the hardware command
	 * queue update module registers on vblank.
	 */
	if (state->pending_config) {
		mtk_ddp_comp_config(comp, state->pending_width,
				    state->pending_height,
				    state->pending_vrefresh, 0,
				    cmdq_handle);

		if (!cmdq_handle)
			state->pending_config = false;
	}

	if (mtk_crtc->pending_planes) {
		for (i = 0; i < mtk_crtc->layer_nr; i++) {
			struct drm_plane *plane = &mtk_crtc->planes[i];
			struct mtk_plane_state *plane_state;

			plane_state = to_mtk_plane_state(plane->state);

			if (!plane_state->pending.config)
				continue;

			comp = mtk_ddp_comp_for_plane(crtc, plane, &local_layer);

			if (comp)
				mtk_ddp_comp_layer_config(comp, local_layer,
							  plane_state,
							  cmdq_handle);
			if (!cmdq_handle)
				plane_state->pending.config = false;
		}

		if (!cmdq_handle)
			mtk_crtc->pending_planes = false;
	}

	if (mtk_crtc->pending_async_planes) {
		for (i = 0; i < mtk_crtc->layer_nr; i++) {
			struct drm_plane *plane = &mtk_crtc->planes[i];
			struct mtk_plane_state *plane_state;

			plane_state = to_mtk_plane_state(plane->state);

			if (!plane_state->pending.async_config)
				continue;

			comp = mtk_ddp_comp_for_plane(crtc, plane, &local_layer);

			if (comp)
				mtk_ddp_comp_layer_config(comp, local_layer,
							  plane_state,
							  cmdq_handle);
			if (!cmdq_handle)
				plane_state->pending.async_config = false;
		}

		if (!cmdq_handle)
			mtk_crtc->pending_async_planes = false;
	}
}

static void mtk_crtc_update_config(struct mtk_crtc *mtk_crtc, bool needs_vblank)
{
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	struct cmdq_pkt *cmdq_handle = &mtk_crtc->cmdq_handle;
#endif
	struct drm_crtc *crtc = &mtk_crtc->base;
	struct mtk_drm_private *priv = crtc->dev->dev_private;
	unsigned int pending_planes = 0, pending_async_planes = 0;
	int i;
	unsigned long flags;

	mutex_lock(&mtk_crtc->hw_lock);

	spin_lock_irqsave(&mtk_crtc->config_lock, flags);
	mtk_crtc->config_updating = true;
	spin_unlock_irqrestore(&mtk_crtc->config_lock, flags);

	if (needs_vblank)
		mtk_crtc->pending_needs_vblank = true;

	pr_info("XAGA-STAGE update_config: crtc%d needs_vblank=%d\n",
		drm_crtc_index(crtc), needs_vblank ? 1 : 0);

	for (i = 0; i < mtk_crtc->layer_nr; i++) {
		struct drm_plane *plane = &mtk_crtc->planes[i];
		struct mtk_plane_state *plane_state;

		plane_state = to_mtk_plane_state(plane->state);
		if (plane_state->pending.dirty) {
			plane_state->pending.config = true;
			plane_state->pending.dirty = false;
			pending_planes |= BIT(i);
		} else if (plane_state->pending.async_dirty) {
			plane_state->pending.async_config = true;
			plane_state->pending.async_dirty = false;
			pending_async_planes |= BIT(i);
		}
	}
	if (pending_planes)
		mtk_crtc->pending_planes = true;
	if (pending_async_planes)
		mtk_crtc->pending_async_planes = true;

	if (priv->data->shadow_register) {
		mtk_mutex_acquire(mtk_crtc->mutex);
		mtk_crtc_ddp_config(crtc, NULL);
		mtk_mutex_release(mtk_crtc->mutex);
	}
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	if (mtk_crtc->cmdq_client.chan) {
		mbox_flush(mtk_crtc->cmdq_client.chan, 2000);
		cmdq_handle->cmd_buf_size = 0;
		cmdq_pkt_clear_event(cmdq_handle, mtk_crtc->cmdq_event);
		cmdq_pkt_wfe(cmdq_handle, mtk_crtc->cmdq_event, false);
		mtk_crtc_ddp_config(crtc, cmdq_handle);
		cmdq_pkt_eoc(cmdq_handle);
		dma_sync_single_for_device(mtk_crtc->cmdq_client.chan->mbox->dev,
					   cmdq_handle->pa_base,
					   cmdq_handle->cmd_buf_size,
					   DMA_TO_DEVICE);
		/*
		 * CMDQ command should execute in next 3 vblank.
		 * One vblank interrupt before send message (occasionally)
		 * and one vblank interrupt after cmdq done,
		 * so it's timeout after 3 vblank interrupt.
		 * If it fail to execute in next 3 vblank, timeout happen.
		 */
		mtk_crtc->cmdq_vblank_cnt = 3;

		spin_lock_irqsave(&mtk_crtc->config_lock, flags);
		mtk_crtc->config_updating = false;
		spin_unlock_irqrestore(&mtk_crtc->config_lock, flags);

		if (pm_runtime_resume_and_get(mtk_crtc->cmdq_client.chan->mbox->dev) < 0)
			goto update_config_out;

		mbox_send_message(mtk_crtc->cmdq_client.chan, cmdq_handle);
		mbox_client_txdone(mtk_crtc->cmdq_client.chan, 0);
		goto update_config_out;
	}
#endif
	spin_lock_irqsave(&mtk_crtc->config_lock, flags);
	mtk_crtc->config_updating = false;
	spin_unlock_irqrestore(&mtk_crtc->config_lock, flags);

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
update_config_out:
#endif
	mutex_unlock(&mtk_crtc->hw_lock);
}

static void mtk_crtc_ddp_irq(void *data)
{
	struct drm_crtc *crtc = data;
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_drm_private *priv = crtc->dev->dev_private;

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	if (!priv->data->shadow_register && !mtk_crtc->cmdq_client.chan)
		mtk_crtc_ddp_config(crtc, NULL);
	else if (mtk_crtc->cmdq_vblank_cnt > 0 && --mtk_crtc->cmdq_vblank_cnt == 0)
		DRM_ERROR("mtk_crtc %d CMDQ execute command timeout!\n",
			  drm_crtc_index(&mtk_crtc->base));
#else
	if (!priv->data->shadow_register)
		mtk_crtc_ddp_config(crtc, NULL);
#endif
	mtk_drm_finish_page_flip(mtk_crtc);
}

static int mtk_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_drm_private *priv = crtc->dev->dev_private;
	unsigned int idx = priv->data->vblank_comp_index;
	struct mtk_ddp_comp *comp = mtk_crtc->ddp_comp[idx];

	mtk_ddp_comp_enable_vblank(comp);

	return 0;
}

static void mtk_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_drm_private *priv = crtc->dev->dev_private;
	unsigned int idx = priv->data->vblank_comp_index;
	struct mtk_ddp_comp *comp = mtk_crtc->ddp_comp[idx];

	mtk_ddp_comp_disable_vblank(comp);
}

static void mtk_crtc_update_output(struct drm_crtc *crtc,
				   struct drm_atomic_state *state)
{
	int crtc_index = drm_crtc_index(crtc);
	int i;
	struct device *dev;
	struct drm_crtc_state *crtc_state = state->crtcs[crtc_index].new_state;
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_drm_private *priv;
	unsigned int encoder_mask = crtc_state->encoder_mask;

	if (!crtc_state->connectors_changed)
		return;

	if (!mtk_crtc->num_conn_routes)
		return;

	priv = ((struct mtk_drm_private *)crtc->dev->dev_private)->all_drm_private[crtc_index];
	dev = priv->dev;

	dev_dbg(dev, "connector change:%d, encoder mask:0x%x for crtc:%d\n",
		crtc_state->connectors_changed, encoder_mask, crtc_index);

	for (i = 0; i < mtk_crtc->num_conn_routes; i++) {
		unsigned int comp_id = mtk_crtc->conn_routes[i].route_ddp;
		struct mtk_ddp_comp *comp = &priv->ddp_comp[comp_id];

		if (comp->encoder_index >= 0 &&
		    (encoder_mask & BIT(comp->encoder_index))) {
			mtk_crtc->ddp_comp[mtk_crtc->ddp_comp_nr - 1] = comp;
			dev_dbg(dev, "Add comp_id: %d at path index %d\n",
				comp->id, mtk_crtc->ddp_comp_nr - 1);
			break;
		}
	}
}

int mtk_crtc_plane_check(struct drm_crtc *crtc, struct drm_plane *plane,
			 struct mtk_plane_state *state)
{
	unsigned int local_layer;
	struct mtk_ddp_comp *comp;

	comp = mtk_ddp_comp_for_plane(crtc, plane, &local_layer);
	if (comp)
		return mtk_ddp_comp_layer_check(comp, local_layer, state);
	return 0;
}

void mtk_crtc_plane_disable(struct drm_crtc *crtc, struct drm_plane *plane)
{
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_plane_state *plane_state = to_mtk_plane_state(plane->state);
	int i;

	/* no need to wait for disabling the plane by CPU */
	if (!mtk_crtc->cmdq_client.chan)
		return;

	if (!mtk_crtc->enabled)
		return;

	/* set pending plane state to disabled */
	for (i = 0; i < mtk_crtc->layer_nr; i++) {
		struct drm_plane *mtk_plane = &mtk_crtc->planes[i];
		struct mtk_plane_state *mtk_plane_state = to_mtk_plane_state(mtk_plane->state);

		if (mtk_plane->index == plane->index) {
			memcpy(mtk_plane_state, plane_state, sizeof(*plane_state));
			break;
		}
	}
	mtk_crtc_update_config(mtk_crtc, false);

	/* wait for planes to be disabled by CMDQ */
	wait_event_timeout(mtk_crtc->cb_blocking_queue,
			   mtk_crtc->cmdq_vblank_cnt == 0,
			   msecs_to_jiffies(500));
#endif
}

void mtk_crtc_async_update(struct drm_crtc *crtc, struct drm_plane *plane,
			   struct drm_atomic_state *state)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);

	if (!mtk_crtc->enabled)
		return;

	mtk_crtc_update_config(mtk_crtc, false);
}

static void mtk_crtc_atomic_enable(struct drm_crtc *crtc,
				   struct drm_atomic_state *state)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_ddp_comp *comp = mtk_crtc->ddp_comp[0];
	int ret;

	DRM_DEBUG_DRIVER("%s %d\n", __func__, crtc->base.id);
	pr_info("XAGA-STAGE atomic_enable: crtc%d\n", drm_crtc_index(crtc));

	ret = mtk_ddp_comp_power_on(comp);
	if (ret < 0) {
		DRM_DEV_ERROR(comp->dev, "Failed to enable power domain: %d\n", ret);
		return;
	}

	mtk_crtc_update_output(crtc, state);

	ret = mtk_crtc_ddp_hw_init(mtk_crtc);
	if (ret) {
		mtk_ddp_comp_power_off(comp);
		return;
	}

	drm_crtc_vblank_on(crtc);
	mtk_crtc->enabled = true;
}

static void mtk_crtc_atomic_disable(struct drm_crtc *crtc,
				    struct drm_atomic_state *state)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	struct mtk_ddp_comp *comp = mtk_crtc->ddp_comp[0];
	int i;

	DRM_DEBUG_DRIVER("%s %d\n", __func__, crtc->base.id);
	if (!mtk_crtc->enabled)
		return;

	/* Set all pending plane state to disabled */
	for (i = 0; i < mtk_crtc->layer_nr; i++) {
		struct drm_plane *plane = &mtk_crtc->planes[i];
		struct mtk_plane_state *plane_state;

		plane_state = to_mtk_plane_state(plane->state);
		plane_state->pending.enable = false;
		plane_state->pending.config = true;
	}
	mtk_crtc->pending_planes = true;

	mtk_crtc_update_config(mtk_crtc, false);
#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	/* Wait for planes to be disabled by cmdq */
	if (mtk_crtc->cmdq_client.chan)
		wait_event_timeout(mtk_crtc->cb_blocking_queue,
				   mtk_crtc->cmdq_vblank_cnt == 0,
				   msecs_to_jiffies(500));
#endif
	/* Wait for planes to be disabled */
	drm_crtc_wait_one_vblank(crtc);

	drm_crtc_vblank_off(crtc);
	mtk_crtc_ddp_hw_fini(mtk_crtc);
	mtk_ddp_comp_power_off(comp);

	mtk_crtc->enabled = false;
}

static void mtk_crtc_atomic_begin(struct drm_crtc *crtc,
				  struct drm_atomic_state *state)
{
	struct drm_crtc_state *crtc_state = drm_atomic_get_new_crtc_state(state,
									  crtc);
	struct mtk_crtc_state *mtk_crtc_state = to_mtk_crtc_state(crtc_state);
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	unsigned long flags;

	if (mtk_crtc->event && mtk_crtc_state->base.event)
		DRM_ERROR("new event while there is still a pending event\n");

	if (mtk_crtc_state->base.event) {
		mtk_crtc_state->base.event->pipe = drm_crtc_index(crtc);
		WARN_ON(drm_crtc_vblank_get(crtc) != 0);

		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		mtk_crtc->event = mtk_crtc_state->base.event;
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);

		mtk_crtc_state->base.event = NULL;
	}
}

static void mtk_crtc_atomic_flush(struct drm_crtc *crtc,
				  struct drm_atomic_state *state)
{
	struct mtk_crtc *mtk_crtc = to_mtk_crtc(crtc);
	int i;

	if (crtc->state->color_mgmt_changed)
		for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
			mtk_ddp_gamma_set(mtk_crtc->ddp_comp[i], crtc->state);
			mtk_ddp_ctm_set(mtk_crtc->ddp_comp[i], crtc->state);
		}
	mtk_crtc_update_config(mtk_crtc, !!mtk_crtc->event);
}

static const struct drm_crtc_funcs mtk_crtc_funcs = {
	.set_config		= drm_atomic_helper_set_config,
	.page_flip		= drm_atomic_helper_page_flip,
	.destroy		= mtk_crtc_destroy,
	.reset			= mtk_crtc_reset,
	.atomic_duplicate_state	= mtk_crtc_duplicate_state,
	.atomic_destroy_state	= mtk_crtc_destroy_state,
	.enable_vblank		= mtk_crtc_enable_vblank,
	.disable_vblank		= mtk_crtc_disable_vblank,
};

static const struct drm_crtc_helper_funcs mtk_crtc_helper_funcs = {
	.mode_fixup	= mtk_crtc_mode_fixup,
	.mode_set_nofb	= mtk_crtc_mode_set_nofb,
	.mode_valid	= mtk_crtc_mode_valid,
	.atomic_begin	= mtk_crtc_atomic_begin,
	.atomic_flush	= mtk_crtc_atomic_flush,
	.atomic_enable	= mtk_crtc_atomic_enable,
	.atomic_disable	= mtk_crtc_atomic_disable,
};

static int mtk_crtc_init(struct drm_device *drm, struct mtk_crtc *mtk_crtc,
			 unsigned int pipe)
{
	struct drm_plane *primary = NULL;
	struct drm_plane *cursor = NULL;
	int i, ret;

	for (i = 0; i < mtk_crtc->layer_nr; i++) {
		if (mtk_crtc->planes[i].type == DRM_PLANE_TYPE_PRIMARY)
			primary = &mtk_crtc->planes[i];
		else if (mtk_crtc->planes[i].type == DRM_PLANE_TYPE_CURSOR)
			cursor = &mtk_crtc->planes[i];
	}

	ret = drm_crtc_init_with_planes(drm, &mtk_crtc->base, primary, cursor,
					&mtk_crtc_funcs, NULL);
	if (ret)
		goto err_cleanup_crtc;

	drm_crtc_helper_add(&mtk_crtc->base, &mtk_crtc_helper_funcs);

	return 0;

err_cleanup_crtc:
	drm_crtc_cleanup(&mtk_crtc->base);
	return ret;
}

static int mtk_crtc_num_comp_planes(struct mtk_crtc *mtk_crtc, int comp_idx)
{
	struct mtk_ddp_comp *comp;

	if (comp_idx > 1)
		return 0;

	comp = mtk_crtc->ddp_comp[comp_idx];
	if (!comp->funcs)
		return 0;

	if (comp_idx == 1 && !comp->funcs->bgclr_in_on)
		return 0;

	return mtk_ddp_comp_layer_nr(comp);
}

static inline
enum drm_plane_type mtk_crtc_plane_type(unsigned int plane_idx,
					unsigned int num_planes)
{
	if (plane_idx == 0)
		return DRM_PLANE_TYPE_PRIMARY;
	else if (plane_idx == (num_planes - 1))
		return DRM_PLANE_TYPE_CURSOR;
	else
		return DRM_PLANE_TYPE_OVERLAY;

}

static int mtk_crtc_init_comp_planes(struct drm_device *drm_dev,
				     struct mtk_crtc *mtk_crtc,
				     int comp_idx, int pipe)
{
	int num_planes = mtk_crtc_num_comp_planes(mtk_crtc, comp_idx);
	struct mtk_ddp_comp *comp = mtk_crtc->ddp_comp[comp_idx];
	int i, ret;

	for (i = 0; i < num_planes; i++) {
		ret = mtk_plane_init(drm_dev,
				&mtk_crtc->planes[mtk_crtc->layer_nr],
				BIT(pipe),
				mtk_crtc_plane_type(mtk_crtc->layer_nr, num_planes),
				mtk_ddp_comp_supported_rotations(comp),
				mtk_ddp_comp_get_blend_modes(comp),
				mtk_ddp_comp_get_formats(comp),
				mtk_ddp_comp_get_num_formats(comp),
				mtk_ddp_comp_is_afbc_supported(comp), i);
		if (ret)
			return ret;

		mtk_crtc->layer_nr++;
	}
	return 0;
}

struct device *mtk_crtc_dma_dev_get(struct drm_crtc *crtc)
{
	struct mtk_crtc *mtk_crtc = NULL;

	if (!crtc)
		return NULL;

	mtk_crtc = to_mtk_crtc(crtc);
	if (!mtk_crtc)
		return NULL;

	return mtk_crtc->dma_dev;
}

int mtk_crtc_create(struct drm_device *drm_dev, const unsigned int *path,
		    unsigned int path_len, int priv_data_index,
		    const struct mtk_drm_route *conn_routes,
		    unsigned int num_conn_routes)
{
	struct mtk_drm_private *priv = drm_dev->dev_private;
	struct device *dev = drm_dev->dev;
	struct mtk_crtc *mtk_crtc;
	unsigned int num_comp_planes = 0;
	int ret;
	int i;
	bool has_ctm = false;
	uint gamma_lut_size = 0;
	struct drm_crtc *tmp;
	int crtc_i = 0;

	if (!path)
		return 0;

	priv = priv->all_drm_private[priv_data_index];

	drm_for_each_crtc(tmp, drm_dev)
		crtc_i++;

	for (i = 0; i < path_len; i++) {
		enum mtk_ddp_comp_id comp_id = path[i];
		struct device_node *node;
		struct mtk_ddp_comp *comp;

		node = priv->comp_node[comp_id];
		comp = &priv->ddp_comp[comp_id];

		/* Not all drm components have a DTS device node, such as ovl_adaptor,
		 * which is the drm bring up sub driver
		 */
		if (!node && comp_id != DDP_COMPONENT_DRM_OVL_ADAPTOR) {
			dev_info(dev,
				"Not creating crtc %d because component %d is disabled or missing\n",
				crtc_i, comp_id);
			return 0;
		}

		if (!comp->dev) {
			dev_err(dev, "Component %pOF not initialized\n", node);
			return -ENODEV;
		}
	}

	mtk_crtc = devm_kzalloc(dev, sizeof(*mtk_crtc), GFP_KERNEL);
	if (!mtk_crtc)
		return -ENOMEM;

	mtk_crtc->mmsys_dev = priv->mmsys_dev;
	mtk_crtc->ddp_comp_nr = path_len;
	mtk_crtc->ddp_comp = devm_kcalloc(dev,
					  mtk_crtc->ddp_comp_nr + (conn_routes ? 1 : 0),
					  sizeof(*mtk_crtc->ddp_comp),
					  GFP_KERNEL);
	if (!mtk_crtc->ddp_comp)
		return -ENOMEM;

	mtk_crtc->mutex = mtk_mutex_get(priv->mutex_dev);
	if (IS_ERR(mtk_crtc->mutex)) {
		ret = PTR_ERR(mtk_crtc->mutex);
		dev_err(dev, "Failed to get mutex: %d\n", ret);
		return ret;
	}

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
		unsigned int comp_id = path[i];
		struct mtk_ddp_comp *comp;

		comp = &priv->ddp_comp[comp_id];
		mtk_crtc->ddp_comp[i] = comp;

		if (comp->funcs) {
			if (comp->funcs->gamma_set && comp->funcs->gamma_get_lut_size) {
				unsigned int lut_sz = mtk_ddp_gamma_get_lut_size(comp);

				if (lut_sz)
					gamma_lut_size = lut_sz;
			}

			if (comp->funcs->ctm_set)
				has_ctm = true;
		}

		mtk_ddp_comp_register_vblank_cb(comp, mtk_crtc_ddp_irq,
						&mtk_crtc->base);
	}

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++)
		num_comp_planes += mtk_crtc_num_comp_planes(mtk_crtc, i);

	mtk_crtc->planes = devm_kcalloc(dev, num_comp_planes,
					sizeof(struct drm_plane), GFP_KERNEL);
	if (!mtk_crtc->planes)
		return -ENOMEM;

	for (i = 0; i < mtk_crtc->ddp_comp_nr; i++) {
		ret = mtk_crtc_init_comp_planes(drm_dev, mtk_crtc, i, crtc_i);
		if (ret)
			return ret;
	}

	/*
	 * Default to use the first component as the dma dev.
	 * In the case of ovl_adaptor sub driver, it needs to use the
	 * dma_dev_get function to get representative dma dev.
	 */
	mtk_crtc->dma_dev = mtk_ddp_comp_dma_dev_get(&priv->ddp_comp[path[0]]);

	ret = mtk_crtc_init(drm_dev, mtk_crtc, crtc_i);
	if (ret < 0)
		return ret;

	if (gamma_lut_size)
		drm_mode_crtc_set_gamma_size(&mtk_crtc->base, gamma_lut_size);
	drm_crtc_enable_color_mgmt(&mtk_crtc->base, 0, has_ctm, gamma_lut_size);
	mutex_init(&mtk_crtc->hw_lock);
	spin_lock_init(&mtk_crtc->config_lock);

#if IS_REACHABLE(CONFIG_MTK_CMDQ)
	i = priv->mbox_index++;
	mtk_crtc->cmdq_client.client.dev = mtk_crtc->mmsys_dev;
	mtk_crtc->cmdq_client.client.tx_block = false;
	mtk_crtc->cmdq_client.client.knows_txdone = true;
	mtk_crtc->cmdq_client.client.rx_callback = ddp_cmdq_cb;
	mtk_crtc->cmdq_client.chan =
			mbox_request_channel(&mtk_crtc->cmdq_client.client, i);
	if (IS_ERR(mtk_crtc->cmdq_client.chan)) {
		dev_dbg(dev, "mtk_crtc %d failed to create mailbox client, writing register by CPU now\n",
			drm_crtc_index(&mtk_crtc->base));
		mtk_crtc->cmdq_client.chan = NULL;
	}

	if (mtk_crtc->cmdq_client.chan) {
		ret = of_property_read_u32_index(priv->mutex_node,
						 "mediatek,gce-events",
						 i,
						 &mtk_crtc->cmdq_event);
		if (ret) {
			dev_dbg(dev, "mtk_crtc %d failed to get mediatek,gce-events property\n",
				drm_crtc_index(&mtk_crtc->base));
			mbox_free_channel(mtk_crtc->cmdq_client.chan);
			mtk_crtc->cmdq_client.chan = NULL;
		} else {
			ret = cmdq_pkt_create(&mtk_crtc->cmdq_client,
					      &mtk_crtc->cmdq_handle,
					      PAGE_SIZE);
			if (ret) {
				dev_dbg(dev, "mtk_crtc %d failed to create cmdq packet\n",
					drm_crtc_index(&mtk_crtc->base));
				mbox_free_channel(mtk_crtc->cmdq_client.chan);
				mtk_crtc->cmdq_client.chan = NULL;
			}
		}

		/* for sending blocking cmd in crtc disable */
		init_waitqueue_head(&mtk_crtc->cb_blocking_queue);
	}
#endif

	if (conn_routes) {
		for (i = 0; i < num_conn_routes; i++) {
			unsigned int comp_id = conn_routes[i].route_ddp;
			struct device_node *node = priv->comp_node[comp_id];
			struct mtk_ddp_comp *comp = &priv->ddp_comp[comp_id];

			if (!comp->dev) {
				dev_dbg(dev, "comp_id:%d, Component %pOF not initialized\n",
					comp_id, node);
				/* mark encoder_index to -1, if route comp device is not enabled */
				comp->encoder_index = -1;
				continue;
			}

			mtk_ddp_comp_encoder_index_set(&priv->ddp_comp[comp_id]);
		}

		mtk_crtc->num_conn_routes = num_conn_routes;
		mtk_crtc->conn_routes = conn_routes;

		/* increase ddp_comp_nr at the end of mtk_crtc_create */
		mtk_crtc->ddp_comp_nr++;
	}

	return 0;
}
