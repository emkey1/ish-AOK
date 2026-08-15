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
#include <spawn.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/native.h"
#include "kernel/native_io.h"
#include "kernel/native_libc.h"
#include "kernel/native_syscall.h"
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

static int nlibc_tty_ioctl(int fd_no, dword_t cmd, void *arg, size_t size, bool out);

// Same reason as native_have_task: refuse rather than dereference NULL.
#define NLIBC_NEED_TASK() do { if (!native_have_task()) return nlibc_fail(_EFAULT); } while (0)

static int nlibc_fail(int guest_err) {
    errno = nlibc_host_errno(guest_err);
    return -1;
}

// The tail nearly every entry point shares: a negative guest errno becomes -1
// with a translated `errno`; anything else is the result. Widened to long so
// the same helper serves lseek's off_t and read's ssize_t.
static long nlibc_ret(sqword_t res) {
    return res < 0 ? nlibc_fail((int) res) : (long) res;
}

// ------------------------------------------------------------ flag translation
//
// Host and guest constants are different numbers for the same ideas, so every
// flag word crossing the shim has to be mapped rather than passed through. An
// unmapped bit is DROPPED, which shows up as "that option did not take"
// instead of the guest acting on some unrelated flag.
struct nlibc_flagmap { unsigned long host; dword_t guest; };

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

// open(2) flags. Only the two access-mode bits agree; everything else moves.
// SmallCLUE's O_CREAT is Darwin's 0x0200, which in the guest's numbering is
// O_NOCTTY|O_NONBLOCK -- so a create quietly became an open-existing before
// this table, and every applet that writes a new file failed with ENOENT
// against a rootfs that did not already have the target.
//
// The guest column is the asm-generic (arm64) numbering, which is what
// kernel/native_syscall.h speaks; kernel/calls.c's openat case maps it onward
// to the fs layer's internal values.
static const struct nlibc_flagmap nlibc_open_flags[] = {
    { O_CREAT,     0x40 },     { O_EXCL,      0x80 },
    { O_NOCTTY,    0x100 },    { O_TRUNC,     0x200 },
    { O_APPEND,    0x400 },    { O_NONBLOCK,  0x800 },
    { O_DIRECTORY, 0x4000 },   { O_NOFOLLOW,  0x8000 },
    { O_CLOEXEC,   0x80000 },  { O_SYNC,      0x101000 },
};

static dword_t nlibc_open_flags_to_guest(int flags) {
    // O_RDONLY/O_WRONLY/O_RDWR are 0/1/2 on both sides.
    return (dword_t) (flags & O_ACCMODE) | nlibc_flags_to_guest((unsigned long) flags,
            nlibc_open_flags, NLIBC_MAP_COUNT(nlibc_open_flags));
}

// Darwin's AT_FDCWD is -2, the guest's is -100. Any other value is a real
// descriptor, and the shim's descriptors ARE the guest's, so it passes through.
static int nlibc_at_fd(int dirfd) {
    return dirfd == AT_FDCWD ? AT_FDCWD_ : dirfd;
}

// Marshals a path and reports the two ways it can fail apart: a NULL path is
// the caller's EFAULT, an empty scratch is ENOMEM.
#define NLIBC_PATH(name, path) \
    guest_addr_t name = native_scratch_str(path); \
    if (name == 0) \
        return nlibc_fail((path) == NULL ? _EFAULT : _ENOMEM)

// Signal numbers. The two agree through SIGABRT and then diverge completely:
// Linux's SIGUSR1 is 10, which on Darwin is SIGBUS; Linux's SIGCHLD is 17,
// which on Darwin is SIGSTOP. A raw number passed through would have `timeout`
// stopping its child where it meant to reap it, and `kill -USR1` hitting
// something else entirely.
struct nlibc_signalmap { int host; int guest; };
static const struct nlibc_signalmap nlibc_signals[] = {
    { SIGHUP, SIGHUP_ },   { SIGINT, SIGINT_ },     { SIGQUIT, SIGQUIT_ },
    { SIGILL, SIGILL_ },   { SIGTRAP, SIGTRAP_ },   { SIGABRT, SIGABRT_ },
    { SIGBUS, SIGBUS_ },   { SIGFPE, SIGFPE_ },     { SIGKILL, SIGKILL_ },
    { SIGUSR1, SIGUSR1_ }, { SIGSEGV, SIGSEGV_ },   { SIGUSR2, SIGUSR2_ },
    { SIGPIPE, SIGPIPE_ }, { SIGALRM, SIGALRM_ },   { SIGTERM, SIGTERM_ },
    { SIGCHLD, SIGCHLD_ }, { SIGCONT, SIGCONT_ },   { SIGSTOP, SIGSTOP_ },
    { SIGTSTP, SIGTSTP_ }, { SIGTTIN, SIGTTIN_ },   { SIGTTOU, SIGTTOU_ },
    { SIGURG, SIGURG_ },   { SIGXCPU, SIGXCPU_ },   { SIGXFSZ, SIGXFSZ_ },
    { SIGVTALRM, SIGVTALRM_ }, { SIGPROF, SIGPROF_ }, { SIGWINCH, SIGWINCH_ },
    { SIGIO, SIGIO_ },     { SIGSYS, SIGSYS_ },
};

// 0 for a signal the other side has no name for -- Darwin's SIGEMT and SIGINFO
// have no Linux number, and Linux's SIGSTKFLT and SIGPWR have no Darwin one.
// The kernel rejects 0 with EINVAL, which is the honest answer.
static int nlibc_signal_to_guest(int host_sig) {
    for (size_t i = 0; i < NLIBC_MAP_COUNT(nlibc_signals); i++)
        if (nlibc_signals[i].host == host_sig)
            return nlibc_signals[i].guest;
    return 0;
}
static int nlibc_signal_to_host(int guest_sig) {
    for (size_t i = 0; i < NLIBC_MAP_COUNT(nlibc_signals); i++)
        if (nlibc_signals[i].guest == guest_sig)
            return nlibc_signals[i].host;
    return 0;
}

// ------------------------------------------------------------ descriptors

int nlibc_openat(int dirfd, const char *path, int flags, ...) {
    NATIVE_FRAME;
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t) va_arg(ap, int);
        va_end(ap);
    }
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_openat, nlibc_at_fd(dirfd),
            guest_path, nlibc_open_flags_to_guest(flags), mode));
}

int nlibc_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t) va_arg(ap, int);
        va_end(ap);
    }
    return nlibc_openat(AT_FDCWD, path, flags, mode);
}

int nlibc_close(int fd_no) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_close, fd_no));
}

ssize_t nlibc_read(int fd_no, void *buf, size_t n) {
    NATIVE_FRAME;
    guest_addr_t guest_buf = native_scratch_alloc(n);
    if (guest_buf == 0 && n > 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_read, fd_no, guest_buf, n);
    if (res > 0 && native_scratch_get(buf, guest_buf, (size_t) res) < 0)
        return nlibc_fail(_EFAULT);
    return nlibc_ret(res);
}

ssize_t nlibc_write(int fd_no, const void *buf, size_t n) {
    NATIVE_FRAME;
    guest_addr_t guest_buf = native_scratch_put(buf, n);
    if (guest_buf == 0 && n > 0)
        return nlibc_fail(_ENOMEM);
    return nlibc_ret(native_syscall(NATIVE_SYS_write, fd_no, guest_buf, n));
}

off_t nlibc_lseek(int fd_no, off_t off, int whence) {
    // SEEK_SET/SEEK_CUR/SEEK_END are 0/1/2 on both sides.
    return (off_t) nlibc_ret(native_syscall(NATIVE_SYS_lseek, fd_no, off, whence));
}

int nlibc_access(const char *path, int mode) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    // F_OK/R_OK/W_OK/X_OK are 0/4/2/1 on both sides.
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_faccessat, AT_FDCWD_,
            guest_path, mode, 0));
}

// ------------------------------------------------------------------- stat

// The syscall layer speaks the asm-generic ABI, so what comes back is the
// arm64 struct stat -- fs/stat.h's own definition of it, rather than a second
// copy here that could drift from the one the kernel writes.
static void nlibc_guest_stat_to_host(const struct arm64_stat_ *in, struct stat *out) {
    memset(out, 0, sizeof(*out));
    out->st_dev = (dev_t) in->dev;
    out->st_ino = (ino_t) in->ino;
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

static int nlibc_statat(int dirfd, const char *path, struct stat *st, dword_t at_flags) {
    NATIVE_FRAME;
    if (st == NULL)
        return nlibc_fail(_EFAULT);
    NLIBC_PATH(guest_path, path);
    guest_addr_t guest_st = native_scratch_alloc(sizeof(struct arm64_stat_));
    if (guest_st == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_newfstatat, nlibc_at_fd(dirfd),
            guest_path, guest_st, at_flags);
    if (res < 0)
        return nlibc_fail((int) res);
    struct arm64_stat_ guest_stat;
    if (native_scratch_get(&guest_stat, guest_st, sizeof(guest_stat)) < 0)
        return nlibc_fail(_EFAULT);
    nlibc_guest_stat_to_host(&guest_stat, st);
    return 0;
}

int nlibc_stat(const char *path, struct stat *st) { return nlibc_statat(AT_FDCWD, path, st, 0); }
int nlibc_lstat(const char *path, struct stat *st) {
    return nlibc_statat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW_);
}

int nlibc_fstat(int fd_no, struct stat *st) {
    NATIVE_FRAME;
    if (st == NULL)
        return nlibc_fail(_EFAULT);
    guest_addr_t guest_st = native_scratch_alloc(sizeof(struct arm64_stat_));
    if (guest_st == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_fstat, fd_no, guest_st);
    if (res < 0)
        return nlibc_fail((int) res);
    struct arm64_stat_ guest_stat;
    if (native_scratch_get(&guest_stat, guest_st, sizeof(guest_stat)) < 0)
        return nlibc_fail(_EFAULT);
    nlibc_guest_stat_to_host(&guest_stat, st);
    return 0;
}

ssize_t nlibc_readlink(const char *path, char *buf, size_t bufsize) {
    NATIVE_FRAME;
    if (buf == NULL)
        return nlibc_fail(_EFAULT);
    NLIBC_PATH(guest_path, path);
    guest_addr_t guest_buf = native_scratch_alloc(bufsize);
    if (guest_buf == 0 && bufsize > 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_readlinkat, AT_FDCWD_, guest_path,
            guest_buf, bufsize);
    if (res > 0 && native_scratch_get(buf, guest_buf, (size_t) res) < 0)
        return nlibc_fail(_EFAULT);
    return nlibc_ret(res);
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

// getdents64's record layout, which is syscall ABI rather than an internal
// detail -- fs/dir.c writes exactly this. Mirrored rather than shared because
// the definition there is private to that file; if the ABI ever moved, both
// would have to move together anyway.
struct nlibc_dirent64 {
    uint64_t inode;
    uint64_t offset;
    uint16_t reclen;
    uint8_t type;
    char name[];
} __attribute__((packed));

// DIR is opaque to callers -- they only ever hand it back to readdir/closedir
// -- so a private struct behind the same pointer is safe and avoids trying to
// synthesize a host DIR. The buffer is what makes this one syscall per batch
// of entries rather than one per entry.
struct nlibc_dir {
    int fd;
    size_t filled;
    size_t pos;
    struct dirent ent;
    char buf[8192];
};

DIR *nlibc_opendir(const char *path) {
    int fd = nlibc_open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return NULL;
    struct nlibc_dir *dir = calloc(1, sizeof(*dir));
    if (dir == NULL) {
        nlibc_close(fd);
        errno = ENOMEM;
        return NULL;
    }
    dir->fd = fd;
    return (DIR *) dir;
}

struct dirent *nlibc_readdir(DIR *handle) {
    struct nlibc_dir *dir = (struct nlibc_dir *) handle;
    if (dir == NULL) {
        errno = EBADF;
        return NULL;
    }
    if (dir->pos >= dir->filled) {
        NATIVE_FRAME;
        guest_addr_t guest_buf = native_scratch_alloc(sizeof(dir->buf));
        if (guest_buf == 0) {
            errno = ENOMEM;
            return NULL;
        }
        sqword_t res = native_syscall(NATIVE_SYS_getdents64, dir->fd, guest_buf,
                sizeof(dir->buf));
        if (res <= 0) {
            if (res < 0)
                nlibc_fail((int) res);
            return NULL;   // end of directory, or error
        }
        if (native_scratch_get(dir->buf, guest_buf, (size_t) res) < 0) {
            nlibc_fail(_EFAULT);
            return NULL;
        }
        dir->filled = (size_t) res;
        dir->pos = 0;
    }

    const struct nlibc_dirent64 *entry = (const void *) (dir->buf + dir->pos);
    // A record short enough to overlap its own header, or one claiming to run
    // past what was read, would walk this loop off the buffer.
    if (entry->reclen < sizeof(*entry) + 1 ||
            dir->pos + entry->reclen > dir->filled) {
        nlibc_fail(_EIO);
        return NULL;
    }
    dir->pos += entry->reclen;

    memset(&dir->ent, 0, sizeof(dir->ent));
    dir->ent.d_ino = (ino_t) entry->inode;
    dir->ent.d_type = entry->type;
    size_t name_max = entry->reclen - sizeof(*entry);
    if (name_max > sizeof(dir->ent.d_name) - 1)
        name_max = sizeof(dir->ent.d_name) - 1;
    memcpy(dir->ent.d_name, entry->name, name_max);
    dir->ent.d_name[name_max] = '\0';
    dir->ent.d_reclen = sizeof(dir->ent);
    return &dir->ent;
}

int nlibc_closedir(DIR *handle) {
    struct nlibc_dir *dir = (struct nlibc_dir *) handle;
    if (dir == NULL)
        return nlibc_fail(_EBADF);
    int fd = dir->fd;
    free(dir);
    return nlibc_close(fd);
}

// ---------------------------------------------------------------- mutation

// AT_REMOVEDIR, which fs/dir.c's unlinkat reads. Named here because kernel/fs.c
// keeps its own private copy rather than exporting one.
#define NLIBC_AT_REMOVEDIR 0x200

int nlibc_unlink(const char *path) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_unlinkat, AT_FDCWD_, guest_path, 0));
}
int nlibc_rmdir(const char *path) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_unlinkat, AT_FDCWD_, guest_path,
            NLIBC_AT_REMOVEDIR));
}
int nlibc_mkdir(const char *path, mode_t mode) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_mkdirat, AT_FDCWD_, guest_path, mode));
}
int nlibc_rename(const char *from, const char *to) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_from, from);
    NLIBC_PATH(guest_to, to);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_renameat, AT_FDCWD_, guest_from,
            AT_FDCWD_, guest_to));
}
int nlibc_symlink(const char *target, const char *linkpath) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_target, target);
    NLIBC_PATH(guest_link, linkpath);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_symlinkat, guest_target,
            AT_FDCWD_, guest_link));
}
int nlibc_link(const char *from, const char *to) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_from, from);
    NLIBC_PATH(guest_to, to);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_linkat, AT_FDCWD_, guest_from,
            AT_FDCWD_, guest_to));
}

int nlibc_chmod(const char *path, mode_t mode) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_fchmodat, AT_FDCWD_, guest_path, mode));
}
static int nlibc_chown_common(const char *path, uid_t uid, gid_t gid, dword_t at_flags) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_fchownat, AT_FDCWD_, guest_path,
            uid, gid, at_flags));
}
int nlibc_chown(const char *path, uid_t uid, gid_t gid) {
    return nlibc_chown_common(path, uid, gid, 0);
}
int nlibc_lchown(const char *path, uid_t uid, gid_t gid) {
    return nlibc_chown_common(path, uid, gid, AT_SYMLINK_NOFOLLOW_);
}
int nlibc_truncate(const char *path, off_t len) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_truncate, guest_path, len));
}

// --------------------------------------------------------------------- cwd

