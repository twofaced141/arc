#ifndef OOM_H
#define OOM_H

#include <stdint.h>

int oom_kill_victim(void);
uint32_t oom_count_process_pages(void);
int oom_kill_pid(int pid);

#endif
