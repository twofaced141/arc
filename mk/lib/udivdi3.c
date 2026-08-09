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
