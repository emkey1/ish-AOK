// Memory mapped without PROT_EXEC does not execute (NX).
//
// Nothing checked: code written into a page mapped PROT_READ|PROT_WRITE ran,
// and so did code on the stack and the heap, so any overflow that could write
// code could run it. What Linux does, asserted here on every architecture:
//
//   - a call into a RW page, the stack or the heap raises SIGSEGV with
//     si_code SEGV_ACCERR and si_addr the address that could not be fetched;
//     so does a PROT_NONE page, and an unmapped one is SEGV_MAPERR;
//   - code that runs off the end of an executable page into one that is not
//     faults at the first byte of the second page, having run the first;
//   - mprotect to PROT_READ|PROT_EXEC makes the same page run, and back to
//     PROT_READ|PROT_WRITE stops it again (translated code must not outlive
//     the permission);
//   - mprotect over a range with a hole changes the pages before the hole
//     and still fails with ENOMEM; a range that starts in a hole changes
//     nothing;
//   - mprotect with PROT_GROWSDOWN on a stack page makes the whole stack
//     executable, down to pages it grows into later -- glibc's way of
//     honouring a library that needs an executable stack -- and is EINVAL on
//     a mapping that does not grow down;
//   - personality(READ_IMPLIES_EXEC) makes a later readable mapping run;
//   - /proc/self/maps shows the stack without x;
//   - a signal handler still returns (64-bit guests return through a
//     trampoline the kernel maps, no longer through the stack).
#define _GNU_SOURCE
#include <alloca.h>
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef READ_IMPLIES_EXEC
#define READ_IMPLIES_EXEC 0x0400000
#endif
#ifndef PROT_GROWSDOWN
#define PROT_GROWSDOWN 0x01000000
#endif
#ifndef PROT_GROWSUP
#define PROT_GROWSUP 0x02000000
#endif

static long page_size;
static sigjmp_buf jb;
static volatile sig_atomic_t fault_code;
static void *volatile fault_addr;

static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void) sig; (void) uc;
    fault_code = si->si_code;
    fault_addr = si->si_addr;
    siglongjmp(jb, 1);
}

// The architecture's "return" instruction, and a no-op that falls through.
#if defined(__x86_64__) || defined(__i386__)
static const unsigned char RET[] = {0xc3};
static const unsigned char NOP[] = {0x90};
#elif defined(__aarch64__)
static const unsigned char RET[] = {0xc0, 0x03, 0x5f, 0xd6};
static const unsigned char NOP[] = {0x1f, 0x20, 0x03, 0xd5};
#elif defined(__riscv)
static const unsigned char RET[] = {0x67, 0x80, 0x00, 0x00}; // jalr x0, 0(ra)
static const unsigned char NOP[] = {0x13, 0x00, 0x00, 0x00}; // addi x0, x0, 0
#else
#error "unknown architecture"
#endif