char *nlibc_getcwd(char *buf, size_t size) {
    NATIVE_FRAME;
    char path[MAX_PATH];
    guest_addr_t guest_path = native_scratch_alloc(sizeof(path));
    if (guest_path == 0) {
        errno = ENOMEM;
        return NULL;
    }
    sqword_t res = native_syscall(NATIVE_SYS_getcwd, guest_path, sizeof(path));
    if (res < 0 || native_scratch_get(path, guest_path, sizeof(path)) < 0) {
        nlibc_fail(res < 0 ? (int) res : _EFAULT);
        return NULL;
    }
    path[sizeof(path) - 1] = '\0';
    if (path[0] == '\0')
        strcpy(path, "/");
    if (buf == NULL) {
        char *out = strdup(path);
        if (out == NULL)
            errno = ENOMEM;
        return out;
    }
    // The caller's buffer governs, and it is usually Darwin's PATH_MAX (1024)
    // against AOK's MAX_PATH (4096) -- see the note in nlibc_realpath, where
    // confusing the two smashed df's stack on every call.
    if (strlen(path) + 1 > size) {
        errno = ERANGE;
        return NULL;
    }
    strcpy(buf, path);
    return buf;
}

int nlibc_chdir(const char *path) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_chdir, guest_path));
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
        return O_RDONLY;
    int flags;
    switch (mode[0]) {
        case 'r': flags = strchr(mode, '+') ? O_RDWR : O_RDONLY; break;
        case 'w': flags = (strchr(mode, '+') ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
        case 'a': flags = (strchr(mode, '+') ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
        default:  flags = O_RDONLY; break;
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
    int err = native_spawn(path, argv, native_env_vector(), &pid);
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
    int err = native_spawn("/bin/sh", argv, native_env_vector(), &pid);
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

// popen is a guest pipe, a child task with one end wired to its stdio, and a
// FILE* over the other end -- the same funopen wrapper every stream the shim
// hands out is built from, so the caller's fgets/fwrite work unchanged.
//
// pclose has to find the child again from the FILE*, and there is no portable
// way to recover a funopen cookie, so the pairs are kept in a list. It is
// file-scope, which two native programs running at once would share; that is
// safe because the key is a FILE* and no two tasks can produce the same one,
// but it is the same sharing noted for nlibc_std and the passwd cache.
struct nlibc_popen {
    struct nlibc_popen *next;
    FILE *file;
    dword_t pid;
};
static struct nlibc_popen *nlibc_popens;
static pthread_mutex_t nlibc_popen_lock = PTHREAD_MUTEX_INITIALIZER;

static bool nlibc_popen_take(FILE *file, dword_t *pid_out) {
    pthread_mutex_lock(&nlibc_popen_lock);
    for (struct nlibc_popen **link = &nlibc_popens; *link != NULL; link = &(*link)->next) {
        if ((*link)->file != file)
            continue;
        struct nlibc_popen *found = *link;
        *link = found->next;
        pthread_mutex_unlock(&nlibc_popen_lock);
        *pid_out = found->pid;
        free(found);
        return true;
    }
    pthread_mutex_unlock(&nlibc_popen_lock);
    return false;
}

FILE *nlibc_popen(const char *command, const char *mode) {
    if (command == NULL || mode == NULL || (mode[0] != 'r' && mode[0] != 'w')) {
        errno = EINVAL;
        return NULL;
    }
    bool reading = mode[0] == 'r';

    int fds[2];
    if (nlibc_pipe(fds) < 0)
        return NULL;
    // Reading from the child means the child writes: it gets fds[1], and it
    // gets it as its stdout. Writing to the child is the mirror image.
    int child_fd = reading ? fds[1] : fds[0];
    int parent_fd = reading ? fds[0] : fds[1];
    int child_slot = reading ? 1 : 0;

    // In order, as a forked child would have done it: put our end of the pipe
    // where the child expects it, then drop both originals. The child must hold
    // neither our end -- or nobody ever sees EOF, because a writer still exists
    // -- nor a second copy of its own under the number pipe() gave it, which
    // would otherwise sit on top of its stderr.
    struct native_spawn_action actions[3];
    size_t n = 0;
    actions[n++] = (struct native_spawn_action) {
        .kind = NATIVE_SPAWN_DUP2, .fd = child_slot, .from = child_fd };
    actions[n++] = (struct native_spawn_action) {
        .kind = NATIVE_SPAWN_CLOSE, .fd = parent_fd };
    if (child_fd != child_slot)
        actions[n++] = (struct native_spawn_action) {
            .kind = NATIVE_SPAWN_CLOSE, .fd = child_fd };

    struct native_spawn_opts opts = {
        .pgid = NATIVE_SPAWN_PGID_INHERIT,
        .actions = actions,
        .action_count = n,
    };

    struct nlibc_popen *entry = malloc(sizeof(*entry));
    char *argv[] = { (char *) "/bin/sh", (char *) "-c", (char *) command, NULL };
    dword_t pid = 0;
    int err = entry != NULL
        ? native_spawn_opts("/bin/sh", argv, native_env_vector(), &opts, &pid)
        : _ENOMEM;
    // Ours only for handing to the child; the child has its own copy now.
    nlibc_close(child_fd);
    if (err < 0) {
        free(entry);
        nlibc_close(parent_fd);
        nlibc_fail(err);
        return NULL;
    }

    FILE *file = nlibc_file_wrap(parent_fd, 1);
    if (file == NULL) {
        free(entry);
        nlibc_close(parent_fd);
        return NULL;
    }
    entry->file = file;
    entry->pid = pid;
    pthread_mutex_lock(&nlibc_popen_lock);
    entry->next = nlibc_popens;
    nlibc_popens = entry;
    pthread_mutex_unlock(&nlibc_popen_lock);
    return file;
}

// Returns the child's wait status, as system() does -- not the exit code.
int nlibc_pclose(FILE *stream) {
    dword_t pid = 0;
    if (stream == NULL || !nlibc_popen_take(stream, &pid)) {
        // Not a stream popen handed out. POSIX makes this undefined; refusing
        // is better than closing something and waiting for an unrelated child.
        errno = EINVAL;
        return -1;
    }
    // Closing the parent's end first is what lets a child blocked writing to a
    // full pipe finish, so the wait below cannot deadlock against it.
    fclose(stream);
    int status = 0;
    if (native_waitpid(pid, &status, 0) < 0)
        return -1;
    return status;
}

// ------------------------------------------------- remaining host-libc holes
//
// Everything below exists so that NOTHING in SmallCLUE can reach the host's
// libc for a filesystem- or process-shaped call. That matters most on the
// macOS CLI build, where these would otherwise succeed against the developer's
// own machine: a host fork(), a host chroot(), a host kill(). Failing with
// ENOSYS is always better than silently acting on the wrong system.
//
// Most of these were refusals when the shim reimplemented calls by hand -- a
// dup2 or a poll was more machinery than the applet needing it was worth. Over
// the syscall dispatcher they are a few lines each, which is the point.

int nlibc_dup(int fd_no) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_dup, fd_no));
}

// A pipe made of HOST descriptors would be unusable: every other descriptor a
// native program holds is a guest fd, so the two numbering spaces would collide
// and a close() on one end would shut something unrelated. pipe2 with no flags
// is plain pipe -- Linux has no bare pipe(2) in the asm-generic table.
int nlibc_pipe(int fds[2]) {
    NATIVE_FRAME;
    if (fds == NULL)
        return nlibc_fail(_EFAULT);
    guest_addr_t guest_fds = native_scratch_alloc(2 * sizeof(dword_t));
    if (guest_fds == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_pipe2, guest_fds, 0);
    if (res < 0)
        return nlibc_fail((int) res);
    dword_t out[2] = {0, 0};
    if (native_scratch_get(out, guest_fds, sizeof(out)) < 0)
        return nlibc_fail(_EFAULT);
    fds[0] = (int) out[0];
    fds[1] = (int) out[1];
    return 0;
}

int nlibc_dup2(int oldfd, int newfd) {
    // dup2(fd, fd) is a no-op returning fd, where dup3 rejects it with EINVAL.
    // The check that the descriptor is open still has to happen.
    if (oldfd == newfd) {
        if (native_syscall(NATIVE_SYS_fcntl, oldfd, F_GETFD, 0) < 0)
            return nlibc_fail(_EBADF);
        return newfd;
    }
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_dup3, oldfd, newfd, 0));
}

// fcntl commands 0-4 (DUPFD/GETFD/SETFD/GETFL/SETFL) happen to agree between
// Darwin and Linux; everything above them does not (Darwin's F_GETLK is 7,
// Linux's is 5), and the lock commands would need struct flock translated as
// well. Refusing the rest is better than acting on the wrong command.
int nlibc_fcntl(int fd_no, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);

    switch (cmd) {
        case F_DUPFD:
        case F_GETFD:
        case F_SETFD:
            return (int) nlibc_ret(native_syscall(NATIVE_SYS_fcntl, fd_no, cmd, arg));
        case F_DUPFD_CLOEXEC:
            return (int) nlibc_ret(native_syscall(NATIVE_SYS_fcntl, fd_no, 1030, arg));
        case F_GETFL: {
            sqword_t res = native_syscall(NATIVE_SYS_fcntl, fd_no, F_GETFL, 0);
            if (res < 0)
                return nlibc_fail((int) res);
            // Comes back in the guest's numbering, and the caller will test it
            // against Darwin's.
            return (int) (((dword_t) res & 3) | (int) nlibc_flags_to_host((dword_t) res,
                    nlibc_open_flags, NLIBC_MAP_COUNT(nlibc_open_flags)));
        }
        case F_SETFL:
            return (int) nlibc_ret(native_syscall(NATIVE_SYS_fcntl, fd_no, F_SETFL,
                    nlibc_open_flags_to_guest((int) arg)));
        default:
            return nlibc_fail(_ENOSYS);
    }
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
        int fd = nlibc_open(template, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            return -1;
    }
    return nlibc_fail(_EEXIST);
}

// utimensat's UTIME_NOW/UTIME_OMIT, which the guest reads out of tv_nsec.
#define NLIBC_UTIME_NOW  ((1l << 30) - 1l)
#define NLIBC_UTIME_OMIT ((1l << 30) - 2l)

// The guest's struct timespec is two 64-bit words, which is also Darwin's on
// arm64 -- but writing the pair explicitly keeps this correct if the shim is
// ever built somewhere they differ.
struct nlibc_guest_timespec { sqword_t sec; sqword_t nsec; };

static int nlibc_utimens(int dirfd, const char *path, int fd_no,
        const struct timeval times[2]) {
    NATIVE_FRAME;
    struct nlibc_guest_timespec ts[2];
    for (int i = 0; i < 2; i++) {
        if (times == NULL) {
            ts[i].sec = 0;
            ts[i].nsec = NLIBC_UTIME_NOW;
        } else {
            ts[i].sec = times[i].tv_sec;
            ts[i].nsec = (sqword_t) times[i].tv_usec * 1000;
        }
    }
    guest_addr_t guest_ts = native_scratch_put(ts, sizeof(ts));
    if (guest_ts == 0)
        return nlibc_fail(_ENOMEM);
    if (path == NULL) {
        // futimes: utimensat with a NULL path acts on the descriptor itself.
        return (int) nlibc_ret(native_syscall(NATIVE_SYS_utimensat, fd_no, 0, guest_ts, 0));
    }
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_utimensat, nlibc_at_fd(dirfd),
            guest_path, guest_ts, 0));
}

int nlibc_utimes(const char *path, const struct timeval times[2]) {
    return nlibc_utimens(AT_FDCWD, path, -1, times);
}
int nlibc_futimes(int fd_no, const struct timeval times[2]) {
    return nlibc_utimens(AT_FDCWD, NULL, fd_no, times);
}

int nlibc_chroot(const char *path) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_chroot, guest_path));
}

int nlibc_kill(pid_t pid, int sig) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_kill, pid,
            nlibc_signal_to_guest(sig)));
}

int nlibc_mknod(const char *path, mode_t mode, dev_t dev) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    // Darwin packs a dev_t as major:minor = 24:8, the guest as 8:8 in the low
    // 16 bits (fs/dev.h). Repacking rather than passing the raw number through.
    dword_t guest_dev = (dword_t) (((major(dev) & 0xff) << 8) | (minor(dev) & 0xff));
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_mknodat, AT_FDCWD_, guest_path,
            mode, guest_dev));
}

// The guest writes the asm-generic statfs64 (fs/stat.h's amd64_statfs_, shared
// by every 64-bit ABI here); Darwin's struct statfs is a different shape with
// different field widths, so the fields are copied across by name.
static void nlibc_guest_statfs_to_host(const struct amd64_statfs_ *in, struct statfs *out) {
    memset(out, 0, sizeof(*out));
    out->f_bsize = (uint32_t) (in->bsize > 0 ? in->bsize : 4096);
    out->f_blocks = in->blocks;
    out->f_bfree = in->bfree;
    out->f_bavail = in->bavail;
    out->f_files = in->files;
    out->f_ffree = in->ffree;
}

int nlibc_statfs(const char *path, void *buf) {
    NATIVE_FRAME;
    if (buf == NULL)
        return nlibc_fail(_EFAULT);
    NLIBC_PATH(guest_path, path);
    guest_addr_t guest_buf = native_scratch_alloc(sizeof(struct amd64_statfs_));
    if (guest_buf == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_statfs, guest_path, guest_buf);
    if (res < 0)
        return nlibc_fail((int) res);
    struct amd64_statfs_ guest_statfs;
    if (native_scratch_get(&guest_statfs, guest_buf, sizeof(guest_statfs)) < 0)
        return nlibc_fail(_EFAULT);
    nlibc_guest_statfs_to_host(&guest_statfs, buf);
    return 0;
}

// fork() has no guest syscall that would help: the caller is host code running
// on a task's thread, and a forked task would resume in the middle of a C
// function with no image to execute. nlibc_execv's spawn-then-exit is the
// shape that does work (see the process section above).
int nlibc_fork(void)                        { return nlibc_fail(_ENOSYS); } // init, runit, micro
int nlibc_glob(const char *p, int f, void *e, void *g) { (void) p; (void) f; (void) e; (void) g; return nlibc_fail(_ENOSYS); } // shell globbing

