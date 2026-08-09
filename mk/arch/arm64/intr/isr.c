#include "isr.h"
#include "gic.h"
#include "clkevt_arm.h"
#include "uart.h"
#include "vmm.h"
#include "personality.h"
#include <stdint.h>

extern uint64_t scheduler_switch(registers_t *r);

static isr_t interrupt_handlers[256];

static const char *const exception_classes[64] = {
    [0x00] = "Unknown",
    [0x01] = "Trapped WF*",
    [0x03] = "Trapped MCR/MRC",
    [0x04] = "Trapped MCRR/MRRC",
    [0x05] = "Trapped MRS/MSR",
    [0x06] = "Trapped SVE/SME",
    [0x07] = "Trapped TSTART",
    [0x14] = "SVC (AArch32)",
    [0x15] = "SVC (AArch64)",
    [0x16] = "Trapped MSR/MRS (AArch64)",
    [0x17] = "Access to SVE/SME",
    [0x18] = "Pointer Auth",
    [0x20] = "Instr Abort (lower EL)",
    [0x21] = "Instr Abort (same EL)",
    [0x22] = "PC alignment",
    [0x24] = "Data Abort (lower EL)",
    [0x25] = "Data Abort (same EL)",
    [0x26] = "SP alignment",
    [0x27] = "Trapped FP",
    [0x28] = "SError",
    [0x2C] = "Breakpoint (lower EL)",
    [0x2D] = "Breakpoint (same EL)",
    [0x2E] = "SW Step (lower EL)",
    [0x2F] = "SW Step (same EL)",
    [0x30] = "Watchpoint (lower EL)",
    [0x31] = "Watchpoint (same EL)",
    [0x34] = "BKPT (AArch32)",
    [0x35] = "BRK (AArch64)",
};

void isr_init(void) {
    for (int i = 0; i < 256; i++)
        interrupt_handlers[i] = 0;
}

void register_interrupt_handler(uint8_t n, isr_t handler) {
    interrupt_handlers[n] = handler;
}

void exception_init(void) {
    extern void vectors(void);
    __asm__ __volatile__("msr vbar_el1, %0" : : "r"(&vectors));
}

void irq_enable(void) {
    __asm__ __volatile__("msr daifclr, #2");
}

static void dump_regs(registers_t *r) {
    uart_print("\n=== EXCEPTION DUMP ===\n");
    for (int i = 0; i < 30; i += 4) {
        uart_print("x");
        if (i < 10) uart_putchar('0' + i);
        else uart_putchar('0' + i / 10), uart_putchar('0' + i % 10);
        uart_print("=");
        uart_print_hex64(r->x[i]);
        uart_print(" x");
        if (i + 1 < 10) uart_putchar('0' + i + 1);
        else uart_putchar('0' + (i + 1) / 10), uart_putchar('0' + (i + 1) % 10);
        uart_print("=");
        uart_print_hex64(r->x[i + 1]);

        if (i + 2 < 30) {
            uart_print(" x");
            if (i + 2 < 10) uart_putchar('0' + i + 2);
            else uart_putchar('0' + (i + 2) / 10), uart_putchar('0' + (i + 2) % 10);
            uart_print("=");
            uart_print_hex64(r->x[i + 2]);
            uart_print(" x");
            if (i + 3 < 10) uart_putchar('0' + i + 3);
            else uart_putchar('0' + (i + 3) / 10), uart_putchar('0' + (i + 3) % 10);
            uart_print("=");
            uart_print_hex64(r->x[i + 3]);
        }
        uart_putchar('\n');
    }
    uart_print("lr=");
    uart_print_hex64(r->lr);
    uart_print(" sp=");
    uart_print_hex64(r->sp);
    uart_print("\n");
    uart_print("elr=");
    uart_print_hex64(r->elr);
    uart_print(" spsr=");
    uart_print_hex64(r->spsr);
    uart_print("\n");
    uart_print("esr=");
    uart_print_hex64(r->esr);
    uart_print(" far=");
    uart_print_hex64(r->far);
    uart_print("\n");
    uint32_t ec = (r->esr >> 26) & 0x3F;
    uart_print("ec=");
    uart_print_hex64(ec);
    if (exception_classes[ec]) {
        uart_print(" (");
        uart_print(exception_classes[ec]);
        uart_print(")");
    }
    uart_print("\n");
    uart_print("=== END DUMP ===\n");
}

/* AArch64 SVC ABI: x8=sysno(+1024), x0-3=args, ret in x0 */
static void handle_svc(registers_t *r) {
    if (bsd_syscall_dispatch)
        r->x[0] = (uint64_t)bsd_syscall_dispatch(r);
}

void sync_handler(registers_t *r) {
    uint32_t ec = (r->esr >> 26) & 0x3F;

    if (ec == 0x24 || ec == 0x25) {
        uint64_t fault_addr = r->far;
        if (vmm_handle_page_fault(r, fault_addr, (uint32_t)r->esr))
            return;
    }

    if (ec == 0x15) {
        handle_svc(r);
        return;
    }

    dump_regs(r);
    uart_print("\narc: stopping\n");
    for (;;)
        __asm__ __volatile__("wfi");
}

uint64_t irq_handler(registers_t *r) {
    uint32_t iar = GICC_IAR;
    uint32_t irq = iar & 0x3FF;

    if (interrupt_handlers[irq])
        interrupt_handlers[irq](r);

    GICC_EOIR = iar;

    if (irq == TIMER_IRQ)
        return scheduler_switch(r);

    return (uint64_t)r;
}

void fiq_handler(registers_t *r) {
    (void)r;
    uart_print("arc: unexpected FIQ\n");
}

void serr_handler(registers_t *r) {
    (void)r;
    uart_print("arc: unexpected SError\n");
}
