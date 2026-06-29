#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "isr.h"
#include "process.h"

void scheduler_init(void);
void scheduler_add_process(process_t *proc);
void scheduler_remove_process(process_t *proc);
void *scheduler_switch(registers_t *r);
process_t *scheduler_current_process(void);

#endif
