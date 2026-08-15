// Implementation of the SmallCLUE libc shim. See kernel/native_libc.h for
// why every one of these exists.
//
// This file must NOT be compiled with the shim force-included -- it is the
// thing the shim redirects *to*, and needs the real libc.

#define NATIVE_LIBC_NO_REDIRECT

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/native.h"
#include "kernel/native_io.h"
#include "kernel/native_libc.h"
#include "kernel/task.h"
#include "fs/tty.h"
#include "kernel/uts.h"
#include "util/list.h"
#include "fs/fd.h"
#include "fs/path.h"

// AOK's errno values are negative LINUX errnos, and the numbering is not the
// host's. Flipping the sign and storing that in `errno` is wrong the moment
// the two disagree: SmallCLUE formats its messages with the HOST's strerror,
// so Linux ENOSYS (38) came out as macOS ENOTSOCK -- which is exactly the
// "df: /: Socket operation on non-socket" that surfaced this. ENOENT and a
// handful of others happen to share a number, which is why most errors looked
// fine and only the unusual ones were nonsense.
//
// Anything not listed maps to EINVAL rather than being passed through, so a
// new guest errno produces a wrong-but-plausible message instead of a wildly
// misleading one from an unrelated part of the host's table.
static int nlibc_host_errno(int guest_err) {
    switch (guest_err < 0 ? -guest_err : guest_err) {
        case 1:  return EPERM;
        case 2:  return ENOENT;
        case 3:  return ESRCH;
        case 4:  return EINTR;
        case 5:  return EIO;
        case 6:  return ENXIO;
        case 7:  return E2BIG;
        case 8:  return ENOEXEC;
        case 9:  return EBADF;
        case 10: return ECHILD;
        case 11: return EAGAIN;
        case 12: return ENOMEM;
        case 13: return EACCES;
        case 14: return EFAULT;
        case 16: return EBUSY;
        case 17: return EEXIST;
        case 18: return EXDEV;
        case 19: return ENODEV;
        case 20: return ENOTDIR;
        case 21: return EISDIR;
        case 22: return EINVAL;
        case 23: return ENFILE;
        case 24: return EMFILE;
        case 25: return ENOTTY;
        case 26: return ETXTBSY;
        case 27: return EFBIG;
        case 28: return ENOSPC;
        case 29: return ESPIPE;
        case 30: return EROFS;
        case 31: return EMLINK;
        case 32: return EPIPE;
        case 33: return EDOM;
        case 34: return ERANGE;
        case 36: return ENAMETOOLONG;
        case 38: return ENOSYS;   // Linux 38 is macOS 78; this is the one that bit
        case 39: return ENOTEMPTY;
        case 40: return ELOOP;
        default: return EINVAL;
    }
}

extern char **environ;   // Darwin: needs declaring outside a main program

static int nlibc_tty_ioctl(int fd_no, int cmd, void *arg);

// Same reason as native_have_task: refuse rather than dereference NULL.
#define NLIBC_NEED_TASK() do { if (!native_have_task()) return nlibc_fail(_EFAULT); } while (0)

static int nlibc_fail(int guest_err) {
    errno = nlibc_host_errno(guest_err);
    return -1;
}

// ------------------------------------------------------------ descriptors

int nlibc_open(const char *path, int flags, ...) {
    struct fd *fd = NULL;
    int err = native_open(path, flags, &fd);
    if (err < 0)
        return nlibc_fail(err);
    fd_t installed = f_install(fd, flags);
    if (installed < 0) {
        native_close(fd);
        return nlibc_fail(installed);
    }
    return installed;
}

int nlibc_openat(int dirfd, const char *path, int flags, ...) {
    // Relative-to-a-descriptor opens are not reached by the applets AOK
    // currently builds; resolving against cwd silently would be wrong for a
    // real dirfd, so refuse rather than guess.
    if (dirfd != AT_FDCWD)
        return nlibc_fail(_ENOSYS);
    return nlibc_open(path, flags);
}

int nlibc_close(int fd) {
    int err = f_close(fd);
    return err < 0 ? nlibc_fail(err) : 0;
}

ssize_t nlibc_read(int fd_no, void *buf, size_t n) {
    NLIBC_NEED_TASK();
    // Every read and write is a yield point, which is what makes a native
    // program interruptible at all -- see native_checkpoint in kernel/native.h.
    // A ^C during `top` lands here rather than being ignored forever.
    native_checkpoint();
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return nlibc_fail(_EBADF);
    ssize_t res = native_read(fd, buf, n);
    return res < 0 ? nlibc_fail((int) res) : res;
}

ssize_t nlibc_write(int fd_no, const void *buf, size_t n) {
    native_checkpoint();
    ssize_t res = native_write(fd_no, buf, n);
    return res < 0 ? nlibc_fail((int) res) : res;
}

off_t nlibc_lseek(int fd_no, off_t off, int whence) {
    NLIBC_NEED_TASK();
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return nlibc_fail(_EBADF);
    if (fd->ops->lseek == NULL)
        return nlibc_fail(_ESPIPE);
    off_t res = fd->ops->lseek(fd, off, whence);
    return res < 0 ? nlibc_fail((int) res) : res;
}

