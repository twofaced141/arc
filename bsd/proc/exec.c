#include "bsd/proc.h"
#include "bsd/errno.h"
#include "bsd/vfs.h"
#include "bsd/signal.h"
#include "thread.h"
#include "scheduler.h"
#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "spinlock.h"
#include "debug.h"
#include "memory.h"
#include "isr.h"

#include "bsd/arch.h"

#if BSD_ELF_CLASS == ELFCLASS64
#include "bsd/elf64.h"
typedef Elf64_Ehdr elf_ehdr_t;
typedef Elf64_Phdr elf_phdr_t;
typedef Elf64_Dyn  elf_dyn_t;
#else
#include "bsd/elf32.h"
typedef Elf32_Ehdr elf_ehdr_t;
typedef Elf32_Phdr elf_phdr_t;
typedef Elf32_Dyn  elf_dyn_t;
#endif

#define INTERP_BASE_ADDR 0x10000000
#define TEMP_BUF_SIZE    4096

struct elf_load_state {
    elf_ehdr_t ehdr;
    elf_phdr_t *phdrs;
    page_directory_t *page_dir;
    vnode_t *vp;
    int is_dynamic;
    bsd_elf_addr_t interp_base;
    bsd_elf_addr_t interp_entry;
    bsd_elf_addr_t interp_phoff;
    char interp_path[256];
    bsd_elf_addr_t max_seg_end;
};

static int elf_read(vnode_t *vp, void *buf, uint32_t offset, uint32_t size) {
    if (!vp || !vp->ops || !vp->ops->read)
        return -1;
    return vp->ops->read(vp, buf, size, offset);
}

static int elf_validate(elf_ehdr_t *ehdr) {
    uint32_t *magic = (uint32_t *)ehdr->e_ident;
    if (*magic != ELF_MAGIC)
        return -ENOEXEC;
    if (ehdr->e_ident[EI_CLASS] != BSD_ELF_CLASS)
        return -ENOEXEC;
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB)
        return -ENOEXEC;
    if (ehdr->e_machine != BSD_ELF_MACHINE)
        return -ENOEXEC;
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
        return -ENOEXEC;
    if (ehdr->e_phentsize != sizeof(elf_phdr_t))
        return -ENOEXEC;
    return 0;
}

static int elf_find_interpreter(struct elf_load_state *st) {
    st->is_dynamic = 0;
    st->interp_path[0] = '\0';

    for (uint16_t i = 0; i < st->ehdr.e_phnum; i++) {
        if (st->phdrs[i].p_type != PT_INTERP)
            continue;
        if (st->phdrs[i].p_filesz >= sizeof(st->interp_path))
            return -ENOEXEC;
        if (elf_read(st->vp, st->interp_path,
                     st->phdrs[i].p_offset, st->phdrs[i].p_filesz) < 0)
            return -EIO;
        st->interp_path[st->phdrs[i].p_filesz] = '\0';
        st->is_dynamic = 1;
        break;
    }
    return 0;
}

