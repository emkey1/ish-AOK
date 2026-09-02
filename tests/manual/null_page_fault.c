// A dereference of an unmapped low address must fault, and must NOT be
// quietly turned into a fresh zero page by the stack's MAP_GROWSDOWN region.
//
// emu/memory.c's mem_ptr()/mem_ptr_fault() extend a growsdown region onto an
// unmapped page whenever the next mapped page ANYWHERE above it is growsdown.
// Linux's expand_downwards() bounds that two ways -- never below mmap_min_addr
// (65536), and never more than RLIMIT_STACK below the stack -- but AOK had no
// bound at all. In every 64-bit guest layout the stack (~4 GiB) is the LOWEST
// mapping in the address space, with the executable and all libraries far
// above it at 0x7fff_...., so *every* address below the stack, the whole NULL
// page included, silently allocated zero-filled memory instead of faulting: a
// guest NULL read returned 0 and a guest NULL store SUCCEEDED.
//
// Real-world cost of that: foot(1)'s render workers passed a NULL
// pixman_image_t to pixman_image_fill_rectangles() on device. Instead of a
// SIGSEGV on the first field read, _pixman_image_validate() read a stale
// non-NULL "transform" pointer out of the auto-mapped NULL page (left there by
// an earlier NULL store) and died three loads later on a garbage address, with
// nothing in the crash report pointing back at the NULL that caused it.
//
// The reads/writes here are deliberate NULL/wild-pointer accesses, so each one
// runs in its own forked child and is judged by its wait status. Also checks
// that ordinary stack growth still works, since that is what the growsdown
// path exists for: this must not be fixed by refusing to grow the stack.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

static void check(int cond, const char *what) {
    if (cond) {
        test_logf("ok: %s\n", what);
    } else {
        printf("FAIL: %s (errno=%d %s)\n", what, errno, strerror(errno));
        failures_total++;
    }
}

enum access_kind { DO_READ, DO_WRITE };

// Run one access in a child. Returns 1 if the child died on SIGSEGV/SIGBUS
// (the correct outcome), 0 if it survived (the bug).
static int access_faults(uintptr_t addr, enum access_kind kind) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        check(0, "fork");
        return 0;
    }
    if (pid == 0) {
        volatile unsigned char *p = (volatile unsigned char *) addr;
        if (kind == DO_READ) {
            unsigned char v = *p;
            // Keep the load from being optimized away, and make the "survived"
            // path observable in verbose runs.
            _exit(v == 0 ? 40 : 41);
        }
        *p = 0x5a;
        _exit(42);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        check(0, "waitpid");
        return 0;
    }
    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        test_logf("  %#zx %s -> signal %d\n", (size_t) addr,
                  kind == DO_READ ? "read" : "write", sig);
        return sig == SIGSEGV || sig == SIGBUS;
    }
    test_logf("  %#zx %s -> SURVIVED (exit %d)\n", (size_t) addr,
              kind == DO_READ ? "read" : "write",
              WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return 0;
}

// Get an address that is definitely NOT mapped, near `hint` if the layout
// allows it: map one page (a plain hint, never MAP_FIXED, so nothing existing
// can be clobbered) and hand back the address after unmapping it. Picking
// "surely unmapped" wild-pointer addresses by hand does not survive contact
// with four guest layouts -- 16 MiB is amd64's and riscv64's mmap_floor, so a
// literal 0x1000000 probe lands on a real mapping there.
static int unmapped_addr_near(uintptr_t hint, uintptr_t *out) {
    void *p = mmap((void *) hint, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return 0;
    if (munmap(p, 4096) != 0)
        return 0;
    *out = (uintptr_t) p;
    return 1;
}

static void check_faults(uintptr_t addr, const char *what) {
    char label[128];
    snprintf(label, sizeof(label), "read of %s (%#zx) faults", what, (size_t) addr);
    check(access_faults(addr, DO_READ), label);
    snprintf(label, sizeof(label), "write to %s (%#zx) faults", what, (size_t) addr);
    check(access_faults(addr, DO_WRITE), label);
}

// Recurse with a large-ish frame so the main stack really has to grow. 60 x
// 64 KiB stays under the 8 MiB default RLIMIT_STACK, so this is required to
// work on Linux and under AOK alike.
static volatile unsigned char stack_sink;
static void grow_stack(int depth) {
    char frame[64 * 1024];
    memset(frame, (unsigned char) depth, sizeof(frame));
    stack_sink = (unsigned char) frame[depth];
    if (depth > 0)
        grow_stack(depth - 1);
}

static int stack_growth_works(void) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        grow_stack(60);
        _exit(0);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    // The NULL page itself: the field offsets a real NULL-struct dereference
    // lands on, not just offset 0.
    check_faults(0x0, "NULL");
    check_faults(0x30, "NULL + 0x30");
    check_faults(0x38, "NULL + 0x38");

    // Everything under mmap_min_addr is off limits on Linux.
    check_faults(0x1000, "one page up");
    check_faults(0xfff0, "just below mmap_min_addr");

    // Wild pointers well above mmap_min_addr but below the stack. These are
    // where a truncated/garbage pointer most often lands, and they were
    // auto-mapped too -- the unbounded growsdown scan reached the stack from
    // anywhere below it.
    static const uintptr_t hints[] = {0x1000000, 0x20000000, 0x40000000};
    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        uintptr_t wild;
        if (!unmapped_addr_near(hints[i], &wild)) {
            test_logf("skip: no free page near %#zx (%s)\n",
                      (size_t) hints[i], strerror(errno));
            continue;
        }
        check_faults(wild, "unmapped page below the stack");
    }

    // ...but a page that is genuinely just below the stack still gets mapped,
    // which is the whole point of MAP_GROWSDOWN.
    check(stack_growth_works(), "ordinary stack growth to ~3.8 MiB still works");

    // A page the process really did map at a low address must stay usable:
    // this bounds the implicit growsdown extension, not explicit mappings.
    void *low = mmap((void *) 0x20000000, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (low == MAP_FAILED) {
        test_logf("skip: no anonymous page available (%s)\n", strerror(errno));
    } else {
        *(volatile unsigned char *) low = 0x5a;
        check(*(volatile unsigned char *) low == 0x5a,
              "explicitly mapped page is readable/writable");
        munmap(low, 4096);
        check_faults((uintptr_t) low, "that page after munmap");
    }

    return finish_suite("null_page_fault");
}
