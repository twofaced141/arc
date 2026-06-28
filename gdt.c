#include "gdt.h"

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

static struct gdt_entry gdt[3];
static struct gdt_ptr   gdtp;

static void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t granularity) {
    gdt[num].base_low    = base & 0xFFFF;
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;

    gdt[num].limit_low   = limit & 0xFFFF;
    gdt[num].granularity  = ((limit >> 16) & 0x0F) | (granularity & 0xF0);

    gdt[num].access = access;
}

void gdt_install(void) {
    gdtp.limit = sizeof(struct gdt_entry) * 3 - 1;
    gdtp.base  = (uint32_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);                          /* null descriptor   */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);           /* kernel code 0x08  */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);           /* kernel data 0x10  */

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
        : : "m"(gdtp) : "eax", "memory"
    );
}
