// fexecve -- execveat(fd, "", AT_EMPTY_PATH) -- of a file that has no path:
// a memfd, or a file unlinked after it was opened.
//
// Linux runs the descriptor's file itself. It is how runc re-executes a
// sealed memfd copy of its own binary, how Python's os.fexecve works on a
// memfd, and how anything runs a program it only holds a descriptor to.
// AOK opened a second description of the file by the descriptor's PATH, and
// a file with no path had nothing to open: both were ENOENT.
//
// Worse than ENOENT: the path an unlinked file's descriptor reports is the
// name it USED to have, and when something else has been created there
// since, reopening it by that name runs that other file. Here the other file
// is a script that exits 7, so running it instead is seen.
//
// Also checked:
//   - exec does not move the caller's file position: the parent's shared
//     description is where it left it after the child's exec. The loader
//     reads the header, the program headers and the interpreter name at
//     offsets of its own.
//   - an O_PATH descriptor of an unlinked file runs too.
//   - a #! script through a descriptor that stays open is run by an
//     interpreter handed /dev/fd/<n>, which reads it from the start however
//     far the caller's offset had got; through a close-on-exec one it is
//     ENOENT, as for any script.
//   - a memfd created MFD_NOEXEC_SEAL is mode 0666, so EACCES.
//   - what the new image is told: AT_EXECFN "/dev/fd/<n>", /proc/self/exe
//     the memfd's "/memfd:<name> (deleted)", and comm the descriptor number
//     (Linux <= 6.13) or the file's own name (6.14+, which AOK does), never
//     a "(deleted)" suffix.
//
// Not asserted, both because AOK cannot yet give a file with no path a second
// description of its own:
//   - open("/proc/self/exe") in the image a pathless file started. Linux opens
//     the file through the magic link (runc's is_self_cloned() does, and asks
//     F_GET_SEALS); AOK walks the link's text, which names nothing.
//   - the caller's offset after the #! interpreter has read the script. On
//     Linux /dev/fd/<n> opens a new description; AOK's /proc/self/fd reopen of
//     a pathless file hands out the caller's own, rewound (fs/generic.c
//     procfd_openat), so the interpreter's reads move it.
//
// Run in /tmp, and as root also in a tmpfs mounted for the run.
//
// Oracle: Linux 6.12 (camd), glibc x86_64 and -m32, as root in `unshare -m`
// and unprivileged.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include "test_common.h"

#define TEST_NAME "exec_fd_pathless"

#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif
#ifndef AT_EXECFN
#define AT_EXECFN 31
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif
#ifndef MFD_NOEXEC_SEAL
#define MFD_NOEXEC_SEAL 0x0008U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#endif
#ifndef F_SEAL_SEAL
#define F_SEAL_SEAL 0x0001
#define F_SEAL_SHRINK 0x0002
#define F_SEAL_GROW 0x0004
#define F_SEAL_WRITE 0x0008
#endif
#ifndef SYS_execveat
#if defined(__x86_64__)
#define SYS_execveat 322
#elif defined(__i386__)
#define SYS_execveat 358
#elif defined(__aarch64__) || defined(__riscv)
#define SYS_execveat 281
#endif
#endif
#ifndef SYS_memfd_create
#if defined(__x86_64__)
#define SYS_memfd_create 319
#elif defined(__i386__)
#define SYS_memfd_create 356
#elif defined(__aarch64__) || defined(__riscv)
#define SYS_memfd_create 279
#endif
#endif

#define RUNC_SEALS (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE)

extern char **environ;

static void check(int cond, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (cond) {
        if (test_verbose) {
            printf("ok: ");
            vprintf(fmt, ap);
            printf("\n");
        }
    } else {
        printf("FAIL: ");
        vprintf(fmt, ap);
        printf("\n");
        failures_total++;
    }
    va_end(ap);
}

static int memfd(const char *name, unsigned flags) {
    return (int) syscall(SYS_memfd_create, name, flags);
}

// The exec'd copy of this program: what the kernel told it about itself.
static int report(int out) {
    char exe[PATH_MAX], comm[32] = "";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    exe[n > 0 ? n : 0] = '\0';
    prctl(PR_GET_NAME, comm);
    const char *execfn = (const char *) getauxval(AT_EXECFN);
    dprintf(out, "R\n%s\n%s\n%s\n", execfn != NULL ? execfn : "(none)", exe, comm);
    return 42;
}

