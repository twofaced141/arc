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


#include "isr.h"
#include "idt.h"
#include "panic.h"
#include "debug.h"
#include "gdt.h"
#include "string.h"
#include "personality.h"

static isr_t interrupt_handlers[256];
static int logged_first_user_irq;
static int in_double_fault;

static const char *const exception_messages[32] = {
    "Division By Zero",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point Exception",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point Exception",
    "Virtualization Exception",
    "Control Protection Exception",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception",
    "Security Exception",
    "Reserved"
};

/* Runs on the dedicated double-fault stack (task gate → df_tss).  The
 * hardware saved the faulting task's context into the shared kernel TSS;
 * rebuild a registers_t from it so the panic dump shows where the first
 * fault happened, not the zeros of the df_tss. */
void double_fault_handler(registers_t *r) {
    struct tss *ts = tss_get_kernel();
    registers_t saved;

    log_printf(LOG_LEVEL_ERROR, "\nDouble Fault: kernel stack overflow or "
                 "corrupted kernel stack (handled on dedicated DF stack)\n");

    if (in_double_fault) {
        log_print(LOG_LEVEL_ERROR, "re-entrant double fault — halting\n");
        __asm__ __volatile__("cli; hlt");
        for (;;) __asm__ __volatile__("hlt");
    }
    in_double_fault = 1;

    memset(&saved, 0, sizeof(saved));
    saved.eax = ts->eax; saved.ecx = ts->ecx; saved.edx = ts->edx;
    saved.ebx = ts->ebx; saved.esp = ts->esp; saved.ebp = ts->ebp;
    saved.esi = ts->esi; saved.edi = ts->edi;
    saved.eip = ts->eip; saved.cs = ts->cs; saved.eflags = ts->eflags;
    saved.ss = ts->ss;
    saved.int_no = 8;
    saved.err_code = r->err_code;

    panic("Double Fault", &saved);
}

void isr_handler(registers_t *r) {
    if (interrupt_handlers[r->int_no]) {
        interrupt_handlers[r->int_no](r);
        return;
    }

    const char *reason = "Unknown Exception";
    if (r->int_no < 32)
        reason = exception_messages[r->int_no];

    panic(reason, r);
}

void irq_handler(registers_t *r) {
    if (!logged_first_user_irq && ((r->cs & 3) == 3)) {
        logged_first_user_irq = 1;
        debug_printf("irq: ring3 int=%u eip=%p cs=%x\r\n",
                     r->int_no - 32, r->eip, r->cs);
    }

    if (interrupt_handlers[r->int_no])
        interrupt_handlers[r->int_no](r);

    /* Always send EOI to the PIC */
    if (r->int_no >= 32 && r->int_no < 48)
        pic_send_eoi((unsigned char)(r->int_no - 32));

    /* Wake any user-space IRQ subscriber (BSD layer).
     * Subscribers identify IRQs by their ISA number (IRQ0-15, matching
     * the APIC/PIC vector 32-47 mapping), not by the raw IDT vector. */
    if (sys_driver_irq_dispatch) {
        uint8_t irq = (r->int_no >= 32 && r->int_no < 48)
                        ? (uint8_t)(r->int_no - 32) : (uint8_t)r->int_no;
        sys_driver_irq_dispatch(irq);
    }
}

void register_interrupt_handler(uint8_t n, isr_t handler) {
    interrupt_handlers[n] = handler;
}

void isr_init(void) {
    for (int i = 0; i < 256; i++)
        interrupt_handlers[i] = 0;
}
