#ifndef ISR_H
#define ISR_H

#include <stdint.h>

#define SYSCALL_GETPID   0u
#define SYSCALL_PUTC     1u
#define SYSCALL_YIELD    2u
#define SYSCALL_EXIT     3u
#define SYSCALL_WRITE    4u
#define SYSCALL_READ     5u
#define SYSCALL_SLEEP    6u
#define SYSCALL_GETTICKS 7u
#define SYSCALL_OPEN     8u
#define SYSCALL_CLOSE    9u
#define SYSCALL_LSEEK    10u
#define SYSCALL_FSTAT    11u
#define SYSCALL_BRK      12u
#define SYSCALL_SBRK     13u
#define SYSCALL_FORK     14u
#define SYSCALL_EXECVE   15u
#define SYSCALL_WAITPID  16u
#define SYSCALL_CHDIR    17u
#define SYSCALL_GETCWD   18u
#define SYSCALL_LISTDIR  19u
#define SYSCALL_KILL     20u
#define SYSCALL_DUP2     21u
#define SYSCALL_PIPE     22u

typedef struct {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} registers_t;

typedef void (*isr_t)(registers_t *);

void isr_init(void);
void register_interrupt_handler(uint8_t n, isr_t handler);

#endif
