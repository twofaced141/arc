#ifndef THREAD_H
#define THREAD_H

#include <stdint.h>
#include "isr.h"

struct task;

#define THREAD_UNUSED   0
#define THREAD_READY    1
#define THREAD_RUNNING  2
#define THREAD_BLOCKED  3
#define THREAD_ZOMBIE   4

#define MAX_THREADS     1024
#define THREAD_KSTACK_SIZE  8192
#define MAX_SLEEP_AVG   100
#define PRIO_MAX        140
#define MAX_RT_PRIO     100

typedef struct prio_array prio_array_t;

typedef struct thread {
    uint32_t tid;
    uint32_t state;
    uint64_t kernel_rsp;
    uint8_t *kernel_stack;
    uint64_t kernel_stack_top;
    void *page_dir;
    struct task *task;
    int32_t static_prio;
    int32_t prio;
    int32_t sleep_avg;
    uint32_t time_slice;
    struct thread *next;
    struct thread *prev;
    prio_array_t *array;
    uint64_t entry;
    uint32_t sleep_until;
    uint64_t tls_base;              /* per-thread TLS base (unused on arm64 yet) */
    char name[32];

    /* FP/SIMD state — Q0-Q31 (each 16 bytes, offsets 0..496), then
     * FPSR (512) and FPCR (516).  16-byte aligned for stp/ldp q.     *
     * Switched eagerly in the scheduler (no lazy TS mechanism on     *
     * AArch64 EL1).                                                   */
    uint8_t fpu_state[576] __attribute__((aligned(16)));
} thread_t;

/* Initialise the FP/SIMD state image to a clean reset state. */
static inline void thread_fpu_state_init(uint8_t *st) {
    for (int i = 0; i < 576; i++) st[i] = 0;
    *(uint32_t *)&st[516] = 0x00000000;   /* FPCR: round-to-nearest, no traps */
    *(uint32_t *)&st[512] = 0x00000000;   /* FPSR */
}

void thread_init(void);
thread_t *thread_create(uint64_t entry, void *page_dir, int user);
void thread_exit(int exitcode);
thread_t *thread_current(void);
uint32_t thread_get_tid(void);
thread_t *thread_find(uint32_t tid);

#endif
