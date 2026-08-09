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


/* 64-bit divide/modulo helpers for 32-bit targets (libgcc-compatible).
 * x86_64/aarch64 have native 64-bit divide instructions and never call
 * these, but on i386 the compiler emits __udivdi3/__umoddi3/__divdi3/__moddi3
 * for any uint64_t/int64_t division. */

#include <stdint.h>

static uint64_t udiv_qr(uint64_t n, uint64_t d, uint64_t *r)
{
	uint64_t q = 0, bit = 1;

	if (d == 0) {
		*r = n;
		return 0;
	}
	while (d < n && !(d >> 63)) {
		d <<= 1;
		bit <<= 1;
	}
	for (; bit; bit >>= 1) {
		if (n >= d) {
			n -= d;
			q |= bit;
		}
		d >>= 1;
	}
	*r = n;
	return q;
}

uint64_t __udivdi3(uint64_t n, uint64_t d)
{
	uint64_t r;

	return udiv_qr(n, d, &r);
}

uint64_t __umoddi3(uint64_t n, uint64_t d)
{
	uint64_t r;

	udiv_qr(n, d, &r);
	return r;
}

int64_t __divdi3(int64_t a, int64_t b)
{
	uint64_t q, r;
	int neg = (a < 0) != (b < 0);
	uint64_t ua = a < 0 ? (uint64_t)-a : (uint64_t)a;
	uint64_t ub = b < 0 ? (uint64_t)-b : (uint64_t)b;

	q = udiv_qr(ua, ub, &r);
	return neg ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t a, int64_t b)
{
	uint64_t r;
	uint64_t ua = a < 0 ? (uint64_t)-a : (uint64_t)a;
	uint64_t ub = b < 0 ? (uint64_t)-b : (uint64_t)b;

	udiv_qr(ua, ub, &r);
	return a < 0 ? -(int64_t)r : (int64_t)r;
}
