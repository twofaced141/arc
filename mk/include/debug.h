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


#ifndef DEBUG_H
#define DEBUG_H

#include <stdint.h>
#include <stdarg.h>

void debug_init(void);
void debug_putchar(char c);
void debug_write(const char *buf, unsigned int count);
void debug_print(const char *s);
void debug_println(const char *s);
void debug_print_hex8(uint8_t value);
void debug_print_hex16(uint16_t value);
void debug_print_hex32(uint32_t value);
void debug_print_hex64(uint64_t value);
void debug_print_dec(uint32_t value);
void debug_printf(const char *fmt, ...);

/* Lock-free variants for panic paths: no serial_lock (a CPU panicking
 * while another holds the lock must not spin forever).  Callers are
 * responsible for exclusive UART ownership (see panic_owner in panic.c). */
void debug_print_raw(const char *s);
void debug_printf_raw(const char *fmt, ...);

/* ── Log levels ─────────────────────────────────────────────────────
 * Messages logged with log_print()/log_printf() are only emitted when
 * their level is <= the global log level (log_set_level()).
 *
 *   LOG_LEVEL_ERROR   (0) — failures, faults, corruption
 *   LOG_LEVEL_WARN    (1) — recoverable anomalies, fallbacks
 *   LOG_LEVEL_INFO    (2) — milestones, per-device/per-fs registration
 *   LOG_LEVEL_DEBUG   (3) — verbose per-instance detail
 *
 * Default is LOG_LEVEL_INFO.  Override at boot with "loglevel=" on the
 * kernel command line (error|warn|info|debug or 0-3), or at runtime
 * with log_set_level().  debug_print()/debug_printf() are unconditional
 * and always emit regardless of the level.
 */

typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_WARN  = 1,
    LOG_LEVEL_INFO  = 2,
    LOG_LEVEL_DEBUG = 3,
} log_level_t;

void log_set_level(log_level_t level);
log_level_t log_get_level(void);
void log_print(log_level_t level, const char *s);
void log_printf(log_level_t level, const char *fmt, ...);
void log_parse_cmdline(const char *cmdline);

/* Fatal error: print reason and halt the system (mk/lib/panic.c) */
void panic_simple(const char *reason);

#endif