// Darwin and Linux number their ioctls differently, so the request has to be
// mapped, not forwarded. Only the terminal ones are handled; anything else
// still refuses rather than being passed through with a number that means
// something unrelated on the other side.
int nlibc_ioctl(int fd_no, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    switch (request) {
        case TIOCGWINSZ: {
            // struct winsize and AOK's winsize_ are both four u16 in the same
            // order, so this one is a straight copy after the request maps.
            struct winsize_ w = {};
            if (nlibc_tty_ioctl(fd_no, TIOCGWINSZ_, &w, sizeof(w), true) < 0)
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
        case TIOCSWINSZ: {
            const struct winsize *in = arg;
            if (in == NULL)
                return nlibc_fail(_EFAULT);
            struct winsize_ w = {
                .row = in->ws_row, .col = in->ws_col,
                .xpixel = in->ws_xpixel, .ypixel = in->ws_ypixel,
            };
            return nlibc_tty_ioctl(fd_no, TIOCSWINSZ_, &w, sizeof(w), false);
        }
        // Job control, for the callers that spell these as ioctls rather than
        // through tcgetpgrp/tcsetpgrp. Both are a single dword either way.
        case TIOCGPGRP: {
            pid_t got = nlibc_tcgetpgrp(fd_no);
            if (got < 0)
                return -1;
            if (arg == NULL)
                return nlibc_fail(_EFAULT);
            *(pid_t *) arg = got;
            return 0;
        }
        case TIOCSPGRP:
            if (arg == NULL)
                return nlibc_fail(_EFAULT);
            return nlibc_tcsetpgrp(fd_no, *(pid_t *) arg);
        // Claim this terminal as the session's controlling one. AOK implements
        // it (tiocsctty, fs/tty.c); the argument is the "steal it from another
        // session" flag and travels by value, not by pointer.
        case TIOCSCTTY:
            return (int) nlibc_ret(native_syscall(NATIVE_SYS_ioctl, fd_no,
                    TIOCSCTTY_, (uintptr_t) arg));
        default:
            return nlibc_fail(_ENOSYS);
    }
}

// struct pollfd is {int, short, short} on both sides and the POLL* bits share
// their values, so this one really is a straight copy.
int nlibc_poll(void *fds, unsigned nfds, int timeout) {
    NATIVE_FRAME;
    size_t size = (size_t) nfds * sizeof(struct pollfd);
    guest_addr_t guest_fds = native_scratch_put(fds, size);
    if (guest_fds == 0 && size > 0)
        return nlibc_fail(_ENOMEM);

    // ppoll takes a timespec rather than poll's milliseconds; a negative
    // timeout is "wait forever", which is a NULL pointer here.
    guest_addr_t guest_ts = 0;
    if (timeout >= 0) {
        struct nlibc_guest_timespec ts = {
            .sec = timeout / 1000,
            .nsec = (sqword_t) (timeout % 1000) * 1000000,
        };
        guest_ts = native_scratch_put(&ts, sizeof(ts));
        if (guest_ts == 0)
            return nlibc_fail(_ENOMEM);
    }
    sqword_t res = native_syscall(NATIVE_SYS_ppoll, guest_fds, nfds, guest_ts, 0, 0);
    if (res >= 0 && size > 0 && native_scratch_get(fds, guest_fds, size) < 0)
        return nlibc_fail(_EFAULT);
    return (int) nlibc_ret(res);
}

// An fd_set is a bit array in both libcs, and on a little-endian machine the
// byte-level layout is the same despite Darwin's 32-bit words against Linux's
// 64-bit ones. Only the number of bytes the guest reads has to be worked out
// from nfds, since the two disagree on FD_SETSIZE.
int nlibc_select(int nfds, void *readfds, void *writefds, void *errorfds, void *timeout) {
    NATIVE_FRAME;
    if (nfds < 0)
        return nlibc_fail(_EINVAL);
    size_t size = ((size_t) nfds + 7) / 8;
    size = (size + 7) & ~(size_t) 7;   // the guest reads whole 64-bit words

    guest_addr_t guest_sets[3] = {0, 0, 0};
    void *host_sets[3] = { readfds, writefds, errorfds };
    for (int i = 0; i < 3; i++) {
        if (host_sets[i] == NULL)
            continue;
        guest_sets[i] = native_scratch_put(host_sets[i], size);
        if (guest_sets[i] == 0)
            return nlibc_fail(_ENOMEM);
    }

    guest_addr_t guest_ts = 0;
    if (timeout != NULL) {
        const struct timeval *tv = timeout;
        struct nlibc_guest_timespec ts = {
            .sec = tv->tv_sec,
            .nsec = (sqword_t) tv->tv_usec * 1000,
        };
        guest_ts = native_scratch_put(&ts, sizeof(ts));
        if (guest_ts == 0)
            return nlibc_fail(_ENOMEM);
    }

    sqword_t res = native_syscall(NATIVE_SYS_pselect6, nfds, guest_sets[0],
            guest_sets[1], guest_sets[2], guest_ts, 0);
    if (res < 0)
        return nlibc_fail((int) res);
    for (int i = 0; i < 3; i++)
        if (guest_sets[i] != 0 && native_scratch_get(host_sets[i], guest_sets[i], size) < 0)
            return nlibc_fail(_EFAULT);
    return (int) res;
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
    NATIVE_FRAME;
    if (name == NULL)
        return nlibc_fail(_EFAULT);
    guest_addr_t guest_name = native_scratch_put(name, len);
    if (guest_name == 0)
        return nlibc_fail(_ENOMEM);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_sethostname, guest_name, len));
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
    NATIVE_FRAME;
    if (buf == NULL)
        return nlibc_fail(_EFAULT);
    guest_addr_t guest_uts = native_scratch_alloc(sizeof(struct uname));
    if (guest_uts == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_uname, guest_uts);
    if (res < 0)
        return nlibc_fail((int) res);
    struct uname u;
    if (native_scratch_get(&u, guest_uts, sizeof(u)) < 0)
        return nlibc_fail(_EFAULT);
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

// `out` says which way the argument travels: true when the kernel fills it in
// (TCGETS, TIOCGWINSZ), false when the caller supplies it (TCSETS).
static int nlibc_tty_ioctl(int fd_no, dword_t cmd, void *arg, size_t size, bool out) {
    NATIVE_FRAME;
    guest_addr_t guest_arg = out ? native_scratch_alloc(size)
                                 : native_scratch_put(arg, size);
    if (guest_arg == 0)
        return nlibc_fail(arg == NULL ? _EFAULT : _ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_ioctl, fd_no, cmd, guest_arg);
    if (res < 0)
        return nlibc_fail((int) res);
    if (out && native_scratch_get(arg, guest_arg, size) < 0)
        return nlibc_fail(_EFAULT);
    return (int) res;
}

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

int nlibc_tcgetattr(int fd_no, struct termios *out) {
    if (out == NULL)
        return nlibc_fail(_EFAULT);
    struct termios_ t = {};
    if (nlibc_tty_ioctl(fd_no, TCGETS_, &t, sizeof(t), true) < 0)
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
    if (nlibc_tty_ioctl(fd_no, TCGETS_, &t, sizeof(t), true) < 0)
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
    return nlibc_tty_ioctl(fd_no, TCSETS_, &t, sizeof(t), false);
}

int nlibc_isatty(int fd_no) {
    struct termios_ t = {};
    if (nlibc_tty_ioctl(fd_no, TCGETS_, &t, sizeof(t), true) < 0) {
        errno = ENOTTY;
        return 0;
    }
    return 1;
}

// Queue selectors: Darwin numbers them 1/2/3, the guest 0/1/2 (fs/tty.h).
static int nlibc_tcflush_queue(int host_queue) {
    switch (host_queue) {
        case TCIFLUSH:  return TCIFLUSH_;
        case TCOFLUSH:  return TCOFLUSH_;
        case TCIOFLUSH: return TCIOFLUSH_;
        default:        return -1;
    }
}

int nlibc_tcflush(int fd_no, int queue) {
    int guest_queue = nlibc_tcflush_queue(queue);
    if (guest_queue < 0)
        return nlibc_fail(_EINVAL);
    // TCFLSH takes its argument by value rather than by pointer -- AOK's
    // tty_ioctl_size reports 0 bytes for it -- so there is nothing to marshal.
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_ioctl, fd_no, TCFLSH_, guest_queue));
}

// tcdrain waits for queued output to be written. AOK's tty has no output
// queue to drain: fs/tty.c hands a write to the driver before returning, so
// by the time the caller reaches here the data is already gone. Succeeding is
// the accurate answer rather than a convenient one -- and it has no TCSBRK to
// forward to in any case.
int nlibc_tcdrain(int fd_no) {
    (void) fd_no;
    return 0;
}

// ------------------------------------------------------------------ the pty
//
// AOK has the whole mechanism already -- /dev/ptmx, devpts, TIOCGPTN and
// TIOCSPTLCK (fs/pty.c) -- it was just unreachable from host code. This is
// what a native shell needs before it can run a job under a terminal of its
// own, which is exactly the piece exsh is waiting on.

int nlibc_posix_openpt(int flags) {
    return nlibc_open("/dev/ptmx", flags);
}

// Linux's grantpt has nothing to do: devpts creates the slave with the right
// owner and mode when the master is opened. glibc's is a no-op too.
int nlibc_grantpt(int fd_no) {
    (void) fd_no;
    return 0;
}

int nlibc_unlockpt(int fd_no) {
    dword_t unlock = 0;
    return nlibc_tty_ioctl(fd_no, TIOCSPTLCK_, &unlock, sizeof(unlock), false);
}

char *nlibc_ptsname(int fd_no) {
    // ptsname's contract is a pointer into storage the caller does not own,
    // valid until the next call.
    static char name[32];
    dword_t number = 0;
    if (nlibc_tty_ioctl(fd_no, TIOCGPTN_, &number, sizeof(number), true) < 0)
        return NULL;
    snprintf(name, sizeof(name), "/dev/pts/%u", number);
    return name;
}

// The guest's own answer, through /proc/self/fd -- AOK's procfs makes each
// descriptor a symlink to what it points at (fs/proc/pid.c), which is how
// ttyname is implemented on Linux too.
char *nlibc_ttyname(int fd_no) {
    static char name[MAX_PATH];
    if (!nlibc_isatty(fd_no)) {
        errno = ENOTTY;
        return NULL;
    }
    char link[64];
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd_no);
    ssize_t len = nlibc_readlink(link, name, sizeof(name) - 1);
    if (len < 0)
        return NULL;
    name[len] = '\0';
    return name;
}

// ------------------------------------------------------------- odds and ends

// tmpfile() creates its file in the HOST's /tmp and hands back a host FILE*.
// The guest has its own /tmp, and this is the only sensible place for it.
FILE *nlibc_tmpfile(void) {
    char path[64];
    for (int attempt = 0; attempt < 128; attempt++) {
        snprintf(path, sizeof(path), "/tmp/nlibc-%d-%d", (int) nlibc_getpid(), rand());
        int fd = nlibc_open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST)
                continue;
            return NULL;
        }
        // Unlinked immediately, as tmpfile's contract requires: the file lives
        // exactly as long as the descriptor.
        nlibc_unlink(path);
        FILE *f = nlibc_fdopen(fd, "w+");
        if (f == NULL)
            nlibc_close(fd);
        return f;
    }
    errno = EEXIST;
    return NULL;
}

// glob() refuses, so a glob_t never holds anything of ours to release. Freeing
// the host's idea of one would be reading a structure nothing ever filled in.
void nlibc_globfree(void *pglob) {
    (void) pglob;
}

// sysinfo's struct is the other one whose width follows the TASK's ABI
// (kernel/uname.c), so it is read back in whichever shape that guest uses.
// Only the two fields sysconf needs are lifted out.
static int nlibc_guest_sysinfo(uint64_t *totalram, unsigned *procs) {
    NATIVE_FRAME;
    bool wide = guest_abi_is_64bit(current->abi);
    size_t size = wide ? sizeof(struct amd64_sys_info) : sizeof(struct sys_info);
    guest_addr_t guest_info = native_scratch_alloc(size);
    if (guest_info == 0)
        return _ENOMEM;
    sqword_t res = native_syscall(NATIVE_SYS_sysinfo, guest_info);
    if (res < 0)
        return (int) res;
    if (wide) {
        struct amd64_sys_info info;
        if (native_scratch_get(&info, guest_info, sizeof(info)) < 0)
            return _EFAULT;
        *totalram = info.totalram * (info.mem_unit > 0 ? info.mem_unit : 1);
        *procs = info.procs;
    } else {
        struct sys_info info;
        if (native_scratch_get(&info, guest_info, sizeof(info)) < 0)
            return _EFAULT;
        *totalram = (uint64_t) info.totalram * (info.mem_unit > 0 ? info.mem_unit : 1);
        *procs = info.procs;
    }
    return 0;
}

// sysconf answers about the HOST: its page size, its CPU count, its clock
// tick. Only the values a program actually asks for are answered, from the
// guest; anything else fails rather than reporting the Mac's.
long nlibc_sysconf(int name) {
    switch (name) {
        case _SC_PAGESIZE:
            return 4096;   // the guest's page size (emu/mmu.h), not the host's
        case _SC_CLK_TCK:
            return 100;    // what AOK's /proc/<pid>/stat counts in
        case _SC_OPEN_MAX:
            return 1024;
        case _SC_NPROCESSORS_CONF:
        case _SC_NPROCESSORS_ONLN: {
            unsigned procs = 0;
            uint64_t ram = 0;
            if (nlibc_guest_sysinfo(&ram, &procs) == 0 && procs > 0)
                return (long) procs;
            return 1;
        }
        case _SC_PHYS_PAGES: {
            unsigned procs = 0;
            uint64_t ram = 0;
            if (nlibc_guest_sysinfo(&ram, &procs) < 0) {
                errno = EINVAL;
                return -1;
            }
            return (long) (ram / 4096);
        }
        default:
            errno = EINVAL;
            return -1;
    }
}

