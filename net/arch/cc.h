#ifndef ARCH_CC_H
#define ARCH_CC_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uint64_t  u64_t;
typedef int64_t   s64_t;
typedef uintptr_t mem_ptr_t;

#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "u"

#define BYTE_ORDER LITTLE_ENDIAN

#define LWIP_PROVIDE_ERRNO 1

typedef unsigned int sys_prot_t;

#define SYS_ARCH_DECL_PROTECT(lev) sys_prot_t lev
#define SYS_ARCH_PROTECT(lev) do { lev = 0; } while(0)
#define SYS_ARCH_UNPROTECT(lev) do { (void)lev; } while(0)

void debug_printf(const char *fmt, ...);
void panic_simple(const char *msg);

#define LWIP_PLATFORM_DIAG(x) do { debug_printf(x); } while(0)
#define LWIP_PLATFORM_ASSERT(x) do { panic_simple(x); } while(0)

#define LWIP_RAND() ((u32_t)1)

#endif
