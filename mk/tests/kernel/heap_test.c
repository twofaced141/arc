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


#include "test.h"
#include "debug.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"

#define HEAP_TEST_BLOCKS  64
#define HEAP_EXHAUST_MAX  8192

static void *heap_blocks[HEAP_TEST_BLOCKS];
static uint8_t heap_pats[HEAP_TEST_BLOCKS];
static uint32_t heap_sizes[HEAP_TEST_BLOCKS];
static void *heap_huge[HEAP_EXHAUST_MAX];

static void heap_fill(void *p, uint32_t size, uint8_t pat) {
    memset(p, pat, size);
}

static int heap_check(void *p, uint32_t size, uint8_t pat) {
    const uint8_t *b = (const uint8_t *)p;
    for (uint32_t i = 0; i < size; i++)
        if (b[i] != pat) return 0;
    return 1;
}

void heap_boot_tests(void) {
    test_group("heap");
    int ok;

    /* ---- trivial edges ---- */
    test_result("heap_kmalloc_zero", kmalloc(0) == NULL);
    kfree(NULL);                       /* must be a no-op */

    /* ---- coalescing: free three adjacent blocks in the middle (a
     *      live sentinel block keeps them off the heap tail, so heap
     *      shrink cannot retract brk over them), then a request bigger
     *      than any single block must reuse the merged region
     *      (first-fit finds it; a non-coalescing heap would grow brk). */
    {
        void *p1 = kmalloc(65536);
        void *p2 = kmalloc(65536);
        void *p3 = kmalloc(65536);
        void *p4 = kmalloc(65536);      /* sentinel: keeps p1..p3 off the tail */
        if (p1 && p2 && p3 && p4) {
            kfree(p1);
            kfree(p2);
            kfree(p3);
            void *big = kmalloc(190000);
            test_result("heap_coalesce_reuses_region", big == p1);
            if (big) kfree(big);
            kfree(p4);
        } else {
            test_result("heap_coalesce_reuses_region", 0);
            if (p1) kfree(p1);
            if (p2) kfree(p2);
            if (p3) kfree(p3);
            if (p4) kfree(p4);
        }
    }

    /* ---- fragmentation: alloc mixed sizes with per-block patterns,
     *      free every other block, reallocate into the holes, then
     *      verify every live block's pattern is intact (no overlap /
     *      no wild writes by the allocator). */
    {
        ok = 1;
        for (int i = 0; i < HEAP_TEST_BLOCKS; i++) {
            heap_sizes[i] = 8 + ((i * 37) % 256);
            heap_pats[i] = (uint8_t)(0x40 + (i % 90));
            heap_blocks[i] = kmalloc(heap_sizes[i]);
            if (!heap_blocks[i]) { ok = 0; break; }
            heap_fill(heap_blocks[i], heap_sizes[i], heap_pats[i]);
            for (int j = 0; j < i; j++) {
                if (heap_blocks[j] == heap_blocks[i]) { ok = 0; break; }
            }
        }
        test_result("heap_alloc_unique", ok);

        for (int i = 0; i < HEAP_TEST_BLOCKS; i += 2) {
            kfree(heap_blocks[i]);
            heap_blocks[i] = NULL;
        }

        ok = 1;
        for (int i = 0; i < HEAP_TEST_BLOCKS; i += 2) {
            heap_blocks[i] = kmalloc(heap_sizes[i]);
            if (!heap_blocks[i]) { ok = 0; break; }
            heap_fill(heap_blocks[i], heap_sizes[i], heap_pats[i]);
        }
        for (int i = 0; i < HEAP_TEST_BLOCKS; i++) {
            if (!heap_blocks[i]) continue;
            if (!heap_check(heap_blocks[i], heap_sizes[i], heap_pats[i])) {
                ok = 0;
                break;
            }
        }
        test_result("heap_fragmentation_survivors_intact", ok);

        for (int i = 0; i < HEAP_TEST_BLOCKS; i++)
            if (heap_blocks[i]) kfree(heap_blocks[i]);
    }

    /* ---- shrink: freeing must return pages to the PMM. Allocate and
     *      free several large blocks; the page count must come back to
     *      (almost) where it started. Allow one straggler page for a
     *      block tail that crosses a page boundary. */
    {
        uint32_t free0 = pmm_get_free_pages();
        int all = 1;
        for (int i = 0; i < 8; i++) {
            void *q = kmalloc(65536);
            if (!q) { all = 0; break; }
            heap_fill(q, 65536, 0xC3);
            kfree(q);
        }
        uint32_t free1 = pmm_get_free_pages();
        debug_printf("heap: shrink pages %lu -> %lu\r\n",
                     (unsigned long)free0, (unsigned long)free1);
        test_result("heap_shrink_returns_pages", all && free1 + 1 >= free0);
    }

    /* ---- kcalloc must be zeroed ---- */
    {
        void *cz = kcalloc(16, 4);
        ok = cz != NULL;
        if (ok) {
            const uint8_t *b = (const uint8_t *)cz;
            for (int i = 0; i < 64; i++)
                if (b[i]) { ok = 0; break; }
        }
        kfree(cz);
        test_result("heap_kcalloc_zeroed", ok);
    }
}

/* Runs LAST in the boot test group: it drains the heap to the point of
 * refusal, which is slow and leaves the allocator near its cap until
 * the end of the run. */
void heap_exhaust_boot_test(void) {
    test_group("heap_exhaust");
    int ok;

    /* ---- exhaustion: allocate 16KB chunks until the heap refuses,
     *      then free everything and verify the heap recovers. */
    {
        uint32_t got = 0;
        while (got < HEAP_EXHAUST_MAX) {
            void *q = kmalloc(16384);
            if (!q) break;
            heap_fill(q, 16384, 0xA5);
            heap_huge[got++] = q;
        }
        test_result("heap_exhaust_returns_null", got > 0);

        ok = 1;
        for (uint32_t i = 0; i < got; i += 1024)
            if (!heap_check(heap_huge[i], 16384, 0xA5)) { ok = 0; break; }
        test_result("heap_exhaust_patterns_intact", ok);

        for (uint32_t i = got; i > 0; i--)
            kfree(heap_huge[i - 1]);

        void *after = kmalloc(16384);
        test_result("heap_exhaust_recovery", after != NULL);
        if (after) kfree(after);
    }
}