// The guest's rusage layout follows the TASK's ABI -- kernel/resource.c's
// write_guest_rusage_abi picks between the 64-bit and 32-bit shapes -- so
// unlike everything else here, this reads back in whichever shape the guest
// image that exec'd would have used.
int nlibc_getrusage(int who, void *usage) {
    NATIVE_FRAME;
    if (usage == NULL)
        return nlibc_fail(_EFAULT);
    // RUSAGE_SELF is 0 and RUSAGE_CHILDREN is -1 on both sides.
    size_t size = guest_abi_is_64bit(current->abi) ?
            sizeof(struct amd64_rusage_) : sizeof(struct rusage_);
    guest_addr_t guest_usage = native_scratch_alloc(size);
    if (guest_usage == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_getrusage, who, guest_usage);
    if (res < 0)
        return nlibc_fail((int) res);

    struct rusage *out = usage;
    memset(out, 0, sizeof(*out));
    if (guest_abi_is_64bit(current->abi)) {
        struct amd64_rusage_ guest;
        if (native_scratch_get(&guest, guest_usage, sizeof(guest)) < 0)
            return nlibc_fail(_EFAULT);
        out->ru_utime.tv_sec = guest.utime.sec;
        out->ru_utime.tv_usec = guest.utime.usec;
        out->ru_stime.tv_sec = guest.stime.sec;
        out->ru_stime.tv_usec = guest.stime.usec;
        out->ru_maxrss = guest.maxrss;
        out->ru_minflt = guest.minflt;
        out->ru_majflt = guest.majflt;
        out->ru_nvcsw = guest.nvcsw;
        out->ru_nivcsw = guest.nivcsw;
    } else {
        struct rusage_ guest;
        if (native_scratch_get(&guest, guest_usage, sizeof(guest)) < 0)
            return nlibc_fail(_EFAULT);
        out->ru_utime.tv_sec = guest.utime.sec;
        out->ru_utime.tv_usec = guest.utime.usec;
        out->ru_stime.tv_sec = guest.stime.sec;
        out->ru_stime.tv_usec = guest.stime.usec;
        out->ru_maxrss = guest.maxrss;
        out->ru_minflt = guest.minflt;
        out->ru_majflt = guest.majflt;
        out->ru_nvcsw = guest.nvcsw;
        out->ru_nivcsw = guest.nivcsw;
    }
    return 0;
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

// ---------------------------------------------------------------- networking
//
// AOK's sockets are real host sockets underneath (fs/sock.c), so what routing
// through the dispatcher buys is not the traffic -- it is everything around
// it. A socket becomes a GUEST descriptor, which is what makes it work with
// the same close, poll, select, dup and fd table as every other descriptor a
// native program holds; before this a socket() came back as a HOST fd that
// collided with guest fd numbers, so closing one closed something else.
//
// Nothing here can be passed through unchanged. Darwin's AF_INET6 is 30 and
// the guest's is 10; Darwin's SOL_SOCKET is 0xffff and the guest's is 1; every
// SO_ and MSG_ constant is renumbered; and a sockaddr carries a length byte on
// Darwin where the guest has a 16-bit family. Getting any of those wrong is
// silent -- a bind to the wrong family, an option set on the wrong level -- so
// each is a table with the guest values written out.

// Guest (Linux) constants. Spelled as literals because these are the ABI the
// syscall layer speaks, not values this build's headers would agree with.
#define NLIBC_AF_UNIX_   1
#define NLIBC_AF_INET_   2
#define NLIBC_AF_INET6_  10
#define NLIBC_SOL_SOCKET_ 1

static const struct nlibc_signalmap nlibc_families[] = {
    { AF_UNIX, NLIBC_AF_UNIX_ }, { AF_INET, NLIBC_AF_INET_ },
    { AF_INET6, NLIBC_AF_INET6_ },
};

static int nlibc_family_to_guest(int host_family) {
    for (size_t i = 0; i < NLIBC_MAP_COUNT(nlibc_families); i++)
        if (nlibc_families[i].host == host_family)
            return nlibc_families[i].guest;
    return -1;
}
static int nlibc_family_to_host(int guest_family) {
    for (size_t i = 0; i < NLIBC_MAP_COUNT(nlibc_families); i++)
        if (nlibc_families[i].guest == guest_family)
            return nlibc_families[i].host;
    return -1;
}

// SOCK_STREAM/DGRAM/RAW/SEQPACKET are 1/2/3/5 on both sides; only the two
// flags ORed into the type differ.
static const struct nlibc_flagmap nlibc_sock_type_flags[] = {
    // Darwin has no SOCK_NONBLOCK/SOCK_CLOEXEC of its own -- they are Linux
    // extensions -- so the host side uses the values Linux gave them, which
    // is what a program written against Linux would have passed anyway.
    { 0x800, 0x800 },     // SOCK_NONBLOCK
    { 0x80000, 0x80000 }, // SOCK_CLOEXEC
};

static const struct nlibc_flagmap nlibc_msg_flags[] = {
    { MSG_OOB, 0x1 }, { MSG_PEEK, 0x2 }, { MSG_DONTROUTE, 0x4 },
    { MSG_CTRUNC, 0x8 }, { MSG_TRUNC, 0x20 }, { MSG_DONTWAIT, 0x40 },
    { MSG_EOR, 0x80 }, { MSG_WAITALL, 0x100 },
};

// SOL_SOCKET options. Everything else (IPPROTO_IP/TCP/IPV6 levels and their
// options) happens to share numbers between the two, so only this level needs
// a table.
static const struct nlibc_signalmap nlibc_sock_opts[] = {
    { SO_DEBUG, 1 },      { SO_REUSEADDR, 2 },  { SO_TYPE, 3 },
    { SO_ERROR, 4 },      { SO_DONTROUTE, 5 },  { SO_BROADCAST, 6 },
    { SO_SNDBUF, 7 },     { SO_RCVBUF, 8 },     { SO_KEEPALIVE, 9 },
    { SO_OOBINLINE, 10 }, { SO_LINGER, 13 },    { SO_REUSEPORT, 15 },
    { SO_RCVLOWAT, 18 },  { SO_SNDLOWAT, 19 },  { SO_RCVTIMEO, 20 },
    { SO_SNDTIMEO, 21 },
};

// The guest's sockaddrs. Same sizes as Darwin's, different heads: Darwin
// spends its first byte on sa_len, the guest gives the family a full 16 bits.
struct nlibc_sockaddr_in { uint16_t family; uint16_t port; uint32_t addr; uint8_t zero[8]; };
struct nlibc_sockaddr_in6 { uint16_t family; uint16_t port; uint32_t flowinfo;
                            uint8_t addr[16]; uint32_t scope_id; };
struct nlibc_sockaddr_un { uint16_t family; char path[108]; };

// Host sockaddr -> guest. Returns the guest length, or a negative errno.
static ssize_t nlibc_sockaddr_to_guest(const void *host, socklen_t host_len,
        void *out, size_t out_size) {
    const struct sockaddr *sa = host;
    if (host == NULL || host_len < (socklen_t) sizeof(sa->sa_family))
        return _EINVAL;
    switch (sa->sa_family) {
        case AF_INET: {
            const struct sockaddr_in *in = host;
            if (out_size < sizeof(struct nlibc_sockaddr_in))
                return _EINVAL;
            struct nlibc_sockaddr_in *g = out;
            memset(g, 0, sizeof(*g));
            g->family = NLIBC_AF_INET_;
            g->port = in->sin_port;             // already network order
            g->addr = in->sin_addr.s_addr;
            return sizeof(*g);
        }
        case AF_INET6: {
            const struct sockaddr_in6 *in6 = host;
            if (out_size < sizeof(struct nlibc_sockaddr_in6))
                return _EINVAL;
            struct nlibc_sockaddr_in6 *g = out;
            memset(g, 0, sizeof(*g));
            g->family = NLIBC_AF_INET6_;
            g->port = in6->sin6_port;
            g->flowinfo = in6->sin6_flowinfo;
            memcpy(g->addr, &in6->sin6_addr, sizeof(g->addr));
            g->scope_id = in6->sin6_scope_id;
            return sizeof(*g);
        }
        case AF_UNIX: {
            const struct sockaddr_un *un = host;
            if (out_size < sizeof(struct nlibc_sockaddr_un))
                return _EINVAL;
            struct nlibc_sockaddr_un *g = out;
            memset(g, 0, sizeof(*g));
            g->family = NLIBC_AF_UNIX_;
            // Darwin's sun_path is 104 bytes against the guest's 108, so the
            // copy is bounded by the smaller of the two rather than either.
            size_t path_max = sizeof(un->sun_path) < sizeof(g->path) ?
                    sizeof(un->sun_path) : sizeof(g->path) - 1;
            memcpy(g->path, un->sun_path, path_max);
            return (ssize_t) (offsetof(struct nlibc_sockaddr_un, path) +
                    strnlen(g->path, sizeof(g->path) - 1) + 1);
        }
        default:
            return _EAFNOSUPPORT;
    }
}

// Guest sockaddr -> host. Returns the host length, or a negative errno.
static ssize_t nlibc_sockaddr_to_host(const void *guest, size_t guest_len,
        void *out, size_t out_size) {
    uint16_t family;
    if (guest_len < sizeof(family))
        return _EINVAL;
    memcpy(&family, guest, sizeof(family));
    switch (family) {
        case NLIBC_AF_INET_: {
            if (guest_len < sizeof(struct nlibc_sockaddr_in) ||
                    out_size < sizeof(struct sockaddr_in))
                return _EINVAL;
            const struct nlibc_sockaddr_in *g = guest;
            struct sockaddr_in *in = out;
            memset(in, 0, sizeof(*in));
            in->sin_len = sizeof(*in);
            in->sin_family = AF_INET;
            in->sin_port = g->port;
            in->sin_addr.s_addr = g->addr;
            return sizeof(*in);
        }
        case NLIBC_AF_INET6_: {
            if (guest_len < sizeof(struct nlibc_sockaddr_in6) ||
                    out_size < sizeof(struct sockaddr_in6))
                return _EINVAL;
            const struct nlibc_sockaddr_in6 *g = guest;
            struct sockaddr_in6 *in6 = out;
            memset(in6, 0, sizeof(*in6));
            in6->sin6_len = sizeof(*in6);
            in6->sin6_family = AF_INET6;
            in6->sin6_port = g->port;
            in6->sin6_flowinfo = g->flowinfo;
            memcpy(&in6->sin6_addr, g->addr, sizeof(g->addr));
            in6->sin6_scope_id = g->scope_id;
            return sizeof(*in6);
        }
        case NLIBC_AF_UNIX_: {
            if (out_size < sizeof(struct sockaddr_un))
                return _EINVAL;
            const struct nlibc_sockaddr_un *g = guest;
            struct sockaddr_un *un = out;
            memset(un, 0, sizeof(*un));
            un->sun_len = sizeof(*un);
            un->sun_family = AF_UNIX;
            size_t avail = guest_len > offsetof(struct nlibc_sockaddr_un, path) ?
                    guest_len - offsetof(struct nlibc_sockaddr_un, path) : 0;
            if (avail > sizeof(un->sun_path) - 1)
                avail = sizeof(un->sun_path) - 1;
            memcpy(un->sun_path, g->path, avail);
            return sizeof(*un);
        }
        default:
            return _EAFNOSUPPORT;
    }
}

int nlibc_socket(int domain, int type, int protocol) {
    int guest_domain = nlibc_family_to_guest(domain);
    if (guest_domain < 0)
        return nlibc_fail(_EAFNOSUPPORT);
    // The type's low bits are the socket kind, which both sides number alike.
    int guest_type = (type & 0xf) | (int) nlibc_flags_to_guest((unsigned long) type,
            nlibc_sock_type_flags, NLIBC_MAP_COUNT(nlibc_sock_type_flags));
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_socket, guest_domain,
            guest_type, protocol));
}

// bind and connect differ only in which syscall they issue.
static int nlibc_bind_or_connect(unsigned num, int fd_no, const void *addr, socklen_t len) {
    NATIVE_FRAME;
    char guest_addr[sizeof(struct nlibc_sockaddr_un)];
    ssize_t guest_len = nlibc_sockaddr_to_guest(addr, len, guest_addr, sizeof(guest_addr));
    if (guest_len < 0)
        return nlibc_fail((int) guest_len);
    guest_addr_t scratch = native_scratch_put(guest_addr, (size_t) guest_len);
    if (scratch == 0)
        return nlibc_fail(_ENOMEM);
    return (int) nlibc_ret(native_syscall(num, fd_no, scratch, guest_len));
}

int nlibc_bind(int fd_no, const void *addr, socklen_t len) {
    return nlibc_bind_or_connect(NATIVE_SYS_bind, fd_no, addr, len);
}
int nlibc_connect(int fd_no, const void *addr, socklen_t len) {
    return nlibc_bind_or_connect(NATIVE_SYS_connect, fd_no, addr, len);
}
int nlibc_listen(int fd_no, int backlog) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_listen, fd_no, backlog));
}

// accept/getsockname/getpeername all hand back an address the same way.
static int nlibc_accept_common(unsigned num, int fd_no, void *addr, socklen_t *len) {
    NATIVE_FRAME;
    guest_addr_t guest_sa = 0, guest_salen = 0;
    if (addr != NULL && len != NULL) {
        guest_sa = native_scratch_alloc(sizeof(struct nlibc_sockaddr_un));
        dword_t size = sizeof(struct nlibc_sockaddr_un);
        guest_salen = native_scratch_put(&size, sizeof(size));
        if (guest_sa == 0 || guest_salen == 0)
            return nlibc_fail(_ENOMEM);
    }
    sqword_t res = native_syscall(num, fd_no, guest_sa, guest_salen);
    if (res < 0)
        return nlibc_fail((int) res);
    if (guest_sa != 0) {
        dword_t got = 0;
        char raw[sizeof(struct nlibc_sockaddr_un)];
        if (native_scratch_get(&got, guest_salen, sizeof(got)) < 0 ||
                native_scratch_get(raw, guest_sa, sizeof(raw)) < 0)
            return nlibc_fail(_EFAULT);
        char host_addr[sizeof(struct sockaddr_un)];
        ssize_t host_len = nlibc_sockaddr_to_host(raw, got, host_addr, sizeof(host_addr));
        if (host_len < 0)
            return nlibc_fail((int) host_len);
        if ((socklen_t) host_len > *len)
            host_len = *len;
        memcpy(addr, host_addr, (size_t) host_len);
        *len = (socklen_t) host_len;
    }
    return (int) res;
}

int nlibc_accept(int fd_no, void *addr, socklen_t *len) {
    return nlibc_accept_common(NATIVE_SYS_accept, fd_no, addr, len);
}
int nlibc_getsockname(int fd_no, void *addr, socklen_t *len) {
    return nlibc_accept_common(NATIVE_SYS_getsockname, fd_no, addr, len);
}
int nlibc_getpeername(int fd_no, void *addr, socklen_t *len) {
    return nlibc_accept_common(NATIVE_SYS_getpeername, fd_no, addr, len);
}

static dword_t nlibc_msg_to_guest(int flags) {
    return nlibc_flags_to_guest((unsigned long) flags, nlibc_msg_flags,
            NLIBC_MAP_COUNT(nlibc_msg_flags));
}

ssize_t nlibc_sendto(int fd_no, const void *buf, size_t len, int flags,
        const void *addr, socklen_t addrlen) {
    NATIVE_FRAME;
    guest_addr_t guest_buf = native_scratch_put(buf, len);
    if (guest_buf == 0 && len > 0)
        return nlibc_fail(_ENOMEM);
    guest_addr_t guest_sa = 0;
    ssize_t guest_salen = 0;
    if (addr != NULL) {
        char raw[sizeof(struct nlibc_sockaddr_un)];
        guest_salen = nlibc_sockaddr_to_guest(addr, addrlen, raw, sizeof(raw));
        if (guest_salen < 0)
            return nlibc_fail((int) guest_salen);
        guest_sa = native_scratch_put(raw, (size_t) guest_salen);
        if (guest_sa == 0)
            return nlibc_fail(_ENOMEM);
    }
    return nlibc_ret(native_syscall(NATIVE_SYS_sendto, fd_no, guest_buf, len,
            nlibc_msg_to_guest(flags), guest_sa, guest_salen));
}

ssize_t nlibc_send(int fd_no, const void *buf, size_t len, int flags) {
    return nlibc_sendto(fd_no, buf, len, flags, NULL, 0);
}

ssize_t nlibc_recvfrom(int fd_no, void *buf, size_t len, int flags,
        void *addr, socklen_t *addrlen) {
    NATIVE_FRAME;
    guest_addr_t guest_buf = native_scratch_alloc(len);
    if (guest_buf == 0 && len > 0)
        return nlibc_fail(_ENOMEM);
    guest_addr_t guest_sa = 0, guest_salen = 0;
    if (addr != NULL && addrlen != NULL) {
        guest_sa = native_scratch_alloc(sizeof(struct nlibc_sockaddr_un));
        dword_t size = sizeof(struct nlibc_sockaddr_un);
        guest_salen = native_scratch_put(&size, sizeof(size));
        if (guest_sa == 0 || guest_salen == 0)
            return nlibc_fail(_ENOMEM);
    }
    sqword_t res = native_syscall(NATIVE_SYS_recvfrom, fd_no, guest_buf, len,
            nlibc_msg_to_guest(flags), guest_sa, guest_salen);
    if (res < 0)
        return nlibc_fail((int) res);
    if (res > 0 && native_scratch_get(buf, guest_buf, (size_t) res) < 0)
        return nlibc_fail(_EFAULT);
    if (guest_sa != 0) {
        dword_t got = 0;
        char raw[sizeof(struct nlibc_sockaddr_un)];
        if (native_scratch_get(&got, guest_salen, sizeof(got)) < 0 ||
                native_scratch_get(raw, guest_sa, sizeof(raw)) < 0)
            return nlibc_fail(_EFAULT);
        char host_addr[sizeof(struct sockaddr_un)];
        ssize_t host_len = nlibc_sockaddr_to_host(raw, got, host_addr, sizeof(host_addr));
        if (host_len > 0) {
            if ((socklen_t) host_len > *addrlen)
                host_len = *addrlen;
            memcpy(addr, host_addr, (size_t) host_len);
            *addrlen = (socklen_t) host_len;
        }
    }
    return (ssize_t) res;
}

ssize_t nlibc_recv(int fd_no, void *buf, size_t len, int flags) {
    return nlibc_recvfrom(fd_no, buf, len, flags, NULL, NULL);
}

int nlibc_shutdown(int fd_no, int how) {
    // SHUT_RD/WR/RDWR are 0/1/2 on both sides.
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_shutdown, fd_no, how));
}

// Levels other than SOL_SOCKET are IPPROTO_ numbers, which agree.
static int nlibc_sockopt_to_guest(int level, int option, int *guest_level) {
    if (level != SOL_SOCKET) {
        *guest_level = level;
        return option;
    }
    *guest_level = NLIBC_SOL_SOCKET_;
    for (size_t i = 0; i < NLIBC_MAP_COUNT(nlibc_sock_opts); i++)
        if (nlibc_sock_opts[i].host == option)
            return nlibc_sock_opts[i].guest;
    return -1;
}

int nlibc_setsockopt(int fd_no, int level, int option, const void *value, socklen_t len) {
    NATIVE_FRAME;
    int guest_level;
    int guest_option = nlibc_sockopt_to_guest(level, option, &guest_level);
    if (guest_option < 0)
        return nlibc_fail(_ENOPROTOOPT);
    guest_addr_t guest_value = native_scratch_put(value, len);
    if (guest_value == 0 && len > 0)
        return nlibc_fail(_ENOMEM);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_setsockopt, fd_no, guest_level,
            guest_option, guest_value, len));
}

int nlibc_getsockopt(int fd_no, int level, int option, void *value, socklen_t *len) {
    NATIVE_FRAME;
    if (len == NULL)
        return nlibc_fail(_EFAULT);
    int guest_level;
    int guest_option = nlibc_sockopt_to_guest(level, option, &guest_level);
    if (guest_option < 0)
        return nlibc_fail(_ENOPROTOOPT);
    guest_addr_t guest_value = native_scratch_alloc(*len);
    dword_t size = *len;
    guest_addr_t guest_size = native_scratch_put(&size, sizeof(size));
    if ((guest_value == 0 && *len > 0) || guest_size == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_getsockopt, fd_no, guest_level,
            guest_option, guest_value, guest_size);
    if (res < 0)
        return nlibc_fail((int) res);
    if (native_scratch_get(&size, guest_size, sizeof(size)) < 0 ||
            (size > 0 && native_scratch_get(value, guest_value, size) < 0))
        return nlibc_fail(_EFAULT);
    *len = size;
    return 0;
}

