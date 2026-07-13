#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "block.h"
#include "terminal.h"
#include "idt.h"
#include "isr.h"
#include "keyboard.h"
#include "tty.h"
#include "debug.h"
#include "panic.h"
#include "gdt.h"
#include "pit.h"
#include "pmm.h"
#include "vmm.h"
#include "process.h"
#include "scheduler.h"
#include "multiboot2.h"
#include "pci.h"
#include "fs.h"
#include "fd.h"
#include "ata.h"
#include "ext2.h"
#include "rtc.h"
#include "signal.h"
#include "syscalls.h"
#include "mouse.h"
#include "apic.h"
#include "cpuid.h"
#include "acpi.h"
#include "fpu.h"
#include "e1000.h"
#include "net.h"
#include "ramfs.h"
extern char _binary_initramfs_init_elf_start[];
extern char _binary_initramfs_init_elf_end[];
extern process_t *initramfs_run(void);
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);
extern void isr128(void);

extern void irq0(void);  extern void irq1(void);  extern void irq2(void);  extern void irq3(void);
extern void irq4(void);  extern void irq5(void);  extern void irq6(void);  extern void irq7(void);
extern void irq8(void);  extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void); extern void irq15(void);

static void idt_install(void) {
    uint16_t code_selector = KERNEL_CS;

    idt_init();

    idt_set_gate(0,  (uint32_t)isr0,  code_selector, 0x8E);
    idt_set_gate(1,  (uint32_t)isr1,  code_selector, 0x8E);
    idt_set_gate(2,  (uint32_t)isr2,  code_selector, 0x8E);
    idt_set_gate(3,  (uint32_t)isr3,  code_selector, 0x8E);
    idt_set_gate(4,  (uint32_t)isr4,  code_selector, 0x8E);
    idt_set_gate(5,  (uint32_t)isr5,  code_selector, 0x8E);
    idt_set_gate(6,  (uint32_t)isr6,  code_selector, 0x8E);
    idt_set_gate(7,  (uint32_t)isr7,  code_selector, 0x8E);
    idt_set_gate(8,  (uint32_t)isr8,  code_selector, 0x8E);
    idt_set_gate(9,  (uint32_t)isr9,  code_selector, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, code_selector, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, code_selector, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, code_selector, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, code_selector, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, code_selector, 0x8E);
    idt_set_gate(15, (uint32_t)isr15, code_selector, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, code_selector, 0x8E);
    idt_set_gate(17, (uint32_t)isr17, code_selector, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, code_selector, 0x8E);
    idt_set_gate(19, (uint32_t)isr19, code_selector, 0x8E);
    idt_set_gate(20, (uint32_t)isr20, code_selector, 0x8E);
    idt_set_gate(21, (uint32_t)isr21, code_selector, 0x8E);
    idt_set_gate(22, (uint32_t)isr22, code_selector, 0x8E);
    idt_set_gate(23, (uint32_t)isr23, code_selector, 0x8E);
    idt_set_gate(24, (uint32_t)isr24, code_selector, 0x8E);
    idt_set_gate(25, (uint32_t)isr25, code_selector, 0x8E);
    idt_set_gate(26, (uint32_t)isr26, code_selector, 0x8E);
    idt_set_gate(27, (uint32_t)isr27, code_selector, 0x8E);
    idt_set_gate(28, (uint32_t)isr28, code_selector, 0x8E);
    idt_set_gate(29, (uint32_t)isr29, code_selector, 0x8E);
    idt_set_gate(30, (uint32_t)isr30, code_selector, 0x8E);
    idt_set_gate(31, (uint32_t)isr31, code_selector, 0x8E);
    idt_set_gate(32, (uint32_t)irq0,  code_selector, 0x8E);
    idt_set_gate(33, (uint32_t)irq1,  code_selector, 0x8E);
    idt_set_gate(34, (uint32_t)irq2,  code_selector, 0x8E);
    idt_set_gate(35, (uint32_t)irq3,  code_selector, 0x8E);
    idt_set_gate(36, (uint32_t)irq4,  code_selector, 0x8E);
    idt_set_gate(37, (uint32_t)irq5,  code_selector, 0x8E);
    idt_set_gate(38, (uint32_t)irq6,  code_selector, 0x8E);
    idt_set_gate(39, (uint32_t)irq7,  code_selector, 0x8E);
    idt_set_gate(40, (uint32_t)irq8,  code_selector, 0x8E);
    idt_set_gate(41, (uint32_t)irq9,  code_selector, 0x8E);
    idt_set_gate(42, (uint32_t)irq10, code_selector, 0x8E);
    idt_set_gate(43, (uint32_t)irq11, code_selector, 0x8E);
    idt_set_gate(44, (uint32_t)irq12, code_selector, 0x8E);
    idt_set_gate(45, (uint32_t)irq13, code_selector, 0x8E);
    idt_set_gate(46, (uint32_t)irq14, code_selector, 0x8E);
    idt_set_gate(47, (uint32_t)irq15, code_selector, 0x8E);

    /* Set up IDT entries for vectors 48-255 with default IRQ stubs */
    extern uint32_t irq_default_stubs[];
    for (int i = 0; i < 208; i++)
        if (48 + i != 128)
            idt_set_gate(48 + i, irq_default_stubs[i], code_selector, 0x8E);
    /* Syscall gate must be DPL=3 for ring-3 access */
    idt_set_gate(128, (uint32_t)isr128, code_selector, 0xEE);

    pic_remap();
}