static void check(const char *label, int ok, long got, long want) {
    if (!ok)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s %s (got %#lx, want %#lx)\n", label, ok ? "ok" : "FAIL", got, want);
}

// Call `code` and report: 0 if it returned, else the SIGSEGV si_code (with
// fault_addr set).
static int try_call(void *code) {
    fault_code = 0;
    fault_addr = NULL;
    if (sigsetjmp(jb, 1) == 0) {
        __builtin___clear_cache((char *) code, (char *) code + 16);
        ((void (*)(void)) code)();
        return 0;
    }
    return fault_code;
}

static void put_ret(void *at) {
    memcpy(at, RET, sizeof(RET));
}

static void expect_fault(const char *label, void *code, int want_code, void *want_addr) {
    int c = try_call(code);
    char l[128];
    snprintf(l, sizeof l, "%s: SIGSEGV", label);
    check(l, c == want_code, c, want_code);
    snprintf(l, sizeof l, "%s: si_addr", label);
    check(l, fault_addr == want_addr, (long) (uintptr_t) fault_addr, (long) (uintptr_t) want_addr);
}

static void expect_runs(const char *label, void *code) {
    int c = try_call(code);
    check(label, c == 0, c, 0);
}

static void leg_rw_page(void) {
    char *p = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    put_ret(p);
    expect_fault("RW page", p, SEGV_ACCERR, p);
    mprotect(p, page_size, PROT_READ | PROT_EXEC);
    expect_runs("RX after mprotect runs", p);
    mprotect(p, page_size, PROT_READ | PROT_WRITE);
    expect_fault("RW again after running", p, SEGV_ACCERR, p);
    mprotect(p, page_size, PROT_READ | PROT_WRITE | PROT_EXEC);
    expect_runs("RWX runs", p);
    mprotect(p, page_size, PROT_NONE);
    expect_fault("PROT_NONE", p, SEGV_ACCERR, p);
    munmap(p, page_size);
    expect_fault("unmapped", p, SEGV_MAPERR, p);
}

static void leg_run_off_the_end(void) {
    char *p = mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *second = p + page_size;
    // A no-op at the very end of an executable page, then a return on the
    // next page, which is not executable.
    memcpy(second - sizeof(NOP), NOP, sizeof(NOP));
    put_ret(second);
    mprotect(p, page_size, PROT_READ | PROT_EXEC);
    expect_fault("runs off the end into RW", second - sizeof(NOP), SEGV_ACCERR, second);
    mprotect(second, page_size, PROT_READ | PROT_EXEC);
    expect_runs("...and runs once it is RX", second - sizeof(NOP));
    munmap(p, 2 * page_size);
}

static void leg_mprotect_hole(void) {
    // [A][hole][C], both halves RW with a return in them.
    char *a = mmap(NULL, 3 * page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *c = a + 2 * page_size;
    put_ret(a);
    put_ret(c);
    munmap(a + page_size, page_size);
    errno = 0;
    int r = mprotect(a, 3 * page_size, PROT_READ | PROT_EXEC);
    check("mprotect across a hole fails", r == -1, r, -1);
    check("...with ENOMEM", errno == ENOMEM, errno, ENOMEM);
    expect_runs("...and the pages before the hole run", a);
    expect_fault("...and the pages after it do not", c, SEGV_ACCERR, c);
    errno = 0;
    r = mprotect(a + page_size, 2 * page_size, PROT_READ | PROT_EXEC);
    check("mprotect starting in a hole is ENOMEM", r == -1 && errno == ENOMEM, errno, ENOMEM);
    expect_fault("...and changes nothing", c, SEGV_ACCERR, c);
    munmap(a, page_size);
    munmap(c, page_size);
}

// Returns into a buffer far below the caller's frame, on stack pages that did
// not exist when the stack was made executable.
static __attribute__((noinline)) int call_on_deep_stack(void) {
    size_t depth = 256 * 1024;
    volatile unsigned char *deep = alloca(depth);
    for (size_t i = depth; i >= (size_t) page_size; i -= (size_t) page_size)
        deep[i - 1] = 0;
    unsigned char *code = (unsigned char *) (((uintptr_t) deep + 15) & ~(uintptr_t) 15);
    put_ret(code);
    return try_call(code);
}

static void leg_stack_grows_down(void) {
    char *m = mmap(NULL, page_size, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    errno = 0;
    int r = mprotect(m, page_size, PROT_READ | PROT_GROWSDOWN);
    check("PROT_GROWSDOWN on a plain mapping is EINVAL", r == -1 && errno == EINVAL, errno, EINVAL);
    errno = 0;
    r = mprotect(m, page_size, PROT_READ | PROT_GROWSUP);
    check("PROT_GROWSUP is EINVAL", r == -1 && errno == EINVAL, errno, EINVAL);
    munmap(m, page_size);

    // In a child: the stack stays executable for the rest of its life.
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        failures_total = 0;
        unsigned char buf[64] __attribute__((aligned(16)));
        put_ret(buf);
        expect_fault("stack before PROT_GROWSDOWN", buf, SEGV_ACCERR, buf);
        uintptr_t page = (uintptr_t) buf & ~(uintptr_t) (page_size - 1);
        errno = 0;
        r = mprotect((void *) page, page_size,
                     PROT_READ | PROT_WRITE | PROT_EXEC | PROT_GROWSDOWN);
        check("mprotect(PROT_GROWSDOWN|RWX) on the stack", r == 0, errno, 0);
        expect_runs("...then the stack runs", buf);
        int deep = call_on_deep_stack();
        check("...and so does stack it grows into later", deep == 0, deep, 0);
        fflush(stdout);
        _exit(failures_total ? 1 : 0);
    }
    int st;
    waitpid(pid, &st, 0);
    check("PROT_GROWSDOWN child", WIFEXITED(st) && WEXITSTATUS(st) == 0, st, 0);
}

static void leg_stack(void) {
    // Aligned so the whole instruction is on one page.
    unsigned char buf[64] __attribute__((aligned(16)));
    put_ret(buf);
    expect_fault("stack", buf, SEGV_ACCERR, buf);
}

static void leg_heap(void) {
    unsigned char *h = malloc(64);
    put_ret(h);
    expect_fault("heap", h, SEGV_ACCERR, h);
    free(h);
}

static void leg_read_implies_exec(void) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        failures_total = 0;
        if (personality(READ_IMPLIES_EXEC) < 0) {
            printf("FAIL personality errno=%d\n", errno);
            _exit(1);
        }
        char *p = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        put_ret(p);
        expect_runs("READ_IMPLIES_EXEC: RW mapping runs", p);
        fflush(stdout);
        _exit(failures_total ? 1 : 0);
    }
    int st;
    waitpid(pid, &st, 0);
    check("READ_IMPLIES_EXEC child", WIFEXITED(st) && WEXITSTATUS(st) == 0, st, 0);
}

static void leg_maps(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int stack_seen = 0, stack_x = 0;
    while (f != NULL && fgets(line, sizeof line, f) != NULL) {
        if (strstr(line, "[stack]") != NULL) {
            stack_seen = 1;
            char perms[5] = {0};
            sscanf(line, "%*s %4s", perms);
            stack_x = perms[2] == 'x';
        }
    }
    if (f != NULL)
        fclose(f);
    check("maps has [stack]", stack_seen, stack_seen, 1);
    check("stack is not executable", !stack_x, stack_x, 0);
}

static volatile sig_atomic_t usr1;
static void on_usr1(int sig) {
    (void) sig;
    usr1++;
}

static void leg_signal_return(void) {
    struct sigaction sa = {0};
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    raise(SIGUSR1);
    raise(SIGUSR1);
    check("handlers return", usr1 == 2, usr1, 2);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    page_size = sysconf(_SC_PAGESIZE);
    struct sigaction sa = {0};
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);

    leg_signal_return();
    leg_rw_page();
    leg_run_off_the_end();
    leg_mprotect_hole();
    leg_stack();
    leg_stack_grows_down();
    leg_heap();
    leg_read_implies_exec();
    leg_maps();
    return finish_suite("nx_enforce");
}
