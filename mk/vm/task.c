/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026, fierce
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS AS IS AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */


#include "task.h"
#include "port.h"
#include "thread.h"
#include "scheduler.h"
#include "vmm.h"
#include "vm_object.h"
#include "debug.h"
#include "string.h"

/* ================================================================
 * task.c — Task lifecycle
 *
 * Tasks are individually kmalloc'd and looked up through a
 * fixed-size hash table (256 buckets, linked-list chains).
 * ================================================================ */

#define TASK_HASH_BITS   8
#define TASK_HASH_SIZE   (1u << TASK_HASH_BITS)  /* 256 */

/* Forward declarations from cspace.c */
extern int cspace_init(cspace_t *cs);
extern void cspace_destroy(cspace_t *cs);

/* ---- Hash table ---- */
static task_t *task_hash[TASK_HASH_SIZE];
static uint32_t next_task_id = 1;
static spinlock_t task_lock = SPINLOCK_INIT;

static inline uint32_t task_hash_id(uint32_t tid) {
    /* Simple hash: use lower bits of task_id */
    return tid & (TASK_HASH_SIZE - 1);
}

void task_init(void) {
    memset(task_hash, 0, sizeof(task_hash));
    next_task_id = 1;
    log_print(LOG_LEVEL_DEBUG, "task: init done\r\n");
}

task_t *task_create(const char *name) {
    task_t *t = (task_t *)kmalloc(sizeof(task_t));
    if (!t)
        return NULL;

    memset(t, 0, sizeof(task_t));

    if (name) {
        int ni = 0;
        while (name[ni] && ni < TASK_NAME_MAX - 1) {
            t->name[ni] = name[ni];
            ni++;
        }
        t->name[ni] = '\0';
    }

    if (cspace_init(&t->cspace) < 0) {
        kfree(t);
        return NULL;
    }

    uint32_t flags;
    spin_lock_irqsave(&task_lock, &flags);

    t->task_id = next_task_id++;
    t->map     = NULL;
    t->main_thread = NULL;

    /* Insert into hash table */
    uint32_t idx = task_hash_id(t->task_id);
    t->next = task_hash[idx];
    task_hash[idx] = t;

    spin_unlock_irqrestore(&task_lock, flags);

    log_printf(LOG_LEVEL_DEBUG, "task: created id=%u '%s'\r\n", t->task_id, t->name);
    return t;
}

void task_destroy(task_t *task) {
    if (!task) return;

    uint32_t flags;
    spin_lock_irqsave(&task_lock, &flags);

    /* Unlink from hash table */
    uint32_t idx = task_hash_id(task->task_id);
    task_t **pp = &task_hash[idx];
    while (*pp) {
        if (*pp == task) {
            *pp = task->next;
            break;
        }
        pp = &(*pp)->next;
    }

    spin_unlock_irqrestore(&task_lock, flags);

    /* Free all C-space slots (destroys referenced objects) */
    for (int i = 0; i < task->cspace.max_slots; i++) {
        if (task->cspace.slots && task->cspace.slots[i].in_use) {
            cspace_free_slot(&task->cspace, i);
        }
    }

    /* Destroy VM map if present */
    if (task->map) {
        vm_map_destroy(task->map);
        vmm_free_directory((page_directory_t *)task->map->pml4);
        kfree(task->map);
        task->map = NULL;
    }

    cspace_destroy(&task->cspace);
    kfree(task);

    log_printf(LOG_LEVEL_DEBUG, "task: destroyed\r\n");
}

task_t *task_find(uint32_t task_id) {
    if (task_id == 0) return NULL;

    uint32_t flags;
    spin_lock_irqsave(&task_lock, &flags);

    uint32_t idx = task_hash_id(task_id);
    task_t *t = task_hash[idx];
    while (t) {
        if (t->task_id == task_id) {
            spin_unlock_irqrestore(&task_lock, flags);
            return t;
        }
        t = t->next;
    }

    spin_unlock_irqrestore(&task_lock, flags);
    return NULL;
}

task_t *task_current(void) {
    thread_t *thr = thread_current();
    if (!thr) return NULL;
    return thr->task;
}

/* ================================================================
 * Syscall handlers
 * ================================================================ */

int sys_task_create(void) {
    task_t *t = task_create("user_task");
    if (!t) return -1;

    /* Create a VM map for this task */
    vm_map_t *map = (vm_map_t *)kmalloc(sizeof(vm_map_t));
    if (!map) {
        task_destroy(t);
        return -1;
    }

    page_directory_t *pml4 = vmm_create_directory();
    if (!pml4) {
        kfree(map);
        task_destroy(t);
        return -1;
    }

    if (vm_map_init(map, (struct page_directory *)pml4) < 0) {
        vmm_free_directory(pml4);
        kfree(map);
        task_destroy(t);
        return -1;
    }

    t->map = map;

    return (int)t->task_id;
}

int sys_task_destroy(void) {
    task_t *cur = task_current();
    if (!cur) return -1;
    task_destroy(cur);
    return 0;
}

int sys_slot_alloc(uint32_t type, uint32_t rights) {
    task_t *cur = task_current();
    if (!cur) return -1;

    int slot = cspace_alloc_slot(&cur->cspace, type, rights, 0);
    if (slot < 0) return -1;

    return (int)task_make_handle(cur->task_id, slot);
}

int sys_slot_free(uint64_t handle) {
    uint32_t tid = handle_task_id(handle);
    int slot = handle_slot(handle);

    task_t *t = task_find(tid);
    if (!t) return -1;

    return cspace_free_slot(&t->cspace, slot);
}
