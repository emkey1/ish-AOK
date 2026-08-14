// Implementation of the SmallCLUE libc shim. See kernel/smallclue_shim.h for
// why every one of these exists.
//
// This file must NOT be compiled with the shim force-included -- it is the
// thing the shim redirects *to*, and needs the real libc.

#define SMALLCLUE_SHIM_NO_REDIRECT

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/native_io.h"
#include "kernel/smallclue_shim.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/path.h"

// AOK's errno values are negative guest errnos; the host libc wants a positive
// host errno in `errno`. The numbering matches Linux's, which is what the
// guest ABI uses, so the translation is just the sign -- but doing it in one
// place keeps every shim function's error path honest.
static int sc_fail(int guest_err) {
    errno = guest_err < 0 ? -guest_err : guest_err;
    return -1;
}

// ------------------------------------------------------------ descriptors

int sc_open(const char *path, int flags, ...) {
    struct fd *fd = NULL;
    int err = native_open(path, flags, &fd);
    if (err < 0)
        return sc_fail(err);
    fd_t installed = f_install(fd, flags);
    if (installed < 0) {
        native_close(fd);
        return sc_fail(installed);
    }
    return installed;
}

int sc_openat(int dirfd, const char *path, int flags, ...) {
    // Relative-to-a-descriptor opens are not reached by the applets AOK
    // currently builds; resolving against cwd silently would be wrong for a
    // real dirfd, so refuse rather than guess.
    if (dirfd != AT_FDCWD)
        return sc_fail(_ENOSYS);
    return sc_open(path, flags);
}

int sc_close(int fd) {
    int err = f_close(fd);
    return err < 0 ? sc_fail(err) : 0;
}

ssize_t sc_read(int fd_no, void *buf, size_t n) {
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return sc_fail(_EBADF);
    ssize_t res = native_read(fd, buf, n);
    return res < 0 ? sc_fail((int) res) : res;
}

ssize_t sc_write(int fd_no, const void *buf, size_t n) {
    ssize_t res = native_write(fd_no, buf, n);
    return res < 0 ? sc_fail((int) res) : res;
}

off_t sc_lseek(int fd_no, off_t off, int whence) {
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return sc_fail(_EBADF);
    if (fd->ops->lseek == NULL)
        return sc_fail(_ESPIPE);
    off_t res = fd->ops->lseek(fd, off, whence);
    return res < 0 ? sc_fail((int) res) : res;
}

int sc_access(const char *path, int mode) {
    struct statbuf sb = {};
    int err = native_stat(path, &sb, true);
    if (err < 0)
        return sc_fail(err);
    // F_OK is existence only, which the stat above already proved. The
    // permission bits are checked against the task's own credentials, the same
    // way the guest's access(2) would.
    if (mode == F_OK)
        return 0;
    int check = 0;
    if (mode & R_OK) check |= AC_R;
    if (mode & W_OK) check |= AC_W;
    if (mode & X_OK) check |= AC_X;
    err = access_check(&sb, check);
    return err < 0 ? sc_fail(err) : 0;
}

// ------------------------------------------------------------------- stat

static void sc_statbuf_to_stat(const struct statbuf *in, struct stat *out) {
    memset(out, 0, sizeof(*out));
    out->st_dev = (dev_t) in->dev;
    out->st_ino = (ino_t) in->inode;
    out->st_mode = (mode_t) in->mode;
    out->st_nlink = (nlink_t) in->nlink;
    out->st_uid = (uid_t) in->uid;
    out->st_gid = (gid_t) in->gid;
    out->st_rdev = (dev_t) in->rdev;
    out->st_size = (off_t) in->size;
    out->st_blksize = (blksize_t) in->blksize;
    out->st_blocks = (blkcnt_t) in->blocks;
    out->st_atime = (time_t) in->atime;
    out->st_mtime = (time_t) in->mtime;
    out->st_ctime = (time_t) in->ctime;
}

static int sc_stat_common(const char *path, struct stat *st, bool follow) {
    struct statbuf sb = {};
    int err = native_stat(path, &sb, follow);
    if (err < 0)
        return sc_fail(err);
    sc_statbuf_to_stat(&sb, st);
    return 0;
}

