#ifndef _SETJMP_H
#define _SETJMP_H

#include <signal.h>

typedef int jmp_buf[6];

typedef struct {
    jmp_buf __jmpbuf;
    sigset_t __saved_mask;
    int __mask_was_saved;
} sigjmp_buf[1];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

int sigsetjmp(sigjmp_buf env, int savesigs);
void siglongjmp(sigjmp_buf env, int val);

#endif
