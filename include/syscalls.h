#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stdint.h>
#include "isr.h"

extern uint32_t scheduler_syscall_no;
void syscall_handler(registers_t *r);
void sys_sethostname(const char *name);
void sys_gethostname(char *buf, uint32_t size);
int sys_setpgid(int pid, int pgid);
int sys_getpgid(int pid);

#endif
