#include <unistd.h>
#include <stdlib.h>

int main(void) {
    int wm_pid = fork();
    if (wm_pid == 0) {
        char *argv[] = {"/bin/wm", NULL};
        execve("/bin/wm", argv, NULL);
        exit(1);
    }

    for (;;) {
        int pid = fork();
        if (pid < 0) continue;
        if (pid == 0) {
            char *argv[] = {"/bin/shell", NULL};
            execve("/bin/shell", argv, NULL);
            exit(1);
        }
        waitpid(pid, NULL, 0);
    }
}
