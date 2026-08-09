#ifndef ISR_H
#define ISR_H

#include <stdint.h>

/* Microkernel syscall numbers */
#define SYSCALL_THREAD_CREATE  0
#define SYSCALL_THREAD_EXIT    1
#define SYSCALL_THREAD_YIELD   2
#define SYSCALL_PORT_CREATE    3
#define SYSCALL_PORT_SEND      4
#define SYSCALL_PORT_RECV      5
#define SYSCALL_PORT_CALL      6
#define SYSCALL_PORT_REPLY     7
#define SYSCALL_GETPID         8
#define SYSCALL_CONSOLE_PUTC   9
#define SYSCALL_SLEEP          10
#define SYSCALL_GETTICKS       11
#define SYSCALL_SHUTDOWN       12

typedef struct {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} registers_t;

typedef void (*isr_t)(registers_t *);

void isr_init(void);
void register_interrupt_handler(uint8_t n, isr_t handler);
void double_fault_handler(registers_t *r);

#endif
