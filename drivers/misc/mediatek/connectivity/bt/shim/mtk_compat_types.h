/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MTK_COMPAT_TYPES_H_
#define _MTK_COMPAT_TYPES_H_

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>

#ifndef UINT32
typedef u32 UINT32;
#endif
#ifndef PUINT32
typedef u32 *PUINT32;
#endif
#ifndef UINT16
typedef u16 UINT16;
#endif
#ifndef PUINT16
typedef u16 *PUINT16;
#endif
#ifndef UINT8
typedef u8 UINT8;
#endif
#ifndef PUINT8
typedef u8 *PUINT8;
#endif
#ifndef INT32
typedef s32 INT32;
#endif
#ifndef PINT32
typedef s32 *PINT32;
#endif
#ifndef INT16
typedef s16 INT16;
#endif
#ifndef PINT16
typedef s16 *PINT16;
#endif
#ifndef INT8
typedef s8 INT8;
#endif
#ifndef PINT8
typedef s8 *PINT8;
#endif
#ifndef BOOL
typedef int BOOL;
#endif
#ifndef PBOOL
typedef int *PBOOL;
#endif
#ifndef VOID
#define VOID void
#endif
#ifndef PVOID
typedef void *PVOID;
#endif

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

typedef enum _ENUM_WMTMSG_TYPE_T {
	WMTMSG_TYPE_POWER_ON = 0,
	WMTMSG_TYPE_POWER_OFF = 1,
	WMTMSG_TYPE_RESET = 2,
	WMTMSG_TYPE_STP_RDY = 3,
	WMTMSG_TYPE_HW_RST = 4,
	WMTMSG_TYPE_MAX
} ENUM_WMTMSG_TYPE_T, *P_ENUM_WMTMSG_TYPE_T;

typedef enum _WMTRSTMSG {
	WMTRSTMSG_RESET_START = 0,
	WMTRSTMSG_RESET_END = 1,
	WMTRSTMSG_MAX
} WMTRSTMSG, *P_WMTRSTMSG;

typedef enum _WMTCHIN {
	WMTCHIN_CHIN_AUTO = 0,
	WMTCHIN_CHIN_ON = 1,
	WMTCHIN_CHIN_OFF = 2,
	WMTCHIN_MAX
} WMTCHIN, *P_WMTCHIN;

#endif /* _MTK_COMPAT_TYPES_H_ */
