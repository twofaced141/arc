#ifndef ISR_H
#define ISR_H

#include <stdint.h>

#define SYSCALL_GETPID   0u
#define SYSCALL_PUTC     1u
#define SYSCALL_YIELD    2u
#define SYSCALL_EXIT     3u
#define SYSCALL_WRITE    4u
#define SYSCALL_READ     5u
#define SYSCALL_SLEEP    6u
#define SYSCALL_GETTICKS 7u
#define SYSCALL_OPEN     8u
#define SYSCALL_CLOSE    9u
#define SYSCALL_LSEEK    10u
#define SYSCALL_FSTAT    11u
#define SYSCALL_BRK      12u
#define SYSCALL_SBRK     13u
#define SYSCALL_FORK     14u
#define SYSCALL_EXECVE   15u
#define SYSCALL_WAITPID  16u
#define SYSCALL_CHDIR    17u
#define SYSCALL_GETCWD   18u
#define SYSCALL_LISTDIR  19u
#define SYSCALL_KILL     20u
#define SYSCALL_DUP2     21u
#define SYSCALL_PIPE     22u
#define SYSCALL_IOCTL    23u
#define SYSCALL_GETTIME  24u
#define SYSCALL_SIGACTION 25u
#define SYSCALL_SIGRETURN 26u
#define SYSCALL_KMALLOC_TEST 27u
#define SYSCALL_UNAME    28u
#define SYSCALL_MMAP     29u
#define SYSCALL_MUNMAP   30u
#define SYSCALL_STAT     31u
#define SYSCALL_LSTAT    32u
#define SYSCALL_GETDENTS 33u
#define SYSCALL_UNLINK   34u
#define SYSCALL_RENAME   35u
#define SYSCALL_MKDIR    36u
#define SYSCALL_RMDIR    37u
#define SYSCALL_CHMOD    38u
#define SYSCALL_CHOWN    39u
#define SYSCALL_ACCESS   40u
#define SYSCALL_GETUID   41u
#define SYSCALL_GETGID   42u
#define SYSCALL_GETEUID  43u
#define SYSCALL_GETEGID  44u
#define SYSCALL_SIGPROCMASK 45u
#define SYSCALL_SIGPENDING  46u
#define SYSCALL_SIGSUSPEND  47u
#define SYSCALL_READLINK   48u
#define SYSCALL_LINK       49u
#define SYSCALL_TRUNCATE   50u
#define SYSCALL_FTRUNCATE  51u
#define SYSCALL_ALARM      52u
#define SYSCALL_SYMLINK    53u
#define SYSCALL_FCHDIR     54u
#define SYSCALL_FCHMOD     55u
#define SYSCALL_FCHOWN     56u
#define SYSCALL_DUP        57u
#define SYSCALL_SETUID     58u
#define SYSCALL_SETGID     59u
#define SYSCALL_SETEUID    60u
#define SYSCALL_SETEGID    61u
#define SYSCALL_GETPPID    62u
#define SYSCALL_PAUSE      63u
#define SYSCALL_FSYNC      64u
#define SYSCALL_FDATASYNC  65u
#define SYSCALL_NICE       66u
#define SYSCALL_GETPRIORITY 67u
#define SYSCALL_SETPRIORITY 68u
#define SYSCALL_UTIMENSAT  69u
#define SYSCALL_UMASK      70u
#define SYSCALL_REBOOT      71u
#define SYSCALL_SETHOSTNAME 72u
#define SYSCALL_GETHOSTNAME 73u
#define SYSCALL_SETPGID     74u
#define SYSCALL_GETPGID     75u
#define SYSCALL_TCSETPGRP   76u
#define SYSCALL_TCGETPGRP   77u
#define SYSCALL_POWEROFF    78u
#define SYSCALL_SOCKET      79u
#define SYSCALL_BIND        80u
#define SYSCALL_CONNECT     81u
#define SYSCALL_LISTEN      82u
#define SYSCALL_ACCEPT      83u
#define SYSCALL_SEND        84u
#define SYSCALL_RECV        85u
#define SYSCALL_MOUNT       86u
#define SYSCALL_GETFBINFO   87u
#define SYSCALL_GETMOUSE    88u
#define SYSCALL_OOM_KILL    89u
#define SYSCALL_MAP_FB      90u
#define SYSCALL_FB_SET_ACTIVE 91u

typedef struct {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} registers_t;

typedef void (*isr_t)(registers_t *);

void isr_init(void);
void register_interrupt_handler(uint8_t n, isr_t handler);

#endif
