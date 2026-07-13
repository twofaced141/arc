#ifndef FPU_H
#define FPU_H

#include <stdint.h>
#include "isr.h"

struct process;

void fpu_init(void);
void fpu_nm_handler(registers_t *r);
void fpu_clear_owner(struct process *proc);
void fpu_init_state(uint8_t *state);

#endif
