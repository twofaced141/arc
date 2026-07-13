#include "apic.h"
#include "idt.h"
#include "vmm.h"
#include "debug.h"

static volatile uint32_t *ioapic = (volatile uint32_t *)IOAPIC_VADDR;

static uint32_t ioapic_read(uint32_t reg) {
    ioapic[IOAPIC_IOREGSEL / 4] = reg;
    return ioapic[IOAPIC_IOWIN / 4];
}

static void ioapic_write(uint32_t reg, uint32_t val) {
    ioapic[IOAPIC_IOREGSEL / 4] = reg;
    ioapic[IOAPIC_IOWIN / 4] = val;
}

static void ioapic_redirect_isa_irq(uint8_t isa_irq, uint8_t vector, uint32_t apic_id, int masked) {
    uint32_t reg = IOAPIC_REDTBL + isa_irq * 2;
    uint32_t low = vector | IOAPIC_RED_FIXED | IOAPIC_RED_PHYSICAL
                   | IOAPIC_RED_EDGE | IOAPIC_RED_ACTIVEHIGH;
    uint32_t high = (apic_id & 0xFF) << 24;

    if (masked)
        low |= IOAPIC_RED_MASK;

    ioapic_write(reg, low);
    ioapic_write(reg + 1, high);
}

void ioapic_init(void) {
    uint32_t apic_id;
    uint32_t ver;
    int max_pin;

    vmm_map_page(vmm_get_kernel_directory(), IOAPIC_PHYS_BASE, IOAPIC_VADDR,
                 VMM_PRESENT | VMM_WRITABLE | VMM_CACHE_DISABLE);

    ver = ioapic_read(IOAPIC_VER);
    max_pin = (ver >> 16) & 0xFF;
    apic_id = lapic_read(LAPIC_ID);

    debug_printf("ioapic: ver=%x max_pin=%u apic_id=%x\r\n", ver, max_pin, apic_id);

    for (int i = 0; i <= max_pin; i++) {
        ioapic_write(IOAPIC_REDTBL + i * 2, IOAPIC_RED_MASK);
        ioapic_write(IOAPIC_REDTBL + i * 2 + 1, 0);
    }

    for (int i = 0; i < 16; i++) {
        uint8_t vec = 32 + i;
        ioapic_redirect_isa_irq((uint8_t)i, vec, apic_id & 0xFF, 1);
    }

    /* IRQ0 goes through PIC→LAPIC LINT0 ExtINT, not IOAPIC */
    ioapic_redirect_isa_irq(0, 32, apic_id & 0xFF, 1);
    ioapic_redirect_isa_irq(1, 33, apic_id & 0xFF, 0);
    ioapic_redirect_isa_irq(4, 36, apic_id & 0xFF, 0);
    ioapic_redirect_isa_irq(8, 40, apic_id & 0xFF, 0);

    /* Verify IOAPIC routing for IRQ0 */
    uint32_t irq0_lo = ioapic_read(IOAPIC_REDTBL + 0 * 2);
    uint32_t irq0_hi = ioapic_read(IOAPIC_REDTBL + 0 * 2 + 1);
    debug_printf("ioapic: IRQ0 redir lo=0x%x hi=0x%x\r\n", irq0_lo, irq0_hi);

    debug_printf("ioapic: initialized\r\n");
}

void ioapic_mask_pic(void) {
    /* Keep IRQ0 (bit 0) unmasked for PIT → PIC → LAPIC LINT0 ExtINT path */
    outb(0x21, 0xFE);  /* master PIC: IRQ0 unmasked, IRQ1-7 masked */
    outb(0xA1, 0xFF);  /* slave PIC: all masked */
    debug_printf("ioapic: PIC masked (IRQ0 unmasked for ExtINT)\r\n");
}