static int elf_load_segments_common(struct elf_load_state *st,
                                    vnode_t *src_vp,
                                    elf_phdr_t *phdrs, uint16_t phnum,
                                    bsd_elf_addr_t base_offset,
                                    bsd_elf_addr_t *out_max_end)
{
    bsd_elf_addr_t max_end = 0;

    for (uint16_t i = 0; i < phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD)
            continue;

        bsd_elf_addr_t seg_start = phdrs[i].p_vaddr;
        bsd_elf_addr_t seg_end_file = seg_start + phdrs[i].p_filesz;
        bsd_elf_addr_t seg_end_mem = seg_start + phdrs[i].p_memsz;

        if (seg_end_mem < seg_start)
            return -ENOEXEC;
        if (seg_end_mem > USER_STACK_TOP)
            return -ENOEXEC;

        bsd_elf_addr_t page_start = seg_start & ~(bsd_elf_addr_t)(PAGE_SIZE - 1);
        bsd_elf_addr_t page_end = (seg_end_mem + PAGE_SIZE - 1) & ~(bsd_elf_addr_t)(PAGE_SIZE - 1);

        uint32_t page_flags = VMM_PRESENT | VMM_USER;
        if (phdrs[i].p_flags & PF_W)
            page_flags |= VMM_WRITABLE;

        /* If the segment has a BSS zero-fill region (memsz > filesz), pages
         * that overlap that region must be mapped writable even when the
         * segment's p_flags lack PF_W (toolchains sometimes put .rodata and
         * .bss in the same read-only segment). */
        int has_bss = phdrs[i].p_memsz > phdrs[i].p_filesz;

        for (bsd_elf_addr_t vaddr = page_start; vaddr < page_end; vaddr += PAGE_SIZE) {
            uint32_t flags = page_flags;

            if (has_bss && vaddr < seg_end_mem && vaddr + PAGE_SIZE > seg_end_file)
                flags |= VMM_WRITABLE;

            bsd_elf_addr_t map_vaddr = vaddr + base_offset;

            void *phys = pmm_alloc_page();
            if (!phys)
                return -ENOMEM;

            vmm_map_page(st->page_dir, (bsd_elf_addr_t)(uintptr_t)phys,
                         map_vaddr, flags);

            bsd_elf_addr_t copy_start = vaddr > seg_start ? vaddr : seg_start;
            bsd_elf_addr_t copy_end = vaddr + PAGE_SIZE < seg_end_file
                                  ? vaddr + PAGE_SIZE : seg_end_file;

            void *page_va = vmm_temp_map((uintptr_t)phys);
            memset(page_va, 0, PAGE_SIZE);

            if (copy_start < copy_end) {
                uint32_t page_off = copy_start - vaddr;
                uint32_t file_off = phdrs[i].p_offset + (copy_start - seg_start);
                uint32_t copy_len = copy_end - copy_start;

                uint8_t tmp_buf[TEMP_BUF_SIZE];
                if (copy_len > TEMP_BUF_SIZE)
                    copy_len = TEMP_BUF_SIZE;

                if (elf_read(src_vp, tmp_buf, file_off, copy_len) > 0) {
                    uint8_t *dst = (uint8_t *)page_va + page_off;
                    memcpy(dst, tmp_buf, copy_len);
                }
            }

            vmm_temp_unmap();
        }

        if (seg_end_mem > max_end)
            max_end = seg_end_mem;
    }

    if (out_max_end)
        *out_max_end = max_end;
    return 0;
}

static int elf_load_interpreter(struct elf_load_state *st) {
    if (!st->is_dynamic)
        return 0;

    vnode_t *ivp = vfs_lookup(st->interp_path);
    if (!ivp) {
        log_printf(LOG_LEVEL_ERROR, "exec: interpreter not found: %s\r\n", st->interp_path);
        return -ENOENT;
    }

    elf_ehdr_t iehdr;
    if (elf_read(ivp, &iehdr, 0, sizeof(iehdr)) < 0) {
        vnode_put(ivp);
        return -EIO;
    }

    if (elf_validate(&iehdr) < 0) {
        log_print(LOG_LEVEL_ERROR, "exec: bad interpreter ELF\r\n");
        vnode_put(ivp);
        return -ENOEXEC;
    }

    uint32_t iphdrs_size = iehdr.e_phnum * sizeof(elf_phdr_t);
    elf_phdr_t *iphdrs = (elf_phdr_t *)kmalloc(iphdrs_size);
    if (!iphdrs) {
        vnode_put(ivp);
        return -ENOMEM;
    }

    if (elf_read(ivp, iphdrs, iehdr.e_phoff, iphdrs_size) < 0) {
        kfree(iphdrs);
        vnode_put(ivp);
        return -EIO;
    }

    bsd_elf_addr_t lowest_vaddr = (bsd_elf_addr_t)-1;
    for (uint16_t i = 0; i < iehdr.e_phnum; i++) {
        if (iphdrs[i].p_type == PT_LOAD && iphdrs[i].p_vaddr < lowest_vaddr)
            lowest_vaddr = iphdrs[i].p_vaddr;
    }

    bsd_elf_addr_t interp_load_base = INTERP_BASE_ADDR;
    if (lowest_vaddr != (bsd_elf_addr_t)-1 && lowest_vaddr < INTERP_BASE_ADDR)
        interp_load_base = INTERP_BASE_ADDR;

    bsd_elf_addr_t base_offset = interp_load_base;
    if (lowest_vaddr != (bsd_elf_addr_t)-1 && lowest_vaddr != 0)
        base_offset = interp_load_base;

    bsd_elf_addr_t max_end = 0;
    int err = elf_load_segments_common(st, ivp, iphdrs, iehdr.e_phnum,
                                       base_offset, &max_end);
    if (err < 0) {
        kfree(iphdrs);
        vnode_put(ivp);
        return err;
    }

    st->interp_base = base_offset;
    st->interp_entry = iehdr.e_entry + base_offset;
    st->interp_phoff = iehdr.e_phoff + base_offset;

    kfree(iphdrs);
    vnode_put(ivp);

    return 0;
}

