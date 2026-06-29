#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "isr.h"
#include "process.h"

void scheduler_init(void);
void scheduler_add_process(process_t *proc);
void *scheduler_switch(registers_t *r);

#endif
