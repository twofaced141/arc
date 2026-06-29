.section .text
.globl _start

.equ SYSCALL_GETPID, 0
.equ SYSCALL_PUTC,   1
.equ SYSCALL_YIELD,  2
.equ SYSCALL_EXIT,   3
.equ SYSCALL_WRITE,  4
.equ SYSCALL_READ,   5
.equ SYSCALL_FORK,   14
.equ SYSCALL_EXECVE, 15
.equ SYSCALL_WAITPID,16
.equ SYSCALL_CHDIR,  17
.equ SYSCALL_GETCWD, 18
.equ SYSCALL_LISTDIR,19
.equ SYSCALL_OPEN,   8
.equ SYSCALL_CLOSE,  9
.equ SYSCALL_KILL,   20

.equ STDIN,  0
.equ STDOUT, 1

_start:
    movl $input_buf, %ebp

shell_loop:
    movl $prompt, %ecx
    movl $2, %edx
    movl $SYSCALL_WRITE, %eax
    movl $STDOUT, %ebx
    int $0x80

    movl %ebp, %edi
    xorl %ecx, %ecx

read_loop:
    pushl %ecx
    pushl %edi
    call read_char
    popl %edi
    popl %ecx
    movb %al, %dl
    cmpb $'\n', %dl
    je read_done
    cmpb $'\b', %dl
    je backspace
    cmpb $0x7F, %dl
    je backspace

    movzbl %dl, %ebx
    movl $SYSCALL_PUTC, %eax
    int $0x80

    movb %dl, (%edi)
    incl %edi
    incl %ecx
    cmpl $255, %ecx
    jl read_loop
    jmp read_done

backspace:
    cmpl $0, %ecx
    je read_loop
    decl %edi
    decl %ecx
    movl $SYSCALL_PUTC, %eax
    movl $8, %ebx
    int $0x80
    movl $SYSCALL_PUTC, %eax
    movl $' ', %ebx
    int $0x80
    movl $SYSCALL_PUTC, %eax
    movl $8, %ebx
    int $0x80
    jmp read_loop

read_done:
    movl $SYSCALL_PUTC, %eax
    movl $10, %ebx
    int $0x80

    movb $0, (%edi)

    cmpb $0, (%ebp)
    je shell_loop

    movl %ebp, %esi

check_exit:
    leal cmd_exit, %edi
    call strcmp
    cmpl $0, %eax
    jne check_pwd
    xorl %ebx, %ebx
    movl $SYSCALL_EXIT, %eax
    int $0x80

check_pwd:
    leal cmd_pwd, %edi
    call strcmp
    cmpl $0, %eax
    jne check_cd
    movl $SYSCALL_GETCWD, %eax
    leal pwd_buf, %ebx
    movl $256, %ecx
    int $0x80
    cmpl $0, %eax
    jl shell_loop
    movl %eax, %edx
    movl $SYSCALL_WRITE, %eax
    movl $STDOUT, %ebx
    leal pwd_buf, %ecx
    int $0x80
    movl $SYSCALL_PUTC, %eax
    movl $10, %ebx
    int $0x80
    jmp shell_loop

check_cd:
    leal cmd_cd, %edi
    call strcmp
    cmpl $0, %eax
    jne check_ls
    call skip_arg
    cmpb $0, (%esi)
    je shell_loop
    movl $SYSCALL_CHDIR, %eax
    movl %esi, %ebx
    int $0x80
    jmp shell_loop

check_ls:
    leal cmd_ls, %edi
    call strcmp
    cmpl $0, %eax
    jne check_getpid
    call skip_arg
    cmpb $0, (%esi)
    jne ls_with_arg
    xorl %ebx, %ebx
    jmp do_listdir
ls_with_arg:
    movl %esi, %ebx
do_listdir:
    movl $SYSCALL_LISTDIR, %eax
    leal ls_buf, %ecx
    movl $1024, %edx
    int $0x80
    cmpl $0, %eax
    jl shell_loop
    movl %eax, %edx
    movl $SYSCALL_WRITE, %eax
    movl $STDOUT, %ebx
    leal ls_buf, %ecx
    int $0x80
    jmp shell_loop

check_getpid:
    leal cmd_getpid, %edi
    call strcmp
    cmpl $0, %eax
    jne check_kill
    movl $SYSCALL_GETPID, %eax
    int $0x80
    movl %eax, %ebx
    call print_uint32
    movl $SYSCALL_PUTC, %eax
    movl $10, %ebx
    int $0x80
    jmp shell_loop

check_kill:
    leal cmd_kill, %edi
    call strcmp
    cmpl $0, %eax
    jne check_cat
    call skip_arg
    cmpb $0, (%esi)
    je shell_loop
    call parse_uint32
    movl %eax, %ebx
    movl $SYSCALL_KILL, %eax
    int $0x80
    jmp shell_loop

check_cat:
    leal cmd_cat, %edi
    call strcmp
    cmpl $0, %eax
    jne try_exec
    call skip_arg
    cmpb $0, (%esi)
    je shell_loop
    movl $SYSCALL_OPEN, %eax
    movl %esi, %ebx
    xorl %ecx, %ecx
    int $0x80
    cmpl $0, %eax
    jl cat_err
    movl %eax, %edi
