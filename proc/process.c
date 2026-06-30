#include "process.h"
#include "pmm.h"
#include "vmm.h"
#include "terminal.h"
#include "debug.h"
#include "isr.h"
#include "gdt.h"
#include "fd.h"
#include "fs.h"
#include "elf32.h"
#include "scheduler.h"
#include "signal.h"

process_t processes[MAX_PROCESSES];
static uint32_t next_pid = 1;
spinlock_t proc_lock = SPINLOCK_INIT;

void process_init(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        processes[i].pid = 0;
        processes[i].state = PROC_UNUSED;
        processes[i].page_dir = NULL;
    }
    debug_print("process: init done\r\n");
}

process_t *process_create_user(uint32_t eip, const void *code, uint32_t code_size) {
    process_t *proc = NULL;

    uint32_t flags;
    spin_lock_irqsave(&proc_lock, &flags);

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROC_UNUSED) {
            proc = &processes[i];
            break;
        }
    }
    if (!proc) { spin_unlock_irqrestore(&proc_lock, flags); return NULL; }

    proc->pid = next_pid++;
    proc->state = PROC_READY;

    spin_unlock_irqrestore(&proc_lock, flags);
    proc->time_slice = 5;
    proc->sleep_until = 0;

    proc->kernel_stack = (uint8_t *)pmm_alloc_pages(PROC_KSTACK_SIZE / PAGE_SIZE);
    if (!proc->kernel_stack) { proc->state = PROC_UNUSED; return NULL; }
    proc->kernel_stack_top = (uint32_t)proc->kernel_stack + PROC_KSTACK_SIZE;

    fd_init_table(proc->fd_table);
    proc->heap_break = USER_HEAP_START;
    proc->heap_mapped_end = USER_HEAP_START;
    proc->mmap_brk = USER_MMAP_START;
    proc->uid = 0;
    proc->gid = 0;
    proc->euid = 0;
    proc->egid = 0;

    proc->page_dir = vmm_create_directory();
    if (!proc->page_dir) { pmm_free_pages(proc->kernel_stack, PROC_KSTACK_SIZE / PAGE_SIZE); proc->state = PROC_UNUSED; return NULL; }

    uint32_t pages = (code_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t code_virt = eip & ~0xFFF;
    uint32_t *code_pages = NULL;
    uint32_t pages_alloced = 0;

    if (pages > 0) {
        code_pages = (uint32_t *)kmalloc(pages * sizeof(uint32_t));
        if (!code_pages) goto err_pagedir;
    }

    for (uint32_t i = 0; i < pages; i++) {
        uint32_t phys = (uint32_t)pmm_alloc_page();
        if (!phys) goto err_code_pages;
        code_pages[i] = phys;
        pages_alloced = i + 1;
        if (i == 0) {
            uint8_t *tmp = (uint8_t *)vmm_temp_map(phys);
            for (uint32_t j = 0; j < code_size && j < PAGE_SIZE; j++)
                tmp[j] = ((uint8_t *)code)[j];
            for (uint32_t j = code_size; j < PAGE_SIZE; j++)
                tmp[j] = 0;
            vmm_temp_unmap();
        } else {
            uint8_t *tmp = (uint8_t *)vmm_temp_map(phys);
            for (uint32_t j = 0; j < PAGE_SIZE; j++)
                tmp[j] = 0;
            vmm_temp_unmap();
        }
        (void)vmm_map_page(proc->page_dir, phys, code_virt + i * PAGE_SIZE,
                     VMM_PRESENT | VMM_WRITABLE | VMM_USER);
    }

    uint32_t user_stack_top = 0xC0000000;
    for (int si = 0; si < USER_STACK_PAGES; si++) {
        uint32_t phys = (uint32_t)pmm_alloc_page();
        if (!phys) goto err_code_pages;
        vmm_map_page(proc->page_dir, phys, user_stack_top - (si + 1) * PAGE_SIZE,
                     VMM_PRESENT | VMM_WRITABLE | VMM_USER);
    }
    proc->user_esp = user_stack_top;
    proc->eip = eip;

    registers_t *frame = (registers_t *)(proc->kernel_stack_top - sizeof(registers_t));
    frame->gs = 0x23;
    frame->fs = 0x23;
    frame->es = 0x23;
    frame->ds = 0x23;
    frame->edi = 0;
    frame->esi = 0;
    frame->ebp = 0;
    frame->esp = (uint32_t)&frame->int_no;
    frame->ebx = 0;
    frame->edx = 0;
    frame->ecx = 0;
    frame->eax = 0;
    frame->int_no = 0;
    frame->err_code = 0;
    frame->eip = eip;
    frame->cs = 0x1B;
    frame->eflags = 0x202;
    frame->useresp = proc->user_esp;
    frame->ss = 0x23;

    proc->kernel_esp = (uint32_t)frame;

    if (code_pages) kfree(code_pages);
    return proc;

err_code_pages:
    for (uint32_t i = 0; i < pages_alloced; i++) {
        vmm_unmap_page(proc->page_dir, code_virt + i * PAGE_SIZE);
        pmm_free_page((void *)code_pages[i]);
    }
    if (code_pages) kfree(code_pages);
err_pagedir:
    vmm_free_directory(proc->page_dir);
    pmm_free_pages(proc->kernel_stack, PROC_KSTACK_SIZE / PAGE_SIZE);
    proc->state = PROC_UNUSED;
    return NULL;
}

