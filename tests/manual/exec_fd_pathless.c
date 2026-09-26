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
//   - open("/proc/self/exe") in the new image reaches the file it runs --
//     the same inode as the caller's descriptor, not whatever took the name
//     since -- and F_GET_SEALS through it gives the memfd's seals. runc's
//     is_self_cloned() is exactly that open and that question. AOK walked
//     the link's text, which names nothing for a file with no path: ENOENT.
//   - the #! interpreter reads the script through a description of its own:
//     the caller's offset, 12, is still 12 afterwards. AOK handed the
//     interpreter the caller's own description, rewound to 0.
//
// And O_PATH through the same links: open("/proc/self/fd/<n>", O_PATH) of a
// memfd or an unlinked file is a handle on that very file (Linux's walk jumps
// to it), which fexecve runs; so is O_PATH of /proc/self/exe in the new image.
// AOK walked the link's text: ENOENT for a memfd, and for an unlinked file
// whatever had taken its name since.
//
// And /proc/self/fd/<n> of a file with no path, opened directly: a new
// description at offset 0 whose reads leave the caller's where it was, the
// same inode, the access mode asked for. As Linux opens the inode afresh, an
// O_RDWR open of an O_RDONLY descriptor is allowed wherever the file's
// permissions allow it; that is asserted for memfds and in a tmpfs. In /tmp
// it is not: AOK's /tmp is a host file, and Darwin has no way to open an
// unlinked file afresh, so AOK's new description is a duplicate of the
// caller's host one and can have no access that one lacks (EACCES).
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
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

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

#define RUNC_SEALS (F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE)
#define NO_SEALS_CHECK (-1000)

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

// The exec'd copy of this program: what the kernel told it about itself, and
// what open("/proc/self/exe") reaches -- runc's is_self_cloned() opens it and
// asks F_GET_SEALS.
static int report(int out) {
    char exe[PATH_MAX], comm[32] = "";
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    exe[n > 0 ? n : 0] = '\0';
    prctl(PR_GET_NAME, comm);
    const char *execfn = (const char *) getauxval(AT_EXECFN);
    int open_err = 0, seals = 0, opath_err = 0;
    struct stat st = {0}, opath_st = {0};
    int handle = open("/proc/self/exe", O_PATH | O_CLOEXEC);
    if (handle < 0 || fstat(handle, &opath_st) != 0)
        opath_err = errno;
    if (handle >= 0)
        close(handle);
    int self = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (self < 0) {
        open_err = errno;
    } else {
        if (fstat(self, &st) != 0)
            open_err = errno;
        seals = fcntl(self, F_GET_SEALS);
        if (seals < 0)
            seals = -errno;
        close(self);
    }
    dprintf(out, "R\n%s\n%s\n%s\n%d\n%llu\n%llu\n%d\n%d\n%llu\n%llu\n",
            execfn != NULL ? execfn : "(none)", exe, comm, open_err, (unsigned long long) st.st_dev,
            (unsigned long long) st.st_ino, seals, opath_err, (unsigned long long) opath_st.st_dev,
            (unsigned long long) opath_st.st_ino);
    return 42;
}

