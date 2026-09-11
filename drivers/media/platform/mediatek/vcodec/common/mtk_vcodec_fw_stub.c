// SPDX-License-Identifier: GPL-2.0
/*
 * "No firmware" codec backend.
 *
 * The stateless decoder still performs the firmware handshake: per-codec init
 * registers an IPI handler and sends AP_IPIMSG_DEC_INIT, then refuses to
 * continue unless a reply told it where the "vsi" shared block lives. With no
 * firmware behind this backend the reply is synthesized here, and the vsi is a
 * kernel-owned DMA-coherent buffer.
 *
 * This is enough to bring the decoder up (formats, buffers, requests) but not
 * to decode a frame: for H.264/HEVC/VP9/AV1 the register-level hardware
 * programming lives in the firmware, and MT6895's VCP firmware speaks the
 * vendor stateful ABI (vcodec_ipi_msg.h), not the stateless one implemented
 * here. Expect a bounded "lat decode timeout", never a frame.
 */

#include "../decoder/mtk_vcodec_dec_drv.h"
#include "../decoder/vdec_ipi_msg.h"
#include "../encoder/mtk_vcodec_enc_drv.h"
#include "mtk_vcodec_fw_priv.h"

/*
 * Capability mask the decoder reads. It drives which formats are exposed at
 * open (mtk_vcodec_get_supported_formats()) and which hardware path is used:
 * the stateless coded formats this SoC decodes, the capture formats, and the
 * extended (multi-core lat+core) path. No MTK_VCODEC_INNER_RACING -- that one
 * needs firmware cooperation.
 */
#define MTK_VCODEC_STUB_DEC_CAPA	(MTK_VDEC_FORMAT_MT21C | \
					 MTK_VDEC_FORMAT_MM21 | \
					 MTK_VDEC_FORMAT_H264_SLICE | \
					 MTK_VDEC_FORMAT_HEVC_FRAME | \
					 MTK_VDEC_FORMAT_VP8_FRAME | \
					 MTK_VDEC_FORMAT_VP9_FRAME | \
					 MTK_VDEC_FORMAT_AV1_FRAME | \
					 MTK_VDEC_IS_SUPPORT_EXT)

static int mtk_vcodec_fw_stub_load_firmware(struct mtk_vcodec_fw *fw)
{
	/* There is nothing to load; there is also no one to talk to. */
	return 0;
}

static unsigned int mtk_vcodec_fw_stub_get_vdec_capa(struct mtk_vcodec_fw *fw)
{
	return MTK_VCODEC_STUB_DEC_CAPA;
}

static unsigned int mtk_vcodec_fw_stub_get_venc_capa(struct mtk_vcodec_fw *fw)
{
	return 0;
}

/*
 * The stateless decoders map one shared block (the "vsi") during per-codec
 * init and refuse to continue when it is NULL. There is no firmware to place
 * it, so use a kernel-owned DMA-coherent buffer: the LAT/CORE read the decode
 * parameters out of it, so it has to be DMA-able.
 */
#define MTK_VCODEC_STUB_VSI_SIZE	SZ_128K

static void *mtk_vcodec_fw_stub_map_dm_addr(struct mtk_vcodec_fw *fw,
					    u32 dtcm_dmem_addr)
{
	return fw->vsi_buf;
}

static int mtk_vcodec_fw_stub_ipi_register(struct mtk_vcodec_fw *fw, int id,
					   mtk_vcodec_ipi_handler handler,
					   const char *name, void *priv)
{
	if (id < 0 || id >= MTK_VCODEC_STUB_MAX_IPI) {
		dev_err(&fw->pdev->dev, "invalid ipi id %d for %s\n", id, name);
		return -EINVAL;
	}

	fw->stub_handler[id] = handler;
	fw->stub_priv[id] = priv;

	dev_info(&fw->pdev->dev, "registered %s handler on ipi %d\n", name, id);

	return 0;
}

/*
 * Reply to AP_IPIMSG_DEC_INIT the way the firmware would: echo the AP instance
 * address back (the handler recovers its vpu pointer from it), report the vsi
 * we mapped, and advertise ABI 1 so the decoder keeps using vpu_inst_addr
 * rather than a firmware-assigned instance id.
 */
