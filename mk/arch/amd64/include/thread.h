#ifndef THREAD_H
#define THREAD_H

#include <stdint.h>
#include "vmm.h"
#include "isr.h"
#include "spinlock.h"

/* Forward declaration — task.h includes thread.h, so we can't include it here */
struct task;

#define THREAD_UNUSED   0
#define THREAD_READY    1
#define THREAD_RUNNING  2
#define THREAD_BLOCKED  3
#define THREAD_ZOMBIE   4

#define MAX_THREADS     1024
#define THREAD_KSTACK_SIZE  16384

/* Maximum sleep average (in ticks, for O(1) interactive bonus) */
#define MAX_SLEEP_AVG 100

/* Priority range: 0 = highest, 139 = lowest */
#define PRIO_MAX 140
#define MAX_RT_PRIO 100   /* 0-99: RT, 100-139: normal */

typedef struct prio_array prio_array_t;

typedef struct thread {
    uint32_t tid;
    uint32_t state;
    union { uint64_t kernel_rsp; uint64_t kernel_esp; uint32_t pad_kernel_esp; };
    uint8_t *kernel_stack;
    uint64_t kernel_stack_top;
    page_directory_t *page_dir;

    /* Owning task (holds C-space + VM map) */
    struct task *task;

    int32_t static_prio;
    int32_t prio;
    int32_t sleep_avg;
    uint32_t time_slice;
    struct thread *next;
    struct thread *prev;
    prio_array_t *array;

    union { uint64_t rip; uint64_t eip; };
    union { uint64_t user_rsp; uint64_t user_esp; };
    uint32_t sleep_until;
    uint64_t tls_base;              /* CLONE_SETTLS: fs_base for this thread */
    char name[32];

    /* FPU/SSE state — FXSAVE area (x87 + XMM + MXCSR, 512 bytes).
     * 16-byte aligned as required by fxsave/fxrstor.  Switched lazily
     * via CR0.TS + #NM: the state is captured by fxsave only at the
     * moment a new thread takes ownership (see scheduler.c). */
    uint8_t fpu_state[512] __attribute__((aligned(16)));
} thread_t;

/* Initialise a 512-byte FXSAVE image to the architectural x87/SSE
 * reset state (FCW=0x037F, empty tag word, MXCSR=0x1F80). */
static inline void thread_fpu_state_init(uint8_t *st) {
    for (int i = 0; i < 512; i++) st[i] = 0;
    *(uint16_t *)&st[0]  = 0x037F;   /* FCW: RN, 64-bit precision, all exceptions masked */
    *(uint16_t *)&st[4]  = 0xFFFF;   /* FTW: all x87 registers empty */
    *(uint32_t *)&st[32] = 0x1F80;   /* MXCSR: RN, all SIMD exceptions masked */
}

void thread_init(void);
thread_t *thread_create(uint64_t rip, page_directory_t *page_dir, int user);
void thread_exit(int exitcode);
thread_t *thread_current(void);
uint32_t thread_get_tid(void);
thread_t *thread_find(uint32_t tid);

#endif
