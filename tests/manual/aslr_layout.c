// exec randomizes the address space layout (ASLR).
//
// Every process was laid out the same way every run -- libc and the stack at
// the same addresses each time -- so an exploit could use them as constants.
// Asserted here, as on Linux, by re-executing this program several times and
// comparing what each run reports:
//
//   - by default the stack, an anonymous mmap, libc, the program itself (when
//     it is PIE) and the heap's start all move between runs;
//   - under personality(ADDR_NO_RANDOMIZE) -- setarch -R, gdb's default --
//     they do not move at all, and the flag survives the exec;
//   - /proc/sys/kernel/randomize_va_space reads 2; as root, 0 stops all of
//     it and 1 stops only the heap moving on its own (then put back to 2).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#ifndef ADDR_NO_RANDOMIZE
#define ADDR_NO_RANDOMIZE 0x0040000
#endif

#define RUNS 6

struct report {
    // brk_rel is the heap's start relative to the program: randomize_va_space
    // 2 moves the heap on its own, while 1 moves it only with a PIE program.
    uint64_t stack, mmap, libc, main, brk_rel;
    int pie;
};

static const char *self_path;

static int is_pie(void) {
    int fd = open("/proc/self/exe", O_RDONLY);
    if (fd < 0)
        return 0;
    unsigned char e[20];
    ssize_t n = read(fd, e, sizeof e);
    close(fd);
    // e_type sits at offset 16 in both ELF classes; little-endian here.
    return n == (ssize_t) sizeof e && (e[16] | e[17] << 8) == ET_DYN;
}

static int child_report(int fd) {
    volatile int local = 0;
    struct report r = {0};
    r.stack = (uint64_t) (uintptr_t) &local;
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    r.mmap = (uint64_t) (uintptr_t) m;
    r.libc = (uint64_t) (uintptr_t) dlsym(RTLD_DEFAULT, "malloc");
    r.main = (uint64_t) (uintptr_t) &child_report;
    r.brk_rel = (uint64_t) (uintptr_t) sbrk(0) - r.main;
    r.pie = is_pie();
    return write(fd, &r, sizeof r) == (ssize_t) sizeof r ? 0 : 1;
}

// Run this program RUNS times and collect what each reports.
static int collect(struct report out[RUNS]) {
    for (int i = 0; i < RUNS; i++) {
        int p[2];
        if (pipe(p) < 0)
            return -1;
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            close(p[0]);
            char fdarg[16];
            snprintf(fdarg, sizeof fdarg, "%d", p[1]);
            execl(self_path, self_path, "--report", fdarg, (char *) NULL);
            _exit(127);
        }
        close(p[1]);
        ssize_t n = read(p[0], &out[i], sizeof out[i]);
        close(p[0]);
        int st;
        waitpid(pid, &st, 0);
        if (n != (ssize_t) sizeof out[i])
            return -1;
    }
    return 0;
}

static int distinct(const struct report r[RUNS], size_t off) {
    int count = 0;
    for (int i = 0; i < RUNS; i++) {
        uint64_t v = *(const uint64_t *) ((const char *) &r[i] + off);
        int seen = 0;
        for (int j = 0; j < i; j++)
            if (*(const uint64_t *) ((const char *) &r[j] + off) == v)
                seen = 1;
        count += !seen;
    }
    return count;
}

static void check(const char *label, int ok, long got, long want) {
    if (!ok)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-52s %s (got %ld, want %ld)\n", label, ok ? "ok" : "FAIL", got, want);
}

static const struct { const char *name; size_t off; } fields[] = {
    {"stack", offsetof(struct report, stack)},
    {"mmap", offsetof(struct report, mmap)},
    {"libc", offsetof(struct report, libc)},
    {"main", offsetof(struct report, main)},
    {"heap (from the program)", offsetof(struct report, brk_rel)},
};

// `moves` says which fields must vary (1), stay put (0), or are not asked (-1).
static void expect(const char *what, const int moves[5]) {
    struct report r[RUNS];
    if (collect(r) != 0) {
        failf("collect", 0, 0, 0, 0, 0, 0);
        return;
    }
    for (unsigned f = 0; f < 5; f++) {
        if (moves[f] < 0)
            continue;
        if (f == 3 && !r[0].pie) {
            test_logf("  %s: not a PIE, main not asked\n", what);
            continue;
        }
        int d = distinct(r, fields[f].off);
        char label[96];
        snprintf(label, sizeof label, "%s: %s %s", what, fields[f].name,
                 moves[f] ? "moves" : "stays");
        check(label, moves[f] ? d > 1 : d == 1, d, moves[f] ? RUNS : 1);
    }
}

static int read_va_space(void) {
    FILE *f = fopen("/proc/sys/kernel/randomize_va_space", "r");
    int v = -1;
    if (f != NULL) {
        if (fscanf(f, "%d", &v) != 1)
            v = -1;
        fclose(f);
    }
    return v;
}

static int write_va_space(int v) {
    FILE *f = fopen("/proc/sys/kernel/randomize_va_space", "w");
    if (f == NULL)
        return -1;
    int ok = fprintf(f, "%d\n", v) > 0;
    return fclose(f) == 0 && ok ? 0 : -1;
}

int main(int argc, char **argv) {
    self_path = argv[0];
    if (argc == 3 && strcmp(argv[1], "--report") == 0)
        return child_report(atoi(argv[2]));
    test_init(argc, argv);
    static char abs_self[512];
    if (self_path[0] != '/' && realpath(self_path, abs_self) != NULL)
        self_path = abs_self;

    int va = read_va_space();
    check("randomize_va_space is 2", va == 2, va, 2);
    int pers = personality(0xffffffff);
    check("ADDR_NO_RANDOMIZE clear by default", (pers & ADDR_NO_RANDOMIZE) == 0, pers, 0);

    static const int all_move[5] = {1, 1, 1, 1, 1};
    static const int none_move[5] = {0, 0, 0, 0, 0};
    expect("default", all_move);

    int old = personality(0xffffffff);
    personality(old | ADDR_NO_RANDOMIZE);
    expect("ADDR_NO_RANDOMIZE", none_move);
    personality(old);

    if (geteuid() == 0 && va == 2) {
        if (write_va_space(0) == 0) {
            expect("randomize_va_space=0", none_move);
            static const int all_but_brk[5] = {1, 1, 1, 1, 0};
            write_va_space(1);
            expect("randomize_va_space=1", all_but_brk);
            write_va_space(2);
        } else {
            failf("write randomize_va_space", (uint64_t) errno, 0, 0, 0, 0, 0);
        }
        check("randomize_va_space put back", read_va_space() == 2, read_va_space(), 2);
    } else {
        test_logf("not root: randomize_va_space legs skipped\n");
    }
    return finish_suite("aslr_layout");
}
