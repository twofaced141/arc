#ifndef IO_CHANNEL_H
#define IO_CHANNEL_H

#include <stdint.h>
#include <stddef.h>
#include "spinlock.h"

/* ================================================================
 * io_channel.h — Generic I/O Request Channel
 *
 * A request queue that lets kernel subsystems delegate I/O work to
 * userspace drivers.  The pattern is:
 *
 *   kernel code → io_channel_request(ch, &req) → [enqueue, spin]
 *                                                   ↓
 *   userspace driver ← sys_io_get_request()        |
 *                   → AHCI DMA / MMIO              |
 *                   → sys_io_complete()  ──────────→ [wake caller]
 *
 * Opcodes and arg[] are device-specific; the channel itself is
 * opcode-agnostic.  New driver types (NVMe, e1000, input) reuse
 * the same channel + syscall interface.
 * ================================================================ */

/* ---- Request descriptor ---- */

#define IO_REQ_MAX_ARG  4
#define IO_REQ_BUF_SIZE 4096  /* max DMA buffer per request */

struct io_request {
    uint64_t request_id;       /* opaque — pass back to complete() */
    uint32_t opcode;           /* driver-specific operation code   */
    uint32_t flags;            /* IORQF_* flags                    */
    uint64_t arg[IO_REQ_MAX_ARG]; /* driver-specific arguments     */
    uint64_t buf_phys;         /* physical address of data buffer  */
    uint64_t buf_size;         /* size in bytes                    */
};

/* Flag bits */
#define IORQF_READ   0x0001   /* data flows: device → buf_phys   */
#define IORQF_WRITE  0x0002   /* data flows: buf_phys → device   */

/* ---- Channel handle (opaque, used by kernel and syscalls) ---- */

#define IO_CHANNEL_MAX_NAME 48
#define IO_CHANNEL_MAX      8
#define IO_CHANNEL_QUEUE_DEPTH 16

struct io_channel_entry {
    struct io_request  req;
    volatile int       completed;   /* written by completer, read by waiter */
    int                result;      /* completion status */
    uint32_t           waiting_tid; /* TID of blocked thread, 0 if none */
};

typedef struct {
    char                       name[IO_CHANNEL_MAX_NAME];
    struct io_channel_entry    queue[IO_CHANNEL_QUEUE_DEPTH];
    int                        head;   /* dequeue index (userspace reads) */
    int                        tail;   /* enqueue index (kernel writes)  */
    int                        count;
    spinlock_t                 lock;
} io_channel_t;

/* ---- Kernel API ---- */

/* Create an I/O channel.  Returns handle (0 .. IO_CHANNEL_MAX-1)
 * on success, -1 on failure. */
int io_channel_create(const char *name);

/* Send a request through a channel.
 * Blocks (scheduler-aware) until the userspace driver completes it.
 * Returns 0 on success, negative on error. */
int io_channel_request(int handle, struct io_request *req);

/* Kernel-side lookup by name (used by block_ipc etc.). */
int io_channel_lookup(const char *name);

/* Internal helpers (called from sys_driver.c dispatch handlers) */
int io_channel_get_request(int handle, struct io_request *user_req);
int io_channel_complete(int handle, uint64_t request_id, int result);

/* Init */
void io_channel_init(void);

#endif /* IO_CHANNEL_H */
