// A signal interrupting a blocked eventfd read or poll: EINTR without
// SA_RESTART, the read restarted with it, on the main thread and on a waiter
// thread.
#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static volatile sig_atomic_t handler_count;
static volatile sig_atomic_t last_sig;

struct helper_args {
    pid_t target_tid;
    int event_fd;
    int delay_sig_ms;
    int delay_write_ms;
    int do_signal;
    int do_write;
    uint64_t write_value;
};

struct waiter_result {
    int rc;
    int err;
    pid_t tid;
    int ready;
};

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long) (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void signal_handler(int sig) {
    handler_count++;
    last_sig = sig;
}

static pid_t gettid_linux(void) {
    return (pid_t) syscall(SYS_gettid);
}

static void send_thread_signal(pid_t tid, int sig) {
    if (syscall(SYS_tgkill, getpid(), tid, sig) != 0) {
        perror("tgkill");
        exit(1);
    }
}

static void reset_state(void) {
    handler_count = 0;
    last_sig = 0;
}

static void install_handler(int flags) {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = signal_handler;
    act.sa_flags = flags;
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGUSR1, &act, NULL) != 0) {
        perror("sigaction");
        exit(1);
    }
}

static void *signal_and_optional_write(void *arg) {
    struct helper_args *args = arg;
    sleep_ms(args->delay_sig_ms);
    if (args->do_signal) {
        send_thread_signal(args->target_tid, SIGUSR1);
    }
    if (args->do_write) {
        sleep_ms(args->delay_write_ms);
        if (write(args->event_fd, &args->write_value, sizeof(args->write_value)) !=
                (ssize_t) sizeof(args->write_value)) {
            perror("write");
            exit(1);
        }
    }
    return NULL;
}

static void wait_until_ready(struct waiter_result *result) {
    while (!__atomic_load_n(&result->ready, __ATOMIC_ACQUIRE))
        sched_yield();
    sleep_ms(20);
}

static void *eventfd_waiter_thread(void *arg) {
    struct waiter_result *result = arg;
    int efd = (int) (intptr_t) result->err;
    uint64_t value = 0;

    result->tid = gettid_linux();
    __atomic_store_n(&result->ready, 1, __ATOMIC_RELEASE);
    errno = 0;
    result->rc = (int) read(efd, &value, sizeof(value));
    result->err = errno;
    return NULL;
}

static void *eventfd_poll_waiter_thread(void *arg) {
    struct waiter_result *result = arg;
    int efd = (int) (intptr_t) result->err;
    struct pollfd pfd = {
        .fd = efd,
        .events = POLLIN,
        .revents = 0,
    };

    result->tid = gettid_linux();
    __atomic_store_n(&result->ready, 1, __ATOMIC_RELEASE);
    errno = 0;
    result->rc = poll(&pfd, 1, 200);
    result->err = errno;
    return NULL;
}

static void test_eventfd_no_restart(void) {
    int efd;
    pthread_t thread;
    struct helper_args args;
    uint64_t value = 0;
    ssize_t rc;
    int err;

    efd = eventfd(0, 0);
    if (efd < 0) {
        perror("eventfd");
        exit(1);
    }

    reset_state();
    install_handler(0);
    args = (struct helper_args) {
        .target_tid = gettid_linux(),
        .event_fd = efd,
        .delay_sig_ms = 20,
        .delay_write_ms = 0,
        .do_signal = 1,
        .do_write = 0,
        .write_value = 0,
    };
    if (pthread_create(&thread, NULL, signal_and_optional_write, &args) != 0) {
        perror("pthread_create");
        exit(1);
    }

    errno = 0;
    rc = read(efd, &value, sizeof(value));
    err = errno;
    pthread_join(thread, NULL);
    close(efd);

    test_logf("eventfd no-restart: rc=%zd errno=%d count=%d sig=%d\n",
              rc, err, handler_count, last_sig);

    if (rc != -1 || err != EINTR || handler_count != 1 || last_sig != SIGUSR1)
        failf("eventfd no-restart", (uint64_t) rc, err, handler_count,
              (uint64_t) -1, EINTR, 1);
}

