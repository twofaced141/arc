/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * bsd/net/net.c — glue between ARC BSD personality and lwIP
 *
 * Пока lwIP не завендорен (bsd/net/lwip/src отсутствует) — no-op.
 * После vendor_lwip.sh здесь будет lwip_init() + net_timer_tick().
 */

void net_init(void) {
    /* no-op until lwIP is vendored; keep compilation without lwIP headers */
}

void net_timer_tick(void) {
}
