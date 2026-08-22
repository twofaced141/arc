/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * bsd/net/net.c — glue between ARC BSD personality and lwIP
 *
 * No-op until lwIP is vendored (bsd/net/lwip/src missing).
 * After vendor_lwip.sh it will call lwip_init() + net_timer_tick().
 */

void net_init(void);
void net_timer_tick(void);

/* net_init / net_timer_tick are implemented in if.c when lwIP is present.
 * This file remains as a fallback alias so that callers linking only net.c
 * still resolve.  The real implementations are in if.c.
 * If you need to avoid duplicate symbols, keep this file empty when
 * LWIP is vendored — the linker will pick the strong definitions from if.c.
 */
