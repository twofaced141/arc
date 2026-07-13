#include "panic.h"
#include "debug.h"
#include "terminal.h"
#include "kallsyms.h"

static void panic_putchar(char c) {
    debug_putchar(c);
    if (c == '\r')
        return;
    terminal_putchar(c);
}

static void panic_print(const char *s) {
    while (*s)
        panic_putchar(*s++);
}

static void panic_print_hex32(uint32_t value) {
    static const char hex[] = "0123456789ABCDEF";

    for (int shift = 28; shift >= 0; shift -= 4)
        panic_putchar(hex[(value >> shift) & 0xF]);
}

static void print_register(const char *name, uint32_t value) {
    panic_print(name);
    panic_print(": 0x");
    panic_print_hex32(value);
    panic_print("\r\n");
}

static void panic_print_addr(uint32_t addr) {
    panic_print("  0x");
    panic_print_hex32(addr);
    const char *sym = kallsyms_lookup(addr);
    if (sym) {
        panic_print(" <");
        panic_print(sym);
        panic_print(">");
    }
    panic_print("\r\n");
}

static void panic_stack_trace(const registers_t *r) {
    panic_print("\r\nStack trace:\r\n");
    panic_print("  eip: 0x");
    panic_print_hex32(r->eip);

    if (r->cs & 3) {
        panic_print(" (user mode)\r\n");
        return;
    }

    {
        const char *sym = kallsyms_lookup(r->eip);
        if (sym) {
            panic_print(" <");
            panic_print(sym);
            panic_print(">");
        }
    }
    panic_print("\r\n");

    uint32_t *ebp = (uint32_t *)r->ebp;
    for (int frame = 0; frame < 16 && (uint32_t)ebp >= 0x100000; frame++) {
        uint32_t ret = ebp[1];
        panic_print_addr(ret);
        ebp = (uint32_t *)ebp[0];
    }
}

void panic_simple(const char *reason) {
    panic_print("Panic!\r\n");
    panic_print("Reason: ");
    panic_print(reason);
    panic_print("\r\n");

    uint32_t ebp_val;
    __asm__ __volatile__("mov %%ebp, %0" : "=r"(ebp_val));
    panic_print("\r\nStack trace:\r\n");
    uint32_t *fp = (uint32_t *)ebp_val;
    for (int frame = 0; frame < 16 && (uint32_t)fp >= 0x100000; frame++) {
        uint32_t ret = fp[1];
        panic_print_addr(ret);
        fp = (uint32_t *)fp[0];
    }

    for (;;)
        __asm__ __volatile__("cli; hlt");
}

void panic(const char *reason, const registers_t *r) {
    panic_print("Panic!\r\n");
    panic_print("Reason: ");
    panic_print(reason);
    panic_print("\r\n\r\nRegisters dump:\r\n");

    print_register("EAX", r->eax);
    print_register("EBX", r->ebx);
    print_register("ECX", r->ecx);
    print_register("EDX", r->edx);
    print_register("ESI", r->esi);
    print_register("EDI", r->edi);
    print_register("EBP", r->ebp);
    print_register("ESP", r->esp);
    print_register("EIP", r->eip);
    print_register("CS", r->cs);
    print_register("DS", r->ds);
    print_register("ES", r->es);
    print_register("FS", r->fs);
    print_register("GS", r->gs);
    print_register("SS", r->ss);
    print_register("EFLAGS", r->eflags);
    print_register("USERESP", r->useresp);
    print_register("INT_NO", r->int_no);
    print_register("ERR_CODE", r->err_code);

    panic_stack_trace(r);

    for (;;)
        __asm__ __volatile__("cli; hlt");
}
