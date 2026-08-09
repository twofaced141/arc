#include "gdt.h"
#include "debug.h"

#define KERNEL_STACK_SIZE 65536
#define DF_STACK_SIZE     16384

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
static struct tss df_tss;
static struct gdt_entry gdt[7];
static struct gdt_ptr   gdtp;

static uint8_t kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));

/* Dedicated stack for the double-fault handler.  32-bit mode has no IST,
 * so a double fault is delivered through a task gate that switches to this
 * TSS and stack; the handler must never run on a possibly-corrupt thread
 * stack (that would triple fault without diagnostics). */
static uint8_t df_stack[DF_STACK_SIZE] __attribute__((aligned(16)));

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

    /* Double-fault TSS descriptor (sel 0x30).  Never loaded with ltr —
     * the CPU switches to it through the #DF task gate.  Must stay
     * available (not busy) so the task switch can happen. */
    extern void isr8(void);
    gdt_set_gate(6, (uint32_t)&df_tss, sizeof(struct tss) - 1, 0x89, 0x00);

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

    /* Double-fault task: fresh stack, IF cleared, entry at isr8 (the #DF
     * stub: error code was pushed by the CPU after the task switch).  The
     * faulting task's context is saved by the hardware into kernel_tss;
     * CR3 is fixed up after paging comes up (df_tss_sync_cr3). */
    for (uint32_t i = 0; i < sizeof(struct tss) / 4; i++)
        ((uint32_t *)&df_tss)[i] = 0;

    df_tss.cs  = KERNEL_CS;
    df_tss.eip = (uint32_t)&isr8;
    df_tss.ss  = KERNEL_DS;
    df_tss.esp = (uint32_t)&df_stack[DF_STACK_SIZE];
    df_tss.ds = df_tss.es = df_tss.fs = df_tss.gs = KERNEL_DS;
    df_tss.eflags = 0x2;                 /* IF = 0 while in the handler */
    df_tss.iomap_base = sizeof(struct tss);

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

struct tss *tss_get_kernel(void) {
    return &kernel_tss;
}

/* Keep the double-fault task's CR3 in sync with the kernel page directory
 * so the task switch lands on a page table that maps the handler. */
void df_tss_sync_cr3(void) {
    uint32_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    df_tss.cr3 = cr3;
}
