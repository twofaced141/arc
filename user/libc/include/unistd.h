#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>
#include <stdint.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

int read(int fd, void *buf, size_t count);
int write(int fd, const void *buf, size_t count);
int open(const char *path, int flags, ...);
int close(int fd);
pid_t fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
pid_t waitpid(pid_t pid, int *status, int options);
unsigned int sleep(unsigned int seconds);
int brk(void *addr);
void *sbrk(intptr_t increment);
pid_t getpid(void);
char *getcwd(char *buf, size_t size);
int chdir(const char *path);
off_t lseek(int fd, off_t offset, int whence);
int dup2(int oldfd, int newfd);
int pipe(int pipefd[2]);
int ioctl(int fd, int request, void *arg);
int kill(pid_t pid, int sig);
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void *addr, size_t length);
int uname(void *buf);
int yield(void);
int getticks(void);
int getdents(const char *path, void *dirp, unsigned int count);
int unlink(const char *path);
int rename(const char *oldpath, const char *newpath);
int mkdir(const char *path, unsigned int mode);
int rmdir(const char *path);
int chmod(const char *path, unsigned int mode);
int chown(const char *path, unsigned int owner, unsigned int group);
int access(const char *path, int mode);
unsigned int getuid(void);
unsigned int getgid(void);
unsigned int geteuid(void);
unsigned int getegid(void);

struct rtc_time {
    unsigned short year;
    unsigned char  month;
    unsigned char  day;
    unsigned char  hour;
    unsigned char  minute;
    unsigned char  second;
};
int gettime(struct rtc_time *t);

#endif
