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
#include "rtc.h"
#include "signal.h"


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
    "fork", "execve", "waitpid", "chdir", "getcwd", "listdir", "kill", "dup2",
    "pipe", "ioctl", "gettime", "sigaction", "sigreturn",
    "kmalloc_test"
};

extern uint32_t scheduler_syscall_no;

static int user_range_ok(const void *addr, uint32_t size, int writable) {
    if (size == 0) return 1;
    uint32_t start = (uint32_t)addr;
    if (start + size < start) return 0;
    if (start + size > 0xC0000000) return 0;
    page_directory_t *dir = vmm_get_current_directory();
    uint32_t end_page = (start + size + PAGE_SIZE - 1) & ~0xFFF;
    for (uint32_t page = start & ~0xFFF; page < end_page; page += PAGE_SIZE) {
        int flags = vmm_get_page_flags(dir, page);
        if (!(flags & VMM_PRESENT)) return 0;
        if (writable && !(flags & VMM_WRITABLE) && !(flags & VMM_COW)) return 0;
    }
    return 1;
}

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
        char kpath[256];
        char *kargs = NULL;
        int ok = 0;
        if (cur && strncpy_from_user(kpath, (const char *)r->ebx, 256) > 0) {
            if (r->ecx) {
                kargs = (char *)kmalloc(4096);
                if (kargs && strncpy_from_user(kargs, (const char *)r->ecx, 4096) > 0)
                    ok = 1;
            } else {
                ok = 1;
            }
        }
        if (ok && process_exec(cur, kpath, (const char *)kargs, r, cur->cwd_inode) == 0)
            r->eax = 0;
        else
            r->eax = -1;
        if (kargs) kfree(kargs);
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
        if (user_range_ok((void *)r->ecx, r->edx, 0))
            r->eax = fd_write(cur->fd_table, (int)r->ebx, (void *)r->ecx, r->edx);
        else
            r->eax = -1;
        break;
    case SYSCALL_READ:
        if (user_range_ok((void *)r->ecx, r->edx, 1))
            r->eax = fd_read(cur->fd_table, (int)r->ebx, (void *)r->ecx, r->edx);
        else
            r->eax = -1;
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
    case SYSCALL_OPEN: {
        char kpath[256];
        if (strncpy_from_user(kpath, (const char *)r->ebx, 256) > 0)
            r->eax = fd_open(cur->fd_table, kpath, r->ecx, cur->cwd_inode);
        else
            r->eax = -1;
        break;
    }
    case SYSCALL_CLOSE:
        r->eax = fd_close(cur->fd_table, (int)r->ebx);
        break;
    case SYSCALL_LSEEK:
        r->eax = fd_lseek(cur->fd_table, (int)r->ebx, (int32_t)r->ecx, (int)r->edx);
        break;
    case SYSCALL_FSTAT: {
        stat_t kst;
        if (fd_fstat(cur->fd_table, (int)r->ebx, &kst) == 0 &&
            copy_to_user((void *)r->ecx, &kst, sizeof(stat_t)) == 0)
            r->eax = 0;
        else
            r->eax = -1;
        break;
    }
    case SYSCALL_BRK: {
        uint32_t new_brk = r->ebx;
        if (new_brk == 0) {
            r->eax = cur->heap_break;
            break;
        }
        if (new_brk >= USER_HEAP_START && new_brk < 0xC0000000) {
            uint32_t new_mapped = (new_brk + PAGE_SIZE - 1) & ~0xFFF;
            uint32_t old_mapped = cur->heap_mapped_end;
            if (new_brk > cur->heap_break) {
                for (uint32_t a = old_mapped; a < new_mapped; a += PAGE_SIZE) {
                    uint32_t phys = (uint32_t)pmm_alloc_page();
                    if (!phys) { r->eax = -1; break; }
                    vmm_map_page(cur->page_dir, phys, a,
                                VMM_PRESENT | VMM_WRITABLE | VMM_USER);
                }
                cur->heap_mapped_end = new_mapped > old_mapped ? new_mapped : old_mapped;
            } else if (new_brk < cur->heap_break && new_mapped < old_mapped) {
                for (uint32_t a = new_mapped; a < old_mapped; a += PAGE_SIZE) {
                    uint32_t phys = vmm_get_physical(cur->page_dir, a);
                    if (phys) {
                        pmm_refcount_dec(phys & ~0xFFF);
                        vmm_unmap_page(cur->page_dir, a);
                    }
                }
                cur->heap_mapped_end = new_mapped;
            }
            cur->heap_break = new_brk;
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
            if (new_brk > old) {
                for (uint32_t a = old_mapped; a < new_mapped; a += PAGE_SIZE) {
                    uint32_t phys = (uint32_t)pmm_alloc_page();
                    if (!phys) { r->eax = -1; break; }
                    vmm_map_page(cur->page_dir, phys, a,
                                VMM_PRESENT | VMM_WRITABLE | VMM_USER);
                }
                cur->heap_mapped_end = new_mapped > old_mapped ? new_mapped : old_mapped;
            } else if (new_brk < old && new_mapped < old_mapped) {
                for (uint32_t a = new_mapped; a < old_mapped; a += PAGE_SIZE) {
                    uint32_t phys = vmm_get_physical(cur->page_dir, a);
                    if (phys) {
                        pmm_refcount_dec(phys & ~0xFFF);
                        vmm_unmap_page(cur->page_dir, a);
                    }
                }
                cur->heap_mapped_end = new_mapped;
            }
            cur->heap_break = new_brk;
        }
        r->eax = old;
        break;
    }
    case SYSCALL_WAITPID: {
        int pid = (int)r->ebx;
        uint32_t *status = (uint32_t *)r->ecx;
        int options = (int)r->edx;
        uint32_t kstatus;
        uint32_t *status_ptr = status ? &kstatus : NULL;
        int result = process_waitpid(cur, pid, status_ptr, options);
        if (result == -2) {
            cur->kernel_esp = (uint32_t)r;
            cur->state = PROC_BLOCKED;
            r->eax = 0;
        } else if (result >= 0) {
            if (status_ptr && copy_to_user(status, &kstatus, sizeof(uint32_t)) < 0)
                r->eax = -1;
            else
                r->eax = (uint32_t)result;
        } else {
            r->eax = -1;
        }
        break;
    }
    case SYSCALL_CHDIR: {
        char kpath[256];
        if (!cur || strncpy_from_user(kpath, (const char *)r->ebx, 256) <= 0)
            { r->eax = -1; break; }
        ext2_fs_t *fs = fs_get_ext2();
        if (!fs || !fs->present) { r->eax = -1; break; }
        uint32_t ino;
        uint8_t type;
        if (ext2_resolve(fs, cur->cwd_inode, kpath, &ino, &type) == 0 && type == EXT2_FT_DIR) {
            cur->cwd_inode = ino;
            r->eax = 0;
        } else {
            r->eax = -1;
        }
        break;
    }
    case SYSCALL_GETCWD: {
        uint32_t size = r->ecx;
        if (!cur || size == 0) { r->eax = -1; break; }
        char *kbuf = (char *)kmalloc(size);
        if (!kbuf) { r->eax = -1; break; }
        ext2_fs_t *fs = fs_get_ext2();
        if (!fs || !fs->present) { kfree(kbuf); r->eax = -1; break; }

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
        if (err) { kfree(kbuf); r->eax = -1; break; }

        int pos = 0;
        if (n == 0) {
            if (pos < (int)size) kbuf[pos++] = '/';
        } else {
            kbuf[pos++] = '/';
            for (int i = n - 1; i >= 0; i--) {
                uint32_t parent_ino = (i < n - 1) ? ino_chain[i + 1] : EXT2_ROOT_INO;
                char name[EXT2_NAME_MAX + 1];
                if (ext2_find_name(fs, parent_ino, ino_chain[i], name) < 0) {
                    char tmp[16];
                    char *tp = tmp + 15;
                    *tp = '\0';
                    uint32_t nn = ino_chain[i];
                    do { *--tp = '0' + (nn % 10); nn /= 10; } while (nn);
                    char *np = name;
                    *np++ = 'i'; *np++ = 'n'; *np++ = 'o'; *np++ = 'd';
                    *np++ = 'e'; *np++ = '_';
                    while (*tp && (uint32_t)(np - name) < EXT2_NAME_MAX)
                        *np++ = *tp++;
                    *np = '\0';
                }
                for (int k = 0; name[k] && pos < (int)size - 1; k++)
                    kbuf[pos++] = name[k];
                if (i > 0 && pos < (int)size - 1)
                    kbuf[pos++] = '/';
            }
        }
        if (err) { kfree(kbuf); r->eax = -1; break; }
        kbuf[pos] = '\0';
        if (copy_to_user((void *)r->ebx, kbuf, pos + 1) == 0)
            r->eax = pos;
        else
            r->eax = -1;
        kfree(kbuf);
        break;
    }
    case SYSCALL_LISTDIR: {
        char kpath[256];
        char *kpath_ptr = NULL;
        uint32_t size = r->edx;
        if (!cur || !r->ecx) { r->eax = -1; break; }
        ext2_fs_t *fs = fs_get_ext2();
        if (!fs || !fs->present) { r->eax = -1; break; }

        if (r->ebx) {
            if (strncpy_from_user(kpath, (const char *)r->ebx, 256) <= 0)
                { r->eax = -1; break; }
            kpath_ptr = kpath;
        }

        uint32_t dir_ino;
        uint8_t type;
        if (!kpath_ptr) {
            dir_ino = cur->cwd_inode;
        } else {
            if (ext2_resolve(fs, cur->cwd_inode, kpath_ptr, &dir_ino, &type) < 0 ||
                type != EXT2_FT_DIR) {
                r->eax = -1; break;
            }
        }

        char *kbuf = (char *)kmalloc(size);
        if (!kbuf) { r->eax = -1; break; }
        uint32_t bytes = 0;
        int count = ext2_read_names(fs, dir_ino, kbuf, size, &bytes);
        if (count < 0) { kfree(kbuf); r->eax = -1; break; }
        if (copy_to_user((void *)r->ecx, kbuf, bytes) == 0)
            r->eax = (int)bytes;
        else
            r->eax = -1;
        kfree(kbuf);
        break;
    }
    case SYSCALL_KILL: {
        int pid = (int)r->ebx;
        int sig = (int)r->ecx;
        if (sig == 0) sig = SIGTERM;
        if (cur && pid == (int)cur->pid && (sig == SIGKILL || sig == SIGTERM)) {
            cur->exit_code = 128 + sig;
            cur->kernel_esp = (uint32_t)r;
            process_exit(cur);
            scheduler_syscall_no = 3;
        } else {
            r->eax = sys_kill_sig(pid, sig);
        }
        break;
    }
    case SYSCALL_SIGACTION: {
        int signum = (int)r->ebx;
        const sigaction_t *act = (const sigaction_t *)r->ecx;
        sigaction_t *oldact = (sigaction_t *)r->edx;
        r->eax = sys_sigaction(signum, act, oldact);
        break;
    }
    case SYSCALL_SIGRETURN: {
        r->eax = sys_sigreturn(r);
        break;
    }
    case SYSCALL_DUP2: {
        int oldfd = (int)r->ebx;
        int newfd = (int)r->ecx;
        r->eax = fd_dup2(cur->fd_table, oldfd, newfd);
        break;
    }
    case SYSCALL_PIPE: {
        if (!r->ebx) { r->eax = -1; break; }
        int fds[2];
        if (fd_pipe(cur->fd_table, fds) == 0) {
            if (copy_to_user((void *)r->ebx, fds, sizeof(fds)) == 0)
                r->eax = 0;
            else
                r->eax = -1;
        } else {
            r->eax = -1;
        }
        break;
    }
    case SYSCALL_IOCTL: {
        int fd = (int)r->ebx;
        int request = (int)r->ecx;
        void *arg = (void *)r->edx;
        r->eax = fd_ioctl(cur->fd_table, fd, request, arg);
        break;
    }
    case SYSCALL_GETTIME: {
        if (!r->ebx) { r->eax = -1; break; }
        rtc_time_t ktime;
        rtc_read_time(&ktime);
        if (copy_to_user((void *)r->ebx, &ktime, sizeof(rtc_time_t)) == 0)
            r->eax = 0;
        else
            r->eax = -1;
        break;
    }
    case SYSCALL_KMALLOC_TEST: {
        int mode = (int)r->ebx;
        if (mode == 0) {
            uint32_t size = r->ecx;
            uint8_t *p = (uint8_t *)kmalloc(size);
            if (!p) { r->eax = -1; break; }
            for (uint32_t i = 0; i < size; i++)
                p[i] = (uint8_t)(i & 0xFF);
            uint32_t ok = 1;
            for (uint32_t i = 0; i < size; i++) {
                if (p[i] != (uint8_t)(i & 0xFF)) { ok = 0; break; }
            }
            kfree(p);
            r->eax = ok ? 0 : -1;
        } else if (mode == 1) {
            uint32_t total = 0;
            uint32_t max_avail = 0;
            for (uint32_t sz = 16; sz <= 0x40000000; sz *= 2) {
                void *p = kmalloc(sz);
                if (!p) {
                    max_avail = total;
                    break;
                }
                total += sz;
                kfree(p);
            }
            if (max_avail == 0) max_avail = total;
            r->eax = (int)max_avail;
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

        rtc_init();
        pci_scan();
        ata_init();


        ext2_fs_t ext2;
        if (ext2_init(&ext2) < 0)
            panic_simple("ext2 init failed");
        fs_set_ext2(&ext2);
        debug_printf("kernel: ext2 mounted\r\n");

        file_t *init_elf = fs_open("/bin/init", EXT2_ROOT_INO);
        if (!init_elf)
            panic_simple("/bin/init not found");

        process_t *proc = process_create_elf(init_elf);
        if (!proc)
            panic_simple("process_create_elf failed");
        scheduler_add_process(proc);
    } else {
        debug_printf("kernel: not booted with Multiboot2\r\n");
    }

    __asm__ __volatile__("sti");

    for (;;)
        __asm__ __volatile__("hlt");
}
