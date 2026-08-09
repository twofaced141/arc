#ifndef REGISTERS_H
#define REGISTERS_H

#include <stdint.h>

typedef struct {
    union { uint64_t rax; uint64_t eax; };
    union { uint64_t rbx; uint64_t ebx; };
    union { uint64_t rcx; uint64_t ecx; };
    union { uint64_t rdx; uint64_t edx; };
    union { uint64_t rsi; uint64_t esi; };
    union { uint64_t rdi; uint64_t edi; };
    union { uint64_t rbp; uint64_t ebp; };
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t int_no, err_code;
    union { uint64_t rip; uint64_t eip; };
    uint64_t cs;
    union { uint64_t rflags; uint64_t eflags; };
    union { uint64_t rsp; uint64_t esp; uint64_t useresp; };
    uint64_t ss;
} registers_t;

#endif
