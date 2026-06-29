#include "isr.h"
#include "idt.h"
#include "panic.h"
#include "debug.h"
#include "terminal.h"

static isr_t interrupt_handlers[256];
static int logged_first_user_irq;

static const char *const exception_messages[32] = {
    "Division By Zero",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved"
};

void isr_handler(registers_t *r) {
    if (interrupt_handlers[r->int_no]) {
        interrupt_handlers[r->int_no](r);
        return;
    }

    const char *reason = "Unknown Exception";
    if (r->int_no < 32)
        reason = exception_messages[r->int_no];

    panic(reason, r);
}

void irq_handler(registers_t *r) {
    if (!logged_first_user_irq && ((r->cs & 3) == 3)) {
        logged_first_user_irq = 1;
        debug_printf("irq: ring3 int=%u eip=%p cs=%x\r\n",
                     r->int_no - 32, r->eip, r->cs);
    }

    if (interrupt_handlers[r->int_no])
        interrupt_handlers[r->int_no](r);

    pic_send_eoi((unsigned char)(r->int_no - 32));
}

void register_interrupt_handler(uint8_t n, isr_t handler) {
    interrupt_handlers[n] = handler;
}

void isr_init(void) {
    for (int i = 0; i < 256; i++)
        interrupt_handlers[i] = 0;
}
