/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CONN_DBG_H
#define CONN_DBG_H

enum conn_dbg_log_type {
	CONN_DBG_LOG_TYPE_HW_ERR = 0,
	CONN_DBG_LOG_TYPE_NUM
};

static inline int conn_dbg_add_log(enum conn_dbg_log_type type, const char *buf) { return 0; }
#define conn_dbg_add_log_once(_type, _buf) do { } while (0)

#endif /* CONN_DBG_H */
