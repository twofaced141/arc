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
#include "block.h"
#include "signal.h"
#include "rtc.h"
#include "pmm.h"
#include "memory.h"
#include "syscalls.h"
#include "procfs.h"
#include "devfs.h"
#include <string.h>
#include "idt.h"
#include "acpi.h"
#include "framebuffer.h"
#include "mouse.h"
#include "oom.h"

static char kernel_hostname[64] = "opencode";

/* For absolute paths, resolve mount point and return EXT2_ROOT_INO as cwd.
   For relative paths, use proc->cwd_inode and root fs. */
static ext2_fs_t *sys_fs(process_t *proc, char *path, uint32_t *cwd) {
    if (path && path[0] == '/') {
        ext2_fs_t *fs = fs_for_path(path);
        if (cwd) *cwd = EXT2_ROOT_INO;
        return fs;
    }
    if (cwd) *cwd = proc->cwd_inode;
    return fs_get_ext2();
}

static uint32_t find_pid(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state != PROC_UNUSED && processes[i].state != PROC_ZOMBIE &&
            (int)processes[i].pid == pid)
            return (uint32_t)i;
    }
    return 0xFFFFFFFF;
}

void sys_sethostname(const char *name) {
    int i;
    for (i = 0; i < 63 && name[i]; i++)
        kernel_hostname[i] = name[i];
    kernel_hostname[i] = '\0';
}

void sys_gethostname(char *buf, uint32_t size) {
    int i;
    for (i = 0; i < (int)size - 1 && kernel_hostname[i]; i++)
        buf[i] = kernel_hostname[i];
    buf[i] = '\0';
}

int sys_setpgid(int pid, int pgid) {
    uint32_t flags;
    spin_lock_irqsave(&proc_lock, &flags);
    uint32_t idx = find_pid(pid);
    if (idx == 0xFFFFFFFF) { spin_unlock_irqrestore(&proc_lock, flags); return -1; }
    processes[idx].pgid = (uint32_t)pgid;
    spin_unlock_irqrestore(&proc_lock, flags);
    return 0;
}

int sys_getpgid(int pid) {
    uint32_t flags;
    spin_lock_irqsave(&proc_lock, &flags);
    uint32_t idx = find_pid(pid);
    if (idx == 0xFFFFFFFF) { spin_unlock_irqrestore(&proc_lock, flags); return -1; }
    uint32_t pgid = processes[idx].pgid;
    spin_unlock_irqrestore(&proc_lock, flags);
    return (int)pgid;
}

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
        if (!(flags & VMM_USER)) return 0;
        if (writable && !(flags & VMM_WRITABLE) && !(flags & VMM_COW)) return 0;
    }
    return 1;
}

