#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>

#define HISTORY_SIZE 16

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void exec_cmd_line(const char *line) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return;

    char cmd[64];
    const char *cmd_start = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    int cmd_len = p - cmd_start;
    if (cmd_len > 63) cmd_len = 63;
    int i;
    for (i = 0; i < cmd_len; i++) cmd[i] = cmd_start[i];
    cmd[i] = '\0';

    while (*p == ' ' || *p == '\t') p++;
    const char *args_start = p;

    char path[64];
    int has_slash = 0;
    for (i = 0; cmd[i]; i++)
        if (cmd[i] == '/') { has_slash = 1; break; }

    if (has_slash) {
        for (i = 0; cmd[i] && i < 63; i++) path[i] = cmd[i];
        path[i] = '\0';
    } else {
        i = 0;
        const char *prefix = "/bin/";
        while (*prefix && i < 63) path[i++] = *prefix++;
        int j = 0;
        while (cmd[j] && i < 63) path[i++] = cmd[j++];
        path[i] = '\0';
    }

    char *argv_child[16];
    int ac = 0;
    char argbuf[256];
    int bi = 0;

    for (int k = 0; cmd[k]; k++) argbuf[bi++] = cmd[k];
    argbuf[bi++] = '\0';
    argv_child[ac++] = argbuf + (bi - 1 - (int)strlen(cmd));

    const char *ap = args_start;
    while (*ap && ac < 15) {
        while (*ap == ' ') ap++;
        if (!*ap) break;
        argv_child[ac++] = argbuf + bi;
        while (*ap && *ap != ' ') argbuf[bi++] = *ap++;
        argbuf[bi++] = '\0';
    }
    argv_child[ac] = NULL;

    int fd = open(path, 0);
    if (fd < 0) {
        printf("%s: not found\n", cmd);
        exit(1);
    }
    close(fd);

    execve(path, argv_child, NULL);
    exit(1);
}

