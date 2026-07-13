#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

typedef unsigned int size_t;
typedef int ssize_t;
typedef int pid_t;
typedef int off_t;
typedef long long off64_t;
typedef unsigned int mode_t;
typedef unsigned int dev_t;
typedef unsigned int ino_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int nlink_t;
typedef unsigned int blksize_t;
typedef unsigned int blkcnt_t;
#ifndef __time_t_defined
#define __time_t_defined
typedef long time_t;
#endif
typedef long suseconds_t;
#ifndef __clock_t_defined
#define __clock_t_defined
typedef long clock_t;
#endif
typedef unsigned int socklen_t;
typedef unsigned long long rlim_t;
#endif