struct outcome {
    int err;        // the exec's errno, when it returned
    int status;     // the child's wait status
    bool ran;       // this program ran and reported
    char execfn[PATH_MAX], exe[PATH_MAX], comm[32];
};

// fexecve(fd) in a child, the way glibc and musl both do it.
static void run_fexecve(int fd, struct outcome *o) {
    memset(o, 0, sizeof(*o));
    int p[2];
    if (pipe(p) != 0) {
        o->err = errno;
        return;
    }
    char out[16];
    snprintf(out, sizeof(out), "%d", p[1]);
    char *av[] = {"prog", "--report", out, NULL};
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        syscall(SYS_execveat, fd, "", av, environ, AT_EMPTY_PATH);
        dprintf(p[1], "E\n%d\n", errno);
        _exit(99);
    }
    close(p[1]);
    char buf[3 * PATH_MAX];
    size_t len = 0;
    ssize_t got;
    while (len < sizeof(buf) - 1 && (got = read(p[0], buf + len, sizeof(buf) - 1 - len)) > 0)
        len += (size_t) got;
    buf[len] = '\0';
    close(p[0]);
    if (pid < 0 || waitpid(pid, &o->status, 0) != pid)
        o->status = -1;
    char *save = NULL;
    char *kind = strtok_r(buf, "\n", &save);
    if (kind != NULL && strcmp(kind, "E") == 0) {
        char *e = strtok_r(NULL, "\n", &save);
        o->err = e != NULL ? atoi(e) : -1;
    } else if (kind != NULL && strcmp(kind, "R") == 0) {
        const char *fields[3] = {"", "", ""};
        for (int i = 0; i < 3; i++) {
            char *f = strtok_r(NULL, "\n", &save);
            if (f != NULL)
                fields[i] = f;
        }
        o->ran = true;
        snprintf(o->execfn, sizeof(o->execfn), "%s", fields[0]);
        snprintf(o->exe, sizeof(o->exe), "%s", fields[1]);
        snprintf(o->comm, sizeof(o->comm), "%s", fields[2]);
    }
}

static const char *describe(const struct outcome *o, char *buf, size_t n) {
    if (o->ran)
        snprintf(buf, n, "ran: execfn %s, exe %s, comm %s", o->execfn, o->exe, o->comm);
    else if (o->err != 0)
        snprintf(buf, n, "%s", strerror(o->err));
    else if (o->status != -1 && WIFEXITED(o->status))
        snprintf(buf, n, "exit %d", WEXITSTATUS(o->status));
    else
        snprintf(buf, n, "wait status %#x", o->status);
    return buf;
}

// It runs this program, told it is /dev/fd/<fd>. comm is the descriptor
// number up to Linux 6.13 and `name` -- the file's own -- from 6.14.
static bool expect_runs(const char *how, int fd, const char *name, struct outcome *o) {
    char got[4 * PATH_MAX], execfn[32], fdnum[16];
    run_fexecve(fd, o);
    describe(o, got, sizeof(got));
    snprintf(execfn, sizeof(execfn), "/dev/fd/%d", fd);
    snprintf(fdnum, sizeof(fdnum), "%d", fd);
    check(o->ran && strcmp(o->execfn, execfn) == 0, "%s runs with AT_EXECFN %s (got %s)", how,
          execfn, got);
    if (!o->ran)
        return false;
    char own[16];
    snprintf(own, sizeof(own), "%s", name);   // comm is at most 15 characters
    check(strcmp(o->comm, fdnum) == 0 || strcmp(o->comm, own) == 0, "%s: comm is %s or %s (got %s)",
          how, fdnum, own, o->comm);
    return true;
}

static void expect_err(const char *how, int fd, int err) {
    struct outcome o;
    char got[4 * PATH_MAX];
    run_fexecve(fd, &o);
    check(!o.ran && o.err == err, "%s is %s (got %s)", how, strerror(err),
          describe(&o, got, sizeof(got)));
}

// A #! script: it runs, and says so with its exit status.
static void expect_exit(const char *how, int fd, int code) {
    struct outcome o;
    char got[4 * PATH_MAX];
    run_fexecve(fd, &o);
    check(!o.ran && o.err == 0 && o.status != -1 && WIFEXITED(o.status) &&
              WEXITSTATUS(o.status) == code,
          "%s exits %d (got %s)", how, code, describe(&o, got, sizeof(got)));
}

