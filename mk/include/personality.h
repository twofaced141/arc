#ifndef MK_PERSONALITY_H
#define MK_PERSONALITY_H

#include <stdint.h>
#include "isr.h"

/*
 * Optional BSD personality hooks.
 *
 * The microkernel builds and runs standalone.  The BSD personality
 * layer (~/Projects/bsd, built with `make WITH_BSD=1`) provides strong
 * definitions of these symbols; when it is not linked in, the weak
 * declarations below resolve to NULL and every call site in mk checks
 * for NULL before calling.
 */

struct proc;

extern void bsd_init(const char *cmdline) __attribute__((weak));
extern int64_t bsd_syscall_dispatch(registers_t *r) __attribute__((weak));
extern void sys_driver_irq_dispatch(uint8_t irq_num) __attribute__((weak));
extern struct proc *proc_current(void) __attribute__((weak));
extern void proc_exit(int code, registers_t *r) __attribute__((weak));

#endif /* MK_PERSONALITY_H */