static int elf_map_segments(struct elf_load_state *st) {
    return elf_load_segments_common(st, st->vp, st->phdrs, st->ehdr.e_phnum,
                                    0, &st->max_seg_end);
}

static int elf_setup_stack(proc_t *proc, struct elf_load_state *st) {
    (void)proc;
    bsd_elf_addr_t stack_top = USER_STACK_TOP;

    for (int si = 0; si < USER_STACK_PAGES; si++) {
        void *phys = pmm_alloc_page();
        if (!phys)
            return -ENOMEM;

        bsd_elf_addr_t vaddr = stack_top - (si + 1) * PAGE_SIZE;
        vmm_map_page(st->page_dir, (bsd_elf_addr_t)(uintptr_t)phys, vaddr,
                     VMM_PRESENT | VMM_WRITABLE | VMM_USER);
    }

    return 0;
}

static int elf_setup_tls(struct elf_load_state *st) {
    void *tls_phys = pmm_alloc_page();
    if (!tls_phys)
        return -ENOMEM;

    uint8_t *page = (uint8_t *)vmm_temp_map((uintptr_t)tls_phys);
    memset(page, 0, PAGE_SIZE);

    arch_setup_tls_page(page, USER_TLS_VADDR);

    vmm_temp_unmap();
    vmm_map_page(st->page_dir, (bsd_elf_addr_t)(uintptr_t)tls_phys,
                 USER_TLS_VADDR, VMM_PRESENT | VMM_WRITABLE | VMM_USER);
    return 0;
}


#define EXEC_STR_MAX_TOTAL (64 * 1024)
#define EXEC_ARG_MAX 128

/* Copy a NUL-terminated string from user memory into `dst` (capacity
 * dst_cap).  Returns length (excluding NUL) or -1 on EFAULT/overflow. */
static int exec_user_str(char *dst, size_t dst_cap, const char *usr) {
    size_t len = 0;
    size_t clen;
    for (;;) {
        char chunk[32];
        clen = sizeof(chunk);
        if (copy_from_user(chunk, usr + len, (uint32_t)clen) != 0)
            return -1;
        for (size_t i = 0; i < clen; i++) {
            if (chunk[i] == '\0')
                clen = i;
        }
        if (clen < sizeof(chunk))
            goto done;
        len += sizeof(chunk);
        if (len >= dst_cap)
            return -1;
    }
done:
    if (len + clen >= dst_cap)
        return -1;
    if (clen > 0)
        copy_from_user(dst + len, usr + len, (uint32_t)clen);
    dst[len + clen] = '\0';
    return (int)(len + clen);
}

/* Collect an argv/envp pointer array into one kernel buffer.
 * Returns 0 on success (count set, buf/kbuf_len filled), -errno on
 * failure.  The buffer must be kfree'd by the caller. */
static int exec_collect_strings(char *usr_ptrs, char **kbuf,
                                size_t *kbuf_len, int *count) {
    size_t cap = 4096;
    size_t used = 0;
    char *buf = (char *)kmalloc(cap);
    int n = 0;

    if (!buf)
        return -ENOMEM;

    for (;;) {
        char *s;
        if (copy_from_user(&s, usr_ptrs + n * sizeof(char *),
                           sizeof(char *)) != 0) {
            kfree(buf);
            return -EFAULT;
        }
        if (!s)
            break;
        if (n >= EXEC_ARG_MAX) {
            kfree(buf);
            return -E2BIG;
        }

        size_t need;
        int len = exec_user_str(buf + used, cap - used, s);
        if (len < 0) {
            /* String may exceed the remaining space — measure it into a
             * stack buffer to grow the allocation precisely. */
            char tmp[256];
            len = exec_user_str(tmp, sizeof(tmp), s);
            if (len < 0) {
                kfree(buf);
                return -EFAULT;
            }
            need = (size_t)len + 1;
            if (used + need > EXEC_STR_MAX_TOTAL) {
                kfree(buf);
                return -E2BIG;
            }
            if (used + need > cap) {
                size_t ncap = cap;
                while (ncap < used + need)
                    ncap *= 2;
                char *nb = (char *)kmalloc(ncap);
                if (!nb) {
                    kfree(buf);
                    return -ENOMEM;
                }
                memcpy(nb, buf, used);
                kfree(buf);
                buf = nb;
                cap = ncap;
            }
            /* Re-read with the grown buffer. */
            if (exec_user_str(buf + used, cap - used, s) < 0) {
                kfree(buf);
                return -EFAULT;
            }
        }
        used += (size_t)len + 1;
        n++;
    }

    *kbuf = buf;
    *kbuf_len = used;
    *count = n;
    return 0;
}

