#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#endif

#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1
#endif

#ifndef FUTEX_PRIVATE_FLAG
#define FUTEX_PRIVATE_FLAG 128
#endif

#ifndef FUTEX_WAKE_OP
#define FUTEX_WAKE_OP 5
#endif
#ifndef FUTEX_OP
#define FUTEX_OP_SET 0
#define FUTEX_OP_ADD 1
#define FUTEX_OP_CMP_EQ 0
#define FUTEX_OP(op, oparg, cmp, cmparg) \
    (((op & 0xf) << 28) | ((cmp & 0xf) << 24) | ((oparg & 0xfff) << 12) | (cmparg & 0xfff))
#endif

static volatile uint32_t futex_word;
static volatile sig_atomic_t handler_count;
static volatile sig_atomic_t last_sig;

struct wait_result {
    int rc;
    int err;
    int timeout_ms;
    int done;
    pid_t tid;
    int ready;
};

struct wake_args {
    int delay_ms;
    int delay_wake_ms;
    int send_signal;
    int process_signal;
    int wake_count;
    uint32_t set_value;
    pid_t target_tid;
};

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long) (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static int raw_syscall_status(long ret) {
    if ((unsigned long) ret >= (unsigned long) -4095) {
        errno = (int) -ret;
        return -1;
    }
    return (int) ret;
}

#if defined(__i386__)
static long raw_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
    // Bind a1/a2/a3/a4/a5 directly to their ABI registers (b/c/d/S/D) so the
    // compiler can never place one of them in the same register another
    // operand needs -- the previous version used generic "rm" constraints
    // for a1/a4/a5/a6 plus explicit "mov %[argN], %%reg" shuffles, which let
    // gcc -O2 pick e.g. edi for [arg1] and esi for [arg5] and then clobber
    // esi (via the [arg4] move, reusing edx's register when a3==a4) before
    // the [arg5] move read it, silently corrupting uaddr2/val3 for any
    // futex(2) op that uses all 6 args (FUTEX_WAKE_OP). ebp has no such
    // single-letter constraint, so a6 goes in via the stack: pushed first
    // (while ebp/esp are still whatever the compiler expects, so %[arg6]'s
    // own ebp/esp-relative addressing mode is still valid), then loaded into
    // ebp via a literal esp-relative offset. a6 is "m", NOT "rm": the other
    // six operands already pin eax/ebx/ecx/edx/esi/edi, so once ebp is also
    // unavailable (a frame pointer, or -fno-pie letting the allocator try
    // ebp and fail) there is no register left for the "r" alternative and
    // gcc reports "'asm' operand has impossible constraints", which is what
    // blocked the whole i386 regression suite from building. Memory-only
    // needs no spare register, and "push mem" is a legal i386 encoding.
    // An xchgl-with-memory version of this (swap ebp and a6's stack slot
    // directly) is what real libcs use, but it's only safe under
    // -fomit-frame-pointer: with a frame pointer, gcc addresses
    // %[arg6] as -N(%ebp), and the second xchgl (restoring ebp) then
    // computes that same -N(%ebp) using the *new* ebp (a6's syscall value,
    // not the frame pointer) -- garbage address, corrupting the stack.
    // Plain esp-relative offsets here are literal in the template text, so
    // they stay correct regardless of the caller's frame-pointer choice.
    long ret;
    __asm__ volatile(
        "push %[arg6]\n\t"
        "push %%ebp\n\t"
        "mov 4(%%esp), %%ebp\n\t"
        "int $0x80\n\t"
        "pop %%ebp\n\t"
        "add $4, %%esp\n\t"
        : "=a"(ret)
        : "a"(nr), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5), [arg6] "m"(a6)
        : "memory", "cc");
    return ret;
}
#elif defined(__x86_64__)
static long raw_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory", "cc");
    return ret;
}
#elif defined(__aarch64__)
static long raw_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
    // A REAL raw syscall (svc #0) returning the kernel's negative-errno
    // value, which raw_syscall_status() decodes. The libc syscall()
    // wrapper (below) already returns -1 + sets errno, so feeding its -1
    // into raw_syscall_status() mis-decodes it as errno=1 (EPERM) — the
    // whole point of this per-arch shim is to bypass that.
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    register long x2 __asm__("x2") = a3;
    register long x3 __asm__("x3") = a4;
    register long x4 __asm__("x4") = a5;
    register long x5 __asm__("x5") = a6;
    __asm__ volatile(
        "svc #0"
        : "+r"(x0)
        : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
        : "memory", "cc");
    return x0;
}
#elif defined(__riscv) && __riscv_xlen == 64
static long raw_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
    // Same rationale as the aarch64 shim above: a real raw syscall (ecall)
    // returning the kernel's negative-errno value directly, bypassing the
    // libc syscall() wrapper's own -1/errno conversion.
    register long a7 __asm__("a7") = nr;
    register long x10 __asm__("a0") = a1;
    register long x11 __asm__("a1") = a2;
    register long x12 __asm__("a2") = a3;
    register long x13 __asm__("a3") = a4;
    register long x14 __asm__("a4") = a5;
    register long x15 __asm__("a5") = a6;
    __asm__ volatile(
        "ecall"
        : "+r"(x10)
        : "r"(a7), "r"(x11), "r"(x12), "r"(x13), "r"(x14), "r"(x15)
        : "memory");
    return x10;
}
#else
static long raw_syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6) {
    return syscall(nr, a1, a2, a3, a4, a5, a6);
}
#endif