cat_loop:
    movl $SYSCALL_READ, %eax
    movl %edi, %ebx
    leal ls_buf, %ecx
    movl $1024, %edx
    int $0x80
    cmpl $0, %eax
    jle cat_done
    movl %eax, %edx
    movl $SYSCALL_WRITE, %eax
    movl $STDOUT, %ebx
    leal ls_buf, %ecx
    int $0x80
    jmp cat_loop
cat_done:
    movl $SYSCALL_CLOSE, %eax
    movl %edi, %ebx
    int $0x80
    movl $SYSCALL_PUTC, %eax
    movl $10, %ebx
    int $0x80
    jmp shell_loop
cat_err:
    movl $err_cat, %ecx
    movl $15, %edx
    movl $SYSCALL_WRITE, %eax
    movl $STDOUT, %ebx
    int $0x80
    jmp shell_loop

try_exec:
    movl %ebp, %esi
find_word_end:
    cmpb $0, (%esi)
    je have_word
    cmpb $' ', (%esi)
    je split_done
    cmpb $'\t', (%esi)
    je split_done
    incl %esi
    jmp find_word_end
split_done:
    movb $0, (%esi)
have_word:
    movl $SYSCALL_FORK, %eax
    int $0x80
    cmpl $0, %eax
    jne parent_wait

    movl $SYSCALL_EXECVE, %eax
    movl %ebp, %ebx
    int $0x80

    movl $err_exec, %ecx
    movl $6, %edx
    movl $SYSCALL_WRITE, %eax
    movl $STDOUT, %ebx
    int $0x80
    movl $1, %ebx
    movl $SYSCALL_EXIT, %eax
    int $0x80

parent_wait:
    cmpl $-1, %eax
    je shell_loop
    movl %eax, %ebx
    xorl %ecx, %ecx
    xorl %edx, %edx
    movl $SYSCALL_WAITPID, %eax
    int $0x80
    jmp shell_loop

read_char:
    movl $SYSCALL_READ, %eax
    movl $STDIN, %ebx
    movl $char_buf, %ecx
    movl $1, %edx
    int $0x80
    cmpl $0, %eax
    jg read_char_done
    movl $SYSCALL_YIELD, %eax
    int $0x80
    jmp read_char
read_char_done:
    movb char_buf, %al
    ret

skip_arg:
    cmpb $0, (%esi)
    je skip_arg_done
    cmpb $' ', (%esi)
    je skip_arg_found
    cmpb $'\t', (%esi)
    je skip_arg_found
    incl %esi
    jmp skip_arg
skip_arg_found:
    incl %esi
skip_arg_done:
    ret

strcmp:
    pushl %esi
    pushl %edi
strcmp_loop:
    movb (%edi), %cl
    cmpb $0, %cl
    je strcmp_done
    movb (%esi), %dl
    cmpb $' ', %dl
    je strcmp_done
    cmpb $0, %dl
    je strcmp_done
    cmpb %cl, %dl
    jne strcmp_nomatch
    incl %esi
    incl %edi
    jmp strcmp_loop
strcmp_done:
    movb (%edi), %cl
    cmpb $0, %cl
    jne strcmp_nomatch
    popl %edi
    popl %esi
    xorl %eax, %eax
    ret
strcmp_nomatch:
    popl %edi
    popl %esi
    movl $1, %eax
    ret

print_uint32:
    pushl %ebp
    leal num_buf + 10, %ebp
    movb $0, (%ebp)
    decl %ebp
    movl $10, %ecx
    movl %ebx, %eax
print_loop:
    xorl %edx, %edx
    divl %ecx
    addb $'0', %dl
    movb %dl, (%ebp)
    decl %ebp
    cmpl $0, %eax
    jne print_loop
    incl %ebp
    movl %ebp, %ecx
    leal num_buf + 10, %edx
    subl %ebp, %edx
    movl $SYSCALL_WRITE, %eax
    movl $STDOUT, %ebx
    int $0x80
    popl %ebp
    ret

parse_uint32:
    xorl %eax, %eax
    xorl %edx, %edx
parse_loop:
    movb (%esi), %dl
    cmpb $0, %dl
    je parse_done
    cmpb $' ', %dl
    je parse_done
    cmpb $'\t', %dl
    je parse_done
    imull $10, %eax
    subb $'0', %dl
    addl %edx, %eax
    incl %esi
    jmp parse_loop
parse_done:
    ret

.section .data
prompt:  .asciz "$ "
cmd_exit: .asciz "exit"
cmd_pwd:  .asciz "pwd"
cmd_cd:   .asciz "cd"
cmd_ls:   .asciz "ls"
cmd_getpid: .asciz "getpid"
cmd_kill: .asciz "kill"
cmd_cat:  .asciz "cat"
err_exec: .asciz "error\n"
err_cat:  .asciz "cat: not found\n"

.section .bss
char_buf: .skip 1
input_buf: .skip 256
pwd_buf:  .skip 256
ls_buf:   .skip 1024
num_buf:  .skip 12