// ---------------------------------------------------------------- resolution
//
// getaddrinfo on the host reads the MAC's /etc/resolv.conf and /etc/hosts and
// answers from the Mac's resolver. What a guest asks for has to be answered
// from the guest.
//
// Numeric addresses and /etc/hosts are served here, from the guest's own
// files. Names needing DNS are NOT: that wants a stub resolver speaking to the
// nameservers in the guest's /etc/resolv.conf over the sockets above, which is
// a piece of work in its own right. EAI_NONAME is the honest answer until then
// -- SmallCLUE's own DNS applet does its queries itself and only uses
// getaddrinfo to turn the SERVER argument into an address, which is numeric in
// the case anyone actually types.

#define NLIBC_EAI_NONAME -2
#define NLIBC_EAI_FAIL   -4
#define NLIBC_EAI_MEMORY -10

static int nlibc_parse_numeric(const char *node, int family, void *out, int *out_family) {
    struct in_addr v4;
    if ((family == AF_UNSPEC || family == AF_INET) && inet_pton(AF_INET, node, &v4) == 1) {
        memcpy(out, &v4, sizeof(v4));
        *out_family = AF_INET;
        return 0;
    }
    struct in6_addr v6;
    if ((family == AF_UNSPEC || family == AF_INET6) && inet_pton(AF_INET6, node, &v6) == 1) {
        memcpy(out, &v6, sizeof(v6));
        *out_family = AF_INET6;
        return 0;
    }
    return -1;
}

// The GUEST's /etc/hosts, read through the shim's own open/read.
static int nlibc_hosts_lookup(const char *node, int family, void *out, int *out_family) {
    FILE *f = nlibc_fopen("/etc/hosts", "r");
    if (f == NULL)
        return -1;
    char line[512];
    int found = -1;
    while (found < 0 && fgets(line, sizeof(line), f) != NULL) {
        char *hash = strchr(line, '#');
        if (hash != NULL)
            *hash = '\0';
        char *save = NULL;
        char *addr = strtok_r(line, " \t\r\n", &save);
        if (addr == NULL)
            continue;
        for (char *name = strtok_r(NULL, " \t\r\n", &save); name != NULL;
                name = strtok_r(NULL, " \t\r\n", &save)) {
            if (strcasecmp(name, node) == 0) {
                found = nlibc_parse_numeric(addr, family, out, out_family);
                break;
            }
        }
    }
    fclose(f);
    return found;
}

int nlibc_getaddrinfo(const char *node, const char *service,
        const struct addrinfo *hints, struct addrinfo **res) {
    if (res == NULL)
        return NLIBC_EAI_FAIL;
    *res = NULL;
    if (node == NULL)
        return NLIBC_EAI_NONAME;

    int family = hints != NULL ? hints->ai_family : AF_UNSPEC;
    int socktype = hints != NULL && hints->ai_socktype != 0 ? hints->ai_socktype : SOCK_STREAM;
    int protocol = hints != NULL ? hints->ai_protocol : 0;

    uint8_t raw[16];
    int found_family = 0;
    if (nlibc_parse_numeric(node, family, raw, &found_family) < 0) {
        if (hints != NULL && (hints->ai_flags & AI_NUMERICHOST))
            return NLIBC_EAI_NONAME;
        if (nlibc_hosts_lookup(node, family, raw, &found_family) < 0)
            return NLIBC_EAI_NONAME;
    }

    // Ports: a numeric service, or one named in the guest's /etc/services --
    // which is not consulted, for the same reason DNS is not. Numeric covers
    // what a program passes programmatically.
    unsigned port = 0;
    if (service != NULL && service[0] != '\0') {
        char *end = NULL;
        unsigned long parsed = strtoul(service, &end, 10);
        if (end == NULL || *end != '\0' || parsed > 65535)
            return NLIBC_EAI_NONAME;
        port = (unsigned) parsed;
    }

    struct addrinfo *ai = calloc(1, sizeof(*ai));
    if (ai == NULL)
        return NLIBC_EAI_MEMORY;
    if (found_family == AF_INET) {
        struct sockaddr_in *sa = calloc(1, sizeof(*sa));
        if (sa == NULL) { free(ai); return NLIBC_EAI_MEMORY; }
        sa->sin_len = sizeof(*sa);
        sa->sin_family = AF_INET;
        sa->sin_port = htons((uint16_t) port);
        memcpy(&sa->sin_addr, raw, sizeof(sa->sin_addr));
        ai->ai_addr = (struct sockaddr *) sa;
        ai->ai_addrlen = sizeof(*sa);
    } else {
        struct sockaddr_in6 *sa = calloc(1, sizeof(*sa));
        if (sa == NULL) { free(ai); return NLIBC_EAI_MEMORY; }
        sa->sin6_len = sizeof(*sa);
        sa->sin6_family = AF_INET6;
        sa->sin6_port = htons((uint16_t) port);
        memcpy(&sa->sin6_addr, raw, sizeof(sa->sin6_addr));
        ai->ai_addr = (struct sockaddr *) sa;
        ai->ai_addrlen = sizeof(*sa);
    }
    ai->ai_family = found_family;
    ai->ai_socktype = socktype;
    ai->ai_protocol = protocol;
    *res = ai;
    return 0;
}

void nlibc_freeaddrinfo(struct addrinfo *res) {
    while (res != NULL) {
        struct addrinfo *next = res->ai_next;
        free(res->ai_addr);
        free(res->ai_canonname);
        free(res);
        res = next;
    }
}

const char *nlibc_gai_strerror(int code) {
    switch (code) {
        case 0:                return "no error";
        case NLIBC_EAI_NONAME: return "name or service not known";
        case NLIBC_EAI_MEMORY: return "memory allocation failure";
        default:               return "non-recoverable failure in name resolution";
    }
}

// The reverse direction is DNS too (PTR lookups), so the numeric form is all
// there is until the resolver exists. NI_NAMEREQD asks for a name or nothing,
// which is exactly what has to fail here.
int nlibc_getnameinfo(const void *addr, socklen_t addrlen, char *host, socklen_t hostlen,
        char *serv, socklen_t servlen, int flags) {
    const struct sockaddr *sa = addr;
    if (sa == NULL || host == NULL || hostlen == 0)
        return NLIBC_EAI_FAIL;
    if (flags & NI_NAMEREQD)
        return NLIBC_EAI_NONAME;
    const void *raw;
    unsigned port;
    if (sa->sa_family == AF_INET && addrlen >= sizeof(struct sockaddr_in)) {
        raw = &((const struct sockaddr_in *) addr)->sin_addr;
        port = ntohs(((const struct sockaddr_in *) addr)->sin_port);
    } else if (sa->sa_family == AF_INET6 && addrlen >= sizeof(struct sockaddr_in6)) {
        raw = &((const struct sockaddr_in6 *) addr)->sin6_addr;
        port = ntohs(((const struct sockaddr_in6 *) addr)->sin6_port);
    } else {
        return NLIBC_EAI_FAIL;
    }
    if (inet_ntop(sa->sa_family, raw, host, hostlen) == NULL)
        return NLIBC_EAI_FAIL;
    if (serv != NULL && servlen > 0)
        snprintf(serv, servlen, "%u", port);
    return 0;
}

// getifaddrs enumerates the HOST's interfaces -- en0, the Mac's addresses. The
// guest's are in /proc/net/dev and behind SIOCGIFADDR, and building an ifaddrs
// list from those is a piece of work with no caller yet: SmallCLUE only
// reaches for this in one diagnostic path. Refusing is what keeps the Mac's
// interfaces from being reported as the guest's.
int nlibc_getifaddrs(struct ifaddrs **ifap) {
    if (ifap != NULL)
        *ifap = NULL;
    return nlibc_fail(_ENOSYS);
}
void nlibc_freeifaddrs(struct ifaddrs *ifa) { (void) ifa; }

// ------------------------------------------------------------------- signals
//
// A native program's handler is HOST code, and the kernel delivers a signal by
// building a sigcontext and jumping the guest CPU to the handler's address --
// which for host code is not a thing that can be done, there being no guest
// instruction stream to jump into. So dispositions split two ways:
//
//  - SIG_DFL and SIG_IGN go straight to the kernel. Those are dispositions it
//    implements itself, and they have to reach it: nohup's SIG_IGN on SIGHUP
//    and the SIG_IGN on SIGPIPE that keeps a broken pipe from killing an
//    applet are only meaningful there.
//  - A real handler is recorded here and the signal BLOCKED, so the kernel
//    holds it pending instead of taking the default action. Every syscall
//    drains the pending set (native_checkpoint -> nlibc_deliver_signals) and
//    calls the handler on the program's own thread.
//
// A handler therefore runs between syscalls rather than at an arbitrary
// instruction. For the programs this serves -- SIGWINCH redraws, ^C in an
// interactive applet -- that is where they would have run anyway. What it does
// not give: interruption of a compute loop that makes no syscalls at all, and
// no SA_SIGINFO or sigaltstack.

typedef void (*nlibc_sighandler)(int);

// Indexed by HOST signal number. Static, like the rest of this file's state:
// two native programs running at once already share nlibc_std and the passwd
// cache, and fixing that is a separate piece of work.
static nlibc_sighandler nlibc_handlers[NSIG];

// how values: Linux 0/1/2, Darwin 1/2/3.
static int nlibc_sigmask_how(int host_how) {
    switch (host_how) {
        case SIG_BLOCK:   return SIG_BLOCK_;
        case SIG_UNBLOCK: return SIG_UNBLOCK_;
        case SIG_SETMASK: return SIG_SETMASK_;
        default:          return -1;
    }
}

static sigset_t_ nlibc_sigset_to_guest(const sigset_t *host) {
    sigset_t_ out = 0;
    for (size_t i = 0; i < NLIBC_MAP_COUNT(nlibc_signals); i++)
        if (sigismember(host, nlibc_signals[i].host) == 1)
            out |= (sigset_t_) 1 << (nlibc_signals[i].guest - 1);
    return out;
}

static void nlibc_sigset_from_guest(sigset_t_ guest, sigset_t *host) {
    sigemptyset(host);
    for (size_t i = 0; i < NLIBC_MAP_COUNT(nlibc_signals); i++)
        if (guest & ((sigset_t_) 1 << (nlibc_signals[i].guest - 1)))
            sigaddset(host, nlibc_signals[i].host);
}

static int nlibc_rt_sigprocmask(int guest_how, sigset_t_ set, sigset_t_ *old_out) {
    NATIVE_FRAME;
    guest_addr_t guest_set = native_scratch_put(&set, sizeof(set));
    guest_addr_t guest_old = old_out != NULL ? native_scratch_alloc(sizeof(set)) : 0;
    if (guest_set == 0 || (old_out != NULL && guest_old == 0))
        return _ENOMEM;
    sqword_t res = native_syscall(NATIVE_SYS_rt_sigprocmask, guest_how, guest_set,
            guest_old, sizeof(sigset_t_));
    if (res < 0)
        return (int) res;
    if (old_out != NULL && native_scratch_get(old_out, guest_old, sizeof(*old_out)) < 0)
        return _EFAULT;
    return 0;
}

// Sets the kernel's disposition and the block bit that goes with it.
static int nlibc_set_disposition(int guest_sig, nlibc_sighandler handler) {
    NATIVE_FRAME;
    // The kernel only ever sees SIG_DFL or SIG_IGN from a native program; a
    // real handler is ours to run, and the kernel's default action for it must
    // not fire in the meantime, which is what the block below is for.
    struct sigaction_ act = {
        .handler = handler == SIG_IGN ? SIG_IGN_ : SIG_DFL_,
    };
    guest_addr_t guest_act = native_scratch_put(&act, sizeof(act));
    if (guest_act == 0)
        return _ENOMEM;
    sqword_t res = native_syscall(NATIVE_SYS_rt_sigaction, guest_sig, guest_act, 0,
            sizeof(sigset_t_));
    if (res < 0)
        return (int) res;

    bool ours = handler != SIG_DFL && handler != SIG_IGN;
    return nlibc_rt_sigprocmask(ours ? SIG_BLOCK_ : SIG_UNBLOCK_,
            (sigset_t_) 1 << (guest_sig - 1), NULL);
}

int nlibc_sigaction(int host_sig, const struct sigaction *act, struct sigaction *oact) {
    if (host_sig <= 0 || host_sig >= NSIG)
        return nlibc_fail(_EINVAL);
    int guest_sig = nlibc_signal_to_guest(host_sig);
    if (guest_sig == 0)
        return nlibc_fail(_EINVAL);

    if (oact != NULL) {
        memset(oact, 0, sizeof(*oact));
        oact->sa_handler = nlibc_handlers[host_sig];
    }
    if (act == NULL)
        return 0;

    // SA_SIGINFO would want a siginfo the kernel cannot hand host code, so the
    // three-argument form is refused rather than silently called with two.
    if (act->sa_flags & SA_SIGINFO)
        return nlibc_fail(_ENOSYS);

    int err = nlibc_set_disposition(guest_sig, act->sa_handler);
    if (err < 0)
        return nlibc_fail(err);
    nlibc_handlers[host_sig] = act->sa_handler;
    return 0;
}

nlibc_sighandler nlibc_signal(int host_sig, nlibc_sighandler handler) {
    if (host_sig <= 0 || host_sig >= NSIG) {
        nlibc_fail(_EINVAL);
        return SIG_ERR;
    }
    int guest_sig = nlibc_signal_to_guest(host_sig);
    if (guest_sig == 0) {
        nlibc_fail(_EINVAL);
        return SIG_ERR;
    }
    nlibc_sighandler previous = nlibc_handlers[host_sig];
    int err = nlibc_set_disposition(guest_sig, handler);
    if (err < 0) {
        nlibc_fail(err);
        return SIG_ERR;
    }
    nlibc_handlers[host_sig] = handler;
    return previous;
}

int nlibc_sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    int guest_how = nlibc_sigmask_how(how);
    if (guest_how < 0)
        return nlibc_fail(_EINVAL);
    // A signal this shim handles must stay blocked whatever the program asks:
    // unblocking it would let the kernel take the default action for something
    // the program believes it has a handler for.
    sigset_t_ guest_set = set != NULL ? nlibc_sigset_to_guest(set) : 0;
    if (set != NULL && (guest_how == SIG_UNBLOCK_ || guest_how == SIG_SETMASK_)) {
        for (int sig = 1; sig < NSIG; sig++) {
            nlibc_sighandler h = nlibc_handlers[sig];
            if (h == NULL || h == SIG_DFL || h == SIG_IGN)
                continue;
            int guest_sig = nlibc_signal_to_guest(sig);
            if (guest_sig == 0)
                continue;
            sigset_t_ bit = (sigset_t_) 1 << (guest_sig - 1);
            if (guest_how == SIG_UNBLOCK_)
                guest_set &= ~bit;
            else
                guest_set |= bit;
        }
    }
    sigset_t_ old = 0;
    int err = nlibc_rt_sigprocmask(guest_how, guest_set, oldset != NULL ? &old : NULL);
    if (err < 0)
        return nlibc_fail(err);
    if (oldset != NULL)
        nlibc_sigset_from_guest(old, oldset);
    return 0;
}

int nlibc_sigpending(sigset_t *set) {
    NATIVE_FRAME;
    if (set == NULL)
        return nlibc_fail(_EFAULT);
    guest_addr_t guest_set = native_scratch_alloc(sizeof(sigset_t_));
    if (guest_set == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_rt_sigpending, guest_set);
    if (res < 0)
        return nlibc_fail((int) res);
    sigset_t_ pending = 0;
    if (native_scratch_get(&pending, guest_set, sizeof(pending)) < 0)
        return nlibc_fail(_EFAULT);
    nlibc_sigset_from_guest(pending, set);
    return 0;
}

