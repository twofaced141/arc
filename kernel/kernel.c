#include <stddef.h>
#include <stdint.h>
#include "terminal.h"
#include "idt.h"
#include "isr.h"
#include "keyboard.h"
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

#define VGA_BUFFER 0xB8000
#define VGA_WIDTH  80
#define VGA_HEIGHT 25

enum vga_color {
    VGA_BLACK         = 0,
    VGA_BLUE          = 1,
    VGA_GREEN         = 2,
    VGA_CYAN          = 3,
    VGA_RED           = 4,
    VGA_MAGENTA       = 5,
    VGA_BROWN         = 6,
    VGA_LIGHT_GREY    = 7,
    VGA_DARK_GREY     = 8,
    VGA_LIGHT_BLUE    = 9,
    VGA_LIGHT_GREEN   = 10,
    VGA_LIGHT_CYAN    = 11,
    VGA_LIGHT_RED     = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_LIGHT_BROWN   = 14,
    VGA_WHITE         = 15,
};

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
    return (uint16_t)uc | (uint16_t)color << 8;
}

size_t terminal_row;
size_t terminal_column;
uint8_t terminal_color;
static uint16_t *terminal_buffer;
static void terminal_scroll(void) {
    for (size_t y = 1; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_buffer[(y - 1) * VGA_WIDTH + x] = terminal_buffer[y * VGA_WIDTH + x];
        }
    }
    size_t last = (VGA_HEIGHT - 1) * VGA_WIDTH;
    for (size_t x = 0; x < VGA_WIDTH; x++)
        terminal_buffer[last + x] = vga_entry(' ', terminal_color);
    terminal_row = VGA_HEIGHT - 1;
}

static void terminal_update_cursor(void) {
    uint16_t pos = terminal_row * VGA_WIDTH + terminal_column;
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void terminal_init(void) {
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = vga_entry_color(VGA_LIGHT_GREY, VGA_BLACK);
    terminal_buffer = (uint16_t *)VGA_BUFFER;
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = vga_entry(' ', terminal_color);
        }
    }
    terminal_update_cursor();
}

void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y) {
    const size_t index = y * VGA_WIDTH + x;
    terminal_buffer[index] = vga_entry(c, color);
}

void terminal_putchar(char c) {
    if (c == '\n') {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_scroll();
        terminal_update_cursor();
        return;
    }
    terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT)
            terminal_scroll();
    }
    terminal_update_cursor();
}

void terminal_write(const char *data, size_t size) {
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

void terminal_print(const char *data) {
    while (*data)
        terminal_putchar(*data++);
}

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
    idt_set_gate(128, (uint32_t)isr128, code_selector, 0xEE);

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

    pic_remap();
}

static const char *syscall_names[] = {
    "getpid", "putc", "yield", "exit", "write", "read",
    "sleep", "getticks", "open", "close", "lseek", "fstat", "brk", "sbrk",
    "fork", "execve", "waitpid", "chdir", "getcwd", "listdir", "kill", "dup2"
};

extern uint32_t scheduler_syscall_no;

