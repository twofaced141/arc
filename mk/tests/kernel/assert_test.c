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
#include "assert.h"
#include "debug.h"

void assert_boot_tests(void) {
    test_group("assert");
    int ok;

    ok = test_check_int(VERIFY(1 == 1), 1);
    test_result("assert_verify_true", ok);

#ifndef CONFIG_DEBUG
    ok = test_check_int(VERIFY(0), 0);
    test_result("assert_verify_false", ok);
#endif

    {
        int x = 42;
        ok = test_check_int(VERIFY(x == 42), 1);
        ok = ok && test_check_int(VERIFY(x), 1);
        ok = ok && test_check_int(VERIFY(!0), 1);
        test_result("assert_verify_expr", ok);
    }

#ifndef CONFIG_DEBUG
    ASSERT(1);
    ASSERT(0);
    test_result("assert_assert_release", 1);
#else
    ASSERT(1);
    test_result("assert_assert_debug", 1);
#endif
}