// Dequeues one signal from `set`, waiting if `wait` is true. Returns the guest
// signal number, or a negative errno.
static int nlibc_sigtake(sigset_t_ set, bool wait) {
    NATIVE_FRAME;
    guest_addr_t guest_set = native_scratch_put(&set, sizeof(set));
    if (guest_set == 0)
        return _ENOMEM;
    guest_addr_t guest_timeout = 0;
    if (!wait) {
        // The only timespec in this file whose width follows the TASK's ABI
        // rather than being fixed at 64-bit: sys_rt_sigtimedwait_common reads
        // it through guest_abi_is_64bit(current->abi), where utimensat, ppoll
        // and pselect all take the 64-bit shape whatever the task is.
        if (guest_abi_is_64bit(current->abi)) {
            struct { sqword_t sec, nsec; } zero = {0, 0};
            guest_timeout = native_scratch_put(&zero, sizeof(zero));
        } else {
            struct { sdword_t sec, nsec; } zero = {0, 0};
            guest_timeout = native_scratch_put(&zero, sizeof(zero));
        }
        if (guest_timeout == 0)
            return _ENOMEM;
    }
    return (int) native_syscall(NATIVE_SYS_rt_sigtimedwait, guest_set, 0,
            guest_timeout, sizeof(sigset_t_));
}

int nlibc_sigwait(const sigset_t *set, int *sig) {
    if (set == NULL || sig == NULL)
        return EINVAL;   // sigwait reports through its return value, not errno
    int res = nlibc_sigtake(nlibc_sigset_to_guest(set), true);
    if (res < 0)
        return nlibc_host_errno(res);
    *sig = nlibc_signal_to_host(res);
    return 0;
}

// Called from native_checkpoint, which every syscall goes through: this is
// where a shim-held handler actually runs.
void nlibc_deliver_signals(void) {
    // A handler makes syscalls of its own, each of which checkpoints again.
    static bool delivering;
    if (delivering)
        return;

    // Which of ours are pending. Cheap enough not to matter next to the
    // syscall it precedes, but skipped entirely when nothing is installed.
    sigset_t_ ours = 0;
    for (int host_sig = 1; host_sig < NSIG; host_sig++) {
        nlibc_sighandler h = nlibc_handlers[host_sig];
        if (h == NULL || h == SIG_DFL || h == SIG_IGN)
            continue;
        int guest_sig = nlibc_signal_to_guest(host_sig);
        if (guest_sig != 0)
            ours |= (sigset_t_) 1 << (guest_sig - 1);
    }
    if (ours == 0)
        return;

    delivering = true;
    for (;;) {
        int guest_sig = nlibc_sigtake(ours, false);
        if (guest_sig <= 0)
            break;
        int host_sig = nlibc_signal_to_host(guest_sig);
        nlibc_sighandler handler = host_sig > 0 && host_sig < NSIG ?
                nlibc_handlers[host_sig] : NULL;
        if (handler != NULL && handler != SIG_DFL && handler != SIG_IGN)
            handler(host_sig);
    }
    delivering = false;
}

// -------------------------------------------------------------------- session
//
// setsid/setpgid change what the KERNEL records for the task, so there is
// nothing to translate -- these are the plainest possible case for routing
// through the dispatcher, and they were unreachable before it.

pid_t nlibc_setsid(void) {
    return (pid_t) nlibc_ret(native_syscall(NATIVE_SYS_setsid));
}
int nlibc_setpgid(pid_t pid, pid_t pgid) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_setpgid, pid, pgid));
}
pid_t nlibc_getpgid(pid_t pid) {
    return (pid_t) nlibc_ret(native_syscall(NATIVE_SYS_getpgid, pid));
}
pid_t nlibc_getsid(pid_t pid) {
    return (pid_t) nlibc_ret(native_syscall(NATIVE_SYS_getsid, pid));
}
// getpgrp() is getpgid(0) with the argument spelled out; POSIX gives it no
// parameter, so there is nothing else it could mean.
pid_t nlibc_getpgrp(void) {
    return (pid_t) nlibc_ret(native_syscall(NATIVE_SYS_getpgid, 0));
}

// The foreground process group of a terminal. Two ioctls in AOK (fs/tty.c),
// which is also how glibc and musl implement these -- they are not syscalls of
// their own on Linux. Left to the host libc they would have asked the Mac's
// terminal about a guest fd number, which is a different terminal or none.
pid_t nlibc_tcgetpgrp(int fd_no) {
    dword_t pgrp = 0;
    if (nlibc_tty_ioctl(fd_no, TIOCGPGRP_, &pgrp, sizeof(pgrp), true) < 0)
        return -1;
    return (pid_t) pgrp;
}
int nlibc_tcsetpgrp(int fd_no, pid_t pgrp) {
    dword_t value = (dword_t) pgrp;
    return nlibc_tty_ioctl(fd_no, TIOCSPGRP_, &value, sizeof(value), false);
}

// --------------------------------------------------------------- environment
//
// getenv() in host code answers about the HOST process -- the developer's
// shell on the macOS build, the app's on iOS. `env` printed the Mac's
// environment, and every PATH search a native program made walked the Mac's
// directories. The storage lives on the task (kernel/native.h); these are the
// libc shapes over it.

char **nlibc_environ(void) { return native_env_vector(); }
char ***nlibc_environp(void) { return native_env_slot(); }

char *nlibc_getenv(const char *name) {
    // getenv's contract is that the pointer stays valid until the environment
    // is next changed, which is exactly the lifetime of the entry it points
    // into -- so this hands back the storage rather than a copy.
    return (char *) native_env_get(name);
}

int nlibc_setenv(const char *name, const char *value, int overwrite) {
    int err = native_env_set(name, value, overwrite != 0);
    return err < 0 ? nlibc_fail(err) : 0;
}

int nlibc_unsetenv(const char *name) {
    int err = native_env_unset(name);
    return err < 0 ? nlibc_fail(err) : 0;
}

int nlibc_putenv(char *entry) {
    // putenv's contract is that the caller's string BECOMES the environment
    // entry. Copying instead is the safe divergence: the alternative is
    // holding a pointer into a caller's buffer that may be a local.
    if (entry == NULL)
        return nlibc_fail(_EINVAL);
    const char *eq = strchr(entry, '=');
    if (eq == NULL)
        return nlibc_unsetenv(entry);
    char name[256];
    size_t len = (size_t) (eq - entry);
    if (len >= sizeof(name))
        return nlibc_fail(_EINVAL);
    memcpy(name, entry, len);
    name[len] = '\0';
    return nlibc_setenv(name, eq + 1, 1);
}

// ------------------------------------------------------------------ identity
//
// `whoami` answered "mobile" -- the iOS user account. Every one of these was
// reaching the host: getuid returned the app's uid, getpid the app's pid, and
// getpwuid read the DEVICE's passwd database rather than the guest's. Same
// class as uname and getmntinfo, and the guard's denylist had no idea it
// existed either.
//
// The task carries the guest's credentials (kernel/task.h), and the guest's
// /etc/passwd and /etc/group are ordinary files reachable through the VFS.

uid_t nlibc_getuid(void)  { return (uid_t) native_syscall(NATIVE_SYS_getuid); }
uid_t nlibc_geteuid(void) { return (uid_t) native_syscall(NATIVE_SYS_geteuid); }
gid_t nlibc_getgid(void)  { return (gid_t) native_syscall(NATIVE_SYS_getgid); }
gid_t nlibc_getegid(void) { return (gid_t) native_syscall(NATIVE_SYS_getegid); }
pid_t nlibc_getpid(void)  { return (pid_t) native_syscall(NATIVE_SYS_getpid); }
pid_t nlibc_getppid(void) { return (pid_t) native_syscall(NATIVE_SYS_getppid); }

int nlibc_getgroups(int size, gid_t list[]) {
    NATIVE_FRAME;
    if (size == 0)
        return (int) nlibc_ret(native_syscall(NATIVE_SYS_getgroups, 0, 0));
    if (size < 0 || list == NULL)
        return nlibc_fail(_EINVAL);
    // The guest's gid_t is 32 bits, as Darwin's is.
    size_t bytes = (size_t) size * sizeof(uint32_t);
    guest_addr_t guest_list = native_scratch_alloc(bytes);
    if (guest_list == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_getgroups, size, guest_list);
    if (res < 0)
        return nlibc_fail((int) res);
    if (res > 0 && native_scratch_get(list, guest_list, (size_t) res * sizeof(uint32_t)) < 0)
        return nlibc_fail(_EFAULT);
    return (int) res;
}

int nlibc_setuid(uid_t uid) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_setuid, uid));
}
int nlibc_setgid(gid_t gid) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_setgid, gid));
}
int nlibc_initgroups(const char *user, gid_t group) {
    (void) user; (void) group;
    return 0;   // no supplementary groups to set; succeeding is the honest no-op
}

// /etc/passwd and /etc/group, read from the GUEST. One entry is cached at a
// time, which is what getpwuid's contract allows -- the returned pointer is
// only valid until the next call.
static char nlibc_pw_line[512];
static struct passwd nlibc_pw;
static char nlibc_gr_line[512];
static struct group nlibc_gr;
static char *nlibc_gr_members[1];

static char *nlibc_next_field(char **cursor) {
    char *start = *cursor;
    if (start == NULL)
        return NULL;
    char *colon = strchr(start, ':');
    if (colon != NULL) {
        *colon = '\0';
        *cursor = colon + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

// Walks a colon-separated database, handing each line to `match`.
static bool nlibc_scan_db(const char *path, char *linebuf, size_t linelen,
        bool (*match)(char **fields, size_t n, const void *key), const void *key) {
    struct fd *fd = NULL;
    if (native_open(path, O_RDONLY_, &fd) < 0)
        return false;
    char buf[4096];
    size_t held = 0;
    bool found = false;
    for (;;) {
        ssize_t n = native_read(fd, buf + held, sizeof(buf) - held - 1);
        if (n < 0)
            break;
        size_t avail = held + (size_t) n;
        buf[avail] = '\0';
        char *line = buf, *nl;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = '\0';
            if (line[0] != '\0' && line[0] != '#') {
                snprintf(linebuf, linelen, "%s", line);
                char *cursor = linebuf;
                char *fields[8] = {0};
                size_t nf = 0;
                while (nf < 8) {
                    char *f = nlibc_next_field(&cursor);
                    if (f == NULL) break;
                    fields[nf++] = f;
                }
                if (match(fields, nf, key)) {
                    found = true;
                    goto done;
                }
            }
            line = nl + 1;
        }
        if (n == 0)
            break;
        held = strlen(line);
        memmove(buf, line, held);
    }
done:
    native_close(fd);
    return found;
}

struct nlibc_pw_key { const char *name; uid_t uid; bool by_name; };

static bool nlibc_pw_match(char **f, size_t n, const void *keyv) {
    const struct nlibc_pw_key *key = keyv;
    if (n < 7)
        return false;
    if (key->by_name ? strcmp(f[0], key->name) != 0
                     : (uid_t) strtoul(f[2], NULL, 10) != key->uid)
        return false;
    nlibc_pw.pw_name = f[0];
    nlibc_pw.pw_passwd = f[1];
    nlibc_pw.pw_uid = (uid_t) strtoul(f[2], NULL, 10);
    nlibc_pw.pw_gid = (gid_t) strtoul(f[3], NULL, 10);
    nlibc_pw.pw_gecos = f[4];
    nlibc_pw.pw_dir = f[5];
    nlibc_pw.pw_shell = f[6];
    return true;
}

struct passwd *nlibc_getpwuid(uid_t uid) {
    struct nlibc_pw_key key = { NULL, uid, false };
    memset(&nlibc_pw, 0, sizeof(nlibc_pw));
    if (!nlibc_scan_db("/etc/passwd", nlibc_pw_line, sizeof(nlibc_pw_line),
                       nlibc_pw_match, &key))
        return NULL;
    return &nlibc_pw;
}

struct passwd *nlibc_getpwnam(const char *name) {
    if (name == NULL)
        return NULL;
    struct nlibc_pw_key key = { name, 0, true };
    memset(&nlibc_pw, 0, sizeof(nlibc_pw));
    if (!nlibc_scan_db("/etc/passwd", nlibc_pw_line, sizeof(nlibc_pw_line),
                       nlibc_pw_match, &key))
        return NULL;
    return &nlibc_pw;
}

struct nlibc_gr_key { const char *name; gid_t gid; bool by_name; };

static bool nlibc_gr_match(char **f, size_t n, const void *keyv) {
    const struct nlibc_gr_key *key = keyv;
    if (n < 3)
        return false;
    if (key->by_name ? strcmp(f[0], key->name) != 0
                     : (gid_t) strtoul(f[2], NULL, 10) != key->gid)
        return false;
    nlibc_gr_members[0] = NULL;
    nlibc_gr.gr_name = f[0];
    nlibc_gr.gr_passwd = f[1];
    nlibc_gr.gr_gid = (gid_t) strtoul(f[2], NULL, 10);
    nlibc_gr.gr_mem = nlibc_gr_members;
    return true;
}

struct group *nlibc_getgrgid(gid_t gid) {
    struct nlibc_gr_key key = { NULL, gid, false };
    memset(&nlibc_gr, 0, sizeof(nlibc_gr));
    if (!nlibc_scan_db("/etc/group", nlibc_gr_line, sizeof(nlibc_gr_line),
                       nlibc_gr_match, &key))
        return NULL;
    return &nlibc_gr;
}

struct group *nlibc_getgrnam(const char *name) {
    if (name == NULL)
        return NULL;
    struct nlibc_gr_key key = { name, 0, true };
    memset(&nlibc_gr, 0, sizeof(nlibc_gr));
    if (!nlibc_scan_db("/etc/group", nlibc_gr_line, sizeof(nlibc_gr_line),
                       nlibc_gr_match, &key))
        return NULL;
    return &nlibc_gr;
}

// =========================================================== the rest of it
//
// Everything below exists because "route what the current program happens to
// call" is how the shim got a 77-entry syscall list against a 217-syscall
// kernel (see tools/gen-native-syscalls.py). The numbers are all reachable
// now; these are the libc entry points on top of them, added by walking the
// syscall table rather than by waiting for a program to miss one.
//
// Shapes, in rough order of how much care each needs:
//   - a syscall with scalar arguments, which is a one-liner;
//   - a syscall with a struct, which needs the guest's layout written out;
//   - a constant AOK's kernel has no syscall for, answered here;
//   - a refusal, where the honest answer is that it cannot work in-process.

// ------------------------------------------------------------ scalar syscalls

int nlibc_fchmod(int fd_no, mode_t mode) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_fchmod, fd_no, mode));
}
int nlibc_fchown(int fd_no, uid_t uid, gid_t gid) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_fchown, fd_no, uid, gid));
}
int nlibc_fchdir(int fd_no) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_fchdir, fd_no));
}
int nlibc_fsync(int fd_no) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_fsync, fd_no));
}
int nlibc_fdatasync(int fd_no) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_fdatasync, fd_no));
}
int nlibc_ftruncate(int fd_no, off_t len) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_ftruncate, fd_no, len));
}
int nlibc_syncfs(int fd_no) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_syncfs, fd_no));
}
pid_t nlibc_gettid(void) {
    return (pid_t) nlibc_ret(native_syscall(NATIVE_SYS_gettid));
}
int nlibc_sched_yield(void) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_sched_yield));
}
mode_t nlibc_umask(mode_t mask) {
    // umask cannot fail and returns the PREVIOUS mask, so there is no error to
    // translate -- and translating one would turn a legitimate mask into -1.
    return (mode_t) native_syscall(NATIVE_SYS_umask, mask);
}
int nlibc_setreuid(uid_t r, uid_t e) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_setreuid, r, e));
}
int nlibc_setregid(gid_t r, gid_t e) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_setregid, r, e));
}

// Darwin's flock operation numbers are 1/2/4/8 and the guest's are 1/2/4/8 as
// well -- LOCK_SH/EX/NB/UN agree -- so this is one of the few where passing the
// value through is correct rather than lazy.
int nlibc_flock(int fd_no, int operation) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_flock, fd_no, operation));
}

// kill(-pgrp), which is what killpg IS on Linux; there is no syscall of its own.
int nlibc_killpg(pid_t pgrp, int sig) {
    if (pgrp < 0)
        return nlibc_fail(_EINVAL);
    return nlibc_kill(-pgrp, sig);
}

int nlibc_faccessat(int dirfd, const char *path, int mode, int flags) {
    NATIVE_FRAME;
    if (path == NULL)
        return nlibc_fail(_EFAULT);
    guest_addr_t guest_path = native_scratch_str(path);
    if (guest_path == 0)
        return nlibc_fail(_ENOMEM);
    // AT_EACCESS is 0x10 on Darwin and 0x200 on the guest; the rest of the mode
    // bits (R_OK/W_OK/X_OK/F_OK) agree.
    dword_t guest_flags = (flags & AT_EACCESS) ? 0x200 : 0;
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_faccessat2,
            nlibc_at_fd(dirfd), guest_path, mode, guest_flags));
}

