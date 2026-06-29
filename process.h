#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "vmm.h"

#define PROC_UNUSED   0
#define PROC_READY    1
#define PROC_RUNNING  2
#define PROC_BLOCKED  3
#define PROC_ZOMBIE   4

#define MAX_PROCESSES    64
#define PROC_KSTACK_SIZE 8192

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
} process_t;

void process_init(void);
process_t *process_create_user(uint32_t eip, const void *code, uint32_t code_size);
void process_exit(process_t *proc);

#endif
