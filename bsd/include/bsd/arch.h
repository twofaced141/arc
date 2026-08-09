#ifndef BSD_ARCH_H
#define BSD_ARCH_H

#include <stdint.h>
#include "isr.h"
#include "thread.h"
#include "bsd/elf32.h"
#include "bsd/elf64.h"

#if defined(__x86_64__)
#  define BSD_ELF_CLASS   ELFCLASS64
#  define BSD_ELF_MACHINE EM_X86_64
#  define BSD_ELF_RELTYPE R_X86_64_RELATIVE
typedef uint64_t bsd_elf_addr_t;
#elif defined(__i386__) || defined(__i686__)
#  define BSD_ELF_CLASS   ELFCLASS32
#  define BSD_ELF_MACHINE EM_386
#  define BSD_ELF_RELTYPE R_386_RELATIVE
typedef uint32_t bsd_elf_addr_t;
#elif defined(__aarch64__)
#  ifndef EM_AARCH64
#    define EM_AARCH64 183
#  endif
#  ifndef R_AARCH64_RELATIVE
#    define R_AARCH64_RELATIVE 1027
#  endif
#  define BSD_ELF_CLASS   ELFCLASS64
#  define BSD_ELF_MACHINE EM_AARCH64
#  define BSD_ELF_RELTYPE R_AARCH64_RELATIVE
typedef uint64_t bsd_elf_addr_t;
#endif

#if defined(__x86_64__)
#  define bsd_syscall_num(r)    ((r)->rax - 1024)
#  define bsd_syscall_arg0(r)   ((r)->rdi)
#  define bsd_syscall_arg1(r)   ((r)->rsi)
#  define bsd_syscall_arg2(r)   ((r)->rdx)
#  define bsd_syscall_arg3(r)   ((r)->r10)
#  define bsd_syscall_arg4(r)   ((r)->r8)
#  define bsd_syscall_arg5(r)   ((r)->r9)
#  define bsd_syscall_ret(r,v)  ((r)->rax = (uint64_t)(v))
#  define bsd_syscall_ret0(r)   ((r)->rax = 0)
#  define bsd_entry(r)          ((r)->rip)
#  define BSD_SYSCALL_INS_LEN   2   /* `syscall` (0F 05) */
#elif defined(__i386__) || defined(__i686__)
#  define bsd_syscall_num(r)    ((r)->eax - 1024)
#  define bsd_syscall_arg0(r)   ((r)->ebx)
#  define bsd_syscall_arg1(r)   ((r)->ecx)
#  define bsd_syscall_arg2(r)   ((r)->edx)
#  define bsd_syscall_arg3(r)   ((r)->esi)
#  define bsd_syscall_arg4(r)   ((r)->edi)
#  define bsd_syscall_arg5(r)   ((r)->ebp)
#  define bsd_syscall_ret(r,v)  ((r)->eax = (uint32_t)(v))
#  define bsd_syscall_ret0(r)   ((r)->eax = 0)
#  define bsd_entry(r)          ((r)->eip)
#  define BSD_SYSCALL_INS_LEN   2   /* `int $0x80` (CD 80) */
#elif defined(__aarch64__)
#  define bsd_syscall_num(r)    ((r)->x[0] - 1024)
#  define bsd_syscall_arg0(r)   ((r)->x[1])
#  define bsd_syscall_arg1(r)   ((r)->x[2])
#  define bsd_syscall_arg2(r)   ((r)->x[3])
#  define bsd_syscall_arg3(r)   ((r)->x[4])
#  define bsd_syscall_arg4(r)   ((r)->x[5])
#  define bsd_syscall_arg5(r)   ((r)->x[6])
#  define bsd_syscall_ret(r,v)  ((r)->x[0] = (uint64_t)(v))
#  define bsd_syscall_ret0(r)   ((r)->x[0] = 0)
#  define bsd_entry(r)          ((r)->elr)
#  define BSD_SYSCALL_INS_LEN   4   /* `svc #0` (D4000001) */
#endif

void arch_setup_exec_regs(registers_t *r, uint64_t entry, uint64_t stack_top);
registers_t *arch_fork_setup_regs(thread_t *child_thread, registers_t *parent);
void arch_setup_tls_page(void *page, uint64_t tls_vaddr);
registers_t *arch_clone_setup_regs(thread_t *child_thread, registers_t *parent,
                                   uint64_t child_stack);
void arch_thread_set_tls(thread_t *t, uint64_t tls_base);

#endif