static void syscall_handler(registers_t *r) {
    process_t *cur = scheduler_current_process();
    scheduler_syscall_no = r->eax;

    switch (r->eax) {
    case SYSCALL_FORK: {
        if (cur) {
            process_t *child = process_fork(cur, r);
            if (child) {
                scheduler_add_process(child);
                r->eax = child->pid;
            } else {
                r->eax = -1;
            }
        } else {
            r->eax = -1;
        }
        break;
    }
    case SYSCALL_EXECVE: {
        const char *path = (const char *)r->ebx;
        if (cur && process_exec(cur, path, r, cur->cwd_inode) == 0) {
            r->eax = 0;
        } else {
            r->eax = -1;
        }
        break;
    }
    case SYSCALL_GETPID:
        r->eax = cur ? cur->pid : 0;
        break;
    case SYSCALL_PUTC: {
        char c = (char)r->ebx;
        terminal_putchar(c);
        debug_putchar(c);
        break;
    }
    case SYSCALL_YIELD:
        cur->kernel_esp = (uint32_t)r;
        cur->state = PROC_READY;
        break;
    case SYSCALL_EXIT:
        if (cur) {
            cur->exit_code = r->ebx;
            cur->kernel_esp = (uint32_t)r;
            process_exit(cur);
        }
        break;
    case SYSCALL_WRITE:
        r->eax = fd_write(cur->fd_table, (int)r->ebx, (void *)r->ecx, r->edx);
        break;
    case SYSCALL_READ:
        r->eax = fd_read(cur->fd_table, (int)r->ebx, (void *)r->ecx, r->edx);
        break;
    case SYSCALL_SLEEP: {
        uint32_t ms = r->ebx;
        cur->sleep_until = pit_get_ticks() + (ms + 9) / 10;
        cur->kernel_esp = (uint32_t)r;
        cur->state = PROC_BLOCKED;
        break;
    }
    case SYSCALL_GETTICKS:
        r->eax = pit_get_ticks();
        break;
    case SYSCALL_OPEN:
        r->eax = fd_open(cur->fd_table, (const char *)r->ebx, r->ecx, cur->cwd_inode);
        break;
    case SYSCALL_CLOSE:
        r->eax = fd_close(cur->fd_table, (int)r->ebx);
        break;
    case SYSCALL_LSEEK:
        r->eax = fd_lseek(cur->fd_table, (int)r->ebx, (int32_t)r->ecx, (int)r->edx);
        break;
    case SYSCALL_FSTAT:
        r->eax = fd_fstat(cur->fd_table, (int)r->ebx, (stat_t *)r->ecx);
        break;
    case SYSCALL_BRK: {
        uint32_t new_brk = r->ebx;
        if (new_brk == 0 || (new_brk >= USER_HEAP_START && new_brk < 0xC0000000)) {
            if (new_brk != 0) {
                uint32_t new_mapped = (new_brk + PAGE_SIZE - 1) & ~0xFFF;
                uint32_t old_mapped = cur->heap_mapped_end;
                for (uint32_t a = old_mapped; a < new_mapped; a += PAGE_SIZE) {
                    uint32_t phys = (uint32_t)pmm_alloc_page();
                    if (!phys) { r->eax = -1; break; }
                    vmm_map_page(cur->page_dir, phys, a,
                                VMM_PRESENT | VMM_WRITABLE | VMM_USER);
                }
                cur->heap_mapped_end = new_mapped > old_mapped ? new_mapped : old_mapped;
                cur->heap_break = new_brk;
            }
        }
        r->eax = cur->heap_break;
        break;
    }
    case SYSCALL_SBRK: {
        int32_t inc = (int32_t)r->ebx;
        uint32_t old = cur->heap_break;
        uint32_t new_brk = old + inc;
        if (new_brk >= USER_HEAP_START && new_brk < 0xC0000000) {
            uint32_t new_mapped = (new_brk + PAGE_SIZE - 1) & ~0xFFF;
            uint32_t old_mapped = cur->heap_mapped_end;
            for (uint32_t a = old_mapped; a < new_mapped; a += PAGE_SIZE) {
                uint32_t phys = (uint32_t)pmm_alloc_page();
                if (!phys) { r->eax = -1; break; }
                vmm_map_page(cur->page_dir, phys, a,
                            VMM_PRESENT | VMM_WRITABLE | VMM_USER);
            }
            cur->heap_mapped_end = new_mapped > old_mapped ? new_mapped : old_mapped;
            cur->heap_break = new_brk;
        }
        r->eax = old;
        break;
    }
    case SYSCALL_WAITPID: {
        int pid = (int)r->ebx;
        uint32_t *status = (uint32_t *)r->ecx;
        int options = (int)r->edx;
        int result = process_waitpid(cur, pid, status, options);
        if (result == -2) {
            cur->kernel_esp = (uint32_t)r;
            cur->state = PROC_BLOCKED;
            r->eax = 0;
        } else {
            r->eax = result >= 0 ? (uint32_t)result : -1;
        }
        break;
    }
    case SYSCALL_CHDIR: {
        const char *path = (const char *)r->ebx;
        if (!path || !cur) { r->eax = -1; break; }
        ext2_fs_t *fs = fs_get_ext2();
        if (!fs || !fs->present) { r->eax = -1; break; }
        uint32_t ino;
        uint8_t type;
        if (ext2_resolve(fs, cur->cwd_inode, path, &ino, &type) == 0 && type == EXT2_FT_DIR) {
            cur->cwd_inode = ino;
            r->eax = 0;
        } else {
            r->eax = -1;
        }
        break;
    }
    case SYSCALL_GETCWD: {
        char *buf = (char *)r->ebx;
        uint32_t size = r->ecx;
        if (!buf || size == 0 || !cur) { r->eax = -1; break; }
        ext2_fs_t *fs = fs_get_ext2();
        if (!fs || !fs->present) { r->eax = -1; break; }

        uint32_t ino_chain[256];
        int n = 0;
        uint32_t ino = cur->cwd_inode;
        int err = 0;
        while (ino != EXT2_ROOT_INO && n < 256) {
            ino_chain[n++] = ino;
            uint32_t parent;
            uint8_t t;
            if (ext2_lookup(fs, ino, "..", &parent, &t) < 0) { err = 1; break; }
            ino = parent;
        }
        if (err) { r->eax = -1; break; }

        int pos = 0;
        if (n == 0) {
            if (pos < (int)size) buf[pos++] = '/';
        } else {
            buf[pos++] = '/';
            for (int i = n - 1; i >= 0; i--) {
                uint32_t parent_ino = (i < n - 1) ? ino_chain[i + 1] : EXT2_ROOT_INO;
                char name[EXT2_NAME_MAX + 1];
                if (ext2_find_name(fs, parent_ino, ino_chain[i], name) < 0)
                    { err = 1; break; }
                for (int k = 0; name[k] && pos < (int)size - 1; k++)
                    buf[pos++] = name[k];
                if (i > 0 && pos < (int)size - 1)
                    buf[pos++] = '/';
            }
        }
        if (err) { r->eax = -1; break; }
        if (pos < (int)size) buf[pos] = '\0';
        r->eax = pos;
        break;
    }
    case SYSCALL_LISTDIR: {
        const char *path = (const char *)r->ebx;
        char *buf = (char *)r->ecx;
        uint32_t size = r->edx;
        if (!buf || !cur) { r->eax = -1; break; }
        ext2_fs_t *fs = fs_get_ext2();
        if (!fs || !fs->present) { r->eax = -1; break; }

        uint32_t dir_ino;
        uint8_t type;
        if (!path || path[0] == '\0') {
            dir_ino = cur->cwd_inode;
        } else {
            if (ext2_resolve(fs, cur->cwd_inode, path, &dir_ino, &type) < 0 ||
                type != EXT2_FT_DIR) {
                r->eax = -1; break;
            }
        }

        uint32_t bytes = 0;
        int count = ext2_read_names(fs, dir_ino, buf, size, &bytes);
        if (count < 0) { r->eax = -1; break; }
        r->eax = (int)bytes;
        break;
    }
    case SYSCALL_KILL: {
        int pid = (int)r->ebx;
        if (cur && pid == (int)cur->pid) {
            cur->exit_code = 0;
            cur->kernel_esp = (uint32_t)r;
            process_exit(cur);
            scheduler_syscall_no = 3;
        } else {
            r->eax = process_kill(pid);
        }
        break;
    }
    case SYSCALL_DUP2: {
        int oldfd = (int)r->ebx;
        int newfd = (int)r->ecx;
        r->eax = fd_dup2(cur->fd_table, oldfd, newfd);
        break;
    }
    case SYSCALL_PIPE: {
        int *pipefd = (int *)r->ebx;
        if (!pipefd) { r->eax = -1; break; }
        int fds[2];
        if (fd_pipe(cur->fd_table, fds) == 0) {
            pipefd[0] = fds[0];
            pipefd[1] = fds[1];
            r->eax = 0;
        } else {
            r->eax = -1;
        }
        break;
    }
    }
}

