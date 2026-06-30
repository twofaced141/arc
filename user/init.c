#include <unistd.h>
#include <stdlib.h>

int main(void) {
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
