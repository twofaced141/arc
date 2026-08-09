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
#include "task.h"
#include "vmm.h"
#include "string.h"

#define CSPACE_ROUNDS  300     /* forces growth past the default 256 */

static int cspace_slots[CSPACE_ROUNDS];

static void cspace_free_all(cspace_t *cs) {
    for (int i = 1; i < cs->max_slots; i++)
        if (cs->slots[i].in_use)
            cspace_free_slot(cs, i);
}

void cspace_boot_tests(void) {
    test_group("cspace");
    int ok;
    cspace_t cs;

    test_result("cspace_init", cspace_init(&cs) == 0);

    /* ---- slot 0 is reserved ---- */
    {
        int s = cspace_alloc_slot(&cs, CAP_PORT, CAP_SEND, 0x1111);
        test_result("cspace_slot0_reserved", s == 1);
        test_result("cspace_lookup_slot0_null", cspace_lookup(&cs, 0) == NULL);
        test_result("cspace_free_slot0_fails", cspace_free_slot(&cs, 0) == -1);
        if (s >= 0) cspace_free_slot(&cs, s);
    }

    /* ---- type/rights/object_id roundtrip ---- */
    {
        ok = 1;
        for (int i = 0; i < 10; i++) {
            int s = cspace_alloc_slot(&cs, CAP_PORT, CAP_SEND | CAP_RECV,
                                      0x1000 + i);
            if (s < 0) { ok = 0; break; }
            cslot_t *c = cspace_lookup(&cs, s);
            if (!c || c->type != CAP_PORT ||
                c->rights != (CAP_SEND | CAP_RECV) ||
                c->object_id != (uint64_t)(0x1000 + i) || !c->in_use) {
                ok = 0;
                break;
            }
        }
        test_result("cspace_roundtrip_fields", ok);
        for (int i = 0; i < 10; i++)
            cspace_free_slot(&cs, 1 + i);
    }

    /* ---- free semantics: double free, bogus slot, out of range ---- */
    {
        int sf = cspace_alloc_slot(&cs, CAP_THREAD, CAP_READ, 0x7777);
        ok = sf >= 0;
        if (ok) {
            ok = cspace_free_slot(&cs, sf) == 0 &&
                 cspace_lookup(&cs, sf) == NULL &&
                 cspace_free_slot(&cs, sf) == -1 &&      /* double free */
                 cspace_free_slot(&cs, -1) == -1 &&
                 cspace_free_slot(&cs, cs.max_slots) == -1;
        }
        test_result("cspace_free_semantics", ok);
    }

    /* ---- growth past the default 256 slots ---- */
    {
        ok = 1;
        for (int i = 0; i < CSPACE_ROUNDS; i++) {
            cspace_slots[i] = cspace_alloc_slot(&cs, CAP_MEMORY,
                                                CAP_READ | CAP_WRITE,
                                                0x2000 + i);
            if (cspace_slots[i] < 0) { ok = 0; break; }
        }
        if (ok) {
            for (int i = 0; i < CSPACE_ROUNDS; i++) {
                cslot_t *c = cspace_lookup(&cs, cspace_slots[i]);
                if (!c || c->object_id != (uint64_t)(0x2000 + i) ||
                    c->type != CAP_MEMORY) {
                    ok = 0;
                    break;
                }
            }
        }
        test_result("cspace_grow_past_256", ok);
        for (int i = 0; i < CSPACE_ROUNDS; i++)
            if (cspace_slots[i] >= 0) cspace_free_slot(&cs, cspace_slots[i]);
    }

    /* ---- move semantics ---- */
    {
        cspace_t cs2;
        test_result("cspace_init_dst", cspace_init(&cs2) == 0);

        int sm = cspace_alloc_slot(&cs, CAP_PORT, CAP_SEND, 0xAAAA);
        int dm = sm >= 0 ? cspace_move(&cs, &cs2, sm) : -1;
        ok = dm >= 0 &&
             cspace_lookup(&cs, sm) == NULL &&
             cspace_lookup(&cs2, dm) != NULL &&
             cspace_lookup(&cs2, dm)->object_id == 0xAAAA &&
             cspace_lookup(&cs2, dm)->type == CAP_PORT &&
             cspace_lookup(&cs2, dm)->rights == CAP_SEND;
        test_result("cspace_move_transfer", ok);
        test_result("cspace_move_unused_fails",
                    cspace_move(&cs, &cs2, sm) == -1);
        if (dm >= 0) cspace_free_slot(&cs2, dm);
        cspace_destroy(&cs2);
    }

    /* ---- exhaustion at the hard cap, then recovery ---- */
    {
        uint32_t got = 0;
        while (got < CSPACE_MAX_SLOTS) {
            int s = cspace_alloc_slot(&cs, CAP_PORT, 0, 0x3333);
            if (s < 0) break;
            got++;
        }
        /* slot 0 is reserved, so the allocatable capacity is MAX-1 */
        debug_printf("cspace: exhausted got=%lu of %d max=%d\r\n",
                     (unsigned long)got, CSPACE_MAX_SLOTS - 1, cs.max_slots);
        test_result("cspace_exhaust_at_cap", got == CSPACE_MAX_SLOTS - 1);
        test_result("cspace_exhaust_alloc_fails",
                    cspace_alloc_slot(&cs, CAP_PORT, 0, 1) == -1);

        /* one free slot must be immediately reusable */
        int r = cspace_free_slot(&cs, 1) == 0
                    ? cspace_alloc_slot(&cs, CAP_PORT, 0, 1)
                    : -1;
        test_result("cspace_exhaust_slot_reuse", r >= 0);

        cspace_free_all(&cs);
        test_result("cspace_exhaust_recovery",
                    cspace_alloc_slot(&cs, CAP_PORT, 0, 1) >= 0);
    }

    cspace_free_all(&cs);
    cspace_destroy(&cs);

    /* ---- task teardown with live caps must not crash ---- */
    {
        task_t *t = task_create("csp_teardown");
        ok = t != NULL;
        if (ok) {
            int a = cspace_alloc_slot(&t->cspace, CAP_PORT, CAP_SEND, 0xDEADBEEF);
            int b = cspace_alloc_slot(&t->cspace, CAP_THREAD, CAP_READ, 0xCAFEBABE);
            ok = (a >= 0 && b >= 0);
            task_destroy(t);
        }
        test_result("cspace_task_destroy_live_caps", ok);

        /* the task hash must still work after a destroy */
        task_t *t2 = task_create("csp_again");
        test_result("cspace_task_create_after_destroy", t2 != NULL);
        if (t2) task_destroy(t2);
    }
}
