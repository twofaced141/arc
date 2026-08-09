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


#include "clocksource.h"

static struct clocksource *current_cs;
static uint64_t cs_mult;
static unsigned cs_shift;

void clocksource_register(struct clocksource *cs) {
    current_cs = cs;
    cs_shift = 10;
    cs_mult = (1000000000ULL << cs_shift) / cs->freq;
    if (cs_mult == 0) {
        cs_shift = 0;
        cs_mult = 1000000000ULL / cs->freq;
    }
}

struct clocksource *clocksource_current(void) {
    return current_cs;
}

uint64_t clocksource_read(void) {
    return current_cs ? current_cs->read() : 0;
}

uint64_t clocksource_read_ns(void) {
    uint64_t counter = clocksource_read();
    if (current_cs && current_cs->mask)
        counter &= current_cs->mask;
    return (counter * cs_mult) >> cs_shift;
}

uint32_t clocksource_freq(void) {
    return current_cs ? current_cs->freq : 0;
}
