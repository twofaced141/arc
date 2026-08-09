#include "idt.h"
#include "debug.h"

static struct idt_entry idt[IDT_ENTRIES];
static struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idt_ptr;

void idt_set_gate_ist(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags, uint8_t ist) {
    idt[num].base_low = base & 0xFFFF;
    idt[num].base_mid = (base >> 16) & 0xFFFF;
    idt[num].base_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].sel = sel;
    idt[num].ist = ist;
    idt[num].flags = flags;
    idt[num].zero = 0;
}

void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags) {
    idt_set_gate_ist(num, base, sel, flags, 0);
}

void pic_remap(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(0x21, 0x00);
    outb(0xA1, 0x00);
}

void pic_send_eoi(unsigned char irq) {
    if (irq >= 8)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

void idt_init(void) {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base = (uint64_t)&idt[0];

    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    __asm__ __volatile__("lidt %0" : : "m"(idt_ptr));
}

/* Expose the installed IDT to the SMP code (AP trampoline shares it). */
void idt_get_ptr(uint64_t *base, uint16_t *limit) {
    *base  = idt_ptr.base;
    *limit = idt_ptr.limit;
}