int nlibc_access(const char *path, int mode) {
    struct statbuf sb = {};
    int err = native_stat(path, &sb, true);
    if (err < 0)
        return nlibc_fail(err);
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
    return err < 0 ? nlibc_fail(err) : 0;
}

// ------------------------------------------------------------------- stat

static void nlibc_statbuf_to_stat(const struct statbuf *in, struct stat *out) {
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

static int nlibc_stat_common(const char *path, struct stat *st, bool follow) {
    struct statbuf sb = {};
    int err = native_stat(path, &sb, follow);
    if (err < 0)
        return nlibc_fail(err);
    nlibc_statbuf_to_stat(&sb, st);
    return 0;
}

int nlibc_stat(const char *path, struct stat *st) { return nlibc_stat_common(path, st, true); }
int nlibc_lstat(const char *path, struct stat *st) { return nlibc_stat_common(path, st, false); }

int nlibc_fstat(int fd_no, struct stat *st) {
    NLIBC_NEED_TASK();
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return nlibc_fail(_EBADF);
    struct statbuf sb = {};
    int err = fd->mount->fs->fstat(fd, &sb);
    if (err < 0)
        return nlibc_fail(err);
    nlibc_statbuf_to_stat(&sb, st);
    return 0;
}

ssize_t nlibc_readlink(const char *path, char *buf, size_t bufsize) {
    ssize_t res = generic_readlinkat(AT_PWD, path, buf, bufsize);
    return res < 0 ? nlibc_fail((int) res) : res;
}

char *nlibc_realpath(const char *path, char *resolved) {
    // realpath(3)'s contract is that the caller's buffer holds PATH_MAX bytes
    // -- 1024 on Darwin. AOK's MAX_PATH is 4096, and writing that much into a
    // caller's PATH_MAX buffer smashed its stack on EVERY call: df's
    //     char resolved[PATH_MAX]; realpath(query_path, resolved)
    // aborted in __stack_chk_fail before printing a single row. The two
    // constants are not interchangeable and the caller's one governs.
    //
    // Canonicalisation goes through AOK rather than string-joining: opening
    // the path and asking for its path is the same resolution the guest gets,
    // symlinks and mounts included.
    struct fd *fd = NULL;
    int err = native_open(path, O_RDONLY_, &fd);
    if (err < 0) {
        nlibc_fail(err);
        return NULL;
    }
    char canonical[MAX_PATH];
    err = generic_getpath(fd, canonical);
    native_close(fd);
    if (err < 0) {
        nlibc_fail(err);
        return NULL;
    }
    if (canonical[0] == '\0')
        strcpy(canonical, "/");

    size_t need = strlen(canonical) + 1;
    if (resolved == NULL) {
        char *out = malloc(need);
        if (out == NULL) {
            errno = ENOMEM;
            return NULL;
        }
        memcpy(out, canonical, need);
        return out;
    }
    if (need > PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(resolved, canonical, need);
    return resolved;
}

// ------------------------------------------------------------- directories

// DIR is opaque to callers -- they only ever hand it back to readdir/closedir
// -- so a private struct behind the same pointer is safe and avoids trying to
// synthesize a host DIR.
struct nlibc_dir {
    struct fd *fd;
    struct dirent ent;
};

DIR *nlibc_opendir(const char *path) {
    struct fd *fd = NULL;
    int err = native_open(path, O_RDONLY_, &fd);
    if (err < 0) {
        nlibc_fail(err);
        return NULL;
    }
    struct nlibc_dir *dir = calloc(1, sizeof(*dir));
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

struct dirent *nlibc_readdir(DIR *handle) {
    struct nlibc_dir *dir = (struct nlibc_dir *) handle;
    if (dir == NULL) {
        errno = EBADF;
        return NULL;
    }
    struct dir_entry entry = {};
    int res = native_readdir(dir->fd, &entry);
    if (res <= 0) {
        if (res < 0)
            nlibc_fail(res);
        return NULL;   // end of directory, or error
    }
    memset(&dir->ent, 0, sizeof(dir->ent));
    dir->ent.d_ino = (ino_t) entry.inode;
    dir->ent.d_type = entry.type;
    strncpy(dir->ent.d_name, entry.name, sizeof(dir->ent.d_name) - 1);
    dir->ent.d_reclen = sizeof(dir->ent);
    return &dir->ent;
}

int nlibc_closedir(DIR *handle) {
    struct nlibc_dir *dir = (struct nlibc_dir *) handle;
    if (dir == NULL)
        return nlibc_fail(_EBADF);
    if (dir->fd->ops->readdir_end != NULL)
        dir->fd->ops->readdir_end(dir->fd);
    native_close(dir->fd);
    free(dir);
    return 0;
}

// ---------------------------------------------------------------- mutation

int nlibc_unlink(const char *path) {
    int err = generic_unlinkat(AT_PWD, path);
    return err < 0 ? nlibc_fail(err) : 0;
}
int nlibc_rmdir(const char *path) {
    int err = generic_rmdirat(AT_PWD, path);
    return err < 0 ? nlibc_fail(err) : 0;
}
int nlibc_mkdir(const char *path, mode_t mode) {
    int err = generic_mkdirat(AT_PWD, path, mode);
    return err < 0 ? nlibc_fail(err) : 0;
}
int nlibc_rename(const char *from, const char *to) {
    int err = generic_renameat(AT_PWD, from, AT_PWD, to, 0);
    return err < 0 ? nlibc_fail(err) : 0;
}
int nlibc_symlink(const char *target, const char *linkpath) {
    int err = generic_symlinkat(target, AT_PWD, linkpath);
    return err < 0 ? nlibc_fail(err) : 0;
}
int nlibc_link(const char *from, const char *to) {
    int err = generic_linkat(AT_PWD, from, AT_PWD, to);
    return err < 0 ? nlibc_fail(err) : 0;
}

static int nlibc_setattr(const char *path, struct attr attr) {
    int err = generic_setattrat(AT_PWD, path, attr, true);
    return err < 0 ? nlibc_fail(err) : 0;
}
int nlibc_chmod(const char *path, mode_t mode) {
    return nlibc_setattr(path, make_attr(mode, mode));
}
int nlibc_chown(const char *path, uid_t uid, gid_t gid) {
    int err = generic_setattrat(AT_PWD, path, make_attr(uid, uid), true);
    if (err >= 0)
        err = generic_setattrat(AT_PWD, path, make_attr(gid, gid), true);
    return err < 0 ? nlibc_fail(err) : 0;
}
int nlibc_lchown(const char *path, uid_t uid, gid_t gid) {
    int err = generic_setattrat(AT_PWD, path, make_attr(uid, uid), false);
    if (err >= 0)
        err = generic_setattrat(AT_PWD, path, make_attr(gid, gid), false);
    return err < 0 ? nlibc_fail(err) : 0;
}
int nlibc_truncate(const char *path, off_t len) {
    return nlibc_setattr(path, make_attr(size, len));
}

// --------------------------------------------------------------------- cwd

char *nlibc_getcwd(char *buf, size_t size) {
    char path[MAX_PATH];
    int err = native_getcwd(path);
    if (err < 0) {
        nlibc_fail(err);
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

int nlibc_chdir(const char *path) {
    struct fd *fd = NULL;
    int err = native_open(path, O_RDONLY_, &fd);
    if (err < 0)
        return nlibc_fail(err);
    fs_chdir(current->fs, fd);
    return 0;
}

// ------------------------------------------------------------------- stdio

// funopen() gives a real FILE* driven by our callbacks, which is what lets the
// ~1600 fprintf/fputs/fread/fwrite/fclose call sites keep working untouched:
// only where the handle comes from changes.
static int nlibc_file_read(void *cookie, char *buf, int n) {
    ssize_t res = nlibc_read((int) (intptr_t) cookie, buf, (size_t) n);
    return (int) res;
}
static int nlibc_file_write(void *cookie, const char *buf, int n) {
    ssize_t res = nlibc_write((int) (intptr_t) cookie, buf, (size_t) n);
    return (int) res;
}
static fpos_t nlibc_file_seek(void *cookie, fpos_t off, int whence) {
    return nlibc_lseek((int) (intptr_t) cookie, (off_t) off, whence);
}
static int nlibc_file_close(void *cookie) {
    return nlibc_close((int) (intptr_t) cookie);
}
// The standard streams must not close the guest's fd 0/1/2 when the FILE is
// closed or the program exits.
static int nlibc_file_close_noop(void *cookie) { (void) cookie; return 0; }

static FILE *nlibc_file_wrap(int fd, int closes_fd) {
    FILE *f = funopen((void *) (intptr_t) fd, nlibc_file_read, nlibc_file_write,
            nlibc_file_seek, closes_fd ? nlibc_file_close : nlibc_file_close_noop);
    if (f == NULL)
        errno = ENOMEM;
    return f;
}

static int nlibc_mode_to_flags(const char *mode) {
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

FILE *nlibc_fopen(const char *path, const char *mode) {
    int fd = nlibc_open(path, nlibc_mode_to_flags(mode));
    if (fd < 0)
        return NULL;
    FILE *f = nlibc_file_wrap(fd, 1);
    if (f == NULL)
        nlibc_close(fd);
    return f;
}

FILE *nlibc_freopen(const char *path, const char *mode, FILE *stream) {
    if (stream != NULL)
        fclose(stream);
    return nlibc_fopen(path, mode);
}

FILE *nlibc_fdopen(int fd, const char *mode) {
    (void) mode;
    return nlibc_file_wrap(fd, 1);
}

// One wrapper per standard stream, made on demand and kept, so repeated use
// does not leak a FILE each time and buffering behaves consistently.
static FILE *nlibc_std[3];

static FILE *nlibc_std_stream(int fd) {
    if (nlibc_std[fd] == NULL) {
        nlibc_std[fd] = nlibc_file_wrap(fd, 0);
        if (nlibc_std[fd] != NULL) {
            // A funopen stream is fully buffered by default, and a native
            // program exits by returning rather than through exit(), so
            // nothing would ever flush it -- printf output simply vanished.
            // Match what a terminal program expects instead: stderr
            // unbuffered, stdout line-buffered. nlibc_flush_std() below still
            // catches the tail when stdout is a pipe.
            setvbuf(nlibc_std[fd], NULL, fd == 2 ? _IONBF : _IOLBF, BUFSIZ);
        }
    }
    return nlibc_std[fd];
}

// Called once a native program returns, before its task exits: a return from
// main() is not exit(), so the C runtime never flushes these for us.
void nlibc_flush_std(void) {
    for (int i = 0; i < 3; i++)
        if (nlibc_std[i] != NULL)
            fflush(nlibc_std[i]);
}
FILE *nlibc_stdin(void)  { return nlibc_std_stream(0); }
FILE *nlibc_stdout(void) { return nlibc_std_stream(1); }
FILE *nlibc_stderr(void) { return nlibc_std_stream(2); }

int nlibc_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vfprintf(nlibc_stdout(), fmt, args);
    va_end(args);
    return n;
}
int nlibc_puts(const char *s) {
    if (fputs(s, nlibc_stdout()) < 0)
        return EOF;
    return fputc('\n', nlibc_stdout()) == EOF ? EOF : 0;
}
int nlibc_putchar(int c) { return fputc(c, nlibc_stdout()); }
void nlibc_perror(const char *s) {
    if (s != NULL && s[0] != '\0')
        fprintf(nlibc_stderr(), "%s: %s\n", s, strerror(errno));
    else
        fprintf(nlibc_stderr(), "%s\n", strerror(errno));
}

// ----------------------------------------------------------------- process
//
// A native program's child is a real AOK task, not a host process -- there is
// only one of those. native_spawn/native_waitpid (kernel/native_io.h) do the
// work; these give it the shapes a C program expects.
//
// exec-in-place is the one that does not fit: execv replaces the CALLER, and a
// native program cannot be replaced by a guest image in the middle of a C
// function. It is implemented as spawn-then-exit, which is observably the same
// to everything except the caller itself -- the pid changes. nohup and chroot
// use it and do not care; a program that depends on keeping its pid across
// exec would notice.

static int nlibc_exec_common(const char *path, char *const argv[], int search_path) {
    if (path == NULL || argv == NULL)
        return nlibc_fail(_EFAULT);

    char resolved[MAX_PATH];
    if (search_path && strchr(path, '/') == NULL) {
        if (native_path_search(path, resolved, sizeof(resolved)) < 0)
            return nlibc_fail(_ENOENT);
        path = resolved;
    }

    dword_t pid = 0;
    int err = native_spawn(path, argv, environ, &pid);
    if (err < 0)
        return nlibc_fail(err);

    // Stand in for "this process became that program": wait for it and exit
    // with its status, so the caller's caller sees what it expects.
    int status = 0;
    if (native_waitpid(pid, &status, 0) < 0)
        nlibc_exit(127);
    nlibc_exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
}

int nlibc_execv(const char *path, char *const argv[]) {
    return nlibc_exec_common(path, argv, 0);
}
int nlibc_execvp(const char *file, char *const argv[]) {
    return nlibc_exec_common(file, argv, 1);
}
int nlibc_execl(const char *path, const char *arg0, ...) {
    // Collect the varargs into a vector; callers here pass short lists.
    enum { MAX_ARGS = 64 };
    char *argv[MAX_ARGS];
    size_t n = 0;
    va_list ap;
    va_start(ap, arg0);
    argv[n++] = (char *) arg0;
    while (n < MAX_ARGS - 1) {
        char *next = va_arg(ap, char *);
        if (next == NULL)
            break;
        argv[n++] = next;
    }
    va_end(ap);
    argv[n] = NULL;
    return nlibc_exec_common(path, argv, 0);
}

int nlibc_system(const char *command) {
    if (command == NULL)
        return 1;   // a shell is available
    char *argv[] = { (char *) "/bin/sh", (char *) "-c", (char *) command, NULL };
    dword_t pid = 0;
    int err = native_spawn("/bin/sh", argv, environ, &pid);
    if (err < 0)
        return nlibc_fail(err);
    int status = 0;
    if (native_waitpid(pid, &status, 0) < 0)
        return -1;
    return status;
}

pid_t nlibc_waitpid(pid_t pid, int *status, int options) {
    int res = native_waitpid((dword_t) pid, status, options);
    return res < 0 ? nlibc_fail(res) : res;
}
pid_t nlibc_wait(int *status) {
    return nlibc_waitpid(-1, status, 0);
}

// popen needs a pipe between parent and child, which needs the guest's own
// pipe and dup2 on the child side -- neither is wired yet. Refusing keeps it
// honest rather than half-working.
FILE *nlibc_popen(const char *command, const char *mode) {
    (void) command; (void) mode;
    errno = ENOSYS;
    return NULL;
}
int nlibc_pclose(FILE *stream) {
    (void) stream;
    return nlibc_fail(_ENOSYS);
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

int nlibc_dup(int fd_no) {
    NLIBC_NEED_TASK();
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return nlibc_fail(_EBADF);
    fd_t copy = f_install(fd_retain(fd), 0);
    return copy < 0 ? nlibc_fail(copy) : copy;
}

int nlibc_mkstemp(char *template) {
    // mkstemp's contract is create-exclusively and report the name used. The
    // pattern is the caller's buffer, so the substitution has to happen there.
    if (template == NULL) return nlibc_fail(_EINVAL);
    size_t len = strlen(template);
    if (len < 6 || strcmp(template + len - 6, "XXXXXX") != 0)
        return nlibc_fail(_EINVAL);
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int attempt = 0; attempt < 128; attempt++) {
        for (int i = 0; i < 6; i++)
            template[len - 6 + i] = alphabet[(unsigned) rand() % (sizeof(alphabet) - 1)];
        int fd = nlibc_open(template, O_RDWR_ | O_CREAT_ | O_EXCL_, 0600);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            return -1;
    }
    return nlibc_fail(_EEXIST);
}

static int nlibc_utime_common(const char *path) {
    struct timespec now = timespec_now(CLOCK_REALTIME);
    int err = generic_utime(AT_PWD, path, now, now, true);
    return err < 0 ? nlibc_fail(err) : 0;
}
int nlibc_utimes(const char *path, const struct timeval times[2]) {
    (void) times;  // callers here only ever touch a file to "now"
    return nlibc_utime_common(path);
}

// Refusals. Each names the applet that wanted it, so the cost of the gap is
// visible rather than mysterious.
int nlibc_fork(void)                        { return nlibc_fail(_ENOSYS); } // init, runit, micro
int nlibc_chroot(const char *p)             { (void) p; return nlibc_fail(_ENOSYS); } // chroot
int nlibc_kill(pid_t p, int s)              { (void) p; (void) s; return nlibc_fail(_ENOSYS); } // timeout, init
int nlibc_mknod(const char *p, mode_t m, dev_t d) { (void) p; (void) m; (void) d; return nlibc_fail(_ENOSYS); } // mknod
int nlibc_futimes(int fd, const struct timeval t[2]) { (void) fd; (void) t; return nlibc_fail(_ENOSYS); } // touch
int nlibc_statfs(const char *path, void *buf) {
    struct fd *fd = NULL;
    int err = native_open(path, O_RDONLY_, &fd);
    if (err < 0)
        return nlibc_fail(err);
    struct statfsbuf sb = {};
    err = mount_statfs(fd->mount, &sb);
    native_close(fd);
    if (err < 0)
        return nlibc_fail(err);
    struct statfs *out = buf;
    memset(out, 0, sizeof(*out));
    out->f_bsize = (uint32_t) (sb.bsize > 0 ? sb.bsize : 4096);
    out->f_blocks = sb.blocks;
    out->f_bfree = sb.bfree;
    out->f_bavail = sb.bavail;
    out->f_files = sb.files;
    out->f_ffree = sb.ffree;
    return 0;
}
int nlibc_glob(const char *p, int f, void *e, void *g) { (void) p; (void) f; (void) e; (void) g; return nlibc_fail(_ENOSYS); } // shell globbing
int nlibc_dup2(int a, int b)                { (void) a; (void) b; return nlibc_fail(_ENOSYS); } // nohup, micro
int nlibc_fcntl(int fd, int cmd, ...)       { (void) fd; (void) cmd; return nlibc_fail(_ENOSYS); }
// Darwin and Linux number their ioctls differently, so the request has to be
// mapped, not forwarded. Only the terminal ones are handled; anything else
// still refuses rather than being passed through with a number that means
// something unrelated on the other side.
int nlibc_ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    switch (request) {
        case TIOCGWINSZ: {
            // struct winsize and AOK's winsize_ are both four u16 in the same
            // order, so this one is a straight copy after the request maps.
            struct winsize_ w = {};
            if (nlibc_tty_ioctl(fd, TIOCGWINSZ_, &w) < 0)
                return -1;
            struct winsize *out = arg;
            if (out == NULL)
                return nlibc_fail(_EFAULT);
            out->ws_row = w.row;
            out->ws_col = w.col;
            out->ws_xpixel = w.xpixel;
            out->ws_ypixel = w.ypixel;
            return 0;
        }
        default:
            return nlibc_fail(_ENOSYS);
    }
}
int nlibc_poll(void *f, unsigned n, int t)  { (void) f; (void) n; (void) t; return nlibc_fail(_ENOSYS); }
int nlibc_select(int n, void *r, void *w, void *e, void *tv) {
    (void) n; (void) r; (void) w; (void) e; (void) tv; return nlibc_fail(_ENOSYS);
}

// ------------------------------------------------------ host-global refusals
//
// These change state belonging to the whole machine, not to a guest process.
// An applet must never be able to reach the host's clock, hostname, mount
// table or power state -- on the macOS CLI build those calls would land on the
// developer's own system, and `reboot` in particular is not a mistake worth
// making once. iOS marks several of them unavailable outright, which is how
// clock_settime surfaced: the CLI build accepted it and only the iOS SDK
// refused.
//
// A guest-side implementation would go through AOK's own syscall handlers, not
// these. Until then, refusing is the only safe answer.
// A guest cannot move the host's clock, and should not be told it failed
// either -- the guest's own clock is the host's. Succeed and do nothing,
// matching what a VM does when the hypervisor owns the timebase.
int nlibc_clock_settime(int clk, const struct timespec *ts) {
    (void) clk; (void) ts;
    return 0;
}

// This genuinely works: AOK namespaces the hostname per task (kernel/uts.h),
// so setting it changes what the GUEST reports, exactly as it would in a VM,
// without touching the host's.
int nlibc_sethostname(const char *name, size_t len) {
    if (name == NULL)
        return nlibc_fail(_EFAULT);
    if (len >= UTS_NAME_LENGTH)
        return nlibc_fail(_EINVAL);
    struct uts_namespace *ns = current->uts_ns;
    lock(&ns->lock, 0);
    memcpy(ns->hostname, name, len);
    ns->hostname[len] = '\0';
    unlock(&ns->lock);
    return 0;
}

// Not reached today -- SmallCLUE's reboot applet prints and exits without
// calling this -- but wired rather than left to find the host's reboot(2) if
// that ever changes. Rebooting a guest has no meaning here; the app is the
// machine.
int nlibc_reboot(int howto) {
    (void) howto;
    return nlibc_fail(_EPERM);
}

// Routed to AOK's own mount, so `mount` behaves for a native applet exactly as
// it does for the guest. do_mount requires mounts_lock to be HELD (kernel/fs.h
// groups it with the other "must hold mounts_lock while calling these"
// entries) -- this called it without, which is the kind of thing that only
// shows up under contention.
int nlibc_mount(const char *src, const char *tgt, const char *type, unsigned long f, const void *d) {
    (void) d;
    if (src == NULL || tgt == NULL || type == NULL)
        return nlibc_fail(_EFAULT);
    const struct fs_ops *fs = fs_lookup(type);
    if (fs == NULL)
        return nlibc_fail(_ENODEV);
    lock(&mounts_lock, 0);
    int err = do_mount(fs, src, tgt, "", (int) f);
    unlock(&mounts_lock);
    return err < 0 ? nlibc_fail(err) : 0;
}

// sysctl answers about the HOST -- hw.memsize is the Mac's RAM, KERN_BOOTTIME
// the Mac's boot. Failing is the right answer rather than the easy one:
// SmallCLUE already handles the failure, falling back to /proc (which AOK
// serves for the guest) and to a monotonic clock for uptime, so the guest gets
// guest numbers. Failing here also short-circuits the mach_host_self /
// host_statistics64 path in smallclueReadMemStats, which runs only after
// sysctlbyname succeeds and would otherwise report host memory.
int nlibc_sysctl(int *name, unsigned namelen, void *old, size_t *oldlen,
              const void *new, size_t newlen) {
    (void) name; (void) namelen; (void) old; (void) oldlen;
    (void) new; (void) newlen;
    errno = ENOTSUP;
    return -1;
}
int nlibc_sysctlbyname(const char *name, void *old, size_t *oldlen,
                    const void *new, size_t newlen) {
    (void) name; (void) old; (void) oldlen; (void) new; (void) newlen;
    errno = ENOTSUP;
    return -1;
}

// uname reported the HOST: Darwin, the Mac's kernel version, the Mac's
// hostname. A guest must see the guest. do_uname is the same source of truth
// the guest's own uname(2) uses, so a native applet and a translated one now
// agree.
int nlibc_uname(struct utsname *buf) {
    if (buf == NULL)
        return nlibc_fail(_EFAULT);
    struct uname u = {};
    do_uname(&u);
    memset(buf, 0, sizeof(*buf));
    strncpy(buf->sysname, u.system, sizeof(buf->sysname) - 1);
    strncpy(buf->nodename, u.hostname, sizeof(buf->nodename) - 1);
    strncpy(buf->release, u.release, sizeof(buf->release) - 1);
    strncpy(buf->version, u.version, sizeof(buf->version) - 1);
    strncpy(buf->machine, u.arch, sizeof(buf->machine) - 1);
    return 0;
}

// df walks the mount table. On macOS SmallCLUE reaches for getmntinfo, which
// answers about the HOST -- df inside the guest listed the Mac's volumes. The
// traversal itself belongs in fs/mount.c, which owns the locking; doing it
// here held mounts_lock across mount_statfs and hung.
int nlibc_getmntinfo(struct statfs **mntbufp, int flags) {
    (void) flags;
    if (mntbufp == NULL)
        return nlibc_fail(_EFAULT);

    struct mount_info *info = NULL;
    size_t count = 0;
    int err = mount_snapshot(&info, &count);
    if (err < 0)
        return nlibc_fail(err);

    struct statfs *out = calloc(count > 0 ? count : 1, sizeof(*out));
    if (out == NULL) {
        free(info);
        errno = ENOMEM;
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        out[i].f_bsize = (uint32_t) (info[i].statfs.bsize > 0 ? info[i].statfs.bsize : 4096);
        out[i].f_blocks = info[i].statfs.blocks;
        out[i].f_bfree = info[i].statfs.bfree;
        out[i].f_bavail = info[i].statfs.bavail;
        out[i].f_files = info[i].statfs.files;
        out[i].f_ffree = info[i].statfs.ffree;
        strncpy(out[i].f_mntfromname, info[i].source, sizeof(out[i].f_mntfromname) - 1);
        strncpy(out[i].f_mntonname, info[i].point, sizeof(out[i].f_mntonname) - 1);
        strncpy(out[i].f_fstypename, info[i].type, sizeof(out[i].f_fstypename) - 1);
    }
    free(info);

    // Freed on the next call: getmntinfo's contract is that the caller never
    // owns the buffer.
    static struct statfs *previous;
    free(previous);
    previous = out;
    *mntbufp = out;
    return (int) count;
}

// ------------------------------------------------------- interruptible waits
//
// A host sleep parks the thread with nothing watching for signals, which is
// the main reason `top` could not be quit: it spends almost all its time in a
// one-second sleep between refreshes, so ^C had nowhere to land. Sleeping in
// short slices with a checkpoint between them makes the wait interruptible
// without changing what the program observes -- it still sleeps the requested
// time, just in pieces.
#define SC_SLEEP_SLICE_US 50000  // 50ms: responsive to a keypress, negligible overhead

static int nlibc_sleep_us(uint64_t total_us) {
    struct timespec slice;
    while (total_us > 0) {
        native_checkpoint();     // may not return, if the signal is fatal
        uint64_t chunk = total_us < SC_SLEEP_SLICE_US ? total_us : SC_SLEEP_SLICE_US;
        slice.tv_sec = (time_t) (chunk / 1000000u);
        slice.tv_nsec = (long) ((chunk % 1000000u) * 1000u);
        nanosleep(&slice, NULL);
        total_us -= chunk;
    }
    native_checkpoint();
    return 0;
}

unsigned int nlibc_sleep(unsigned int seconds) {
    nlibc_sleep_us((uint64_t) seconds * 1000000u);
    return 0;
}

int nlibc_usleep(unsigned int usec) {
    return nlibc_sleep_us(usec);
}

int nlibc_nanosleep(const struct timespec *req, struct timespec *rem) {
    if (req == NULL) {
        errno = EINVAL;
        return -1;
    }
    nlibc_sleep_us((uint64_t) req->tv_sec * 1000000u + (uint64_t) (req->tv_nsec / 1000));
    if (rem != NULL) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

// ---------------------------------------------------------------- terminal
//
// Why this is not a pass-through: a native program is host C, so it speaks
// Darwin's struct termios, while AOK's tty speaks Linux's (fs/tty.h). The two
// disagree on layout AND on the flag values -- ICANON is 1<<1 on Linux and
// 0x100 on Darwin, and the c_cc indices differ (VMIN is 6 vs 16). Copying the
// struct across would set the wrong bits, which is worse than not implementing
// it: the terminal would end up in some other mode rather than raw.
//
// Only the flags a program plausibly changes are translated. An untranslated
// bit is dropped rather than passed through, so a mistake shows up as "that
// mode did not take" instead of a terminal in an undefined state.
//
// This is what `q` in `top` needed: without raw mode the applet never sees a
// keystroke, which is why it could not be quit.

static int nlibc_tty_ioctl(int fd_no, int cmd, void *arg) {
    NLIBC_NEED_TASK();
    struct fd *fd = f_get(fd_no);
    if (fd == NULL)
        return nlibc_fail(_EBADF);
    if (fd->ops->ioctl == NULL)
        return nlibc_fail(_ENOTTY);
    int err = fd->ops->ioctl(fd, cmd, arg);
    return err < 0 ? nlibc_fail(err) : 0;
}

struct nlibc_flagmap { unsigned long host; dword_t guest; };

// Only the bits worth honouring; see the note above about dropping the rest.
static const struct nlibc_flagmap nlibc_lflags[] = {
    { ISIG,   ISIG_ }, { ICANON, ICANON_ }, { ECHO, ECHO_ }, { ECHOE, ECHOE_ },
};
static const struct nlibc_flagmap nlibc_iflags[] = {
    { ICRNL,  ICRNL_ }, { IXON, IXON_ },
};
static const struct nlibc_flagmap nlibc_oflags[] = {
    { OPOST,  OPOST_ },
};

#define NLIBC_MAP_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static dword_t nlibc_flags_to_guest(unsigned long host, const struct nlibc_flagmap *map, size_t n) {
    dword_t out = 0;
    for (size_t i = 0; i < n; i++)
        if (host & map[i].host)
            out |= map[i].guest;
    return out;
}
static unsigned long nlibc_flags_to_host(dword_t guest, const struct nlibc_flagmap *map, size_t n) {
    unsigned long out = 0;
    for (size_t i = 0; i < n; i++)
        if (guest & map[i].guest)
            out |= map[i].host;
    return out;
}

int nlibc_tcgetattr(int fd_no, struct termios *out) {
    if (out == NULL)
        return nlibc_fail(_EFAULT);
    struct termios_ t = {};
    if (nlibc_tty_ioctl(fd_no, TCGETS_, &t) < 0)
        return -1;
    memset(out, 0, sizeof(*out));
    out->c_lflag = nlibc_flags_to_host(t.lflags, nlibc_lflags, NLIBC_MAP_COUNT(nlibc_lflags));
    out->c_iflag = nlibc_flags_to_host(t.iflags, nlibc_iflags, NLIBC_MAP_COUNT(nlibc_iflags));
    out->c_oflag = nlibc_flags_to_host(t.oflags, nlibc_oflags, NLIBC_MAP_COUNT(nlibc_oflags));
    out->c_cc[VMIN] = t.cc[VMIN_];
    out->c_cc[VTIME] = t.cc[VTIME_];
    out->c_cc[VINTR] = t.cc[VINTR_];
    out->c_cc[VQUIT] = t.cc[VQUIT_];
    out->c_cc[VSUSP] = t.cc[VSUSP_];
    out->c_cc[VEOF] = t.cc[VEOF_];
    return 0;
}

int nlibc_tcsetattr(int fd_no, int action, const struct termios *in) {
    (void) action;  // TCSANOW/DRAIN/FLUSH all map to the same guest ioctl here
    if (in == NULL)
        return nlibc_fail(_EFAULT);
    // Read-modify-write: preserve the bits this translation does not cover
    // rather than zeroing them.
    struct termios_ t = {};
    if (nlibc_tty_ioctl(fd_no, TCGETS_, &t) < 0)
        return -1;
    dword_t lmask = 0, imask = 0, omask = 0;
    for (size_t i = 0; i < NLIBC_MAP_COUNT(nlibc_lflags); i++) lmask |= nlibc_lflags[i].guest;
    for (size_t i = 0; i < NLIBC_MAP_COUNT(nlibc_iflags); i++) imask |= nlibc_iflags[i].guest;
    for (size_t i = 0; i < NLIBC_MAP_COUNT(nlibc_oflags); i++) omask |= nlibc_oflags[i].guest;
    t.lflags = (t.lflags & ~lmask) | nlibc_flags_to_guest(in->c_lflag, nlibc_lflags, NLIBC_MAP_COUNT(nlibc_lflags));
    t.iflags = (t.iflags & ~imask) | nlibc_flags_to_guest(in->c_iflag, nlibc_iflags, NLIBC_MAP_COUNT(nlibc_iflags));
    t.oflags = (t.oflags & ~omask) | nlibc_flags_to_guest(in->c_oflag, nlibc_oflags, NLIBC_MAP_COUNT(nlibc_oflags));
    t.cc[VMIN_] = in->c_cc[VMIN];
    t.cc[VTIME_] = in->c_cc[VTIME];
    return nlibc_tty_ioctl(fd_no, TCSETS_, &t);
}

int nlibc_isatty(int fd_no) {
    struct termios_ t = {};
    if (nlibc_tty_ioctl(fd_no, TCGETS_, &t) < 0) {
        errno = ENOTTY;
        return 0;
    }
    return 1;
}

// --------------------------------------------------- threads a program makes
//
// A native program may create its own pthreads -- SmallCLUE runs its editor on
// one (nextvi_app.c). Those are raw host threads, so AOK's `current` is NULL on
// them, and the first shim call that touches the fd table dereferenced it and
// took the whole app down with EXC_BAD_ACCESS. That is the worst possible
// outcome: not the applet failing, the process dying.
//
// pthread_create is wrapped so a thread inherits the task of whoever created
// it, which is what the program already assumes -- it expects its threads to
// share the process's descriptors and cwd.
//
// The two host threads then share one struct task. That is right for the
// pattern in use here, where the creator blocks in pthread_join and only one
// runs at a time, and it is what exsh will want for its own threads. Genuinely
// concurrent use of the same task from two threads is not made safe by this;
// it would need the task's own locking to be audited for it.

struct nlibc_thread_start {
    void *(*fn)(void *);
    void *arg;
    struct task *task;
};

static void *nlibc_thread_trampoline(void *opaque) {
    struct nlibc_thread_start *start = opaque;
    current = start->task;   // inherit, so the shim has a task to work against
    void *(*fn)(void *) = start->fn;
    void *arg = start->arg;
    free(start);
    return fn(arg);
}

int nlibc_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                         void *(*fn)(void *), void *arg) {
    if (fn == NULL)
        return EINVAL;
    struct nlibc_thread_start *start = malloc(sizeof(*start));
    if (start == NULL)
        return EAGAIN;
    start->fn = fn;
    start->arg = arg;
    start->task = current;
    int err = pthread_create(thread, attr, nlibc_thread_trampoline, start);
    if (err != 0)
        free(start);
    return err;
}

// ------------------------------------------------------------------- exit
//
// The host's exit() ends the PROCESS, and a native program shares the app's
// process -- so a plain exit() in an applet terminated iSH-AOK itself. That is
// how `env` came to take the whole emulator down after its child finished, and
// SmallCLUE calls exit()/_exit() in nine places besides.
//
// A native program is a guest TASK, so ending it means do_exit_group, exactly
// as returning from its main does (kernel/native.c).
noreturn void nlibc_exit(int status) {
    nlibc_flush_std();
    do_exit_group((status & 0xff) << 8);
}
