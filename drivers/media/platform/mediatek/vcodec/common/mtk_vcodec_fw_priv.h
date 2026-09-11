/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _MTK_VCODEC_FW_PRIV_H_
#define _MTK_VCODEC_FW_PRIV_H_

#include "mtk_vcodec_fw.h"

struct mtk_vcodec_dec_dev;
struct mtk_vcodec_enc_dev;

/* The stateless decoders register on the SCP_IPI_VDEC_LAT/CORE ids, which are
 * small; the table only has to cover what the codec really uses. */
#define MTK_VCODEC_STUB_MAX_IPI		32
/* Enough messages to show the whole AP->firmware sequence once in dmesg
 * without turning every decoded frame into a log line. */
#define MTK_VCODEC_STUB_LOG_MSGS	32

struct mtk_vcodec_fw {
	enum mtk_vcodec_fw_type type;
	const struct mtk_vcodec_fw_ops *ops;
	struct platform_device *pdev;
	struct mtk_scp *scp;
	/* no-firmware backend: the DMA-coherent block that stands in for the
	 * firmware-provided "vsi" shared memory */
	void *vsi_buf;
	dma_addr_t vsi_dma;
	/* no-firmware backend: handlers the decoders registered, indexed by IPI
	 * id, so replies synthesized on their behalf reach the right instance */
	mtk_vcodec_ipi_handler stub_handler[MTK_VCODEC_STUB_MAX_IPI];
	void *stub_priv[MTK_VCODEC_STUB_MAX_IPI];
	u64 stub_inst_addr;
	unsigned int stub_msg_count;
	enum mtk_vcodec_fw_use fw_use;
};

struct mtk_vcodec_fw_ops {
	int (*load_firmware)(struct mtk_vcodec_fw *fw);
	unsigned int (*get_vdec_capa)(struct mtk_vcodec_fw *fw);
	unsigned int (*get_venc_capa)(struct mtk_vcodec_fw *fw);
	void *(*map_dm_addr)(struct mtk_vcodec_fw *fw, u32 dtcm_dmem_addr);
	int (*ipi_register)(struct mtk_vcodec_fw *fw, int id,
			    mtk_vcodec_ipi_handler handler, const char *name,
			    void *priv);
	int (*ipi_send)(struct mtk_vcodec_fw *fw, int id, void *buf,
			unsigned int len, unsigned int wait);
	void (*release)(struct mtk_vcodec_fw *fw);
};

#if IS_ENABLED(CONFIG_VIDEO_MEDIATEK_VCODEC_VPU)
struct mtk_vcodec_fw *mtk_vcodec_fw_vpu_init(void *priv, enum mtk_vcodec_fw_use fw_use);
#else
static inline struct mtk_vcodec_fw *
mtk_vcodec_fw_vpu_init(void *priv, enum mtk_vcodec_fw_use fw_use)
{
	return ERR_PTR(-ENODEV);
}
#endif /* CONFIG_VIDEO_MEDIATEK_VCODEC_VPU */

#if IS_ENABLED(CONFIG_VIDEO_MEDIATEK_VCODEC_SCP)
struct mtk_vcodec_fw *mtk_vcodec_fw_scp_init(void *priv, enum mtk_vcodec_fw_use fw_use);
#else
static inline struct mtk_vcodec_fw *
mtk_vcodec_fw_scp_init(void *priv, enum mtk_vcodec_fw_use fw_use)
{
	return ERR_PTR(-ENODEV);
}
#endif /* CONFIG_VIDEO_MEDIATEK_VCODEC_SCP */

#endif /* _MTK_VCODEC_FW_PRIV_H_ */
