// rseq_register.c -- restartable sequences (rseq(2)).
//
// glibc 2.35 and later register an rseq area for every thread as it starts,
// and read the current CPU number out of it (sched_getcpu). AOK answered
// ENOSYS, which glibc survives, but a kernel claiming a 4.18-or-later release
// without it is claiming something it does not have.
//
// What is checked, against Linux 6.12: registration's argument rules and
// errnos, unregistration, the cpu_id fields a registration fills in, that a
// fork keeps the registration and an exec drops it -- and the one behaviour an
// rseq critical section exists for: a signal that interrupts one sends the
// thread to its abort handler instead of back into the middle of it.
//
// glibc's own registration is checked too, when there is one, and then the
// test re-runs itself with glibc.pthread.rseq=0 so it can register areas of
// its own.
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "test_common.h"

#ifndef SYS_rseq
#if defined(__x86_64__)
#define SYS_rseq 334
#elif defined(__i386__)
#define SYS_rseq 386
#else
#define SYS_rseq 293
#endif
#endif

#define RSEQ_FLAG_UNREGISTER 1
#define RSEQ_CPU_ID_UNINITIALIZED ((uint32_t) -1)
// Any 32-bit value will do, as long as the four bytes before every abort
// handler hold it; this is x86's, which the test uses on every architecture.
#define TEST_SIG 0x53053053u

struct rseq_area {
    uint32_t cpu_id_start;
    uint32_t cpu_id;
    uint64_t rseq_cs;
    uint32_t flags;
    uint32_t node_id;
    uint32_t mm_cid;
    char end[];
} __attribute__((aligned(32)));

struct rseq_cs_desc {
    uint32_t version;
    uint32_t flags;
    uint64_t start_ip;
    uint64_t post_commit_offset;
    uint64_t abort_ip;
} __attribute__((aligned(32)));

// glibc 2.35+'s registration, when it made one.
extern const unsigned int __rseq_size __attribute__((weak));
extern const ptrdiff_t __rseq_offset __attribute__((weak));

static long ncpus;

static long rseq_call(void *area, uint32_t len, int flags, uint32_t sig) {
    long r = syscall(SYS_rseq, area, len, flags, sig);
    return r < 0 ? -errno : r;
}

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-52s got=%-6ld want=%ld\n", label, got, want);
}

// A critical section, per architecture: store the descriptor into the area's
// rseq_cs, then spin inside [cs_start, cs_end) until *flag is set. Leaving
// through cs_end returns 1 (it ran to its end); the kernel sending it to
// cs_abort returns 2. The four bytes before cs_abort are TEST_SIG, which the
// kernel checks before it will jump there.
//
//     int rseq_cs_spin(volatile int *flag, struct rseq_area *rs, uintptr_t desc);
#if defined(__x86_64__)
__asm__(
    ".text\n"
    ".globl rseq_cs_spin\n"
    ".type rseq_cs_spin, @function\n"
    "rseq_cs_spin:\n"
    "    movq %rdx, 8(%rsi)\n"
    ".globl rseq_cs_start\n"
    "rseq_cs_start:\n"
    "    cmpl $0, (%rdi)\n"
    "    je rseq_cs_start\n"
    ".globl rseq_cs_end\n"
    "rseq_cs_end:\n"
    "    movl $1, %eax\n"
    "    ret\n"
    "    .long 0x53053053\n"
    ".globl rseq_cs_abort\n"
    "rseq_cs_abort:\n"
    "    movl $2, %eax\n"
    "    ret\n");
#elif defined(__i386__)
__asm__(
    ".text\n"
    ".globl rseq_cs_spin\n"
    ".type rseq_cs_spin, @function\n"
    "rseq_cs_spin:\n"
    "    movl 4(%esp), %ecx\n"
    "    movl 8(%esp), %edx\n"
    "    movl 12(%esp), %eax\n"
    "    movl %eax, 8(%edx)\n"
    "    movl $0, 12(%edx)\n"
    ".globl rseq_cs_start\n"
    "rseq_cs_start:\n"
    "    cmpl $0, (%ecx)\n"
    "    je rseq_cs_start\n"
    ".globl rseq_cs_end\n"
    "rseq_cs_end:\n"
    "    movl $1, %eax\n"
    "    ret\n"
    "    .long 0x53053053\n"
    ".globl rseq_cs_abort\n"
    "rseq_cs_abort:\n"
    "    movl $2, %eax\n"
    "    ret\n");
