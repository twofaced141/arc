#include "bsd/arch.h"
#include "vmm.h"
#include "string.h"

#if defined(__aarch64__)
void arch_setup_exec_regs(registers_t *r, uint64_t entry, uint64_t stack_top) {
    for (int i = 0; i < 30; i++) r->x[i] = 0;
    r->lr = 0;
    r->spsr = 0;
    r->elr = entry;
    r->esr = 0;
    r->far = 0;
    r->sp = stack_top;
}

registers_t *arch_fork_setup_regs(thread_t *child_thread, registers_t *parent) {
    registers_t *child_regs = (registers_t *)(child_thread->kernel_stack_top - sizeof(registers_t));
    memcpy(child_regs, parent, sizeof(registers_t));
    child_regs->x[0] = 0;
    return child_regs;
}

registers_t *arch_clone_setup_regs(thread_t *child_thread, registers_t *parent,
                                   uint64_t child_stack) {
    registers_t *child_regs = (registers_t *)(child_thread->kernel_stack_top - sizeof(registers_t));
    memcpy(child_regs, parent, sizeof(registers_t));
    child_regs->x[0] = 0;               /* child: clone() returns 0 */
    child_regs->sp = child_stack;
    return child_regs;
}

void arch_thread_set_tls(thread_t *t, uint64_t tls_base) {
    t->tls_base = tls_base;
}

void arch_setup_tls_page(void *page, uint64_t tls_vaddr) {
    (void)tls_vaddr;
    uint8_t *p = (uint8_t *)page;
    for (int i = 0; i < 4096; i++) p[i] = 0;
}
#endif
