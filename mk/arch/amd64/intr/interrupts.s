.section .text

.macro ISR_NOERRCODE num
.global isr\num
isr\num:
    push $0
    push $\num
    jmp isr_common_stub
.endm

.macro ISR_ERRCODE num
.global isr\num
isr\num:
    push $\num
    jmp isr_common_stub
.endm

ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_NOERRCODE 17
ISR_ERRCODE   18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_ERRCODE   30
ISR_NOERRCODE 31
ISR_NOERRCODE 128

.macro IRQ num, vec
.global irq\num
irq\num:
    push $0
    push $\vec
    jmp irq_common_stub
.endm

IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

/* SMP IPI vector (0x4F) */
IRQ 79, 79

.extern isr_handler
isr_common_stub:
    push %r15
    push %r14
    push %r13
    push %r12
    push %r11
    push %r10
    push %r9
    push %r8
    push %rbp
    push %rdi
    push %rsi
    push %rdx
    push %rcx
    push %rbx
    push %rax

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs


    cld
    mov %rsp, %rdi
    call isr_handler

    mov %rsp, %rdi
    call scheduler_switch
    mov %rax, %rsp

    pop %rax
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbp
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15

    add $16, %rsp
    iretq

.extern irq_handler
irq_common_stub:
    push %r15
    push %r14
    push %r13
    push %r12
    push %r11
    push %r10
    push %r9
    push %r8
    push %rbp
    push %rdi
    push %rsi
    push %rdx
    push %rcx
    push %rbx
    push %rax

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs


    cld
    mov %rsp, %rdi
    call irq_handler

    mov %rsp, %rdi
    call scheduler_switch
    mov %rax, %rsp

    pop %rax
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbp
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15

    add $16, %rsp
    iretq

/* --- syscall entry for user mode ---
 *
 * Per-CPU state is accessed through %gs (kernel GS.base, never
 * swapped):  %gs:32 = syscall kernel stack, %gs:40 = saved user RSP.
 * Both live in struct arch_cpu — see the _Static_asserts there. */

.extern scheduler_switch

.global syscall_entry
syscall_entry:
    movq %rsp, %gs:40
    /* Per-CPU syscall stack: %gs:32 = cpu->arch.syscall_rsp0, kept in
     * sync with the TSS rsp0 of this CPU by tss_set_kernel_stack().
     * (Offset contract enforced by _Static_assert in arch_cpu.h.) */
    movq %gs:32, %rsp

    push $0x23
    pushq %gs:40
    push %r11
    push $0x2B
    push %rcx

    push $0
    push $128

    push %r15
    push %r14
    push %r13
    push %r12
    push %r11
    push %r10
    push %r9
    push %r8
    push %rbp
    push %rdi
    push %rsi
    push %rdx
    push %rcx
    push %rbx
    push %rax

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs


    cld
    mov %rsp, %rdi
    call isr_handler

    mov %rsp, %rdi
    call scheduler_switch
    mov %rax, %rsp

    pop %rax
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbp
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15

    add $16, %rsp
    iretq

/* thread_yield — yield CPU from kernel-thread context.
 *
 * Called from a kernel thread (e.g. boot pager) that wants to give
 * up the CPU.  Builds a registers_t frame on the stack so that when
 * the thread is later rescheduled, execution resumes after the call.
 */
.global thread_yield
thread_yield:
    /* Build a registers_t + iretq frame that matches the layout      */
    /* created by isr_common_stub + the CPU.  On resume via iretq,    */
    /* we land at .yield_resume which returns to the caller.          */
    /* iretq frame (CPU pushes these on interrupts) */
    push $0x10                              /* ss = kernel data seg   */
    lea 8(%rsp), %rax                       /* rsp = &return_addr     */
    push %rax                               /* rsp slot               */
    pushfq                                   /* rflags                 */
    push $0x08                              /* cs = kernel code seg   */
    lea .yield_resume(%rip), %rax
    push %rax                               /* rip = resume point     */

    /* ISR macro level */
    push $0                                 /* err_code               */
    push $0                                 /* int_no                 */

    /* Register pushes — same order as isr_common_stub */
    push %r15
    push %r14
    push %r13
    push %r12
    push %r11
    push %r10
    push %r9
    push %r8
    push %rbp
    push %rdi
    push %rsi
    push %rdx
    push %rcx
    push %rbx
    push %rax

    mov %rsp, %rdi
    call scheduler_switch
    mov %rax, %rsp                          /* switch to new stack    */

    /* Pop registers from (possibly new) thread's frame */
    pop %rax
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbp
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15

    add $16, %rsp                           /* skip int_no, err_code  */
    iretq

.yield_resume:
    /* IRETQ in 64-bit mode pops RIP, CS, RFLAGS and advances the stack
     * past the RSP/SS slots (40 bytes total, same-ring or not) — the
     * stack lands exactly on the return address pushed by `call`, so
     * `ret` reaches thread_yield()'s caller. */
    mov %rsp, %rdi
    call yield_resume_probe
    ret


/* thread_exit_switch — resume the scheduler-chosen thread.
 *
 * Called from thread_exit(): rdi = next thread's kernel_rsp.
 * Switches stack, pops the full registers_t frame and iretq to the
 * thread where it left off — same layout as isr_common_stub's
 * return path.  Kernel stacks are reclaimed by thread_create() when
 * the zombie slot is reused, never while running on them.
 */
.global thread_exit_switch
thread_exit_switch:
    mov %rdi, %rsp
    pop %rax
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbp
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15

    add $16, %rsp
    iretq

