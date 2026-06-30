#include <stddef.h>
#include <stdint.h>
#include "isr.h"
#include "process.h"
#include "scheduler.h"
#include "vmm.h"
#include "terminal.h"
#include "debug.h"
#include "pit.h"
#include "fs.h"
#include "fd.h"
#include "ext2.h"
#include "signal.h"
#include "rtc.h"
#include "pmm.h"
#include "memory.h"
#include "syscalls.h"
#include "procfs.h"

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

void syscall_handler(registers_t *r) {
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
                    if (vmm_map_page(cur->page_dir, phys, a,
                                VMM_PRESENT | VMM_WRITABLE | VMM_USER) < 0) {
                        r->eax = -1; break;
                    }
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
                    if (vmm_map_page(cur->page_dir, phys, a,
                                VMM_PRESENT | VMM_WRITABLE | VMM_USER) < 0) {
                        r->eax = -1; break;
                    }
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

        if (r->ebx) {
            if (strncpy_from_user(kpath, (const char *)r->ebx, 256) <= 0)
                { r->eax = -1; break; }
            kpath_ptr = kpath;
        }

        if (kpath_ptr && procfs_is_proc_path(kpath_ptr)) {
            char *kbuf = (char *)kmalloc(size);
            if (!kbuf) { r->eax = -1; break; }
            uint32_t bytes = 0;
            if (procfs_readdir(kpath_ptr, kbuf, size, &bytes) < 0) {
                kfree(kbuf); r->eax = -1; break;
            }
            if (copy_to_user((void *)r->ecx, kbuf, bytes) == 0)
                r->eax = (int)bytes;
            else
                r->eax = -1;
            kfree(kbuf);
            break;
        }

        ext2_fs_t *fs = fs_get_ext2();
        if (!fs || !fs->present) { r->eax = -1; break; }

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

        if (dir_ino == EXT2_ROOT_INO) {
            const char *virt[] = {"proc", NULL};
            for (int i = 0; virt[i] && bytes < size; i++) {
                const char *s = virt[i];
                while (*s && bytes < size) kbuf[bytes++] = *s++;
                if (bytes < size) kbuf[bytes++] = '\n';
            }
        }

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
    case SYSCALL_UNAME: {
        if (!r->ebx) { r->eax = -1; break; }
        utsname_t uts;
        int i;
        const char *s = "opencodeOS";
        for (i = 0; i < UTSNAME_LEN - 1 && s[i]; i++)
            uts.sysname[i] = s[i];
        uts.sysname[i] = '\0';

        s = "opencode";
        for (i = 0; i < UTSNAME_LEN - 1 && s[i]; i++)
            uts.nodename[i] = s[i];
        uts.nodename[i] = '\0';

        s = "0.1.0";
        for (i = 0; i < UTSNAME_LEN - 1 && s[i]; i++)
            uts.release[i] = s[i];
        uts.release[i] = '\0';

        s = "#1 Tue Jun 30 2026";
        for (i = 0; i < UTSNAME_LEN - 1 && s[i]; i++)
            uts.version[i] = s[i];
        uts.version[i] = '\0';

        s = "i386";
        for (i = 0; i < UTSNAME_LEN - 1 && s[i]; i++)
            uts.machine[i] = s[i];
        uts.machine[i] = '\0';

        if (copy_to_user((void *)r->ebx, &uts, sizeof(uts)) == 0)
            r->eax = 0;
        else
            r->eax = -1;
        break;
    }
    case SYSCALL_MMAP: {
        if (!cur) { r->eax = (uint32_t)MAP_FAILED; break; }
        uint32_t addr = (uint32_t)r->ebx;
        uint32_t length = r->ecx;
        int prot = r->edx;
        int flags = r->esi;

        if (length == 0) { r->eax = (uint32_t)MAP_FAILED; break; }

        uint32_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
        uint32_t virt;

        if (addr == 0) {
            virt = cur->mmap_brk;
            if (virt == 0) virt = USER_MMAP_START;
        } else {
            virt = addr & ~0xFFF;
        }

        uint32_t page_flags = VMM_PRESENT | VMM_USER;
        if (prot & PROT_WRITE) page_flags |= VMM_WRITABLE;

        if (flags & MAP_ANONYMOUS) {
            for (uint32_t i = 0; i < pages; i++) {
                uint32_t vaddr = virt + i * PAGE_SIZE;
                if (vaddr >= 0xC0000000) {
                    r->eax = (uint32_t)MAP_FAILED;
                    break;
                }
                uint32_t phys = (uint32_t)pmm_alloc_page();
                if (!phys) { r->eax = (uint32_t)MAP_FAILED; break; }
                if (vmm_map_page(cur->page_dir, phys, vaddr, page_flags) < 0) {
                    pmm_free_page((void *)phys);
                    r->eax = (uint32_t)MAP_FAILED;
                    break;
                }
            }
        } else {
            r->eax = (uint32_t)MAP_FAILED;
            break;
        }

        if (r->eax != (uint32_t)MAP_FAILED) {
            cur->mmap_brk = virt + pages * PAGE_SIZE;
            r->eax = virt;
        }
        break;
    }
    case SYSCALL_MUNMAP: {
        if (!cur) { r->eax = -1; break; }
        uint32_t addr = (uint32_t)r->ebx;
        uint32_t length = r->ecx;

        if (length == 0) { r->eax = -1; break; }

        uint32_t start = addr & ~0xFFF;
        uint32_t end = ((addr + length + PAGE_SIZE - 1) & ~0xFFF);
        uint32_t ok = 1;

        for (uint32_t vaddr = start; vaddr < end; vaddr += PAGE_SIZE) {
            if (!vmm_is_page_present(cur->page_dir, vaddr)) {
                ok = 0;
                continue;
            }
            uint32_t phys = vmm_get_physical(cur->page_dir, vaddr);
            if (phys) {
                pmm_refcount_dec(phys & ~0xFFF);
            }
            vmm_unmap_page(cur->page_dir, vaddr);
        }

        r->eax = ok ? 0 : -1;
        break;
    }
    case SYSCALL_STAT:
    case SYSCALL_LSTAT: {
        char kpath[256];
        if (!cur || strncpy_from_user(kpath, (const char *)r->ebx, 256) <= 0)
            { r->eax = -1; break; }

        if (procfs_is_proc_path(kpath)) {
            stat_t kst;
            kst.st_dev = 0;
            kst.st_ino = 1;
            kst.st_mode = 0444;
            kst.st_nlink = 1;
            kst.st_uid = 0;
            kst.st_gid = 0;
            kst.st_rdev = 0;
            kst.st_size = 0;
            kst.st_atime = 0;
            kst.st_mtime = 0;
            kst.st_ctime = 0;
            kst.st_blksize = 1024;
            kst.st_blocks = 0;
            if (copy_to_user((void *)r->ecx, &kst, sizeof(stat_t)) == 0)
                r->eax = 0;
            else
                r->eax = -1;
            break;
        }

        ext2_fs_t *fs = fs_get_ext2();
        if (!fs || !fs->present) { r->eax = -1; break; }
        uint32_t ino;
        uint8_t type;
        if (ext2_resolve(fs, cur->cwd_inode, kpath, &ino, &type) < 0)
            { r->eax = -1; break; }
        stat_t kst;
        if (ext2_stat(fs, ino, &kst) < 0)
            { r->eax = -1; break; }
        if (copy_to_user((void *)r->ecx, &kst, sizeof(stat_t)) == 0)
            r->eax = 0;
        else
            r->eax = -1;
        break;
    }
    case SYSCALL_GETDENTS: {
        char kpath[256];
        char *kpath_ptr = NULL;
        uint32_t count = r->edx;
        if (!cur || !r->ecx) { r->eax = -1; break; }

        if (r->ebx) {
            if (strncpy_from_user(kpath, (const char *)r->ebx, 256) <= 0)
                { r->eax = -1; break; }
            kpath_ptr = kpath;
        }

        if (kpath_ptr && procfs_is_proc_path(kpath_ptr)) {
            uint32_t bufsz = count * sizeof(dirent_t);
            dirent_t *kbuf = (dirent_t *)kmalloc(bufsz);
            if (!kbuf) { r->eax = -1; break; }
            int n = procfs_getdents(kpath_ptr, kbuf, count);
            if (n < 0) { kfree(kbuf); r->eax = -1; break; }
            if (copy_to_user((void *)r->ecx, kbuf, n * sizeof(dirent_t)) == 0)
                r->eax = n;
            else
                r->eax = -1;
            kfree(kbuf);
            break;
        }

        ext2_fs_t *fs = fs_get_ext2();
        if (!fs || !fs->present) { r->eax = -1; break; }

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

        uint32_t bufsz = count * sizeof(dirent_t);
        dirent_t *kbuf = (dirent_t *)kmalloc(bufsz);
        if (!kbuf) { r->eax = -1; break; }
        int n = ext2_getdents(fs, dir_ino, kbuf, count);
        if (n < 0) { kfree(kbuf); r->eax = -1; break; }

        if (dir_ino == EXT2_ROOT_INO && n < (int)count) {
            dirent_t *d = &kbuf[n];
            d->d_ino = 1;
            d->d_off = n;
            d->d_type = 2;
            d->d_reclen = sizeof(dirent_t);
            int j = 0;
            const char *proc = "proc";
            while (proc[j] && j < 255) { d->d_name[j] = proc[j]; j++; }
            d->d_name[j] = '\0';
            n++;
        }

        if (copy_to_user((void *)r->ecx, kbuf, n * sizeof(dirent_t)) == 0)
            r->eax = n;
        else
            r->eax = -1;
        kfree(kbuf);
        break;
    }
    case SYSCALL_UNLINK: {
        char kpath[256];
        if (!cur || strncpy_from_user(kpath, (const char *)r->ebx, 256) <= 0)
            { r->eax = -1; break; }
        ext2_fs_t *fs = fs_get_ext2();
        if (!fs || !fs->present) { r->eax = -1; break; }
        uint32_t dir_ino;
        uint8_t type;
        const char *base_name = kpath;
        const char *slash = kpath;
        while (*slash) slash++;
        while (slash > kpath && *slash != '/') slash--;
        if (slash == kpath) {
            dir_ino = cur->cwd_inode;
            base_name = kpath;
            if (*base_name == '/') { dir_ino = EXT2_ROOT_INO; base_name++; }
        } else {
            char dir_path[256];
            int len = slash - kpath;
            for (int i = 0; i < len && i < 255; i++) dir_path[i] = kpath[i];
            dir_path[len] = '\0';
            if (ext2_resolve(fs, cur->cwd_inode, dir_path, &dir_ino, &type) < 0 || type != EXT2_FT_DIR)
                { r->eax = -1; break; }
            base_name = slash + 1;
        }
        if (!*base_name) { r->eax = -1; break; }
        r->eax = ext2_unlink(fs, dir_ino, base_name) == 0 ? 0 : -1;
        break;
    }
    case SYSCALL_RENAME: {
        char kold[256], knew[256];
        if (!cur || strncpy_from_user(kold, (const char *)r->ebx, 256) <= 0 ||
            strncpy_from_user(knew, (const char *)r->ecx, 256) <= 0)
            { r->eax = -1; break; }
        ext2_fs_t *fs = fs_get_ext2();
        if (!fs || !fs->present) { r->eax = -1; break; }

        uint32_t old_dir_ino, new_dir_ino;
        uint8_t type;
        const char *old_base, *new_base;
        const char *slash;

        slash = kold;
        while (*slash) slash++;
        while (slash > kold && *slash != '/') slash--;
        if (slash == kold) {
            old_dir_ino = cur->cwd_inode;
            old_base = kold;
            if (*old_base == '/') { old_dir_ino = EXT2_ROOT_INO; old_base++; }
        } else {
            char dir_path[256];
            int len = slash - kold;
            for (int i = 0; i < len && i < 255; i++) dir_path[i] = kold[i];
            dir_path[len] = '\0';
            if (ext2_resolve(fs, cur->cwd_inode, dir_path, &old_dir_ino, &type) < 0 || type != EXT2_FT_DIR)
                { r->eax = -1; break; }
            old_base = slash + 1;
        }

        slash = knew;
        while (*slash) slash++;
        while (slash > knew && *slash != '/') slash--;
        if (slash == knew) {
            new_dir_ino = cur->cwd_inode;
            new_base = knew;
            if (*new_base == '/') { new_dir_ino = EXT2_ROOT_INO; new_base++; }
        } else {
            char dir_path[256];
            int len = slash - knew;
            for (int i = 0; i < len && i < 255; i++) dir_path[i] = knew[i];
            dir_path[len] = '\0';
            if (ext2_resolve(fs, cur->cwd_inode, dir_path, &new_dir_ino, &type) < 0 || type != EXT2_FT_DIR)
                { r->eax = -1; break; }
            new_base = slash + 1;
        }

        if (!*old_base || !*new_base) { r->eax = -1; break; }
        r->eax = ext2_rename(fs, old_dir_ino, old_base, new_dir_ino, new_base) == 0 ? 0 : -1;
        break;
    }
    case SYSCALL_MKDIR: {
        char kpath[256];
        if (!cur || strncpy_from_user(kpath, (const char *)r->ebx, 256) <= 0)
            { r->eax = -1; break; }
        ext2_fs_t *fs = fs_get_ext2();
        if (!fs || !fs->present) { r->eax = -1; break; }
        uint32_t dir_ino;
        uint8_t type;
        const char *base_name = kpath;
        const char *slash = kpath;
        while (*slash) slash++;
        while (slash > kpath && *slash != '/') slash--;
        if (slash == kpath) {
            dir_ino = cur->cwd_inode;
            base_name = kpath;
            if (*base_name == '/') { dir_ino = EXT2_ROOT_INO; base_name++; }
        } else {
            char dir_path[256];
            int len = slash - kpath;
            for (int i = 0; i < len && i < 255; i++) dir_path[i] = kpath[i];
            dir_path[len] = '\0';
            if (ext2_resolve(fs, cur->cwd_inode, dir_path, &dir_ino, &type) < 0 || type != EXT2_FT_DIR)
                { r->eax = -1; break; }
            base_name = slash + 1;
        }
        if (!*base_name) { r->eax = -1; break; }
        uint32_t new_ino;
        r->eax = ext2_mkdir(fs, base_name, dir_ino, &new_ino) == 0 ? 0 : -1;
        break;
    }
    case SYSCALL_RMDIR: {
        char kpath[256];
        if (!cur || strncpy_from_user(kpath, (const char *)r->ebx, 256) <= 0)
            { r->eax = -1; break; }
        ext2_fs_t *fs = fs_get_ext2();
        if (!fs || !fs->present) { r->eax = -1; break; }
        uint32_t dir_ino;
        uint8_t type;
        const char *base_name = kpath;
        const char *slash = kpath;
        while (*slash) slash++;
        while (slash > kpath && *slash != '/') slash--;
        if (slash == kpath) {
            dir_ino = cur->cwd_inode;
            base_name = kpath;
            if (*base_name == '/') { dir_ino = EXT2_ROOT_INO; base_name++; }
        } else {
            char dir_path[256];
            int len = slash - kpath;
            for (int i = 0; i < len && i < 255; i++) dir_path[i] = kpath[i];
            dir_path[len] = '\0';
            if (ext2_resolve(fs, cur->cwd_inode, dir_path, &dir_ino, &type) < 0 || type != EXT2_FT_DIR)
                { r->eax = -1; break; }
            base_name = slash + 1;
        }
        if (!*base_name) { r->eax = -1; break; }
        r->eax = ext2_rmdir(fs, dir_ino, base_name) == 0 ? 0 : -1;
        break;
    }
    case SYSCALL_CHMOD: {
        char kpath[256];
        if (!cur || strncpy_from_user(kpath, (const char *)r->ebx, 256) <= 0)
            { r->eax = -1; break; }
        ext2_fs_t *fs = fs_get_ext2();
        if (!fs || !fs->present) { r->eax = -1; break; }
        uint32_t ino;
        uint8_t type;
        if (ext2_resolve(fs, cur->cwd_inode, kpath, &ino, &type) < 0)
            { r->eax = -1; break; }
        ext2_inode_t inode;
        if (ext2_read_inode(fs, ino, &inode) < 0)
            { r->eax = -1; break; }
        inode.mode = (inode.mode & ~0xFFF) | ((uint16_t)(r->ecx) & 0xFFF);
        inode.ctime = 0;
        r->eax = ext2_write_inode(fs, ino, &inode) == 0 ? 0 : -1;
        break;
    }
    case SYSCALL_CHOWN: {
        char kpath[256];
        if (!cur || strncpy_from_user(kpath, (const char *)r->ebx, 256) <= 0)
            { r->eax = -1; break; }
        ext2_fs_t *fs = fs_get_ext2();
        if (!fs || !fs->present) { r->eax = -1; break; }
        uint32_t ino;
        uint8_t type;
        if (ext2_resolve(fs, cur->cwd_inode, kpath, &ino, &type) < 0)
            { r->eax = -1; break; }
        ext2_inode_t inode;
        if (ext2_read_inode(fs, ino, &inode) < 0)
            { r->eax = -1; break; }
        inode.uid = (uint16_t)r->ecx;
        inode.gid = (uint16_t)r->edx;
        inode.ctime = 0;
        r->eax = ext2_write_inode(fs, ino, &inode) == 0 ? 0 : -1;
        break;
    }
    case SYSCALL_ACCESS: {
        char kpath[256];
        if (!cur || strncpy_from_user(kpath, (const char *)r->ebx, 256) <= 0)
            { r->eax = -1; break; }
        ext2_fs_t *fs = fs_get_ext2();
        if (!fs || !fs->present) {
            if (procfs_is_proc_path(kpath)) { r->eax = 0; break; }
            r->eax = -1; break;
        }
        uint32_t ino;
        uint8_t type;
        if (ext2_resolve(fs, cur->cwd_inode, kpath, &ino, &type) < 0) {
            if (procfs_is_proc_path(kpath)) { r->eax = 0; break; }
            r->eax = -1; break;
        }
        r->eax = 0;
        break;
    }
    case SYSCALL_GETUID:
        r->eax = cur ? cur->uid : 0;
        break;
    case SYSCALL_GETGID:
        r->eax = cur ? cur->gid : 0;
        break;
    case SYSCALL_GETEUID:
        r->eax = cur ? cur->euid : 0;
        break;
    case SYSCALL_GETEGID:
        r->eax = cur ? cur->egid : 0;
        break;
    }
}