static int futex_wait_raw(volatile uint32_t *uaddr, uint32_t expected, const struct timespec *timeout) {
    long ret = raw_syscall6(SYS_futex, (long) (uintptr_t) uaddr,
                            FUTEX_WAIT | FUTEX_PRIVATE_FLAG, expected,
                            (long) (uintptr_t) timeout, 0, 0);
    return raw_syscall_status(ret);
}

static int futex_wake_raw(volatile uint32_t *uaddr, int count) {
    long ret = raw_syscall6(SYS_futex, (long) (uintptr_t) uaddr,
                            FUTEX_WAKE | FUTEX_PRIVATE_FLAG, count, 0, 0, 0);
    return raw_syscall_status(ret);
}

static long futex_wake_op_raw(volatile uint32_t *uaddr1, uint32_t wake1, uint32_t wake2,
        volatile uint32_t *uaddr2, uint32_t encoded_op) {
    long ret = raw_syscall6(SYS_futex, (long) (uintptr_t) uaddr1,
                            FUTEX_WAKE_OP | FUTEX_PRIVATE_FLAG, wake1, wake2,
                            (long) (uintptr_t) uaddr2, encoded_op);
    return raw_syscall_status(ret);
}

static volatile uint32_t wake_op_a, wake_op_b;

static void *wake_op_waiter_a(void *arg) {
    (void) arg;
    futex_wait_raw(&wake_op_a, 0, NULL);
    return NULL;
}

static void *wake_op_waiter_b(void *arg) {
    (void) arg;
    futex_wait_raw(&wake_op_b, 0, NULL);
    return NULL;
}

// Covers FUTEX_WAKE_OP (issue #423 Tier 3): SET *uaddr2, wake 1 waiter on
// uaddr1 unconditionally, and -- since the old value at uaddr2 (0) compares
// equal to the encoded cmparg (0) -- also wake 1 waiter on uaddr2.
static void test_futex_wake_op(void) {
    wake_op_a = 0;
    wake_op_b = 0;
    pthread_t ta, tb;
    if (pthread_create(&ta, NULL, wake_op_waiter_a, NULL) != 0 ||
            pthread_create(&tb, NULL, wake_op_waiter_b, NULL) != 0) {
        perror("pthread_create");
        exit(1);
    }
    usleep(200000);

    int op = FUTEX_OP(FUTEX_OP_SET, 1, FUTEX_OP_CMP_EQ, 0);
    long woken = futex_wake_op_raw(&wake_op_a, 1, 1, &wake_op_b, op);
    usleep(200000);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    test_logf("futex_wake_op: woken=%ld wake_op_b=%u\n", woken, wake_op_b);
    if (woken != 2 || wake_op_b != 1)
        failf("futex wake_op SET+CMP_EQ wakes both sides", woken, wake_op_b, 0, 2, 1, 0);
}

