// native_fnmatch_applets.c -- SmallCLUE's pattern options against GNU's
// answers: find -name/-iname/-path, ls -I/--hide, grep --include/--exclude,
// diff -x, tar --exclude and --wildcards.
//
// Native programs are host code, so their fnmatch was Darwin's, which refuses
// (an error, 2) a pattern holding a `[` that opens no bracket expression, and
// one ending in a lone `\`, where glibc matches the `[` as itself. So every
// one of these failed on a name like `a[b`: find found nothing, ls excluded
// nothing, diff -x compared what it was told to skip, grep --include matched
// nothing, and tar --wildcards said "Not found in archive". The shim's
// fnmatch follows glibc's rules now (kernel/native_fnmatch.c).
//
// The expectations are GNU's (findutils 4.10, coreutils 9.7, grep 3.11,
// diffutils 3.10, tar 1.35; Debian on camd), LC_ALL=C, each output sorted.
// Skips wherever the native multi-call binary is missing, real Linux included.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define SMALLCLUE "/AOK/native/smallclue"
#define TDIR "/tmp/native_fnmatch_applets"

static int failures, checks;

struct fa_case {
    const char *argv[10];
    const char *expect;         // sorted lines
    int status;
};

static const struct fa_case cases[] = {
    {{"find", "d", "-name", "a[b"}, "d/a[b\nd/s/a[b\n", 0},
    {{"find", "d", "-name", "[x"}, "d/[x\n", 0},
    {{"find", "d", "-path", "d/s/a[b"}, "d/s/a[b\n", 0},
    {{"find", "d", "-iname", "A[B"}, "d/a[b\nd/s/a[b\n", 0},
    {{"find", "d", "-name", "*["}, "d/.h[\n", 0},
    {{"find", "d", "-name", "b\\"}, "", 0},                 // a trailing `\` matches nothing
    {{"find", "d", "-name", "b\\\\"}, "d/b\\\n", 0},
    {{"find", "d", "-name", "[!a]*"}, "d\nd/.h[\nd/Ab\nd/[x\nd/b\\\nd/s\nd/s/q\n", 0},
    {{"ls", "-I", "a[b", "d"}, "Ab\n[x\na]\nabc\nb\\\ns\n", 0},
    {{"ls", "--hide=[x", "d"}, "Ab\na[b\na]\nabc\nb\\\ns\n", 0},
    {{"ls", "-a", "-I", ".h[", "d"}, ".\n..\nAb\n[x\na[b\na]\nabc\nb\\\ns\n", 0},
    {{"grep", "-r", "--include=a[b", "x", "d"}, "d/a[b:x\nd/s/a[b:x\n", 0},
    {{"grep", "-r", "--exclude=a[b", "x", "d"}, "d/.h[:x\nd/Ab:x\nd/[x:x\nd/a]:x\nd/abc:x\nd/b\\:x\nd/s/q:x\n", 0},
    {{"grep", "-r", "--exclude=[!a]*", "x", "d"}, "d/a[b:x\nd/a]:x\nd/abc:x\nd/s/a[b:x\n", 0},
    {{"diff", "-r", "-x", "a[b", "d", "e"}, "---\n1c1\n< x\n> y\ndiff -r -x 'a[b' d/abc e/abc\n", 1},
    {{"tar", "-cf", "t.tar", "--exclude=a[b", "--exclude=b\\\\", "d"}, "", 0},
    {{"tar", "-tf", "t.tar"}, "d/\nd/.h[\nd/Ab\nd/[x\nd/a]\nd/abc\nd/s/\nd/s/q\n", 0},
    {{"tar", "-cf", "t2.tar", "d"}, "", 0},
    {{"tar", "-tf", "t2.tar", "--wildcards", "d/a[b"}, "d/a[b\n", 0},
};

static int cmp_lines(const void *a, const void *b) {
    return strcmp(*(char *const *) a, *(char *const *) b);
}

