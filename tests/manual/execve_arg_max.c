// execve() takes as much argument and environment text as Linux's stack-based limit allows.
//
// Linux (fs/exec.c, 5.10: bprm_stack_limits, copy_strings, copy_string_kernel)
// lets the argument strings, the environment strings, the file name and one
// pointer per argument and variable together take a quarter of RLIMIT_STACK --
// never more than 3/4 of _STK_LIM (6 MiB), never less than ARG_MAX (128 KiB).
// One string may be MAX_ARG_STRLEN (32 pages) long, its NUL included. Past
// either, execve fails E2BIG. `getconf ARG_MAX` reports the quarter of the
// stack limit, and xargs, find -exec + and the shells size commands to it.
//
// iSH-AOK read argv and envp into one fixed 32-page buffer each, so any command
// past 128 KiB of argument text was E2BIG whatever the stack limit: `cp -pn
// "$t"/*` over 2099 names of ~95 bytes said "Argument list too long" while
// getconf said 2 MiB. A #! line's rewritten argv had the same fixed buffer.
//
// Every case finds the exact boundary rather than a comfortable margin: the
// largest command Linux's arithmetic allows must run (the child checks argc
// and a checksum of everything it was handed), and one byte more must be
// E2BIG. That pins every term of the sum -- the file name, the pointers, the
// environment -- because leaving any out lets the one-byte-over case run.
//
//   - the default stack limit, 1 MiB (256 KiB of text), 256 KiB (the 128 KiB
//     floor), and unlimited (the 6 MiB cap), set in the child before exec;
//   - the same total carried mostly by the environment;
//   - one string of MAX_ARG_STRLEN bytes with its NUL, and one byte longer,
//     in argv and in envp;
//   - a #! script with many arguments: the rewritten argv (interpreter, its
//     argument, the script's name, then argv[1..]) is held to the room the
//     ORIGINAL argc and envc left, at the same exact boundary;
//   - a stack limit under the 128 KiB floor: the strings are copied into the
//     new stack as they are counted, a pointer's width below its top, and
//     growing it past RLIMIT_STACK fails that copy with E2BIG. One byte past
//     the last page the limit allows is E2BIG; at it the exec goes ahead (and
//     the program dies for want of stack -- on Linux too, so only "not E2BIG"
//     is asked of it). iSH-AOK killed the child there with SIGSEGV instead.
//
// The pointer size is the KERNEL's: 8 for a 64-bit kernel even under -m32, 4
// on a 32-bit one (iSH-AOK's i386 guests are shown an i686 kernel).
//
// Measured on Linux 6.12 x86_64 (Debian), built 64-bit and -m32. 6.12 differs
// from 5.10 only for an empty argv (5.18+ count it as one pointer), which no
// case here uses.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test_common.h"

#define MAX_ARG_STRLEN (32 * 4096)
#define ARG_MAX_FLOOR 131072
#define STK_LIM (8UL * 1024 * 1024)

#define CHILD_MARK "--argmax-child"
#define EXIT_EXEC_BASE 100      // a child whose execve failed exits 100 + errno
#define EXIT_MISMATCH 3         // the exec'd child saw something else

extern char **environ;

static char self_path[PATH_MAX];
static size_t kptr;             // the kernel's sizeof(void *)

// FNV-1a over each string and its NUL.
static uint64_t sum_strings(uint64_t h, char *const *v, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const unsigned char *p = (const unsigned char *) v[i];
        do {
            h ^= *p;
            h *= 0x100000001b3ULL;
        } while (*p++ != '\0');
    }
    return h;
}

// The exec'd side: argv[k] is CHILD_MARK, argv[k+1] the argc expected,
// argv[k+2] the checksum of argv[k+3..] and then of environ.
static int child_main(int argc, char **argv, int k) {
    if (k + 2 >= argc)
        return EXIT_MISMATCH;
    long want_argc = strtol(argv[k + 1], NULL, 10);
    uint64_t want_sum = strtoull(argv[k + 2], NULL, 16);
    size_t envc = 0;
    while (environ[envc] != NULL)
        envc++;
    uint64_t h = sum_strings(0xcbf29ce484222325ULL, argv + k + 3, (size_t) (argc - k - 3));
    h = sum_strings(h, environ, envc);
    if (want_argc != argc || want_sum != h) {
        fprintf(stderr, "child: argc %d (want %ld) sum %016" PRIx64 " (want %016" PRIx64 ")\n",
                argc, want_argc, h, want_sum);
        return EXIT_MISMATCH;
    }
    return 0;
}

