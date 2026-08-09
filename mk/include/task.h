#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "spinlock.h"

/* ================================================================
 * task.h — Task + C-space (capability space) for the arc kernel.
 *
 * A task is the container for:
 *   - C-space (array of capability slots)
 *   - VM map (address space)
 *   - Threads
 *
 * User-visible handle = (task_id << 32) | slot_index.
 * This gives O(1) cspace lookup through the owning task's slot array.
 * ================================================================ */

#define TASK_NAME_MAX      32
#define CSPACE_DEFAULT_SLOTS  256
#define CSPACE_MAX_SLOTS   65536      /* hard upper bound for sanity */
#define CSPACE_WORD_BITS   32
#define CSPACE_BITMAP_WORDS  (CSPACE_MAX_SLOTS / CSPACE_WORD_BITS)  /* 2048 */

/* ---- Capability types ---- */
#define CAP_PORT     1
#define CAP_MEMORY   2
#define CAP_THREAD   3

/* ---- Capability rights ---- */
#define CAP_SEND     (1u << 0)
#define CAP_RECV     (1u << 1)
#define CAP_REPLY    (1u << 2)
#define CAP_READ     (1u << 3)
#define CAP_WRITE    (1u << 4)
#define CAP_EXEC     (1u << 5)

/* ---- Forward declarations ---- */
struct vm_map;
struct thread;
struct ipc_port;

/* ---- C-space ---- */

typedef struct cslot {
    uint64_t object_id;     /* kernel object pointer / ID */
    uint32_t type;          /* CAP_PORT | CAP_MEMORY | CAP_THREAD */
    uint32_t rights;        /* rights bitmap */
    int      in_use;
} cslot_t;

typedef struct cspace {
    cslot_t   *slots;            /* dynamically allocated */
    uint32_t   *free_bitmap;     /* dynamically allocated */
    int         max_slots;       /* current capacity */
    uint32_t    bitmap_words;    /* words in free_bitmap */
    spinlock_t  lock;
} cspace_t;

/* ---- Task ---- */

typedef struct task {
    uint32_t  task_id;
    char      name[TASK_NAME_MAX];
    cspace_t  cspace;
    struct vm_map  *map;           /* address space */
    struct thread *main_thread;    /* first / primary thread */
    struct task   *next;           /* hash table link / global list */
} task_t;

/* ---- Handle encoding / decoding ---- */

static inline uint64_t task_make_handle(uint32_t task_id, int slot) {
    return ((uint64_t)task_id << 32) | (uint32_t)(slot & 0x7FFFFFFF);
}

static inline uint32_t handle_task_id(uint64_t handle) {
    return (uint32_t)(handle >> 32);
}

static inline int handle_slot(uint64_t handle) {
    return (int)(handle & 0x7FFFFFFF);
}

/* ================================================================
 * API
 * ================================================================ */

/* ---- Task lifecycle ---- */
void     task_init(void);
task_t  *task_create(const char *name);
void     task_destroy(task_t *task);
task_t  *task_find(uint32_t task_id);
task_t  *task_current(void);    /* task of the running thread */

/* ---- C-space operations ---- */
int      cspace_init(cspace_t *cs);
void     cspace_destroy(cspace_t *cs);
int      cspace_alloc_slot(cspace_t *cs, uint32_t type, uint32_t rights, uint64_t object_id);
int      cspace_free_slot(cspace_t *cs, int slot);
cslot_t *cspace_lookup(cspace_t *cs, int slot);
int      cspace_move(cspace_t *from, cspace_t *to, int slot);

/* ---- Syscalls ---- */
int sys_task_create(void);
int sys_task_destroy(void);
int sys_slot_alloc(uint32_t type, uint32_t rights);
int sys_slot_free(uint64_t handle);

#endif /* TASK_H */
