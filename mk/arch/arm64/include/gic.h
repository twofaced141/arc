#ifndef GIC_H
#define GIC_H

#include <stdint.h>

#define GICD_BASE   0x08000000
#define GICC_BASE   0x08010000

#define GICD_CTLR          (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_TYPER         (*(volatile uint32_t *)(GICD_BASE + 0x004))
#define GICD_IIDR          (*(volatile uint32_t *)(GICD_BASE + 0x008))
#define GICD_ISENABLER(n)  (*(volatile uint32_t *)(GICD_BASE + 0x100 + (n) * 4))
#define GICD_ICENABLER(n)  (*(volatile uint32_t *)(GICD_BASE + 0x180 + (n) * 4))
#define GICD_IPRIORITYR(n) (*(volatile uint32_t *)(GICD_BASE + 0x400 + (n) * 4))
#define GICD_ITARGETSR(n)  (*(volatile uint32_t *)(GICD_BASE + 0x800 + (n) * 4))
#define GICD_ICPENDR(n)    (*(volatile uint32_t *)(GICD_BASE + 0x280 + (n) * 4))

#define GICC_CTLR   (*(volatile uint32_t *)(GICC_BASE + 0x0000))
#define GICC_PMR    (*(volatile uint32_t *)(GICC_BASE + 0x0004))
#define GICC_IAR    (*(volatile uint32_t *)(GICC_BASE + 0x000C))
#define GICC_EOIR   (*(volatile uint32_t *)(GICC_BASE + 0x0010))

#define GICC_PMR_PRIO 0xFF

void gic_init(void);
void gic_enable_irq(uint32_t irq);
void gic_disable_irq(uint32_t irq);
void gic_eoi(uint32_t irq);

#endif
