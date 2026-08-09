#ifndef BSD_SYSINFO_H
#define BSD_SYSINFO_H

#include <stdint.h>

/* struct sysinfo — sysinfo(2), Linux-compatible layout */
struct sysinfo {
    long    uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned short pad;
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int  mem_unit;
};

#endif
