// vfork_exec_stale_jit.c -- a task that execs while ANOTHER task still holds
// the address space it is leaving must not keep executing the old program's
// JIT translations.
//
// The JIT frontends memoize block lookups in a per-HOST-THREAD array keyed by
// guest address (jit/jit.c, struct jit_entry_scratch). That array used to be a
// stack object zeroed on every entry, so staleness was impossible; commit
// 711b48f3 made it persistent to stop memset'ing ~40 KB per guest syscall, and
// guarded it with two counters:
//
//   - a global free counter, bumped whenever a block is freed, so a thread
//     drops pointers that could name freed memory;
//   - per-jit cleanup_seq, bumped when blocks are invalidated into jetsam, so
//     a thread drops translations of code that has been overwritten.
//
// Neither covers the third way a cached pointer goes wrong: it stays perfectly
// valid and simply stops describing THIS address space. execve replaces the
// task's mm -- and so its jit -- while the host thread, and its cache, live on.
// The cache is keyed by guest address alone, so a leftover entry is a HIT
// (block->addr == ip, not jetsam) whenever the new image reuses an address the
// old one had translated, and the frontend then runs the previous program's
// code.
//
// The free counter looks like it should have caught this, and for an ordinary
// exec it does: the task's own jit drops to zero references, is freed, and the
// counter moves. What defeats it is a SHARED address space. After vfork (or any
// CLONE_VM without CLONE_THREAD) the parent still holds that jit, so the child's
// exec frees nothing, the counter never moves, and the stale entries are not
// merely stale but live.
//
// That is dash's implicit exec of the last command in a `-c` script, which is
// why this presented as "a riscv64 glibc binary segfaults at startup, but only
// when a bare PATH-resolved command is dash's last command". `exec uname` and
// `(uname)` both worked -- the first execs in place and frees its own jit, the
// second forks a private copy which is freed at the exec. It showed up as wild
// jumps to unmapped data-shaped addresses (0x20000b4200) and near-NULL derefs
// inside ld.so, always while ld.so was mapping libc. Instrumented: after the
// exec, 58 entries from the previous program survived and one was hit at an ip
// the new jit had no block for at all.
//
// Nothing about it is riscv64-specific -- the memo lives in the shared frontend
// machinery and all four (i386, amd64, arm64, riscv64) took the same change --
// so this test is arch-neutral and is built for every guest.
//
// Shape of the test: a child that shares the parent's mm (CLONE_VM|CLONE_VFORK,
// with its OWN stack, so nothing here depends on the vfork shared-stack hazard
// dash relies on) warms the JIT across ld.so, libc and its own text, then execs
// a different program while the parent -- and therefore the old jit -- stays
// alive. The exec'd program must run and exit normally. Dying on a signal is
// the regression.
//
// THE EXEC TARGET'S SIZE IS THE WHOLE DIFFICULTY, and it is why this execs a
// sweep of purpose-built peers rather than one binary, let alone a system one.
// AOK asks pt_find_hole() for an image's total mapped page span (kernel/exec.c
// find_hole_for_elf), so two images share a load address if and only if they
// round to the same number of pages -- and the cache is keyed by address, so
// only an image that lands on top of this one can reuse its translations.
// Measured on this test, unfixed: of peers padded 0..8 pages, EXACTLY ONE
// (pad=1) collides, and it fails 24/24 while every other size passes 24/24.
// A single fixed peer is therefore a coin flip that any edit to this file
// re-tosses -- adding the peer-lookup helper below was by itself enough to move
// the collision off a fixed peer and turn 24/24 into 0/24. Padding sweeps the
// span across a window instead, so one peer always lands, whatever this file
// grows into. Same reason a system binary is no good: uname happens to collide
// today and /bin/sh (~72 KiB larger) reproduces nothing, and a package update
// re-tosses that too.
//
// A/B: on the unfixed build the colliding peer kills every child with SIGSEGV;
// the fixed build passes the whole sweep. Also passes on real Linux, where the
// sequence is unremarkable and no peer can collide with anything.
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "test_common.h"

#define ROUNDS_PER_PEER 4
#define PEER_PAD_PAGES 12 // peers are built with 0..PEER_PAD_PAGES-1 pad pages
#define EXEC_STATUS 7     // what a peer returns
#define SUITE "vfork_exec_stale_jit"
#define PEER_STEM SUITE "_peer"
#define CHILD_STACK_SIZE (256 * 1024)

static char peers[PEER_PAD_PAGES][PATH_MAX];
static unsigned peer_count;

// The peers are built into the same directory as this binary. Prefer
// /proc/self/exe over argv[0]: the harness invokes tests by full path, but a
// hand-run need not. ISH_STALE_JIT_PEER names a single peer instead, for
// running this outside the harness.
static void find_peers(const char *argv0) {
    const char *override = getenv("ISH_STALE_JIT_PEER");
    if (override != NULL) {
        if (access(override, X_OK) == 0) {
            snprintf(peers[0], PATH_MAX, "%s", override);
            peer_count = 1;
        }
        return;
    }

    char self[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = '\0';
    } else if (argv0 != NULL && strchr(argv0, '/') != NULL) {
        snprintf(self, sizeof(self), "%s", argv0);
    } else {
        return;
    }
    char *slash = strrchr(self, '/');
    if (slash == NULL)
        return;
    *slash = '\0';

    for (unsigned i = 0; i < PEER_PAD_PAGES; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s%u", self, PEER_STEM, i);
        if (access(path, X_OK) == 0)
            snprintf(peers[peer_count++], PATH_MAX, "%s", path);
    }
}

