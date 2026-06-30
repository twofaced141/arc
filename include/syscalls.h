#ifndef SYSCALLS_H
#define SYSCALLS_H

#include <stdint.h>
#include "isr.h"

extern uint32_t scheduler_syscall_no;
void syscall_handler(registers_t *r);

#endif