// --------------------------------------------------------- resource limits
//
// The RESOURCE numbers disagree, and only from 5 upward, which is the sort of
// difference that looks like it works: RLIMIT_CPU through RLIMIT_CORE are 0-4
// on both, so anything testing those alone would pass while NOFILE silently
// asked about something else. Darwin's RLIMIT_AS is 5 where the guest's is 9,
// and Darwin's NOFILE is 8 where the guest's is 7.
static int nlibc_rlimit_to_guest(int resource) {
    switch (resource) {
        case RLIMIT_CPU:     return 0;
        case RLIMIT_FSIZE:   return 1;
        case RLIMIT_DATA:    return 2;
        case RLIMIT_STACK:   return 3;
        case RLIMIT_CORE:    return 4;
        case RLIMIT_NPROC:   return 6;
        case RLIMIT_NOFILE:  return 7;
        case RLIMIT_MEMLOCK: return 8;
        case RLIMIT_AS:      return 9;
        default:             return -1;
    }
}

// The guest's rlim_t is 64-bit and its RLIM_INFINITY is ~0ULL; Darwin's is
// RLIM_INFINITY too but spelled differently, so the sentinel is translated
// rather than copied.
struct nlibc_guest_rlimit { uint64_t cur, max; };
#define NLIBC_GUEST_RLIM_INFINITY (~(uint64_t) 0)

static rlim_t nlibc_rlim_to_host(uint64_t v) {
    return v == NLIBC_GUEST_RLIM_INFINITY ? RLIM_INFINITY : (rlim_t) v;
}
static uint64_t nlibc_rlim_to_guest(rlim_t v) {
    return v == RLIM_INFINITY ? NLIBC_GUEST_RLIM_INFINITY : (uint64_t) v;
}

int nlibc_getrlimit(int resource, struct rlimit *out) {
    NATIVE_FRAME;
    int guest_res = nlibc_rlimit_to_guest(resource);
    if (guest_res < 0 || out == NULL)
        return nlibc_fail(guest_res < 0 ? _EINVAL : _EFAULT);
    guest_addr_t guest_rl = native_scratch_alloc(sizeof(struct nlibc_guest_rlimit));
    if (guest_rl == 0)
        return nlibc_fail(_ENOMEM);
    // prlimit64 with pid 0 and a NULL "new" is the modern getrlimit, and it is
    // the one whose struct is unambiguously 64-bit on every guest ABI.
    sqword_t res = native_syscall(NATIVE_SYS_prlimit64, 0, guest_res, 0, guest_rl);
    if (res < 0)
        return nlibc_fail((int) res);
    struct nlibc_guest_rlimit rl = {};
    if (native_scratch_get(&rl, guest_rl, sizeof(rl)) < 0)
        return nlibc_fail(_EFAULT);
    out->rlim_cur = nlibc_rlim_to_host(rl.cur);
    out->rlim_max = nlibc_rlim_to_host(rl.max);
    return 0;
}

int nlibc_setrlimit(int resource, const struct rlimit *in) {
    NATIVE_FRAME;
    int guest_res = nlibc_rlimit_to_guest(resource);
    if (guest_res < 0 || in == NULL)
        return nlibc_fail(guest_res < 0 ? _EINVAL : _EFAULT);
    struct nlibc_guest_rlimit rl = {
        .cur = nlibc_rlim_to_guest(in->rlim_cur),
        .max = nlibc_rlim_to_guest(in->rlim_max),
    };
    guest_addr_t guest_rl = native_scratch_put(&rl, sizeof(rl));
    if (guest_rl == 0)
        return nlibc_fail(_ENOMEM);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_prlimit64, 0, guest_res,
            guest_rl, 0));
}

// The descriptor ceiling, which on Darwin is its own call and on Linux is just
// RLIMIT_NOFILE. bash asks for it when it builds its fd bitmap.
int nlibc_getdtablesize(void) {
    struct rlimit rl;
    if (nlibc_getrlimit(RLIMIT_NOFILE, &rl) < 0)
        return 256;   // a plausible floor rather than an error; the caller sizes
                      // a table with it and cannot do anything useful with -1
    if (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > INT_MAX)
        return INT_MAX;
    return (int) rl.rlim_cur;
}

// ------------------------------------------------------------------- timers
//
// struct itimerval is two timevals, and a timeval is two 64-bit words on both
// sides for a 64-bit guest -- but the guest's is written out here rather than
// assumed, because that stops being true the moment a 32-bit guest task calls.
struct nlibc_guest_timeval { sqword_t sec, usec; };
struct nlibc_guest_itimerval { struct nlibc_guest_timeval interval, value; };

static void nlibc_itimer_to_guest(const struct itimerval *in,
        struct nlibc_guest_itimerval *out) {
    out->interval.sec = in->it_interval.tv_sec;
    out->interval.usec = in->it_interval.tv_usec;
    out->value.sec = in->it_value.tv_sec;
    out->value.usec = in->it_value.tv_usec;
}
static void nlibc_itimer_to_host(const struct nlibc_guest_itimerval *in,
        struct itimerval *out) {
    out->it_interval.tv_sec = in->interval.sec;
    out->it_interval.tv_usec = in->interval.usec;
    out->it_value.tv_sec = in->value.sec;
    out->it_value.tv_usec = in->value.usec;
}

// ITIMER_REAL/VIRTUAL/PROF are 0/1/2 on both sides.
int nlibc_setitimer(int which, const struct itimerval *in, struct itimerval *out) {
    NATIVE_FRAME;
    guest_addr_t guest_new = 0, guest_old = 0;
    if (in != NULL) {
        struct nlibc_guest_itimerval it = {};
        nlibc_itimer_to_guest(in, &it);
        guest_new = native_scratch_put(&it, sizeof(it));
        if (guest_new == 0)
            return nlibc_fail(_ENOMEM);
    }
    if (out != NULL) {
        guest_old = native_scratch_alloc(sizeof(struct nlibc_guest_itimerval));
        if (guest_old == 0)
            return nlibc_fail(_ENOMEM);
    }
    sqword_t res = native_syscall(NATIVE_SYS_setitimer, which, guest_new, guest_old);
    if (res < 0)
        return nlibc_fail((int) res);
    if (out != NULL) {
        struct nlibc_guest_itimerval it = {};
        if (native_scratch_get(&it, guest_old, sizeof(it)) < 0)
            return nlibc_fail(_EFAULT);
        nlibc_itimer_to_host(&it, out);
    }
    return 0;
}

int nlibc_getitimer(int which, struct itimerval *out) {
    NATIVE_FRAME;
    if (out == NULL)
        return nlibc_fail(_EFAULT);
    guest_addr_t guest_it = native_scratch_alloc(sizeof(struct nlibc_guest_itimerval));
    if (guest_it == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_getitimer, which, guest_it);
    if (res < 0)
        return nlibc_fail((int) res);
    struct nlibc_guest_itimerval it = {};
    if (native_scratch_get(&it, guest_it, sizeof(it)) < 0)
        return nlibc_fail(_EFAULT);
    nlibc_itimer_to_host(&it, out);
    return 0;
}

// Linux has no alarm syscall on asm-generic; alarm IS setitimer(ITIMER_REAL),
// which is how glibc and musl implement it too. The return value is the number
// of seconds left on the previous timer, rounded UP -- a timer with 1ns to run
// must not report 0, which would mean "no alarm was set".
unsigned int nlibc_alarm(unsigned int seconds) {
    struct itimerval new_it = {}, old_it = {};
    new_it.it_value.tv_sec = seconds;
    if (nlibc_setitimer(ITIMER_REAL, &new_it, &old_it) < 0)
        return 0;
    unsigned int left = (unsigned int) old_it.it_value.tv_sec;
    if (old_it.it_value.tv_usec != 0)
        left++;
    return left;
}

// struct tms is four clock_t. The guest's are 64-bit words on a 64-bit ABI.
struct nlibc_guest_tms { sqword_t utime, stime, cutime, cstime; };

clock_t nlibc_times(struct tms *out) {
    NATIVE_FRAME;
    guest_addr_t guest_tms = 0;
    if (out != NULL) {
        guest_tms = native_scratch_alloc(sizeof(struct nlibc_guest_tms));
        if (guest_tms == 0)
            return (clock_t) nlibc_fail(_ENOMEM);
    }
    sqword_t res = native_syscall(NATIVE_SYS_times, guest_tms);
    if (res < 0)
        return (clock_t) nlibc_fail((int) res);
    if (out != NULL) {
        struct nlibc_guest_tms t = {};
        if (native_scratch_get(&t, guest_tms, sizeof(t)) < 0)
            return (clock_t) nlibc_fail(_EFAULT);
        out->tms_utime = t.utime;
        out->tms_stime = t.stime;
        out->tms_cutime = t.cutime;
        out->tms_cstime = t.cstime;
    }
    return (clock_t) res;
}

// ------------------------------------------------------------------ vectored
//
// readv/writev take an array of iovecs holding HOST pointers, and marshalling
// those into guest space would mean copying every buffer and rebuilding the
// array. Looping over the plain calls is the same system calls in the same
// order with the same short-read semantics, and it is what the guest sees
// either way.
ssize_t nlibc_readv(int fd_no, const struct iovec *iov, int count) {
    if (iov == NULL || count < 0)
        return nlibc_fail(_EINVAL);
    ssize_t total = 0;
    for (int i = 0; i < count; i++) {
        if (iov[i].iov_len == 0)
            continue;
        ssize_t n = nlibc_read(fd_no, iov[i].iov_base, iov[i].iov_len);
        if (n < 0)
            return total > 0 ? total : -1;   // errno is already set
        total += n;
        if ((size_t) n < iov[i].iov_len)
            break;                            // short read ends the whole call
    }
    return total;
}

ssize_t nlibc_writev(int fd_no, const struct iovec *iov, int count) {
    if (iov == NULL || count < 0)
        return nlibc_fail(_EINVAL);
    ssize_t total = 0;
    for (int i = 0; i < count; i++) {
        if (iov[i].iov_len == 0)
            continue;
        ssize_t n = nlibc_write(fd_no, iov[i].iov_base, iov[i].iov_len);
        if (n < 0)
            return total > 0 ? total : -1;
        total += n;
        if ((size_t) n < iov[i].iov_len)
            break;
    }
    return total;
}

ssize_t nlibc_pread(int fd_no, void *buf, size_t n, off_t off) {
    NATIVE_FRAME;
    if (buf == NULL && n > 0)
        return nlibc_fail(_EFAULT);
    guest_addr_t guest_buf = native_scratch_alloc(n);
    if (guest_buf == 0 && n > 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_pread, fd_no, guest_buf, n, off);
    if (res < 0)
        return nlibc_fail((int) res);
    if (res > 0 && native_scratch_get(buf, guest_buf, (size_t) res) < 0)
        return nlibc_fail(_EFAULT);
    return (ssize_t) res;
}

ssize_t nlibc_pwrite(int fd_no, const void *buf, size_t n, off_t off) {
    NATIVE_FRAME;
    if (buf == NULL && n > 0)
        return nlibc_fail(_EFAULT);
    guest_addr_t guest_buf = native_scratch_put(buf, n);
    if (guest_buf == 0 && n > 0)
        return nlibc_fail(_ENOMEM);
    return (ssize_t) nlibc_ret(native_syscall(NATIVE_SYS_pwrite, fd_no,
            guest_buf, n, off));
}

// ------------------------------------------------------------------ identity

int nlibc_gethostname(char *name, size_t len) {
    // The guest's nodename, from uname -- there is no gethostname syscall, and
    // the host's would answer with the Mac's name.
    struct utsname u;
    if (nlibc_uname(&u) < 0)
        return -1;
    if (name == NULL)
        return nlibc_fail(_EFAULT);
    size_t need = strlen(u.nodename) + 1;
    if (need > len)
        return nlibc_fail(_ENAMETOOLONG);
    memcpy(name, u.nodename, need);
    return 0;
}

// getentropy over getrandom. Its contract is all-or-nothing for up to 256
// bytes, where getrandom may return fewer, so this loops.
int nlibc_getentropy(void *buf, size_t len) {
    NATIVE_FRAME;
    if (len > 256)
        return nlibc_fail(_EIO);
    if (buf == NULL && len > 0)
        return nlibc_fail(_EFAULT);
    guest_addr_t guest_buf = native_scratch_alloc(len);
    if (guest_buf == 0 && len > 0)
        return nlibc_fail(_ENOMEM);
    size_t got = 0;
    while (got < len) {
        sqword_t res = native_syscall(NATIVE_SYS_getrandom, guest_buf + got,
                len - got, 0);
        if (res < 0)
            return nlibc_fail((int) res);
        if (res == 0)
            return nlibc_fail(_EIO);
        got += (size_t) res;
    }
    if (len > 0 && native_scratch_get(buf, guest_buf, len) < 0)
        return nlibc_fail(_EFAULT);
    return 0;
}

// ----------------------------------------------------------- temporary names
//
// mkdtemp and mktemp share mkstemp's contract: the pattern is the caller's
// buffer and the name used has to be written back into it.
static bool nlibc_fill_template(char *template) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    size_t len = strlen(template);
    if (len < 6 || strcmp(template + len - 6, "XXXXXX") != 0)
        return false;
    for (int i = 0; i < 6; i++)
        template[len - 6 + i] = alphabet[(unsigned) rand() % (sizeof(alphabet) - 1)];
    return true;
}

char *nlibc_mkdtemp(char *template) {
    if (template == NULL) {
        nlibc_fail(_EINVAL);
        return NULL;
    }
    for (int attempt = 0; attempt < 128; attempt++) {
        if (!nlibc_fill_template(template)) {
            nlibc_fail(_EINVAL);
            return NULL;
        }
        if (nlibc_mkdir(template, 0700) == 0)
            return template;
        if (errno != EEXIST)
            return NULL;
    }
    nlibc_fail(_EEXIST);
    return NULL;
}

// mktemp only reports a name that did not exist a moment ago, which is why it
// is the unsafe one everywhere. Same contract here; the race is the caller's.
char *nlibc_mktemp(char *template) {
    if (template == NULL) {
        nlibc_fail(_EINVAL);
        return NULL;
    }
    for (int attempt = 0; attempt < 128; attempt++) {
        if (!nlibc_fill_template(template)) {
            nlibc_fail(_EINVAL);
            return NULL;
        }
        struct stat st;
        if (nlibc_stat(template, &st) < 0 && errno == ENOENT)
            return template;
    }
    template[0] = '\0';
    return template;
}

// -------------------------------------------------------------- system limits
//
// pathconf and confstr have no syscall behind them on any system: they report
// constants the C library knows. Answering from the host's headers would be
// answering about iOS, so the guest's values are written out.
long nlibc_pathconf(const char *path, int name) {
    (void) path;
    return nlibc_fpathconf(-1, name);
}

long nlibc_fpathconf(int fd_no, int name) {
    (void) fd_no;
    switch (name) {
        case _PC_LINK_MAX:         return 127;
        case _PC_MAX_CANON:        return 255;
        case _PC_MAX_INPUT:        return 255;
        case _PC_NAME_MAX:         return 255;
        case _PC_PATH_MAX:         return 4096;   // the guest's PATH_MAX
        case _PC_PIPE_BUF:         return 4096;
        case _PC_CHOWN_RESTRICTED: return 1;
        case _PC_NO_TRUNC:         return 1;
        case _PC_VDISABLE:         return 0;
        default:                   return -1;     // "no limit", errno untouched
    }
}

size_t nlibc_confstr(int name, char *buf, size_t len) {
    // _CS_PATH is the only one anything asks for in practice, and it must be
    // the GUEST's default PATH rather than the Mac's /usr/bin:/bin.
    const char *value = name == _CS_PATH ? "/usr/local/bin:/usr/bin:/bin" : NULL;
    if (value == NULL) {
        errno = EINVAL;
        return 0;
    }
    size_t need = strlen(value) + 1;
    if (buf != NULL && len > 0) {
        size_t copy = need < len ? need : len;
        memcpy(buf, value, copy);
        buf[copy - 1] = '\0';
    }
    return need;
}