// ------------------------------------------------------------- the builder

struct vec {
    char **v;
    size_t n, cap;
};

static void vec_push(struct vec *v, char *s) {
    if (v->n + 2 > v->cap) {
        v->cap = v->cap ? v->cap * 2 : 64;
        v->v = realloc(v->v, v->cap * sizeof(char *));
        if (v->v == NULL) {
            perror("realloc");
            exit(2);
        }
    }
    v->v[v->n++] = s;
    v->v[v->n] = NULL;
}

static void vec_free(struct vec *v) {
    for (size_t i = 0; i < v->n; i++)
        free(v->v[i]);
    free(v->v);
    *v = (struct vec) {0};
}

static char *filler(size_t len, size_t seed, const char *prefix) {
    size_t plen = strlen(prefix);
    char *s = malloc(len + 1);
    if (s == NULL) {
        perror("malloc");
        exit(2);
    }
    memcpy(s, prefix, plen);
    for (size_t j = plen; j < len; j++)
        s[j] = (char) ('a' + (seed * 7 + j) % 26);
    s[len] = '\0';
    return s;
}

// What Linux charges for a vector's strings and their pointers.
static size_t cost(const struct vec *v) {
    size_t c = 0;
    for (size_t i = 0; i < v->n; i++)
        c += strlen(v->v[i]) + 1 + kptr;
    return c;
}

// Fillers whose cost (bytes + NUL + pointer) adds up to exactly `budget`,
// appended to v. Ordinary ones are 100 bytes; the last is stretched to land
// on the byte. In envp they look like variables.
static void fill(struct vec *v, size_t budget, bool is_env) {
    const char *prefix = is_env ? "AMX=" : "";
    size_t plen = strlen(prefix);
    size_t unit = 100 + 1 + kptr;
    size_t n = budget / unit, r = budget % unit;
    if (r != 0 && r < plen + 1 + kptr) {
        // Too small for a string of its own: fold it into the last one.
        if (n == 0) {
            fprintf(stderr, "fill: budget %zu too small\n", budget);
            exit(2);
        }
        n--;
        r += unit;
    }
    for (size_t i = 0; i < n; i++)
        vec_push(v, filler(100, v->n + i, prefix));
    if (r != 0)
        vec_push(v, filler(r - 1 - kptr, v->n, prefix));
}

// Linux's limit for a stack limit (bprm_stack_limits).
static size_t arg_limit(rlim_t stack) {
    size_t l = STK_LIM / 4 * 3;
    if (stack != RLIM_INFINITY && stack / 4 < l)
        l = (size_t) (stack / 4);
    if (l < ARG_MAX_FLOOR)
        l = ARG_MAX_FLOOR;
    return l;
}

// --------------------------------------------------------------- one exec

#define RLIM_KEEP ((rlim_t) -2)

// Fork, set the stack limit, execve(path, argv, envp). Returns 0 when the
// program ran and saw what it was given, the errno execve failed with, or -1
// for anything else.
static int run(const char *path, struct vec *argv, struct vec *envp, rlim_t stack) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        if (stack != RLIM_KEEP) {
            struct rlimit rl;
            getrlimit(RLIMIT_STACK, &rl);
            rl.rlim_cur = stack;
            if (setrlimit(RLIMIT_STACK, &rl) != 0)
                _exit(EXIT_EXEC_BASE + 99);
        }
        execve(path, argv->v, envp->v);
        int e = errno;
        _exit(EXIT_EXEC_BASE + (e < 99 ? e : 98));
    }
    int status;
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    if (!WIFEXITED(status)) {
        test_logf("  (child killed by signal %d)\n", WTERMSIG(status));
        return -1;
    }
    int code = WEXITSTATUS(status);
    if (code == 0)
        return 0;
    if (code >= EXIT_EXEC_BASE)
        return code - EXIT_EXEC_BASE;
    return -1;
}

static void expect(const char *label, int got, int want) {
    if (got == want) {
        test_logf("  %-56s %s\n", label, want ? strerror(want) : "ran");
        return;
    }
    printf("FAIL %s: got %s, expected %s\n", label,
           got == 0 ? "ran" : got < 0 ? "child failure" : strerror(got),
           want == 0 ? "ran" : strerror(want));
    failures_total++;
}

