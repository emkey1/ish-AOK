// hardlink_proc_path.c -- a file with several hard links is named by the link
// it was opened through: /proc/self/fd/N for an open, /proc/self/exe for an
// exec. Ubuntu 25.10's coreutils are 115 hard links of one multicall binary
// that compares argv[0] with /proc/self/exe; with the exe link naming
// another of the links, every utility refused to run. A rename of an opened
// single-link file must still be followed. (Not covered: renaming the opened
// name of a file that has OTHER links -- fakefs then names one of those, as
// it always has; Linux follows the rename.)

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

static int failures;
static void expect(const char *what, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        printf("FAIL %s: %s, want %s\n", what, got, want);
        failures++;
    }
}
static void link_of(const char *link, char *out) {
    ssize_t n = readlink(link, out, PATH_MAX - 1);
    out[n < 0 ? 0 : n] = '\0';
}

int main(int argc, char **argv) {
    char got[PATH_MAX], fdlink[64];
    if (argc > 1 && strcmp(argv[1], "child") == 0) {
        link_of("/proc/self/exe", got);
        puts(got);
        return 0;
    }
    char dir[] = "/tmp/hardlink_proc_path.XXXXXX";
    if (mkdtemp(dir) == NULL) { perror("mkdtemp"); return 1; }
    char a[PATH_MAX], b[PATH_MAX], c[PATH_MAX], d[PATH_MAX];
    snprintf(a, sizeof(a), "%s/a", dir); snprintf(b, sizeof(b), "%s/b", dir);
    snprintf(c, sizeof(c), "%s/c", dir); snprintf(d, sizeof(d), "%s/d", dir);
    int fd = open(a, O_CREAT | O_WRONLY, 0644);
    close(fd);
    link(a, b);
    link(a, c);

    fd = open(b, O_RDONLY);
    snprintf(fdlink, sizeof(fdlink), "/proc/self/fd/%d", fd);
    link_of(fdlink, got);
    expect("fd opened as b", got, b);
    int fd2 = open(c, O_RDONLY);
    snprintf(fdlink, sizeof(fdlink), "/proc/self/fd/%d", fd2);
    link_of(fdlink, got);
    expect("fd opened as c", got, c);
    close(fd);
    close(fd2);
    unlink(b);
    unlink(c); // a is a single link again
    fd = open(a, O_RDONLY);
    rename(a, d); // the opened file moves: follow it
    snprintf(fdlink, sizeof(fdlink), "/proc/self/fd/%d", fd);
    link_of(fdlink, got);
    expect("fd opened as a, then a renamed to d", got, d);
    close(fd);

    // exec through a hard link of this program
    char self[PATH_MAX], prog[PATH_MAX];
    link_of("/proc/self/exe", self);
    snprintf(prog, sizeof(prog), "%s/prog2", dir);
    int in = open(self, O_RDONLY), out = open(prog, O_CREAT | O_WRONLY, 0755);
    char buf[65536];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        write(out, buf, (size_t) n);
    close(in);
    close(out);
    char prog_link[PATH_MAX];
    snprintf(prog_link, sizeof(prog_link), "%s/prog_link", dir);
    link(prog, prog_link);
    int p[2];
    pipe(p);
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        execl(prog_link, "prog_link", "child", (char *) NULL);
        _exit(127);
    }
    close(p[1]);
    n = read(p[0], got, sizeof(got) - 1);
    got[n < 0 ? 0 : n] = '\0';
    if (n > 0 && got[n - 1] == '\n')
        got[n - 1] = '\0';
    waitpid(pid, NULL, 0);
    expect("exe of a program run as prog_link", got, prog_link);

    unlink(d); unlink(prog); unlink(prog_link); rmdir(dir);
    printf("hardlink_proc_path: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
