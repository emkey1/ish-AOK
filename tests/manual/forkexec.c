// fork, exec the command given in the child, wait for it in the parent: the
// smallest fork, exec and wait round trip.
#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    int pid = fork();
    if (pid < 0) {
        perror("fork");
        abort();
    }
    if (pid == 0) {
        // child
        if (execv(argv[1], argv + 1) < 0) {
            perror("exec");
            abort();
        }
    } else {
        // parent
        if (waitpid(pid, NULL, 0) != pid) {
            perror("wait");
            abort();
        }
    }
}
