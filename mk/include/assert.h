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


#ifndef ASSERT_H
#define ASSERT_H

#ifdef CONFIG_DEBUG

extern void panic_simple(const char *reason);

#define __ASSERT_STR2(x) #x
#define __ASSERT_STR(x)  __ASSERT_STR2(x)

#define ASSERT(cond) \
    do { \
        if (!(cond)) \
            panic_simple("ASSERT(" #cond ") at " __FILE__ ":" __ASSERT_STR(__LINE__)); \
    } while (0)

#define VERIFY(cond) \
    ({ \
        int _v_ = !!(cond); \
        if (!_v_) \
            panic_simple("VERIFY(" #cond ") at " __FILE__ ":" __ASSERT_STR(__LINE__)); \
        _v_; \
    })

#define VM_VERIFY(cond)    ASSERT(cond)
#define LOCK_VERIFY(cond)  ASSERT(cond)
#define VFS_VERIFY(cond)   ASSERT(cond)

#else

#define ASSERT(cond)         ((void)0)
#define VERIFY(cond)         (!!(cond))
#define VM_VERIFY(cond)      ((void)0)
#define LOCK_VERIFY(cond)    ((void)0)
#define VFS_VERIFY(cond)     ((void)0)

#endif

#endif
