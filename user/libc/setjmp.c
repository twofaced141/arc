#include <setjmp.h>
#include <stddef.h>

int setjmp(jmp_buf env) {
    __asm__ __volatile__(
        "movl %%ebx, (%0)\n\t"
        "movl %%esi, 4(%0)\n\t"
        "movl %%edi, 8(%0)\n\t"
        "movl %%ebp, 12(%0)\n\t"
        "movl %%esp, 16(%0)\n\t"
        "movl (%%esp), %%eax\n\t"
        "movl %%eax, 20(%0)\n\t"
        : : "r"(env) : "eax", "memory");
    return 0;
}

void longjmp(jmp_buf env, int val) {
    __asm__ __volatile__(
        "movl (%0), %%ebx\n\t"
        "movl 4(%0), %%esi\n\t"
        "movl 8(%0), %%edi\n\t"
        "movl 12(%0), %%ebp\n\t"
        "movl 16(%0), %%esp\n\t"
        "movl 20(%0), %%eax\n\t"
        "movl %1, %%ecx\n\t"
        "testl %%ecx, %%ecx\n\t"
        "jnz 1f\n\t"
        "movl $1, %%ecx\n\t"
        "1:\n\t"
        "movl %%ecx, %%eax\n\t"
        "jmp *%%eax\n\t"
        : : "r"(env), "r"(val) : "eax", "ecx", "memory");
    __builtin_unreachable();
}

int sigsetjmp(sigjmp_buf env, int savesigs) {
    env->__mask_was_saved = savesigs;
    if (savesigs)
        sigprocmask(SIG_SETMASK, NULL, &env->__saved_mask);
    return setjmp(env->__jmpbuf);
}

void siglongjmp(sigjmp_buf env, int val) {
    if (env->__mask_was_saved)
        sigprocmask(SIG_SETMASK, &env->__saved_mask, NULL);
    longjmp(env->__jmpbuf, val);
}