static volatile int sigint_received;

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IONBF, 0);
    char input[256];
    char history[HISTORY_SIZE][256];
    int history_count = 0;
    int history_pos = -1;

    /* Ignore SIGINT in the shell itself */
    struct sigaction sa, sa_dfl;
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sa_dfl.sa_handler = SIG_DFL;
    sa_dfl.sa_flags = 0;

    /* Put TTY in raw mode so we handle echo, editing, and escape sequences */
    struct termios raw, canon;
    tcgetattr(0, &canon);
    raw = canon;
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    tcsetattr(0, TCSANOW, &raw);

    /* Become foreground process group */
    setpgid(0, 0);
    tcsetpgrp(1, getpid());

    for (;;) {
        printf("# ");

        int pos = 0;
        input[0] = '\0';
        history_pos = -1;
        char saved_input[256];
        saved_input[0] = '\0';

        for (;;) {
            if (sigint_received) {
                sigint_received = 0;
                if (pos > 0) {
                    printf("^C\n");
                    pos = 0;
                    input[0] = '\0';
                }
                printf("# ");
            }

            char c = getchar();

            if (c == '\n') {
                putchar('\n');
                break;
            }

            if (c == '\b' || c == 0x7F) {
                if (pos > 0) {
                    pos--;
                    printf("\b \b");
                }
                continue;
            }

            if (c == 0x03) {
                if (pos > 0) {
                    printf("^C\n");
                    pos = 0;
                    input[0] = '\0';
                    printf("# ");
                }
                continue;
            }

            if (c == 0x1B) {
                char bracket = getchar();
                char arrow = getchar();
                if (bracket != '[') continue;

                if (arrow == 'A' && history_count > 0) {
                    if (history_pos == -1) {
                        strncpy(saved_input, input, 256);
                        saved_input[255] = '\0';
                        history_pos = history_count - 1;
                    } else if (history_pos > 0) {
                        history_pos--;
                    } else {
                        continue;
                    }
                    int idx = history_pos % HISTORY_SIZE;
                    int new_len = strlen(history[idx]);
                    if (new_len > 254) new_len = 254;
                    printf("\r# ");
                    for (int i = 0; i < pos; i++) printf(" ");
                    printf("\r# ");
                    for (int i = 0; i < new_len; i++) {
                        putchar(history[idx][i]);
                        input[i] = history[idx][i];
                    }
                    input[new_len] = '\0';
                    pos = new_len;
                } else if (arrow == 'B') {
                    if (history_pos == -1) continue;
                    history_pos++;
                    if (history_pos >= history_count) {
                        history_pos = -1;
                        int new_len = strlen(saved_input);
                        if (new_len > 254) new_len = 254;
                        printf("\r# ");
                        for (int i = 0; i < pos; i++) printf(" ");
                        printf("\r# ");
                        for (int i = 0; i < new_len; i++) {
                            putchar(saved_input[i]);
                            input[i] = saved_input[i];
                        }
                        input[new_len] = '\0';
                        pos = new_len;
                    } else {
                        int idx = history_pos % HISTORY_SIZE;
                        int new_len = strlen(history[idx]);
                        if (new_len > 254) new_len = 254;
                        printf("\r# ");
                        for (int i = 0; i < pos; i++) printf(" ");
                        printf("\r# ");
                        for (int i = 0; i < new_len; i++) {
                            putchar(history[idx][i]);
                            input[i] = history[idx][i];
                        }
                        input[new_len] = '\0';
                        pos = new_len;
                    }
                }
                continue;
            }

            if (pos < 254) {
                putchar(c);
                input[pos++] = c;
                input[pos] = '\0';
            }
        }

        const char *p = input;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') continue;

        if (history_count == 0 || !streq(history[(history_count - 1) % HISTORY_SIZE], input)) {
            strncpy(history[history_count % HISTORY_SIZE], input, 256);
            history[history_count % HISTORY_SIZE][255] = '\0';
            history_count++;
        }

        /* Handle builtins directly (only if no pipe/redirect) */
        int has_pipe_redir = 0;
        for (const char *sp = p; *sp; sp++) {
            if (*sp == '|' || *sp == '>') { has_pipe_redir = 1; break; }
        }
        if (!has_pipe_redir) {
            char builtin_cmd[64];
            const char *bp = p;
            while (*bp && *bp != ' ' && *bp != '\t') bp++;
            int bcl = bp - p;
            if (bcl > 63) bcl = 63;
            for (int i = 0; i < bcl; i++) builtin_cmd[i] = p[i];
            builtin_cmd[bcl] = '\0';

            if (streq(builtin_cmd, "exit")) return 0;

            if (streq(builtin_cmd, "cd")) {
                while (*bp == ' ' || *bp == '\t') bp++;
                if (*bp) {
                    char dir[256];
                    int di = 0;
                    while (*bp && *bp != ' ' && *bp != '\t' && di < 255)
                        dir[di++] = *bp++;
                    dir[di] = '\0';
                    if (chdir(dir) < 0)
                        printf("cd: %s: no such directory\n", dir);
                }
                continue;
            }
        }

        /* Find pipe '|' and redirect '>' */
        char *pipe_pos = NULL;
        char *redir_pos = NULL;
        for (int i = 0; input[i]; i++) {
            if (input[i] == '|') pipe_pos = &input[i];
            if (input[i] == '>') redir_pos = &input[i];
        }

        if (pipe_pos && (!redir_pos || pipe_pos < redir_pos)) {
            /* cmd1 | cmd2  or  cmd1 | cmd2 > outfile */
            *pipe_pos = '\0';
            char *right = pipe_pos + 1;
            while (*right == ' ') right++;

            char *outfile = NULL;
            for (int i = 0; right[i]; i++) {
                if (right[i] == '>') {
                    right[i] = '\0';
                    outfile = &right[i] + 1;
                    while (*outfile == ' ') outfile++;
                    break;
                }
            }

            int pipefds[2];
            if (pipe(pipefds) < 0) continue;

            tcsetattr(0, TCSANOW, &canon);
            int pid1 = fork();
            if (pid1 < 0) continue;
            if (pid1 == 0) {
                setpgid(0, 0);
                close(pipefds[0]);
                dup2(pipefds[1], 1);
                close(pipefds[1]);
                sigaction(SIGINT, &sa_dfl, NULL);
                exec_cmd_line(p);
                exit(1);
            }

            int pid2 = fork();
            if (pid2 < 0) continue;
            if (pid2 == 0) {
                setpgid(0, pid1);
                close(pipefds[1]);
                dup2(pipefds[0], 0);
                close(pipefds[0]);
                if (outfile) {
                    truncate(outfile, 0);
                    int fd = open(outfile, O_WRONLY);
                    if (fd < 0) fd = open(outfile, O_CREAT | O_WRONLY);
                    if (fd >= 0) {
                        dup2(fd, 1);
                        close(fd);
                    }
                }
                sigaction(SIGINT, &sa_dfl, NULL);
                exec_cmd_line(right);
                exit(1);
            }

            close(pipefds[0]);
            close(pipefds[1]);
            tcsetpgrp(1, pid1);
            waitpid(pid1, NULL, 0);
            waitpid(pid2, NULL, 0);
            tcsetpgrp(1, getpid());
            tcsetattr(0, TCSANOW, &raw);

        } else if (redir_pos) {
            /* cmd > outfile */
            *redir_pos = '\0';
            char *outfile = redir_pos + 1;
            while (*outfile == ' ') outfile++;

            tcsetattr(0, TCSANOW, &canon);
            int pid = fork();
            if (pid < 0) continue;
            if (pid == 0) {
                setpgid(0, 0);
                truncate(outfile, 0);
                int fd = open(outfile, O_WRONLY);
                if (fd < 0) fd = open(outfile, O_CREAT | O_WRONLY);
                if (fd >= 0) {
                    dup2(fd, 1);
                    close(fd);
                }
                sigaction(SIGINT, &sa_dfl, NULL);
                exec_cmd_line(p);
                exit(1);
            }
            tcsetpgrp(1, pid);
            waitpid(pid, NULL, 0);
            tcsetpgrp(1, getpid());
            tcsetattr(0, TCSANOW, &raw);

        } else {
            tcsetattr(0, TCSANOW, &canon);
            int pid = fork();
            if (pid < 0) continue;
            if (pid == 0) {
                setpgid(0, 0);
                sigaction(SIGINT, &sa_dfl, NULL);
                exec_cmd_line(p);
                exit(1);
            }
            tcsetpgrp(1, pid);
            waitpid(pid, NULL, 0);
            tcsetpgrp(1, getpid());
            tcsetattr(0, TCSANOW, &raw);
        }
    }
}
