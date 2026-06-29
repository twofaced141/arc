#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "vmm.h"
#include "fd.h"
#include "fs.h"
#include "isr.h"

#define PROC_UNUSED   0
#define PROC_READY    1
#define PROC_RUNNING  2
#define PROC_BLOCKED  3
#define PROC_ZOMBIE   4

#define MAX_PROCESSES    64
#define PROC_KSTACK_SIZE 8192
#define USER_HEAP_START  0x40000000

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
    uint32_t heap_break;
    uint32_t heap_mapped_end;
    uint32_t sleep_until;
    uint32_t exit_code;
    uint32_t parent_pid;
    uint32_t wait_child_pid;
    uint32_t cwd_inode;
} process_t;

void process_init(void);
process_t *process_create_user(uint32_t eip, const void *code, uint32_t code_size);
process_t *process_create_elf(file_t *file);
process_t *process_fork(process_t *parent, registers_t *r);
int process_exec(process_t *proc, const char *path, registers_t *r, uint32_t cwd_inode);
void process_exit(process_t *proc);
int process_waitpid(process_t *proc, int pid, uint32_t *status, int options);
int process_kill(int pid);

#endif