void syscall_handler(registers_t *r) {
    process_t *cur = scheduler_current_process();

    /* Linux i386 syscall translation for GLIBC-compiled binaries (entry != 0x08000000) */
    if (cur && cur->is_linux_syscall) {
        uint32_t lin_eax = r->eax;
        switch (r->eax) {
            case 1:   r->eax = SYSCALL_EXIT;     break;
            case 3:   if (r->ecx == 0 && r->edx == 0 && r->esi == 0) {
                          r->eax = SYSCALL_EXIT;
                      } else {
                          r->eax = SYSCALL_READ;
                      }   break;
            case 4:   r->eax = SYSCALL_WRITE;    break;
            case 5:   r->eax = SYSCALL_OPEN;     break;
            case 6:   r->eax = SYSCALL_CLOSE;    break;
            case 19:  r->eax = SYSCALL_LSEEK;    break;
            case 45:  r->eax = SYSCALL_BRK;      break;
            case 91:  r->eax = SYSCALL_MUNMAP;   break;
            case 107: r->eax = SYSCALL_STAT;     break;
            case 140: r->eax = SYSCALL_LSEEK;    break;
            case 174: r->eax = SYSCALL_SIGACTION; break;
            case 175: r->eax = SYSCALL_SIGPROCMASK; break;
            case 176: r->eax = SYSCALL_SIGPENDING;  break;
            case 177: r->eax = SYSCALL_SIGSUSPEND;  break;
            case 192:
                r->ebp = r->ebp << 12;
                r->eax = SYSCALL_MMAP;
                break;
            case 252: r->eax = SYSCALL_EXIT;     break;
            default:
                break;
        }
    }

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
        if (new_brk >= cur->heap_initial && new_brk < 0xC0000000) {
            uint32_t new_mapped = (new_brk + PAGE_SIZE - 1) & ~0xFFF;
            uint32_t old_mapped = cur->heap_mapped_end;
            if (new_brk > cur->heap_break) {
                for (uint32_t a = old_mapped; a < new_mapped; a += PAGE_SIZE) {
                    uint32_t phys = (uint32_t)pmm_alloc_page();
                    if (!phys) { r->eax = -1; break; }
                    if (vmm_map_page(cur->page_dir, phys, a,
                                VMM_PRESENT | VMM_WRITABLE | VMM_USER) < 0) {
                        pmm_free_page((void *)phys);
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
        if (new_brk >= cur->heap_initial && new_brk < 0xC0000000) {
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
            debug_printf("DBG: waitpid err=%d\r\n", result);
            r->eax = -1;
        }
        break;
    }
    case SYSCALL_CHDIR: {
        char kpath[256];
        if (!cur || strncpy_from_user(kpath, (const char *)r->ebx, 256) <= 0)
            { r->eax = -1; break; }
        uint32_t resolve_cwd;
        ext2_fs_t *fs = sys_fs(cur, kpath, &resolve_cwd);
        if (!fs || !fs->present) { r->eax = -1; break; }
        if (fs != fs_get_ext2()) { r->eax = -1; break; }
        uint32_t ino;
        uint8_t type;
        if (ext2_resolve(fs, resolve_cwd, kpath, &ino, &type) == 0 && type == EXT2_FT_DIR) {
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

        /* Normalize relative "proc"/"dev" paths from root to absolute */
        if (kpath_ptr && kpath_ptr[0] != '/' && cur->cwd_inode == EXT2_ROOT_INO) {
            if (strcmp(kpath_ptr, "proc") == 0 || strncmp(kpath_ptr, "proc/", 5) == 0 ||
                strcmp(kpath_ptr, "dev") == 0 || strncmp(kpath_ptr, "dev/", 4) == 0) {
                char abs_path[256];
                abs_path[0] = '/';
                int ai = 1;
                for (int j = 0; kpath_ptr[j] && ai < 255; j++, ai++)
                    abs_path[ai] = kpath_ptr[j];
                abs_path[ai] = '\0';
                for (int j = 0; j < 256; j++) kpath[j] = abs_path[j];
                kpath_ptr = kpath;
            }
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

        if (kpath_ptr && devfs_is_dev_path(kpath_ptr)) {
            char *kbuf = (char *)kmalloc(size);
            if (!kbuf) { r->eax = -1; break; }
            uint32_t bytes = 0;
            if (devfs_readdir(kpath_ptr, kbuf, size, &bytes) < 0) {
                kfree(kbuf); r->eax = -1; break;
            }
            if (copy_to_user((void *)r->ecx, kbuf, bytes) == 0)
                r->eax = (int)bytes;
            else
                r->eax = -1;
            kfree(kbuf);
            break;
        }

        uint32_t dir_ino;
        uint8_t type;
        ext2_fs_t *list_fs;
        if (!kpath_ptr) {
            list_fs = fs_get_ext2();
            if (!list_fs || !list_fs->present) { r->eax = -1; break; }
            dir_ino = cur->cwd_inode;
        } else {
            uint32_t resolve_cwd;
            list_fs = sys_fs(cur, kpath_ptr, &resolve_cwd);
            if (!list_fs || !list_fs->present) { r->eax = -1; break; }
            if (ext2_resolve(list_fs, resolve_cwd, kpath_ptr, &dir_ino, &type) < 0 ||
                type != EXT2_FT_DIR) {
                r->eax = -1; break;
            }
        }

        char *kbuf = (char *)kmalloc(size);
        if (!kbuf) { r->eax = -1; break; }
        uint32_t bytes = 0;
        int count = ext2_read_names(list_fs, dir_ino, kbuf, size, &bytes);
        if (count < 0) { kfree(kbuf); r->eax = -1; break; }

        if (dir_ino == EXT2_ROOT_INO && list_fs == fs_get_ext2()) {
            const char *virt[] = {"proc", "dev", NULL};
            for (int i = 0; virt[i] && bytes < size; i++) {
                const char *s = virt[i];
                while (*s && bytes < size) kbuf[bytes++] = *s++;
                if (bytes < size) kbuf[bytes++] = '\n';
            }
            /* Mount points (non-root) */
            for (int mi = 0; mi < fs_mount_count(); mi++) {
                const char *mp = fs_mount_point(mi);
                if (!mp || strcmp(mp, "/") == 0) continue;
                const char *leaf = mp;
                while (*leaf == '/') leaf++;
                if (!*leaf) continue;
                const char *s = leaf;
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
    case SYSCALL_SIGPROCMASK: {
        int how = (int)r->ebx;
        const uint32_t *set = (const uint32_t *)r->ecx;
        uint32_t *oldset = (uint32_t *)r->edx;
        r->eax = sys_sigprocmask(how, set, oldset);
        break;
    }
    case SYSCALL_SIGPENDING: {
        uint32_t *set = (uint32_t *)r->ebx;
        r->eax = sys_sigpending(set);
        break;
    }
    case SYSCALL_SIGSUSPEND: {
        const uint32_t *mask = (const uint32_t *)r->ebx;
        cur->kernel_esp = (uint32_t)r;
        r->eax = sys_sigsuspend(r, mask);
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
        int fd = (int)r->edi;
        uint32_t offset = r->ebp;

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

        if (flags & MAP_PHYS) {
            if (cur->uid != 0) { r->eax = (uint32_t)MAP_FAILED; break; }
            uint32_t phys_base = (uint32_t)fd;
            if ((phys_base & 0xFFF) != 0) { debug_printf("mmap: unaligned phys\n"); r->eax = (uint32_t)MAP_FAILED; break; }
            for (uint32_t i = 0; i < pages; i++) {
                uint32_t vaddr = virt + i * PAGE_SIZE;
                if (vaddr >= 0xC0000000) { debug_printf("mmap: vaddr in kernel space 0x%x\n", vaddr); r->eax = (uint32_t)MAP_FAILED; break; }
                uint32_t paddr = phys_base + i * PAGE_SIZE;
                if (vmm_map_page(cur->page_dir, paddr, vaddr, page_flags) < 0) {
                    debug_printf("mmap: vmm_map_page failed for paddr=0x%x vaddr=0x%x\n", paddr, vaddr);
                    r->eax = (uint32_t)MAP_FAILED;
                    break;
                }
            }
        } else if (flags & MAP_ANONYMOUS) {
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
            if (fd < 0 || fd >= FD_MAX) { r->eax = (uint32_t)MAP_FAILED; break; }
            fd_entry_t *ent = &cur->fd_table[fd];
            if (ent->type != FD_FILE) { r->eax = (uint32_t)MAP_FAILED; break; }

            uint32_t saved_pos = ent->pos;
            ent->pos = offset;

            uint8_t *tmp_page = (uint8_t *)kmalloc(PAGE_SIZE);
            if (!tmp_page) { r->eax = (uint32_t)MAP_FAILED; break; }

            for (uint32_t i = 0; i < pages; i++) {
                uint32_t vaddr = virt + i * PAGE_SIZE;
                if (vaddr >= 0xC0000000) {
                    r->eax = (uint32_t)MAP_FAILED;
                    break;
                }

                int bytes = fd_read(cur->fd_table, fd, tmp_page, PAGE_SIZE);
                if (bytes < 0) bytes = 0;

                uint32_t phys = (uint32_t)pmm_alloc_page();
                if (!phys) { r->eax = (uint32_t)MAP_FAILED; break; }

                uint8_t *mapped = (uint8_t *)vmm_temp_map(phys);
                for (int j = 0; j < (int)PAGE_SIZE; j++)
                    mapped[j] = (j < bytes) ? tmp_page[j] : 0;
                vmm_temp_unmap();

                if (vmm_map_page(cur->page_dir, phys, vaddr, page_flags) < 0) {
                    pmm_free_page((void *)phys);
                    r->eax = (uint32_t)MAP_FAILED;
                    break;
                }
            }

            kfree(tmp_page);
            ent->pos = saved_pos;
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

        /* Normalize relative virtual paths from root */
        if (kpath[0] != '/' && cur->cwd_inode == EXT2_ROOT_INO) {
            if (strcmp(kpath, "proc") == 0 || strncmp(kpath, "proc/", 5) == 0 ||
                strcmp(kpath, "dev") == 0 || strncmp(kpath, "dev/", 4) == 0) {
                char abs_path[256];
                abs_path[0] = '/';
                int ai = 1;
                for (int j = 0; kpath[j] && ai < 255; j++, ai++)
                    abs_path[ai] = kpath[j];
                abs_path[ai] = '\0';
                for (int j = 0; j < 256; j++) kpath[j] = abs_path[j];
            }
        }

        if (procfs_is_proc_path(kpath) || devfs_is_dev_path(kpath)) {
            stat_t kst;
            kst.st_dev = 0;
            if (devfs_is_dev_path(kpath)) {
                int is_dir = (kpath[4] == '\0' || (kpath[4] == '/' && kpath[5] == '\0'));
                if (is_dir)
                    kst.st_ino = 2;
                else
                    kst.st_ino = 3;
                kst.st_mode = is_dir ? (0x4000 | 0555) : (0x2000 | 0666);
                kst.st_rdev = 1;
            } else {
                kst.st_ino = 1;
                kst.st_mode = 0444;
                kst.st_rdev = 0;
            }
            kst.st_nlink = 1;
            kst.st_uid = 0;
            kst.st_gid = 0;
            kst.st_rdev = 0;
            kst.st_size = 0;
            kst.st_atim_sec = 0;
            kst.st_atim_nsec = 0;
            kst.st_mtim_sec = 0;
            kst.st_mtim_nsec = 0;
            kst.st_ctim_sec = 0;
            kst.st_ctim_nsec = 0;
            kst.st_blksize = 1024;
            kst.st_blocks = 0;
            if (copy_to_user((void *)r->ecx, &kst, sizeof(stat_t)) == 0)
                r->eax = 0;
            else
                r->eax = -1;
            break;
        }

        uint32_t resolve_cwd;
        ext2_fs_t *fs = sys_fs(cur, kpath, &resolve_cwd);
        if (!fs || !fs->present) { r->eax = -1; break; }
        uint32_t ino;
        uint8_t type;
        int err;
        if (r->eax == SYSCALL_LSTAT)
            err = ext2_resolve_nofollow(fs, resolve_cwd, kpath, &ino, &type);
        else
            err = ext2_resolve(fs, resolve_cwd, kpath, &ino, &type);
        if (err < 0)
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

        if (kpath_ptr && kpath_ptr[0] != '/' && cur->cwd_inode == EXT2_ROOT_INO) {
            if (strcmp(kpath_ptr, "proc") == 0 || strncmp(kpath_ptr, "proc/", 5) == 0 ||
                strcmp(kpath_ptr, "dev") == 0 || strncmp(kpath_ptr, "dev/", 4) == 0) {
                char abs_path[256];
                abs_path[0] = '/';
                int ai = 1;
                for (int j = 0; kpath_ptr[j] && ai < 255; j++, ai++)
                    abs_path[ai] = kpath_ptr[j];
                abs_path[ai] = '\0';
                for (int j = 0; j < 256; j++) kpath[j] = abs_path[j];
                kpath_ptr = kpath;
            }
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

        if (kpath_ptr && devfs_is_dev_path(kpath_ptr)) {
            uint32_t bufsz = count * sizeof(dirent_t);
            dirent_t *kbuf = (dirent_t *)kmalloc(bufsz);
            if (!kbuf) { r->eax = -1; break; }
            int n = devfs_getdents(kpath_ptr, kbuf, count);
            if (n < 0) { kfree(kbuf); r->eax = -1; break; }
            if (copy_to_user((void *)r->ecx, kbuf, n * sizeof(dirent_t)) == 0)
                r->eax = n;
            else
                r->eax = -1;
            kfree(kbuf);
            break;
        }

        uint32_t dir_ino;
        uint8_t type;
        ext2_fs_t *getd_fs;
        if (!kpath_ptr) {
            getd_fs = fs_get_ext2();
            if (!getd_fs || !getd_fs->present) { r->eax = -1; break; }
            dir_ino = cur->cwd_inode;
        } else {
            uint32_t resolve_cwd;
            getd_fs = sys_fs(cur, kpath_ptr, &resolve_cwd);
            if (!getd_fs || !getd_fs->present) { r->eax = -1; break; }
            if (ext2_resolve(getd_fs, resolve_cwd, kpath_ptr, &dir_ino, &type) < 0 ||
                type != EXT2_FT_DIR) {
                r->eax = -1; break;
            }
        }

        uint32_t bufsz = count * sizeof(dirent_t);
        dirent_t *kbuf = (dirent_t *)kmalloc(bufsz);
        if (!kbuf) { r->eax = -1; break; }
        int n = ext2_getdents(getd_fs, dir_ino, kbuf, count);
        if (n < 0) { kfree(kbuf); r->eax = -1; break; }

        int is_root_fs_root = (dir_ino == EXT2_ROOT_INO && getd_fs == fs_get_ext2());
        if (is_root_fs_root) {
            /* Virtual entries: proc, dev */
            if (n + 1 < (int)count) {
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
            if (n + 1 < (int)count) {
                dirent_t *d = &kbuf[n];
                d->d_ino = 2;
                d->d_off = n;
                d->d_type = 2;
                d->d_reclen = sizeof(dirent_t);
                int j = 0;
                const char *dev = "dev";
                while (dev[j] && j < 255) { d->d_name[j] = dev[j]; j++; }
                d->d_name[j] = '\0';
                n++;
            }
            /* Mount points (non-root) */
            for (int mi = 0; mi < fs_mount_count(); mi++) {
                const char *mp = fs_mount_point(mi);
                if (!mp || strcmp(mp, "/") == 0) continue;
                if (n + 1 >= (int)count) break;
                const char *leaf = mp;
                while (*leaf == '/') leaf++;
                if (!*leaf) continue;
                dirent_t *d = &kbuf[n];
                d->d_ino = 3 + mi;
                d->d_off = n;
                d->d_type = 2;
                d->d_reclen = sizeof(dirent_t);
                int j = 0;
                while (leaf[j] && j < 255) { d->d_name[j] = leaf[j]; j++; }
                d->d_name[j] = '\0';
                n++;
            }
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
        uint32_t resolve_cwd;
        ext2_fs_t *fs = sys_fs(cur, kpath, &resolve_cwd);
        if (!fs || !fs->present) { r->eax = -1; break; }
        uint32_t dir_ino;
        uint8_t type;
        const char *base_name = kpath;
        const char *slash = kpath;
        while (*slash) slash++;
        while (slash > kpath && *slash != '/') slash--;
        if (slash == kpath) {
            dir_ino = resolve_cwd;
            base_name = kpath;
            if (*base_name == '/') { dir_ino = EXT2_ROOT_INO; base_name++; }
        } else {
            char dir_path[256];
            int len = slash - kpath;
            for (int i = 0; i < len && i < 255; i++) dir_path[i] = kpath[i];
            dir_path[len] = '\0';
            if (ext2_resolve(fs, resolve_cwd, dir_path, &dir_ino, &type) < 0 || type != EXT2_FT_DIR)
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

        uint32_t old_rcwd;
        ext2_fs_t *fs = sys_fs(cur, kold, &old_rcwd);
        if (!fs || !fs->present) { r->eax = -1; break; }
        uint32_t new_rcwd;
        ext2_fs_t *new_fs = sys_fs(cur, knew, &new_rcwd);
        if (new_fs != fs) { r->eax = -1; break; }

        uint32_t old_dir_ino, new_dir_ino;
        uint8_t type;
        const char *old_base, *new_base;
        const char *slash;

        slash = kold;
        while (*slash) slash++;
        while (slash > kold && *slash != '/') slash--;
        if (slash == kold) {
            old_dir_ino = old_rcwd;
            old_base = kold;
            if (*old_base == '/') { old_dir_ino = EXT2_ROOT_INO; old_base++; }
        } else {
            char dir_path[256];
            int len = slash - kold;
            for (int i = 0; i < len && i < 255; i++) dir_path[i] = kold[i];
            dir_path[len] = '\0';
            if (ext2_resolve(fs, old_rcwd, dir_path, &old_dir_ino, &type) < 0 || type != EXT2_FT_DIR)
                { r->eax = -1; break; }
            old_base = slash + 1;
        }

        slash = knew;
        while (*slash) slash++;
        while (slash > knew && *slash != '/') slash--;
        if (slash == knew) {
            new_dir_ino = new_rcwd;
            new_base = knew;
            if (*new_base == '/') { new_dir_ino = EXT2_ROOT_INO; new_base++; }
        } else {
            char dir_path[256];
            int len = slash - knew;
            for (int i = 0; i < len && i < 255; i++) dir_path[i] = knew[i];
            dir_path[len] = '\0';
            if (ext2_resolve(fs, new_rcwd, dir_path, &new_dir_ino, &type) < 0 || type != EXT2_FT_DIR)
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
        uint32_t resolve_cwd;
        ext2_fs_t *fs = sys_fs(cur, kpath, &resolve_cwd);
        if (!fs || !fs->present) { r->eax = -1; break; }
        uint32_t dir_ino;
        uint8_t type;
        const char *base_name = kpath;
        const char *slash = kpath;
        while (*slash) slash++;
        while (slash > kpath && *slash != '/') slash--;
        if (slash == kpath) {
            dir_ino = resolve_cwd;
            base_name = kpath;
            if (*base_name == '/') { dir_ino = EXT2_ROOT_INO; base_name++; }
        } else {
            char dir_path[256];
            int len = slash - kpath;
            for (int i = 0; i < len && i < 255; i++) dir_path[i] = kpath[i];
            dir_path[len] = '\0';
            if (ext2_resolve(fs, resolve_cwd, dir_path, &dir_ino, &type) < 0 || type != EXT2_FT_DIR)
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
        uint32_t resolve_cwd;
        ext2_fs_t *fs = sys_fs(cur, kpath, &resolve_cwd);
        if (!fs || !fs->present) { r->eax = -1; break; }
        uint32_t dir_ino;
        uint8_t type;
        const char *base_name = kpath;
        const char *slash = kpath;
        while (*slash) slash++;
        while (slash > kpath && *slash != '/') slash--;
        if (slash == kpath) {
            dir_ino = resolve_cwd;
            base_name = kpath;
            if (*base_name == '/') { dir_ino = EXT2_ROOT_INO; base_name++; }
        } else {
            char dir_path[256];
            int len = slash - kpath;
            for (int i = 0; i < len && i < 255; i++) dir_path[i] = kpath[i];
            dir_path[len] = '\0';
            if (ext2_resolve(fs, resolve_cwd, dir_path, &dir_ino, &type) < 0 || type != EXT2_FT_DIR)
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
        uint32_t resolve_cwd;
        ext2_fs_t *fs = sys_fs(cur, kpath, &resolve_cwd);
        if (!fs || !fs->present) { r->eax = -1; break; }
        uint32_t ino;
        uint8_t type;
        if (ext2_resolve(fs, resolve_cwd, kpath, &ino, &type) < 0)
            { r->eax = -1; break; }
        ext2_inode_t inode;
        if (ext2_read_inode(fs, ino, &inode) < 0)
            { r->eax = -1; break; }
        if (cur->euid != inode.uid && cur->uid != 0) { r->eax = -1; break; }
        inode.mode = (inode.mode & ~0xFFF) | ((uint16_t)(r->ecx) & 0xFFF);
        inode.ctime = 0;
        r->eax = ext2_write_inode(fs, ino, &inode) == 0 ? 0 : -1;
        break;
    }
    case SYSCALL_CHOWN: {
        char kpath[256];
        if (!cur || strncpy_from_user(kpath, (const char *)r->ebx, 256) <= 0)
            { r->eax = -1; break; }
        uint32_t resolve_cwd;
        ext2_fs_t *fs = sys_fs(cur, kpath, &resolve_cwd);
        if (!fs || !fs->present) { r->eax = -1; break; }
        uint32_t ino;
        uint8_t type;
        if (ext2_resolve(fs, resolve_cwd, kpath, &ino, &type) < 0)
            { r->eax = -1; break; }
        ext2_inode_t inode;
        if (ext2_read_inode(fs, ino, &inode) < 0)
            { r->eax = -1; break; }
        if (cur->uid != 0) { r->eax = -1; break; }
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
        if (kpath[0] != '/' && cur->cwd_inode == EXT2_ROOT_INO) {
            if (strcmp(kpath, "proc") == 0 || strncmp(kpath, "proc/", 5) == 0 ||
                strcmp(kpath, "dev") == 0 || strncmp(kpath, "dev/", 4) == 0) {
                char abs_path[256];
                abs_path[0] = '/';
                int ai = 1;
                for (int j = 0; kpath[j] && ai < 255; j++, ai++)
                    abs_path[ai] = kpath[j];
                abs_path[ai] = '\0';
                for (int j = 0; j < 256; j++) kpath[j] = abs_path[j];
            }
        }
        uint32_t resolve_cwd;
        ext2_fs_t *fs = sys_fs(cur, kpath, &resolve_cwd);
        if (!fs || !fs->present) {
            if (procfs_is_proc_path(kpath) || devfs_is_dev_path(kpath)) { r->eax = 0; break; }
            r->eax = -1; break;
        }
        uint32_t ino;
        uint8_t type;
        if (ext2_resolve(fs, resolve_cwd, kpath, &ino, &type) < 0) {
            if (procfs_is_proc_path(kpath) || devfs_is_dev_path(kpath)) { r->eax = 0; break; }
            r->eax = -1; break;
        }
        r->eax = 0;
        break;
    }
    case SYSCALL_READLINK: {
        char kpath[256];
        if (!cur || strncpy_from_user(kpath, (const char *)r->ebx, 256) <= 0)
            { r->eax = -1; break; }
        char *kbuf = (char *)kmalloc(256);
        if (!kbuf) { r->eax = -1; break; }
        uint32_t resolve_cwd;
        ext2_fs_t *fs = sys_fs(cur, kpath, &resolve_cwd);
        if (!fs || !fs->present) { kfree(kbuf); r->eax = -1; break; }
        uint32_t ino;
        uint8_t type;
        if (ext2_resolve_nofollow(fs, resolve_cwd, kpath, &ino, &type) < 0 ||
            type != EXT2_FT_SYMLINK) {
            kfree(kbuf); r->eax = -1; break;
        }
        if (ext2_read_link(fs, ino, kbuf, 256) < 0) { kfree(kbuf); r->eax = -1; break; }
        int len = 0;
        while (kbuf[len]) len++;
        if (copy_to_user((void *)r->ecx, kbuf, len + 1) < 0) { kfree(kbuf); r->eax = -1; break; }
        kfree(kbuf);
        r->eax = len;
        break;
    }
    case SYSCALL_LINK: {
        char kold[256], knew[256];
        if (!cur || strncpy_from_user(kold, (const char *)r->ebx, 256) <= 0 ||
            strncpy_from_user(knew, (const char *)r->ecx, 256) <= 0)
            { r->eax = -1; break; }
        uint32_t old_rcwd;
        ext2_fs_t *fs = sys_fs(cur, kold, &old_rcwd);
        if (!fs || !fs->present) { r->eax = -1; break; }
        /* Both paths must be on the same fs */
        uint32_t new_rcwd;
        ext2_fs_t *new_fs = sys_fs(cur, knew, &new_rcwd);
        if (new_fs != fs) { r->eax = -1; break; }
        uint32_t old_ino, new_dir_ino;
        uint8_t old_type, new_dir_type;
        if (ext2_resolve(fs, old_rcwd, kold, &old_ino, &old_type) < 0 ||
            (old_type != EXT2_FT_REG_FILE && old_type != EXT2_FT_SYMLINK))
            { r->eax = -1; break; }
        const char *new_base = knew;
        const char *slash = knew;
        while (*slash) slash++;
        while (slash > knew && *slash != '/') slash--;
        if (slash == knew) {
            new_dir_ino = new_rcwd;
            new_base = knew;
            if (*new_base == '/') { new_dir_ino = EXT2_ROOT_INO; new_base++; }
        } else {
            char dir_path[256];
            int len = slash - knew;
            for (int i = 0; i < len && i < 255; i++) dir_path[i] = knew[i];
            dir_path[len] = '\0';
            if (ext2_resolve(fs, new_rcwd, dir_path, &new_dir_ino, &new_dir_type) < 0 ||
                new_dir_type != EXT2_FT_DIR)
                { r->eax = -1; break; }
            new_base = slash + 1;
        }
        if (!*new_base) { r->eax = -1; break; }
        r->eax = ext2_link(fs, new_dir_ino, new_base, old_ino) == 0 ? 0 : -1;
        break;
    }
    case SYSCALL_TRUNCATE:
    case SYSCALL_FTRUNCATE: {
        uint32_t ino;
        uint8_t type;
        ext2_fs_t *fs;
        if (scheduler_syscall_no == SYSCALL_TRUNCATE) {
            char kpath[256];
            if (!cur || strncpy_from_user(kpath, (const char *)r->ebx, 256) <= 0)
                { r->eax = -1; break; }
            uint32_t resolve_cwd;
            fs = sys_fs(cur, kpath, &resolve_cwd);
            if (!fs || !fs->present) { r->eax = -1; break; }
            if (ext2_resolve(fs, resolve_cwd, kpath, &ino, &type) < 0 ||
                (type != EXT2_FT_REG_FILE && type != EXT2_FT_SYMLINK))
                { r->eax = -1; break; }
        } else {
            fs = fs_get_ext2();
            if (!fs || !fs->present) { r->eax = -1; break; }
            if (fd_fstat(cur->fd_table, (int)r->ebx, &(stat_t){0}) < 0)
                { r->eax = -1; break; }
            fd_entry_t *ent = &cur->fd_table[(int)r->ebx];
            if (ent->type != FD_FILE) { r->eax = -1; break; }
            ino = ent->file->ext2_ino;
        }
        r->eax = ext2_truncate(fs, ino) == 0 ? 0 : -1;
        break;
    }
    case SYSCALL_ALARM: {
        uint32_t seconds = r->ebx;
        cur->alarm_ticks = seconds * 100;
        cur->alarm_remaining = seconds * 100;
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
    case SYSCALL_SYMLINK: {
        char kpath[256], ktarget[256];
        if (!cur || strncpy_from_user(kpath, (const char *)r->ebx, 256) <= 0 ||
            strncpy_from_user(ktarget, (const char *)r->ecx, 256) <= 0)
            { r->eax = -1; break; }
        uint32_t resolve_cwd;
        ext2_fs_t *fs = sys_fs(cur, kpath, &resolve_cwd);
        if (!fs || !fs->present) { r->eax = -1; break; }
        uint32_t dir_ino;
        uint8_t type;
        const char *base_name = kpath;
        const char *slash = kpath;
        while (*slash) slash++;
        while (slash > kpath && *slash != '/') slash--;
        if (slash == kpath) {
            dir_ino = resolve_cwd;
            base_name = kpath;
            if (*base_name == '/') { dir_ino = EXT2_ROOT_INO; base_name++; }
        } else {
            char dir_path[256];
            int len = slash - kpath;
            for (int i = 0; i < len && i < 255; i++) dir_path[i] = kpath[i];
            dir_path[len] = '\0';
            if (ext2_resolve(fs, resolve_cwd, dir_path, &dir_ino, &type) < 0 || type != EXT2_FT_DIR)
                { r->eax = -1; break; }
            base_name = slash + 1;
        }
        if (!*base_name) { r->eax = -1; break; }
        uint32_t new_ino;
        r->eax = ext2_symlink(fs, base_name, dir_ino, ktarget, &new_ino) == 0 ? 0 : -1;
        break;
    }
    case SYSCALL_FCHDIR: {
        if (!cur) { r->eax = -1; break; }
        uint32_t new_cwd;
        r->eax = fd_fchdir(cur->fd_table, (int)r->ebx, &new_cwd) == 0
                 ? (cur->cwd_inode = new_cwd, 0) : -1;
        break;
    }
    case SYSCALL_FCHMOD: {
        if (!cur) { r->eax = -1; break; }
        r->eax = fd_fchmod(cur->fd_table, (int)r->ebx, (uint16_t)r->ecx);
        break;
    }
    case SYSCALL_FCHOWN: {
        if (!cur) { r->eax = -1; break; }
        r->eax = fd_fchown(cur->fd_table, (int)r->ebx, (uint16_t)r->ecx, (uint16_t)r->edx);
        break;
    }
    case SYSCALL_DUP: {
        if (!cur) { r->eax = -1; break; }
        r->eax = fd_dup(cur->fd_table);
        break;
    }
    case SYSCALL_SETUID: {
        if (!cur) { r->eax = -1; break; }
        if (cur->uid != 0) { r->eax = -1; break; }
        cur->uid = (uint16_t)r->ebx;
        cur->euid = (uint16_t)r->ebx;
        r->eax = 0;
        break;
    }
    case SYSCALL_SETGID: {
        if (!cur) { r->eax = -1; break; }
        if (cur->uid != 0) { r->eax = -1; break; }
        cur->gid = (uint16_t)r->ebx;
        cur->egid = (uint16_t)r->ebx;
        r->eax = 0;
        break;
    }
    case SYSCALL_SETEUID: {
        if (!cur) { r->eax = -1; break; }
        if (cur->uid != 0) { r->eax = -1; break; }
        cur->euid = (uint16_t)r->ebx;
        r->eax = 0;
        break;
    }
    case SYSCALL_SETEGID: {
        if (!cur) { r->eax = -1; break; }
        if (cur->uid != 0) { r->eax = -1; break; }
        cur->egid = (uint16_t)r->ebx;
        r->eax = 0;
        break;
    }
    case SYSCALL_GETPPID:
        r->eax = cur ? cur->parent_pid : 0;
        break;
    case SYSCALL_PAUSE: {
        if (!cur) { r->eax = -1; break; }
        cur->kernel_esp = (uint32_t)r;
        cur->state = PROC_BLOCKED;
        cur->sleep_until = 0;
        break;
    }
    case SYSCALL_FSYNC:
    case SYSCALL_FDATASYNC: {
        if (!cur) { r->eax = -1; break; }
        r->eax = fd_fsync(cur->fd_table, (int)r->ebx);
        break;
    }
    case SYSCALL_NICE: {
        if (!cur) { r->eax = -1; break; }
        int inc = (int)r->ebx;
        int new_nice = (int)cur->nice + inc;
        if (new_nice < -20) new_nice = -20;
        if (new_nice > 19) new_nice = 19;
        cur->nice = (uint32_t)(new_nice + 20);
        r->eax = (int)cur->nice - 20;
        break;
    }
    case SYSCALL_GETPRIORITY: {
        if (!cur) { r->eax = -1; break; }
        int which = (int)r->ebx;
        int who = (int)r->ecx;
        if (which == 0) {
            r->eax = (int)cur->nice - 20;
        } else if (which == 1) {
            uint32_t flags;
            spin_lock_irqsave(&proc_lock, &flags);
            for (int i = 0; i < MAX_PROCESSES; i++) {
                if (processes[i].state != PROC_UNUSED && (int)processes[i].pid == who) {
                    r->eax = (int)processes[i].nice - 20;
                    spin_unlock_irqrestore(&proc_lock, flags);
                    break;
                }
            }
            spin_unlock_irqrestore(&proc_lock, flags);
        } else {
            r->eax = -1;
        }
        break;
    }
    case SYSCALL_SETPRIORITY: {
        if (!cur) { r->eax = -1; break; }
        int which = (int)r->ebx;
        int who = (int)r->ecx;
        int prio = (int)r->edx;
        if (prio < -20) prio = -20;
        if (prio > 19) prio = 19;
        if (which == 0) {
            cur->nice = (uint32_t)(prio + 20);
            r->eax = 0;
        } else if (which == 1) {
            uint32_t flags;
            spin_lock_irqsave(&proc_lock, &flags);
            for (int i = 0; i < MAX_PROCESSES; i++) {
                if (processes[i].state != PROC_UNUSED && (int)processes[i].pid == who) {
                    processes[i].nice = (uint32_t)(prio + 20);
                    r->eax = 0;
                    spin_unlock_irqrestore(&proc_lock, flags);
                    break;
                }
            }
            spin_unlock_irqrestore(&proc_lock, flags);
        } else {
            r->eax = -1;
        }
        break;
    }
    case SYSCALL_UTIMENSAT: {
        char kpath[256];
        if (!cur || strncpy_from_user(kpath, (const char *)r->ebx, 256) <= 0)
            { r->eax = -1; break; }
        uint32_t times[2];
        if (r->ecx) {
            if (copy_from_user(times, (const void *)r->ecx, sizeof(times)) < 0)
                { r->eax = -1; break; }
        } else {
            times[0] = 0; times[1] = 0;
        }
        uint32_t resolve_cwd;
        ext2_fs_t *fs = sys_fs(cur, kpath, &resolve_cwd);
        if (!fs || !fs->present) { r->eax = -1; break; }
        uint32_t ino;
        uint8_t type;
        if (ext2_resolve(fs, resolve_cwd, kpath, &ino, &type) < 0)
            { r->eax = -1; break; }
        r->eax = ext2_utimens(fs, ino, times) == 0 ? 0 : -1;
        break;
    }
    case SYSCALL_UMASK: {
        if (!cur) { r->eax = -1; break; }
        uint32_t old = cur->umask;
        cur->umask = r->ebx & 0x1FF;
        r->eax = old;
        break;
    }
    case SYSCALL_REBOOT: {
        (void)cur;
        debug_print("sys: reboot\r\n");
        if (acpi_available())
            acpi_reboot();
        __asm__ __volatile__("cli");
        for (volatile int i = 0; i < 100000; i++);
        outb(0x64, 0xFE);
        for (;;) __asm__ __volatile__("hlt");
    }
    case SYSCALL_POWEROFF: {
        (void)cur;
        debug_print("sys: poweroff (halt)\r\n");
        __asm__ __volatile__("cli");
        for (;;) __asm__ __volatile__("hlt");
    }
    case SYSCALL_SETHOSTNAME: {
        char kbuf[64];
        if (!cur || !r->ebx || strncpy_from_user(kbuf, (const char *)r->ebx, 64) <= 0)
            { r->eax = -1; break; }
        sys_sethostname(kbuf);
        r->eax = 0;
        break;
    }
    case SYSCALL_GETHOSTNAME: {
        char kbuf[64];
        if (!cur || !r->ebx)
            { r->eax = -1; break; }
        sys_gethostname(kbuf, 64);
        if (copy_to_user((void *)r->ebx, kbuf, 64) == 0)
            r->eax = 0;
        else
            r->eax = -1;
        break;
    }
    case SYSCALL_SETPGID: {
        if (!cur) { r->eax = -1; break; }
        int pid = (int)r->ebx;
        int pgid = (int)r->ecx;
        if (pid == 0) pid = (int)cur->pid;
        if (pgid == 0) pgid = pid;
        r->eax = sys_setpgid(pid, pgid);
        break;
    }
    case SYSCALL_GETPGID: {
        if (!cur) { r->eax = -1; break; }
        int pid = (int)r->ebx;
        if (pid == 0) pid = (int)cur->pid;
        r->eax = sys_getpgid(pid);
        break;
    }
    case SYSCALL_TCSETPGRP: {
        (void)r->ebx;
        foreground_pgid = (uint32_t)r->ecx;
        r->eax = 0;
        break;
    }
    case SYSCALL_TCGETPGRP: {
        (void)r->ebx;
        r->eax = (int)foreground_pgid;
        break;
    }
    case SYSCALL_SOCKET:
        r->eax = fd_socket(cur->fd_table, (int)r->ebx, (int)r->ecx, (int)r->edx);
        break;
    case SYSCALL_BIND:
        r->eax = fd_bind(cur->fd_table, (int)r->ebx, (const void *)r->ecx, (uint32_t)r->edx);
        break;
    case SYSCALL_CONNECT:
        r->eax = fd_connect(cur->fd_table, (int)r->ebx, (const void *)r->ecx, (uint32_t)r->edx);
        if ((int)r->eax == -2) {
            cur->kernel_esp = (uint32_t)r;
            cur->state = PROC_BLOCKED;
        }
        break;
    case SYSCALL_LISTEN:
        r->eax = fd_listen(cur->fd_table, (int)r->ebx, (int)r->ecx);
        break;
    case SYSCALL_ACCEPT:
        r->eax = fd_accept(cur->fd_table, (int)r->ebx, (void *)r->ecx, (uint32_t *)r->edx);
        if ((int)r->eax == -2) {
            cur->kernel_esp = (uint32_t)r;
            cur->state = PROC_BLOCKED;
        }
        break;
    case SYSCALL_SEND:
        r->eax = fd_send(cur->fd_table, (int)r->ebx, (const void *)r->ecx, (uint32_t)r->edx, (int)r->esi);
        break;
    case SYSCALL_RECV:
        r->eax = fd_recv(cur->fd_table, (int)r->ebx, (void *)r->ecx, (uint32_t)r->edx, (int)r->esi);
        if ((int)r->eax == -2) {
            cur->kernel_esp = (uint32_t)r;
            cur->state = PROC_BLOCKED;
        }
        break;
    case SYSCALL_MOUNT: {
        if (!cur || cur->uid != 0) { r->eax = -1; break; }
        char kdev[256], ktarget[256];
        if (strncpy_from_user(kdev, (const char *)r->ebx, 256) <= 0 ||
            strncpy_from_user(ktarget, (const char *)r->ecx, 256) <= 0)
            { r->eax = -1; break; }

        block_device_t *blk = NULL;
        /* Map /dev/<name> to block device */
        if (kdev[0] == '/' && kdev[1] == 'd' && kdev[2] == 'e' && kdev[3] == 'v' && kdev[4] == '/') {
            char *bname = kdev + 5;
            for (int i = 0; i < block_device_count(); i++) {
                block_device_t *d = block_device_get(i);
                const char *dn = d->name;
                const char *bn = bname;
                int match = 1;
                while (*dn && *bn && *dn == *bn) { dn++; bn++; }
                if (*dn != *bn) match = 0;
                if (match) { blk = d; break; }
            }
        }
        if (!blk) { r->eax = -1; break; }

        uint32_t lba = 0;
        if (kdev[4] == '/') {
            const char *num = kdev + 5;
            while (*num >= '0' && *num <= '9') num++;
            if (*num) {
                int pnum = 0;
                const char *n = kdev + 5;
                while (*n >= '0' && *n <= '9') { pnum = pnum * 10 + (*n - '0'); n++; }
                mbr_t mbr;
                if (mbr_parse(blk, &mbr) == 0 && pnum >= 0 && pnum < 4 && mbr.partitions[pnum].type != 0)
                    lba = mbr.partitions[pnum].lba_start;
            }
        }

        if (fs_mount(ktarget, blk, lba) < 0) {
            /* Try probing ext2 at various offsets */
            int found = 0;
            if (ext2_probe(blk, 0) == 0 && fs_mount(ktarget, blk, 0) == 0) found = 1;
            if (!found) {
                mbr_t mbr;
                if (mbr_parse(blk, &mbr) == 0) {
                    for (int p = 0; p < 4; p++) {
                        if (mbr.partitions[p].type && ext2_probe(blk, mbr.partitions[p].lba_start) == 0) {
                            if (fs_mount(ktarget, blk, mbr.partitions[p].lba_start) == 0) { found = 1; break; }
                        }
                    }
                }
            }
            r->eax = found ? 0 : -1;
        } else {
            r->eax = 0;
        }
        break;
    }
    case SYSCALL_GETFBINFO: {
        if (!cur) { r->eax = -1; break; }
        fb_info_t info;
        fb_get_info(&info);
        debug_printf("getfb: addr=%x w=%u h=%u p=%u bpp=%u ebx=%x\r\n",
                     info.addr, info.width, info.height, info.pitch, info.bpp, r->ebx);
        fb_info_t __attribute__((aligned(4))) uinfo;
        uinfo.addr   = info.addr;
        uinfo.pitch  = info.pitch;
        uinfo.width  = info.width;
        uinfo.height = info.height;
        uinfo.bpp    = info.bpp;
        uinfo.type   = info.type;
        if (copy_to_user(&uinfo, (void *)r->ebx, sizeof(fb_info_t)) < 0) {
            debug_printf("getfb: copy_to_user FAILED ebx=%x\r\n", r->ebx);
            r->eax = -1; break;
        }
        r->eax = 0;
        break;
    }
    case SYSCALL_GETMOUSE: {
        if (!cur) { r->eax = -1; break; }
        mouse_state_t __attribute__((aligned(4))) state;
        mouse_get_state(&state);
        if (copy_to_user(&state, (void *)r->ebx, sizeof(mouse_state_t)) < 0)
            { r->eax = -1; break; }
        r->eax = 0;
        break;
    }
    case SYSCALL_OOM_KILL: {
        if (!cur || cur->uid != 0) { r->eax = -1; break; }
        int pid = (int)r->ebx;
        if (pid == 0) {
            r->eax = oom_kill_victim();
        } else {
            r->eax = oom_kill_pid(pid);
        }
        break;
    }
    case SYSCALL_MAP_FB: {
        uint32_t phys = fb_get_phys();
        debug_printf("mapfb: phys=%x\r\n", phys);
        if (!phys) { r->eax = 0; break; }
        fb_info_t info;
        fb_get_info(&info);
        uint32_t fb_size = info.pitch * info.height;
        uint32_t pages = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;
        debug_printf("mapfb: pitch=%u h=%u pages=%u\r\n", info.pitch, info.height, pages);
        uint32_t vaddr = 0xA0000000;
        process_t *p = scheduler_current_process();
        if (!p || !p->page_dir) { r->eax = 0; break; }
        for (uint32_t i = 0; i < pages; i++) {
            if (vmm_map_page(p->page_dir, phys + i * PAGE_SIZE,
                             vaddr + i * PAGE_SIZE,
                             VMM_PRESENT | VMM_WRITABLE | VMM_USER) < 0) {
                r->eax = 0;
                break;
            }
        }
        r->eax = vaddr;
        break;
    }
    case SYSCALL_FB_SET_ACTIVE: {
        fb_set_active((int)r->ebx);
        r->eax = 0;
        break;
    }
    }
}
