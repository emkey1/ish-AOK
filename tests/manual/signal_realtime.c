// Real-time signals queued with sigqueue arrive in order, each with its value.
#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static void test_rt_sigqueue_fifo(void) {
    sigset_t set;
    sigset_t oldset;
    struct timespec timeout = {.tv_sec = 1, .tv_nsec = 0};
    siginfo_t info;
    int rt = SIGRTMIN;
    int values[3] = {0x1234, 0x2345, 0x3456};

    sigemptyset(&set);
    sigaddset(&set, rt);
    if (pthread_sigmask(SIG_BLOCK, &set, &oldset) != 0) {
        perror("pthread_sigmask");
        exit(1);
    }

    for (unsigned i = 0; i < 3; i++) {
        union sigval val;
        val.sival_int = values[i];
        if (sigqueue(getpid(), rt, val) != 0) {
            perror("sigqueue");
            exit(1);
        }
    }

    for (unsigned i = 0; i < 3; i++) {
        memset(&info, 0, sizeof(info));
        errno = 0;
        int sig = sigtimedwait(&set, &info, &timeout);
        test_logf("signal realtime: idx=%u sig=%d code=%d pid=%d val=%d\n",
                  i, sig, info.si_code, info.si_pid, info.si_value.sival_int);

        if (sig != rt || info.si_code != SI_QUEUE || info.si_pid != getpid() ||
            info.si_value.sival_int != values[i]) {
            failf("signal realtime fifo", sig, info.si_code, info.si_value.sival_int,
                  rt, SI_QUEUE, values[i]);
        }
    }

    if (pthread_sigmask(SIG_SETMASK, &oldset, NULL) != 0) {
        perror("pthread_sigmask restore");
        exit(1);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_rt_sigqueue_fifo();
    return finish_suite("signal_realtime");
}
