#include "apic.h"
#include "cpuid.h"
#include "idt.h"
#include "pit.h"
#include "isr.h"
#include "vmm.h"
#include "debug.h"

static volatile uint32_t *lapic = (volatile uint32_t *)LAPIC_VADDR;

int apic_enabled = 0;

uint32_t lapic_read(uint32_t reg) {
    return lapic[reg / 4];
}

void lapic_write(uint32_t reg, uint32_t val) {
    lapic[reg / 4] = val;
    lapic[LAPIC_ID / 4];
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

void lapic_init(void) {
    uint32_t eax, edx;
    uint64_t msr;

    if (!cpuid_has_apic()) {
        debug_printf("lapic: not supported by CPU\r\n");
        return;
    }

    __asm__ __volatile__("rdmsr" : "=a"(eax), "=d"(edx) : "c"(IA32_APIC_BASE));
    msr = ((uint64_t)edx << 32) | eax;

    if (!(msr & (1 << 11))) {
        msr |= (1 << 11);
        if (!(msr & (1 << 10)))
            msr |= (1 << 10);
        eax = (uint32_t)msr;
        edx = (uint32_t)(msr >> 32);
        __asm__ __volatile__("wrmsr" : : "a"(eax), "d"(edx), "c"(IA32_APIC_BASE));
    }

    vmm_map_page(vmm_get_kernel_directory(), LAPIC_PHYS_BASE, LAPIC_VADDR,
                 VMM_PRESENT | VMM_WRITABLE | VMM_CACHE_DISABLE);

    debug_printf("lapic: id=%x version=%x\r\n",
                 lapic_read(LAPIC_ID), lapic_read(0x030));

    lapic_write(LAPIC_SVR, 0xFF | LAPIC_SVR_ENABLE);
    lapic_write(LAPIC_DFR, 0xFFFFFFFF);
    lapic_write(LAPIC_LDR, 0);
    lapic_write(LAPIC_TPR, 0);

    lapic_write(LAPIC_LVT_TIMER,  LAPIC_LVT_MASKED);
    /* LINT0: ExtINT delivery from the master PIC (for PIT IRQ0) */
    /* Mask vector bits, use ExtINT (111) delivery, not masked */
    lapic_write(LAPIC_LVT_LINT0, (7 << 8));  /* ExtINT delivery, unmasked */
    lapic_write(LAPIC_LVT_LINT1,  LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR,  LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_PM,     LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);

    apic_enabled = 1;
    debug_printf("lapic: initialized\r\n");
}

static uint32_t lapic_timer_calibrate(void) {
    outb(0x43, 0x30);
    outb(0x40, 0xFF);
    outb(0x40, 0xFF);

    lapic_write(LAPIC_TIMER_DCR, LAPIC_TIMER_DIV16);
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);
    lapic_read(LAPIC_TIMER_CCR);

    uint32_t pit_target = PIT_BASE_FREQ / 100;

    outb(0x43, 0x00);
    uint32_t pit_start = inb(0x40) | (inb(0x40) << 8);

    while (1) {
        outb(0x43, 0x00);
        uint32_t pit_cur = inb(0x40) | (inb(0x40) << 8);
        uint32_t elapsed = pit_start - pit_cur;
        if (elapsed >= pit_target)
            break;
    }

    uint32_t lapic_elapsed = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CCR);
    return lapic_elapsed;
}

void lapic_timer_init(void) {
    uint32_t count = lapic_timer_calibrate();
    debug_printf("lapic: timer calibration count=%u\r\n", count);

    /* Mask the LAPIC timer — PIT will be used as system timer instead */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);

    debug_printf("lapic: timer masked, using PIT\r\n");
}
