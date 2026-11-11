// checkpoint_overlap.c -- helper for checkpoint_overlap.sh: memory an image has to
// carry intact, while threads and processes die as fast as they can, so that a save
// walking tasks the machine has not really stopped finds them half gone. Prints OK
// while the memory is what it was written as, and exits when the file named by its
// second argument appears. The third is how many megabytes to hold.
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHURNERS 4

static void *noop(void *arg) { return arg; }

static void *churn(void *arg) {
    for (;;) {
        pthread_t t[4];
        int n = 0;
        for (int i = 0; i < 4; i++)
            if (pthread_create(&t[n], NULL, noop, NULL) == 0)
                n++;
        for (int i = 0; i < n; i++)
            pthread_join(t[i], NULL);
        pid_t p = fork();
        if (p == 0)
            _exit(0);
        if (p > 0)
            waitpid(p, NULL, 0);
    }
    return arg;
}

static uint32_t pattern(size_t i) { return (uint32_t) (i * 2654435761u); }

int main(int argc, char **argv) {
    if (argc < 4)
        return 2;
    size_t ballast_bytes = (size_t) atoi(argv[3]) * 1024 * 1024;
    setvbuf(stdout, NULL, _IOLBF, 0);
    uint32_t *ballast = mmap(NULL, ballast_bytes, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ballast == MAP_FAILED)
        return 2;
    size_t words = ballast_bytes / sizeof(*ballast);
    for (size_t i = 0; i < words; i++)
        ballast[i] = pattern(i);
    for (int i = 0; i < CHURNERS; i++) {
        pthread_t t;
        pthread_create(&t, NULL, churn, NULL);
    }
    usleep(200000);
    close(open(argv[1], O_CREAT | O_WRONLY, 0644));
    for (unsigned round = 0; access(argv[2], F_OK) != 0; round++) {
        size_t bad = 0;
        for (size_t i = 0; i < words; i += 1021)
            bad += ballast[i] != pattern(i);
        if (bad != 0) {
            printf("CORRUPT %zu\n", bad);
            return 1;
        }
        printf("OK %u\n", round);
        sleep(1);
    }
    printf("DONE\n");
    _exit(0);
}
