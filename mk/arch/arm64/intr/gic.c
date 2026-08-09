#include "gic.h"
#include "uart.h"

void gic_init(void) {
    uint32_t type = GICD_TYPER;
    (void)type;

    GICD_CTLR = 1;
    GICC_CTLR = 1;
    GICC_PMR = 0xFF;

    uart_print("gic: initialized\n");
}

void gic_enable_irq(uint32_t irq) {
    if (irq > 1019) return;
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;
    uint32_t prio_reg = irq / 4;
    uint32_t prio_shift = (irq % 4) * 8;

    GICD_IPRIORITYR(prio_reg) &= ~(0xFF << prio_shift);
    GICD_IPRIORITYR(prio_reg) |= (0x80 << prio_shift);
    GICD_ISENABLER(reg) = (1u << bit);
}

void gic_disable_irq(uint32_t irq) {
    if (irq > 1019) return;
    uint32_t reg = irq / 32;
    uint32_t bit = irq % 32;
    GICD_ICENABLER(reg) = (1u << bit);
}

void gic_eoi(uint32_t irq) {
    GICC_EOIR = irq;
}