#elif defined(__aarch64__)
__asm__(
    ".text\n"
    ".globl rseq_cs_spin\n"
    ".type rseq_cs_spin, %function\n"
    "rseq_cs_spin:\n"
    "    str x2, [x1, #8]\n"
    ".globl rseq_cs_start\n"
    "rseq_cs_start:\n"
    "    ldr w3, [x0]\n"
    "    cbz w3, rseq_cs_start\n"
    ".globl rseq_cs_end\n"
    "rseq_cs_end:\n"
    "    mov w0, #1\n"
    "    ret\n"
    "    .inst 0x53053053\n"
    ".globl rseq_cs_abort\n"
    "rseq_cs_abort:\n"
    "    mov w0, #2\n"
    "    ret\n");
#elif defined(__riscv)
__asm__(
    ".text\n"
    ".option push\n"
    ".option norvc\n"
    ".globl rseq_cs_spin\n"
    ".type rseq_cs_spin, @function\n"
    "rseq_cs_spin:\n"
    "    sd a2, 8(a1)\n"
    ".globl rseq_cs_start\n"
    "rseq_cs_start:\n"
    "    lw t0, 0(a0)\n"
    "    beqz t0, rseq_cs_start\n"
    ".globl rseq_cs_end\n"
    "rseq_cs_end:\n"
    "    li a0, 1\n"
    "    ret\n"
    "    .4byte 0x53053053\n"
    ".globl rseq_cs_abort\n"
    "rseq_cs_abort:\n"
    "    li a0, 2\n"
    "    ret\n"
    ".option pop\n");
#else
#error "no critical section for this architecture"
#endif

int rseq_cs_spin(volatile int *flag, struct rseq_area *rs, uintptr_t desc);
extern char rseq_cs_start[], rseq_cs_end[], rseq_cs_abort[];

static volatile int cs_flag;
static void cs_handler(int sig) {
    (void) sig;
    cs_flag = 1;
}

struct kicker {
    pthread_t target;
    volatile int ready;
};

static void *kick(void *arg) {
    struct kicker *k = arg;
    while (!k->ready)
        sched_yield();
    struct timespec ts = {0, 20 * 1000 * 1000};
    nanosleep(&ts, NULL);
    pthread_kill(k->target, SIGUSR1);
    return NULL;
}

// A signal taken inside the critical section must come back at its abort
// handler. Linux may also abort one on a preemption before the signal comes,
// in which case the flag is still clear and the section is simply re-entered.
static int critical_section_aborts(struct rseq_area *rs) {
    static struct rseq_cs_desc desc;
    desc.version = 0;
    desc.flags = 0;
    desc.start_ip = (uintptr_t) rseq_cs_start;
    desc.post_commit_offset = (uintptr_t) rseq_cs_end - (uintptr_t) rseq_cs_start;
    desc.abort_ip = (uintptr_t) rseq_cs_abort;

    struct sigaction sa = {.sa_handler = cs_handler};
    sigaction(SIGUSR1, &sa, NULL);
    int result = 0;
    for (int attempt = 0; attempt < 3 && result != 2; attempt++) {
        cs_flag = 0;
        struct kicker k = {.target = pthread_self()};
        pthread_t t;
        pthread_create(&t, NULL, kick, &k);
        k.ready = 1;
        do {
            result = rseq_cs_spin(&cs_flag, rs, (uintptr_t) &desc);
        } while (result == 2 && !cs_flag);
        pthread_join(t, NULL);
        rs->rseq_cs = 0;
        test_logf("  critical section attempt %d: %s\n", attempt,
                  result == 2 ? "aborted" : "ran to its end");
    }
    signal(SIGUSR1, SIG_DFL);
    return result;
}