static void mtk_vcodec_fw_stub_ack_init(struct mtk_vcodec_fw *fw,
					const struct vdec_ap_ipi_init *init,
					mtk_vcodec_ipi_handler handler,
					void *priv)
{
	struct vdec_vpu_ipi_init_ack ack = {
		.msg_id = VPU_IPIMSG_DEC_INIT_ACK,
		.status = 0,
		.ap_inst_addr = init->ap_inst_addr,
		.vpu_inst_addr = (u32)fw->vsi_dma,
		.vdec_abi_version = 1,
		.inst_id = 0,
	};

	fw->stub_inst_addr = init->ap_inst_addr;

	dev_info(&fw->pdev->dev,
		 "DEC_INIT ack: ap_inst=0x%llx vsi=0x%llx\n",
		 init->ap_inst_addr, (u64)fw->vsi_dma);

	handler(&ack, sizeof(ack), priv);
}

/*
 * Trace the first exchanges at info level: enough to see the whole AP ->
 * firmware sequence in dmesg without turning every decoded frame into a log
 * line.
 */
static void mtk_vcodec_fw_stub_trace(struct mtk_vcodec_fw *fw, u32 msg_id, int id)
{
	if (fw->stub_msg_count++ >= MTK_VCODEC_STUB_LOG_MSGS)
		return;

	dev_info(&fw->pdev->dev, "decoder sent ipi %d msg 0x%x\n", id, msg_id);
}

static int mtk_vcodec_fw_stub_ipi_send(struct mtk_vcodec_fw *fw, int id,
				       void *buf, unsigned int len,
				       unsigned int wait)
{
	u32 msg_id;
	mtk_vcodec_ipi_handler handler;
	void *priv;

	if (id < 0 || id >= MTK_VCODEC_STUB_MAX_IPI || len < sizeof(msg_id))
		return -EINVAL;

	msg_id = *(u32 *)buf;
	handler = fw->stub_handler[id];
	priv = fw->stub_priv[id];

	mtk_vcodec_fw_stub_trace(fw, msg_id, id);

	if (!handler)
		return 0;

	switch (msg_id) {
	case AP_IPIMSG_DEC_INIT:
		if (len < sizeof(struct vdec_ap_ipi_init)) {
			dev_err(&fw->pdev->dev, "short DEC_INIT (%u bytes)\n", len);
			return -EINVAL;
		}
		mtk_vcodec_fw_stub_ack_init(fw, buf, handler, priv);
		return 0;
	default:
		/* Nothing in this path waits for the other replies; they only
		 * show up in the trace above. */
		return 0;
	}
}

static void mtk_vcodec_fw_stub_release(struct mtk_vcodec_fw *fw)
{
}

static const struct mtk_vcodec_fw_ops mtk_vcodec_fw_stub_ops = {
	.load_firmware = mtk_vcodec_fw_stub_load_firmware,
	.get_vdec_capa = mtk_vcodec_fw_stub_get_vdec_capa,
	.get_venc_capa = mtk_vcodec_fw_stub_get_venc_capa,
	.map_dm_addr = mtk_vcodec_fw_stub_map_dm_addr,
	.ipi_register = mtk_vcodec_fw_stub_ipi_register,
	.ipi_send = mtk_vcodec_fw_stub_ipi_send,
	.release = mtk_vcodec_fw_stub_release,
};

struct mtk_vcodec_fw *mtk_vcodec_fw_stub_init(void *priv,
					      enum mtk_vcodec_fw_use fw_use)
{
	struct platform_device *plat_dev;
	struct mtk_vcodec_fw *fw;

	if (fw_use == ENCODER)
		plat_dev = ((struct mtk_vcodec_enc_dev *)priv)->plat_dev;
	else
		plat_dev = ((struct mtk_vcodec_dec_dev *)priv)->plat_dev;

	fw = devm_kzalloc(&plat_dev->dev, sizeof(*fw), GFP_KERNEL);
	if (!fw)
		return ERR_PTR(-ENOMEM);

	fw->type = STUB;
	fw->ops = &mtk_vcodec_fw_stub_ops;
	fw->pdev = plat_dev;

	fw->vsi_buf = dmam_alloc_coherent(&plat_dev->dev, MTK_VCODEC_STUB_VSI_SIZE,
					  &fw->vsi_dma, GFP_KERNEL);
	if (!fw->vsi_buf)
		return ERR_PTR(-ENOMEM);

	dev_info(&plat_dev->dev, "using the no-firmware codec backend\n");

	return fw;
}
