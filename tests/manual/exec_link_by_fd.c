// execveat() and linkat(AT_EMPTY_PATH) through a descriptor: in a mount that
// `umount -l` has detached, and from inside a chroot.
//
// Both take the file the descriptor names, not a path. AOK's paths are
// strings, so it asked the descriptor for its path (generic_getpath) and
// resolved that again from the top -- and a path is exactly what these
// descriptors do not have:
//
//   - In a detached mount the path is a private staging point,
//     /.ish-fsmount/<n>, that no walk from the root may enter (fs/path.c).
//     execveat(fd, "", AT_EMPTY_PATH) -- fexecve -- and execveat(dirfd,
//     "prog", 0) were ENOENT, and so was linkat(fd, "", dirfd, "new",
//     AT_EMPTY_PATH). Linux 6.12 runs and links all of them.
//   - In a chroot the path carries the chroot's own prefix, and resolving it
//     from the chroot's root put the prefix on twice: execveat(dirfd, "junk")
//     and execveat(fd, "", AT_EMPTY_PATH) looked for /jail/jail/junk, and
//     even execveat(AT_FDCWD, "junk") did. ENOENT again.
//
// What the new image is told about itself follows Linux's bprm->filename:
// AT_EXECFN (and the name a #! interpreter is handed) is "/dev/fd/<n>" for
// an empty name and "/dev/fd/<n>/<name>" for one relative to a descriptor,
// never the descriptor's path -- which in a detached mount would print the
// staging point. So a script run through a descriptor works exactly when the
// interpreter can open /dev/fd/<n>: through a close-on-exec descriptor it
// cannot, and Linux refuses the exec with ENOENT rather than start an
// interpreter that will fail. /proc/self/exe is the file's own path, which in
// a detached mount starts at the mount's root.
//
// Also here, because it failed the same way: a /proc/self/fd/N reopen of a
// file in a detached mount is a fresh open with the caller's flags, so an
// O_RDWR reopen of an O_RDONLY descriptor works as root. It fell back to
// handing out the old description, and refused the stronger mode.
//
// A binfmt_misc interpreter is handed the file's name too, so a registered
// format answers exactly as a #! script does (as root, with a format whose
// magic is "#elbf", registered for the run and removed after).
//
// And the flags: AT_SYMLINK_NOFOLLOW on a final symlink is ELOOP, as is an
// O_PATH descriptor of a symlink; a directory is EACCES; a file that is no
// program is ENOEXEC, which is also the witness in the chroot, where nothing
// a dynamic program needs is present.
//
// Every exec and link is run on the tmpfs before it is detached too, as the
// control. Unprivileged there is no mount and no chroot: the same checks run
// on a plain directory.
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

#define TEST_NAME "exec_link_by_fd"

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
#ifndef SYS_execveat
#if defined(__x86_64__)
#define SYS_execveat 322
#elif defined(__i386__)
#define SYS_execveat 358
#elif defined(__aarch64__) || defined(__riscv)
#define SYS_execveat 281
#endif
#endif

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

#define FEXECVE (-1)