int sc_stat(const char *path, struct stat *st) { return sc_stat_common(path, st, true); }
int sc_lstat(const char *path, struct stat *st) { return sc_stat_common(path, st, false); }

int sc_fstat(int fd_no, struct stat *st) {
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return sc_fail(_EBADF);
    struct statbuf sb = {};
    int err = fd->mount->fs->fstat(fd, &sb);
    if (err < 0)
        return sc_fail(err);
    sc_statbuf_to_stat(&sb, st);
    return 0;
}

ssize_t sc_readlink(const char *path, char *buf, size_t bufsize) {
    ssize_t res = generic_readlinkat(AT_PWD, path, buf, bufsize);
    return res < 0 ? sc_fail((int) res) : res;
}

char *sc_realpath(const char *path, char *resolved) {
    // Enough for the callers here (readlink -f, watch's exec resolution):
    // confirm it exists, then hand back the path the VFS would use.
    struct statbuf sb = {};
    int err = native_stat(path, &sb, true);
    if (err < 0) {
        sc_fail(err);
        return NULL;
    }
    char *out = resolved != NULL ? resolved : malloc(MAX_PATH);
    if (out == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    if (path[0] == '/') {
        strncpy(out, path, MAX_PATH - 1);
        out[MAX_PATH - 1] = '\0';
    } else {
        char cwd[MAX_PATH];
        if (native_getcwd(cwd) < 0)
            cwd[0] = '\0';
        snprintf(out, MAX_PATH, "%s/%s", cwd, path);
    }
    return out;
}

// ------------------------------------------------------------- directories

// DIR is opaque to callers -- they only ever hand it back to readdir/closedir
// -- so a private struct behind the same pointer is safe and avoids trying to
// synthesize a host DIR.
struct sc_dir {
    struct fd *fd;
    struct dirent ent;
};

DIR *sc_opendir(const char *path) {
    struct fd *fd = NULL;
    int err = native_open(path, O_RDONLY_, &fd);
    if (err < 0) {
        sc_fail(err);
        return NULL;
    }
    struct sc_dir *dir = calloc(1, sizeof(*dir));
    if (dir == NULL) {
        native_close(fd);
        errno = ENOMEM;
        return NULL;
    }
    dir->fd = fd;
    if (fd->ops->readdir_begin != NULL)
        fd->ops->readdir_begin(fd);
    return (DIR *) dir;
}

struct dirent *sc_readdir(DIR *handle) {
    struct sc_dir *dir = (struct sc_dir *) handle;
    if (dir == NULL) {
        errno = EBADF;
        return NULL;
    }
    struct dir_entry entry = {};
    int res = native_readdir(dir->fd, &entry);
    if (res <= 0) {
        if (res < 0)
            sc_fail(res);
        return NULL;   // end of directory, or error
    }
    memset(&dir->ent, 0, sizeof(dir->ent));
    dir->ent.d_ino = (ino_t) entry.inode;
    dir->ent.d_type = entry.type;
    strncpy(dir->ent.d_name, entry.name, sizeof(dir->ent.d_name) - 1);
    dir->ent.d_reclen = sizeof(dir->ent);
    return &dir->ent;
}

int sc_closedir(DIR *handle) {
    struct sc_dir *dir = (struct sc_dir *) handle;
    if (dir == NULL)
        return sc_fail(_EBADF);
    if (dir->fd->ops->readdir_end != NULL)
        dir->fd->ops->readdir_end(dir->fd);
    native_close(dir->fd);
    free(dir);
    return 0;
}

// ---------------------------------------------------------------- mutation

int sc_unlink(const char *path) {
    int err = generic_unlinkat(AT_PWD, path);
    return err < 0 ? sc_fail(err) : 0;
}
int sc_rmdir(const char *path) {
    int err = generic_rmdirat(AT_PWD, path);
    return err < 0 ? sc_fail(err) : 0;
}
int sc_mkdir(const char *path, mode_t mode) {
    int err = generic_mkdirat(AT_PWD, path, mode);
    return err < 0 ? sc_fail(err) : 0;
}
int sc_rename(const char *from, const char *to) {
    int err = generic_renameat(AT_PWD, from, AT_PWD, to, 0);
    return err < 0 ? sc_fail(err) : 0;
}
int sc_symlink(const char *target, const char *linkpath) {
    int err = generic_symlinkat(target, AT_PWD, linkpath);
    return err < 0 ? sc_fail(err) : 0;
}
int sc_link(const char *from, const char *to) {
    int err = generic_linkat(AT_PWD, from, AT_PWD, to);
    return err < 0 ? sc_fail(err) : 0;
}

static int sc_setattr(const char *path, struct attr attr) {
    int err = generic_setattrat(AT_PWD, path, attr, true);
    return err < 0 ? sc_fail(err) : 0;
}
int sc_chmod(const char *path, mode_t mode) {
    return sc_setattr(path, make_attr(mode, mode));
}
int sc_chown(const char *path, uid_t uid, gid_t gid) {
    int err = generic_setattrat(AT_PWD, path, make_attr(uid, uid), true);
    if (err >= 0)
        err = generic_setattrat(AT_PWD, path, make_attr(gid, gid), true);
    return err < 0 ? sc_fail(err) : 0;
}
int sc_lchown(const char *path, uid_t uid, gid_t gid) {
    int err = generic_setattrat(AT_PWD, path, make_attr(uid, uid), false);
    if (err >= 0)
        err = generic_setattrat(AT_PWD, path, make_attr(gid, gid), false);
    return err < 0 ? sc_fail(err) : 0;
}
int sc_truncate(const char *path, off_t len) {
    return sc_setattr(path, make_attr(size, len));
}

// --------------------------------------------------------------------- cwd

char *sc_getcwd(char *buf, size_t size) {
    char path[MAX_PATH];
    int err = native_getcwd(path);
    if (err < 0) {
        sc_fail(err);
        return NULL;
    }
    if (path[0] == '\0')
        strcpy(path, "/");
    if (buf == NULL) {
        char *out = strdup(path);
        if (out == NULL)
            errno = ENOMEM;
        return out;
    }
    if (strlen(path) + 1 > size) {
        errno = ERANGE;
        return NULL;
    }
    strcpy(buf, path);
    return buf;
}

int sc_chdir(const char *path) {
    struct fd *fd = NULL;
    int err = native_open(path, O_RDONLY_, &fd);
    if (err < 0)
        return sc_fail(err);
    fs_chdir(current->fs, fd);
    return 0;
}

// ------------------------------------------------------------------- stdio

// funopen() gives a real FILE* driven by our callbacks, which is what lets the
// ~1600 fprintf/fputs/fread/fwrite/fclose call sites keep working untouched:
// only where the handle comes from changes.
static int sc_file_read(void *cookie, char *buf, int n) {
    ssize_t res = sc_read((int) (intptr_t) cookie, buf, (size_t) n);
    return (int) res;
}
static int sc_file_write(void *cookie, const char *buf, int n) {
    ssize_t res = sc_write((int) (intptr_t) cookie, buf, (size_t) n);
    return (int) res;
}
static fpos_t sc_file_seek(void *cookie, fpos_t off, int whence) {
    return sc_lseek((int) (intptr_t) cookie, (off_t) off, whence);
}
static int sc_file_close(void *cookie) {
    return sc_close((int) (intptr_t) cookie);
}
// The standard streams must not close the guest's fd 0/1/2 when the FILE is
// closed or the program exits.
static int sc_file_close_noop(void *cookie) { (void) cookie; return 0; }

static FILE *sc_file_wrap(int fd, int closes_fd) {
    FILE *f = funopen((void *) (intptr_t) fd, sc_file_read, sc_file_write,
            sc_file_seek, closes_fd ? sc_file_close : sc_file_close_noop);
    if (f == NULL)
        errno = ENOMEM;
    return f;
}

static int sc_mode_to_flags(const char *mode) {
    if (mode == NULL || mode[0] == '\0')
        return O_RDONLY_;
    int flags;
    switch (mode[0]) {
        case 'r': flags = strchr(mode, '+') ? O_RDWR_ : O_RDONLY_; break;
        case 'w': flags = (strchr(mode, '+') ? O_RDWR_ : O_WRONLY_) | O_CREAT_ | O_TRUNC_; break;
        case 'a': flags = (strchr(mode, '+') ? O_RDWR_ : O_WRONLY_) | O_CREAT_ | O_APPEND_; break;
        default:  flags = O_RDONLY_; break;
    }
    return flags;
}

FILE *sc_fopen(const char *path, const char *mode) {
    int fd = sc_open(path, sc_mode_to_flags(mode));
    if (fd < 0)
        return NULL;
    FILE *f = sc_file_wrap(fd, 1);
    if (f == NULL)
        sc_close(fd);
    return f;
}

FILE *sc_freopen(const char *path, const char *mode, FILE *stream) {
    if (stream != NULL)
        fclose(stream);
    return sc_fopen(path, mode);
}

FILE *sc_fdopen(int fd, const char *mode) {
    (void) mode;
    return sc_file_wrap(fd, 1);
}

// One wrapper per standard stream, made on demand and kept, so repeated use
// does not leak a FILE each time and buffering behaves consistently.
static FILE *sc_std[3];

static FILE *sc_std_stream(int fd) {
    if (sc_std[fd] == NULL) {
        sc_std[fd] = sc_file_wrap(fd, 0);
        if (sc_std[fd] != NULL) {
            // A funopen stream is fully buffered by default, and a native
            // program exits by returning rather than through exit(), so
            // nothing would ever flush it -- printf output simply vanished.
            // Match what a terminal program expects instead: stderr
            // unbuffered, stdout line-buffered. sc_flush_std() below still
            // catches the tail when stdout is a pipe.
            setvbuf(sc_std[fd], NULL, fd == 2 ? _IONBF : _IOLBF, BUFSIZ);
        }
    }
    return sc_std[fd];
}

// Called once a native program returns, before its task exits: a return from
// main() is not exit(), so the C runtime never flushes these for us.
void sc_flush_std(void) {
    for (int i = 0; i < 3; i++)
        if (sc_std[i] != NULL)
            fflush(sc_std[i]);
}
FILE *sc_stdin(void)  { return sc_std_stream(0); }
FILE *sc_stdout(void) { return sc_std_stream(1); }
FILE *sc_stderr(void) { return sc_std_stream(2); }

int sc_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vfprintf(sc_stdout(), fmt, args);
    va_end(args);
    return n;
}
int sc_puts(const char *s) {
    if (fputs(s, sc_stdout()) < 0)
        return EOF;
    return fputc('\n', sc_stdout()) == EOF ? EOF : 0;
}
int sc_putchar(int c) { return fputc(c, sc_stdout()); }
void sc_perror(const char *s) {
    if (s != NULL && s[0] != '\0')
        fprintf(sc_stderr(), "%s: %s\n", s, strerror(errno));
    else
        fprintf(sc_stderr(), "%s\n", strerror(errno));
}

