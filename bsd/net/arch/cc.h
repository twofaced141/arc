/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * bsd/net/arch/cc.h — lwIP compiler/platform abstraction for ARC
 *
 * lwIP expects arch/cc.h to define:
 *   u8_t, s8_t, u16_t ...  byteorder macros, LWIP_PLATFORM_DIAG/ASSERT
 */

#ifndef ARC_NET_ARCH_CC_H
#define ARC_NET_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>

/* lwIP uses these exact typedefs */
typedef uint8_t  u8_t;
typedef int8_t   s8_t;
typedef uint16_t u16_t;
typedef int16_t  s16_t;
typedef uint32_t u32_t;
typedef int32_t  s32_t;
typedef uintptr_t mem_ptr_t;
typedef uint32_t sys_prot_t;

/* printf formatter for mem_ptr_t */
#define U16_F "hu"
#define S16_F "hd"
#define X16_F "hx"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"

/* Byte order — ARC is little-endian on all three arches (amd64/i386/arm64 LE) */
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN 4321
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* Use <string.h> for lwIP mem* */
#define LWIP_NO_CTYPE_H 0
/* Prevent lwIP arch.h from redefining ssize_t (we use long from bsd/vfs.h) */
#define LWIP_NO_UNISTD_H 1
#ifndef SSIZE_MAX
#define SSIZE_MAX 2147483647
#endif
/* provide ssize_t for lwIP headers that need it (etharp.h etc) */
#ifndef _SSIZE_T_DEFINED
typedef long ssize_t;
#define _SSIZE_T_DEFINED 1
#endif

/* Compiler hints */
#define LWIP_UNUSED_ARG(x)  ((void)(x))

/* Diagnostics — log instead of trap so #UD does not panic the kernel.
 * Original used __builtin_trap() -> ud2 -> panic Invalid Opcode. */
#include "debug.h"
#define LWIP_PLATFORM_DIAG(x)   do { log_printf(LOG_LEVEL_ERROR, "lwIP DIAG %s\n", x); } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { log_printf(LOG_LEVEL_ERROR, "lwIP ASSERT %s:%d: %s\n", __FILE__, __LINE__, (x)); } while(0)

#endif /* ARC_NET_ARCH_CC_H */
