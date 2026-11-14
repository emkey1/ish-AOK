// native_tar_quoting.c -- SmallCLUE's tar names against GNU tar 1.35's.
//
// GNU tar's default --quoting-style is escape, so every member name it lists
// has its backslashes doubled and its control characters, DEL and (outside a
// UTF-8 locale) high bytes spelled \ooo. SmallCLUE's tar printed names
// straight through (found 2026-10-09, fixed 2026-11-14), which is wrong twice
// over: a name is ambiguous once a backslash in it can also have come from an
// escape, and an archive carrying an escape sequence in a member name could
// drive the terminal of whoever ran `tar t` on it before extracting anything.
//
// Every expectation below is what GNU tar 1.35 printed for this exact
// archive, under TZ=UTC and LC_ALL=C in build/devuan-arm64-test. The archive
// is built here rather than by a tar, so the test depends on no other
// implementation and the bytes cannot drift.
//
// Skips wherever the native multi-call binary is missing, real Linux included.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define SMALLCLUE "/AOK/native/smallclue"
#define DIR "/tmp/native_tar_quoting"
#define TAR DIR "/bin/tar"
#define ARCHIVE DIR "/q.tar"

struct member {
    const char *name;
    char type;
    const char *link;
};

// One member per class GNU quotes, plus a symlink and a hard link so that the
// long listing's "-> " and "link to " targets are covered too.
static const struct member members[] = {
    {"d/", '5', ""},
    {"d/plain", '0', ""},
    {"d/bsl\\x", '0', ""},
    {"d/bell\ax", '0', ""},
    {"d/bs\bx", '0', ""},
    {"d/ff\fx", '0', ""},
    {"d/nl\nx", '0', ""},
    {"d/cr\rx", '0', ""},
    {"d/tab\tx", '0', ""},
    {"d/vt\vx", '0', ""},
    {"d/ctl\001x", '0', ""},
    {"d/esc\033x", '0', ""},
    {"d/del\177x", '0', ""},
    {"d/raw\377x", '0', ""},
    {"d/hi80\200x", '0', ""},
    {"d/hi\303\251x", '0', ""},
    {"d/sp x", '0', ""},
    {"d/dq\"x", '0', ""},
    {"d/sq'x", '0', ""},
    {"d/amp&x", '0', ""},
    {"d/star*x", '0', ""},
    {"d/tilde~x", '0', ""},
    {"d/sym\tlink", '2', "tgt\\ctl\001x"},
    {"d/hard\\two", '1', "d/bsl\\x"},
};

// GNU tar 1.35, `tar tf`, on the archive below.
static const char *const expect_tf =
    "d/\n"
    "d/plain\n"
    "d/bsl\\\\x\n"
    "d/bell\\ax\n"
    "d/bs\\bx\n"
    "d/ff\\fx\n"
    "d/nl\\nx\n"
    "d/cr\\rx\n"
    "d/tab\\tx\n"
    "d/vt\\vx\n"
    "d/ctl\\001x\n"
    "d/esc\\033x\n"
    "d/del\\177x\n"
    "d/raw\\377x\n"
    "d/hi80\\200x\n"
    "d/hi\\303\\251x\n"
    "d/sp x\n"
    "d/dq\"x\n"
    "d/sq'x\n"
    "d/amp&x\n"
    "d/star*x\n"
    "d/tilde~x\n"
    "d/sym\\tlink\n"
    "d/hard\\\\two\n";

// GNU tar 1.35, `tar tvf`, same archive, TZ=UTC.
static const char *const expect_tvf =
    "drwxr-xr-x root/root         0 2023-11-14 22:13 d/\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/plain\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/bsl\\\\x\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/bell\\ax\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/bs\\bx\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/ff\\fx\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/nl\\nx\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/cr\\rx\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/tab\\tx\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/vt\\vx\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/ctl\\001x\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/esc\\033x\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/del\\177x\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/raw\\377x\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/hi80\\200x\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/hi\\303\\251x\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/sp x\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/dq\"x\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/sq'x\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/amp&x\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/star*x\n"
    "-rw-r--r-- root/root         0 2023-11-14 22:13 d/tilde~x\n"
    "lrw-r--r-- root/root         0 2023-11-14 22:13 d/sym\\tlink -> tgt\\\\ctl\\001x\n"
    "hrw-r--r-- root/root         0 2023-11-14 22:13 d/hard\\\\two link to d/bsl\\\\x\n";

static void check(const char *label, int cond) {
    if (cond) {
        test_logf("ok   %s\n", label);
    } else {
        printf("FAIL %s\n", label);
        failures_total++;
    }
}

