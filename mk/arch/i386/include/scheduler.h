#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include "isr.h"
#include "thread.h"

#define BITMAP_SIZE  5
#define PRIO_ARRAY_BITS 140

struct prio_array {
    int nr_active;
    thread_t *queue[PRIO_ARRAY_BITS];
    uint32_t bitmap[BITMAP_SIZE];
};

typedef struct prio_array prio_array_t;

void scheduler_init(void);
void scheduler_add_thread(thread_t *thread);
void scheduler_remove_thread(thread_t *thread);
void scheduler_unblock_thread(thread_t *thread);
void *scheduler_switch(registers_t *r);
thread_t *scheduler_current_thread(void);
void scheduler_set_nice(thread_t *t, int nice);

/* Block the calling thread until clockevent ticks reach deadline.
 * Returns 0 when woken, -1 if the sleep was not set up.
 */
int scheduler_sleep_ticks(uint64_t deadline);
void scheduler_block_current(void);

void thread_yield(void);

#endif
