// SPDX-License-Identifier: GPL-2.0
/*
 * STP/WMT stubs for the pearl (MT6895) BT bring-up.
 *
 * The ported conninfra stack (connectivity/conninfra) covers chip power,
 * register access and the debug plumbing. What it does not carry is the
 * legacy STP core (common_main/wmt) that the /dev/stpbt loader chardev
 * talks to: on this platform the kernel driver itself downloads the patch
 * over HCI (WMT in HCI vendor commands, opcode 0xFC6F), so that loader
 * path is inert. These thin wrappers keep wmt/stp_chrdev_bt.c linking:
 * func_on/off map straight onto the real conninfra power API, the STP
 * entry points report "not ready / empty" so a legacy loader open fails
 * cleanly instead of hanging.
 */
#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

#include "wmt_exp.h"
#include "conninfra.h"

MTK_WCN_BOOL mtk_wcn_wmt_func_on(ENUM_WMTDRV_TYPE_T type)
{
	if (type == WMTDRV_TYPE_BT)
		return conninfra_pwr_on(CONNDRV_TYPE_BT) == 0 ?
		       MTK_WCN_BOOL_TRUE : MTK_WCN_BOOL_FALSE;
	return MTK_WCN_BOOL_TRUE;
}
EXPORT_SYMBOL(mtk_wcn_wmt_func_on);

MTK_WCN_BOOL mtk_wcn_wmt_func_off(ENUM_WMTDRV_TYPE_T type)
{
	if (type == WMTDRV_TYPE_BT)
		return conninfra_pwr_off(CONNDRV_TYPE_BT) == 0 ?
		       MTK_WCN_BOOL_TRUE : MTK_WCN_BOOL_FALSE;
	return MTK_WCN_BOOL_TRUE;
}
EXPORT_SYMBOL(mtk_wcn_wmt_func_off);

int mtk_wcn_wmt_assert(ENUM_WMTDRV_TYPE_T type, int reason)
{
	return conninfra_trigger_whole_chip_rst(CONNDRV_TYPE_BT, "wmt_assert");
}
EXPORT_SYMBOL(mtk_wcn_wmt_assert);

int mtk_wcn_wmt_ic_info_get(unsigned long arg) { return 0; }
int mtk_wcn_wmt_adie_workable(void) { return 1; }
EXPORT_SYMBOL(mtk_wcn_wmt_adie_workable);
EXPORT_SYMBOL(mtk_wcn_wmt_ic_info_get);
void mtk_wcn_wmt_msgcb_reg(ENUM_WMTDRV_TYPE_T type, void *cb) {}
EXPORT_SYMBOL(mtk_wcn_wmt_msgcb_reg);
void mtk_wcn_wmt_msgcb_unreg(ENUM_WMTDRV_TYPE_T type) {}
EXPORT_SYMBOL(mtk_wcn_wmt_msgcb_unreg);
int mtk_wcn_wmt_psm_ctrl(int state) { return 0; }
EXPORT_SYMBOL(mtk_wcn_wmt_psm_ctrl);

/* Legacy STP core: absent by design (see file comment). */
MTK_WCN_BOOL mtk_wcn_stp_is_ready(void) { return MTK_WCN_BOOL_FALSE; }
EXPORT_SYMBOL(mtk_wcn_stp_is_ready);
MTK_WCN_BOOL mtk_wcn_stp_is_rxqueue_empty(int task) { return MTK_WCN_BOOL_TRUE; }
EXPORT_SYMBOL(mtk_wcn_stp_is_rxqueue_empty);
int mtk_wcn_stp_receive_data(unsigned char *buf, unsigned int len, int task)
{ return 0; }
EXPORT_SYMBOL(mtk_wcn_stp_receive_data);
int mtk_wcn_stp_send_data(const unsigned char *buf, unsigned int len,
			  unsigned char reserved)
{ return 0; }
EXPORT_SYMBOL(mtk_wcn_stp_send_data);
void mtk_wcn_stp_set_bluez(MTK_WCN_BOOL flag) {}
EXPORT_SYMBOL(mtk_wcn_stp_set_bluez);
int mtk_wcn_stp_register_event_cb(int hw_id, void *cb) { return 0; }
EXPORT_SYMBOL(mtk_wcn_stp_register_event_cb);

/* Symbols that used to live in the dropped wmt files. */
UINT32 gBtDbgLevel = 0;
EXPORT_SYMBOL(gBtDbgLevel);

ssize_t send_hci_frame(const unsigned char *buf, size_t count)
{
	/* Only the /dev/stpbt loader used this; the BTIF HCI path sends via
	 * the btif FIFO directly. */
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(send_hci_frame);

#ifdef MODULE
/*
 * mainline stopped exporting sched_setscheduler, so the module needs a
 * private lookalike. Built-in builds link against the real symbol instead.
 */
int sched_setscheduler(struct task_struct *p, int policy,
		       const struct sched_param *param)
{
	if (policy == SCHED_FIFO || policy == SCHED_RR)
		sched_set_fifo(p);
	return 0;
}
EXPORT_SYMBOL(sched_setscheduler);
#endif /* MODULE */

MODULE_LICENSE("GPL");
