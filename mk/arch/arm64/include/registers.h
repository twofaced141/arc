#ifndef REGISTERS_H
#define REGISTERS_H

#include <stdint.h>

typedef struct {
    uint64_t x[30];
    uint64_t lr;
    uint64_t spsr;
    uint64_t elr;
    uint64_t esr;
    uint64_t far;
    uint64_t sp;
} registers_t;

#endif
