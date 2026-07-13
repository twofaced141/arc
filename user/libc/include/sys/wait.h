#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/types.h>

#define WNOHANG     1
#define WUNTRACED   2
#define WCONTINUED  4
#define WEXITED     8
#define WSTOPPED    16
#define WNOWAIT     0x01000000

#define WEXITSTATUS(s)  (((s) & 0xFF00) >> 8)
#define WTERMSIG(s)     ((s) & 0x7F)
#define WSTOPSIG(s)     WEXITSTATUS(s)
#define WIFEXITED(s)    (WTERMSIG(s) == 0)
#define WIFSIGNALED(s)  (((s) & 0xFF) && ((s) & 0xFF) != 0x7F)
#define WIFSTOPPED(s)   (((s) & 0xFF) == 0x7F)
#define WIFCONTINUED(s) ((s) == 0xFFFF)

pid_t wait(int *status);
pid_t waitpid(pid_t pid, int *status, int options);
pid_t wait3(int *status, int options, void *rusage);
pid_t wait4(pid_t pid, int *status, int options, void *rusage);

#endif