static void reset_handler_state(void) {
    handler_count = 0;
    last_sig = 0;
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

static void *wake_helper(void *arg) {
    struct wake_args *args = arg;
    sleep_ms(args->delay_ms);
    if (args->send_signal) {
        if (args->process_signal) {
            if (kill(getpid(), SIGUSR1) != 0) {
                perror("kill");
                exit(1);
            }
        } else {
            send_thread_signal(args->target_tid, SIGUSR1);
        }
    }
    if (args->wake_count > 0) {
        sleep_ms(args->delay_wake_ms);
        futex_word = args->set_value;
        futex_wake_raw(&futex_word, args->wake_count);
    }
    return NULL;
}

static void *waiter_thread(void *arg) {
    struct wait_result *result = arg;
    struct timespec timeout;
    timeout.tv_sec = result->timeout_ms / 1000;
    timeout.tv_nsec = (long) (result->timeout_ms % 1000) * 1000000L;
    result->tid = gettid_linux();
    __atomic_store_n(&result->ready, 1, __ATOMIC_RELEASE);
    errno = 0;
    result->rc = futex_wait_raw(&futex_word, 0, &timeout);
    result->err = errno;
    __atomic_store_n(&result->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void wait_until_ready(struct wait_result *result) {
    while (!__atomic_load_n(&result->ready, __ATOMIC_ACQUIRE))
        sched_yield();
    sleep_ms(20);
}

static void test_futex_wait_eagain(void) {
    struct timespec timeout = {.tv_sec = 0, .tv_nsec = 1000000};
    int rc;
    int err;

    futex_word = 1;
    errno = 0;
    rc = futex_wait_raw(&futex_word, 0, &timeout);
    err = errno;

    test_logf("futex wait eagain: rc=%d errno=%d word=%u\n", rc, err, futex_word);

    if (rc != -1 || err != EAGAIN)
        failf("futex wait eagain", rc, err, futex_word, -1, EAGAIN, 1);
}

static void test_futex_wait_timeout(void) {
    struct timespec timeout = {.tv_sec = 0, .tv_nsec = 50000000};
    int rc;
    int err;

    futex_word = 0;
    errno = 0;
    rc = futex_wait_raw(&futex_word, 0, &timeout);
    err = errno;

    test_logf("futex wait timeout: rc=%d errno=%d word=%u\n", rc, err, futex_word);

    if (rc != -1 || err != ETIMEDOUT)
        failf("futex wait timeout", rc, err, futex_word, -1, ETIMEDOUT, 0);
}

static void test_futex_wait_wake_single(void) {
    pthread_t thread;
    struct wait_result result = {.timeout_ms = 1000};
    int woke;
    int failed;

    futex_word = 0;
    if (pthread_create(&thread, NULL, waiter_thread, &result) != 0) {
        perror("pthread_create");
        exit(1);
    }
    wait_until_ready(&result);
    futex_word = 1;
    errno = 0;
    woke = futex_wake_raw(&futex_word, 1);
    pthread_join(thread, NULL);

    failed = woke != 1 || result.rc != 0 || result.err != 0 || futex_word != 1;
    test_log_if(failed, "futex wait/wake single: wake=%d rc=%d errno=%d word=%u\n",
                woke, result.rc, result.err, futex_word);

    if (woke != 1 || result.rc != 0 || result.err != 0)
        failf("futex wait wake single", woke, result.rc, result.err, 1, 0, 0);
}

static void test_futex_wake_counts(void) {
    pthread_t threads[2];
    struct wait_result results[2] = {
        {.timeout_ms = 1000},
        {.timeout_ms = 1000},
    };
    int wake1;
    int wake2;
    int done_after_wake1 = 0;
    int failed;

    futex_word = 0;
    for (int i = 0; i < 2; i++) {
        if (pthread_create(&threads[i], NULL, waiter_thread, &results[i]) != 0) {
            perror("pthread_create");
            exit(1);
        }
    }
    for (int i = 0; i < 2; i++)
        wait_until_ready(&results[i]);
    futex_word = 1;
    wake1 = futex_wake_raw(&futex_word, 1);
    sleep_ms(20);
    for (int i = 0; i < 2; i++)
        done_after_wake1 += __atomic_load_n(&results[i].done, __ATOMIC_ACQUIRE);
    wake2 = futex_wake_raw(&futex_word, 1);
    for (int i = 0; i < 2; i++)
        pthread_join(threads[i], NULL);

    failed = wake1 != 1 || wake2 != 1 || done_after_wake1 != 1 ||
             results[0].rc != 0 || results[1].rc != 0 ||
             results[0].err != 0 || results[1].err != 0;
    test_log_if(failed, "futex wake counts: wake1=%d wake2=%d done_after_wake1=%d rc0=%d err0=%d rc1=%d err1=%d\n",
                wake1, wake2, done_after_wake1,
                results[0].rc, results[0].err, results[1].rc, results[1].err);

    if (wake1 != 1 || wake2 != 1 || done_after_wake1 != 1)
        failf("futex wake counts", wake1, wake2, done_after_wake1, 1, 1, 1);
}

static void test_futex_signal_no_restart(void) {
    pthread_t thread;
    struct wait_result result = {.timeout_ms = 200};

    reset_handler_state();
    install_handler(0);
    futex_word = 0;
    if (pthread_create(&thread, NULL, waiter_thread, &result) != 0) {
        perror("pthread_create");
        exit(1);
    }
    wait_until_ready(&result);

    send_thread_signal(result.tid, SIGUSR1);
    pthread_join(thread, NULL);

    test_logf("futex signal no-restart: rc=%d errno=%d count=%d sig=%d\n",
              result.rc, result.err, handler_count, last_sig);

    if (result.rc != -1 || result.err != EINTR || handler_count != 1 || last_sig != SIGUSR1)
        failf("futex signal no-restart", result.rc, result.err, handler_count, -1, EINTR, 1);
}

static void test_futex_signal_restart(void) {
    pthread_t thread;
    struct timespec timeout = {.tv_sec = 1, .tv_nsec = 0};
    struct wake_args args = {
        .delay_ms = 20,
        .delay_wake_ms = 20,
        .send_signal = 1,
        .wake_count = 1,
        .set_value = 0,
        .target_tid = gettid_linux(),
    };
    int rc;
    int err;

    reset_handler_state();
    install_handler(SA_RESTART);
    futex_word = 0;
    if (pthread_create(&thread, NULL, wake_helper, &args) != 0) {
        perror("pthread_create");
        exit(1);
    }

    errno = 0;
    rc = futex_wait_raw(&futex_word, 0, &timeout);
    err = errno;
    pthread_join(thread, NULL);

    test_logf("futex signal restart: rc=%d errno=%d count=%d sig=%d word=%u\n",
              rc, err, handler_count, last_sig, futex_word);

    if (rc != 0 || handler_count != 1 || last_sig != SIGUSR1 || futex_word != 0)
        failf("futex signal restart", rc, err, handler_count, 0, 0, 1);
}

static void test_futex_signal_process_no_restart(void) {
    pthread_t thread;
    struct wait_result result = {.timeout_ms = 200};

    reset_handler_state();
    install_handler(0);
    futex_word = 0;
    if (pthread_create(&thread, NULL, waiter_thread, &result) != 0) {
        perror("pthread_create");
        exit(1);
    }
    wait_until_ready(&result);

    if (kill(getpid(), SIGUSR1) != 0) {
        perror("kill");
        exit(1);
    }
    pthread_join(thread, NULL);

    test_logf("futex signal process no-restart: rc=%d errno=%d count=%d sig=%d\n",
              result.rc, result.err, handler_count, last_sig);

    if (result.rc != -1 || result.err != EINTR || handler_count != 1 || last_sig != SIGUSR1)
        failf("futex signal process no-restart", result.rc, result.err, handler_count, -1, EINTR, 1);
}

static void test_futex_signal_then_wake_no_restart(void) {
    pthread_t thread;
    struct timespec timeout = {.tv_sec = 1, .tv_nsec = 0};
    struct wake_args args = {
        .delay_ms = 20,
        .delay_wake_ms = 100,
        .send_signal = 1,
        .process_signal = 0,
        .wake_count = 1,
        .set_value = 1,
        .target_tid = gettid_linux(),
    };
    int rc;
    int err;

    reset_handler_state();
    install_handler(0);
    futex_word = 0;
    if (pthread_create(&thread, NULL, wake_helper, &args) != 0) {
        perror("pthread_create");
        exit(1);
    }

    errno = 0;
    rc = futex_wait_raw(&futex_word, 0, &timeout);
    err = errno;
    pthread_join(thread, NULL);

    test_logf("futex signal then wake no-restart: rc=%d errno=%d count=%d sig=%d word=%u\n",
              rc, err, handler_count, last_sig, futex_word);

    if (rc != -1 || err != EINTR || handler_count != 1 || last_sig != SIGUSR1)
        failf("futex signal then wake no-restart", rc, err, handler_count, -1, EINTR, 1);
}

static void test_futex_waiter_thread_signal_then_wake_no_restart(void) {
    pthread_t thread;
    struct wait_result result = {.timeout_ms = 1000};

    reset_handler_state();
    install_handler(0);
    futex_word = 0;
    if (pthread_create(&thread, NULL, waiter_thread, &result) != 0) {
        perror("pthread_create");
        exit(1);
    }
    wait_until_ready(&result);

    send_thread_signal(result.tid, SIGUSR1);
    sleep_ms(100);
    futex_word = 1;
    if (futex_wake_raw(&futex_word, 1) < 0) {
        perror("futex_wake_raw");
        exit(1);
    }
    pthread_join(thread, NULL);

    test_logf("futex waiter thread signal then wake no-restart: rc=%d errno=%d count=%d sig=%d word=%u\n",
              result.rc, result.err, handler_count, last_sig, futex_word);

    if (result.rc != -1 || result.err != EINTR || handler_count != 1 || last_sig != SIGUSR1)
        failf("futex waiter thread signal then wake no-restart",
              result.rc, result.err, handler_count, -1, EINTR, 1);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    test_futex_wait_eagain();
    test_futex_wait_timeout();
    test_futex_wait_wake_single();
    test_futex_wake_counts();
    test_futex_signal_restart();
    test_futex_signal_then_wake_no_restart();
    test_futex_waiter_thread_signal_then_wake_no_restart();
    test_futex_wake_op();
    return finish_suite("futex_core");
}
