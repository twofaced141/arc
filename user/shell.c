#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#define HISTORY_SIZE 16

static int streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char input[256];
    char history[HISTORY_SIZE][256];
    int history_count = 0;
    int history_pos = -1;

    for (;;) {
        printf("# ");

        int pos = 0;
        input[0] = '\0';
        history_pos = -1;
        char saved_input[256];
        saved_input[0] = '\0';

        for (;;) {
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

        if (streq(cmd, "exit")) {
            return 0;
        }

        if (streq(cmd, "sleep")) {
            if (*args_start) {
                unsigned int secs = 0;
                while (*args_start >= '0' && *args_start <= '9') {
                    secs = secs * 10 + (*args_start - '0');
                    args_start++;
                }
                if (secs > 0) sleep(secs);
            }
            continue;
        }

        if (streq(cmd, "cd")) {
            if (*args_start) {
                if (chdir(args_start) < 0)
                    printf("cd: %s: no such directory\n", args_start);
            }
            continue;
        }

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
            continue;
        }
        close(fd);

        int pid = fork();
        if (pid < 0) continue;
        if (pid == 0) {
            execve(path, argv_child, NULL);
            exit(1);
        }
        waitpid(pid, NULL, 0);
    }
}
