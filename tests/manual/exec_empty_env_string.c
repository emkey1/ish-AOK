// execve() hands the new program the environment it was given, entry for entry.
// An empty string is an ordinary entry: putenv("") is rejected by libc, but
// execve takes the array the caller builds, and a program that builds one by
// hand can put an empty string anywhere in it. Linux copies it like any other
// string -- envc counts it, environ[] holds it, and /proc/<pid>/environ has a
// bare NUL where it sits.
//
// AOK-Workstation TRUNCATED the environment there. The execve syscalls read argv and
// envp into packed blocks -- "s1\0s2\0...\0\0", each string NUL-terminated and
// one more NUL ending the list -- and read_execve_user_args returned how many
// strings it had read. Only argc was kept. do_execve then recovered envc by
// scanning the block for the end of the list, and an empty string IS an empty
// string followed by the terminator's byte: the scan stopped at the first one.
// A child exec'd with {"A=1", "", "B=2"} came up with A=1 and nothing else.
//
// The same scan sat in exec_fixup_term, so an empty variable before TERM also
// hid TERM from the boot-console rewrite.
//
// The packed format simply cannot answer "how many strings" once one of them
// can be empty -- which is why argc has always been passed alongside the block
// rather than recovered from it. envc now travels the same way.
//
// Every expectation below was measured on Linux 6.12 x86_64 (Devuan 6), built
// 64-bit and -m32, as root and as an unprivileged user. Both execve and
// execveat are driven, because both syscalls read their arguments through the
// same helper.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif

#define CHILD_ARGV0 "eees-child"

// Exit codes the child uses for what is not a test result.
#define EXIT_EXEC_FAILED 97
#define EXIT_EXEC_ENOSYS 98

extern char **environ;

static const char *self_exe;

// ------------------------------------------------------------------- cases

// argv[1] names the case, so the child can look its expectations back up. Each
// argv therefore starts {CHILD_ARGV0, "<name>", ...}.
struct ecase {
    const char *name;
    const char *const *argv;    // NULL-terminated
    const char *const *envp;    // NULL-terminated; what the child must SEE
    bool envp_is_null;          // pass envp as NULL rather than as the array
};

static const char *const argv_start[]   = {CHILD_ARGV0, "start", NULL};
static const char *const argv_middle[]  = {CHILD_ARGV0, "middle", NULL};
static const char *const argv_end[]     = {CHILD_ARGV0, "end", NULL};
static const char *const argv_runs[]    = {CHILD_ARGV0, "runs", NULL};
static const char *const argv_only[]    = {CHILD_ARGV0, "only", NULL};
static const char *const argv_none[]    = {CHILD_ARGV0, "none", NULL};
static const char *const argv_null[]    = {CHILD_ARGV0, "null", NULL};
// The control: empty strings in ARGV, which argc has always carried correctly.
// It shares the middle case's environment so one run covers both blocks.
static const char *const argv_empties[] = {CHILD_ARGV0, "empties", "", "x", "", NULL};

static const char *const env_start[]  = {"", "EEES_A=1", "EEES_B=2", NULL};
static const char *const env_middle[] = {"EEES_A=1", "", "EEES_B=2", NULL};
static const char *const env_end[]    = {"EEES_A=1", "EEES_B=2", "", NULL};
static const char *const env_runs[]   = {"EEES_A=1", "", "", "EEES_B=2", "", NULL};
static const char *const env_only[]   = {"", NULL};
static const char *const env_empty[]  = {NULL};

static const struct ecase cases[] = {
    {"start",   argv_start,   env_start,  false},
    {"middle",  argv_middle,  env_middle, false},
    {"end",     argv_end,     env_end,    false},
    {"runs",    argv_runs,    env_runs,   false},
    {"only",    argv_only,    env_only,   false},
    {"none",    argv_none,    env_empty,  false},
    // "Do not take advantage of this nonstandard and nonportable misfeature!"
    // -- execve(2). Linux counts a NULL envp as zero entries; the point here is
    // that the envc now carried alongside the block is zero and not garbage.
    {"null",    argv_null,    env_empty,  true},
    {"empties", argv_empties, env_middle, false},
};

static const struct ecase *find_case(const char *name) {
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        if (strcmp(cases[i].name, name) == 0)
            return &cases[i];
    return NULL;
}

// ----------------------------------------------------------------- helpers

static size_t vec_len(const char *const *v) {
    size_t n = 0;
    while (v[n] != NULL)
        n++;
    return n;
}

// The strings back to back, each with its own NUL and nothing after the last:
// exactly what /proc/<pid>/cmdline and environ return.
static size_t pack(char *buf, const char *const *v) {
    size_t len = 0;
    for (size_t i = 0; v[i] != NULL; i++) {
        size_t one = strlen(v[i]) + 1;
        memcpy(buf + len, v[i], one);
        len += one;
    }
    return len;
}

static void escape_print(const char *label, const char *buf, size_t len) {
    printf("  %s (%zu bytes): \"", label, len);
    size_t shown = len > 160 ? 160 : len;
    for (size_t i = 0; i < shown; i++) {
        unsigned char c = (unsigned char) buf[i];
        if (c == 0)
            printf("\\0");
        else if (c < 32 || c > 126)
            printf("\\x%02x", c);
        else
            putchar(c);
    }
    printf("%s\"\n", shown < len ? "..." : "");
}

