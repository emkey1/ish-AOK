// native_coreutils.c -- SmallCLUE's rm, wc, head and tail against GNU
// coreutils 9.4's answers.
//
// iSH-AOK's native-links.sh puts SmallCLUE's applets ahead of the distro's on
// PATH, so they run every script that names them. The versions these
// replaced lacked head -c, tail -c and -F, rm -i/-I/-d/-v, and printed wc's
// counts in their own widths (2026-10-01).
//
// The table is generated: each case was run through GNU coreutils in
// build/devuan-amd64-test from the same fixture, and its stdout, exit status
// and the resulting tree recorded (tree NULL: unchanged). These expectations
// are GNU's, not this implementation's.
//
// Skips wherever the native multi-call binary is missing, real Linux included.
#define _GNU_SOURCE
#include <dirent.h>
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
#define TDIR "/tmp/native_coreutils_test"
#define WORK TDIR "/w"

struct cu_case {
    const char *applet;
    const char *args;   // split on spaces
    const char *input;  // NULL: /dev/null
    size_t input_len;
    const char *expect;
    size_t expect_len;
    int expect_status;
    const char *tree;   // `find . | sort` afterwards; NULL: the fixture's
};

static const struct cu_case cases[] = {
    {"head", "lines.txt", NULL, 0,
     "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\n", 49, 0, NULL},
    {"head", "-n 3 lines.txt", NULL, 0,
     "one\ntwo\nthree\n", 14, 0, NULL},
    {"head", "-3 lines.txt", NULL, 0,
     "one\ntwo\nthree\n", 14, 0, NULL},
    {"head", "-n -3 lines.txt", NULL, 0,
     "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\n", 45, 0, NULL},
    {"head", "-c 5 lines.txt", NULL, 0,
     "one\nt", 5, 0, NULL},
    {"head", "-c -5 lines.txt", NULL, 0,
     "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntw", 58, 0, NULL},
    {"head", "-c 1K lines.txt", NULL, 0,
     "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve\n", 63, 0, NULL},
    {"head", "-c 1kB nonl.txt", NULL, 0,
     "no newline", 10, 0, NULL},
    {"head", "-q lines.txt data.txt", NULL, 0,
     "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\nalpha 3 x\nbeta 1 y\ngamma 2 z\nalpha 3 x\ndelta 10 w\n", 99, 0, NULL},
    {"head", "-v nonl.txt", NULL, 0,
     "==> nonl.txt <==\nno newline", 27, 0, NULL},
    {"head", "-n 2 lines.txt data.txt", NULL, 0,
     "==> lines.txt <==\none\ntwo\n\n==> data.txt <==\nalpha 3 x\nbeta 1 y\n", 63, 0, NULL},
    {"head", "-n 2 lines.txt nosuch data.txt", NULL, 0,
     "==> lines.txt <==\none\ntwo\n\n==> data.txt <==\nalpha 3 x\nbeta 1 y\n", 63, 1, NULL},
    {"head", "-n -1 nonl.txt", NULL, 0,
     "", 0, 0, NULL},
    {"head", "-n 0 lines.txt", NULL, 0,
     "", 0, 0, NULL},
    {"head", "-x lines.txt", NULL, 0,
     "", 0, 1, NULL},
    {"head", "-n abc lines.txt", NULL, 0,
     "", 0, 1, NULL},
    {"head", "-c 99999999999999999999999 nonl.txt", NULL, 0,
     "no newline", 10, 0, NULL},
    {"head", "-n 2", "a\nb\nc\nd\n", 8,
     "a\nb\n", 4, 0, NULL},
    {"head", "-n -1", "a\nb\nc\nd\n", 8,
     "a\nb\nc\n", 6, 0, NULL},
    {"head", "-z -n 2", "a\000b\000c\000", 6,
     "a\000b\000", 4, 0, NULL},
    {"head", "dir", NULL, 0,
     "", 0, 1, NULL},
    {"tail", "lines.txt", NULL, 0,
     "three\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve\n", 55, 0, NULL},
    {"tail", "-n 3 lines.txt", NULL, 0,
     "ten\neleven\ntwelve\n", 18, 0, NULL},
    {"tail", "-3 lines.txt", NULL, 0,
     "ten\neleven\ntwelve\n", 18, 0, NULL},
    {"tail", "+10 lines.txt", NULL, 0,
     "ten\neleven\ntwelve\n", 18, 0, NULL},
    {"tail", "-n +10 lines.txt", NULL, 0,
     "ten\neleven\ntwelve\n", 18, 0, NULL},
    {"tail", "-n +0 nonl.txt", NULL, 0,
     "no newline", 10, 0, NULL},
    {"tail", "-c 5 lines.txt", NULL, 0,
     "elve\n", 5, 0, NULL},
    {"tail", "-c +60 lines.txt", NULL, 0,
     "lve\n", 4, 0, NULL},
    {"tail", "-n 1 nonl.txt", NULL, 0,
     "no newline", 10, 0, NULL},
    {"tail", "-n 0 lines.txt", NULL, 0,
     "", 0, 0, NULL},
    {"tail", "-q lines.txt data.txt", NULL, 0,
     "three\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve\nalpha 3 x\nbeta 1 y\ngamma 2 z\nalpha 3 x\ndelta 10 w\n", 105, 0, NULL},
    {"tail", "-v nonl.txt", NULL, 0,
     "==> nonl.txt <==\nno newline", 27, 0, NULL},
    {"tail", "-n 1 lines.txt nosuch data.txt", NULL, 0,
     "==> lines.txt <==\ntwelve\n\n==> data.txt <==\ndelta 10 w\n", 54, 1, NULL},
    {"tail", "dir", NULL, 0,
     "", 0, 1, NULL},
    {"tail", "-n abc lines.txt", NULL, 0,
     "", 0, 1, NULL},
    {"tail", "-n 2", "a\nb\nc\nd\n", 8,
     "c\nd\n", 4, 0, NULL},
    {"tail", "-n +3", "a\nb\nc\nd\n", 8,
     "c\nd\n", 4, 0, NULL},
    {"tail", "-c 4", "a\nb\nc\nd\n", 8,
     "c\nd\n", 4, 0, NULL},
    {"tail", "-z -n 1", "a\000b\000c", 5,
     "c", 1, 0, NULL},
    {"tail", "-f -n 1", "a\nb\nc\nd\n", 8,
     "d\n", 2, 0, NULL},
    {"tail", "-5c lines.txt", NULL, 0,
     "elve\n", 5, 0, NULL},
    {"wc", "data.txt", NULL, 0,
     " 5 15 50 data.txt\n", 18, 0, NULL},
    {"wc", "-l lines.txt", NULL, 0,
     "12 lines.txt\n", 13, 0, NULL},
    {"wc", "-w text.txt", NULL, 0,
     "7 text.txt\n", 11, 0, NULL},
    {"wc", "-c data.txt", NULL, 0,
     "50 data.txt\n", 12, 0, NULL},
    {"wc", "-m text.txt", NULL, 0,
     "34 text.txt\n", 12, 0, NULL},
    {"wc", "-lw data.txt", NULL, 0,
     " 5 15 data.txt\n", 15, 0, NULL},
    {"wc", "data.txt lines.txt", NULL, 0,
     "  5  15  50 data.txt\n 12  12  63 lines.txt\n 17  27 113 total\n", 61, 0, NULL},
    {"wc", "-l data.txt lines.txt", NULL, 0,
     "  5 data.txt\n 12 lines.txt\n 17 total\n", 37, 0, NULL},
    {"wc", "--total=only data.txt lines.txt", NULL, 0,
     "17 27 113\n", 10, 0, NULL},
    {"wc", "--total=never data.txt lines.txt", NULL, 0,
     "  5  15  50 data.txt\n 12  12  63 lines.txt\n", 43, 0, NULL},
    {"wc", "--total=always data.txt", NULL, 0,
     " 5 15 50 data.txt\n 5 15 50 total\n", 33, 0, NULL},
    {"wc", "dir", NULL, 0,
     "      0       0       0 dir\n", 28, 1, NULL},
    {"wc", "data.txt nosuch", NULL, 0,
     " 5 15 50 data.txt\n 5 15 50 total\n", 33, 1, NULL},
    {"wc", "", "one two\nthree\n", 14,
     "      2       3      14\n", 24, 0, NULL},
    {"wc", "-l", "one two\nthree\n", 14,
     "2\n", 2, 0, NULL},
    {"wc", "-c -", "one two\nthree\n", 14,
     "14 -\n", 5, 0, NULL},
    {"rm", "-v nonl.txt", NULL, 0,
     "removed 'nonl.txt'\n", 19, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./empty.txt\n./lines.txt\n./text.txt\n"},
    {"rm", "nosuch", NULL, 0,
     "", 0, 1, NULL},
    {"rm", "-f nosuch", NULL, 0,
     "", 0, 0, NULL},
    {"rm", "dir", NULL, 0,
     "", 0, 1, NULL},
    {"rm", "-d empty", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty.txt\n./lines.txt\n./nonl.txt\n./text.txt\n"},
    {"rm", "-r .", NULL, 0,
     "", 0, 1, NULL},
    {"rm", "-rf dir", NULL, 0,
     "", 0, 0, ".\n./data.txt\n./empty\n./empty.txt\n./lines.txt\n./nonl.txt\n./text.txt\n"},
    {"rm", "-x nonl.txt", NULL, 0,
     "", 0, 1, NULL},
    {"rm", "-v nonl.txt nosuch empty.txt", NULL, 0,
     "removed 'nonl.txt'\nremoved 'empty.txt'\n", 39, 1, ".\n./data.txt\n./dir\n./dir/f1\n./dir/sub\n./dir/sub/f2\n./empty\n./lines.txt\n./text.txt\n"},
};

static void write_file(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        if (write(fd, text, strlen(text)) < 0) {}
        close(fd);
    }
}

static void rm_rf(const char *path) {
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/rm", "rm", "-rf", path, (char *)NULL);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
}

// The fixture every case starts from, as the generator made it.
static void fixture(void) {
    rm_rf(WORK);
    mkdir(WORK, 0755);
    if (chdir(WORK) != 0) return;
    write_file("data.txt", "alpha 3 x\nbeta 1 y\ngamma 2 z\nalpha 3 x\ndelta 10 w\n");
    write_file("lines.txt", "one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten\neleven\ntwelve\n");
    write_file("text.txt", "Hello World\nfoo bar baz\nHELLO \303\251t\303\251\n");
    write_file("nonl.txt", "no newline");
    write_file("empty.txt", "");
    mkdir("dir", 0755);
    mkdir("dir/sub", 0755);
    mkdir("empty", 0755);
    write_file("dir/f1", "x\n");
    write_file("dir/sub/f2", "yy\n");
}

struct names { char **v; size_t n, cap; };

static void walk(const char *rel, struct names *out) {
    if (out->n == out->cap) {
        out->cap = out->cap ? out->cap * 2 : 32;
        out->v = realloc(out->v, out->cap * sizeof(char *));
    }
    out->v[out->n++] = strdup(rel);
    DIR *d = opendir(rel);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", rel, e->d_name);
        walk(path, out);
    }
    closedir(d);
}

