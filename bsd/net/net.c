/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * bsd/net/net.c — glue between ARC BSD personality and lwIP
 *
 * No-op until lwIP is vendored (bsd/net/lwip/src missing).
 * After vendor_lwip.sh it will call lwip_init() + net_timer_tick().
 */

void net_init(void) {
    /* no-op until lwIP is vendored; keep compilation without lwIP headers */
}

void net_timer_tick(void) {
}