// The command's stdout and stderr, lines sorted; its exit status.
static int run(const char *const *argv, char *out, size_t size) {
    int p[2];
    if (pipe(p) != 0)
        return -1;
    pid_t c = fork();
    if (c == 0) {
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[0]);
        signal(SIGPIPE, SIG_DFL);
        char path[128];
        snprintf(path, sizeof path, TDIR "/bin/%s", argv[0]);
        execv(path, (char *const *) argv);
        _exit(127);
    }
    close(p[1]);
    char raw[8192];
    size_t len = 0;
    ssize_t n;
    while (len < sizeof raw - 1 && (n = read(p[0], raw + len, sizeof raw - 1 - len)) > 0)
        len += (size_t) n;
    raw[len] = '\0';
    close(p[0]);
    int st = 0;
    waitpid(c, &st, 0);
    char *lines[256];
    size_t count = 0;
    char *save = NULL;
    for (char *l = strtok_r(raw, "\n", &save); l != NULL && count < 256; l = strtok_r(NULL, "\n", &save))
        lines[count++] = l;
    qsort(lines, count, sizeof *lines, cmp_lines);
    out[0] = '\0';
    for (size_t i = 0; i < count; i++) {
        strncat(out, lines[i], size - strlen(out) - 2);
        strcat(out, "\n");
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static void put(const char *path) {
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) {
        if (write(fd, "x\n", 2) != 2) {}
        close(fd);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (access(SMALLCLUE, X_OK) != 0) {
        printf("native_fnmatch_applets: SKIP (no %s)\n", SMALLCLUE);
        return 0;
    }
    alarm(120);
    setenv("LC_ALL", "C", 1);
    if (system("rm -rf " TDIR) != 0) {}
    mkdir(TDIR, 0755);
    mkdir(TDIR "/bin", 0755);
    const char *applets[] = {"find", "ls", "grep", "diff", "tar"};
    for (unsigned i = 0; i < sizeof applets / sizeof *applets; i++) {
        char link[128];
        snprintf(link, sizeof link, TDIR "/bin/%s", applets[i]);
        if (symlink(SMALLCLUE, link) != 0) {
            printf("FAIL symlink %s: %s\n", link, strerror(errno));
            return 1;
        }
    }
    if (chdir(TDIR) != 0)
        return 1;
    mkdir("d", 0755);
    mkdir("d/s", 0755);
    mkdir("e", 0755);
    mkdir("e/s", 0755);
    const char *files[] = {"a[b", "abc", "a]", "[x", "b\\", "Ab", "s/a[b", "s/q", ".h["};
    for (unsigned i = 0; i < sizeof files / sizeof *files; i++) {
        char path[64];
        snprintf(path, sizeof path, "d/%s", files[i]);
        put(path);
        snprintf(path, sizeof path, "e/%s", files[i]);
        put(path);
    }
    // e differs from d in a[b, which diff -x skips, and in abc
    int fd = open("e/a[b", O_WRONLY | O_TRUNC);
    if (fd >= 0 && write(fd, "y\n", 2) == 2) {}
    close(fd);
    fd = open("e/abc", O_WRONLY | O_TRUNC);
    if (fd >= 0 && write(fd, "y\n", 2) == 2) {}
    close(fd);

    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        const struct fa_case *c = &cases[i];
        char out[8192];
        int st = run(c->argv, out, sizeof out);
        checks++;
        if (st != c->status || strcmp(out, c->expect) != 0) {
            failures++;
            printf("FAIL");
            for (int a = 0; c->argv[a]; a++)
                printf(" %s", c->argv[a]);
            printf(": status %d (want %d)\n  got:\n%s  want:\n%s", st, c->status, out, c->expect);
        }
    }
    if (chdir("/") != 0) {}
    if (system("rm -rf " TDIR) != 0) {}
    printf("native_fnmatch_applets: %s (%d checks, %d failures)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures != 0;
}