/* Lay out argc/argv/envp on the user stack (mapped at USER_STACK_TOP).
 * Returns the new stack pointer, 16-byte aligned.  The stack starts
 * one page below the top: the page containing USER_STACK_TOP is never
 * mapped, so buffers living near sp would otherwise straddle the
 * boundary and get rejected by copy_to_user's range check. */
static uint64_t exec_build_stack(char *argv_buf, int argc,
                                 char *envp_buf, int envc) {
    uint64_t sp = USER_STACK_TOP - 4096;

    /* Copy strings, argv first then envp, growing downward.  Record
     * the user-space address of each string. */
    char *uargv[EXEC_ARG_MAX];
    char *uenvp[EXEC_ARG_MAX];

    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv_buf) + 1;
        sp -= len;
        sp &= ~15ULL;
        uargv[i] = (char *)sp;
        memcpy((void *)sp, argv_buf, len);
        argv_buf += len;
    }
    for (int i = envc - 1; i >= 0; i--) {
        size_t len = strlen(envp_buf) + 1;
        sp -= len;
        sp &= ~15ULL;
        uenvp[i] = (char *)sp;
        memcpy((void *)sp, envp_buf, len);
        envp_buf += len;
    }

    /* envp array (NULL-terminated), then argv array (NULL-terminated),
     * then argc. */
    sp -= 8;
    *(uint64_t *)sp = 0;                    /* envp terminator */
    for (int i = envc - 1; i >= 0; i--) {
        sp -= 8;
        *(uint64_t *)sp = (uint64_t)uenvp[i];
    }
    sp -= 8;
    *(uint64_t *)sp = 0;                    /* argv terminator */
    for (int i = argc - 1; i >= 0; i--) {
        sp -= 8;
        *(uint64_t *)sp = (uint64_t)uargv[i];
    }
    sp -= 8;
    *(uint64_t *)sp = (uint64_t)argc;

    return sp;
}