struct outcome {
    int err;        // the exec's errno, when it returned
    int status;     // the child's wait status
    bool ran;       // this program ran and reported
    char execfn[PATH_MAX], exe[PATH_MAX], comm[32];
    int exe_open_err;                           // open("/proc/self/exe")
    unsigned long long exe_dev, exe_ino;        // ...and what it reached
    int exe_seals;                              // F_GET_SEALS, or -errno
    int exe_opath_err;                          // open("/proc/self/exe", O_PATH)
    unsigned long long exe_opath_dev, exe_opath_ino;
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
        const char *fields[10] = {"", "", "", "-1", "0", "0", "0", "-1", "0", "0"};
        for (int i = 0; i < 10; i++) {
            char *f = strtok_r(NULL, "\n", &save);
            if (f != NULL)
                fields[i] = f;
        }
        o->ran = true;
        snprintf(o->execfn, sizeof(o->execfn), "%s", fields[0]);
        snprintf(o->exe, sizeof(o->exe), "%s", fields[1]);
        snprintf(o->comm, sizeof(o->comm), "%s", fields[2]);
        o->exe_open_err = atoi(fields[3]);
        o->exe_dev = strtoull(fields[4], NULL, 10);
        o->exe_ino = strtoull(fields[5], NULL, 10);
        o->exe_seals = atoi(fields[6]);
        o->exe_opath_err = atoi(fields[7]);
        o->exe_opath_dev = strtoull(fields[8], NULL, 10);
        o->exe_opath_ino = strtoull(fields[9], NULL, 10);
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
// number up to Linux 6.13 and `name` -- the file's own -- from 6.14. And its
// /proc/self/exe opens to the very file `fd` holds, with `seals` (unless
// NO_SEALS_CHECK) as its F_GET_SEALS answer.
static bool expect_runs(const char *how, int fd, const char *name, int seals, struct outcome *o) {
    char got[4 * PATH_MAX], execfn[32], fdnum[16];
    struct stat held;
    check(fstat(fd, &held) == 0, "%s: fstat the descriptor (%s)", how, strerror(errno));
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
    check(o->exe_open_err == 0, "%s: open(/proc/self/exe) in the new image (%s)", how,
          o->exe_open_err != 0 ? strerror(o->exe_open_err) : "ok");
    if (o->exe_open_err == 0)
        check(o->exe_dev == (unsigned long long) held.st_dev &&
                  o->exe_ino == (unsigned long long) held.st_ino,
              "%s: /proc/self/exe opens the file it runs, dev %llu ino %llu (got dev %llu ino %llu)",
              how, (unsigned long long) held.st_dev, (unsigned long long) held.st_ino, o->exe_dev,
              o->exe_ino);
    check(o->exe_opath_err == 0, "%s: open(/proc/self/exe, O_PATH) in the new image (%s)", how,
          o->exe_opath_err != 0 ? strerror(o->exe_opath_err) : "ok");
    if (o->exe_opath_err == 0)
        check(o->exe_opath_dev == (unsigned long long) held.st_dev &&
                  o->exe_opath_ino == (unsigned long long) held.st_ino,
              "%s: /proc/self/exe O_PATH is the file it runs, dev %llu ino %llu (got dev %llu ino %llu)",
              how, (unsigned long long) held.st_dev, (unsigned long long) held.st_ino,
              o->exe_opath_dev, o->exe_opath_ino);
    if (o->exe_open_err == 0 && seals != NO_SEALS_CHECK)
        check(o->exe_seals == seals, "%s: F_GET_SEALS through /proc/self/exe is %#x (got %d)", how,
              seals, o->exe_seals);
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

// open("/proc/self/fd/<fd>", O_PATH): a handle on the file `fd` holds -- the
// same inode, O_PATH as F_GETFL has it, nothing to read -- which fexecve runs.
static void expect_opath_link(const char *how, int fd, const char *name) {
    char proc[64], b[1];
    struct stat held, st;
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    check(fstat(fd, &held) == 0, "%s: fstat the descriptor (%s)", how, strerror(errno));
    int h = open(proc, O_PATH | O_CLOEXEC);
    check(h >= 0, "%s: open %s O_PATH (%s)", how, proc, strerror(errno));
    if (h < 0)
        return;
    check(fstat(h, &st) == 0 && st.st_dev == held.st_dev && st.st_ino == held.st_ino,
          "%s: O_PATH: the same file, dev %llu ino %llu (got dev %llu ino %llu)", how,
          (unsigned long long) held.st_dev, (unsigned long long) held.st_ino,
          (unsigned long long) st.st_dev, (unsigned long long) st.st_ino);
    int fl = fcntl(h, F_GETFL);
    check(fl == O_PATH, "%s: O_PATH: F_GETFL is O_PATH (got %#x)", how, fl);
    errno = 0;
    check(read(h, b, 1) < 0 && errno == EBADF, "%s: O_PATH: read is EBADF (%s)", how,
          strerror(errno));
    errno = 0;
    int d = open(proc, O_PATH | O_DIRECTORY | O_CLOEXEC);
    check(d < 0 && errno == ENOTDIR, "%s: O_PATH|O_DIRECTORY is ENOTDIR (%s)", how,
          d >= 0 ? "opened" : strerror(errno));
    if (d >= 0)
        close(d);
    char run[PATH_MAX + 64];
    struct outcome o;
    snprintf(run, sizeof(run), "%s, through its O_PATH handle", how);
    expect_runs(run, h, name, NO_SEALS_CHECK, &o);
    close(h);
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

#define REOPEN_DATA "0123456789abcdefghij"

// /proc/self/fd/<fd> of a file with no path, opened directly. `fd` is
// O_RDONLY, and the file holds REOPEN_DATA. The open is a new description:
// offset 0, the access mode asked for, and reads that leave the caller's
// offset alone. `upgrade`: also O_RDWR, which Linux grants by the file's
// permissions whatever the caller's descriptor allows.
static void expect_reopen(const char *how, int fd, bool upgrade) {
    char proc[64], got[8];
    struct stat held, st;
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    check(fstat(fd, &held) == 0, "%s: fstat the descriptor (%s)", how, strerror(errno));
    check(lseek(fd, 7, SEEK_SET) == 7, "%s: seek the descriptor to 7 (%s)", how, strerror(errno));

    int r = open(proc, O_RDONLY | O_CLOEXEC);
    check(r >= 0, "%s: open %s O_RDONLY (%s)", how, proc, strerror(errno));
    if (r >= 0) {
        check(fstat(r, &st) == 0 && st.st_dev == held.st_dev && st.st_ino == held.st_ino,
              "%s: O_RDONLY: the same file", how);
        check(lseek(r, 0, SEEK_CUR) == 0, "%s: O_RDONLY: starts at offset 0 (got %lld)", how,
              (long long) lseek(r, 0, SEEK_CUR));
        check(read(r, got, 4) == 4 && memcmp(got, REOPEN_DATA, 4) == 0,
              "%s: O_RDONLY: reads the file from its start", how);
        off_t mine = lseek(r, 0, SEEK_CUR), theirs = lseek(fd, 0, SEEK_CUR);
        check(mine == 4 && theirs == 7, "%s: O_RDONLY: its offset is 4 and the caller's 7 (got %lld "
              "and %lld)", how, (long long) mine, (long long) theirs);
        check(lseek(fd, 15, SEEK_SET) == 15 && lseek(r, 0, SEEK_CUR) == 4,
              "%s: O_RDONLY: the caller's seek leaves it at 4", how);
        int acc = fcntl(r, F_GETFL) & O_ACCMODE;
        check(acc == O_RDONLY, "%s: O_RDONLY: F_GETFL says O_RDONLY (got %#x)", how, acc);
        errno = 0;
        check(write(r, "x", 1) < 0 && errno == EBADF, "%s: O_RDONLY: write is EBADF (%s)", how,
              strerror(errno));
        close(r);
    }

    if (upgrade) {
        int w = open(proc, O_RDWR | O_CLOEXEC);
        check(w >= 0, "%s: open %s O_RDWR (%s)", how, proc, strerror(errno));
        if (w >= 0) {
            int acc = fcntl(w, F_GETFL) & O_ACCMODE;
            check(acc == O_RDWR, "%s: O_RDWR: F_GETFL says O_RDWR (got %#x)", how, acc);
            check(pwrite(w, "XY", 2, 1) == 2, "%s: O_RDWR: write (%s)", how, strerror(errno));
            check(pread(fd, got, 3, 0) == 3 && memcmp(got, "0XY", 3) == 0,
                  "%s: O_RDWR: the write is in the caller's file", how);
            close(w);
        }
    }
}

// A file holding REOPEN_DATA at dir/name, opened O_RDONLY and unlinked.
static void unlinked_reopen(const char *dir) {
    char path[PATH_MAX], how[PATH_MAX + 64];
    snprintf(path, sizeof(path), "%s/data", dir);
    put(path, REOPEN_DATA, 0644);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    check(fd >= 0, "open %s (%s)", path, strerror(errno));
    check(unlink(path) == 0, "unlink %s (%s)", path, strerror(errno));
    if (fd < 0)
        return;
    // Something else at the name now: the reopen must not reach it.
    put(path, "not the file\n", 0644);
    struct statfs sfs;
    bool tmpfs = statfs(dir, &sfs) == 0 && (unsigned long) sfs.f_type == TMPFS_MAGIC;
    snprintf(how, sizeof(how), "reopen of unlinked %s", path);
    expect_reopen(how, fd, tmpfs);
    close(fd);
    unlink(path);
}

static void unlinked_files(const char *dir) {
    char how[PATH_MAX + 64], path[PATH_MAX];
    struct outcome o;

    // An ELF file, close-on-exec, the way fexecve is usually handed one.
    int fd = unlinked_copy(dir, "prog", O_RDONLY | O_CLOEXEC);
    snprintf(how, sizeof(how), "fexecve of unlinked %s/prog", dir);
    if (fd >= 0 && expect_runs(how, fd, "prog", NO_SEALS_CHECK, &o)) {
        snprintf(path, sizeof(path), "%s/prog (deleted)", dir);
        check(strcmp(o.exe, path) == 0, "%s: /proc/self/exe is %s (got %s)", how, path, o.exe);
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
        expect_runs(how, fd, "gone", NO_SEALS_CHECK, &o);
        snprintf(how, sizeof(how), "unlinked %s/gone, with a script there now", dir);
        expect_opath_link(how, fd, "gone");
        close(fd);
    }
    unlink(path);

    // The caller's position is its own: the child shares this description
    // (not close-on-exec, so the new image holds it too) and exec reads the
    // file without moving it.
    fd = unlinked_copy(dir, "pos", O_RDONLY);
    snprintf(how, sizeof(how), "fexecve of unlinked %s/pos at offset 1234", dir);
    if (fd >= 0 && lseek(fd, 1234, SEEK_SET) == 1234) {
        expect_runs(how, fd, "pos", NO_SEALS_CHECK, &o);
        off_t at = lseek(fd, 0, SEEK_CUR);
        check(at == 1234, "%s: the offset is still 1234 (got %lld)", how, (long long) at);
    }
    if (fd >= 0)
        close(fd);

    // O_PATH: nothing can be read through it, and Linux runs it anyway.
    fd = unlinked_copy(dir, "opath", O_PATH | O_CLOEXEC);
    snprintf(how, sizeof(how), "fexecve of an O_PATH descriptor of unlinked %s/opath", dir);
    if (fd >= 0) {
        expect_runs(how, fd, "opath", NO_SEALS_CHECK, &o);
        close(fd);
    }

    // A #! script. Through a descriptor left open the interpreter opens
    // /dev/fd/<n> and reads the script from its start -- the caller's
    // offset is past the #! line -- through a description of its own, so
    // the caller's offset is where it was. Close-on-exec, it cannot.
    snprintf(path, sizeof(path), "%s/scr", dir);
    put(path, "#!/bin/sh\nexit 5\n", 0755);
    fd = open(path, O_RDONLY);
    check(fd >= 0, "open %s (%s)", path, strerror(errno));
    check(unlink(path) == 0, "unlink %s (%s)", path, strerror(errno));
    snprintf(how, sizeof(how), "fexecve of unlinked script %s/scr", dir);
    if (fd >= 0 && lseek(fd, 12, SEEK_SET) == 12) {
        expect_exit(how, fd, 5);
        off_t at = lseek(fd, 0, SEEK_CUR);
        check(at == 12, "%s: the offset is still 12 (got %lld)", how, (long long) at);
        int ce = fcntl(fd, F_DUPFD_CLOEXEC, 0);
        snprintf(how, sizeof(how), "fexecve of unlinked script %s/scr, close-on-exec", dir);
        expect_err(how, ce, ENOENT);
        close(ce);
    }
    if (fd >= 0)
        close(fd);

    unlinked_reopen(dir);
}

// A memfd's /proc/self/fd reopened O_RDONLY is a read-only description of
// the memfd: it cannot be written, write-mapped shared, truncated or sealed
// (Linux's memfd_add_seals wants FMODE_WRITE), and it reads the seals.
static void memfd_reopen(void) {
    int fd = memfd("reopen", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    check(fd >= 0, "memfd_create (%s)", strerror(errno));
    if (fd < 0 || !put_fd(fd, "memfd", REOPEN_DATA, strlen(REOPEN_DATA))) {
        if (fd >= 0)
            close(fd);
        return;
    }
    check(fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) == 0, "seal the memfd (%s)", strerror(errno));
    char proc[64];
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
    int ro = open(proc, O_RDONLY | O_CLOEXEC);
    check(ro >= 0, "memfd: open %s O_RDONLY (%s)", proc, strerror(errno));
    close(fd);   // the read-only description is all that holds it now
    if (ro < 0)
        return;

    char link[PATH_MAX];
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", ro);
    ssize_t n = readlink(proc, link, sizeof(link) - 1);
    link[n > 0 ? n : 0] = '\0';
    check(strcmp(link, "/memfd:reopen (deleted)") == 0,
          "memfd reopen: the link is /memfd:reopen (deleted) (got %s)", link);
    int seals = fcntl(ro, F_GET_SEALS);
    check(seals == F_SEAL_SHRINK, "memfd reopen: F_GET_SEALS is F_SEAL_SHRINK (got %d)", seals);
    errno = 0;
    check(fcntl(ro, F_ADD_SEALS, F_SEAL_GROW) < 0 && errno == EPERM,
          "memfd reopen: F_ADD_SEALS through a read-only description is EPERM (%s)", strerror(errno));
    errno = 0;
    check(ftruncate(ro, 100) < 0 && errno == EINVAL,
          "memfd reopen: ftruncate through a read-only description is EINVAL (%s)", strerror(errno));
    void *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ro, 0);
    check(map == MAP_FAILED && errno == EACCES,
          "memfd reopen: a writable shared mapping of a read-only description is EACCES (%s)",
          map == MAP_FAILED ? strerror(errno) : "mapped");
    if (map != MAP_FAILED)
        munmap(map, 4096);
    map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, ro, 0);
    check(map != MAP_FAILED && memcmp(map, REOPEN_DATA, 4) == 0,
          "memfd reopen: a read-only shared mapping reads the memfd (%s)",
          map == MAP_FAILED ? strerror(errno) : "mapped");
    if (map != MAP_FAILED)
        munmap(map, 4096);
    expect_reopen("reopen of a memfd's read-only description", ro, true);
    close(ro);
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
        if (expect_runs("fexecve of a sealed memfd", fd, "memfd:runc_clon", RUNC_SEALS, &o))
            check(strcmp(o.exe, "/memfd:runc_cloned (deleted)") == 0,
                  "memfd: /proc/self/exe is /memfd:runc_cloned (deleted) (got %s)", o.exe);
        check(lseek(fd, 0, SEEK_CUR) == end, "memfd: the offset is still at the end");
        expect_opath_link("a sealed memfd", fd, "memfd:runc_clon");
    }
    close(fd);

    // Unsealed, as Python's os.fexecve gets one.
    fd = memfd("py", MFD_CLOEXEC);
    check(fd >= 0, "memfd_create (%s)", strerror(errno));
    if (fd >= 0 && copy_self_to(fd, "memfd"))
        expect_runs("fexecve of a memfd", fd, "memfd:py", F_SEAL_SEAL, &o);
    if (fd >= 0)
        close(fd);

    // A #! script in a memfd: the interpreter opens /dev/fd/<n>.
    fd = memfd("scr", 0);
    check(fd >= 0, "memfd_create (%s)", strerror(errno));
    if (fd >= 0 && put_fd(fd, "memfd", "#!/bin/sh\nexit 6\n", 17) &&
            lseek(fd, 12, SEEK_SET) == 12) {
        expect_exit("fexecve of a #! memfd", fd, 6);
        off_t at = lseek(fd, 0, SEEK_CUR);
        check(at == 12, "fexecve of a #! memfd: the offset is still 12 (got %lld)", (long long) at);
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

    memfd_reopen();
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
