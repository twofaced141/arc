#include <stdlib.h>
#include <unistd.h>

extern int main(int argc, char **argv);

void _start(void) {
    const char *args = (const char *)0xBFFFF000;
    int argc = 0;
    char *argv[64];
    char *p = (char *)args;

    while (*p == ' ') p++;
    if (*p) {
        argv[argc++] = p;
        while (*p) {
            if (*p == ' ') {
                *p++ = '\0';
                while (*p == ' ') p++;
                if (*p && argc < 64)
                    argv[argc++] = p;
            } else {
                p++;
            }
        }
    }
    argv[argc] = NULL;

    exit(main(argc, argv));
}