process_t *process_create_elf(file_t *file) {
    Elf32_Ehdr ehdr;
    fs_read(file, &ehdr, 0, sizeof(Elf32_Ehdr));

    uint32_t *magic = (uint32_t *)ehdr.e_ident;
    if (*magic != ELF_MAGIC || ehdr.e_ident[4] != ELF_32BIT ||
        ehdr.e_machine != EM_386 || ehdr.e_type != ET_EXEC)
        return NULL;

    uint32_t phdr_bytes = ehdr.e_phnum * sizeof(Elf32_Phdr);
    Elf32_Phdr *phdrs = (Elf32_Phdr *)kmalloc(phdr_bytes);
    if (!phdrs) return NULL;
    fs_read(file, phdrs, ehdr.e_phoff, phdr_bytes);

    process_t *proc = NULL;

    uint32_t sflags;
    spin_lock_irqsave(&proc_lock, &sflags);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROC_UNUSED) {
            proc = &processes[i];
            break;
        }
    }
    if (!proc) { spin_unlock_irqrestore(&proc_lock, sflags); kfree(phdrs); return NULL; }

    proc->pid = next_pid++;
    proc->state = PROC_READY;
    spin_unlock_irqrestore(&proc_lock, sflags);
    proc->time_slice = 5;
    proc->sleep_until = 0;
    proc->exit_code = 0;
    proc->parent_pid = 0;
    proc->wait_child_pid = 0;
    proc->cwd_inode = EXT2_ROOT_INO;
    proc->mmap_brk = USER_MMAP_START;
    proc->uid = 0;
    proc->gid = 0;
    proc->euid = 0;
    proc->egid = 0;
    signal_init_process(proc);

    proc->kernel_stack = (uint8_t *)pmm_alloc_pages(PROC_KSTACK_SIZE / PAGE_SIZE);
    if (!proc->kernel_stack) { proc->state = PROC_UNUSED; kfree(phdrs); return NULL; }
    proc->kernel_stack_top = (uint32_t)proc->kernel_stack + PROC_KSTACK_SIZE;

    fd_init_table(proc->fd_table);
    proc->heap_break = USER_HEAP_START;
    proc->heap_mapped_end = USER_HEAP_START;

    proc->page_dir = vmm_create_directory();
    if (!proc->page_dir) {
        pmm_free_pages(proc->kernel_stack, PROC_KSTACK_SIZE / PAGE_SIZE);
        proc->state = PROC_UNUSED;
        kfree(phdrs);
        return NULL;
    }

    uint32_t entry = ehdr.e_entry;

    for (uint32_t i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;

        uint32_t seg_start = phdrs[i].p_vaddr;
        uint32_t seg_end_file = seg_start + phdrs[i].p_filesz;
        uint32_t seg_end_mem = seg_start + phdrs[i].p_memsz;
        uint32_t page_start = seg_start & ~0xFFF;
        uint32_t page_end = (seg_end_mem + PAGE_SIZE - 1) & ~0xFFF;

        uint32_t page_flags = VMM_PRESENT | VMM_USER;
        if (phdrs[i].p_flags & PF_W) page_flags |= VMM_WRITABLE;

        for (uint32_t vaddr = page_start; vaddr < page_end; vaddr += PAGE_SIZE) {
            uint32_t phys = (uint32_t)pmm_alloc_page();
            if (!phys) goto err_load;

            (void)vmm_map_page(proc->page_dir, phys, vaddr, page_flags);

            uint8_t *page = (uint8_t *)vmm_temp_map(phys);
            for (uint32_t j = 0; j < PAGE_SIZE; j++)
                page[j] = 0;

            uint32_t copy_start = vaddr > seg_start ? vaddr : seg_start;
            uint32_t copy_end = vaddr + PAGE_SIZE < seg_end_file
                                ? vaddr + PAGE_SIZE : seg_end_file;
            if (copy_start < copy_end) {
                uint32_t page_off = copy_start - vaddr;
                uint32_t file_off = phdrs[i].p_offset + (copy_start - seg_start);
                uint32_t copy_len = copy_end - copy_start;
                fs_read(file, page + page_off, file_off, copy_len);
            }

            vmm_temp_unmap();
        }
    }

    kfree(phdrs);

    uint32_t user_stack_top = 0xC0000000;
    for (int si = 0; si < USER_STACK_PAGES; si++) {
        uint32_t phys = (uint32_t)pmm_alloc_page();
        if (!phys) goto err_cleanup;
        vmm_map_page(proc->page_dir, phys, user_stack_top - (si + 1) * PAGE_SIZE,
                     VMM_PRESENT | VMM_WRITABLE | VMM_USER);
    }
    proc->user_esp = user_stack_top;
    proc->eip = entry;

    registers_t *frame = (registers_t *)(proc->kernel_stack_top - sizeof(registers_t));
    frame->gs = 0x23;
    frame->fs = 0x23;
    frame->es = 0x23;
    frame->ds = 0x23;
    frame->edi = 0;
    frame->esi = 0;
    frame->ebp = 0;
    frame->esp = (uint32_t)&frame->int_no;
    frame->ebx = 0;
    frame->edx = 0;
    frame->ecx = 0;
    frame->eax = 0;
    frame->int_no = 0;
    frame->err_code = 0;
    frame->eip = entry;
    frame->cs = 0x1B;
    frame->eflags = 0x202;
    frame->useresp = proc->user_esp;
    frame->ss = 0x23;

    proc->kernel_esp = (uint32_t)frame;

    return proc;

