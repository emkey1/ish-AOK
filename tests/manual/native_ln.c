// native_ln.c -- SmallCLUE's ln against GNU coreutils 9's behaviour.
//
// SmallCLUE's ln took -s and -f only. native-links.sh puts it first on PATH,
// so `ln -sfn` -- the usual way to repoint a symlink to a directory -- failed
// with "illegal option -- n", silently where a script hid the error:
// start-wayland.sh's debug-log link was never made on a 5th-generation iPad
// (2026-10-01). Every expectation here is what GNU ln 9.4 did in
// build/devuan-amd64-test for the same command, from the comparison that
// checked the rewrite (95 cases, tree, output and status all identical).
//
// Skips wherever the native multi-call binary is missing, real Linux included.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define SMALLCLUE "/AOK/native/smallclue"
#define DIR "/tmp/native_ln_test"
#define LN DIR "/bin/ln"
#define WORK DIR "/w"

static void check(const char *label, int cond) {
    if (cond) {
        test_logf("ok   %s\n", label);
    } else {
        printf("FAIL %s\n", label);
        failures_total++;
    }
}

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

// The fixture the comparison used: files, a directory, symlinks to each.
static void fixture(void) {
    rm_rf(WORK);
    mkdir(WORK, 0755);
    if (chdir(WORK) != 0) return;
    write_file("f1", "f1\n");
    write_file("f2", "f2\n");
    mkdir("d1", 0755);
    mkdir("d2", 0755);
    write_file("d1/in", "in\n");
    if (symlink("d1", "sd1") || symlink("f1", "sf1") || symlink("f2", "oldlink")) {}
    write_file("old", "old\n");
}

// Runs ln in WORK with `input` on stdin; status returned, stdout in `out`.
static int run_ln(const char *input, char *out, size_t out_size, const char *a0, ...) {
    const char *args[24] = {"ln"};
    int n = 1;
    va_list ap;
    va_start(ap, a0);
    for (const char *a = a0; a && n < 23; a = va_arg(ap, const char *))
        args[n++] = a;
    va_end(ap);
    args[n] = NULL;
    int outp[2], inp[2];
    if (pipe(outp) != 0 || pipe(inp) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int null = open("/dev/null", O_WRONLY);
        dup2(inp[0], 0);
        dup2(outp[1], 1);
        dup2(null, 2);
        close(inp[1]);
        close(outp[0]);
        if (chdir(WORK) != 0) _exit(126);
        execv(LN, (char *const *)args);
        _exit(127);
    }
    close(inp[0]);
    close(outp[1]);
    if (input && write(inp[1], input, strlen(input)) < 0) {}
    close(inp[1]);
    size_t len = 0;
    if (out) {
        for (;;) {
            ssize_t r = read(outp[0], out + len, out_size - 1 - len);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            len += (size_t)r;
        }
        out[len] = '\0';
    }
    close(outp[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

static int link_is(const char *path, const char *target) {
    char buf[PATH_MAX];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n < 0) return 0;
    buf[n] = '\0';
    return strcmp(buf, target) == 0;
}

static int same_inode(const char *a, const char *b) {
    struct stat sa, sb;
    return lstat(a, &sa) == 0 && lstat(b, &sb) == 0 && sa.st_ino == sb.st_ino && sa.st_dev == sb.st_dev;
}

static int exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    if (access(SMALLCLUE, X_OK) != 0) {
        printf("native_ln: SKIP (no %s)\n", SMALLCLUE);
        return 0;
    }
    alarm(test_watchdog_secs(120));
    mkdir(DIR, 0755);
    mkdir(DIR "/bin", 0755);
    unlink(LN);
    if (symlink(SMALLCLUE, LN) != 0) {
        printf("FAIL symlink %s: %s\n", LN, strerror(errno));
        return 1;
    }
    char out[1024];

    // -n: repoint a symlink to a directory, rather than linking inside it.
    fixture();
    check("sfn.status", run_ln(NULL, NULL, 0, "-sfn", "f2", "sd1", NULL) == 0);
    check("sfn.repointed", link_is(WORK "/sd1", "f2"));
    fixture();
    check("sf.status", run_ln(NULL, NULL, 0, "-sf", "f2", "sd1", NULL) == 0);
    check("sf.inside_dir", link_is(WORK "/d1/f2", "f2") && link_is(WORK "/sd1", "d1"));

    // -f replaces an existing name.
    fixture();
    check("sf_replace.status", run_ln(NULL, NULL, 0, "-sf", "f2", "oldlink", NULL) == 0);
    check("sf_replace.target", link_is(WORK "/oldlink", "f2"));

    // -T and -t.
    fixture();
    check("T_dir.status", run_ln(NULL, NULL, 0, "-sT", "f2", "d1", NULL) == 1);
    fixture();
    check("t.status", run_ln(NULL, NULL, 0, "-t", "d2", "f1", "f2", NULL) == 0);
    check("t.hard_links", same_inode(WORK "/d2/f1", WORK "/f1") && same_inode(WORK "/d2/f2", WORK "/f2"));

    // -r.
    fixture();
    check("r.status", run_ln(NULL, NULL, 0, "-sr", "d1/in", "d2/rel", NULL) == 0);
    check("r.relative", link_is(WORK "/d2/rel", "../d1/in"));

    // Numbered backups.
    fixture();
    run_ln(NULL, NULL, 0, "--backup=numbered", "-sf", "f2", "old", NULL);
    check("backup.second", run_ln(NULL, NULL, 0, "--backup=numbered", "-sf", "f1", "old", NULL) == 0);
    check("backup.numbered", exists(WORK "/old.~1~") && exists(WORK "/old.~2~") && link_is(WORK "/old", "f1"));

    // -v, GNU's text.
    fixture();
    run_ln(NULL, out, sizeof(out), "-sv", "f1", "s2", NULL);
    check("verbose.text", strcmp(out, "'s2' -> 'f1'\n") == 0);
    if (strcmp(out, "'s2' -> 'f1'\n") != 0) printf("  got: %s", out);

    // -i: declining is a failure and leaves the file; agreeing replaces it.
    fixture();
    check("interactive_no.status", run_ln("n\n", NULL, 0, "-si", "f2", "old", NULL) == 1);
    check("interactive_no.kept", !link_is(WORK "/old", "f2") && exists(WORK "/old"));
    check("interactive_yes.status", run_ln("y\n", NULL, 0, "-si", "f2", "old", NULL) == 0);
    check("interactive_yes.replaced", link_is(WORK "/old", "f2"));

    // What GNU refuses.
    fixture();
    check("same_file", run_ln(NULL, NULL, 0, "-f", "f1", "f1", NULL) == 1);
    check("hard_dir", run_ln(NULL, NULL, 0, "d1", "hd", NULL) == 1);
    check("exists", run_ln(NULL, NULL, 0, "-s", "f1", "old", NULL) == 1);
    check("bad_option", run_ln(NULL, NULL, 0, "-x", "f1", NULL) == 1);
    check("not_a_dir", run_ln(NULL, NULL, 0, "-s", "f1", "f2", "f3", NULL) == 1);

    rm_rf(DIR);
    return finish_suite("native_ln");
}