// The checksum and argc header the child verifies, sized so that writing the
// real values in afterwards changes no byte count.
static void push_header(struct vec *argv, const char *argv0) {
    vec_push(argv, strdup(argv0));
    vec_push(argv, strdup(CHILD_MARK));
    vec_push(argv, strdup("00000000"));
    vec_push(argv, strdup("0000000000000000"));
}

// Fill in the argc the child must see (a #! rewrite changes it) and the
// checksum of the fillers and the environment.
static void seal(struct vec *argv, struct vec *envp, long final_argc) {
    size_t k = 1;   // CHILD_MARK's index in argv
    uint64_t h = sum_strings(0xcbf29ce484222325ULL, argv->v + k + 3, argv->n - k - 3);
    h = sum_strings(h, envp->v, envp->n);
    snprintf(argv->v[k + 1], 9, "%08ld", final_argc);
    snprintf(argv->v[k + 2], 17, "%016" PRIx64, h);
}

// A command whose charge is exactly limit + delta, the filler in argv or envp.
static int exec_at(rlim_t stack, size_t limit, long delta, bool in_env) {
    struct vec argv = {0}, envp = {0};
    push_header(&argv, self_path);
    vec_push(&envp, strdup("AMX_FIXED=1"));
    size_t used = strlen(self_path) + 1 + cost(&argv) + cost(&envp);
    fill(in_env ? &envp : &argv, (size_t) ((long) (limit - used) + delta), in_env);
    seal(&argv, &envp, (long) argv.n);
    int r = run(self_path, &argv, &envp, stack);
    vec_free(&argv);
    vec_free(&envp);
    return r;
}

static void boundary(const char *what, rlim_t stack, bool in_env) {
    rlim_t eff = stack;
    if (stack == RLIM_KEEP) {
        struct rlimit rl;
        getrlimit(RLIMIT_STACK, &rl);
        eff = rl.rlim_cur;
    }
    size_t limit = arg_limit(eff);
    char label[160];
    snprintf(label, sizeof(label), "%s: %zu bytes, exactly the limit", what, limit);
    expect(label, exec_at(stack, limit, 0, in_env), 0);
    snprintf(label, sizeof(label), "%s: one byte over", what);
    expect(label, exec_at(stack, limit, 1, in_env), E2BIG);
}

// One string of `len` bytes plus its NUL, in argv or envp.
static int exec_long_string(size_t len, bool in_env) {
    struct vec argv = {0}, envp = {0};
    push_header(&argv, self_path);
    if (in_env)
        vec_push(&envp, filler(len, 1, "AMX="));
    else
        vec_push(&argv, filler(len, 1, ""));
    seal(&argv, &envp, (long) argv.n);
    int r = run(self_path, &argv, &envp, 8UL * 1024 * 1024);
    vec_free(&argv);
    vec_free(&envp);
    return r;
}

// The strings -- file name, argv, envp, NULs included -- totalling exactly
// `total` bytes, the pointers not counted: the stack-growth bound's terms.
static int exec_strings(rlim_t stack, size_t total) {
    struct vec argv = {0}, envp = {0};
    push_header(&argv, self_path);
    size_t used = strlen(self_path) + 1;
    for (size_t i = 0; i < argv.n; i++)
        used += strlen(argv.v[i]) + 1;
    size_t rem = total - used;
    while (rem > 0) {
        size_t len = rem >= 202 ? 100 : rem > 101 ? rem - 102 : rem - 1;
        vec_push(&argv, filler(len, argv.n, ""));
        rem -= len + 1;
    }
    seal(&argv, &envp, (long) argv.n);
    int r = run(self_path, &argv, &envp, stack);
    vec_free(&argv);
    return r;
}

static void stack_pages_cases(void) {
    rlim_t stack = 64 * 1024;
    size_t at = (size_t) stack - kptr;
    char label[160];
    snprintf(label, sizeof(label), "stack limit 64 KiB: %zu bytes of strings, the last page", at);
    int r = exec_strings(stack, at);
    if (r > 0) {
        printf("FAIL %s: got %s, expected the exec to go ahead\n", label, strerror(r));
        failures_total++;
    } else {
        test_logf("  %-56s %s\n", label, r == 0 ? "ran" : "went ahead (killed)");
    }
    expect("stack limit 64 KiB: one byte past that page", exec_strings(stack, at + 1), E2BIG);
}

// ------------------------------------------------------------------ #!

#define SHEBANG_ARG "-x"

static char script_path[64];

