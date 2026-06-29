#ifndef PANIC_H
#define PANIC_H

#include "isr.h"

void panic(const char *reason, const registers_t *r);

#endif
