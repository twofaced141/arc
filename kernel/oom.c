#include "oom.h"
#include "process.h"
#include "pmm.h"
#include "vmm.h"
#include "debug.h"
#include "scheduler.h"

static uint32_t count_user_pages(process_t *proc) {
    if (!proc->page_dir) return 0;
    uint32_t count = 0;
    for (int i = 0; i < 768; i++) {
        uint32_t pde = proc->page_dir->entries[i];
        if (!(pde & VMM_PRESENT)) continue;
        page_table_t *table = (page_table_t *)(pde & ~0xFFF);
        for (int j = 0; j < 1024; j++) {
            if (table->entries[j] & VMM_PRESENT)
                count++;
        }
    }
    return count;
}

int oom_kill_victim(void) {
    int best_idx = -1;
    uint32_t best_score = 0;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = &processes[i];
        if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE)
            continue;
        if (p->pid <= 1)
            continue;

        uint32_t pages = count_user_pages(p);
        uint32_t score = pages + (p->heap_break - p->heap_initial) / PAGE_SIZE;

        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }

    if (best_idx < 0)
        return -1;

    process_t *victim = &processes[best_idx];
    debug_printf("oom: killing pid=%d name=%s pages=%d\r\n",
                 victim->pid, victim->name, best_score);

    process_kill(victim->pid);
    return 0;
}

int oom_kill_pid(int pid) {
    uint32_t flags;
    spin_lock_irqsave(&proc_lock, &flags);
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = &processes[i];
        if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE) continue;
        if ((int)p->pid != pid) continue;
        spin_unlock_irqrestore(&proc_lock, flags);

        debug_printf("oom: manual kill pid=%d name=%s\r\n", p->pid, p->name);
        process_kill(p->pid);
        return 0;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    return -1;
}

uint32_t oom_count_process_pages(void) {
    uint32_t total = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = &processes[i];
        if (p->state == PROC_UNUSED || p->state == PROC_ZOMBIE)
            continue;
        total += count_user_pages(p);
    }
    return total;
}