err_load:
    kfree(phdrs);
err_cleanup:
    vmm_free_directory(proc->page_dir);
    pmm_free_pages(proc->kernel_stack, PROC_KSTACK_SIZE / PAGE_SIZE);
    proc->state = PROC_UNUSED;
    return NULL;
}

int process_exec(process_t *proc, const char *path, const char *args, registers_t *r, uint32_t cwd_inode) {
    file_t *file = fs_open(path, cwd_inode);
    if (!file) {
        return -1;
    }

    Elf32_Ehdr ehdr;
    fs_read(file, &ehdr, 0, sizeof(Elf32_Ehdr));

    uint32_t *magic = (uint32_t *)ehdr.e_ident;
    if (*magic != ELF_MAGIC || ehdr.e_ident[4] != ELF_32BIT ||
        ehdr.e_machine != EM_386 || ehdr.e_type != ET_EXEC) {
        return -1;
    }

    uint32_t phdr_bytes = ehdr.e_phnum * sizeof(Elf32_Phdr);
    Elf32_Phdr *phdrs = (Elf32_Phdr *)kmalloc(phdr_bytes);
    if (!phdrs) return -1;
    fs_read(file, phdrs, ehdr.e_phoff, phdr_bytes);
    vmm_clear_user_pages(proc->page_dir);

    uint32_t entry = ehdr.e_entry;

    for (uint32_t i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;

        uint32_t seg_start = phdrs[i].p_vaddr;
        uint32_t seg_end_file = seg_start + phdrs[i].p_filesz;
        uint32_t seg_end_mem = seg_start + phdrs[i].p_memsz;
        uint32_t page_start = seg_start & ~0xFFF;
        uint32_t page_end = (seg_end_mem + PAGE_SIZE - 1) & ~0xFFF;

        uint32_t page_flags = VMM_PRESENT | VMM_USER;
        if (phdrs[i].p_flags & PF_W) page_flags |= VMM_WRITABLE;

        for (uint32_t vaddr = page_start; vaddr < page_end; vaddr += PAGE_SIZE) {
            uint32_t phys = (uint32_t)pmm_alloc_page();
            if (!phys) { kfree(phdrs); return -1; }

            (void)vmm_map_page(proc->page_dir, phys, vaddr, page_flags);

            uint8_t *page = (uint8_t *)vmm_temp_map(phys);
            for (uint32_t j = 0; j < PAGE_SIZE; j++)
                page[j] = 0;

            uint32_t copy_start = vaddr > seg_start ? vaddr : seg_start;
            uint32_t copy_end = vaddr + PAGE_SIZE < seg_end_file
                                ? vaddr + PAGE_SIZE : seg_end_file;
            if (copy_start < copy_end) {
                uint32_t page_off = copy_start - vaddr;
                uint32_t file_off = phdrs[i].p_offset + (copy_start - seg_start);
                uint32_t copy_len = copy_end - copy_start;
                fs_read(file, page + page_off, file_off, copy_len);
            }

            vmm_temp_unmap();
        }
    }

    kfree(phdrs);

    uint32_t user_stack_top = 0xC0000000;
    uint32_t stack_top_phys = 0;
    for (int si = 0; si < USER_STACK_PAGES; si++) {
        uint32_t phys = (uint32_t)pmm_alloc_page();
        if (!phys) return -1;
        vmm_map_page(proc->page_dir, phys, user_stack_top - (si + 1) * PAGE_SIZE,
                     VMM_PRESENT | VMM_WRITABLE | VMM_USER);
        if (si == 0) stack_top_phys = phys;
    }

    uint8_t *stack_page = (uint8_t *)vmm_temp_map(stack_top_phys);
    stack_page[0] = '\0';
    if (args) {
        uint32_t i = 0;
        while (args[i] && i < PAGE_SIZE - 1) {
            stack_page[i] = args[i];
            i++;
        }
        stack_page[i] = '\0';
    }
    vmm_temp_unmap();

    r->eax = 0;
    r->ebx = 0;
    r->ecx = 0xBFFFF000;
    r->edx = 0;
    r->esi = 0;
    r->edi = 0;
    r->ebp = 0;
    r->esp = (uint32_t)&r->int_no;
    r->eip = entry;
    r->useresp = user_stack_top;

    proc->eip = entry;
    proc->user_esp = user_stack_top;
    proc->heap_break = USER_HEAP_START;
    proc->heap_mapped_end = USER_HEAP_START;
    proc->mmap_brk = USER_MMAP_START;
    for (int i = 0; i < FD_MAX; i++)
        fd_close(proc->fd_table, i);
    fd_init_table(proc->fd_table);
    signal_init_process(proc);

    return 0;
}