static void test_eventfd_restart(void) {
    int efd;
    pthread_t thread;
    struct helper_args args;
    uint64_t value = 0;
    ssize_t rc;
    int err;

    efd = eventfd(0, 0);
    if (efd < 0) {
        perror("eventfd");
        exit(1);
    }

    reset_state();
    install_handler(SA_RESTART);
    args = (struct helper_args) {
        .target_tid = gettid_linux(),
        .event_fd = efd,
        .delay_sig_ms = 20,
        .delay_write_ms = 20,
        .do_signal = 1,
        .do_write = 1,
        .write_value = 1,
    };
    if (pthread_create(&thread, NULL, signal_and_optional_write, &args) != 0) {
        perror("pthread_create");
        exit(1);
    }

    errno = 0;
    rc = read(efd, &value, sizeof(value));
    err = errno;
    pthread_join(thread, NULL);
    close(efd);

    test_logf("eventfd restart: rc=%zd errno=%d value=%" PRIu64 " count=%d sig=%d\n",
              rc, err, value, handler_count, last_sig);

    if (rc != (ssize_t) sizeof(value) || value != 1 || err != 0 ||
            handler_count != 1 || last_sig != SIGUSR1) {
        failf("eventfd restart", (uint64_t) rc, value, handler_count,
              sizeof(value), 1, 1);
    }
}

static void test_eventfd_poll_no_restart(void) {
    int efd;
    pthread_t thread;
    struct helper_args args;
    struct pollfd pfd;
    int rc;
    int err;

    efd = eventfd(0, 0);
    if (efd < 0) {
        perror("eventfd");
        exit(1);
    }

    reset_state();
    install_handler(0);
    args = (struct helper_args) {
        .target_tid = gettid_linux(),
        .event_fd = efd,
        .delay_sig_ms = 20,
        .delay_write_ms = 0,
        .do_signal = 1,
        .do_write = 0,
        .write_value = 0,
    };
    if (pthread_create(&thread, NULL, signal_and_optional_write, &args) != 0) {
        perror("pthread_create");
        exit(1);
    }

    pfd = (struct pollfd) {
        .fd = efd,
        .events = POLLIN,
        .revents = 0,
    };
    errno = 0;
    rc = poll(&pfd, 1, 200);
    err = errno;
    pthread_join(thread, NULL);
    close(efd);

    test_logf("eventfd poll no-restart: rc=%d errno=%d revents=%#x count=%d sig=%d\n",
              rc, err, pfd.revents, handler_count, last_sig);

    if (rc != -1 || err != EINTR || handler_count != 1 || last_sig != SIGUSR1)
        failf("eventfd poll no-restart", rc, err, handler_count, -1, EINTR, 1);
}

static void test_eventfd_waiter_thread_no_restart(void) {
    int efd;
    pthread_t thread;
    struct waiter_result result = {.rc = 0, .err = 0, .ready = 0};

    efd = eventfd(0, 0);
    if (efd < 0) {
        perror("eventfd");
        exit(1);
    }

    reset_state();
    install_handler(0);
    result.err = efd;
    if (pthread_create(&thread, NULL, eventfd_waiter_thread, &result) != 0) {
        perror("pthread_create");
        exit(1);
    }
    wait_until_ready(&result);

    send_thread_signal(result.tid, SIGUSR1);
    pthread_join(thread, NULL);
    close(efd);

    test_logf("eventfd waiter thread no-restart: rc=%d errno=%d count=%d sig=%d\n",
              result.rc, result.err, handler_count, last_sig);

    if (result.rc != -1 || result.err != EINTR || handler_count != 1 || last_sig != SIGUSR1)
        failf("eventfd waiter thread no-restart", result.rc, result.err, handler_count,
              -1, EINTR, 1);
}

static void test_eventfd_poll_waiter_thread_no_restart(void) {
    int efd;
    pthread_t thread;
    struct waiter_result result = {.rc = 0, .err = 0, .ready = 0};

    efd = eventfd(0, 0);
    if (efd < 0) {
        perror("eventfd");
        exit(1);
    }

    reset_state();
    install_handler(0);
    result.err = efd;
    if (pthread_create(&thread, NULL, eventfd_poll_waiter_thread, &result) != 0) {
        perror("pthread_create");
        exit(1);
    }
    wait_until_ready(&result);

    send_thread_signal(result.tid, SIGUSR1);
    pthread_join(thread, NULL);
    close(efd);

    test_logf("eventfd poll waiter thread no-restart: rc=%d errno=%d count=%d sig=%d\n",
              result.rc, result.err, handler_count, last_sig);

    if (result.rc != -1 || result.err != EINTR || handler_count != 1 || last_sig != SIGUSR1)
        failf("eventfd poll waiter thread no-restart", result.rc, result.err, handler_count,
              -1, EINTR, 1);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_eventfd_no_restart();
    test_eventfd_restart();
    test_eventfd_poll_no_restart();
    test_eventfd_waiter_thread_no_restart();
    test_eventfd_poll_waiter_thread_no_restart();
    return finish_suite("eventfd_interrupt");
}
