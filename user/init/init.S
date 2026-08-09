/* BSD syscall convention (amd64):                                    */
/*   rax = 1024 + syscall_num                                         */
/*   rdi = arg0, rsi = arg1, rdx = arg2                               */
/*   syscall instruction                                              */
/* SYS_WRITE  = 3   (rax = 1027)
 * SYS_EXIT   = 0   (rax = 1024) */
.section .text
.globl _start

_start:
    /* write(1, msg, 20) */
    mov $1027, %rax
    mov $1, %rdi
    lea msg(%rip), %rsi
    mov $20, %rdx
    syscall

    /* exit(0) */
    mov $1024, %rax
    xor %rdi, %rdi
    syscall

    /* Not reached — but if exit fails, loop forever */
1:  hlt
    jmp 1b

.section .rodata
msg:
    .ascii "init: pid 1 running\n"