void kernel_main(uint32_t mboot_magic, multiboot2_info_t *mboot_info) {
    debug_init();
    terminal_init();
    terminal_print("arc\n");

    isr_init();
    idt_install();
    tty_init();
    keyboard_init();
    mouse_init();
    register_interrupt_handler(128, syscall_handler);
    register_interrupt_handler(7, fpu_nm_handler);

    if (mboot_magic == MULTIBOOT2_MAGIC) {
        pmm_init(mboot_info);
        {
            uint32_t cr0;
            __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
            cr0 |= (1 << 1) | (1 << 5);
            cr0 &= ~(1 << 2);
            __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0));
        }
        {
            uint32_t cr4;
            __asm__ __volatile__("mov %%cr4, %0" : "=r"(cr4));
            cr4 |= (1 << 9) | (1 << 10);
            __asm__ __volatile__("mov %0, %%cr4" : : "r"(cr4));
        }

        vmm_init();
        vmm_init_heap();

        {
            multiboot2_tag_t *tag = multiboot2_first_tag(mboot_info);
            while (tag->type != 0) {
                if (tag->type == MULTIBOOT_TAG_FRAMEBUFFER) {
                    multiboot2_tag_framebuffer_t *fb_tag = (multiboot2_tag_framebuffer_t *)tag;
                    debug_printf("fb: tag found addr=0x%x %ux%u pitch=%u bpp=%u type=%u\r\n",
                                 fb_tag->fb_addr, fb_tag->fb_width, fb_tag->fb_height,
                                 fb_tag->fb_pitch, fb_tag->fb_bpp, fb_tag->fb_type);
                    if (fb_tag->fb_bpp >= 8) {
                        fb_info_t fb_info;
                        fb_info.addr   = fb_tag->fb_addr;
                        fb_info.pitch  = fb_tag->fb_pitch;
                        fb_info.width  = fb_tag->fb_width;
                        fb_info.height = fb_tag->fb_height;
                        fb_info.bpp    = fb_tag->fb_bpp;
                        fb_info.type   = fb_tag->fb_type;
                        terminal_init_fb(&fb_info);
                    }
                    break;
                }
                tag = multiboot2_next_tag(tag);
            }
        }

        fs_init(mboot_info);

        pmm_refcount_init();

        if (cpuid_has_apic()) {
            debug_printf("apic: supported, enabling...\r\n");
            lapic_init();
            ioapic_init();
            ioapic_mask_pic();
            lapic_timer_init();
            pit_init();
        } else {
            debug_printf("apic: not supported, using PIC+PIT\r\n");
            pit_init();
        }

        block_devices_init();

        rtc_init();
        pci_scan();
        e1000_probe();
        ata_init();
        acpi_init();
        fpu_init();
        net_init();

        process_init();
        scheduler_init();

        block_device_t *root_dev = NULL;
        uint32_t root_lba = 0;
        for (int i = 0; i < block_device_count(); i++) {
            block_device_t *dev = block_device_get(i);
            debug_printf("kernel: probing %s (%u sectors)\r\n", dev->name, dev->lba_count);
            if (ext2_probe(dev, 0) == 0) { root_dev = dev; root_lba = 0; break; }
            mbr_t mbr;
            if (mbr_parse(dev, &mbr) == 0) {
                for (int p = 0; p < 4; p++) {
                    if (mbr.partitions[p].type && ext2_probe(dev, mbr.partitions[p].lba_start) == 0) {
                        root_dev = dev; root_lba = mbr.partitions[p].lba_start; break;
                    }
                }
                if (root_dev) break;
            }
        }
        if (!root_dev) panic_simple("no ext2 filesystem found");
        if (fs_mount("/", root_dev, root_lba) < 0) panic_simple("fs_mount / failed");
        debug_printf("kernel: / mounted from %s lba=%u\r\n", root_dev->name, root_lba);

        /* Try to mount second ext2 at /mnt */
        for (int i = 0; i < block_device_count(); i++) {
            block_device_t *d = block_device_get(i);
            if (d == root_dev) continue;
            if (ext2_probe(d, 0) == 0) { fs_mount("/mnt", d, 0); break; }
            mbr_t mbr;
            if (mbr_parse(d, &mbr) == 0) {
                for (int p = 0; p < 4; p++) {
                    if (mbr.partitions[p].type && ext2_probe(d, mbr.partitions[p].lba_start) == 0) {
                        fs_mount("/mnt", d, mbr.partitions[p].lba_start); goto mnt_done;
                    }
                }
            }
        }
        mnt_done:;

        /* Probe for FAT32 filesystems on remaining devices */
        for (int i = 0; i < block_device_count(); i++) {
            block_device_t *d = block_device_get(i);
            if (d == root_dev) continue;
            if (fat_probe(d, 0) == 0) {
                debug_printf("kernel: FAT32 on %s lba=0\r\n", d->name);
                fs_mount_fat(d, 0);
                break;
            }
            mbr_t fmbr;
            if (mbr_parse(d, &fmbr) == 0) {
                for (int p = 0; p < 4; p++) {
                    if (fmbr.partitions[p].type == 0x0B || fmbr.partitions[p].type == 0x0C) {
                        if (fat_probe(d, fmbr.partitions[p].lba_start) == 0) {
                            debug_printf("kernel: FAT32 on %s lba=%u\r\n", d->name, fmbr.partitions[p].lba_start);
                            fs_mount_fat(d, fmbr.partitions[p].lba_start);
                            goto fat_done;
                        }
                    }
                }
            }
        }
        fat_done:;

        process_t *proc = NULL;
        int initramfs_size_val = (int)(_binary_initramfs_init_elf_end - _binary_initramfs_init_elf_start);
        if (initramfs_size_val > 0) {
            debug_printf("initramfs: loading /init (%d bytes)\r\n", initramfs_size_val);
            proc = initramfs_run();
        }

        if (!proc) {
            static const char *paths[] = {"/sbin/init","/etc/init","/bin/init","/bin/sh"};
            file_t *f = NULL;
            for (int i = 0; i < 4; i++) { f = fs_open(paths[i], EXT2_ROOT_INO); if (f) break; }
            if (!f) panic_simple("no init found");
            proc = process_create_elf(f);
            if (!proc) panic_simple("process_create_elf failed");
        }

        scheduler_add_process(proc);
        debug_print("kernel: about to enable interrupts\r\n");
    } else {
        debug_printf("kernel: not booted with Multiboot2\r\n");
    }

    __asm__ __volatile__("sti");
    debug_print("kernel: sti done, entering idle loop\r\n");

    for (;;) {
        terminal_flush();
        __asm__ __volatile__("hlt");
    }
}