// The syscall's rules, on areas of our own.
static void check_registration(void) {
    static struct rseq_area area, other;
    static char big[64] __attribute__((aligned(32)));
    static char misaligned_buf[64] __attribute__((aligned(32)));

    // librseq probes for rseq this way: a NULL area is EINVAL, not ENOSYS.
    ck("probe with a NULL area", rseq_call(NULL, 0, 0, 0), -EINVAL);
    ck("unregister while not registered", rseq_call(&area, sizeof area, RSEQ_FLAG_UNREGISTER, TEST_SIG), -EINVAL);
    ck("unknown flag", rseq_call(&area, sizeof area, 2, TEST_SIG), -EINVAL);
    ck("length 31", rseq_call(&area, 31, 0, TEST_SIG), -EINVAL);
    ck("misaligned area", rseq_call(misaligned_buf + 4, 32, 0, TEST_SIG), -EINVAL);
    ck("unmapped area", rseq_call((void *) 0x1000, 32, 0, TEST_SIG), -EFAULT);

    area.cpu_id = RSEQ_CPU_ID_UNINITIALIZED;
    area.cpu_id_start = 12345;
    area.rseq_cs = 0xdead0000;  // stale, as a reused area can be
    ck("register", rseq_call(&area, sizeof area, 0, TEST_SIG), 0);
    ck("cpu_id is a cpu", area.cpu_id < (uint32_t) ncpus, 1);
    ck("cpu_id_start matches", area.cpu_id_start == area.cpu_id, 1);
    ck("node_id", area.node_id, 0);
    ck("mm_cid is below the cpu count", area.mm_cid < (uint32_t) ncpus, 1);
    ck("a stale rseq_cs is cleared", area.rseq_cs == 0, 1);
    // Read around the call, so a migration between the reads cannot make a
    // disagreement: when the area says the same CPU before and after,
    // getcpu must say it too.
    for (int i = 0; i < 20; i++) {
        uint32_t before = area.cpu_id;
        unsigned cpu = 99999, node = 99999;
        long r = syscall(SYS_getcpu, &cpu, &node, NULL);
        uint32_t after = area.cpu_id;
        if (before != after)
            continue;
        ck("getcpu", r, 0);
        ck("getcpu agrees with the area", cpu, before);
        ck("getcpu's node", node, 0);
        break;
    }

    ck("register again", rseq_call(&area, sizeof area, 0, TEST_SIG), -EBUSY);
    ck("register again, another signature", rseq_call(&area, sizeof area, 0, TEST_SIG + 1), -EPERM);
    ck("register again, another area", rseq_call(&other, sizeof other, 0, TEST_SIG), -EINVAL);
    ck("register again, another length", rseq_call(&area, 64, 0, TEST_SIG), -EINVAL);

    ck("critical section is aborted by a signal", critical_section_aborts(&area), 2);

    // The registration outlives a fork.
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        long r = rseq_call(&area, sizeof area, 0, TEST_SIG);
        _exit(r == -EBUSY ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    ck("a fork is still registered", WIFEXITED(st) && WEXITSTATUS(st) == 0, 1);

    ck("unregister, another signature", rseq_call(&area, sizeof area, RSEQ_FLAG_UNREGISTER, TEST_SIG + 1), -EPERM);
    ck("unregister, another area", rseq_call(&other, sizeof other, RSEQ_FLAG_UNREGISTER, TEST_SIG), -EINVAL);
    ck("unregister, another length", rseq_call(&area, 64, RSEQ_FLAG_UNREGISTER, TEST_SIG), -EINVAL);
    ck("unregister with a stray flag", rseq_call(&area, sizeof area, RSEQ_FLAG_UNREGISTER | 2, TEST_SIG), -EINVAL);
    ck("unregister", rseq_call(&area, sizeof area, RSEQ_FLAG_UNREGISTER, TEST_SIG), 0);
    ck("cpu_id reset", area.cpu_id, RSEQ_CPU_ID_UNINITIALIZED);
    ck("cpu_id_start reset", area.cpu_id_start, 0);
    ck("unregister twice", rseq_call(&area, sizeof area, RSEQ_FLAG_UNREGISTER, TEST_SIG), -EINVAL);

    // A longer area, of the extended layout, is fine too.
    ck("register 64 bytes", rseq_call(big, sizeof big, 0, TEST_SIG), 0);
    ck("unregister 64 bytes", rseq_call(big, sizeof big, RSEQ_FLAG_UNREGISTER, TEST_SIG), 0);
}

static void *thread_registers(void *arg) {
    (void) arg;
    static _Thread_local struct rseq_area area;
    area.cpu_id = RSEQ_CPU_ID_UNINITIALIZED;
    long r = rseq_call(&area, sizeof area, 0, TEST_SIG);
    long ok = r == 0 && area.cpu_id < (uint32_t) ncpus &&
            rseq_call(&area, sizeof area, RSEQ_FLAG_UNREGISTER, TEST_SIG) == 0;
    return (void *) ok;
}

static void *thread_pointer(void) {
    void *tp;
#if defined(__x86_64__)
    __asm__("mov %%fs:0, %0" : "=r"(tp));
#elif defined(__i386__)
    __asm__("mov %%gs:0, %0" : "=r"(tp));
#else
    tp = __builtin_thread_pointer();
#endif
    return tp;
}

// glibc's own area: registered, and sched_getcpu reads it.
static void check_libc_registration(void) {
    struct rseq_area *area = (struct rseq_area *) ((char *) thread_pointer() + __rseq_offset);
    ck("glibc: cpu_id is a cpu", area->cpu_id < (uint32_t) ncpus, 1);
    ck("glibc: cpu_id_start matches", area->cpu_id_start == area->cpu_id, 1);
    for (int i = 0; i < 20; i++) {
        uint32_t before = area->cpu_id;
        int cpu = sched_getcpu();
        if (area->cpu_id != before)
            continue;
        ck("glibc: sched_getcpu", cpu, (long) before);
        break;
    }
}

int main(int argc, char **argv) {
    // After an exec: the registration did not survive it.
    if (argc > 1 && strcmp(argv[1], "--after-exec") == 0) {
        static struct rseq_area area;
        return rseq_call(&area, sizeof area, 0, TEST_SIG) == 0 ? 0 : 1;
    }
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));
    ncpus = sysconf(_SC_NPROCESSORS_CONF);
    if (ncpus < 1)
        ncpus = 1;

    if (&__rseq_size != NULL && __rseq_size > 0) {
        check_libc_registration();
        if (getenv("RSEQ_REGISTER_REEXEC") == NULL) {
            // Everything else needs areas of our own, and glibc holds the
            // main thread's.
            setenv("GLIBC_TUNABLES", "glibc.pthread.rseq=0", 1);
            setenv("RSEQ_REGISTER_REEXEC", "1", 1);
            fflush(NULL);
            char *args[] = {argv[0], test_verbose ? "-v" : NULL, NULL};
            execv("/proc/self/exe", args);
            printf("FAIL re-exec without glibc's rseq: %s\n", strerror(errno));
            failures_total++;
            return finish_suite("rseq_register");
        }
    }

    check_registration();

    pthread_t threads[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&threads[i], NULL, thread_registers, NULL);
    for (int i = 0; i < 4; i++) {
        void *ok;
        pthread_join(threads[i], &ok);
        ck("a thread registers its own area", (long) ok, 1);
    }

    // An exec drops the registration: the new image may register afresh.
    static struct rseq_area area;
    ck("register before exec", rseq_call(&area, sizeof area, 0, TEST_SIG), 0);
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        execl("/proc/self/exe", argv[0], "--after-exec", (char *) NULL);
        _exit(2);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    ck("an exec is not registered", WIFEXITED(st) ? WEXITSTATUS(st) : 3, 0);
    rseq_call(&area, sizeof area, RSEQ_FLAG_UNREGISTER, TEST_SIG);

    return finish_suite("rseq_register");
}
