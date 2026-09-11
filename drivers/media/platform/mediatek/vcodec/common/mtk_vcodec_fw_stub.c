// SPDX-License-Identifier: GPL-2.0
/*
 * "No firmware" codec backend.
 *
 * MT6895's VDEC is driven entirely from the kernel by the stateless
 * (Request API) decoder, so the vendor firmware that the stock kernel talks to
 * over its own IPI protocol is not used. The stateless path's only firmware
 * hooks are the firmware load and the capability query; everything else is
 * either unused (LAT/VPU IPI) or kernel-side.
 */

#include "../decoder/mtk_vcodec_dec_drv.h"
#include "../encoder/mtk_vcodec_enc_drv.h"
#include "mtk_vcodec_fw_priv.h"

/*
 * Capability bits the decoder reads: no MTK_VCODEC_INNER_RACING (that needs
 * firmware cooperation), extended (stateless) mode supported.
 */
#define MTK_VCODEC_STUB_DEC_CAPA	MTK_VDEC_IS_SUPPORT_EXT

static int mtk_vcodec_fw_stub_load_firmware(struct mtk_vcodec_fw *fw)
{
	/* Nothing to load: the hardware is driven from the kernel. */
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

static void *mtk_vcodec_fw_stub_map_dm_addr(struct mtk_vcodec_fw *fw,
					    u32 dtcm_dmem_addr)
{
	return NULL;
}

static int mtk_vcodec_fw_stub_ipi_register(struct mtk_vcodec_fw *fw, int id,
					   mtk_vcodec_ipi_handler handler,
					   const char *name, void *priv)
{
	return 0;
}

static int mtk_vcodec_fw_stub_ipi_send(struct mtk_vcodec_fw *fw, int id,
				       void *buf, unsigned int len,
				       unsigned int wait)
{
	return 0;
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

	dev_info(&plat_dev->dev, "using the no-firmware codec backend\n");

	return fw;
}
