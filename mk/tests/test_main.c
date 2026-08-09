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

void test_banner(const char *msg) {
    debug_print(msg);
    debug_print("\r\n");
}

void test_group(const char *name) {
    debug_print("test: [");
    debug_print(name);
    debug_print("]\r\n");
}

int test_check(int cond) {
    return cond;
}

int test_check_u64(uint64_t a, uint64_t b) {
    return a == b;
}

int test_check_int(int a, int b) {
    return a == b;
}

int test_check_ptr(const void *a, const void *b) {
    return a == b;
}

void test_result(const char *name, int ok) {
    debug_print("test: ");
    debug_print(name);
    debug_print(ok ? " PASS\r\n" : " FAIL\r\n");
}

/* ---- Boot test group declarations ---- */
void vm_object_boot_tests(void);
void vm_map_boot_tests(void);
void ipc_boot_tests(void);
void assert_boot_tests(void);
void pmm_boot_tests(void);
void heap_boot_tests(void);
void heap_exhaust_boot_test(void);
void cspace_boot_tests(void);

void test_run_boot_tests(void) {
    test_banner("mk: tests enabled");
    /* heap_boot_tests() must run before the memory-heavy groups: it
     * allocates big contiguous blocks and asserts first-fit reuses the
     * coalesced region (p1), which only holds on a mostly-empty heap.
     * heap_exhaust_boot_test() runs LAST: it drains the heap to the
     * point of refusal, which is slow and leaves the allocator near its
     * cap until the run ends. */
    pmm_boot_tests();
    heap_boot_tests();
    cspace_boot_tests();
    vm_object_boot_tests();
    vm_map_boot_tests();
    ipc_boot_tests();
    assert_boot_tests();
    heap_exhaust_boot_test();
    test_banner("mk: tests done");
}
