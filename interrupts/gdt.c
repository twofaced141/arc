#include "gdt.h"

#define KERNEL_STACK_SIZE 65536

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct tss kernel_tss;
static struct gdt_entry gdt[7];
static struct gdt_ptr   gdtp;

static uint8_t kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t granularity) {
    gdt[num].base_low    = base & 0xFFFF;
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;

    gdt[num].limit_low   = limit & 0xFFFF;
    gdt[num].granularity  = ((limit >> 16) & 0x0F) | (granularity & 0xF0);

    gdt[num].access = access;
}

void gdt_install(void) {
    gdtp.limit = sizeof(struct gdt_entry) * 7 - 1;
    gdtp.base  = (uint32_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);                          /* null descriptor   */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);           /* kernel code 0x08  */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);           /* kernel data 0x10  */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);           /* user code   0x18  */
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);           /* user data   0x20  */

    /* TSS descriptor: base = &kernel_tss, limit = sizeof(tss)-1, access = 0xE9, gran = 0x00 */
    uint32_t tss_base = (uint32_t)&kernel_tss;
    uint32_t tss_limit = sizeof(struct tss) - 1;
    gdt_set_gate(5, tss_base, tss_limit, 0xE9, 0x00);

    /* TLS descriptor: user data segment, base = USER_TLS_VADDR (0xBFFFB000), 4GB limit */
    gdt_set_gate(6, 0xBFFFB000, 0xFFFFFFFF, 0xF2, 0xCF);

    /* Clear TSS and set kernel stack at top of kernel stack area */
    for (uint32_t i = 0; i < sizeof(struct tss) / 4; i++)
        ((uint32_t *)&kernel_tss)[i] = 0;

    kernel_tss.ss0 = KERNEL_DS;
    kernel_tss.esp0 = (uint32_t)&kernel_stack[KERNEL_STACK_SIZE];
    kernel_tss.cs = KERNEL_CS | 3;
    kernel_tss.ds = USER_DS;
    kernel_tss.es = USER_DS;
    kernel_tss.fs = USER_DS;
    kernel_tss.gs = USER_DS;
    kernel_tss.ss = USER_DS;
    kernel_tss.iomap_base = sizeof(struct tss);

    __asm__ __volatile__(
        "lgdt %0\n\t"
        "ljmp $0x08, $.Lgdt_reload\n\t"
        ".Lgdt_reload:\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "mov $0x28, %%ax\n\t"
        "ltr %%ax\n\t"
        : : "m"(gdtp) : "eax", "memory"
    );
}

void tss_set_kernel_stack(uint32_t stack) {
    kernel_tss.esp0 = stack;
}
