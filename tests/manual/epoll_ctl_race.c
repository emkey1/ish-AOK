// Concurrent epoll_ctl on one epoll instance, the Go netpoller's pattern: many
// threads each ADD a socket, then DEL it, as fast as they can, while others ADD
// a socket that is already registered (EEXIST) and MOD a registered one. The
// EEXIST and MOD checks (fs/poll.c poll_has_fd, poll_fd_is_exclusive) used to
// walk the registration list without poll->lock, so a sibling's ADD/DEL
// relinking an entry under the walk faulted the host -- on the device the app
// aborted (seen under a Go Cloudflare scanner on an iPad).
//
//   zig cc -target aarch64-linux-musl -O2 -o epoll_ctl_race epoll_ctl_race.c -lpthread
//   ./epoll_ctl_race [seconds]      prints "ok N ops" and exits 0; before the
//                                   fix the emulator itself died
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CHURNERS 12
#define CHECKERS 4

static int ep;
static int pinned[CHECKERS];
static atomic_long ops;
static atomic_int stop;
static atomic_int failed;

static void *churn(void *arg) {
    (void) arg;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        perror("socketpair");
        atomic_store(&failed, 1);
        return NULL;
    }
    struct epoll_event ev = {.events = EPOLLIN | EPOLLOUT | EPOLLET, .data.fd = sv[0]};
    while (!atomic_load(&stop)) {
        if (epoll_ctl(ep, EPOLL_CTL_ADD, sv[0], &ev) < 0 ||
                epoll_ctl(ep, EPOLL_CTL_DEL, sv[0], NULL) < 0) {
            perror("churn epoll_ctl");
            atomic_store(&failed, 1);
            break;
        }
        atomic_fetch_add(&ops, 2);
    }
    close(sv[0]);
    close(sv[1]);
    return NULL;
}

static void *check(void *arg) {
    int fd = pinned[(long) arg];
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd};
    while (!atomic_load(&stop)) {
        if (epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) == 0 || errno != EEXIST) {
            fprintf(stderr, "re-ADD of a registered fd: want EEXIST, got %s\n",
                    errno ? "an error other than EEXIST" : "success");
            atomic_store(&failed, 1);
            break;
        }
        if (epoll_ctl(ep, EPOLL_CTL_MOD, fd, &ev) < 0) {
            perror("check MOD");
            atomic_store(&failed, 1);
            break;
        }
        atomic_fetch_add(&ops, 2);
    }
    return NULL;
}

int main(int argc, char **argv) {
    int seconds = argc > 1 ? atoi(argv[1]) : 10;
    ep = epoll_create1(0);
    if (ep < 0) {
        perror("epoll_create1");
        return 1;
    }
    for (int i = 0; i < CHECKERS; i++) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
            perror("socketpair");
            return 1;
        }
        pinned[i] = sv[0];
        struct epoll_event ev = {.events = EPOLLIN, .data.fd = sv[0]};
        if (epoll_ctl(ep, EPOLL_CTL_ADD, sv[0], &ev) < 0) {
            perror("pin ADD");
            return 1;
        }
    }
    pthread_t t[CHURNERS + CHECKERS];
    for (long i = 0; i < CHURNERS; i++)
        pthread_create(&t[i], NULL, churn, NULL);
    for (long i = 0; i < CHECKERS; i++)
        pthread_create(&t[CHURNERS + i], NULL, check, (void *) i);
    struct timespec ts = {.tv_sec = seconds};
    nanosleep(&ts, NULL);
    atomic_store(&stop, 1);
    for (int i = 0; i < CHURNERS + CHECKERS; i++)
        pthread_join(t[i], NULL);
    if (atomic_load(&failed))
        return 1;
    printf("ok %ld ops\n", atomic_load(&ops));
    return 0;
}