process_t *process_fork(process_t *parent, registers_t *r) {
    process_t *child = NULL;

    uint32_t flags;
    spin_lock_irqsave(&proc_lock, &flags);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROC_UNUSED) {
            child = &processes[i];
            break;
        }
    }
    if (!child) { spin_unlock_irqrestore(&proc_lock, flags); return NULL; }

    child->pid = next_pid++;
    child->state = PROC_READY;
    spin_unlock_irqrestore(&proc_lock, flags);
    child->time_slice = 5;
    child->sleep_until = 0;
    child->exit_code = 0;
    child->parent_pid = parent->pid;
    child->wait_child_pid = 0;
    child->cwd_inode = parent->cwd_inode;
    child->mmap_brk = parent->mmap_brk;
    child->uid = parent->uid;
    child->gid = parent->gid;
    child->euid = parent->euid;
    child->egid = parent->egid;
    for (int i = 0; i < 32; i++)
        child->sigactions[i] = parent->sigactions[i];
    child->signal_pending = 0;
    child->signal_blocked = parent->signal_blocked;
    child->eip = parent->eip;

    child->kernel_stack = (uint8_t *)pmm_alloc_pages(PROC_KSTACK_SIZE / PAGE_SIZE);
    if (!child->kernel_stack) {
        uint32_t f2; spin_lock_irqsave(&proc_lock, &f2);
        child->state = PROC_UNUSED;
        spin_unlock_irqrestore(&proc_lock, f2);
        return NULL;
    }
    child->kernel_stack_top = (uint32_t)child->kernel_stack + PROC_KSTACK_SIZE;

    fd_init_table(child->fd_table);
    for (int i = 0; i < FD_MAX; i++) {
        child->fd_table[i] = parent->fd_table[i];
        if (child->fd_table[i].type == FD_PIPE) {
            pipe_t *p = (pipe_t *)child->fd_table[i].file;
            p->refcount++;
        }
    }

    child->heap_break = parent->heap_break;
    child->heap_mapped_end = parent->heap_mapped_end;

    child->page_dir = vmm_create_directory();
    if (!child->page_dir) {
        pmm_free_pages(child->kernel_stack, PROC_KSTACK_SIZE / PAGE_SIZE);
        uint32_t f2; spin_lock_irqsave(&proc_lock, &f2);
        child->state = PROC_UNUSED;
        spin_unlock_irqrestore(&proc_lock, f2);
        return NULL;
    }

    vmm_fork_cow_pages(parent->page_dir, child->page_dir);

    registers_t *child_frame = (registers_t *)(child->kernel_stack_top - sizeof(registers_t));
    {
        uint32_t *dst = (uint32_t *)child_frame;
        uint32_t *src = (uint32_t *)r;
        for (uint32_t i = 0; i < sizeof(registers_t) / 4; i++)
            dst[i] = src[i];
    }
    child_frame->eax = 0;

    child->user_esp = r->useresp;
    child->kernel_esp = (uint32_t)child_frame;

    return child;
}