// ----------------------------------------------------------------- process
//
// STAGED: these are the guest-task routing described in kernel/native.h, and
// are not implemented yet. They return ENOSYS rather than falling through to
// the host's fork/exec, which on iOS would either fail confusingly or -- worse
// on a host that does have fork -- spawn a real host process outside the
// guest entirely.
//
// smallcluePlatformSpawn is the one SmallCLUE itself calls (its spawn helper
// dispatches here when SMALLCLUE_PLATFORM_SPAWN is defined); the rest are the
// direct callers the shim redirects.

int sc_execv(const char *path, char *const argv[]) {
    (void) path; (void) argv;
    return sc_fail(_ENOSYS);
}
int sc_execvp(const char *file, char *const argv[]) {
    (void) file; (void) argv;
    return sc_fail(_ENOSYS);
}
int sc_system(const char *command) {
    (void) command;
    return sc_fail(_ENOSYS);
}
FILE *sc_popen(const char *command, const char *mode) {
    (void) command; (void) mode;
    errno = ENOSYS;
    return NULL;
}
int sc_pclose(FILE *stream) {
    (void) stream;
    return sc_fail(_ENOSYS);
}
pid_t sc_waitpid(pid_t pid, int *status, int options) {
    (void) pid; (void) status; (void) options;
    return sc_fail(_ENOSYS);
}
pid_t sc_wait(int *status) {
    (void) status;
    return sc_fail(_ENOSYS);
}

