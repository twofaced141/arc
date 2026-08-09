#ifndef BSD_UTSNAME_H
#define BSD_UTSNAME_H

/* struct utsname — uname(2) (POSIX sys/utsname.h layout) */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
#ifdef __x86_64__
    char domainname[65];
#endif
};

#endif
