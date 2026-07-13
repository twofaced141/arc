#ifndef _SYS_UTSNAME_H
#define _SYS_UTSNAME_H

#define UTS_LENGTH 65

struct utsname {
    char sysname[UTS_LENGTH];
    char nodename[UTS_LENGTH];
    char release[UTS_LENGTH];
    char version[UTS_LENGTH];
    char machine[UTS_LENGTH];
    char domainname[UTS_LENGTH];
};

int uname(struct utsname *buf);

#endif
