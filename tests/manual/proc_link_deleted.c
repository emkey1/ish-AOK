// " (deleted)" in what /proc shows for a file whose name no longer reaches
// it, and getcwd in a directory that has been removed.
//
// Linux's d_path shows such a file by the name it had with " (deleted)"
// after it: readlink of /proc/<pid>/fd/N, cwd and exe, and the name in
// /proc/<pid>/maps and smaps. Tools look for it -- needrestart and lsof to
// find a library or program replaced under a running process, and anything
// that must tell a held deleted file from a live one. AOK showed the bare old
// name, which another file may have taken since. getcwd there is ENOENT on
// Linux; AOK returned the name.
//
// Also: a file renamed while open is shown by its new name, with no suffix; a
// live file has none; and a memfd, which says "(deleted)" itself, says it once.
// Not asserted: after one of two hard links is unlinked, Linux shows the gone
// name with " (deleted)", AOK the surviving name -- which still reaches the
// file.
//
// Oracle: Linux 6.12 (camd), glibc x86_64 and -m32.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#ifndef SYS_memfd_create
#if defined(__x86_64__)
#define SYS_memfd_create 319
#elif defined(__i386__)
#define SYS_memfd_create 356
#else
#define SYS_memfd_create 279
#endif
#endif

extern char **environ;

static void check(int cond, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (!cond) {
        printf("FAIL ");
        vprintf(fmt, ap);
        printf("\n");
        failures_total++;
    } else if (test_verbose) {
        printf("ok ");
        vprintf(fmt, ap);
        printf("\n");
    }
    va_end(ap);
}

static void link_text(const char *proc, char *out, size_t n) {
    ssize_t len = readlink(proc, out, n - 1);
    out[len > 0 ? len : 0] = '\0';
}

static void fd_text(int fd, char *out, size_t n) {
    char proc[64];
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    link_text(proc, out, n);
}

static void expect_fd(const char *what, int fd, const char *want) {
    char got[PATH_MAX];
    fd_text(fd, got, sizeof(got));
    check(strcmp(got, want) == 0, "%s: /proc/self/fd is \"%s\" (got \"%s\")", what, want, got);
}

int main(int argc, char **argv) {
    // The exec'd copy: its own file has just been unlinked.
    if (argc == 3 && strcmp(argv[1], "--exe") == 0) {
        char got[PATH_MAX];
        unlink(argv[2]);
        link_text("/proc/self/exe", got, sizeof(got));
        return strcmp(got, argv[2]) == 0 ? 10 : strncmp(got, argv[2], strlen(argv[2])) == 0 &&
            strcmp(got + strlen(argv[2]), " (deleted)") == 0 ? 0 : 20;
    }
    test_init(argc, argv);
    alarm(test_watchdog_secs(30));

    char dir[] = "/tmp/proc_link_deleted.XXXXXX";
    check(mkdtemp(dir) != NULL, "mkdtemp (%s)", strerror(errno));
    char a[128], b[128], sub[128], m[128], want[PATH_MAX];
    snprintf(a, sizeof(a), "%s/a", dir);
    snprintf(b, sizeof(b), "%s/b", dir);
    snprintf(sub, sizeof(sub), "%s/sub", dir);
    snprintf(m, sizeof(m), "%s/mapped", dir);

    // A live file: its name, nothing after it.
    int fd = open(a, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    check(fd >= 0, "create %s (%s)", a, strerror(errno));
    expect_fd("a live file", fd, a);

    // Unlinked, and then again with another file at the name.
    unlink(a);
    snprintf(want, sizeof(want), "%s (deleted)", a);
    expect_fd("unlinked", fd, want);
    int other = open(a, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    expect_fd("unlinked, its name taken since", fd, want);
    expect_fd("the file that took the name", other, a);
    close(other);
    unlink(a);
    close(fd);

    // Renamed while open: the new name, not deleted.
    fd = open(a, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    check(rename(a, b) == 0, "rename (%s)", strerror(errno));
    expect_fd("renamed while open", fd, b);
    close(fd);
    unlink(b);

    // A directory removed while held, and while it is the cwd.
    check(mkdir(sub, 0755) == 0, "mkdir %s (%s)", sub, strerror(errno));
    fd = open(sub, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    check(rmdir(sub) == 0, "rmdir (%s)", strerror(errno));
    snprintf(want, sizeof(want), "%s (deleted)", sub);
    expect_fd("a removed directory", fd, want);
    close(fd);
    check(mkdir(sub, 0755) == 0 && chdir(sub) == 0, "mkdir and chdir %s (%s)", sub, strerror(errno));
    check(rmdir(sub) == 0, "rmdir the cwd (%s)", strerror(errno));
    char got[PATH_MAX];
    link_text("/proc/self/cwd", got, sizeof(got));
    check(strcmp(got, want) == 0, "a removed cwd: /proc/self/cwd is \"%s\" (got \"%s\")", want, got);
    errno = 0;
    char *cwd = getcwd(got, sizeof(got));
    check(cwd == NULL && errno == ENOENT, "a removed cwd: getcwd is ENOENT (%s)",
          cwd != NULL ? cwd : strerror(errno));
    check(chdir("/") == 0, "chdir / (%s)", strerror(errno));
    cwd = getcwd(got, sizeof(got));
    check(cwd != NULL && strcmp(cwd, "/") == 0, "getcwd at / (%s)", cwd != NULL ? cwd : strerror(errno));

    // A mapped file unlinked: maps says so.
    fd = open(m, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    check(fd >= 0 && ftruncate(fd, 4096) == 0, "create %s (%s)", m, strerror(errno));
    void *map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    check(map != MAP_FAILED, "mmap (%s)", strerror(errno));
    close(fd);
    unlink(m);
    snprintf(want, sizeof(want), "%s (deleted)", m);
    FILE *maps = fopen("/proc/self/maps", "r");
    bool found = false;
    char line[PATH_MAX + 128];
    while (maps != NULL && fgets(line, sizeof(line), maps) != NULL) {
        char *slash = strchr(line, '/');
        if (slash == NULL || strstr(slash, "mapped") == NULL)
            continue;
        slash[strcspn(slash, "\n")] = '\0';
        found = true;
        check(strcmp(slash, want) == 0, "maps names the unlinked mapping \"%s\" (got \"%s\")", want,
              slash);
    }
    if (maps != NULL)
        fclose(maps);
    check(found, "maps lists the mapping");
    if (map != MAP_FAILED)
        munmap(map, 4096);

    // A memfd says "(deleted)" itself, and says it once.
    fd = (int) syscall(SYS_memfd_create, "pld", 0);
    if (fd >= 0) {
        expect_fd("a memfd", fd, "/memfd:pld (deleted)");
        close(fd);
    }

    // The running program's own file unlinked under it: /proc/self/exe.
    char prog[128];
    snprintf(prog, sizeof(prog), "%s/prog", dir);
    int in = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    int out = open(prog, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0755);
    char buf[65536];
    ssize_t n;
    bool copied = in >= 0 && out >= 0;
    while (copied && (n = read(in, buf, sizeof(buf))) > 0)
        copied = write(out, buf, (size_t) n) == n;
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    check(copied, "copy this program to %s", prog);
    if (copied) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            char *av[] = {prog, "--exe", prog, NULL};
            execve(prog, av, environ);
            _exit(99);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        check(code == 0, "its own file unlinked: /proc/self/exe is \"%s (deleted)\" (exit %d: 10 is "
              "without the suffix, 20 something else)", prog, code);
    }
    unlink(prog);
    rmdir(dir);
    return finish_suite("proc_link_deleted");
}