void kernel_main(uint32_t mboot_magic, multiboot2_info_t *mboot_info) {
    debug_init();
    terminal_init();
    terminal_print("opencodeOS\n");

    isr_init();
    idt_install();
    keyboard_init();
    pit_init();
    register_interrupt_handler(128, syscall_handler);

    if (mboot_magic == MULTIBOOT2_MAGIC) {
        pmm_init(mboot_info);
        vmm_init();
        vmm_init_heap();
        fs_init(mboot_info);

        pmm_refcount_init();

        process_init();
        scheduler_init();

        file_t *user_elf = fs_open("user_code.elf", EXT2_ROOT_INO);
        if (user_elf) {
            debug_printf("kernel: loading user_code.elf (%u bytes)\r\n", user_elf->size);
            for (int i = 0; i < 1; i++) {
                process_t *proc = process_create_elf(user_elf);
                if (proc)
                    scheduler_add_process(proc);
            }
        } else {
            debug_printf("kernel: user_code.elf not found\r\n");
        }

        pci_scan();
        ata_init();

        ext2_fs_t ext2;
        if (ext2_init(&ext2) == 0)
            fs_set_ext2(&ext2);
        } else {
            debug_printf("kernel: not booted with Multiboot2\r\n");
        }

    __asm__ __volatile__("sti");

    for (;;)
        __asm__ __volatile__("hlt");
}
