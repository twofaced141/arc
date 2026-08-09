#ifndef ISR_H
#define ISR_H

#include <stdint.h>
#include "registers.h"

typedef void (*isr_t)(registers_t *);

void isr_init(void);
void register_interrupt_handler(uint8_t n, isr_t handler);
void double_fault_handler(registers_t *r);

#endif
