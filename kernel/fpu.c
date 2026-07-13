#include "fpu.h"
#include "process.h"
#include "scheduler.h"
#include "debug.h"

static uint8_t fpu_clean_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
static struct process *fpu_owner;

void fpu_init(void) {
    __asm__ __volatile__("fninit");
    __asm__ __volatile__("fxsave %0" : "=m"(fpu_clean_state));
    fpu_owner = NULL;
    debug_print("fpu: initialized\r\n");
}

void fpu_nm_handler(registers_t *r) {
    (void)r;
    uint32_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1 << 3);
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));

    process_t *cur = scheduler_current_process();
    if (fpu_owner == cur) return;

    if (fpu_owner)
        __asm__ __volatile__("fxsave %0" : "=m"(fpu_owner->fpu_state));

    fpu_owner = cur;
    if (cur)
        __asm__ __volatile__("fxrstor %0" : : "m"(cur->fpu_state));
}

void fpu_clear_owner(process_t *proc) {
    if (fpu_owner == proc)
        fpu_owner = NULL;
}

void fpu_init_state(uint8_t *state) {
    for (int i = 0; i < FPU_STATE_SIZE; i++)
        state[i] = fpu_clean_state[i];
}