// execveat(dirfd, name, flags) -- or fexecve(dirfd) -- in a child.
static void run_exec(int dirfd, const char *name, int flags, struct outcome *o) {
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
        if (flags == FEXECVE)
            fexecve(dirfd, av, environ);
        else
            syscall(SYS_execveat, dirfd, name, av, environ, flags);
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

// It runs this program, which is told `execfn` and whose /proc/self/exe is
// `exe`. comm is the last component of bprm->filename -- the name as spelled,
// a symlink's own and not its target's -- until Linux 6.14, which uses the
// file's own name instead when bprm->filename is a made-up "/dev/fd/<n>"
// one ("exec: fix up /proc/pid/comm in the execveat(AT_EMPTY_PATH) case").
// AOK does that. Either is Linux.
static void expect_runs(const char *how, int dirfd, const char *name, int flags,
                        const char *execfn, const char *exe) {
    struct outcome o;
    char got[4 * PATH_MAX];
    run_exec(dirfd, name, flags, &o);
    describe(&o, got, sizeof(got));
    check(o.ran && strcmp(o.execfn, execfn) == 0, "%s runs with AT_EXECFN %s (got %s)", how, execfn,
          got);
    if (!o.ran)
        return;
    check(strcmp(o.exe, exe) == 0, "%s: /proc/self/exe is %s (got %s)", how, exe, o.exe);
    const char *spelled = strrchr(execfn, '/') != NULL ? strrchr(execfn, '/') + 1 : execfn;
    const char *own = strrchr(exe, '/') != NULL ? strrchr(exe, '/') + 1 : exe;
    bool made_up = dirfd != AT_FDCWD && name[0] != '/';
    check(strcmp(o.comm, spelled) == 0 || (made_up && strcmp(o.comm, own) == 0),
          "%s: comm is %s%s%s (got %s)", how, spelled, made_up ? " or " : "",
          made_up ? own : "", o.comm);
}

static void expect_err(const char *how, int dirfd, const char *name, int flags, int err) {
    struct outcome o;
    char got[4 * PATH_MAX];
    run_exec(dirfd, name, flags, &o);
    check(!o.ran && o.err == err, "%s is %s (got %s)", how, strerror(err),
          describe(&o, got, sizeof(got)));
}

// A #! script: it runs, and says so with its exit status.
static void expect_exit(const char *how, int dirfd, const char *name, int flags, int code) {
    struct outcome o;
    char got[4 * PATH_MAX];
    run_exec(dirfd, name, flags, &o);
    check(!o.ran && o.err == 0 && o.status != -1 && WIFEXITED(o.status) &&
              WEXITSTATUS(o.status) == code,
          "%s exits %d (got %s)", how, code, describe(&o, got, sizeof(got)));
}

static int same_file(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static void put(const char *path, const char *text, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    check(fd >= 0, "create %s (%s)", path, strerror(errno));
    if (fd < 0)
        return;
    check(write(fd, text, strlen(text)) == (ssize_t) strlen(text), "write %s (%s)", path,
          strerror(errno));
    close(fd);
    chmod(path, mode);
}

static void copy_self(const char *to) {
    int in = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    check(in >= 0 && out >= 0, "copy this program to %s (%s)", to, strerror(errno));
    char buf[65536];
    ssize_t n;
    while (in >= 0 && out >= 0 && (n = read(in, buf, sizeof(buf))) > 0)
        check(write(out, buf, (size_t) n) == n, "write %s (%s)", to, strerror(errno));
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
}

// Descriptors in the directory under test. dir and script are not
// close-on-exec, so a #! interpreter can open /dev/fd/<n> through them.
struct fds {
    int dir, dir_ce, prog, prog_path, script, script_ce, misc_ce, junk, data, lnk_path;
};

static void open_fds(const char *dir, struct fds *f) {
    char p[PATH_MAX];
#define OPEN_IN(field, name, flags)                                        \
    do {                                                                   \
        snprintf(p, sizeof(p), "%s/%s", dir, name);                        \
        f->field = open(p, flags);                                         \
        check(f->field >= 0, "open %s (%s)", p, strerror(errno));         \
    } while (0)
    OPEN_IN(dir, ".", O_RDONLY | O_DIRECTORY);
    OPEN_IN(dir_ce, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    OPEN_IN(prog, "prog", O_RDONLY | O_CLOEXEC);
    OPEN_IN(prog_path, "prog", O_PATH | O_CLOEXEC);
    OPEN_IN(script, "script", O_RDONLY);
    OPEN_IN(script_ce, "script", O_RDONLY | O_CLOEXEC);
    OPEN_IN(misc_ce, "misc", O_RDONLY | O_CLOEXEC);
    OPEN_IN(junk, "junk", O_RDONLY | O_CLOEXEC);
    OPEN_IN(data, "data", O_RDONLY | O_CLOEXEC);
    OPEN_IN(lnk_path, "plnk", O_PATH | O_NOFOLLOW | O_CLOEXEC);
#undef OPEN_IN
}

static void close_fds(struct fds *f) {
    int *all[] = {&f->dir, &f->dir_ce, &f->prog, &f->prog_path, &f->script, &f->script_ce,
                  &f->misc_ce, &f->junk, &f->data, &f->lnk_path};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        if (*all[i] >= 0)
            close(*all[i]);
}

// binfmt_misc: a format for files starting "#elbf", run by /bin/sh, which
// reads the rest as a script.
static const char misc_dir[] = "/proc/sys/fs/binfmt_misc";
static char misc_entry[96];
static bool misc_mounted_here;

static bool misc_register(void) {
    char reg[128];
    snprintf(reg, sizeof(reg), "%s/register", misc_dir);
    if (access(reg, F_OK) != 0) {
        check(mount("binfmt_misc", misc_dir, "binfmt_misc", 0, NULL) == 0,
              "mount binfmt_misc on %s (%s)", misc_dir, strerror(errno));
        misc_mounted_here = access(reg, F_OK) == 0;
        if (!misc_mounted_here)
            return false;
    }
    char name[32], spec[96];
    snprintf(name, sizeof(name), "elbf%d", (int) getpid());
    snprintf(misc_entry, sizeof(misc_entry), "%s/%s", misc_dir, name);
    int len = snprintf(spec, sizeof(spec), ":%s:M::#elbf::/bin/sh:", name);
    int fd = open(reg, O_WRONLY | O_CLOEXEC);
    bool ok = fd >= 0 && write(fd, spec, (size_t) len) == len;
    check(ok, "register %s with binfmt_misc (%s)", spec, strerror(errno));
    if (fd >= 0)
        close(fd);
    if (!ok)
        misc_entry[0] = '\0';
    return ok;
}

static void misc_unregister(void) {
    if (misc_entry[0] != '\0') {
        int fd = open(misc_entry, O_WRONLY | O_CLOEXEC);
        check(fd >= 0 && write(fd, "-1", 2) == 2, "remove %s (%s)", misc_entry, strerror(errno));
        if (fd >= 0)
            close(fd);
        check(access(misc_entry, F_OK) != 0, "%s is gone", misc_entry);
    }
    if (misc_mounted_here)
        umount2(misc_dir, 0);
}

// `exe` is where /proc/self/exe says the program is. The cwd is the
// directory under test. `misc`: the binfmt_misc format is registered.
static void exec_checks(const char *phase, const struct fds *f, const char *exe, bool misc) {
    char how[128], execfn[64];
#define HOW(...) (snprintf(how, sizeof(how), __VA_ARGS__), how)
    snprintf(execfn, sizeof(execfn), "/dev/fd/%d", f->prog);
    expect_runs(HOW("%s: execveat(fd, \"\", AT_EMPTY_PATH)", phase), f->prog, "", AT_EMPTY_PATH,
                execfn, exe);
    expect_runs(HOW("%s: fexecve(fd)", phase), f->prog, "", FEXECVE, execfn, exe);
    snprintf(execfn, sizeof(execfn), "/dev/fd/%d", f->prog_path);
    expect_runs(HOW("%s: execveat(O_PATH fd, \"\", AT_EMPTY_PATH)", phase), f->prog_path, "",
                AT_EMPTY_PATH, execfn, exe);
    snprintf(execfn, sizeof(execfn), "/dev/fd/%d/prog", f->dir);
    expect_runs(HOW("%s: execveat(dirfd, \"prog\", 0)", phase), f->dir, "prog", 0, execfn, exe);
    snprintf(execfn, sizeof(execfn), "/dev/fd/%d/plnk", f->dir);
    expect_runs(HOW("%s: execveat(dirfd, \"plnk\", 0), a symlink to prog", phase), f->dir, "plnk",
                0, execfn, exe);
    expect_runs(HOW("%s: execveat(AT_FDCWD, \"prog\", 0)", phase), AT_FDCWD, "prog", 0, "prog",
                exe);

    expect_err(HOW("%s: execveat(dirfd, \"junk\", 0)", phase), f->dir, "junk", 0, ENOEXEC);
    expect_err(HOW("%s: execveat(junk fd, \"\", AT_EMPTY_PATH)", phase), f->junk, "",
               AT_EMPTY_PATH, ENOEXEC);
    expect_err(HOW("%s: execveat(dirfd, \".\", 0)", phase), f->dir, ".", 0, EACCES);
    expect_err(HOW("%s: execveat(dirfd, \"\", AT_EMPTY_PATH)", phase), f->dir, "", AT_EMPTY_PATH,
               EACCES);
    expect_err(HOW("%s: execveat(dirfd, \"plnk\", AT_SYMLINK_NOFOLLOW)", phase), f->dir, "plnk",
               AT_SYMLINK_NOFOLLOW, ELOOP);
    expect_err(HOW("%s: execveat(O_PATH symlink fd, \"\", AT_EMPTY_PATH)", phase), f->lnk_path,
               "", AT_EMPTY_PATH, ELOOP);
    expect_err(HOW("%s: execveat(dirfd, \"missing\", 0)", phase), f->dir, "missing", 0, ENOENT);

    expect_exit(HOW("%s: execveat(script fd, \"\", AT_EMPTY_PATH)", phase), f->script, "",
                AT_EMPTY_PATH, 7);
    expect_exit(HOW("%s: execveat(dirfd, \"script\", 0)", phase), f->dir, "script", 0, 7);
    expect_err(HOW("%s: execveat(close-on-exec script fd, \"\", AT_EMPTY_PATH)", phase),
               f->script_ce, "", AT_EMPTY_PATH, ENOENT);
    expect_err(HOW("%s: execveat(close-on-exec dirfd, \"script\", 0)", phase), f->dir_ce,
               "script", 0, ENOENT);

    if (misc) {
        expect_exit(HOW("%s: execveat(AT_FDCWD, \"misc\", 0), a binfmt_misc format", phase),
                    AT_FDCWD, "misc", 0, 7);
        expect_exit(HOW("%s: execveat(dirfd, \"misc\", 0), a binfmt_misc format", phase), f->dir,
                    "misc", 0, 7);
        expect_err(HOW("%s: execveat(close-on-exec dirfd, \"misc\", 0)", phase), f->dir_ce,
                   "misc", 0, ENOENT);
        expect_err(HOW("%s: execveat(close-on-exec misc fd, \"\", AT_EMPTY_PATH)", phase),
                   f->misc_ce, "", AT_EMPTY_PATH, ENOENT);
    }
#undef HOW
}

// `elsewhere` is a name on another mount, or NULL when there is none.
static void link_checks(const char *phase, const struct fds *f, const char *elsewhere) {
    char name[64];
    struct stat want, got;
    snprintf(name, sizeof(name), "data-%s", phase);
    check(linkat(f->data, "", f->dir, name, AT_EMPTY_PATH) == 0,
          "%s: linkat(fd, \"\", dirfd, \"%s\", AT_EMPTY_PATH) (%s)", phase, name, strerror(errno));
    check(fstat(f->data, &want) == 0 && fstatat(f->dir, name, &got, 0) == 0 &&
              same_file(&want, &got) && got.st_nlink == 2,
          "%s: %s is the descriptor's file, with two links (%s)", phase, name, strerror(errno));
    unlinkat(f->dir, name, 0);

    snprintf(name, sizeof(name), "lnk-%s", phase);
    check(linkat(f->lnk_path, "", f->dir, name, AT_EMPTY_PATH) == 0,
          "%s: linkat(O_PATH symlink fd, \"\", dirfd, \"%s\", AT_EMPTY_PATH) (%s)", phase, name,
          strerror(errno));
    char text[64];
    ssize_t n = readlinkat(f->dir, name, text, sizeof(text) - 1);
    text[n > 0 ? n : 0] = '\0';
    check(strcmp(text, "prog") == 0, "%s: %s is the symlink itself (reads \"%s\")", phase, name,
          text);
    unlinkat(f->dir, name, 0);

    if (elsewhere != NULL) {
        errno = 0;
        check(linkat(f->data, "", AT_FDCWD, elsewhere, AT_EMPTY_PATH) != 0 && errno == EXDEV,
              "%s: linkat(fd, \"\", AT_FDCWD, %s) on another mount is EXDEV (%s)", phase,
              elsewhere, strerror(errno));
        unlink(elsewhere);
    }

    // A /proc/self/fd reopen is a new open, with the caller's flags.
    char proc[64];
    snprintf(proc, sizeof(proc), "/proc/self/fd/%d", f->data);
    int rw = open(proc, O_RDWR | O_CLOEXEC);
    check(rw >= 0, "%s: open(%s, O_RDWR) of an O_RDONLY descriptor (%s)", phase, proc,
          strerror(errno));
    if (rw >= 0) {
        check(pwrite(rw, "D", 1, 0) == 1, "%s: write through the reopened descriptor (%s)", phase,
              strerror(errno));
        char c = 0;
        ssize_t n = pread(f->data, &c, 1, 0);
        check(n == 1 && c == 'D', "%s: the write is the descriptor's file (read %zd: '%c')", phase,
              n, c != 0 ? c : '?');
        close(rw);
    }
}

// From a chroot, where nothing a dynamic program needs exists: a file that
// is no program is ENOEXEC when it is found, ENOENT when it is not.
static void jail_checks(const char *jail, const struct fds *detached) {
    if (chroot(jail) != 0 || chdir("/") != 0) {
        check(0, "chroot %s (%s)", jail, strerror(errno));
        return;
    }
    expect_err("chroot: execveat(AT_FDCWD, \"/junk\", 0)", AT_FDCWD, "/junk", 0, ENOEXEC);
    expect_err("chroot: execveat(AT_FDCWD, \"junk\", 0)", AT_FDCWD, "junk", 0, ENOEXEC);
    int root = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int junk = open("/junk", O_RDONLY | O_CLOEXEC);
    int data = open("/data", O_RDONLY | O_CLOEXEC);
    check(root >= 0 && junk >= 0 && data >= 0, "chroot: open /, /junk, /data (%s)",
          strerror(errno));
    expect_err("chroot: execveat(dirfd, \"junk\", 0)", root, "junk", 0, ENOEXEC);
    expect_err("chroot: execveat(fd, \"\", AT_EMPTY_PATH)", junk, "", AT_EMPTY_PATH, ENOEXEC);
    check(linkat(data, "", AT_FDCWD, "/data2", AT_EMPTY_PATH) == 0,
          "chroot: linkat(fd, \"\", AT_FDCWD, \"/data2\", AT_EMPTY_PATH) (%s)", strerror(errno));
    struct stat a, b;
    check(fstat(data, &a) == 0 && stat("/data2", &b) == 0 && same_file(&a, &b),
          "chroot: /data2 is the descriptor's file (%s)", strerror(errno));
    unlink("/data2");

    if (detached != NULL) {
        expect_err("chroot: execveat(detached dirfd, \"junk\", 0)", detached->dir, "junk", 0,
                   ENOEXEC);
        expect_err("chroot: execveat(detached fd, \"\", AT_EMPTY_PATH)", detached->junk, "",
                   AT_EMPTY_PATH, ENOEXEC);
        check(linkat(detached->data, "", detached->dir, "data-jail", AT_EMPTY_PATH) == 0,
              "chroot: linkat(detached fd, \"\", detached dirfd, \"data-jail\", AT_EMPTY_PATH) "
              "(%s)",
              strerror(errno));
        check(unlinkat(detached->dir, "data-jail", 0) == 0, "chroot: unlink data-jail (%s)",
              strerror(errno));
    }
}

static char base[] = "/tmp/elbf.XXXXXX";

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--report") == 0)
        return report(atoi(argv[2]));
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));

    if (mkdtemp(base) == NULL) {
        printf("FAIL: mkdtemp (%s)\n", strerror(errno));
        return 1;
    }
    char m[64], jail[64], elsewhere[80], tmp[PATH_MAX];
    snprintf(m, sizeof(m), "%s/m", base);
    snprintf(jail, sizeof(jail), "%s/jail", base);
    snprintf(elsewhere, sizeof(elsewhere), "%s/elsewhere", base);
    check(mkdir(m, 0755) == 0, "mkdir %s (%s)", m, strerror(errno));

    bool mounted = mount("t", m, "tmpfs", 0, NULL) == 0;
    if (!mounted) {
        int err = errno;
        check(err == EPERM, "mount -t tmpfs t %s (%s)", m, strerror(err));
        test_logf("mount: %s, not privileged: a plain directory, no detach, no chroot\n",
                  strerror(err));
    }

    snprintf(tmp, sizeof(tmp), "%s/prog", m);
    copy_self(tmp);
    snprintf(tmp, sizeof(tmp), "%s/script", m);
    put(tmp, "#!/bin/sh\nexit 7\n", 0755);
    snprintf(tmp, sizeof(tmp), "%s/misc", m);
    put(tmp, "#elbf\nexit 7\n", 0755);
    snprintf(tmp, sizeof(tmp), "%s/junk", m);
    put(tmp, "this is no program\n", 0755);
    snprintf(tmp, sizeof(tmp), "%s/data", m);
    put(tmp, "data\n", 0644);
    snprintf(tmp, sizeof(tmp), "%s/plnk", m);
    check(symlink("prog", tmp) == 0, "symlink %s (%s)", tmp, strerror(errno));

    bool misc = mounted && misc_register();
    struct fds f;
    open_fds(m, &f);
    check(chdir(m) == 0, "chdir %s (%s)", m, strerror(errno));

    // The control: the same descriptors while the mount is attached.
    char exe[PATH_MAX];
    snprintf(exe, sizeof(exe), "%s/prog", m);
    exec_checks("attached", &f, exe, misc);
    link_checks("attached", &f, mounted ? elsewhere : NULL);

    if (mounted) {
        check(umount2(m, MNT_DETACH) == 0, "umount -l %s (%s)", m, strerror(errno));
        snprintf(tmp, sizeof(tmp), "%s/prog", m);
        check(access(tmp, F_OK) != 0, "%s is gone from the tree", tmp);
        exec_checks("detached", &f, "/prog", misc);
        link_checks("detached", &f, elsewhere);

        check(mkdir(jail, 0755) == 0, "mkdir %s (%s)", jail, strerror(errno));
        snprintf(tmp, sizeof(tmp), "%s/junk", jail);
        put(tmp, "this is no program\n", 0755);
        snprintf(tmp, sizeof(tmp), "%s/data", jail);
        put(tmp, "data\n", 0644);
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0) {
            failures_total = 0;
            jail_checks(jail, &f);
            fflush(stdout);
            _exit(failures_total == 0 ? 0 : 1);
        }
        int status = 0;
        check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == 0,
              "the chroot child passed (status %#x)", status);
    }

    check(chdir("/") == 0, "chdir / (%s)", strerror(errno));
    close_fds(&f);
    if (misc)
        misc_unregister();
    if (mounted) {
        umount2(m, MNT_DETACH);
        const char *in_jail[] = {"junk", "data", "data2"};
        for (size_t i = 0; i < sizeof(in_jail) / sizeof(in_jail[0]); i++) {
            snprintf(tmp, sizeof(tmp), "%s/%s", jail, in_jail[i]);
            unlink(tmp);
        }
        rmdir(jail);
    } else {
        const char *in_m[] = {"prog", "script", "misc", "junk", "data", "plnk"};
        for (size_t i = 0; i < sizeof(in_m) / sizeof(in_m[0]); i++) {
            snprintf(tmp, sizeof(tmp), "%s/%s", m, in_m[i]);
            unlink(tmp);
        }
    }
    unlink(elsewhere);
    check(rmdir(m) == 0, "rmdir %s (%s)", m, strerror(errno));
    rmdir(base);
    return finish_suite(TEST_NAME);
}