int proc_execve(registers_t *r) {
    proc_t *p = proc_current();
    if (!p)
        return -ENOENT;

    char path[256];
    if (copy_from_user(path, (const void *)(uintptr_t)bsd_syscall_arg0(r),
                       sizeof(path) - 1) != 0)
        return -EFAULT;
    path[sizeof(path) - 1] = '\0';

    /* exec(2): relative paths resolve against the current directory. */
    char abs_path[256];
    if (vfs_build_abs_path(p->cwd, path, abs_path, sizeof(abs_path)) != 0)
        return -ENAMETOOLONG;

    /* Collect argv/envp into kernel buffers while still mapped in the
     * old address space (they live in the caller's memory). */
    char *argv_buf = NULL, *envp_buf = NULL;
    size_t argv_len = 0, envp_len = 0;
    int argc = 0, envc = 0;

    char *uargv = (char *)bsd_syscall_arg1(r);
    char *uenvp = (char *)bsd_syscall_arg2(r);
    int rc = 0;
    if (uargv) {
        rc = exec_collect_strings(uargv, &argv_buf, &argv_len, &argc);
        if (rc < 0)
            return rc;
    }
    if (uenvp) {
        rc = exec_collect_strings(uenvp, &envp_buf, &envp_len, &envc);
        if (rc < 0) {
            kfree(argv_buf);
            return rc;
        }
    }

    vnode_t *vp = vfs_lookup(abs_path);
    if (!vp) {
        log_print(LOG_LEVEL_ERROR, "exec: file not found\r\n");
        kfree(argv_buf);
        kfree(envp_buf);
        return -ENOENT;
    }

    struct elf_load_state st;
    memset(&st, 0, sizeof(st));
    st.vp = vp;

    if (elf_read(vp, &st.ehdr, 0, sizeof(st.ehdr)) < 0) {
        vnode_put(vp);
        goto fail;
    }

    int err = elf_validate(&st.ehdr);
    if (err < 0) {
        log_print(LOG_LEVEL_ERROR, "exec: bad ELF header\r\n");
        vnode_put(vp);
        goto fail;
    }

    uint32_t phdr_size = st.ehdr.e_phnum * sizeof(elf_phdr_t);
    st.phdrs = (elf_phdr_t *)kmalloc(phdr_size);
    if (!st.phdrs) {
        vnode_put(vp);
        goto fail;
    }

    if (elf_read(vp, st.phdrs, st.ehdr.e_phoff, phdr_size) < 0) {
        kfree(st.phdrs);
        vnode_put(vp);
        goto fail;
    }

    err = elf_find_interpreter(&st);
    if (err < 0) {
        kfree(st.phdrs);
        vnode_put(vp);
        goto fail;
    }

    st.page_dir = vmm_create_directory();
    if (!st.page_dir) {
        kfree(st.phdrs);
        vnode_put(vp);
        goto fail;
    }

    err = elf_map_segments(&st);
    if (err < 0) {
        vmm_free_directory(st.page_dir);
        kfree(st.phdrs);
        vnode_put(vp);
        goto fail;
    }

    err = elf_setup_stack(p, &st);
    if (err < 0) {
        vmm_free_directory(st.page_dir);
        kfree(st.phdrs);
        vnode_put(vp);
        goto fail;
    }

    err = elf_setup_tls(&st);
    if (err < 0) {
        vmm_free_directory(st.page_dir);
        kfree(st.phdrs);
        vnode_put(vp);
        goto fail;
    }

    bsd_elf_addr_t entry = st.ehdr.e_entry;

    if (st.is_dynamic) {
        err = elf_load_interpreter(&st);
        if (err < 0) {
            vmm_free_directory(st.page_dir);
            kfree(st.phdrs);
            vnode_put(vp);
            goto fail;
        }
        entry = st.interp_entry;
    }

    kfree(st.phdrs);
    vnode_put(vp);

    page_directory_t *old_dir = p->page_dir;
    p->page_dir = st.page_dir;
    if (p->thread)
        p->thread->page_dir = st.page_dir;

    /* Switch CR3 before touching the new address space (stack layout
     * below writes argv/envp into freshly mapped user pages). */
    vmm_switch_directory(st.page_dir);

    if (old_dir) {
        /* FIXME: with CLONE_VM, old_dir is shared with the parent process.
         * Freeing it here would corrupt the parent's address space.
         * For now, skip the free until we have proper ref-counting. */
        // vmm_free_directory(old_dir);
    }

    /* POSIX 2.9.1 (process execution): signals set to be caught revert to
     * the default action, ignored signals stay ignored, all pending
     * signals are cleared, and the altstack is torn down — the new image
     * starts with a fresh stack.  The blocked mask is preserved. */
    {
        sigstate_t *ss = &p->signals;
        for (int sig = 1; sig < NSIG; sig++) {
            if (ss->handler[sig] != SIG_IGN)
                ss->handler[sig] = SIG_DFL;
            ss->sa_mask[sig] = 0;
            ss->sa_flags[sig] = 0;
            ss->sa_restorer[sig] = 0;
            ss->pending[sig] = 0;
        }
        ss->sigframe_addr = 0;
        ss->in_signal = 0;
        ss->on_altstack = 0;
        ss->ss_sp = 0;
        ss->ss_size = 0;
        ss->ss_active = 0;
        p->exit_sig = 0;
        p->stopped = 0;
    }

    /* The new image has no heap, mmap regions or mmap cursor of its
     * own — drop any mappings the old image had (POSIX: exec unmaps
     * all mapped regions). */
    mmap_teardown(p);
    p->heap_end = USER_HEAP_START;
    p->mmap_next = USER_MMAP_START;

    /* Build the initial user stack: argc, argv[], envp[], strings. */
    uint64_t sp = USER_STACK_TOP;
    if (argv_buf || argc > 0)
        sp = exec_build_stack(argv_buf, argc, envp_buf, envc);
    kfree(argv_buf);
    kfree(envp_buf);

    /* POSIX: descriptors with FD_CLOEXEC are closed on a successful
     * exec.  Failure paths above leave them untouched. */
    for (int i = 0; i < p->fd_capacity; i++) {
        if (p->fds[i].used && p->fds[i].cloexec)
            vfs_close(p, i);
    }

    arch_setup_exec_regs(r, entry, sp);

    return 0;

fail:
    kfree(argv_buf);
    kfree(envp_buf);
    return err;
}