int process_waitpid(process_t *proc, int pid, uint32_t *status, int options) {
    if (!proc) return -1;

    uint32_t flags;
    spin_lock_irqsave(&proc_lock, &flags);

    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *child = &processes[i];
        if (child->state == PROC_UNUSED) continue;
        if ((int)child->parent_pid != (int)proc->pid) continue;
        if (pid != -1 && (int)child->pid != pid) continue;

        if (child->state == PROC_ZOMBIE) {
            if (status) *status = child->exit_code;
            child->state = PROC_UNUSED;
            spin_unlock_irqrestore(&proc_lock, flags);
            return child->pid;
        }

        if (options & 1) { spin_unlock_irqrestore(&proc_lock, flags); return 0; }
        proc->wait_child_pid = pid == -1 ? 0xFFFFFFFF : (uint32_t)pid;
        spin_unlock_irqrestore(&proc_lock, flags);
        return -2;
    }

    spin_unlock_irqrestore(&proc_lock, flags);
    return -1;
}

int process_kill(int pid) {
    uint32_t flags;
    spin_lock_irqsave(&proc_lock, &flags);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = &processes[i];
        if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE) continue;
        if ((int)p->pid != pid) continue;

        for (int f = 0; f < FD_MAX; f++)
            fd_close(p->fd_table, f);

        if (p->kernel_stack) {
            pmm_free_pages(p->kernel_stack, PROC_KSTACK_SIZE / PAGE_SIZE);
            p->kernel_stack = NULL;
        }

        if (p->page_dir) {
            if (p->page_dir == vmm_get_current_directory())
                vmm_switch_directory(vmm_get_kernel_directory());
            vmm_free_directory(p->page_dir);
            p->page_dir = NULL;
        }

        p->state = PROC_ZOMBIE;

        for (int j = 0; j < MAX_PROCESSES; j++) {
            process_t *parent = &processes[j];
            if (parent->state == PROC_UNUSED) continue;
            if (parent->pid != p->parent_pid) continue;
            if (parent->state == PROC_BLOCKED &&
                (parent->wait_child_pid == (uint32_t)p->pid || parent->wait_child_pid == 0xFFFFFFFF)) {
                parent->state = PROC_READY;
                parent->wait_child_pid = 0;
            }
            break;
        }

        scheduler_remove_process(p);
        spin_unlock_irqrestore(&proc_lock, flags);
        return 0;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    return -1;
}

void process_exit(process_t *proc) {
    if (!proc) return;
    for (int i = 0; i < FD_MAX; i++)
        fd_close(proc->fd_table, i);

    if (proc->kernel_stack) {
        pmm_free_pages(proc->kernel_stack, PROC_KSTACK_SIZE / PAGE_SIZE);
        proc->kernel_stack = NULL;
    }

    if (proc->page_dir) {
        vmm_switch_directory(vmm_get_kernel_directory());
        vmm_free_directory(proc->page_dir);
        proc->page_dir = NULL;
    }

    {
        uint32_t flags;
        spin_lock_irqsave(&proc_lock, &flags);
        proc->state = PROC_ZOMBIE;

        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t *p = &processes[i];
            if (p->state == PROC_UNUSED) continue;
            if (p->pid != proc->parent_pid) continue;
            if (p->state == PROC_BLOCKED &&
                (p->wait_child_pid == proc->pid || p->wait_child_pid == 0xFFFFFFFF)) {
                p->state = PROC_READY;
                p->wait_child_pid = 0;
            }
            break;
        }
        spin_unlock_irqrestore(&proc_lock, flags);
    }
}
