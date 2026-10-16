// SA_RESTART on a blocked read and a blocked waitpid: EINTR without it, the
// call restarted with it.
#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

static volatile sig_atomic_t handler_count;
static volatile sig_atomic_t last_sig;

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

struct sender_args {
    pid_t pid;
    int write_fd;
    int delay_sig_ms;
    int delay_write_ms;
    char byte;
};

static void *signal_and_optional_write(void *arg) {
    struct sender_args *args = arg;
    sleep_ms(args->delay_sig_ms);
    kill(args->pid, SIGUSR1);
    if (args->write_fd >= 0) {
        sleep_ms(args->delay_write_ms);
        write(args->write_fd, &args->byte, 1);
        close(args->write_fd);
    }
    return NULL;
}

static void reset_state(void) {
    handler_count = 0;
    last_sig = 0;
}

static void test_read_no_restart(void) {
    int pipefd[2];
    pthread_t thread;
    struct sender_args args;
    char ch = '\0';
    ssize_t rc;
    int err;

    if (pipe(pipefd) != 0) {
        perror("pipe");
        exit(1);
    }

    reset_state();
    install_handler(0);
    args = (struct sender_args) {
        .pid = getpid(),
        .write_fd = -1,
        .delay_sig_ms = 20,
        .delay_write_ms = 0,
        .byte = 0,
    };
    pthread_create(&thread, NULL, signal_and_optional_write, &args);

    errno = 0;
    rc = read(pipefd[0], &ch, 1);
    err = errno;
    pthread_join(thread, NULL);
    close(pipefd[0]);
    close(pipefd[1]);

    test_logf("signal read no-restart: rc=%zd errno=%d count=%d sig=%d\n",
              rc, err, handler_count, last_sig);

    if (rc != -1 || err != EINTR || handler_count != 1 || last_sig != SIGUSR1)
        failf("signal read no-restart", (uint64_t) rc, err, handler_count,
              (uint64_t) -1, EINTR, 1);
}

static void test_read_restart(void) {
    int pipefd[2];
    pthread_t thread;
    struct sender_args args;
    char ch = '\0';
    ssize_t rc;
    int err;

    if (pipe(pipefd) != 0) {
        perror("pipe");
        exit(1);
    }

    reset_state();
    install_handler(SA_RESTART);
    args = (struct sender_args) {
        .pid = getpid(),
        .write_fd = pipefd[1],
        .delay_sig_ms = 20,
        .delay_write_ms = 20,
        .byte = 'Z',
    };
    pthread_create(&thread, NULL, signal_and_optional_write, &args);

    errno = 0;
    rc = read(pipefd[0], &ch, 1);
    err = errno;
    pthread_join(thread, NULL);
    close(pipefd[0]);

    test_logf("signal read restart: rc=%zd errno=%d byte=%c count=%d sig=%d\n",
              rc, err, ch, handler_count, last_sig);

    if (rc != 1 || ch != 'Z' || handler_count != 1 || last_sig != SIGUSR1)
        failf("signal read restart", (uint64_t) rc, (unsigned char) ch, handler_count,
              1, 'Z', 1);
}

static void test_waitpid_no_restart(void) {
    pthread_t thread;
    struct sender_args args;
    pid_t child;
    int status = 0;
    pid_t rc;
    int err;

    child = fork();
    if (child < 0) {
        perror("fork");
        exit(1);
    }
    if (child == 0) {
        sleep_ms(200);
        _exit(77);
    }

    reset_state();
    install_handler(0);
    args = (struct sender_args) {
        .pid = getpid(),
        .write_fd = -1,
        .delay_sig_ms = 20,
        .delay_write_ms = 0,
        .byte = 0,
    };
    pthread_create(&thread, NULL, signal_and_optional_write, &args);

    errno = 0;
    rc = waitpid(child, &status, 0);
    err = errno;
    pthread_join(thread, NULL);

    test_logf("signal waitpid no-restart: rc=%d errno=%d count=%d sig=%d\n",
              rc, err, handler_count, last_sig);

    if (rc != -1 || err != EINTR || handler_count != 1 || last_sig != SIGUSR1)
        failf("signal waitpid no-restart", (uint64_t) rc, err, handler_count,
              (uint64_t) -1, EINTR, 1);

    rc = waitpid(child, &status, 0);
    if (rc != child || !WIFEXITED(status) || WEXITSTATUS(status) != 77)
        failf("signal waitpid no-restart reap", rc, status, 0, child, 77, 0);
}

static void test_waitpid_restart(void) {
    pthread_t thread;
    struct sender_args args;
    pid_t child;
    int status = 0;
    pid_t rc;
    int err;

    child = fork();
    if (child < 0) {
        perror("fork");
        exit(1);
    }
    if (child == 0) {
        sleep_ms(200);
        _exit(88);
    }

    reset_state();
    install_handler(SA_RESTART);
    args = (struct sender_args) {
        .pid = getpid(),
        .write_fd = -1,
        .delay_sig_ms = 20,
        .delay_write_ms = 0,
        .byte = 0,
    };
    pthread_create(&thread, NULL, signal_and_optional_write, &args);

    errno = 0;
    rc = waitpid(child, &status, 0);
    err = errno;
    pthread_join(thread, NULL);

    test_logf("signal waitpid restart: rc=%d errno=%d status=%d count=%d sig=%d\n",
              rc, err, status, handler_count, last_sig);

    if (rc != child || !WIFEXITED(status) || WEXITSTATUS(status) != 88 ||
        handler_count != 1 || last_sig != SIGUSR1) {
        failf("signal waitpid restart", rc, status, handler_count, child, 88, 1);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_read_no_restart();
    test_read_restart();
    test_waitpid_no_restart();
    test_waitpid_restart();
    return finish_suite("signal_restart");
}