// Warm the JIT the way a shell startup does, so the per-thread memo is well
// populated before the exec. The first call to each libc symbol runs the
// loader's lazy-binding path, which is where the original crash's stale hit
// landed, so breadth across DISTINCT symbols matters more than iteration count.
__attribute__((noinline))
static long warm_up_jit(long seed) {
    char buf[128];
    long acc = seed;

    for (int i = 0; i < 64; i++) {
        snprintf(buf, sizeof(buf), "%d-%lx-%s", i, acc, "warm");
        acc += (long) strlen(buf);
        acc ^= strtol(buf, NULL, 16);
        if (strchr(buf, '-') != NULL)
            acc++;
        if (strrchr(buf, '-') != NULL)
            acc++;
        if (strstr(buf, "warm") != NULL)
            acc++;
        acc += memcmp(buf, "0", 1) != 0;
        acc += strcmp(buf, "x") != 0;
        acc += strncmp(buf, "0", 1) != 0;
        acc += isalpha((unsigned char) buf[0]) != 0;
        acc += isdigit((unsigned char) buf[0]) != 0;
        acc += toupper((unsigned char) buf[0]);
        void *p = malloc(64 + (size_t) (i & 31));
        if (p != NULL) {
            memset(p, i, 32);
            acc += ((unsigned char *) p)[0];
            void *q = realloc(p, 128);
            if (q != NULL)
                p = q;
            free(p);
        }
        acc += abs((int) acc & 0xffff);
    }

    // Syscalls too: each one leaves and re-enters the JIT, and the frontend
    // consults the memo at exactly that boundary.
    for (int i = 0; i < 8; i++) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        acc += (long) ts.tv_nsec;
        acc += getpid();
        struct stat st;
        if (stat("/", &st) == 0)
            acc += (long) st.st_mode;
    }
    return acc;
}

static volatile long warm_sink;
static const char *child_target;

static int child_main(void *arg) {
    warm_sink = warm_up_jit((long) (intptr_t) arg);
    char *const argv[] = {(char *) PEER_STEM, NULL};
    execv(child_target, argv);
    // Only reached if the exec itself failed; distinguish it from the
    // regression, which kills the child with a signal AFTER a successful exec.
    _exit(90);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(180));

    find_peers(argv[0]);
    if (peer_count == 0) {
        printf("%s: SKIP no %sN binaries next to this one; build them alongside "
               "(setup-regressions.sh does) or set ISH_STALE_JIT_PEER\n", SUITE, PEER_STEM);
        return 0;
    }
    test_logf("peers: %u\n", peer_count);

    void *stack = malloc(CHILD_STACK_SIZE);
    if (stack == NULL) {
        printf("out of memory for child stack\n");
        failures_total++;
        return finish_suite(SUITE);
    }

    unsigned signaled = 0, wrong_status = 0, exec_failed = 0, ok = 0, attempts = 0;
    int first_signal = 0;
    const char *first_bad_peer = NULL;

    for (unsigned p = 0; p < peer_count; p++) {
        child_target = peers[p];
        for (int round = 0; round < ROUNDS_PER_PEER; round++) {
            attempts++;
            // CLONE_VM so the child shares -- and the parent keeps holding --
            // the address space the child is about to exec out of. CLONE_VFORK
            // to match dash, and because it makes the ordering deterministic:
            // the parent does not resume until the child has exec'd. SIGCHLD so
            // the child is reapable with waitpid().
            pid_t pid = clone(child_main, (char *) stack + CHILD_STACK_SIZE,
                              CLONE_VM | CLONE_VFORK | SIGCHLD,
                              (void *) (intptr_t) round);
            if (pid < 0) {
                printf("clone: %s\n", strerror(errno));
                free(stack);
                failures_total++;
                return finish_suite(SUITE);
            }

            int status = 0;
            if (waitpid(pid, &status, 0) != pid) {
                printf("waitpid(%d): %s\n", (int) pid, strerror(errno));
                free(stack);
                failures_total++;
                return finish_suite(SUITE);
            }

            if (WIFSIGNALED(status)) {
                signaled++;
                if (first_signal == 0) {
                    first_signal = WTERMSIG(status);
                    first_bad_peer = peers[p];
                }
                test_log_if(0, "%s round %d: killed by signal %d (%s)\n",
                            peers[p], round, WTERMSIG(status),
                            strsignal(WTERMSIG(status)));
            } else if (!WIFEXITED(status)) {
                wrong_status++;
            } else if (WEXITSTATUS(status) == 90) {
                exec_failed++;
            } else if (WEXITSTATUS(status) != EXEC_STATUS) {
                wrong_status++;
                test_log_if(0, "%s round %d: exit %d, expected %d\n",
                            peers[p], round, WEXITSTATUS(status), EXEC_STATUS);
            } else {
                ok++;
            }
        }
    }
    free(stack);

    if (exec_failed == attempts) {
        printf("%s: SKIP could not exec any peer\n", SUITE);
        return 0;
    }

    test_logf("attempts=%u ok=%u signaled=%u wrong_status=%u exec_failed=%u\n",
              attempts, ok, signaled, wrong_status, exec_failed);

    if (signaled != 0) {
        printf("%u/%u exec'd children died on a signal (first: %d %s, peer %s) -- "
               "the new program ran the previous one's JIT translations\n",
               signaled, attempts, first_signal, strsignal(first_signal),
               first_bad_peer);
        failures_total++;
    }
    if (wrong_status != 0 || exec_failed != 0) {
        printf("%u/%u wrong exit status, %u/%u exec failures\n",
               wrong_status, attempts, exec_failed, attempts);
        failures_total++;
    }
    if (failures_total == 0)
        test_logf("%u shared-mm execs over %u peer sizes, no stale translation reused\n",
                  attempts, peer_count);
    return finish_suite(SUITE);
}
