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


#include <stdio.h>
#include <stdint.h>
#include <string.h>

static int failures = 0;
static int total = 0;

#define TEST(name, cond) do { \
    total++; \
    if (!(cond)) { \
        printf("  FAIL: %s\n", name); \
        failures++; \
    } else { \
        printf("  PASS: %s\n", name); \
    } \
} while(0)

static int bitmap_alloc(uint32_t *bitmap, int nwords) {
    for (int w = 0; w < nwords; w++) {
        if (bitmap[w] == 0xFFFFFFFF) continue;
        for (int b = 0; b < 32; b++) {
            if (!(bitmap[w] & (1u << b))) {
                bitmap[w] |= (1u << b);
                return w * 32 + b;
            }
        }
    }
    return -1;
}

static void bitmap_free(uint32_t *bitmap, int slot) {
    int w = slot / 32;
    int b = slot % 32;
    bitmap[w] &= ~(1u << b);
}

void host_bitmap_tests(void) {
    printf("[bitmap]\n");

    {
        uint32_t bm[1] = {0};
        int s = bitmap_alloc(bm, 1);
        TEST("alloc slot 0", s == 0);
        TEST("bit 0 set", bm[0] == 1);
    }

    {
        uint32_t bm[1] = {0};
        int slots[32];
        for (int i = 0; i < 32; i++) {
            slots[i] = bitmap_alloc(bm, 1);
        }
        TEST("alloc all 32 exhaust", bitmap_alloc(bm, 1) == -1);
        bitmap_free(bm, 15);
        int s = bitmap_alloc(bm, 1);
        TEST("re-alloc freed slot 15", s == 15);
    }

    {
        uint32_t bm[4] = {0};
        for (int i = 0; i < 64; i++) bitmap_alloc(bm, 4);
        int s = bitmap_alloc(bm, 4);
        TEST("multi-word slot 64", s == 64);
        bitmap_free(bm, 33);
        s = bitmap_alloc(bm, 4);
        TEST("multi-word re-alloc 33", s == 33);
    }

    {
        uint32_t bm[2] = {0};
        int a = bitmap_alloc(bm, 2);
        int b = bitmap_alloc(bm, 2);
        int c = bitmap_alloc(bm, 2);
        (void)a; (void)c;
        bitmap_free(bm, b);
        bitmap_free(bm, 0);
        int s1 = bitmap_alloc(bm, 2);
        TEST("re-alloc lowest free", s1 == 0);
        int s2 = bitmap_alloc(bm, 2);
        TEST("re-alloc second free", s2 == 1);
    }

    {
        uint32_t bm[1] = {0xFFFFFFFF};
        TEST("full word returns -1", bitmap_alloc(bm, 1) == -1);
    }

    printf("[bitmap] %d/%d passed\n", total - failures, total);
}
