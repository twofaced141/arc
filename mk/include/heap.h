/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Unified heap HAL — kernel heap allocator interface.
 * Implemented in mk/vm/heap.c for all architectures.
 */

#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

void heap_init(void);

/* vmm_init_heap is the legacy entry point — now a wrapper over heap_init */
void vmm_init_heap(void);

void *kmalloc(uint32_t size);
void *kcalloc(uint32_t count, uint32_t size);
void  kfree(void *ptr);

#endif
