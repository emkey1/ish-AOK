// A handler installed with SA_ONSTACK runs on the stack sigaltstack set up, not
// on the thread's own.
#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_common.h"

static volatile sig_atomic_t handler_count;
static volatile sig_atomic_t handler_on_altstack;
static volatile sig_atomic_t last_sig;
static stack_t configured_stack;

static void onstack_handler(int sig, siginfo_t *info, void *ucontext) {
    char marker;
    uintptr_t sp = (uintptr_t) &marker;
    uintptr_t base = (uintptr_t) configured_stack.ss_sp;
    uintptr_t limit = base + configured_stack.ss_size;

    (void) info;
    (void) ucontext;

    handler_count++;
    last_sig = sig;
    handler_on_altstack = sp >= base && sp < limit;
}

static void test_sigaltstack_delivery(void) {
    struct sigaction act;
    stack_t oldstack;
    void *buf;

    buf = malloc(SIGSTKSZ);
    if (buf == NULL) {
        perror("malloc");
        exit(1);
    }

    configured_stack = (stack_t) {
        .ss_sp = buf,
        .ss_size = SIGSTKSZ,
        .ss_flags = 0,
    };
    if (sigaltstack(&configured_stack, &oldstack) != 0) {
        perror("sigaltstack");
        exit(1);
    }

    memset(&act, 0, sizeof(act));
    act.sa_sigaction = onstack_handler;
    act.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&act.sa_mask);
    if (sigaction(SIGUSR1, &act, NULL) != 0) {
        perror("sigaction");
        exit(1);
    }

    handler_count = 0;
    handler_on_altstack = 0;
    last_sig = 0;

    if (kill(getpid(), SIGUSR1) != 0) {
        perror("kill");
        exit(1);
    }

    test_logf("signal altstack: count=%d onstack=%d sig=%d size=%zu\n",
              handler_count, handler_on_altstack, last_sig, configured_stack.ss_size);

    if (handler_count != 1 || handler_on_altstack != 1 || last_sig != SIGUSR1)
        failf("signal altstack", handler_count, handler_on_altstack, last_sig, 1, 1, SIGUSR1);

    if (sigaltstack(&oldstack, NULL) != 0) {
        perror("sigaltstack restore");
        exit(1);
    }
    free(buf);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_sigaltstack_delivery();
    return finish_suite("signal_altstack");
}