// argv = {"s", CHILD_MARK, ...}: load_script drops argv[0] and pushes the
// script's name (the path execve was given), the #! argument and the
// interpreter, so the child sees {self, "-x", script, CHILD_MARK, ...}. The
// rewritten argv must fit the room the ORIGINAL argc and envc left; argv[0]
// is one byte so the rewrite is what binds.
static int exec_script_at(size_t limit, long delta) {
    struct vec argv = {0}, envp = {0};
    push_header(&argv, "s");
    vec_push(&envp, strdup("AMX_FIXED=1"));
    // Charged by the original count: every pointer and the strings after the
    // rewrite (argv[0] gone, the three added).
    size_t added = strlen(self_path) + 1 + strlen(SHEBANG_ARG) + 1 + strlen(script_path) + 1;
    size_t used = strlen(script_path) + 1 + cost(&argv) - (strlen("s") + 1) + added + cost(&envp);
    fill(&argv, (size_t) ((long) (limit - used) + delta), false);
    seal(&argv, &envp, (long) argv.n + 2);
    int r = run(script_path, &argv, &envp, RLIM_KEEP);
    vec_free(&argv);
    vec_free(&envp);
    return r;
}

static void shebang_cases(void) {
    snprintf(script_path, sizeof(script_path), "/tmp/argmax-%d.sh", (int) getpid());
    int fd = open(script_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        printf("FAIL create %s: %s\n", script_path, strerror(errno));
        failures_total++;
        return;
    }
    char line[PATH_MAX + 16];
    int n = snprintf(line, sizeof(line), "#!%s %s\n", self_path, SHEBANG_ARG);
    if (write(fd, line, (size_t) n) != n) {
        printf("FAIL write %s\n", script_path);
        failures_total++;
    }
    close(fd);

    struct rlimit rl;
    getrlimit(RLIMIT_STACK, &rl);
    size_t limit = arg_limit(rl.rlim_cur);
    char label[160];
    snprintf(label, sizeof(label), "#! script: rewritten argv exactly at %zu", limit);
    expect(label, exec_script_at(limit, 0), 0);
    expect("#! script: rewritten argv one byte over", exec_script_at(limit, 1), E2BIG);
    unlink(script_path);
}

// ------------------------------------------------------------------- main

int main(int argc, char **argv) {
    for (int k = 1; k < argc && k <= 3; k++)
        if (strcmp(argv[k], CHILD_MARK) == 0)
            return child_main(argc, argv, k);
    test_init(argc, argv);
    alarm(test_watchdog_secs(300));

    ssize_t n = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (n <= 0) {
        printf("FAIL readlink /proc/self/exe: %s\n", strerror(errno));
        return 1;
    }
    self_path[n] = '\0';
    if (strlen(self_path) > 100) {
        printf("execve_arg_max: SKIP (binary path too long for a #! line)\n");
        return 0;
    }

    struct utsname u;
    uname(&u);
    bool k64 = strstr(u.machine, "64") != NULL;
    kptr = k64 ? 8 : sizeof(void *);
    struct rlimit rl;
    getrlimit(RLIMIT_STACK, &rl);
    test_logf("kernel %s, pointer %zu, RLIMIT_STACK cur %lld max %lld\n", u.machine, kptr,
              (long long) rl.rlim_cur, (long long) rl.rlim_max);

    boundary("default stack limit", RLIM_KEEP, false);
    boundary("stack limit 1 MiB", 1024 * 1024, false);
    boundary("stack limit 256 KiB (the 128 KiB floor)", 256 * 1024, false);
    if (rl.rlim_max == RLIM_INFINITY)
        boundary("stack limit unlimited (the 6 MiB cap)", RLIM_INFINITY, false);
    else
        printf("  (unlimited stack case skipped: hard limit %lld)\n", (long long) rl.rlim_max);
    boundary("stack limit 1 MiB, filler in envp", 1024 * 1024, true);

    expect("argv string of MAX_ARG_STRLEN bytes with its NUL",
           exec_long_string(MAX_ARG_STRLEN - 1, false), 0);
    expect("argv string one byte longer", exec_long_string(MAX_ARG_STRLEN, false), E2BIG);
    expect("envp string of MAX_ARG_STRLEN bytes with its NUL",
           exec_long_string(MAX_ARG_STRLEN - 1, true), 0);
    expect("envp string one byte longer", exec_long_string(MAX_ARG_STRLEN, true), E2BIG);

    shebang_cases();
    stack_pages_cases();

    return finish_suite("execve_arg_max");
}
