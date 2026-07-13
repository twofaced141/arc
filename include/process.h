#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "vmm.h"
#include "fd.h"
#include "fs.h"
#include "isr.h"
#include "spinlock.h"
#include "signal.h"

#define PROC_UNUSED   0
#define PROC_READY    1
#define PROC_RUNNING  2
#define PROC_BLOCKED  3
#define PROC_ZOMBIE   4
#define PROC_STOPPED  5

#define MAX_PROCESSES    4096
#define PROC_KSTACK_SIZE   8192
#define USER_HEAP_START    0x40000000
#define USER_STACK_PAGES   32
#define USER_TLS_VADDR    0xBFFFB000
#define TLS_CANARY_OFFSET 0x14
#define FPU_STATE_SIZE 512

typedef struct process {
    uint32_t pid;
    uint32_t state;
    uint32_t kernel_esp;
    uint8_t *kernel_stack;
    uint32_t kernel_stack_top;
    page_directory_t *page_dir;
    uint32_t time_slice;
    uint32_t eip;
    uint32_t user_esp;
    fd_entry_t fd_table[FD_MAX];
    uint32_t heap_initial;
    uint32_t heap_break;
    uint32_t heap_mapped_end;
    uint32_t sleep_until;
    uint32_t exit_code;
    uint32_t parent_pid;
    uint32_t wait_child_pid;
    uint32_t cwd_inode;
    sigaction_t sigactions[32];
    uint32_t signal_pending;
    uint32_t signal_blocked;
    uint32_t mmap_brk;
    uint32_t alarm_ticks;
    uint32_t alarm_remaining;
    uint16_t uid;
    uint16_t gid;
    uint16_t euid;
    uint16_t egid;
    uint32_t umask;
    uint32_t nice;
    uint32_t pgid;
    uint32_t is_linux_syscall;
    char name[64];
    uint8_t fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
} process_t;

extern process_t *processes;
extern spinlock_t proc_lock;
void signal_init_process(process_t *proc);

void process_init(void);
process_t *process_create_user(uint32_t eip, const void *code, uint32_t code_size);
process_t *process_create_elf(file_t *file);
process_t *process_fork(process_t *parent, registers_t *r);
int process_exec(process_t *proc, const char *path, const char *args, registers_t *r, uint32_t cwd_inode);
void process_exit(process_t *proc);
int process_waitpid(process_t *proc, int pid, uint32_t *status, int options);
int process_kill(int pid);

#endif
