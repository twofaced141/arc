#ifndef KALLSYMS_H
#define KALLSYMS_H

#include <stdint.h>

typedef struct {
    uint32_t addr;
    const char *name;
} ksym_t;

const char *kallsyms_lookup(uint32_t addr);

#endif