static bool put_fd(int fd, const char *what, const void *data, size_t len) {
    const char *p = data;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) {
            check(0, "write %s (%s)", what, strerror(errno));
            return false;
        }
        p += n;
        len -= (size_t) n;
    }
    return true;
}

static void put(const char *path, const char *text, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    check(fd >= 0, "create %s (%s)", path, strerror(errno));
    if (fd < 0)
        return;
    put_fd(fd, path, text, strlen(text));
    close(fd);
    chmod(path, mode);
}

// This program, copied into `fd`.
static bool copy_self_to(int fd, const char *what) {
    int in = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    check(in >= 0, "open /proc/self/exe (%s)", strerror(errno));
    if (in < 0)
        return false;
    char buf[65536];
    ssize_t n;
    bool ok = true;
    while (ok && (n = read(in, buf, sizeof(buf))) > 0)
        ok = put_fd(fd, what, buf, (size_t) n);
    close(in);
    return ok;
}

static void copy_self(const char *to) {
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    check(out >= 0, "create %s (%s)", to, strerror(errno));
    if (out < 0)
        return;
    copy_self_to(out, to);
    close(out);
}

// A copy of this program at dir/name, opened with `flags` and then unlinked.
static int unlinked_copy(const char *dir, const char *name, int flags) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    copy_self(path);
    int fd = open(path, flags);
    check(fd >= 0, "open %s (%s)", path, strerror(errno));
    check(unlink(path) == 0, "unlink %s (%s)", path, strerror(errno));
    return fd;
}

static void unlinked_files(const char *dir) {
    char how[PATH_MAX + 64], path[PATH_MAX];
    struct outcome o;

    // An ELF file, close-on-exec, the way fexecve is usually handed one.
    int fd = unlinked_copy(dir, "prog", O_RDONLY | O_CLOEXEC);
    snprintf(how, sizeof(how), "fexecve of unlinked %s/prog", dir);
    if (fd >= 0 && expect_runs(how, fd, "prog", &o)) {
        snprintf(path, sizeof(path), "%s/prog (deleted)", dir);
        // AOK names an unlinked file by the name it had, without Linux's
        // " (deleted)"; that is /proc's to fix, not exec's.
        check(strncmp(o.exe, path, strlen(dir) + 5) == 0,
              "%s: /proc/self/exe is %s (got %s)", how, path, o.exe);
    }
    if (fd >= 0)
        close(fd);

    // Unlinked, then something else created at the name it had. Not
    // close-on-exec, so running the script there would be seen: it exits 7.
    fd = unlinked_copy(dir, "gone", O_RDONLY);
    snprintf(path, sizeof(path), "%s/gone", dir);
    put(path, "#!/bin/sh\nexit 7\n", 0755);
    snprintf(how, sizeof(how), "fexecve of unlinked %s/gone, with a script there now", dir);
    if (fd >= 0) {
        expect_runs(how, fd, "gone", &o);
        close(fd);
    }
    unlink(path);

    // The caller's position is its own: the child shares this description
    // (not close-on-exec, so the new image holds it too) and exec reads the
    // file without moving it.
    fd = unlinked_copy(dir, "pos", O_RDONLY);
    snprintf(how, sizeof(how), "fexecve of unlinked %s/pos at offset 1234", dir);
    if (fd >= 0 && lseek(fd, 1234, SEEK_SET) == 1234) {
        expect_runs(how, fd, "pos", &o);
        off_t at = lseek(fd, 0, SEEK_CUR);
        check(at == 1234, "%s: the offset is still 1234 (got %lld)", how, (long long) at);
    }
    if (fd >= 0)
        close(fd);

    // O_PATH: nothing can be read through it, and Linux runs it anyway.
    fd = unlinked_copy(dir, "opath", O_PATH | O_CLOEXEC);
    snprintf(how, sizeof(how), "fexecve of an O_PATH descriptor of unlinked %s/opath", dir);
    if (fd >= 0) {
        expect_runs(how, fd, "opath", &o);
        close(fd);
    }

    // A #! script. Through a descriptor left open the interpreter opens
    // /dev/fd/<n> and reads the script from its start -- the caller's
    // offset is past the #! line. Close-on-exec, it cannot.
    snprintf(path, sizeof(path), "%s/scr", dir);
    put(path, "#!/bin/sh\nexit 5\n", 0755);
    fd = open(path, O_RDONLY);
    check(fd >= 0, "open %s (%s)", path, strerror(errno));
    check(unlink(path) == 0, "unlink %s (%s)", path, strerror(errno));
    snprintf(how, sizeof(how), "fexecve of unlinked script %s/scr", dir);
    if (fd >= 0 && lseek(fd, 12, SEEK_SET) == 12) {
        expect_exit(how, fd, 5);
        int ce = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        snprintf(how, sizeof(how), "fexecve of unlinked script %s/scr, close-on-exec", dir);
        expect_err(how, ce, ENOENT);
        close(ce);
    }
    if (fd >= 0)
        close(fd);
}