static ssize_t read_whole(const char *path, char *buf, size_t max) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t len = 0;
    for (;;) {
        ssize_t n = read(fd, buf + len, max - len);
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0)
            break;
        len += (size_t) n;
        if (len == max)
            break;
    }
    close(fd);
    return (ssize_t) len;
}

// ------------------------------------------------------------------- child

#define BUFSZ 4096

static unsigned child_bad;

static void child_failf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    child_bad++;
}

static void check_vector(const char *what, const char *case_name,
        const char *const *want, char **got) {
    size_t want_n = vec_len(want);
    size_t got_n = 0;
    while (got[got_n] != NULL)
        got_n++;
    if (got_n != want_n) {
        child_failf("FAIL %s %s count got=%zu want=%zu\n", case_name, what, got_n, want_n);
        for (size_t i = 0; i < got_n; i++)
            printf("  got[%zu] = \"%s\"\n", i, got[i]);
        return;
    }
    for (size_t i = 0; i < want_n; i++) {
        if (strcmp(got[i], want[i]) != 0)
            child_failf("FAIL %s %s[%zu] got=\"%s\" want=\"%s\"\n",
                    case_name, what, i, got[i], want[i]);
    }
}

static void check_proc(const char *what, const char *case_name, const char *path,
        const char *const *want) {
    static char want_buf[BUFSZ];
    static char got_buf[BUFSZ];
    size_t want_len = pack(want_buf, want);
    ssize_t got_len = read_whole(path, got_buf, sizeof(got_buf));
    if (got_len < 0) {
        child_failf("FAIL %s %s unreadable: %s\n", case_name, what, strerror(errno));
        return;
    }
    if ((size_t) got_len != want_len || memcmp(got_buf, want_buf, want_len) != 0) {
        child_failf("FAIL %s %s bytes differ\n", case_name, what);
        escape_print("got ", got_buf, (size_t) got_len);
        escape_print("want", want_buf, want_len);
    }
}

static int child_main(int argc, char **argv) {
    if (argc < 2 || argv[1] == NULL) {
        printf("FAIL child: no case name\n");
        return 1;
    }
    const struct ecase *c = find_case(argv[1]);
    if (c == NULL) {
        printf("FAIL child: unknown case \"%s\"\n", argv[1]);
        return 1;
    }

    // The control. argc is passed to the loader explicitly, so empty arguments
    // were never at risk; a failure here is the harness, not the kernel.
    check_vector("argv", c->name, c->argv, argv);
    check_proc("cmdline", c->name, "/proc/self/cmdline", c->argv);

    // The environment, by the same two readings: what the loader put on the
    // stack for the C runtime, and what the kernel says the range holds.
    check_vector("environ", c->name, c->envp, environ);
    check_proc("environ", c->name, "/proc/self/environ", c->envp);

    fflush(stdout);
    return child_bad != 0 ? 1 : 0;
}

// ------------------------------------------------------------------ parent

static bool execveat_missing;

static void run_case(const struct ecase *c, bool use_execveat) {
    const char *how = use_execveat ? "execveat" : "execve";
    if (use_execveat && execveat_missing)
        return;
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        printf("FAIL %s/%s fork: %s\n", c->name, how, strerror(errno));
        failures_total++;
        return;
    }
    if (pid == 0) {
        char *const *av = (char *const *) (const void *) c->argv;
        char *const *ev = c->envp_is_null ? NULL : (char *const *) (const void *) c->envp;
        if (use_execveat) {
#ifdef SYS_execveat
            syscall(SYS_execveat, AT_FDCWD, self_exe, av, ev, 0);
#else
            errno = ENOSYS;
#endif
        } else {
            execve(self_exe, av, ev);
        }
        _exit(errno == ENOSYS ? EXIT_EXEC_ENOSYS : EXIT_EXEC_FAILED);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        printf("FAIL %s/%s waitpid: %s\n", c->name, how, strerror(errno));
        failures_total++;
        return;
    }
    if (!WIFEXITED(status)) {
        printf("FAIL %s/%s child status %#x\n", c->name, how, status);
        failures_total++;
        return;
    }
    int rc = WEXITSTATUS(status);
    if (rc == EXIT_EXEC_ENOSYS && use_execveat) {
        printf("%s: SKIP execveat leg (ENOSYS)\n", "exec_empty_env_string");
        execveat_missing = true;
        return;
    }
    if (rc == EXIT_EXEC_FAILED) {
        printf("FAIL %s/%s could not exec %s\n", c->name, how, self_exe);
        failures_total++;
        return;
    }
    if (rc != 0) {
        // The child has already printed which expectation it missed.
        printf("FAIL %s/%s (child exit %d)\n", c->name, how, rc);
        failures_total++;
        return;
    }
    test_logf("ok %s/%s\n", c->name, how);
}

int main(int argc, char **argv) {
    if (argc > 0 && argv[0] != NULL && strcmp(argv[0], CHILD_ARGV0) == 0)
        return child_main(argc, argv);
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));

    static char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) {
        printf("exec_empty_env_string: SKIP (no /proc/self/exe)\n");
        return 0;
    }
    exe[n] = '\0';
    self_exe = exe;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_case(&cases[i], false);
        run_case(&cases[i], true);
    }
    return finish_suite("exec_empty_env_string");
}