// ------------------------------------------------- remaining host-libc holes
//
// Everything below exists so that NOTHING in SmallCLUE can reach the host's
// libc for a filesystem- or process-shaped call. That matters most on the
// macOS CLI build, where these would otherwise succeed against the developer's
// own machine: a host fork(), a host chroot(), a host kill(). Failing with
// ENOSYS is always better than silently acting on the wrong system.
//
// Some are implemented properly; the rest refuse. tools/check-native-libc.py
// is what keeps this list from silently regrowing.

int sc_dup(int fd_no) {
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return sc_fail(_EBADF);
    fd_t copy = f_install(fd_retain(fd), 0);
    return copy < 0 ? sc_fail(copy) : copy;
}

int sc_mkstemp(char *template) {
    // mkstemp's contract is create-exclusively and report the name used. The
    // pattern is the caller's buffer, so the substitution has to happen there.
    if (template == NULL) return sc_fail(_EINVAL);
    size_t len = strlen(template);
    if (len < 6 || strcmp(template + len - 6, "XXXXXX") != 0)
        return sc_fail(_EINVAL);
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int attempt = 0; attempt < 128; attempt++) {
        for (int i = 0; i < 6; i++)
            template[len - 6 + i] = alphabet[(unsigned) rand() % (sizeof(alphabet) - 1)];
        int fd = sc_open(template, O_RDWR_ | O_CREAT_ | O_EXCL_, 0600);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            return -1;
    }
    return sc_fail(_EEXIST);
}