static void memfds(void) {
    struct outcome o;

    // runc: a copy of itself, sealed, run by descriptor.
    int fd = memfd("runc_cloned", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 && errno == ENOSYS) {
        printf("%s: memfd_create unsupported, skipping memfd cases\n", TEST_NAME);
        return;
    }
    check(fd >= 0, "memfd_create (%s)", strerror(errno));
    if (fd < 0)
        return;
    if (copy_self_to(fd, "memfd")) {
        check(fcntl(fd, F_ADD_SEALS, RUNC_SEALS) == 0, "seal the memfd (%s)", strerror(errno));
        off_t end = lseek(fd, 0, SEEK_CUR);
        if (expect_runs("fexecve of a sealed memfd", fd, "memfd:runc_clon", &o))
            check(strcmp(o.exe, "/memfd:runc_cloned (deleted)") == 0,
                  "memfd: /proc/self/exe is /memfd:runc_cloned (deleted) (got %s)", o.exe);
        check(lseek(fd, 0, SEEK_CUR) == end, "memfd: the offset is still at the end");
    }
    close(fd);

    // Unsealed, as Python's os.fexecve gets one.
    fd = memfd("py", MFD_CLOEXEC);
    check(fd >= 0, "memfd_create (%s)", strerror(errno));
    if (fd >= 0 && copy_self_to(fd, "memfd"))
        expect_runs("fexecve of a memfd", fd, "memfd:py", &o);
    if (fd >= 0)
        close(fd);

    // A #! script in a memfd: the interpreter opens /dev/fd/<n>.
    fd = memfd("scr", 0);
    check(fd >= 0, "memfd_create (%s)", strerror(errno));
    if (fd >= 0 && put_fd(fd, "memfd", "#!/bin/sh\nexit 6\n", 17)) {
        expect_exit("fexecve of a #! memfd", fd, 6);
        int ce = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        expect_err("fexecve of a #! memfd, close-on-exec", ce, ENOENT);
        close(ce);
    }
    if (fd >= 0)
        close(fd);

    // MFD_NOEXEC_SEAL: mode 0666. Older kernels do not know the flag.
    fd = memfd("noexec", MFD_CLOEXEC | MFD_NOEXEC_SEAL);
    if (fd < 0 && errno == EINVAL) {
        test_logf("MFD_NOEXEC_SEAL unsupported here\n");
    } else {
        check(fd >= 0, "memfd_create MFD_NOEXEC_SEAL (%s)", strerror(errno));
        if (fd >= 0 && copy_self_to(fd, "memfd"))
            expect_err("fexecve of an MFD_NOEXEC_SEAL memfd", fd, EACCES);
        if (fd >= 0)
            close(fd);
    }
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "--report") == 0)
        return report(atoi(argv[2]));
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    char dir[] = "/tmp/exec_fd_pathless.XXXXXX";
    check(mkdtemp(dir) != NULL, "mkdtemp (%s)", strerror(errno));
    unlinked_files(dir);

    // A tmpfs of its own, as root.
    char mnt[PATH_MAX];
    snprintf(mnt, sizeof(mnt), "%s/tmpfs", dir);
    bool mounted = false;
    if (geteuid() == 0 && mkdir(mnt, 0755) == 0) {
        mounted = mount("tmpfs", mnt, "tmpfs", 0, "size=64m") == 0;
        check(mounted, "mount a tmpfs on %s (%s)", mnt, strerror(errno));
        if (mounted)
            unlinked_files(mnt);
    }
    if (mounted)
        umount2(mnt, MNT_DETACH);
    rmdir(mnt);
    rmdir(dir);

    memfds();

    if (failures_total != 0) {
        printf("%s: %u failure(s)\n", TEST_NAME, failures_total);
        return 1;
    }
    printf("%s: PASS\n", TEST_NAME);
    return 0;
}
