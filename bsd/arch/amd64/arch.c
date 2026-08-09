#include "bsd/arch.h"
#include "vmm.h"
#include "string.h"

#if defined(__x86_64__)
void arch_setup_exec_regs(registers_t *r, uint64_t entry, uint64_t stack_top) {
    r->rax = 0; r->rbx = 0; r->rcx = 0; r->rdx = 0;
    r->rsi = 0; r->rdi = 0; r->rbp = 0;
    r->r8 = 0; r->r9 = 0; r->r10 = 0; r->r11 = 0;
    r->r12 = 0; r->r13 = 0; r->r14 = 0; r->r15 = 0;
    r->int_no = 0; r->err_code = 0;
    r->rip = entry;
    r->cs = 0x2B;
    r->rflags = 0x202;
    r->useresp = stack_top;
    r->ss = 0x23;
}

registers_t *arch_fork_setup_regs(thread_t *child_thread, registers_t *parent) {
    registers_t *child_regs = (registers_t *)(child_thread->kernel_stack_top - sizeof(registers_t) - 8);
    memcpy(child_regs, parent, sizeof(registers_t));
    child_regs->rax = 0;
    child_thread->kernel_esp = (uint64_t)child_regs;
    child_thread->user_esp = parent->useresp;
    child_thread->eip = parent->rip;
    return child_regs;
}

registers_t *arch_clone_setup_regs(thread_t *child_thread, registers_t *parent,
                                   uint64_t child_stack) {
    registers_t *child_regs = (registers_t *)(child_thread->kernel_stack_top - sizeof(registers_t) - 8);
    memcpy(child_regs, parent, sizeof(registers_t));
    child_regs->rax = 0;                 /* child: clone() returns 0 */
    child_regs->rsp = child_stack;       /* child runs on its own stack */
    child_regs->rdi = 0;                 /* thread function arg starts zeroed */
    child_regs->rflags = 0x202;
    child_thread->kernel_esp = (uint64_t)child_regs;
    child_thread->user_esp = child_stack;
    child_thread->eip = parent->rip;
    return child_regs;
}

void arch_thread_set_tls(thread_t *t, uint64_t tls_base) {
    t->tls_base = tls_base;
}

void arch_setup_tls_page(void *page, uint64_t tls_vaddr) {
    uint8_t *p = (uint8_t *)page;
    for (int i = 0; i < 8; i++)
        p[i] = (tls_vaddr >> (i * 8)) & 0xFF;
    for (int i = 0; i < 8; i++)
        p[8 + i] = (tls_vaddr >> (i * 8)) & 0xFF;
    p[0x10] = 0xCD; p[0x11] = 0x80; p[0x12] = 0xC3;
    p[0x14] = 0xDE; p[0x15] = 0xAD; p[0x16] = 0xBE; p[0x17] = 0xEF;
}
#endif