// ---------------------------------------------------------------- refusals
//
// Dynamic loading cannot work here and the honest answer is to say so. A
// native program lives inside a signed app: on iOS there is no dlopen of
// anything the app did not ship, and a plugin compiled for the GUEST would be
// guest code, which is not something host code can call into. bash's loadable
// builtins are the caller, and they degrade to "not available" rather than
// misbehaving.
void *nlibc_dlopen(const char *path, int mode) {
    (void) path; (void) mode;
    return NULL;
}
void *nlibc_dlsym(void *handle, const char *symbol) {
    (void) handle; (void) symbol;
    return NULL;
}
int nlibc_dlclose(void *handle) {
    (void) handle;
    return -1;
}
const char *nlibc_dlerror(void) {
    return "dynamic loading is not available in iSH-AOK";
}

// ------------------------------------------------- walking passwd and group
//
// getpwent/getgrent enumerate rather than look up, which the scanner above was
// not built for -- it stops at the first match. Rather than a second parser,
// the key becomes an INDEX and the match function counts: the Nth call returns
// the Nth entry. That re-reads the file per entry, which is O(n^2) over a walk
// and completely fine for /etc/passwd; it also means the enumeration sees the
// file as it is now rather than a snapshot, which is what a real getpwent does.
//
// The state is file-scope, so two native programs walking at once would share a
// position -- the same sharing already noted for nlibc_std and the passwd cache.
struct nlibc_ent_key { size_t want, seen; };

static bool nlibc_pw_ent_match(char **f, size_t n, const void *keyv) {
    struct nlibc_ent_key *key = (struct nlibc_ent_key *) keyv;
    if (n < 7)
        return false;
    if (key->seen++ != key->want)
        return false;
    nlibc_pw.pw_name = f[0];
    nlibc_pw.pw_passwd = f[1];
    nlibc_pw.pw_uid = (uid_t) strtoul(f[2], NULL, 10);
    nlibc_pw.pw_gid = (gid_t) strtoul(f[3], NULL, 10);
    nlibc_pw.pw_gecos = f[4];
    nlibc_pw.pw_dir = f[5];
    nlibc_pw.pw_shell = f[6];
    return true;
}

static size_t nlibc_pw_pos;

void nlibc_setpwent(void) { nlibc_pw_pos = 0; }
void nlibc_endpwent(void) { nlibc_pw_pos = 0; }

struct passwd *nlibc_getpwent(void) {
    struct nlibc_ent_key key = { nlibc_pw_pos, 0 };
    memset(&nlibc_pw, 0, sizeof(nlibc_pw));
    if (!nlibc_scan_db("/etc/passwd", nlibc_pw_line, sizeof(nlibc_pw_line),
                       nlibc_pw_ent_match, &key))
        return NULL;
    nlibc_pw_pos++;
    return &nlibc_pw;
}

static bool nlibc_gr_ent_match(char **f, size_t n, const void *keyv) {
    struct nlibc_ent_key *key = (struct nlibc_ent_key *) keyv;
    if (n < 3)
        return false;
    if (key->seen++ != key->want)
        return false;
    nlibc_gr_members[0] = NULL;
    nlibc_gr.gr_name = f[0];
    nlibc_gr.gr_passwd = f[1];
    nlibc_gr.gr_gid = (gid_t) strtoul(f[2], NULL, 10);
    nlibc_gr.gr_mem = nlibc_gr_members;
    return true;
}

static size_t nlibc_gr_pos;

void nlibc_setgrent(void) { nlibc_gr_pos = 0; }
void nlibc_endgrent(void) { nlibc_gr_pos = 0; }

struct group *nlibc_getgrent(void) {
    struct nlibc_ent_key key = { nlibc_gr_pos, 0 };
    memset(&nlibc_gr, 0, sizeof(nlibc_gr));
    if (!nlibc_scan_db("/etc/group", nlibc_gr_line, sizeof(nlibc_gr_line),
                       nlibc_gr_ent_match, &key))
        return NULL;
    nlibc_gr_pos++;
    return &nlibc_gr;
}

// ------------------------------------------------------------------ execve
//
// execv with the environment given explicitly rather than taken from the task.
// Same spawn-then-exit shape, and the same caveat: the pid changes.
int nlibc_execve(const char *path, char *const argv[], char *const envp[]) {
    if (path == NULL || argv == NULL)
        return nlibc_fail(_EFAULT);
    dword_t pid = 0;
    int err = native_spawn(path, argv, envp != NULL ? envp : native_env_vector(),
            &pid);
    if (err < 0)
        return nlibc_fail(err);
    int status = 0;
    if (native_waitpid(pid, &status, 0) < 0)
        nlibc_exit(127);
    nlibc_exit(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));
}

// ------------------------------------------------------------------ pselect
//
// select with a timespec and a signal mask. nlibc_select already goes through
// pselect6 because that is the only one the guest has; this is the same call
// with the two arguments select cannot express.
int nlibc_pselect(int nfds, void *readfds, void *writefds, void *errorfds,
        const struct timespec *timeout, const sigset_t *sigmask) {
    NATIVE_FRAME;
    if (nfds < 0)
        return nlibc_fail(_EINVAL);
    size_t size = ((size_t) nfds + 7) / 8;
    size = (size + 7) & ~(size_t) 7;

    guest_addr_t guest_sets[3] = {0, 0, 0};
    void *host_sets[3] = { readfds, writefds, errorfds };
    for (int i = 0; i < 3; i++) {
        if (host_sets[i] == NULL)
            continue;
        guest_sets[i] = native_scratch_put(host_sets[i], size);
        if (guest_sets[i] == 0)
            return nlibc_fail(_ENOMEM);
    }

    guest_addr_t guest_ts = 0;
    if (timeout != NULL) {
        struct nlibc_guest_timespec ts = {
            .sec = timeout->tv_sec, .nsec = timeout->tv_nsec,
        };
        guest_ts = native_scratch_put(&ts, sizeof(ts));
        if (guest_ts == 0)
            return nlibc_fail(_ENOMEM);
    }

    // pselect6's sixth argument is not the mask but a {mask pointer, size}
    // pair -- the syscall ran out of registers, and getting this wrong means
    // the kernel reads a pointer out of a length.
    struct { qword_t mask; qword_t size; } sigarg = { 0, sizeof(sigset_t_) };
    guest_addr_t guest_sig = 0;
    if (sigmask != NULL) {
        sigset_t_ guest_mask = nlibc_sigset_to_guest(sigmask);
        guest_addr_t guest_maskp = native_scratch_put(&guest_mask, sizeof(guest_mask));
        if (guest_maskp == 0)
            return nlibc_fail(_ENOMEM);
        sigarg.mask = guest_maskp;
        guest_sig = native_scratch_put(&sigarg, sizeof(sigarg));
        if (guest_sig == 0)
            return nlibc_fail(_ENOMEM);
    }

    sqword_t res = native_syscall(NATIVE_SYS_pselect6, nfds, guest_sets[0],
            guest_sets[1], guest_sets[2], guest_ts, guest_sig);
    if (res < 0)
        return nlibc_fail((int) res);
    for (int i = 0; i < 3; i++)
        if (guest_sets[i] != 0 && native_scratch_get(host_sets[i], guest_sets[i], size) < 0)
            return nlibc_fail(_EFAULT);
    return (int) res;
}

// ============================================================== posix_spawn
//
// The general answer to "a native program cannot fork". Everything written
// since about 2005 that wants to start a child can say so with posix_spawn,
// and a program that does needs NO patch to run inside AOK -- which is the
// point. SmallCLUE carries a spawn seam and Nextvi had one added upstream
// precisely because they call fork() directly; anything using posix_spawn is
// spared both.
//
// This can be a translation rather than an interpretation because
// native_spawn_opts takes the same ordered action list posix_spawn does.
//
// The file-actions and attributes objects are OURS. Darwin declares both as
// `void *` (spawn.h), so redirecting the whole family means the caller's
// posix_spawn_file_actions_t holds a pointer to the struct below and never to
// anything the host libc would recognise -- which is what makes this safe.
// Mixing them would not be: a host posix_spawn handed one of these would
// dereference it as its own.

// The whole scheme rests on Darwin declaring these as plain pointers, so that
// what the caller holds is ours to define. If that ever stopped being true, the
// silent failure would be a host posix_spawn dereferencing one of our structs
// as its own -- so it is asserted rather than trusted.
_Static_assert(sizeof(posix_spawn_file_actions_t) == sizeof(void *),
        "posix_spawn_file_actions_t is not a plain pointer on this platform");
_Static_assert(sizeof(posix_spawnattr_t) == sizeof(void *),
        "posix_spawnattr_t is not a plain pointer on this platform");

struct nlibc_spawn_actions {
    struct native_spawn_action *actions;
    size_t count, cap;
    char **paths;          // OPEN copies the path; freed with the object
    size_t path_count, path_cap;
};

struct nlibc_spawn_attr {
    short flags;
    pid_t pgroup;
};

int nlibc_posix_spawn_file_actions_init(void **fa) {
    if (fa == NULL)
        return EINVAL;
    struct nlibc_spawn_actions *a = calloc(1, sizeof(*a));
    if (a == NULL)
        return ENOMEM;
    *fa = a;
    return 0;
}

int nlibc_posix_spawn_file_actions_destroy(void **fa) {
    if (fa == NULL || *fa == NULL)
        return EINVAL;
    struct nlibc_spawn_actions *a = *fa;
    for (size_t i = 0; i < a->path_count; i++)
        free(a->paths[i]);
    free(a->paths);
    free(a->actions);
    free(a);
    *fa = NULL;
    return 0;
}

static int nlibc_spawn_actions_push(void **fa,
        struct native_spawn_action action) {
    if (fa == NULL || *fa == NULL)
        return EINVAL;
    struct nlibc_spawn_actions *a = *fa;
    if (a->count == a->cap) {
        size_t cap = a->cap == 0 ? 8 : a->cap * 2;
        struct native_spawn_action *grown = realloc(a->actions, cap * sizeof(*grown));
        if (grown == NULL)
            return ENOMEM;
        a->actions = grown;
        a->cap = cap;
    }
    a->actions[a->count++] = action;
    return 0;
}

int nlibc_posix_spawn_file_actions_adddup2(void **fa,
        int from, int to) {
    if (from < 0 || to < 0)
        return EBADF;
    return nlibc_spawn_actions_push(fa, (struct native_spawn_action) {
        .kind = NATIVE_SPAWN_DUP2, .fd = to, .from = from });
}

int nlibc_posix_spawn_file_actions_addclose(void **fa, int fd) {
    if (fd < 0)
        return EBADF;
    return nlibc_spawn_actions_push(fa, (struct native_spawn_action) {
        .kind = NATIVE_SPAWN_CLOSE, .fd = fd });
}

int nlibc_posix_spawn_file_actions_addopen(void **fa,
        int fd, const char *path, int flags, mode_t mode) {
    if (fd < 0)
        return EBADF;
    if (fa == NULL || *fa == NULL || path == NULL)
        return EINVAL;
    // The caller may free the path the moment this returns, and the action is
    // not applied until the spawn, so it has to be copied.
    struct nlibc_spawn_actions *a = *fa;
    if (a->path_count == a->path_cap) {
        size_t cap = a->path_cap == 0 ? 4 : a->path_cap * 2;
        char **grown = realloc(a->paths, cap * sizeof(*grown));
        if (grown == NULL)
            return ENOMEM;
        a->paths = grown;
        a->path_cap = cap;
    }
    char *copy = strdup(path);
    if (copy == NULL)
        return ENOMEM;
    a->paths[a->path_count++] = copy;
    // Guest O_* values, since the action is performed against the guest's VFS.
    return nlibc_spawn_actions_push(fa, (struct native_spawn_action) {
        .kind = NATIVE_SPAWN_OPEN, .fd = fd, .path = copy,
        .flags = (int) nlibc_open_flags_to_guest(flags), .mode = (int) mode });
}

int nlibc_posix_spawnattr_init(void **attr) {
    if (attr == NULL)
        return EINVAL;
    struct nlibc_spawn_attr *a = calloc(1, sizeof(*a));
    if (a == NULL)
        return ENOMEM;
    *attr = a;
    return 0;
}

int nlibc_posix_spawnattr_destroy(void **attr) {
    if (attr == NULL || *attr == NULL)
        return EINVAL;
    free(*attr);
    *attr = NULL;
    return 0;
}

int nlibc_posix_spawnattr_setflags(void **attr, short flags) {
    if (attr == NULL || *attr == NULL)
        return EINVAL;
    ((struct nlibc_spawn_attr *) *attr)->flags = flags;
    return 0;
}

int nlibc_posix_spawnattr_getflags(void **attr, short *out) {
    if (attr == NULL || *attr == NULL || out == NULL)
        return EINVAL;
    *out = ((struct nlibc_spawn_attr *) *attr)->flags;
    return 0;
}

int nlibc_posix_spawnattr_setpgroup(void **attr, pid_t pgroup) {
    if (attr == NULL || *attr == NULL)
        return EINVAL;
    ((struct nlibc_spawn_attr *) *attr)->pgroup = pgroup;
    return 0;
}

int nlibc_posix_spawnattr_getpgroup(void **attr, pid_t *out) {
    if (attr == NULL || *attr == NULL || out == NULL)
        return EINVAL;
    *out = ((struct nlibc_spawn_attr *) *attr)->pgroup;
    return 0;
}

// The signal attributes are accepted and ignored, deliberately and narrowly:
// a native program's signal dispositions live in the shim rather than in the
// kernel (see the block comment above nlibc_sigaction), so there is nothing in
// the CHILD for them to reset -- the child is a fresh task with default
// dispositions already, which is what POSIX_SPAWN_SETSIGDEF asks for. Refusing
// would break callers that set them as a matter of routine and would gain
// nothing.
int nlibc_posix_spawnattr_setsigdefault(void **attr, const sigset_t *set) {
    (void) set;
    return (attr != NULL && *attr != NULL) ? 0 : EINVAL;
}
int nlibc_posix_spawnattr_setsigmask(void **attr, const sigset_t *set) {
    (void) set;
    return (attr != NULL && *attr != NULL) ? 0 : EINVAL;
}

static int nlibc_posix_spawn_common(pid_t *pid_out, const char *file,
        const void **fa, const void **attr,
        char *const argv[], char *const envp[], bool search_path) {
    if (file == NULL || argv == NULL)
        return EINVAL;

    struct native_spawn_opts opts = { .pgid = NATIVE_SPAWN_PGID_INHERIT };
    if (attr != NULL && *attr != NULL) {
        const struct nlibc_spawn_attr *a = *attr;
        // Only SETPGROUP changes what the child IS; the rest are either
        // already true of a fresh task or have no meaning here.
        if (a->flags & POSIX_SPAWN_SETPGROUP)
            opts.pgid = a->pgroup;
    }
    if (fa != NULL && *fa != NULL) {
        const struct nlibc_spawn_actions *a = *fa;
        opts.actions = a->actions;
        opts.action_count = a->count;
    }

    char resolved[MAX_PATH];
    const char *path = file;
    if (search_path && strchr(path, '/') == NULL) {
        // The GUEST's PATH, as with execvp.
        if (native_path_search(path, resolved, sizeof(resolved)) < 0)
            return ENOENT;
        path = resolved;
    }

    dword_t pid = 0;
    int err = native_spawn_opts(path, argv,
            envp != NULL ? envp : native_env_vector(), &opts, &pid);
    if (err < 0)
        return nlibc_host_errno(err);   // posix_spawn RETURNS the error; it
                                        // does not set errno
    if (pid_out != NULL)
        *pid_out = (pid_t) pid;
    return 0;
}

int nlibc_posix_spawn(pid_t *pid, const char *path,
        const void **fa, const void **attr,
        char *const argv[], char *const envp[]) {
    return nlibc_posix_spawn_common(pid, path, fa, attr, argv, envp, false);
}

int nlibc_posix_spawnp(pid_t *pid, const char *file,
        const void **fa, const void **attr,
        char *const argv[], char *const envp[]) {
    return nlibc_posix_spawn_common(pid, file, fa, attr, argv, envp, true);
}