static void field(char *p, size_t n, const char *s) {
    size_t len = strlen(s);
    memset(p, 0, n);
    memcpy(p, s, len < n ? len : n);
}

static void octal(char *p, size_t n, unsigned long v) {
    snprintf(p, n, "%0*lo", (int)n - 1, v);
}

// A plain ustar archive, written here so that no other tar has to exist and
// the bytes the oracle saw are the bytes this test lists.
static int make_archive(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    for (size_t i = 0; i < sizeof members / sizeof *members; i++) {
        char h[512];
        memset(h, 0, sizeof h);
        field(h, 100, members[i].name);
        octal(h + 100, 8, members[i].type == '5' ? 0755 : 0644);
        octal(h + 108, 8, 0);
        octal(h + 116, 8, 0);
        octal(h + 124, 12, 0);
        octal(h + 136, 12, 1700000000UL);
        memset(h + 148, ' ', 8);
        h[156] = members[i].type;
        field(h + 157, 100, members[i].link);
        memcpy(h + 257, "ustar", 5);
        memcpy(h + 263, "00", 2);
        field(h + 265, 32, "root");
        field(h + 297, 32, "root");
        octal(h + 329, 8, 0);
        octal(h + 337, 8, 0);
        unsigned sum = 0;
        for (size_t k = 0; k < sizeof h; k++) sum += (unsigned char)h[k];
        snprintf(h + 148, 8, "%06o", sum);
        h[154] = '\0';
        h[155] = ' ';
        fwrite(h, 1, sizeof h, f);
    }
    char zero[1024];
    memset(zero, 0, sizeof zero);
    fwrite(zero, 1, sizeof zero, f);
    return fclose(f);
}

// Runs `tar <op> ARCHIVE` with GNU's own environment for this comparison:
// TZ=UTC so the long listing's times are fixed, LC_ALL=C so a high byte is
// escaped rather than passed through as part of a character.
static int run_tar(const char *op, char *out, size_t size) {
    int p[2];
    if (pipe(p) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[0]);
        close(p[1]);
        setenv("TZ", "UTC", 1);
        setenv("LC_ALL", "C", 1);
        execl(TAR, "tar", op, ARCHIVE, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    size_t n = 0;
    ssize_t r;
    while (n + 1 < size && (r = read(p[0], out + n, size - n - 1)) > 0) n += (size_t)r;
    out[n] = '\0';
    close(p[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void compare(const char *label, const char *got, const char *want) {
    check(label, strcmp(got, want) == 0);
    if (strcmp(got, want) == 0) return;
    // Name the first line that differs; the bytes matter, so show them raw.
    const char *g = got, *w = want;
    int line = 1;
    while (*g && *w) {
        const char *ge = strchr(g, '\n'), *we = strchr(w, '\n');
        size_t gl = ge ? (size_t)(ge - g) : strlen(g), wl = we ? (size_t)(we - w) : strlen(w);
        if (gl != wl || memcmp(g, w, gl) != 0) {
            printf("  line %d\n    GNU:  %.*s\n    ours: %.*s\n", line, (int)wl, w, (int)gl, g);
            return;
        }
        if (!ge || !we) break;
        g = ge + 1;
        w = we + 1;
        line++;
    }
    printf("  differs after line %d (lengths %zu vs %zu)\n", line, strlen(want), strlen(got));
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    if (access(SMALLCLUE, X_OK) != 0) {
        printf("native_tar_quoting: SKIP (no %s)\n", SMALLCLUE);
        return 0;
    }
    alarm(test_watchdog_secs(60));
    mkdir(DIR, 0755);
    mkdir(DIR "/bin", 0755);
    unlink(TAR);
    if (symlink(SMALLCLUE, TAR) != 0) {
        printf("FAIL symlink %s: %s\n", TAR, strerror(errno));
        return 1;
    }
    if (make_archive(ARCHIVE) != 0) {
        printf("FAIL writing %s: %s\n", ARCHIVE, strerror(errno));
        return 1;
    }

    static char out[8192];
    check("tf.status", run_tar("tf", out, sizeof(out)) == 0);
    compare("tf.names", out, expect_tf);
    check("tvf.status", run_tar("tvf", out, sizeof(out)) == 0);
    compare("tvf.names", out, expect_tvf);

    unlink(ARCHIVE);
    unlink(TAR);
    rmdir(DIR "/bin");
    rmdir(DIR);
    return finish_suite("native_tar_quoting");
}
