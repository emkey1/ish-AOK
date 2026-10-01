// A forked child runs the code its memory holds, whatever the parent's JIT has.
//
// AOK lets a forked child copy its parent's translated blocks instead of
// translating them again, and a child's own translations go back to the
// parent for its later children (jit_fork in jit/jit.c). That is right only
// while the code is the same memory in both; once either side writes the
// page, each must run what ITS memory holds. Each case below writes a tiny
// function into an RWX page, runs it on both sides of a fork, and checks the
// value it returns:
//   (a) the parent rewrites after the fork: the child still gets the original
//   (b) a child rewrites: the parent, and a later child, keep the original
//   (c) code only children ran: a second child gets it from the first (on
//       AOK, copied), then the parent rewrites it and a third child gets the
//       new value
//   (d) the parent exits or execs while its child runs, many times over
// and, on AOK as root, that children copy blocks at all (/proc/ish/jit_timing),
// so the cases above exercise the copy rather than plain translation.
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

static int fails;
#define CHECK(cond, ...) do { if (cond) { test_logf("ok: " __VA_ARGS__); test_logf("\n"); } \
    else { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

typedef int (*fn_t)(void);

// Write "return k" at p.
static void emit(unsigned char *p, int k) {
#if defined(__aarch64__)
    uint32_t insn[2] = { 0x52800000u | ((uint32_t) k << 5), 0xd65f03c0u };   // movz w0, #k; ret
    memcpy(p, insn, sizeof insn);
#elif defined(__riscv)
    uint32_t insn[2] = { 0x00000513u | ((uint32_t) k << 20), 0x00008067u };  // li a0, k; ret
    memcpy(p, insn, sizeof insn);
#elif defined(__x86_64__) || defined(__i386__)
    p[0] = 0xb8;                    // mov eax, k
    memcpy(p + 1, &k, 4);
    p[5] = 0xc3;                    // ret
#else
#error unsupported architecture
#endif
    __builtin___clear_cache((char *) p, (char *) p + 16);
}

static int call(unsigned char *p) {
    return ((fn_t) (uintptr_t) p)();
}

// Run p in a child, after `before` (if any) ran in it; the child's exit status
// is what p returned.
static int in_child(unsigned char *p, void (*before)(unsigned char *)) {
    pid_t pid = fork();
    if (pid == 0) {
        if (before != NULL)
            before(p);
        _exit(call(p));
    }
    int status;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static void rewrite_to_3(unsigned char *p) {
    emit(p, 3);
}

static long proc_read(const char *key) {
    FILE *f = fopen("/proc/ish/jit_timing", "r");
    if (f == NULL)
        return -1;
    char name[32];
    long value, found = -1;
    while (fscanf(f, "%31s %ld\n", name, &value) == 2)
        if (strcmp(name, key) == 0)
            found = value;
    fclose(f);
    return found;
}

static int proc_write(const char *path, const char *value) {
    FILE *f = fopen(path, "w");
    if (f == NULL)
        return -1;
    int ok = fputs(value, f) >= 0;
    return fclose(f) == 0 && ok ? 0 : -1;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    long page = sysconf(_SC_PAGESIZE);
    unsigned char *code = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) {
        printf("jit_fork_inherit: SKIP (no RWX mapping: %s)\n", strerror(errno));
        return 0;
    }
    unsigned char *a = code, *b = code + page, *c = code + 2 * page;

    // (a) the parent rewrites after the fork
    emit(a, 1);
    CHECK(call(a) == 1, "(a) parent runs the original");
    int pipefd[2];
    if (pipe(pipefd) != 0) { perror("pipe"); return 2; }
    pid_t pid = fork();
    if (pid == 0) {
        char x;
        close(pipefd[1]);
        if (read(pipefd[0], &x, 1) != 1) _exit(99);
        _exit(call(a));
    }
    close(pipefd[0]);
    emit(a, 2);
    CHECK(call(a) == 2, "(a) parent runs its rewrite");
    if (write(pipefd[1], "x", 1) != 1) { perror("write"); return 2; }
    close(pipefd[1]);
    int status;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "(a) the child runs the original its memory holds (status %#x)", status);

    // (b) a child rewrites
    emit(b, 1);
    CHECK(call(b) == 1, "(b) parent runs the original");
    int r = in_child(b, rewrite_to_3);
    CHECK(r == 3, "(b) the child runs its rewrite (%d)", r);
    CHECK(call(b) == 1, "(b) the parent still runs the original");
    r = in_child(b, NULL);
    CHECK(r == 1, "(b) a later child runs the original (%d)", r);

    // (c) code only children ran
    emit(c, 5);
    r = in_child(c, NULL);
    CHECK(r == 5, "(c) first child (%d)", r);
    r = in_child(c, NULL);
    CHECK(r == 5, "(c) second child (%d)", r);
    emit(c, 6);
    r = in_child(c, NULL);
    CHECK(r == 6, "(c) after the parent's rewrite a child runs the new code (%d)", r);
    CHECK(call(c) == 6, "(c) and so does the parent");

    // (d) the parent goes away while its child runs: a middle process forks
    // the worker and then exits or execs at once; this process reaps both.
    prctl(PR_SET_CHILD_SUBREAPER, 1);
    int bad = 0;
    for (int i = 0; i < 40; i++) {
        pid_t mid = fork();
        if (mid == 0) {
            pid_t worker = fork();
            if (worker == 0) {
                char buf[64];
                int sum = 0;
                for (int j = 0; j < 200; j++) {
                    snprintf(buf, sizeof buf, "%d %x %s", j, j * 7, "w");
                    sum += (int) strlen(buf) + call(a) + call(b);
                }
                _exit(sum > 0 ? 42 : 1);
            }
            if (i % 2 == 0)
                _exit(0);
            execl("/bin/true", "true", (char *) NULL);
            _exit(0);
        }
        for (int reaped = 0; reaped < 2;) {
            pid_t w = waitpid(-1, &status, 0);
            if (w < 0)
                break;
            reaped++;
            if (w != mid && !(WIFEXITED(status) && WEXITSTATUS(status) == 42))
                bad++;
        }
    }
    CHECK(bad == 0, "(d) workers whose parent exited or exec'd all ran right (%d bad)", bad);

    // On AOK: children did copy blocks (needs root to write the knob).
    if (access("/proc/ish/jit_timing", F_OK) == 0) {
        if (proc_write("/proc/ish/jit_timing", "1") != 0) {
            test_logf("not root: inheritance count not checked\n");
        } else {
            for (int i = 0; i < 5; i++)
                in_child(a, NULL);
            long inherited = proc_read("inherited");
            proc_write("/proc/ish/jit_timing", "0");
            CHECK(inherited > 0, "children copied their parent's blocks (%ld)", inherited);
        }
    }

    printf("jit_fork_inherit: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