static int by_name(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// `find . | sort` of the work directory.
static char *tree(void) {
    struct names n = {0};
    walk(".", &n);
    qsort(n.v, n.n, sizeof(char *), by_name);
    size_t len = 1;
    for (size_t i = 0; i < n.n; i++) len += strlen(n.v[i]) + 1;
    char *s = malloc(len), *p = s;
    for (size_t i = 0; i < n.n; i++) {
        p += sprintf(p, "%s\n", n.v[i]);
        free(n.v[i]);
    }
    *p = '\0';
    free(n.v);
    return s;
}

// Runs one case in WORK; status returned, stdout in out/out_len.
static int run_case(const struct cu_case *c, char *out, size_t out_size, size_t *out_len) {
    char argbuf[256], applet[256];
    const char *argv[24];
    int n = 0;
    snprintf(applet, sizeof(applet), TDIR "/bin/%s", c->applet);
    argv[n++] = c->applet;
    snprintf(argbuf, sizeof(argbuf), "%s", c->args);
    for (char *t = strtok(argbuf, " "); t && n < 23; t = strtok(NULL, " "))
        argv[n++] = t;
    argv[n] = NULL;
    int outp[2], inp[2];
    if (pipe(outp) != 0 || pipe(inp) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        dup2(c->input ? inp[0] : null, 0);
        dup2(outp[1], 1);
        dup2(null, 2);
        close(inp[1]);
        close(outp[0]);
        setenv("LC_ALL", "C.UTF-8", 1);
        execv(applet, (char *const *)argv);
        _exit(127);
    }
    close(inp[0]);
    close(outp[1]);
    if (c->input && write(inp[1], c->input, c->input_len) < 0) {}
    close(inp[1]);
    size_t len = 0;
    for (;;) {
        ssize_t r = read(outp[0], out + len, out_size - 1 - len);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        len += (size_t)r;
    }
    out[len] = '\0';
    *out_len = len;
    close(outp[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static void print_bytes(const char *label, const char *s, size_t n) {
    printf("  %s: \"", label);
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\n') printf("\\n");
        else if (ch < 32 || ch >= 127) printf("\\%03o", ch);
        else putchar(ch);
    }
    printf("\"\n");
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    if (access(SMALLCLUE, X_OK) != 0) {
        printf("native_coreutils: SKIP (no %s)\n", SMALLCLUE);
        return 0;
    }
    alarm(test_watchdog_secs(180));
    mkdir(TDIR, 0755);
    mkdir(TDIR "/bin", 0755);
    static const char *const applets[] = {"head", "tail", "wc", "rm"};
    for (size_t i = 0; i < sizeof(applets) / sizeof(applets[0]); i++) {
        char link[256];
        snprintf(link, sizeof(link), TDIR "/bin/%s", applets[i]);
        unlink(link);
        if (symlink(SMALLCLUE, link) != 0) {
            printf("FAIL symlink %s: %s\n", link, strerror(errno));
            return 1;
        }
    }
    fixture();
    char *fixture_tree = tree();
    static char out[65536];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct cu_case *c = &cases[i];
        size_t len = 0;
        fixture();
        int status = run_case(c, out, sizeof(out), &len);
        char *after = tree();
        const char *want_tree = c->tree ? c->tree : fixture_tree;
        int ok_out = len == c->expect_len && memcmp(out, c->expect, len) == 0;
        int ok_status = status == c->expect_status;
        int ok_tree = strcmp(after, want_tree) == 0;
        if (ok_out && ok_status && ok_tree) {
            test_logf("ok   %s %s\n", c->applet, c->args);
        } else {
            printf("FAIL %s %s\n", c->applet, c->args);
            if (!ok_out) {
                print_bytes("want", c->expect, c->expect_len);
                print_bytes("got ", out, len);
            }
            if (!ok_status) printf("  status want %d got %d\n", c->expect_status, status);
            if (!ok_tree) {
                print_bytes("tree want", want_tree, strlen(want_tree));
                print_bytes("tree got ", after, strlen(after));
            }
            failures_total++;
        }
        free(after);
    }
    free(fixture_tree);
    if (chdir("/") != 0) {}
    rm_rf(TDIR);
    return finish_suite("native_coreutils");
}