static int sc_utime_common(const char *path) {
    struct timespec now = timespec_now(CLOCK_REALTIME);
    int err = generic_utime(AT_PWD, path, now, now, true);
    return err < 0 ? sc_fail(err) : 0;
}
int sc_utimes(const char *path, const struct timeval times[2]) {
    (void) times;  // callers here only ever touch a file to "now"
    return sc_utime_common(path);
}

// Refusals. Each names the applet that wanted it, so the cost of the gap is
// visible rather than mysterious.
int sc_fork(void)                        { return sc_fail(_ENOSYS); } // init, runit, micro
int sc_execl(const char *p, const char *a0, ...) { (void) p; (void) a0; return sc_fail(_ENOSYS); } // init, runit
int sc_chroot(const char *p)             { (void) p; return sc_fail(_ENOSYS); } // chroot
int sc_kill(pid_t p, int s)              { (void) p; (void) s; return sc_fail(_ENOSYS); } // timeout, init
int sc_mknod(const char *p, mode_t m, dev_t d) { (void) p; (void) m; (void) d; return sc_fail(_ENOSYS); } // mknod
int sc_futimes(int fd, const struct timeval t[2]) { (void) fd; (void) t; return sc_fail(_ENOSYS); } // touch
int sc_statfs(const char *p, void *b)    { (void) p; (void) b; return sc_fail(_ENOSYS); } // df
int sc_glob(const char *p, int f, void *e, void *g) { (void) p; (void) f; (void) e; (void) g; return sc_fail(_ENOSYS); } // shell globbing
int sc_dup2(int a, int b)                { (void) a; (void) b; return sc_fail(_ENOSYS); } // nohup, micro
int sc_fcntl(int fd, int cmd, ...)       { (void) fd; (void) cmd; return sc_fail(_ENOSYS); }
int sc_ioctl(int fd, unsigned long r, ...) { (void) fd; (void) r; return sc_fail(_ENOSYS); } // tty sizing
int sc_poll(void *f, unsigned n, int t)  { (void) f; (void) n; (void) t; return sc_fail(_ENOSYS); }
int sc_select(int n, void *r, void *w, void *e, void *tv) {
    (void) n; (void) r; (void) w; (void) e; (void) tv; return sc_fail(_ENOSYS);
}
