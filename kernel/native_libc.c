// Implementation of the SmallCLUE libc shim. See kernel/native_libc.h for
// why every one of these exists.
//
// This file must NOT be compiled with the shim force-included -- it is the
// thing the shim redirects *to*, and needs the real libc.

#define NATIVE_LIBC_NO_REDIRECT

#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#if !defined(__linux__)
#include <sys/sysctl.h>   // BSD-only; glibc dropped it and musl never had it
#endif
#include <syslog.h>
#include <spawn.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <utmpx.h>

#include "kernel/calls.h"
#include "platform/platform.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/native.h"
#include "kernel/native_io.h"
#include "kernel/native_kqueue.h"
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

        // The socket errnos. Every one of these used to land on the default
        // and come out as EINVAL, which is how a perfectly ordinary
        // non-blocking connect() -- EINPROGRESS, the expected answer, not an
        // error -- turned into "ssh: connect to host ...: Invalid argument"
        // and stopped ssh dead before it sent a byte. A refused connection, an
        // unreachable host and a timeout were all the same "Invalid argument"
        // too. Linux numbers on the left, host names on the right; they
        // disagree from 41 up, so nothing here can be passed through.
        case 42: return ENOMSG;
        case 43: return EIDRM;
        case 71: return EPROTO;
        case 74: return EBADMSG;
        case 75: return EOVERFLOW;
        case 84: return EILSEQ;
        case 87: return EUSERS;
        case 88: return ENOTSOCK;
        case 89: return EDESTADDRREQ;
        case 90: return EMSGSIZE;
        case 91: return EPROTOTYPE;
        case 92: return ENOPROTOOPT;
        case 93: return EPROTONOSUPPORT;
        case 94: return ESOCKTNOSUPPORT;
        case 95: return EOPNOTSUPP;
        case 96: return EPFNOSUPPORT;
        case 97: return EAFNOSUPPORT;
        case 98: return EADDRINUSE;
        case 99: return EADDRNOTAVAIL;
        case 100: return ENETDOWN;
        case 101: return ENETUNREACH;
        case 102: return ENETRESET;
        case 103: return ECONNABORTED;
        case 104: return ECONNRESET;
        case 105: return ENOBUFS;
        case 106: return EISCONN;
        case 107: return ENOTCONN;
        case 108: return ESHUTDOWN;
        case 109: return ETOOMANYREFS;
        case 110: return ETIMEDOUT;
        case 111: return ECONNREFUSED;
        case 112: return EHOSTDOWN;
        case 113: return EHOSTUNREACH;
        case 114: return EALREADY;
        case 115: return EINPROGRESS;
        case 116: return ESTALE;
        case 122: return EDQUOT;
        case 125: return ECANCELED;

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

// close() without the kqueue bookkeeping, for the bookkeeping itself.
int nlibc_close_raw(int fd_no) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_close, fd_no));
}

int nlibc_close(int fd_no) {
    // A kqueue descriptor is a pipe end with a table behind it, and the table
    // has to go first -- otherwise the number is recycled by the guest while
    // this side still believes it names a queue, and the next kqueue() to be
    // handed the same number inherits someone else's registrations. The hook
    // closes both pipe ends itself, so there is nothing left to do here.
    if (nlibc_kqueue_close_hook(fd_no))
        return 0;
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

// Darwin's AT_SYMLINK_NOFOLLOW is 0x0020 and the guest's is 0x100, so the flag
// cannot be passed through: the raw 0x20 is the guest's AT_EACCESS bit for a
// different call entirely. Nothing else in Darwin's AT_ set has a meaning here
// -- AT_SYMLINK_FOLLOW is the default, and there is no AT_EMPTY_PATH on Darwin
// for a caller to have passed.
int nlibc_fstatat(int dirfd, const char *path, struct stat *st, int flags) {
    dword_t guest_flags = (flags & AT_SYMLINK_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW_ : 0;
    return nlibc_statat(dirfd, path, st, guest_flags);
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

// fdopendir and readdir_r, the two of the directory family that were missing.
//
// They were not missed while the shim only served code AOK compiles, because
// nothing here calls them. They matter now that a foreign toolchain's objects
// are routed by symbol rewriting (tools/gen-nlibc-renames.py): Rust's
// std::fs::read_dir uses fdopendir and readdir_r, so on a build without these
// its directory listing went to the HOST -- against a guest fd, which is the
// exact silent-wrongness the shim exists to prevent. It showed up as an empty
// listing rather than an error, which is the worst way for it to show up.
DIR *nlibc_fdopendir(int fd) {
    // Takes ownership of fd, as the real fdopendir does: closedir closes it.
    if (fd < 0) {
        errno = EBADF;
        return NULL;
    }
    struct nlibc_dir *dir = calloc(1, sizeof(*dir));
    if (dir == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    dir->fd = fd;
    return (DIR *) dir;
}

// The reentrant readdir. POSIX deprecated it and Rust still uses it on Darwin,
// so it is here rather than argued with. Contract: *result is the entry on
// success and NULL at end of directory, and the return value is an errno (0 on
// success), NOT -1 -- getting that backwards makes every caller see an
// immediate end of directory.
int nlibc_readdir_r(DIR *handle, struct dirent *entry, struct dirent **result) {
    if (handle == NULL || entry == NULL || result == NULL)
        return EINVAL;
    errno = 0;
    struct dirent *found = nlibc_readdir(handle);
    if (found == NULL) {
        *result = NULL;
        return errno;   // 0 at end of directory, the error otherwise
    }
    memcpy(entry, found, sizeof(*entry));
    *result = entry;
    return 0;
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
    // d_namlen is a Darwin field with no Linux counterpart, so nothing in the
    // getdents64 record above supplies it and the memset left it zero. That is
    // not a cosmetic omission: it is a field the HOST's struct dirent declares,
    // so host code is entitled to trust it, and portable code reaches it
    // through configure. bash's glob does exactly that --
    // HAVE_STRUCT_DIRENT_D_NAMLEN, then `bcopy (dp->d_name, nextname,
    // D_NAMLEN (dp) + 1)` -- so every glob returned one character per file:
    // `echo *.txt` printed "f f" for f1.txt and f2.txt. Fill in every field of
    // the host's structure, not only the ones the guest's record names.
#if !defined(__linux__)
    // BSD/Darwin only: glibc's struct dirent has no d_namlen, and its D_NAMLEN
    // falls back to strlen(d_name), which is already correct.
    dir->ent.d_namlen = (__uint16_t) strlen(dir->ent.d_name);
#endif
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

// The descriptor a DIR was opened over. This is the fileno() case again, and
// the reason it cannot be waved through as "pure": a native program's DIR is
// the struct above, not a host DIR, so the host's dirfd() would read whatever
// lies at its own layout's offset -- here, the fd field only by accident, and
// nothing at all if either struct ever changes.
int nlibc_dirfd(DIR *handle) {
    struct nlibc_dir *dir = (struct nlibc_dir *) handle;
    if (dir == NULL)
        return nlibc_fail(_EBADF);
    return dir->fd;
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
// Darwin's AT_REMOVEDIR is 0x80 where the guest's is 0x200, so this is another
// flag that must be translated rather than forwarded.
int nlibc_unlinkat(int dirfd, const char *path, int flags) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    dword_t guest_flags = (flags & AT_REMOVEDIR) ? NLIBC_AT_REMOVEDIR : 0;
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_unlinkat, nlibc_at_fd(dirfd),
            guest_path, guest_flags));
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

// ------------------------------------------------------ the *at family
//
// The forms that take a directory descriptor, which the bare ones above are
// already implemented in terms of -- every one of these guest syscalls takes
// an at-fd, and AT_FDCWD_ was simply being passed for it. So these are the
// same calls with nlibc_at_fd(dirfd) where the constant was.
//
// Reached by Rust rather than by the shells: std and rustix prefer the *at
// forms because they are the ones without a race between resolving a path and
// acting on it. Unrouted they were resolving against the HOST's directories.

int nlibc_mkdirat(int dirfd, const char *path, mode_t mode) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_mkdirat, nlibc_at_fd(dirfd),
            guest_path, mode));
}

int nlibc_symlinkat(const char *target, int dirfd, const char *linkpath) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_target, target);
    NLIBC_PATH(guest_link, linkpath);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_symlinkat, guest_target,
            nlibc_at_fd(dirfd), guest_link));
}

ssize_t nlibc_readlinkat(int dirfd, const char *path, char *buf, size_t bufsize) {
    NATIVE_FRAME;
    if (buf == NULL)
        return nlibc_fail(_EFAULT);
    NLIBC_PATH(guest_path, path);
    guest_addr_t guest_buf = native_scratch_alloc(bufsize);
    if (guest_buf == 0 && bufsize > 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_readlinkat, nlibc_at_fd(dirfd),
            guest_path, guest_buf, bufsize);
    if (res > 0 && native_scratch_get(buf, guest_buf, (size_t) res) < 0)
        return nlibc_fail(_EFAULT);
    return nlibc_ret(res);
}

// AT_SYMLINK_NOFOLLOW is 0x0020 on Darwin and 0x100 on the guest, the same
// disagreement fstatat has to translate.
int nlibc_fchmodat(int dirfd, const char *path, mode_t mode, int flags) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    dword_t guest_flags = (flags & AT_SYMLINK_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW_ : 0;
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_fchmodat, nlibc_at_fd(dirfd),
            guest_path, mode, guest_flags));
}

int nlibc_fchownat(int dirfd, const char *path, uid_t uid, gid_t gid, int flags) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_path, path);
    dword_t guest_flags = (flags & AT_SYMLINK_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW_ : 0;
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_fchownat, nlibc_at_fd(dirfd),
            guest_path, uid, gid, guest_flags));
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
//
// While one of these callbacks is on the stack, host stdio HOLDS the FILE's
// lock -- fwrite/getdelim lock the stream for their whole call, the blocking
// I/O included. Dying here (receive_signals' default action for a fatal
// signal does not return) abandons that lock, and Darwin never releases a
// mutex whose owner thread is gone: one native program killed mid-write --
// SIGPIPE from `yes | head -1` was the reproducer -- left its stdout wrapper
// locked forever, and every later native program's first stdio read hung in
// __srefill's _fwalk(lflush) walking onto it. That is the terminal-hangs-
// before-prompt wedge, the second instance of the 1d8eaae0d class.
//
// So the callbacks bracket themselves with a depth marker, and
// native_checkpoint defers fatal delivery (and handler delivery, which can
// longjmp out of host frames the same way) while it is set: the callback
// instead fails with the error the interrupted syscall produced, stdio
// returns through its own unlock, and the program dies at its next signal
// checkpoint outside stdio -- usually its own clean exit(), which also
// flushes and closes its streams properly. nlibc_stdio_defer_fatal() carries
// a give-up limit so a pathological program looping on stdio errors without
// ever leaving stdio still dies (leaking its lock, which fully-buffered
// native stdin -- see nlibc_std_stream -- has made harmless to bystanders).
static __thread int nlibc_stdio_depth;
static __thread unsigned nlibc_stdio_deferred;
#define NLIBC_STDIO_DEFER_LIMIT 1000

bool nlibc_stdio_defer_fatal(void) {
    if (nlibc_stdio_depth <= 0) {
        nlibc_stdio_deferred = 0; // safe point: death releases nothing owned
        return false;
    }
    if (++nlibc_stdio_deferred > NLIBC_STDIO_DEFER_LIMIT)
        return false;
    return true;
}

static int nlibc_file_read(void *cookie, char *buf, int n) {
    nlibc_stdio_depth++;
    ssize_t res = nlibc_read((int) (intptr_t) cookie, buf, (size_t) n);
    nlibc_stdio_depth--;
    return (int) res;
}
static int nlibc_file_write(void *cookie, const char *buf, int n) {
    nlibc_stdio_depth++;
    ssize_t res = nlibc_write((int) (intptr_t) cookie, buf, (size_t) n);
    nlibc_stdio_depth--;
    return (int) res;
}
#if !defined(__linux__)
// fpos_t is an integer on Darwin and an opaque struct on glibc, so this
// BSD-shaped hook only exists where funopen does.
static fpos_t nlibc_file_seek(void *cookie, fpos_t off, int whence) {
    nlibc_stdio_depth++;
    fpos_t res = nlibc_lseek((int) (intptr_t) cookie, (off_t) off, whence);
    nlibc_stdio_depth--;
    return res;
}
#endif
#if defined(__linux__)
// glibc has no funopen. fopencookie is the same idea with a different shape:
// the read/write hooks take and return ssize_t rather than int, and seek is
// handed a pointer to an off64_t that it must UPDATE, returning 0 or -1, where
// BSD's returns the new offset. Adapters rather than rewritten callbacks, so
// the Darwin path -- the one that ships -- keeps using its own hooks unchanged.
static ssize_t nlibc_cookie_read(void *cookie, char *buf, size_t n) {
    nlibc_stdio_depth++;
    ssize_t res = (ssize_t) nlibc_read((int) (intptr_t) cookie, buf, n);
    nlibc_stdio_depth--;
    return res;
}
static ssize_t nlibc_cookie_write(void *cookie, const char *buf, size_t n) {
    nlibc_stdio_depth++;
    ssize_t res = (ssize_t) nlibc_write((int) (intptr_t) cookie, buf, n);
    nlibc_stdio_depth--;
    return res;
}
static int nlibc_cookie_seek(void *cookie, __off64_t *off, int whence) {
    nlibc_stdio_depth++;
    off_t res = nlibc_lseek((int) (intptr_t) cookie, (off_t) *off, whence);
    nlibc_stdio_depth--;
    if (res < 0)
        return -1;
    *off = (__off64_t) res;
    return 0;
}
#endif

static int nlibc_file_close(void *cookie) {
    return nlibc_close((int) (intptr_t) cookie);
}
// The standard streams must not close the guest's fd 0/1/2 when the FILE is
// closed or the program exits.
static int nlibc_file_close_noop(void *cookie) { (void) cookie; return 0; }

// Which guest fd a stream was built over, so fileno() can answer.
//
// This is not a nicety. A funopen stream has NO underlying descriptor, so the
// host's fileno() returns -1 and sets EBADF -- and bash decides whether it is
// interactive with `isatty(fileno(stdin)) && isatty(fileno(stderr))`. Both
// became isatty(-1), so an interactive native bash concluded it was not one,
// read its input as a script, hit EOF and exited immediately. The stray EBADF
// is also what turned up in an unrelated message ("setlocale: ... Bad file
// descriptor"), because errno was still set from it.
//
// fileno was on check-native-libc.py's PURE list, described as safe because
// every FILE* a native program holds is one the shim made. That was the wrong
// conclusion from a true premise: being ours is exactly WHY fileno cannot be
// left to the host -- the answer is a property of where the stream came from.
// owner: which thread built the stream. A native program is a C function on a
// guest task's thread, so "this program's streams" is exactly "this thread's"
// -- the same reasoning that makes nlibc_std thread-local. fflush(NULL) needs
// it to flush the caller's streams instead of every native program's at once.
struct nlibc_stream { struct nlibc_stream *next; FILE *file; int fd; pthread_t owner; };
static struct nlibc_stream *nlibc_streams;
static pthread_mutex_t nlibc_stream_lock = PTHREAD_MUTEX_INITIALIZER;

static void nlibc_stream_remember(FILE *file, int fd) {
    struct nlibc_stream *s = malloc(sizeof(*s));
    if (s == NULL)
        return;   // fileno degrades to -1 for this stream; nothing else breaks
    s->file = file;
    s->fd = fd;
    s->owner = pthread_self();
    pthread_mutex_lock(&nlibc_stream_lock);
    s->next = nlibc_streams;
    nlibc_streams = s;
    pthread_mutex_unlock(&nlibc_stream_lock);
}

// Drop a stream from the registry. The list is keyed by FILE* address, so an
// entry left behind after the FILE is freed can be matched by a LATER stream
// that malloc happens to place at the same address -- and then fileno() answers
// with the dead stream's descriptor.
static void nlibc_stream_forget(FILE *file) {
    pthread_mutex_lock(&nlibc_stream_lock);
    struct nlibc_stream **link = &nlibc_streams;
    while (*link != NULL) {
        if ((*link)->file == file) {
            struct nlibc_stream *dead = *link;
            *link = dead->next;
            free(dead);
            break;
        }
        link = &(*link)->next;
    }
    pthread_mutex_unlock(&nlibc_stream_lock);
}

int nlibc_fileno(FILE *file) {
    if (file == NULL)
        return nlibc_fail(_EBADF);
    pthread_mutex_lock(&nlibc_stream_lock);
    for (struct nlibc_stream *s = nlibc_streams; s != NULL; s = s->next)
        if (s->file == file) {
            int fd = s->fd;
            pthread_mutex_unlock(&nlibc_stream_lock);
            return fd;
        }
    pthread_mutex_unlock(&nlibc_stream_lock);
    // Not one of ours -- open_memstream, say. The host's answer is the right
    // one there, because the stream really is the host's.
    return fileno(file);
}

// Which way a stream goes. funopen() infers the direction from which callbacks
// it is handed, and getting that wrong is not cosmetic -- see below.
enum nlibc_stream_dir { NLIBC_RD, NLIBC_WR, NLIBC_RDWR };

// The direction a mode string asks for, the way fopen reads it: "r" reads, "w"
// and "a" write, and a '+' anywhere makes it both.
static enum nlibc_stream_dir nlibc_mode_dir(const char *mode) {
    if (mode == NULL || mode[0] == '\0')
        return NLIBC_RDWR;    // no claim made; assume the permissive thing
    if (strchr(mode, '+') != NULL)
        return NLIBC_RDWR;
    return mode[0] == 'r' ? NLIBC_RD : NLIBC_WR;
}

// The direction MUST be the stream's real one, not a permissive "both".
//
// funopen() records it as the FILE's direction flag: a write callback alone
// yields __SWR, a read callback alone __SRD, and both together __SRW -- which
// means "read/write, direction not yet decided". For an __SRW stream stdio
// leaves __SWR clear until __swsetup() runs on the first write, and __sflush()
// -- what fflush() is -- opens with `if ((flags & __SWR) == 0) return 0`. A
// stream that stdio does not believe has been written to CANNOT BE FLUSHED.
//
// That is normally invisible, because everything that writes goes through
// __swsetup first. Everything except putc()'s inline fast path, which stores
// straight into the buffer whenever _w > 0 -- and _w is set to the buffer size,
// without __SWR, by fpurge() and by setvbuf(). So:
//
//   zsh's init.c    setvbuf(stdout, outbuf, _IOFBF, BUFSIZ)
//   zsh's redup()   fpurge(stdout) before dup2'ing a redirect onto fd 1
//                     -> _w = 1024, __SWR still clear
//   printf's        putc(c, fout) per literal character
//                     -> five bytes in the buffer, __SWR still clear
//   bin_print's     fflush(fout) -> __sflush -> no __SWR -> returns 0, writes
//                   NOTHING and reports success
//   zsh's redup()   fpurge(stdout) restoring fd 1 -> buffer discarded
//
// and `printf plain > file` wrote an empty file, silently, while `print plain >
// file` worked -- print reaches the buffer through fwrite(), which does call
// __swsetup. The bytes were not misdirected, they were dropped.
//
// Passing the real direction removes the whole failure mode rather than that
// one path through it: a shim stream now carries exactly the direction bits a
// host stream opened the same way carries, so __SWR is set from the start on
// everything opened for writing, exactly as it is on the host's own stdout.
static FILE *nlibc_file_wrap(int fd, int closes_fd, enum nlibc_stream_dir dir) {
#if defined(__linux__)
    cookie_io_functions_t hooks = {
        .read  = dir == NLIBC_WR ? NULL : nlibc_cookie_read,
        .write = dir == NLIBC_RD ? NULL : nlibc_cookie_write,
        .seek  = nlibc_cookie_seek,
        .close = closes_fd ? nlibc_file_close : nlibc_file_close_noop,
    };
    // The direction still has to be spelled out, for the reason above: a stream
    // opened for writing must carry the write bit from the start.
    FILE *f = fopencookie((void *) (intptr_t) fd,
            dir == NLIBC_RD ? "r" : (dir == NLIBC_WR ? "w" : "r+"), hooks);
#else
    FILE *f = funopen((void *) (intptr_t) fd,
            dir == NLIBC_WR ? NULL : nlibc_file_read,
            dir == NLIBC_RD ? NULL : nlibc_file_write,
            nlibc_file_seek, closes_fd ? nlibc_file_close : nlibc_file_close_noop);
#endif
    if (f == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    nlibc_stream_remember(f, fd);
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
    // 0666, and it MUST be passed: nlibc_open is variadic and reads the mode
    // with va_arg whenever O_CREAT is set, so calling it with two arguments
    // for "w" handed the guest whatever junk was in the third argument slot.
    // The symptom was not a permission oddity but a wrong FILE TYPE -- `dd
    // if=f of=g` created something stat reported as a socket and then failed
    // with ENXIO opening it -- and it was intermittent, because the junk
    // differed from call to call. 0666 is what glibc and musl's fopen pass;
    // the guest's umask narrows it from there.
    int fd = nlibc_open(path, nlibc_mode_to_flags(mode), 0666);
    if (fd < 0)
        return NULL;
    FILE *f = nlibc_file_wrap(fd, 1, nlibc_mode_dir(mode));
    if (f == NULL)
        nlibc_close(fd);
    return f;
}

// fclose has to drop the registry entry, or the entry outlives the FILE.
//
// The registry is keyed by FILE* address and libc hands the same FILE slot back
// out to a later fopen -- the hazard nlibc_stream_forget's own comment
// describes. Nothing was closing that loop: fclose was left to the host (it was
// on check-native-libc.py's PURE list, and closing a stream really does not
// reach the host), so every fopen/fclose pair a native program made left a
// stale entry behind, and only nlibc_flush_std's own teardown ever forgot one.
// A later stream landing on that address then inherited the dead entry's answer
// to fileno(), and now also its place in the flush walks below.
int nlibc_fclose(FILE *stream) {
    if (stream != NULL)
        nlibc_stream_forget(stream);
    return fclose(stream);
}

FILE *nlibc_freopen(const char *path, const char *mode, FILE *stream) {
    if (stream != NULL)
        nlibc_fclose(stream);
    return nlibc_fopen(path, mode);
}

// The mode is honoured, not ignored: it is what tells stdio which direction the
// stream runs in, and a stream told "both" when it is really one is the bug
// described at nlibc_file_wrap. zsh's `print -u2 ...` reaches here through
// fdopen(fd, "w"), which is exactly the case that has to come out write-only.
FILE *nlibc_fdopen(int fd, const char *mode) {
    return nlibc_file_wrap(fd, 1, nlibc_mode_dir(mode));
}

// One wrapper per standard stream, made on demand and kept, so repeated use
// does not leak a FILE each time and buffering behaves consistently.
//
// PER TASK, not per process. These wrap descriptor NUMBERS -- 0, 1, 2 -- and
// every native program has its own 0, 1 and 2 pointing at different things. As
// a process-wide cache the first native program to run created the wrappers and
// every later one inherited them, still buffering into the FIRST program's
// descriptors.
//
// A pipeline is where that becomes visible, because both halves are live at
// once: `cat file | less` between two native applets printed NOTHING, while
// either half paired with an emulated program worked perfectly. Same shape as
// bash's globals (docs/bash_native_plan.md) -- a native program is a C function
// on a guest task's thread, not a process, so anything that is "per program"
// has to be per thread.
static __thread FILE *nlibc_std[3];

static FILE *nlibc_std_stream(int fd) {
    if (nlibc_std[fd] == NULL) {
        // Direction as the host's own standard streams have it: stdin reads,
        // stdout and stderr write. Nothing here is read AND written, and saying
        // otherwise is what silently dropped redirected printf output (see
        // nlibc_file_wrap).
        nlibc_std[fd] = nlibc_file_wrap(fd, 0, fd == 0 ? NLIBC_RD : NLIBC_WR);
        if (nlibc_std[fd] != NULL) {
            // A funopen stream is fully buffered by default, and a native
            // program exits by returning rather than through exit(), so
            // nothing would ever flush it -- printf output simply vanished.
            // Match what a terminal program expects instead: stderr
            // unbuffered, stdout line-buffered. nlibc_flush_std() below still
            // catches the tail when stdout is a pipe.
            //
            // stdin stays FULLY buffered, deliberately diverging from a
            // terminal program's usual line-buffered stdin. Darwin's __srefill
            // flushes every line-buffered output stream in the PROCESS before
            // refilling a line-buffered or unbuffered input stream --
            // _fwalk(lflush), taking each stream's lock -- and in this process
            // "every stream" includes every OTHER native program's stdout,
            // plus any lock a killed program's dead thread still holds. A
            // line-buffered stdin is how one leaked lock hung every later
            // native reader at its first getdelim. Input buffering has no
            // line-vs-full semantic difference (a read fills what is
            // available either way); the only casualty is the implicit
            // flush-stdout-before-reading-stdin idiom, which cross-program is
            // exactly the coupling this exists to break.
            setvbuf(nlibc_std[fd], NULL,
                    fd == 2 ? _IONBF : fd == 0 ? _IOFBF : _IOLBF, BUFSIZ);
        }
    }
    return nlibc_std[fd];
}

// Called once a native program returns, before its task exits: a return from
// main() is not exit(), so the C runtime never flushes these for us.
void nlibc_flush_std(void) {
    for (int i = 0; i < 3; i++) {
        if (nlibc_std[i] == NULL)
            continue;
        fflush(nlibc_std[i]);
        // Safe to close: these were wrapped with close_fd = 0, so the FILE goes
        // away and the guest's descriptor does not. Dropped from the registry
        // and nulled so that a task which runs a second native program -- bash
        // re-execing itself is the one that does -- builds fresh wrappers
        // rather than writing into the previous program's buffers.
        FILE *dead = nlibc_std[i];
        nlibc_std[i] = NULL;
        nlibc_stream_forget(dead);
        fclose(dead);
    }
}

// Flush one stream if its lock can be had, and never wait for it.
//
// fflush(NULL) walks EVERY host stream and takes each stream's lock to do it.
// The shim's streams are host FILEs (nlibc_file_wrap), and a native program is
// host code running on a guest task's thread rather than a process -- so a task
// that dies before nlibc_flush_std() runs leaves its stdout and stderr live in
// libc's stream pool, and a task that dies INSIDE stdio leaves that stream's
// lock held. A Darwin mutex is not released when its owner thread goes away, so
// fflush(NULL) then waits on a thread that no longer exists, for ever.
//
// Two CLI processes were caught wedged exactly there -- 0% CPU, 5 hours and 23
// hours after their guest had finished -- blocked in
// _fwalk -> sflush_locked -> flockfile -> __psynch_mutexwait. In both, the
// stream being flushed was one of ours (_file = -1, _write = nlibc_file_write,
// cookie = guest fd 1) and the mutex's recorded owner tid was absent from the
// process's own live thread list.
//
// ftrylockfile() refuses an abandoned lock rather than blocking on it, and the
// FILE lock is recursive, so fflush()'s own flockfile inside the region is
// fine. A stream whose lock nobody can take has nothing recoverable in it.
void nlibc_flush_stream_if_lockable(FILE *file) {
    if (file == NULL)
        return;
    if (ftrylockfile(file) != 0)
        return;
    fflush(file);
    funlockfile(file);
}

// Every stream the shim still owns, best effort. This is what a caller wants
// instead of fflush(NULL) on the way out: the same streams get flushed, and a
// stream held by a departed thread is skipped rather than waited on.
// mine_only: just the calling thread's streams, which is one native program's.
static void nlibc_flush_registered(int mine_only) {
    // The registry lock can be abandoned by a dying thread the same way the
    // stream locks can, so it gets the same treatment.
    if (pthread_mutex_trylock(&nlibc_stream_lock) != 0)
        return;
    pthread_t self = pthread_self();
    size_t count = 0;
    for (struct nlibc_stream *s = nlibc_streams; s != NULL; s = s->next)
        if (!mine_only || pthread_equal(s->owner, self))
            count++;
    FILE **snapshot = count != 0 ? malloc(count * sizeof(*snapshot)) : NULL;
    size_t n = 0;
    if (snapshot != NULL)
        for (struct nlibc_stream *s = nlibc_streams; s != NULL && n < count; s = s->next)
            if (!mine_only || pthread_equal(s->owner, self))
                snapshot[n++] = s->file;
    pthread_mutex_unlock(&nlibc_stream_lock);

    // Deliberately outside the registry lock: a flush runs the stream's write
    // callback, which issues a guest write, and that must not happen while
    // holding a lock the rest of the shim needs to answer fileno().
    for (size_t i = 0; i < n; i++)
        nlibc_flush_stream_if_lockable(snapshot[i]);
    free(snapshot);
}

void nlibc_flush_all_streams(void) { nlibc_flush_registered(0); }
void nlibc_flush_thread_streams(void) { nlibc_flush_registered(1); }

// fflush() -- and specifically fflush(NULL), which is a different function.
//
// fflush(f) really is pure with respect to the host: f is always a stream the
// shim built over a guest fd, and check-native-libc.py said so. fflush(NULL) is
// not that operation at all. It flushes EVERY stream in the process, which here
// means every OTHER concurrently-running native program's stdout and stderr,
// plus AOK's own -- a native program is a function on a task's thread, not a
// process, so "all streams" is not "mine". Same shape as the fileno and getopt
// mistakes above: the premise (every stream is ours) was right and the
// conclusion backwards.
//
// It is also the deadlock in cli_halt, reachable from a native program instead
// of from shutdown: fflush(NULL) takes each stream's lock, and a stream whose
// lock a departed task's thread still holds never gives it up. smallclue's
// shell alone calls fflush(NULL) 20 times, several of them around fork and
// pipeline teardown -- exactly where a sibling task is dying.
//
// So NULL means "this program's streams", flushed without waiting on any of
// them; a real stream keeps the host's behaviour it always had.
int nlibc_fflush(FILE *file) {
    if (file != NULL)
        return fflush(file);
    nlibc_flush_thread_streams();
    return 0;
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

// Defined beside the signal bookkeeping further down; declared here because
// exec is the first caller in this file.
static void nlibc_spawn_default_sigmask(struct native_spawn_opts *opts);
static void nlibc_exec_reset_handlers(void);

// The wait that stands in for "this process became that program". Getting it
// wrong is not a corner case: `zsh -c 'sleep 5'` execs in place, so the pid a
// shell knows for that job IS this wait, and every signal aimed at the job
// lands here rather than on sleep.
//
// Two things have to hold, and neither did.
//
// A signal must not END the wait. native_waitpid reports EINTR when one
// arrives, and reading that as "the child is gone" exited the stand-in with
// 127 -- a shell's "no such job" status, out of a process the shell had just
// killed. `sleep 5 & p=$!; kill -TERM $p; wait $p` answered 127 where every
// other shell answers 143, for SIGKILL just the same, because the stand-in
// died of its own bookkeeping before the signal was ever acted on.
//
// And the signal has to reach the program that replaced us, which nothing else
// will ever signal: the sender aimed at this pid, and after a real exec this
// pid would BE that program. So forward it, then checkpoint -- which, for a
// default-fatal signal, does not return, and that is exactly right. The
// stand-in dies of the same signal the job was killed with, so the parent's
// wait status says "killed by SIGTERM" instead of "exited 127".
//
// SIGCHLD is the one signal not forwarded: the only child this task has is the
// exec'd program itself, so a SIGCHLD here is news ABOUT it, never news FOR it.
//
// sys_kill rather than the shim's own nlibc_kill, and that is not a shortcut:
// every native_syscall CHECKPOINTS on the way in (kernel/native_syscall.c), and
// the signal being forwarded is by definition pending and fatal, so the shim
// route dies inside its own argument evaluation and the child is never
// signalled at all. Measured: `kill -TERM` on a native shell's job left the
// sleep it had exec'd running to full term, reparented to init.
//
// The forward is a best effort, not a guarantee, and the gap is not here: a
// task spawned by a native program INTERMITTENTLY never acts on a pending
// fatal signal at all, SIGKILL included, whoever sends it. That reproduces
// with no exec stand-in in the path and it predates this function, so what is
// queued here is right and what happens to it afterwards is somebody else's
// bug.
static void nlibc_exec_forward_signals(dword_t child) {
    sigset_t_ pending = __atomic_load_n(&current->pending, __ATOMIC_ACQUIRE) |
            __atomic_load_n(&current->sighand->pending, __ATOMIC_ACQUIRE);
    pending &= ~((sigset_t_) 1 << (SIGCHLD_ - 1));
    for (int sig = 1; sig < NUM_SIGS && pending != 0; sig++) {
        sigset_t_ bit = (sigset_t_) 1 << (sig - 1);
        if (!(pending & bit))
            continue;
        pending &= ~bit;
        sys_kill((pid_t_) child, (dword_t) sig);
    }
}

// Exit as the exec'd program did. The guest's wait status word already encodes
// both endings -- code << 8 for an exit, the bare signal number (plus the core
// bit) for a death -- and it is the same word do_exit_group takes, so handing
// it straight back keeps WIFSIGNALED true where it should be. Exiting 128 + N
// instead, as this used to, prints the same number for `$?` and then lies to
// everything that asks HOW the job ended: `jobs`, the "Terminated" line, and
// any script testing the status word itself.
static noreturn void nlibc_exec_standin(dword_t child) {
    nlibc_exec_reset_handlers();
    for (;;) {
        int status = 0;
        int res = native_waitpid(child, &status, 0);
        if (res >= 0) {
            nlibc_flush_std();
            do_exit_group(status);
        }
        if (res != _EINTR)
            nlibc_exit(127);        // the child really is unreachable
        nlibc_exec_forward_signals(child);
        native_checkpoint();        // may not return, and that is the point
    }
}

// Wait for a child this shim started on the program's behalf, and do not let a
// signal be mistaken for the answer.
//
// A signal is not an answer to that question. task_wait_child returns _EINTR
// whenever a signal becomes deliverable while it blocks -- and the callers
// below treated any negative return as "the exec failed", exiting 127 without
// ever collecting the child's status. The child is running perfectly well at
// that point, so the shell got a "command not found" for a command that was
// still executing, and the command's output turned up afterwards, out of order,
// attributed to nothing.
//
// It took a very ordinary line to hit: `wc -c < <(echo hi)` in native zsh
// reported 127 while printing 3, and `/bin/sleep 2 < <(print x)` came back in
// zero seconds. The interrupting signal is SIGCHLD from the process
// substitution's own child, which exits while the consumer is still being
// waited for -- so the bug needed a second child, which is why plain external
// commands never showed it.
//
// EINTR therefore means "look for signals, then carry on waiting", exactly as
// native_syscall does for every other interrupted call: native_checkpoint runs
// the handlers (and does not return at all if the signal is fatal, which is
// what makes ^C still work here), and then the wait is reissued. Every other
// error is still an error.
//
// exec is where it was found, but system() and pclose() reap the same way and
// had the same hole: system() would have returned -1, and pclose() -1, for a
// command that ran to completion.
static int nlibc_wait_for_child(dword_t pid, int *status) {
    for (;;) {
        int res = native_waitpid(pid, status, 0);
        if (res != _EINTR)
            return res;
        native_checkpoint();
    }
}

static int nlibc_exec_common(const char *path, char *const argv[], int search_path) {
    if (path == NULL || argv == NULL)
        return nlibc_fail(_EFAULT);

    char resolved[MAX_PATH];
    if (search_path && strchr(path, '/') == NULL) {
        if (native_path_search(path, resolved, sizeof(resolved)) < 0)
            return nlibc_fail(_ENOENT);
        path = resolved;
    }

    // Through native_spawn_opts rather than native_spawn, for the signal mask:
    // this is the program saying "become that program", and the shim's own
    // blocking must not be part of what it becomes. The CHILD's dispositions
    // are NOT reset here -- exec legitimately preserves SIG_IGN, and a caller
    // that ignored a signal meant it. (The stand-in resets its OWN caught
    // handlers, which is the other half of the same rule: see
    // nlibc_exec_reset_handlers.)
    struct native_spawn_opts opts = { .pgid = NATIVE_SPAWN_PGID_INHERIT };
    nlibc_spawn_default_sigmask(&opts);

    dword_t pid = 0;
    int err = native_spawn_opts(path, argv, native_env_vector(), &opts, &pid);
    if (err < 0)
        return nlibc_fail(err);

    nlibc_exec_standin(pid);
}

int nlibc_execv(const char *path, char *const argv[]) {
    return nlibc_exec_common(path, argv, 0);
}
int nlibc_execvp(const char *file, char *const argv[]) {
    return nlibc_exec_common(file, argv, 1);
}
// execl and execlp differ only in whether the name is searched for on PATH --
// and execlp is the form that was missed, which matters more than it sounds:
// the PATH it would have searched is the HOST's. readpass.c execs SSH_ASKPASS
// this way, so an askpass named without a slash would have been looked up in
// the Mac's directories and run as a device binary in place of the whole app.
static int nlibc_execl_common(const char *path, const char *arg0, va_list ap,
        int search_path) {
    // Collect the varargs into a vector; callers here pass short lists.
    enum { MAX_ARGS = 64 };
    char *argv[MAX_ARGS];
    size_t n = 0;
    argv[n++] = (char *) arg0;
    while (n < MAX_ARGS - 1) {
        char *next = va_arg(ap, char *);
        if (next == NULL)
            break;
        argv[n++] = next;
    }
    argv[n] = NULL;
    return nlibc_exec_common(path, argv, search_path);
}

int nlibc_execl(const char *path, const char *arg0, ...) {
    va_list ap;
    va_start(ap, arg0);
    int res = nlibc_execl_common(path, arg0, ap, 0);
    va_end(ap);
    return res;
}

int nlibc_execlp(const char *file, const char *arg0, ...) {
    va_list ap;
    va_start(ap, arg0);
    int res = nlibc_execl_common(file, arg0, ap, 1);
    va_end(ap);
    return res;
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
    if (nlibc_wait_for_child(pid, &status) < 0)
        return -1;
    return status;
}

// NOT translating the wait OPTIONS, deliberately, and this is the note that
// says so. The guest's and Darwin's numbering agree on WNOHANG and WUNTRACED
// and disagree above: Darwin's WCONTINUED is 0x10 where the guest's is 8, and
// Darwin's WSTOPPED is 8, which is the guest's WCONTINUED. The guest answers
// EINVAL for an option it does not recognise, and bash handles exactly that --
// it retries without WCONTINUED (jobs.c, "WCONTINUED may be rejected by
// waitpid as invalid even when defined").
//
// Mapping them properly was tried and made bash HANG: with WCONTINUED really
// enabled, its wait loop never settles. That is a bug in what the guest reports
// for a continued child rather than in the mapping, and fixing it belongs with
// that. Until then, passing the options through unchanged means the one caller
// that matters takes its own documented fallback, which works.

// The signal number inside a wait status is the GUEST's, and the caller will
// read it back with the HOST's WTERMSIG/WSTOPSIG and print it with the host's
// strsignal. The two numberings agree for the signals people notice -- INT,
// TERM, KILL, HUP, SEGV -- and disagree above that: guest SIGUSR1 is 10, which
// is Darwin's SIGBUS. A native bash reported a child killed by SIGUSR1 as "Bus
// error", and $? was 128 plus the wrong number.
static int nlibc_wait_status_to_host(int status) {
    int sig, host;

    if ((status & 0xff) == 0x7f) {                  // stopped
        sig = (status >> 8) & 0xff;
        host = nlibc_signal_to_host(sig);
        return host != 0 ? ((host << 8) | 0x7f) : status;
    }
    if ((status & 0x7f) != 0 && (status & 0x7f) != 0x7f) {   // terminated
        sig = status & 0x7f;
        host = nlibc_signal_to_host(sig);
        return host != 0 ? ((status & ~0x7f) | host) : status;
    }
    return status;                                  // exited, or continued
}

pid_t nlibc_waitpid(pid_t pid, int *status, int options) {
    int res = native_waitpid((dword_t) pid, status, options);
    if (res >= 0 && status != NULL)
        *status = nlibc_wait_status_to_host(*status);
    return res < 0 ? nlibc_fail(res) : res;
}
pid_t nlibc_wait(int *status) {
    return nlibc_waitpid(-1, status, 0);
}

// wait3 and wait4, which exist here because a native program that DOES fork --
// a shell -- reaches for them and the host's would answer.
//
// zsh's reaper is `pid = wait3(&status, WAITFLAGS, &ru)` (Src/signals.c), and
// while fork always failed that line was unreachable. The moment a subshell
// can be spawned it is the ONLY way zsh learns a child has exited: unrouted, it
// asks Darwin about the app process's children, is told ECHILD forever, and
// every subshell is reaped by nobody.
//
// The rusage is zeroed rather than filled. AOK has no per-task resource
// accounting to report, and zsh's only use of it is the `time` keyword's
// user/system breakdown, which would otherwise be uninitialised stack.
pid_t nlibc_wait4(pid_t pid, int *status, int options, struct rusage *rusage) {
    if (rusage != NULL)
        memset(rusage, 0, sizeof(*rusage));
    return nlibc_waitpid(pid, status, options);
}

pid_t nlibc_wait3(int *status, int options, struct rusage *rusage) {
    return nlibc_wait4(-1, status, options, rusage);
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

    // popen("r") is read-only to the caller and popen("w") write-only, which is
    // also what the pipe end can actually do.
    FILE *file = nlibc_file_wrap(parent_fd, 1, reading ? NLIBC_RD : NLIBC_WR);
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
    nlibc_fclose(stream);
    int status = 0;
    if (nlibc_wait_for_child(pid, &status) < 0)
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

// The three ways a descriptor gets a second number, all of which have to tell
// the kqueue table: a dup of a kqueue names the same queue (native_kqueue.h).
int nlibc_dup(int fd_no) {
    int newfd = (int) nlibc_ret(native_syscall(NATIVE_SYS_dup, fd_no));
    if (newfd >= 0)
        nlibc_kqueue_dup_hook(fd_no, newfd);
    return newfd;
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
    int got = (int) nlibc_ret(native_syscall(NATIVE_SYS_dup3, oldfd, newfd, 0));
    if (got >= 0)
        nlibc_kqueue_dup_hook(oldfd, got);
    return got;
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
        case F_GETFD:
        case F_SETFD:
            return (int) nlibc_ret(native_syscall(NATIVE_SYS_fcntl, fd_no, cmd, arg));
        case F_DUPFD:
        case F_DUPFD_CLOEXEC: {
            int guest_cmd = cmd == F_DUPFD ? F_DUPFD : 1030;
            int got = (int) nlibc_ret(native_syscall(NATIVE_SYS_fcntl, fd_no, guest_cmd, arg));
            if (got >= 0)
                nlibc_kqueue_dup_hook(fd_no, got);
            return got;
        }
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

        // POSIX advisory locks. Everything above is a flag or a dup; these are
        // the ones carrying a struct, and they used to fall to the ENOSYS below
        // -- which is why nothing that locks a file could run natively. SQLite's
        // ENTIRE locking protocol is fcntl() byte-range locks, so a native
        // sqlite3 would either refuse to open a database or, worse, believe it
        // held a lock it did not.
        //
        // Two traps here, both silent when got wrong:
        //
        //  - Darwin's struct flock and Linux's share no field order. Darwin is
        //    {l_start, l_len, l_pid, l_type, l_whence}; Linux is {l_type,
        //    l_whence, l_start, l_len, l_pid}. Copying the struct across gives
        //    nonsense, so it goes field by field.
        //  - The lock TYPES differ too: Darwin F_RDLCK=1, F_UNLCK=2, F_WRLCK=3
        //    against Linux's 0, 1, 2. A straight assignment turns a read lock
        //    into a write lock, which is the kind of bug that shows up as
        //    database corruption rather than as an error.
        //
        // The 64-bit command numbers deliberately. A native program's syscalls
        // always dispatch through the ARM64 table, whatever the task's ABI
        // (syscall_dispatch_native in kernel/calls.c), and that table's fcntl is
        // sys_fcntl -- guest64_locks false, so plain F_SETLK there reads a
        // struct flock32_ and truncates any offset past 2 GiB. SQLite's pending
        // byte sits at 0x40000000. F_SETLK64 reads the 64-bit struct flock_.
        case F_GETLK:
        case F_SETLK:
        case F_SETLKW: {
            // Matches fs/inode.h's struct flock_ up to (not including) its
            // comm[], which is AOK's own and not part of the guest ABI --
            // the kernel reads exactly offsetof(struct flock_, comm) bytes.
            struct guest_flock64 {
                int16_t type;
                int16_t whence;
                int64_t start;
                int64_t len;
                int32_t pid;
            } __attribute__((packed)) g;
            _Static_assert(sizeof(g) == 24, "guest struct flock64 is 24 bytes");

            struct flock *host = (struct flock *) (uintptr_t) arg;
            if (host == NULL)
                return nlibc_fail(_EFAULT);
            int type;
            switch (host->l_type) {
                case F_RDLCK: type = 0; break;
                case F_WRLCK: type = 1; break;
                case F_UNLCK: type = 2; break;
                default: return nlibc_fail(_EINVAL);
            }
            memset(&g, 0, sizeof(g));
            g.type = (int16_t) type;
            g.whence = (int16_t) host->l_whence;
            g.start = (int64_t) host->l_start;
            g.len = (int64_t) host->l_len;
            g.pid = (int32_t) host->l_pid;

            NATIVE_FRAME;
            guest_addr_t guest_lock = native_scratch_put(&g, sizeof(g));
            if (guest_lock == 0)
                return nlibc_fail(_ENOMEM);
            int guest_cmd = cmd == F_GETLK ? 12 : (cmd == F_SETLK ? 13 : 14);
            sqword_t res = native_syscall(NATIVE_SYS_fcntl, fd_no, guest_cmd, guest_lock);
            if (res < 0)
                return nlibc_fail((int) res);
            if (cmd == F_GETLK) {
                if (native_scratch_get(&g, guest_lock, sizeof(g)) < 0)
                    return nlibc_fail(_EFAULT);
                switch (g.type) {
                    case 0: host->l_type = F_RDLCK; break;
                    case 1: host->l_type = F_WRLCK; break;
                    default: host->l_type = F_UNLCK; break;
                }
                host->l_whence = (short) g.whence;
                host->l_start = (off_t) g.start;
                host->l_len = (off_t) g.len;
                host->l_pid = (pid_t) g.pid;
            }
            return 0;
        }

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

// utimes that does NOT follow a final symlink: it sets the times on the link
// itself. Spelled out rather than routed through nlibc_utimens, which passes 0
// for utimensat's flags and has three other callers that want it that way.
int nlibc_lutimes(const char *path, const struct timeval times[2]) {
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
    NLIBC_PATH(guest_path, path);
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_utimensat, AT_FDCWD_,
            guest_path, guest_ts, AT_SYMLINK_NOFOLLOW_));
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

// raise() is kill(getpid()) and nothing else, so routing it is free -- both
// halves were already here. Worth doing rather than parking on PURE beside
// abort(): the host's raise ends the APP, this ends the TASK, and the argument
// for leaving abort() alone ("a crash either way") gets weaker the moment the
// signal is a parameter rather than a constant. OpenSSH's one call site is
// sshbuf.c's deliberate SIGSEGV on detected buffer corruption; the next import
// could add another with a different signal, and this way it does not matter.
int nlibc_raise(int sig) {
    return nlibc_kill(nlibc_getpid(), sig);
}

// mkfifo is not a syscall on either side: Darwin's libc and the guest both
// reach it through mknod with S_IFIFO. Routed because Rust's std reaches for
// it directly, and unrouted it made a FIFO on the Mac.
int nlibc_mkfifo(const char *path, mode_t mode) {
    return nlibc_mknod(path, (mode & 07777) | S_IFIFO, 0);
}

// Darwin's AT_SYMLINK_FOLLOW is 0x0040 and the guest's is 0x400, so the flag
// is translated rather than passed through -- the same trap as fstatat's
// AT_SYMLINK_NOFOLLOW above, and the same fix.
#define NLIBC_AT_SYMLINK_FOLLOW 0x400
int nlibc_linkat(int oldfd, const char *from, int newfd, const char *to, int flags) {
    NATIVE_FRAME;
    NLIBC_PATH(guest_from, from);
    NLIBC_PATH(guest_to, to);
    dword_t guest_flags = (flags & AT_SYMLINK_FOLLOW) ? NLIBC_AT_SYMLINK_FOLLOW : 0;
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_linkat, nlibc_at_fd(oldfd),
            guest_from, nlibc_at_fd(newfd), guest_to, guest_flags));
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

// The descriptor form. Rust reaches for it where a path form would race, and
// helix asks it about the file it already has open.
int nlibc_fstatfs(int fd_no, void *buf) {
    NATIVE_FRAME;
    if (buf == NULL)
        return nlibc_fail(_EFAULT);
    guest_addr_t guest_buf = native_scratch_alloc(sizeof(struct amd64_statfs_));
    if (guest_buf == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_fstatfs, fd_no, guest_buf);
    if (res < 0)
        return nlibc_fail((int) res);
    struct amd64_statfs_ guest_statfs;
    if (native_scratch_get(&guest_statfs, guest_buf, sizeof(guest_statfs)) < 0)
        return nlibc_fail(_EFAULT);
    nlibc_guest_statfs_to_host(&guest_statfs, buf);
    return 0;
}

// statvfs is POSIX's shape over the same information, and Darwin's struct
// statvfs is a different layout from its struct statfs -- so this is a
// translation of a translation rather than an alias. Only the fields a caller
// can act on are filled; f_flag's ST_* bits have no guest counterpart the
// guest statfs carries, so it is left zero rather than invented.
static void nlibc_statfs_to_statvfs(const struct statfs *in, struct statvfs *out) {
    memset(out, 0, sizeof(*out));
    out->f_bsize = in->f_bsize;
    out->f_frsize = in->f_bsize;
    out->f_blocks = in->f_blocks;
    out->f_bfree = in->f_bfree;
    out->f_bavail = in->f_bavail;
    out->f_files = in->f_files;
    out->f_ffree = in->f_ffree;
    out->f_favail = in->f_ffree;
    out->f_namemax = NAME_MAX;
}

int nlibc_fstatvfs(int fd_no, struct statvfs *out) {
    struct statfs host;
    if (out == NULL)
        return nlibc_fail(_EFAULT);
    if (nlibc_fstatfs(fd_no, &host) < 0)
        return -1;
    nlibc_statfs_to_statvfs(&host, out);
    return 0;
}

int nlibc_statvfs(const char *path, struct statvfs *out) {
    struct statfs host;
    if (out == NULL)
        return nlibc_fail(_EFAULT);
    if (nlibc_statfs(path, &host) < 0)
        return -1;
    nlibc_statfs_to_statvfs(&host, out);
    return 0;
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

        // The fd-flag ioctls, which every one of these has an fcntl spelling
        // for -- and that is where they go, because the guest answers them
        // per fd type and the fcntl path is routed already.
        //
        // FIOCLEX is not an obscure corner. Rust's std sets close-on-exec this
        // way on every pipe it opens, so a native Rust program could not spawn
        // a child with a redirected stdout at all: Command::output() failed
        // with ENOSYS while the same spawn with inherited stdio worked, which
        // made it look like the spawn path was broken when the spawn had not
        // been reached. Anything that builds a pipeline lands here.
        case FIOCLEX:
            return nlibc_fcntl(fd_no, F_SETFD, (long) FD_CLOEXEC);
        case FIONCLEX:
            return nlibc_fcntl(fd_no, F_SETFD, 0L);
        case FIONBIO: {
            if (arg == NULL)
                return nlibc_fail(_EFAULT);
            int flags = nlibc_fcntl(fd_no, F_GETFL, 0L);
            if (flags < 0)
                return -1;
            flags = *(const int *) arg ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
            return nlibc_fcntl(fd_no, F_SETFL, (long) flags);
        }
        // Darwin and the guest disagree on the number but not on the shape:
        // one int out. FIONREAD_ is the guest's, from kernel/fs.h.
        case FIONREAD: {
            if (arg == NULL)
                return nlibc_fail(_EFAULT);
            dword_t value = 0;
            if (nlibc_tty_ioctl(fd_no, FIONREAD_, &value, sizeof(value), true) < 0)
                return -1;
            *(int *) arg = (int) value;
            return 0;
        }
        default:
            return nlibc_fail(_ENOSYS);
    }
}

// The timespec form, which is what nlibc_poll is built on anyway. Exposed
// because the kqueue front end's timeout arrives as a timespec, and routing it
// through poll()'s milliseconds would round a sub-millisecond wait either up
// (a timer runtime sleeps too long) or down to zero (it spins).
int nlibc_ppoll(void *fds, unsigned nfds, const struct timespec *timeout) {
    NATIVE_FRAME;
    size_t size = (size_t) nfds * sizeof(struct pollfd);
    guest_addr_t guest_fds = native_scratch_put(fds, size);
    if (guest_fds == 0 && size > 0)
        return nlibc_fail(_ENOMEM);

    guest_addr_t guest_ts = 0;
    if (timeout != NULL) {
        struct nlibc_guest_timespec ts = {
            .sec = timeout->tv_sec,
            .nsec = timeout->tv_nsec,
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
    // Same CAP_SYS_ADMIN gate the guest syscall carries. This path calls
    // do_mount() directly rather than going through sys_mount_guest, so without
    // its own check a native program -- SmallCLUE's mount applet, say -- would
    // be the one way left to mount without privilege. The boot-time do_mount()
    // callers in main.c and the app are deliberately not gated; they run before
    // there is a guest task to have credentials at all.
    if (!current_capable(CAP_SYS_ADMIN_))
        return nlibc_fail(_EPERM);
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
// Split by what the key is actually asking about, because two callers inside
// one Rust binary want opposite answers and both are right.
//
//   available_parallelism() asks hw.ncpu to size a thread pool. AOK
//   deliberately reports fewer CPUs than the host has, reserving cores so the
//   emulator does not starve the UI (get_cpu_count_for_affinity,
//   kernel/resource.c). A program that went around that would size itself for
//   the whole machine.
//
//   std_detect asks hw.optional.arm.FEAT_* to decide which instructions it may
//   emit. A native program IS host arm64 code, so the host's answer is the only
//   correct one; a guest notion of CPU features would have it avoid
//   instructions the silicon has, or use ones it does not.
//
// This replaced a blanket ENOTSUP. That was the safe answer while nothing
// called it -- refusing beats answering about the wrong machine -- but it
// stops being safe once a native program asks: Rust's available_parallelism
// falls back to 1 on ENOTSUP, so every Rust program would have run
// single-threaded and looked like an emulator performance problem.
//
// This file is compiled with NATIVE_LIBC_NO_REDIRECT, so the sysctlbyname
// called below is the real one.
int nlibc_sysctlbyname(const char *name, void *old, size_t *oldlen,
                    const void *new, size_t newlen) {
    if (name == NULL) {
        errno = EINVAL;
        return -1;
    }

    bool is_cpu_count =
            strcmp(name, "hw.ncpu") == 0 ||
            strcmp(name, "hw.logicalcpu") == 0 ||
            strcmp(name, "hw.logicalcpu_max") == 0 ||
            strcmp(name, "hw.activecpu") == 0 ||
            strcmp(name, "hw.availcpu") == 0 ||
            strcmp(name, "hw.physicalcpu") == 0 ||
            strcmp(name, "hw.physicalcpu_max") == 0;
    if (!is_cpu_count) {
#if defined(__linux__)
        // No sysctlbyname on Linux, so there is no host answer to pass
        // through. ENOTSUP is the same answer nlibc_sysctl gives, and callers
        // already handle it -- see its comment. Only the CPU-count path below
        // survives here, and that one is portable.
        (void) old; (void) oldlen; (void) new; (void) newlen;
        errno = ENOTSUP;
        return -1;
#else
        return sysctlbyname(name, old, oldlen, (void *) (uintptr_t) new, newlen);
#endif
    }

    // A guest program does not get to change the host's idea of anything.
    if (new != NULL || newlen != 0) {
        errno = EPERM;
        return -1;
    }

    long value = get_cpu_count_for_affinity();
    if (old == NULL) {
        if (oldlen != NULL)
            *oldlen = sizeof(int);
        return 0;
    }
    if (oldlen == NULL) {
        errno = EFAULT;
        return -1;
    }
    // Callers ask as int or int64; serve the width requested. A short write
    // here is a garbage core count, a long one a buffer overrun.
    if (*oldlen >= sizeof(int64_t)) {
        int64_t v64 = (int64_t) value;
        memcpy(old, &v64, sizeof(v64));
        *oldlen = sizeof(v64);
    } else if (*oldlen >= sizeof(int)) {
        int v32 = (int) value;
        memcpy(old, &v32, sizeof(v32));
        *oldlen = sizeof(v32);
    } else {
        errno = ENOMEM;
        return -1;
    }
    return 0;
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
#if !defined(__linux__)
        // Darwin's struct statfs carries the source, mount point and fs type as
        // strings; Linux's carries none of them -- getmntent(3) is where they
        // live there. Nothing on Linux consumes this, so the numbers are filled
        // in above and the names are simply absent.
        strncpy(out[i].f_mntfromname, info[i].source, sizeof(out[i].f_mntfromname) - 1);
        strncpy(out[i].f_mntonname, info[i].point, sizeof(out[i].f_mntonname) - 1);
        strncpy(out[i].f_fstypename, info[i].type, sizeof(out[i].f_fstypename) - 1);
#endif
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

// Enough of the bits to describe a terminal to something else, which is more
// than "enough to drive one locally".
//
// The short map cost a real bug. ssh sends the CLIENT's terminal modes to the
// server in its pty-req, encoded from tcgetattr. ONLCR was not mapped, so the
// modes said "OPOST on, ONLCR off"; the remote pty obeyed, stopped translating
// newline to CRLF, and every remote command came back staircased down the
// screen. `reset` on the remote cured it, which is what pointed at the modes
// rather than at the local terminal.
//
// So anything a caller might reasonably READ and pass on has to survive the
// round trip, not just the handful the shim itself acts upon. These are the
// flags OpenSSH's ttymodes.c actually puts on the wire.
static const struct nlibc_flagmap nlibc_lflags[] = {
    { ISIG,   ISIG_ },   { ICANON,  ICANON_ },  { ECHO,   ECHO_ },
    { ECHOE,  ECHOE_ },  { ECHOK,   ECHOK_ },   { NOFLSH, NOFLSH_ },
    { ECHOCTL, ECHOCTL_ }, { ECHOKE, ECHOKE_ }, { IEXTEN, IEXTEN_ },
};
static const struct nlibc_flagmap nlibc_iflags[] = {
    { ICRNL,  ICRNL_ },  { IXON,    IXON_ },    { INLCR,  INLCR_ },
    { IGNCR,  IGNCR_ },
};
static const struct nlibc_flagmap nlibc_oflags[] = {
    { OPOST,  OPOST_ },  { ONLCR,   ONLCR_ },   { OCRNL,  OCRNL_ },
    { ONOCR,  ONOCR_ },  { ONLRET,  ONLRET_ },
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
    // The rest of the control characters, for the same reason as the flags
    // above: a caller may be DESCRIBING this terminal to something else rather
    // than driving it. Measured over a real ssh session with only the six
    // above mapped, the remote pty came up reporting
    //     erase = <undef>; kill = <undef>; eol = <undef>
    // which is a terminal where backspace does not erase.
    out->c_cc[VERASE] = t.cc[VERASE_];
    out->c_cc[VKILL] = t.cc[VKILL_];
    out->c_cc[VSTART] = t.cc[VSTART_];
    out->c_cc[VSTOP] = t.cc[VSTOP_];
    out->c_cc[VEOL] = t.cc[VEOL_];
    out->c_cc[VREPRINT] = t.cc[VREPRINT_];
    out->c_cc[VDISCARD] = t.cc[VDISCARD_];
    out->c_cc[VWERASE] = t.cc[VWERASE_];
    out->c_cc[VLNEXT] = t.cc[VLNEXT_];
    out->c_cc[VEOL2] = t.cc[VEOL2_];
    // c_cflag, which was not translated at ALL. Measured on a real session,
    // the remote pty came up "speed 0 baud ... cs5" -- five-bit characters on
    // a hung line.
    //
    // The speed is not cosmetic, and fs/tty.h already records why for the
    // emulated path: cfgetospeed() reads c_cflag & CBAUD, B0 means "hang up",
    // and a BSD sshd honours an ospeed of 0 by SIGHUPing the session leader.
    // The native path reintroduced exactly that by leaving c_cflag empty. Read
    // the guest's own speed rather than asserting one, so a tty that really
    // does report B0 still says so.
    out->c_cflag |= CS8;   // the guest's CS8_ is its CSIZE_, i.e. always 8 here
    if (t.cflags & CREAD_)  out->c_cflag |= CREAD;
    if (t.cflags & PARENB_) out->c_cflag |= PARENB;
    if (t.cflags & HUPCL_)  out->c_cflag |= HUPCL;
    speed_t baud = ((t.cflags & CBAUD_) == B0_) ? B0 : B38400;
    cfsetispeed(out, baud);
    cfsetospeed(out, baud);
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

// Software flow control, and the guest tty has no TCXONC to route it to --
// deliberately, because there is no line to start or stop: TCOOFF/TCOON
// suspend transmission on a serial link, and TCIOFF/TCION send the STOP and
// START characters down one. On a pty both are asking a question the medium
// cannot answer.
//
// Reporting success rather than failing, on the same reasoning as tcdrain
// above: the caller asked for a state the terminal is already in.
int nlibc_tcflow(int fd_no, int action) {
    (void) fd_no; (void) action;
    return 0;
}

// A break is a line condition, and the guest's ttys are not lines. Reporting
// success is right rather than convenient: the contract is "send a break for
// `duration`", and on a pty there is nothing for it to reach -- the same
// reasoning tcdrain above is written with.
int nlibc_tcsendbreak(int fd_no, int duration) {
    (void) fd_no; (void) duration;
    return 0;
}

// The session that owns the terminal, which is not the same question as
// getsid() of the calling process -- a program can hold a descriptor to a
// terminal it is not in the session of.
int nlibc_tcgetsid(int fd_no) {
    dword_t sid = 0;
    if (nlibc_tty_ioctl(fd_no, TIOCGSID_, &sid, sizeof(sid), true) < 0)
        return -1;
    return (int) sid;
}

// The reentrant ttyname. Written over nlibc_ttyname rather than beside it so
// the two cannot come to disagree about what a terminal is called, and it
// reports through its RETURN value -- ttyname_r returns an errno, not -1.
int nlibc_ttyname_r(int fd_no, char *buf, size_t buflen) {
    if (buf == NULL)
        return EINVAL;
    char *name = nlibc_ttyname(fd_no);
    if (name == NULL)
        return errno != 0 ? errno : ENOTTY;
    size_t n = strlen(name) + 1;
    if (n > buflen)
        return ERANGE;
    memcpy(buf, name, n);
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

// The four calls above in the order every BSD does them. OpenSSH's sshpty.c is
// the caller, which today is compiled into libopenssh.a but not pulled into the
// link -- the client's pty comes from deps/smallclue/src/openssh_app.c instead.
// It is implemented rather than left as a hole because the gate looks at the
// ARCHIVE, and because "not called yet" stops being true the day a native sshd
// is built: sshd is the program that allocates ptys, and one taken from the
// host would be a DEVICE pty, on the device's /dev/pts, with host descriptors.
int nlibc_openpty(int *amaster, int *aslave, char *name,
        struct termios *termp, struct winsize *winp) {
    int master = nlibc_posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0)
        return -1;
    if (nlibc_grantpt(master) < 0 || nlibc_unlockpt(master) < 0) {
        nlibc_close(master);
        return -1;
    }
    // ptsname's storage is reused by the next call, and nlibc_open below makes
    // no such call -- but the caller's `name` copy must happen from a stable
    // buffer either way.
    char slave_path[64];
    const char *found = nlibc_ptsname(master);
    if (found == NULL) {
        nlibc_close(master);
        return -1;
    }
    snprintf(slave_path, sizeof(slave_path), "%s", found);
    int slave = nlibc_open(slave_path, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        nlibc_close(master);
        return -1;
    }
    if (termp != NULL)
        nlibc_tcsetattr(slave, TCSAFLUSH, termp);
    if (winp != NULL)
        nlibc_ioctl(slave, TIOCSWINSZ, winp);
    if (name != NULL)
        strcpy(name, slave_path);   // openpty's contract: the caller sizes it
    *amaster = master;
    *aslave = slave;
    return 0;
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
        case _SC_NPROCESSORS_ONLN:
            // This used to answer with sysinfo()'s `procs`, which is not the
            // CPU count -- Linux documents that field as "number of current
            // processes". So the answer was however many tasks happened to be
            // running: 1 on an idle guest, and never related to the question.
            //
            // Found because Rust's available_parallelism() reported 1 while
            // nproc in the same guest said 4. nproc goes through
            // sched_getaffinity, which was right all along, so the two ways of
            // asking disagreed. get_cpu_count_for_affinity is what
            // sched_getaffinity uses (kernel/resource.c) and is the one source
            // of truth for how many CPUs AOK is prepared to hand out.
            return get_cpu_count_for_affinity();
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

// The per-invocation token (kernel/native.h). __thread, seeded from a global
// counter at program entry and handed to every thread the program creates
// below, so any thread of the run -- including one a signal handler borrows --
// answers with the same value, and no run ever repeats another's. A u64 off an
// atomic counter does not recycle, which matters: a recycled token would hand
// a new run some earlier run's per-token state, and that state holds fd
// NUMBERS that now mean something else.
static __thread uint64_t nlibc_invocation_token_v;

uint64_t nlibc_invocation_token(void) {
    return nlibc_invocation_token_v;
}

void nlibc_invocation_token_assign(void) {
    static _Atomic uint64_t next_token;
    nlibc_invocation_token_v =
        atomic_fetch_add_explicit(&next_token, 1, memory_order_relaxed) + 1;
}

struct nlibc_thread_start {
    void *(*fn)(void *);
    void *arg;
    struct task *task;
    uint64_t token;
};

static void *nlibc_thread_trampoline(void *opaque) {
    struct nlibc_thread_start *start = opaque;
    // Before the assignment below, which is this thread's first touch of a
    // __thread variable: a wake poke landing mid-instantiation would make the
    // handler re-enter malloc and abort the process. See
    // signal_thread_locals_init() in util/sync.c.
    signal_thread_locals_init();
    current = start->task;   // inherit, so the shim has a task to work against
    nlibc_invocation_token_v = start->token;   // same run, same token
    void *(*fn)(void *) = start->fn;
    void *arg = start->arg;
    free(start);
    void *result = fn(arg);
    // This thread's syscall marshalling arena is a megabyte of the guest
    // address space, and the program that created the thread is still running
    // in that space. Left behind, a program that starts and finishes threads
    // grows the address space by a megabyte at a time for as long as it runs.
    // (A thread that leaves through pthread_exit or cancellation misses this;
    // the arena then goes when the address space does, as it always did.)
    native_arena_release();
    return result;
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
    start->token = nlibc_invocation_token_v;
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
// Is there too little stack left below here to survive another level of
// whatever the caller was about to do?
//
// WHY THIS IS IN THE SHIM. A native program runs on a GUEST TASK'S THREAD, whose
// stack is far smaller than the main thread's, while the shells' own recursion
// limits assume otherwise -- bash's FUNCNEST is unset (unlimited) by default and
// zsh's message invites you to raise it. Running off the end therefore does not
// kill one shell, it kills the WHOLE APP, because a native program is a function
// call inside it. Measured before this existed: `r() { r; }; r` in native bash
// took the ish process down, and the `echo` after it never ran.
//
// Bounds come from pthread, not from arithmetic on a frame address: Darwin's
// pthread_get_stackaddr_np gives the high end and pthread_get_stacksize_np the
// size, so the low end is exact for a guest task thread, the main thread, or any
// future host thread that ends up running a native program. Only "where am I
// now" comes from __builtin_frame_address, which is the one part that must.
//
// A thread's stack never moves, so this is worked out once per thread and what a
// recursing shell pays afterwards is a load and a compare. State is __thread
// because each guest task answers for its own stack -- a re-launched subshell is
// a fresh task on a fresh thread and must work out its own bounds.
//
// The reserve is what still has to fit below the deepest refused call: the rest
// of the caller's frame, the error formatting, and the unwind back out. When the
// platform cannot answer, nothing is refused -- a guess here would break working
// scripts to prevent a crash that might not be coming.
#define NLIBC_STACK_RESERVE (256 * 1024)

static __thread uintptr_t nlibc_stack_floor;
static __thread int nlibc_stack_state;

int nlibc_stack_exhausted(void) {
    if (nlibc_stack_state == 0) {
#if defined(__APPLE__)
        pthread_t self = pthread_self();
        void *high = pthread_get_stackaddr_np(self);
        size_t size = pthread_get_stacksize_np(self);

        if (high != NULL && size > (size_t) (2 * NLIBC_STACK_RESERVE)) {
            nlibc_stack_floor = (uintptr_t) high - size + NLIBC_STACK_RESERVE;
            nlibc_stack_state = 1;
        } else
#endif
            nlibc_stack_state = -1;
    }
    if (nlibc_stack_state != 1)
        return 0;
    return (uintptr_t) __builtin_frame_address(0) <= nlibc_stack_floor;
}

noreturn void nlibc_exit(int status) {
    nlibc_flush_std();
    // A fatal signal deferred while the program was inside host stdio (see
    // nlibc_stdio_defer_fatal) is still pending here. Take it now that the
    // streams are flushed, closed and unlocked, so the task reports
    // died-by-signal -- what ^C on a blocked native reader should look like
    // to its parent -- rather than the error-path exit code the deferral
    // detoured it into.
    native_checkpoint();
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
#if !defined(__linux__)   // BSD-only sockaddr length field
            in->sin_len = sizeof(*in);
#endif
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
#if !defined(__linux__)   // BSD-only sockaddr length field
            in6->sin6_len = sizeof(*in6);
#endif
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
#if !defined(__linux__)   // BSD-only sockaddr length field
            un->sun_len = sizeof(*un);
#endif
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

// A socketpair of HOST descriptors is the pipe() problem again, and it was
// live: config.h leaves USE_PIPES undefined, so sftp.c, scp.c and
// sshconnect.c's ProxyUseFdpass all take this branch rather than the pipe one.
// The two host fds were stored as a channel's in/out and then handed to the
// routed dup2/read/write/close, which read them as GUEST fd numbers.
//
// Today nlibc_fork's ENOSYS on the very next statement caps the damage at a
// leaked pair of host descriptors per attempt. That is exactly the shape that
// makes a runtime check useless -- "scp fails at fork either way" -- and the
// leak happens before the failure.
int nlibc_socketpair(int domain, int type, int protocol, int fds[2]) {
    NATIVE_FRAME;
    if (fds == NULL)
        return nlibc_fail(_EFAULT);
    int guest_domain = nlibc_family_to_guest(domain);
    if (guest_domain < 0)
        return nlibc_fail(_EAFNOSUPPORT);
    int guest_type = (type & 0xf) | (int) nlibc_flags_to_guest((unsigned long) type,
            nlibc_sock_type_flags, NLIBC_MAP_COUNT(nlibc_sock_type_flags));
    guest_addr_t guest_fds = native_scratch_alloc(2 * sizeof(dword_t));
    if (guest_fds == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_socketpair, guest_domain, guest_type,
            protocol, guest_fds);
    if (res < 0)
        return nlibc_fail((int) res);
    dword_t out[2] = {0, 0};
    if (native_scratch_get(out, guest_fds, sizeof(out)) < 0)
        return nlibc_fail(_EFAULT);
    fds[0] = (int) out[0];
    fds[1] = (int) out[1];
    return 0;
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

// ------------------------------------------ messages with ancillary data
//
// sendmsg/recvmsg exist here for one reason: SCM_RIGHTS. OpenSSH's
// monitor_fdpass.c passes descriptors over the mux socket (mux.c, sshconnect.c
// ProxyUseFdpass), and every part of that is the guest's -- the socket is a
// guest fd, the descriptors INSIDE the cmsg are guest fd numbers, and the
// cmsghdr the kernel expects is the guest's, whose length field is 64 bits
// where Darwin's is 32. Left to the host, mux_client_request_session reported
// "mm_send_fd: sendmsg: Bad file descriptor" and multiplexing did not work at
// all; the alternative, succeeding against a host descriptor with the same
// number, would have been worse.
//
// Two simplifications, both deliberate:
//
//  - The iovec is FLATTENED to a single element. Scatter/gather is a delivery
//    convenience, not a wire property: a stream is a stream and a datagram is
//    one contiguous chunk either way, and the shim already has to copy through
//    guest scratch, so the gather happens during a copy that was happening
//    anyway. It also means one guest iovec to marshal instead of n.
//  - Only SOL_SOCKET/SCM_RIGHTS control messages are handled. Anything else is
//    refused with EOPNOTSUPP rather than forwarded with a level and type that
//    mean something different on the far side. Silently dropping ancillary data
//    is the failure mode this whole file exists to avoid.
#define NLIBC_SCM_RIGHTS_ 1

// The guest's shapes, written out for both widths. asm-generic is what a native
// caller normally speaks, but sendmsg and recvmsg are marked "// ABI" in
// native_syscall_nums.h -- their handlers read the struct according to the
// CALLING TASK's abi -- so a 32-bit guest gets the 32-bit layout, exactly as
// nlibc_getrusage branches for the same reason.
struct nlibc_guest_msghdr64 {
    qword_t name; dword_t namelen; dword_t pad0;
    qword_t iov; qword_t iovlen;
    qword_t control; qword_t controllen;
    dword_t flags; dword_t pad1;
};
struct nlibc_guest_msghdr32 {
    dword_t name, namelen, iov, iovlen, control, controllen, flags;
};
struct nlibc_guest_iovec64 { qword_t base, len; };
struct nlibc_guest_iovec32 { dword_t base, len; };
struct nlibc_guest_cmsghdr64 { qword_t len; dword_t level, type; };
struct nlibc_guest_cmsghdr32 { dword_t len, level, type; };
_Static_assert(sizeof(struct nlibc_guest_msghdr64) == 56, "guest msghdr, 64-bit");
_Static_assert(sizeof(struct nlibc_guest_msghdr32) == 28, "guest msghdr, 32-bit");

// CMSG_ALIGN, whose unit is the guest's `long` rather than the host's.
static size_t nlibc_cmsg_align(size_t n, bool w64) {
    return w64 ? ((n + 7) & ~(size_t) 7) : ((n + 3) & ~(size_t) 3);
}
static size_t nlibc_cmsg_hdr_size(bool w64) {
    return w64 ? sizeof(struct nlibc_guest_cmsghdr64)
               : sizeof(struct nlibc_guest_cmsghdr32);
}

// Total bytes across an iovec, or -1 for a vector the caller got wrong.
static ssize_t nlibc_iov_total(const struct iovec *iov, int count) {
    if (count < 0 || (count > 0 && iov == NULL))
        return -1;
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        if (iov[i].iov_len > 0 && iov[i].iov_base == NULL)
            return -1;
        total += iov[i].iov_len;
    }
    return (ssize_t) total;
}

// Lay a guest msghdr into `raw` for whichever width the task speaks, and report
// how many bytes of it are meaningful.
static size_t nlibc_put_guest_msghdr(void *raw, bool w64, guest_addr_t name,
        size_t namelen, guest_addr_t iov, size_t iovlen,
        guest_addr_t control, size_t controllen) {
    if (w64) {
        struct nlibc_guest_msghdr64 *h = raw;
        memset(h, 0, sizeof(*h));
        h->name = name; h->namelen = (dword_t) namelen;
        h->iov = iov;   h->iovlen = iovlen;
        h->control = control; h->controllen = controllen;
        return sizeof(*h);
    }
    struct nlibc_guest_msghdr32 *h = raw;
    memset(h, 0, sizeof(*h));
    h->name = (dword_t) name; h->namelen = (dword_t) namelen;
    h->iov = (dword_t) iov;   h->iovlen = (dword_t) iovlen;
    h->control = (dword_t) control; h->controllen = (dword_t) controllen;
    return sizeof(*h);
}

ssize_t nlibc_sendmsg(int fd_no, const struct msghdr *msg, int flags) {
    NATIVE_FRAME;
    if (msg == NULL)
        return nlibc_fail(_EFAULT);
    bool w64 = guest_abi_is_64bit(current->abi);

    guest_addr_t guest_name = 0;
    size_t guest_namelen = 0;
    if (msg->msg_name != NULL && msg->msg_namelen > 0) {
        char addr[sizeof(struct nlibc_sockaddr_un)];
        ssize_t n = nlibc_sockaddr_to_guest(msg->msg_name, msg->msg_namelen,
                addr, sizeof(addr));
        if (n < 0)
            return nlibc_fail((int) n);
        guest_name = native_scratch_put(addr, (size_t) n);
        if (guest_name == 0)
            return nlibc_fail(_ENOMEM);
        guest_namelen = (size_t) n;
    }

    ssize_t total = nlibc_iov_total(msg->msg_iov, msg->msg_iovlen);
    if (total < 0)
        return nlibc_fail(_EFAULT);
    guest_addr_t guest_data = 0;
    if (total > 0) {
        char *flat = malloc((size_t) total);
        if (flat == NULL)
            return nlibc_fail(_ENOMEM);
        size_t at = 0;
        for (int i = 0; i < msg->msg_iovlen; i++) {
            memcpy(flat + at, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
            at += msg->msg_iov[i].iov_len;
        }
        guest_data = native_scratch_put(flat, (size_t) total);
        free(flat);
        if (guest_data == 0)
            return nlibc_fail(_ENOMEM);
    }
    char iov_raw[sizeof(struct nlibc_guest_iovec64)];
    size_t iov_size;
    if (w64) {
        struct nlibc_guest_iovec64 v = { guest_data, (qword_t) total };
        memcpy(iov_raw, &v, iov_size = sizeof(v));
    } else {
        struct nlibc_guest_iovec32 v = { (dword_t) guest_data, (dword_t) total };
        memcpy(iov_raw, &v, iov_size = sizeof(v));
    }
    guest_addr_t guest_iov = native_scratch_put(iov_raw, iov_size);
    if (guest_iov == 0)
        return nlibc_fail(_ENOMEM);

    // The control block, rebuilt rather than copied: Darwin's cmsghdr is
    // {u32 len, int level, int type} aligned to 4, the guest's 64-bit one is
    // {u64 len, int level, int type} aligned to 8, and the level and type
    // numbers differ as well (Darwin's SOL_SOCKET is 0xffff).
    uint8_t cbuf[512];
    size_t clen = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR((struct msghdr *) msg); c != NULL;
            c = CMSG_NXTHDR((struct msghdr *) msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
            return nlibc_fail(_EOPNOTSUPP);
        size_t payload = c->cmsg_len - CMSG_LEN(0);
        size_t hdr = nlibc_cmsg_hdr_size(w64);
        size_t need = hdr + payload;
        if (clen + nlibc_cmsg_align(need, w64) > sizeof(cbuf))
            return nlibc_fail(_EMSGSIZE);
        memset(cbuf + clen, 0, nlibc_cmsg_align(need, w64));
        if (w64) {
            struct nlibc_guest_cmsghdr64 h = { need, NLIBC_SOL_SOCKET_,
                                               NLIBC_SCM_RIGHTS_ };
            memcpy(cbuf + clen, &h, sizeof(h));
        } else {
            struct nlibc_guest_cmsghdr32 h = { (dword_t) need, NLIBC_SOL_SOCKET_,
                                               NLIBC_SCM_RIGHTS_ };
            memcpy(cbuf + clen, &h, sizeof(h));
        }
        // The descriptors travel as-is: an fd a native program holds IS a guest
        // fd, which is the whole point of routing socket() and open().
        memcpy(cbuf + clen + hdr, CMSG_DATA(c), payload);
        clen += nlibc_cmsg_align(need, w64);
    }
    guest_addr_t guest_control = 0;
    if (clen > 0) {
        guest_control = native_scratch_put(cbuf, clen);
        if (guest_control == 0)
            return nlibc_fail(_ENOMEM);
    }

    char hdr_raw[sizeof(struct nlibc_guest_msghdr64)];
    size_t hdr_size = nlibc_put_guest_msghdr(hdr_raw, w64, guest_name,
            guest_namelen, guest_iov, 1, guest_control, clen);
    guest_addr_t guest_msg = native_scratch_put(hdr_raw, hdr_size);
    if (guest_msg == 0)
        return nlibc_fail(_ENOMEM);
    return (ssize_t) nlibc_ret(native_syscall(NATIVE_SYS_sendmsg, fd_no, guest_msg,
            nlibc_msg_to_guest(flags)));
}

ssize_t nlibc_recvmsg(int fd_no, struct msghdr *msg, int flags) {
    NATIVE_FRAME;
    if (msg == NULL)
        return nlibc_fail(_EFAULT);
    bool w64 = guest_abi_is_64bit(current->abi);

    ssize_t total = nlibc_iov_total(msg->msg_iov, msg->msg_iovlen);
    if (total < 0)
        return nlibc_fail(_EFAULT);
    guest_addr_t guest_data = 0;
    if (total > 0) {
        guest_data = native_scratch_alloc((size_t) total);
        if (guest_data == 0)
            return nlibc_fail(_ENOMEM);
    }
    char iov_raw[sizeof(struct nlibc_guest_iovec64)];
    size_t iov_size;
    if (w64) {
        struct nlibc_guest_iovec64 v = { guest_data, (qword_t) total };
        memcpy(iov_raw, &v, iov_size = sizeof(v));
    } else {
        struct nlibc_guest_iovec32 v = { (dword_t) guest_data, (dword_t) total };
        memcpy(iov_raw, &v, iov_size = sizeof(v));
    }
    guest_addr_t guest_iov = native_scratch_put(iov_raw, iov_size);
    if (guest_iov == 0)
        return nlibc_fail(_ENOMEM);

    guest_addr_t guest_name = 0;
    size_t guest_namelen = 0;
    if (msg->msg_name != NULL && msg->msg_namelen > 0) {
        guest_namelen = sizeof(struct nlibc_sockaddr_un);
        guest_name = native_scratch_alloc(guest_namelen);
        if (guest_name == 0)
            return nlibc_fail(_ENOMEM);
    }

    // Room for the guest's control block, which is BIGGER than the caller's:
    // its cmsghdr is 16 bytes against Darwin's 12 and it aligns to 8 rather
    // than 4, so a buffer sized like the caller's could truncate a parcel that
    // would have fit. Sizing generously here costs scratch and nothing else --
    // what the caller asked for still bounds what is copied back.
    guest_addr_t guest_control = 0;
    size_t guest_controllen = 0;
    if (msg->msg_control != NULL && msg->msg_controllen > 0) {
        guest_controllen = (size_t) msg->msg_controllen * 2 + 64;
        guest_control = native_scratch_alloc(guest_controllen);
        if (guest_control == 0)
            return nlibc_fail(_ENOMEM);
    }

    char hdr_raw[sizeof(struct nlibc_guest_msghdr64)];
    size_t hdr_size = nlibc_put_guest_msghdr(hdr_raw, w64, guest_name,
            guest_namelen, guest_iov, 1, guest_control, guest_controllen);
    guest_addr_t guest_msg = native_scratch_put(hdr_raw, hdr_size);
    if (guest_msg == 0)
        return nlibc_fail(_ENOMEM);

    sqword_t res = native_syscall(NATIVE_SYS_recvmsg, fd_no, guest_msg,
            nlibc_msg_to_guest(flags));
    if (res < 0)
        return nlibc_fail((int) res);

    // What came back: the kernel writes namelen, controllen and flags into the
    // header it was given.
    if (native_scratch_get(hdr_raw, guest_msg, hdr_size) < 0)
        return nlibc_fail(_EFAULT);
    size_t got_namelen, got_controllen;
    dword_t got_flags;
    if (w64) {
        struct nlibc_guest_msghdr64 *h = (void *) hdr_raw;
        got_namelen = h->namelen; got_controllen = h->controllen;
        got_flags = h->flags;
    } else {
        struct nlibc_guest_msghdr32 *h = (void *) hdr_raw;
        got_namelen = h->namelen; got_controllen = h->controllen;
        got_flags = h->flags;
    }

    if (res > 0) {
        char *flat = malloc((size_t) res);
        if (flat == NULL)
            return nlibc_fail(_ENOMEM);
        if (native_scratch_get(flat, guest_data, (size_t) res) < 0) {
            free(flat);
            return nlibc_fail(_EFAULT);
        }
        size_t at = 0;
        for (int i = 0; i < msg->msg_iovlen && at < (size_t) res; i++) {
            size_t take = msg->msg_iov[i].iov_len;
            if (take > (size_t) res - at)
                take = (size_t) res - at;
            memcpy(msg->msg_iov[i].iov_base, flat + at, take);
            at += take;
        }
        free(flat);
    }

    if (guest_name != 0 && got_namelen > 0) {
        char raw[sizeof(struct nlibc_sockaddr_un)];
        if (native_scratch_get(raw, guest_name, sizeof(raw)) < 0)
            return nlibc_fail(_EFAULT);
        char host_addr[sizeof(struct sockaddr_un)];
        ssize_t host_len = nlibc_sockaddr_to_host(raw, got_namelen,
                host_addr, sizeof(host_addr));
        if (host_len < 0)
            return nlibc_fail((int) host_len);
        if ((socklen_t) host_len > msg->msg_namelen)
            host_len = msg->msg_namelen;
        memcpy(msg->msg_name, host_addr, (size_t) host_len);
        msg->msg_namelen = (socklen_t) host_len;
    } else if (msg->msg_name != NULL) {
        msg->msg_namelen = 0;
    }

    msg->msg_flags = (int) nlibc_flags_to_host(got_flags, nlibc_msg_flags,
            NLIBC_MAP_COUNT(nlibc_msg_flags));

    if (msg->msg_control != NULL) {
        size_t written = 0;
        if (got_controllen > 0) {
            uint8_t cbuf[512];
            size_t take = got_controllen < sizeof(cbuf) ? got_controllen : sizeof(cbuf);
            // More ancillary data than this staging buffer holds is truncation,
            // and the caller has to be told so -- a parcel quietly dropped is
            // descriptors quietly lost.
            if (take < got_controllen)
                msg->msg_flags |= MSG_CTRUNC;
            if (native_scratch_get(cbuf, guest_control, take) < 0)
                return nlibc_fail(_EFAULT);
            size_t hdr = nlibc_cmsg_hdr_size(w64);
            for (size_t at = 0; at + hdr <= take; ) {
                size_t len, level, type;
                if (w64) {
                    struct nlibc_guest_cmsghdr64 h;
                    memcpy(&h, cbuf + at, sizeof(h));
                    len = (size_t) h.len; level = h.level; type = h.type;
                } else {
                    struct nlibc_guest_cmsghdr32 h;
                    memcpy(&h, cbuf + at, sizeof(h));
                    len = h.len; level = h.level; type = h.type;
                }
                if (len < hdr || at + len > take)
                    break;
                size_t payload = len - hdr;
                // Anything that is not SCM_RIGHTS is dropped rather than
                // mistranslated -- and the caller is told, through MSG_CTRUNC,
                // that its control buffer does not hold everything that came.
                if (level != (size_t) NLIBC_SOL_SOCKET_ ||
                        type != (size_t) NLIBC_SCM_RIGHTS_ ||
                        written + CMSG_SPACE(payload) > msg->msg_controllen) {
                    msg->msg_flags |= MSG_CTRUNC;
                } else {
                    struct cmsghdr *out = (struct cmsghdr *)
                            ((uint8_t *) msg->msg_control + written);
                    out->cmsg_len = CMSG_LEN(payload);
                    out->cmsg_level = SOL_SOCKET;
                    out->cmsg_type = SCM_RIGHTS;
                    memcpy(CMSG_DATA(out), cbuf + at + hdr, payload);
                    written += CMSG_SPACE(payload);
                }
                at += nlibc_cmsg_align(len, w64);
            }
        }
        msg->msg_controllen = (socklen_t) written;
    }
    return (ssize_t) res;
}

// The LEVELS other than SOL_SOCKET are IPPROTO_ numbers, which agree. The
// OPTIONS under them do not, and that is worse than it sounds: Darwin's IP_TOS
// is 3 where the guest's is 1, and the guest's 3 is IP_HDRINCL -- so passing
// the number through does not simply fail, it asks for a different option on a
// socket that has no business with it. ssh sets IP_TOS on every connection
// (IPQoS, interactive and bulk) and that is where the two "setsockopt socket 3
// IP_TOS 184: Invalid argument" lines per connection came from.
//
// Only what a native program actually reaches for is listed; anything absent
// is refused with ENOPROTOOPT rather than guessed at, since a wrong guess here
// is silent.
static const struct nlibc_signalmap nlibc_ip_opts[] = {
    { IP_TOS, 1 },            { IP_TTL, 2 },            { IP_HDRINCL, 3 },
    { IP_RETOPTS, 7 },        { IP_RECVTTL, 12 },       { IP_RECVTOS, 13 },
    { IP_MULTICAST_IF, 32 },  { IP_MULTICAST_TTL, 33 }, { IP_MULTICAST_LOOP, 34 },
    { IP_ADD_MEMBERSHIP, 35 },{ IP_DROP_MEMBERSHIP, 36 },
#ifdef IP_RECVPKTINFO
    { IP_RECVPKTINFO, 8 },    // the guest's IP_PKTINFO
#endif
};

static const struct nlibc_signalmap nlibc_ipv6_opts[] = {
    { IPV6_UNICAST_HOPS, 16 },   { IPV6_MULTICAST_IF, 17 },
    { IPV6_MULTICAST_HOPS, 18 }, { IPV6_MULTICAST_LOOP, 19 },
    { IPV6_JOIN_GROUP, 20 },     { IPV6_LEAVE_GROUP, 21 },
    { IPV6_V6ONLY, 26 },         { IPV6_TCLASS, 67 },
};

static int nlibc_sockopt_to_guest(int level, int option, int *guest_level) {
    *guest_level = level;
    const struct nlibc_signalmap *map = NULL;
    size_t n = 0;
    if (level == SOL_SOCKET) {
        *guest_level = NLIBC_SOL_SOCKET_;
        map = nlibc_sock_opts; n = NLIBC_MAP_COUNT(nlibc_sock_opts);
    } else if (level == IPPROTO_IP) {
        map = nlibc_ip_opts; n = NLIBC_MAP_COUNT(nlibc_ip_opts);
    } else if (level == IPPROTO_IPV6) {
        map = nlibc_ipv6_opts; n = NLIBC_MAP_COUNT(nlibc_ipv6_opts);
    } else {
        // IPPROTO_TCP's TCP_NODELAY and TCP_MAXSEG are 1 and 2 on both sides,
        // and those are the only ones reached today.
        return option;
    }
    for (size_t i = 0; i < n; i++)
        if (map[i].host == option)
            return map[i].guest;
    return -1;
}

// Darwin's sendfile is not Linux's with the arguments shuffled: it takes the
// byte count in/out through a pointer, puts the source first, and returns 0 or
// -1 rather than a count. Unrouted it would have copied a HOST file into a
// HOST socket, which is why it is translated rather than passed through.
//
// The guest's sendfile64 wants the offset by pointer and answers with the
// count, so both halves of the shape change here.
int nlibc_sendfile(int in_fd, int out_fd, off_t offset, off_t *len,
                   void *hdtr, int flags) {
    NATIVE_FRAME;
    (void) flags;
    // sf_hdtr prepends and appends caller buffers around the file. Nothing
    // reaches this with one, and quietly dropping the headers would send a
    // truncated message, so it is refused.
    if (hdtr != NULL)
        return nlibc_fail(_ENOSYS);
    if (len == NULL)
        return nlibc_fail(_EFAULT);

    off_t want = *len;
    *len = 0;
    // 0 means "to the end of the file" on Darwin. The guest has no such
    // spelling, so it becomes a large count and the short answer below
    // reports what actually moved.
    size_t count = want > 0 ? (size_t) want : (size_t) SSIZE_MAX;

    sqword_t guest_off = offset;
    guest_addr_t off_ptr = native_scratch_put(&guest_off, sizeof(guest_off));
    if (off_ptr == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_sendfile64, out_fd, in_fd, off_ptr,
            (dword_t) count);
    if (res < 0)
        return nlibc_fail((int) res);
    *len = (off_t) res;
    // Darwin reports a short send as EAGAIN with *len set, not as success.
    if (want > 0 && res < want) {
        errno = EAGAIN;
        return -1;
    }
    return 0;
}

// SO_NOSIGPIPE is Darwin's per-socket "return EPIPE instead of raising
// SIGPIPE", and Linux has no counterpart -- it spells the same wish
// MSG_NOSIGNAL, per send, which write(2) on a socket cannot carry. So there is
// nothing to translate this into.
//
// Accepted rather than refused, because refusing it stops the caller dead:
// mio sets it on every socket it opens on an Apple target, so ENOPROTOOPT here
// meant no async Rust program could open a socket at all.
//
// What is NOT true afterwards: the socket does not actually suppress SIGPIPE.
// A write to a peer that has gone away raises it, and a task that has not
// handled or ignored SIGPIPE dies. That is how every native program already
// behaves, so this is a gap being named rather than one being opened --
// docs/TODO.md carries it.
static bool nlibc_sockopt_is_nosigpipe(int level, int option) {
#ifdef SO_NOSIGPIPE
    return level == SOL_SOCKET && option == SO_NOSIGPIPE;
#else
    (void) level; (void) option;
    return false;
#endif
}

int nlibc_setsockopt(int fd_no, int level, int option, const void *value, socklen_t len) {
    NATIVE_FRAME;
    if (nlibc_sockopt_is_nosigpipe(level, option))
        return 0;
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
    // Answers what setsockopt above accepted, so a caller that sets and reads
    // back does not conclude the socket is in a state it never asked for.
    if (nlibc_sockopt_is_nosigpipe(level, option)) {
        if (value != NULL && *len >= sizeof(int)) {
            *(int *) value = 1;
            *len = sizeof(int);
            return 0;
        }
        return nlibc_fail(_EINVAL);
    }
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

// The peer's credentials on a unix socket. Not routed through nlibc_getsockopt
// because Darwin has no SO_PEERCRED to translate FROM -- it spells this
// LOCAL_PEERCRED at a different level with a different struct -- so the option
// is named in the guest's numbering directly, which is what the table above
// does for every other constant that has no host equivalent.
//
// This is the most immediately provable of the whole set, because no fork
// stands in the way: channels.c's channel_post_mux_listener() accepts a
// ControlMaster connection on a guest AF_UNIX socket and then asks who is on
// the other end. The host's getpeereid either fails with ENOTSOCK on a
// descriptor number that means something else, or succeeds and reports the iOS
// account's uid 501 -- which channels.c compares against the routed getuid()'s
// 0 and refuses the client with "multiplex uid mismatch".
#define NLIBC_SO_PEERCRED_ 17

int nlibc_getpeereid(int fd_no, uid_t *euid, gid_t *egid) {
    NATIVE_FRAME;
    // The guest's struct ucred: pid, uid, gid, three 32-bit fields (fs/sock.h).
    struct { dword_t pid, uid, gid; } cred = { 0, 0, 0 };
    guest_addr_t guest_cred = native_scratch_alloc(sizeof(cred));
    dword_t size = sizeof(cred);
    guest_addr_t guest_size = native_scratch_put(&size, sizeof(size));
    if (guest_cred == 0 || guest_size == 0)
        return nlibc_fail(_ENOMEM);
    sqword_t res = native_syscall(NATIVE_SYS_getsockopt, fd_no, NLIBC_SOL_SOCKET_,
            NLIBC_SO_PEERCRED_, guest_cred, guest_size);
    if (res < 0)
        return nlibc_fail((int) res);
    if (native_scratch_get(&cred, guest_cred, sizeof(cred)) < 0)
        return nlibc_fail(_EFAULT);
    // fs/sock.c answers uid = gid = -1 for a socket with no peer to report --
    // an unconnected socket, or one that is not AF_UNIX at all. getpeereid's
    // contract is to fail there rather than hand back that sentinel.
    if (cred.uid == (dword_t) -1 && cred.gid == (dword_t) -1)
        return nlibc_fail(_ENOTCONN);
    if (euid != NULL)
        *euid = (uid_t) cred.uid;
    if (egid != NULL)
        *egid = (gid_t) cred.gid;
    return 0;
}

// ---------------------------------------------------------------- resolution
//
// getaddrinfo on the host reads the MAC's /etc/resolv.conf and /etc/hosts and
// answers from the Mac's resolver. What a guest asks for has to be answered
// from the guest -- not because the Mac's answer would be slower or scruffier
// but because it would be a different answer: the guest may run its own
// nameservers, its own /etc/hosts, a VPN, or a Tailscale name that the Mac
// cannot see, and the Mac may hold names the guest is not entitled to.
//
// So all three sources here are the guest's. A numeric address is parsed on
// the spot; then the guest's /etc/hosts; and then, for anything left, a stub
// resolver that reads the guest's /etc/resolv.conf and queries the nameservers
// it names -- over the sockets above, which are GUEST sockets, so a query
// leaves through the guest's network stack exactly like every other packet the
// guest sends.
//
// The DNS wire format below is written out again rather than called into.
// SmallCLUE's deps/smallclue/src/core.c already has a complete implementation
// (smallclueDnsEncodeName, smallclueDnsDecodeName, smallclueDnsQueryServer and
// a resolv.conf reader), but that lives ABOVE this file -- it is one of the
// programs the shim serves -- and calling up into it would invert the layering
// and make the shim depend on the applet set. The duplication is deliberate,
// and this copy is deliberately the smaller one: A and AAAA only, UDP only, no
// EDNS0, no TCP fallback on a truncated reply, since an address RRset that
// overflows 512 bytes is not a case a connecting program hits.

#define NLIBC_EAI_NONAME -2
#define NLIBC_EAI_AGAIN  -3
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

// ------------------------------------------------------------ the stub resolver

#define NLIBC_DNS_MAX_SERVERS 3
#define NLIBC_DNS_MAX_SEARCH  4
#define NLIBC_DNS_MAX_ADDRS   8
#define NLIBC_DNS_PORT        53
#define NLIBC_DNS_TYPE_A      1
#define NLIBC_DNS_TYPE_CNAME  5
#define NLIBC_DNS_TYPE_AAAA   28
#define NLIBC_DNS_CLASS_IN    1

// One address, family-tagged, in network order -- what a query yields and what
// the addrinfo builder below turns into a sockaddr.
struct nlibc_addr { int family; uint8_t raw[16]; };

struct nlibc_resolv {
    struct nlibc_addr servers[NLIBC_DNS_MAX_SERVERS];
    int server_count;
    char search[NLIBC_DNS_MAX_SEARCH][256];
    int search_count;
    int timeout_ms;
    int attempts;
    int ndots;
};

// The GUEST's /etc/resolv.conf, read the same way /etc/hosts is: through the
// shim's own fopen, so it is the guest's file and not the Mac's.
static void nlibc_read_resolv_conf(struct nlibc_resolv *rc) {
    memset(rc, 0, sizeof(*rc));
    rc->timeout_ms = 2000;   // per server per attempt
    rc->attempts = 2;
    rc->ndots = 1;

    FILE *f = nlibc_fopen("/etc/resolv.conf", "r");
    if (f == NULL)
        return;
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        char *comment = strpbrk(line, "#;");
        if (comment != NULL)
            *comment = '\0';
        char *save = NULL;
        char *key = strtok_r(line, " \t\r\n", &save);
        if (key == NULL)
            continue;
        if (strcmp(key, "nameserver") == 0) {
            char *value = strtok_r(NULL, " \t\r\n", &save);
            struct nlibc_addr server;
            memset(&server, 0, sizeof(server));
            if (value != NULL && rc->server_count < NLIBC_DNS_MAX_SERVERS &&
                    nlibc_parse_numeric(value, AF_UNSPEC, server.raw, &server.family) == 0)
                rc->servers[rc->server_count++] = server;
        } else if (strcmp(key, "search") == 0 || strcmp(key, "domain") == 0) {
            rc->search_count = 0;   // a later line replaces an earlier one
            for (char *value = strtok_r(NULL, " \t\r\n", &save);
                    value != NULL && rc->search_count < NLIBC_DNS_MAX_SEARCH;
                    value = strtok_r(NULL, " \t\r\n", &save))
                snprintf(rc->search[rc->search_count++], sizeof(rc->search[0]), "%s", value);
        } else if (strcmp(key, "options") == 0) {
            for (char *value = strtok_r(NULL, " \t\r\n", &save); value != NULL;
                    value = strtok_r(NULL, " \t\r\n", &save)) {
                int n;
                if (strncmp(value, "timeout:", 8) == 0 && (n = atoi(value + 8)) > 0)
                    rc->timeout_ms = (n > 30 ? 30 : n) * 1000;
                else if (strncmp(value, "attempts:", 9) == 0 && (n = atoi(value + 9)) > 0)
                    rc->attempts = n > 5 ? 5 : n;
                else if (strncmp(value, "ndots:", 6) == 0 && (n = atoi(value + 6)) >= 0)
                    rc->ndots = n > 15 ? 15 : n;
            }
        }
    }
    fclose(f);
}

// "www.example.com" -> 3www7example3com0. Returns the encoded length, or 0 if
// the name will not fit or is not a legal name.
static size_t nlibc_dns_encode_name(const char *name, uint8_t *buf, size_t size) {
    size_t pos = 0;
    for (const char *p = name; *p != '\0'; ) {
        const char *dot = strchr(p, '.');
        size_t label = dot != NULL ? (size_t) (dot - p) : strlen(p);
        if (label == 0) {
            // A single trailing dot is the root and ends the name; an empty
            // label anywhere else is malformed.
            if (dot != NULL && dot[1] == '\0')
                break;
            return 0;
        }
        if (label > 63 || pos + label + 2 > size)
            return 0;
        buf[pos++] = (uint8_t) label;
        memcpy(buf + pos, p, label);
        pos += label;
        p += label;
        if (*p == '.')
            p++;
    }
    if (pos + 1 > size)
        return 0;
    buf[pos++] = 0;
    return pos;
}

// Reads the name at `pos` into `out` and sets `*after` past the name AS IT
// APPEARS AT `pos` -- past the compression pointer itself, not into what it
// points at, which is what walking a record needs. A pointer may only go
// BACKWARDS, which is both the rule and what makes a crafted reply unable to
// spin this forever.
static bool nlibc_dns_decode_name(const uint8_t *msg, size_t len, size_t pos,
        char *out, size_t out_size, size_t *after) {
    size_t out_len = 0;
    bool jumped = false;
    unsigned guard = 0;
    if (out != NULL && out_size > 0)
        out[0] = '\0';
    for (;;) {
        if (guard++ > 128 || pos >= len)
            return false;
        uint8_t n = msg[pos];
        if ((n & 0xc0) == 0xc0) {
            if (pos + 1 >= len)
                return false;
            size_t target = ((size_t) (n & 0x3f) << 8) | msg[pos + 1];
            if (!jumped && after != NULL)
                *after = pos + 2;
            jumped = true;
            if (target >= pos)
                return false;
            pos = target;
            continue;
        }
        if (n > 63)
            return false;
        if (n == 0) {
            if (!jumped && after != NULL)
                *after = pos + 1;
            return true;
        }
        if (pos + 1 + n > len)
            return false;
        if (out != NULL) {
            if (out_len + n + 2 > out_size)
                return false;
            if (out_len > 0)
                out[out_len++] = '.';
            memcpy(out + out_len, msg + pos + 1, n);
            out_len += n;
            out[out_len] = '\0';
        }
        pos += 1 + n;
    }
}

// One question out, one reply in, over a GUEST socket -- so the packet leaves
// through the guest's stack and reaches whatever the guest's routing says,
// which is the whole point of not calling the Mac's resolver. Returns the
// reply length or -1.
static ssize_t nlibc_dns_exchange(const struct nlibc_addr *server,
        const uint8_t *query, size_t qlen, int timeout_ms,
        uint8_t *reply, size_t reply_size) {
    union { struct sockaddr_in v4; struct sockaddr_in6 v6; } sa;
    socklen_t salen;
    memset(&sa, 0, sizeof(sa));
    if (server->family == AF_INET) {
#if !defined(__linux__)   // BSD-only sockaddr length field
        sa.v4.sin_len = sizeof(sa.v4);
#endif
        sa.v4.sin_family = AF_INET;
        sa.v4.sin_port = htons(NLIBC_DNS_PORT);
        memcpy(&sa.v4.sin_addr, server->raw, 4);
        salen = sizeof(sa.v4);
    } else {
#if !defined(__linux__)   // BSD-only sockaddr length field
        sa.v6.sin6_len = sizeof(sa.v6);
#endif
        sa.v6.sin6_family = AF_INET6;
        sa.v6.sin6_port = htons(NLIBC_DNS_PORT);
        memcpy(&sa.v6.sin6_addr, server->raw, 16);
        salen = sizeof(sa.v6);
    }

    int fd = nlibc_socket(server->family, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    ssize_t got = -1;
    // connect() rather than sendto(): it filters the reply to the server that
    // was asked, and it turns an ICMP port-unreachable into an error on the
    // next call instead of a wait for the full timeout.
    if (nlibc_connect(fd, &sa, salen) == 0 &&
            nlibc_send(fd, query, qlen, 0) == (ssize_t) qlen) {
        // The timeout is the whole reason this cannot hang: a nameserver that
        // is listed and does not answer costs timeout_ms and no more.
        struct pollfd pfd;
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (nlibc_poll(&pfd, 1, timeout_ms) == 1 && (pfd.revents & POLLIN))
            got = nlibc_recv(fd, reply, reply_size, 0);
    }
    nlibc_close(fd);
    return got;
}

// Walks the answer section, following CNAMEs. A recursive resolver returns the
// chain and the addresses it ends at in the same message, so a hop is a
// re-walk of the section with the new owner name rather than another query.
// Returns how many addresses were written.
static int nlibc_dns_answers(const uint8_t *msg, size_t len, const char *qname,
        int qtype, struct nlibc_addr *out, int max) {
    if (len < 12)
        return 0;
    unsigned qdcount = ((unsigned) msg[4] << 8) | msg[5];
    unsigned ancount = ((unsigned) msg[6] << 8) | msg[7];
    size_t answers = 12;
    for (unsigned i = 0; i < qdcount; i++) {
        if (!nlibc_dns_decode_name(msg, len, answers, NULL, 0, &answers))
            return 0;
        answers += 4;   // QTYPE + QCLASS
        if (answers > len)
            return 0;
    }

    char target[256];
    snprintf(target, sizeof(target), "%s", qname);
    size_t tlen = strlen(target);
    if (tlen > 0 && target[tlen - 1] == '.')
        target[tlen - 1] = '\0';

    size_t need = qtype == NLIBC_DNS_TYPE_AAAA ? 16 : 4;
    int count = 0;
    for (int hop = 0; hop < 8; hop++) {
        char next[256];
        next[0] = '\0';
        size_t pos = answers;
        for (unsigned i = 0; i < ancount && pos < len; i++) {
            char owner[256];
            if (!nlibc_dns_decode_name(msg, len, pos, owner, sizeof(owner), &pos))
                return count;
            if (pos + 10 > len)
                return count;
            unsigned type = ((unsigned) msg[pos] << 8) | msg[pos + 1];
            unsigned class = ((unsigned) msg[pos + 2] << 8) | msg[pos + 3];
            unsigned rdlen = ((unsigned) msg[pos + 8] << 8) | msg[pos + 9];
            size_t rdata = pos + 10;
            pos = rdata + rdlen;
            if (pos > len)
                return count;
            if (class != NLIBC_DNS_CLASS_IN || strcasecmp(owner, target) != 0)
                continue;
            if (type == NLIBC_DNS_TYPE_CNAME) {
                if (next[0] == '\0') {
                    size_t ignored = 0;
                    if (!nlibc_dns_decode_name(msg, len, rdata, next, sizeof(next), &ignored))
                        next[0] = '\0';
                }
            } else if ((int) type == qtype && rdlen == need && count < max) {
                out[count].family = qtype == NLIBC_DNS_TYPE_AAAA ? AF_INET6 : AF_INET;
                memset(out[count].raw, 0, sizeof(out[count].raw));
                memcpy(out[count].raw, msg + rdata, need);
                count++;
            }
        }
        if (count > 0 || next[0] == '\0')
            break;
        snprintf(target, sizeof(target), "%s", next);
    }
    return count;
}

// Asks every configured server, `attempts` times round, until one answers.
// Returns the reply's length, NLIBC_EAI_NONAME for an authoritative NXDOMAIN,
// or NLIBC_EAI_AGAIN when nothing answered at all.
//
// Split out from nlibc_dns_query below because there are two shapes of answer
// to walk -- addresses for A and AAAA, a name for PTR -- and only the walking
// differs. Asking is identical, down to the per-exchange ID.
static ssize_t nlibc_dns_transact(const struct nlibc_resolv *rc, const char *name,
        int qtype, uint8_t *reply, size_t reply_size) {
    uint8_t query[512];
    memset(query, 0, sizeof(query));
    size_t name_len = nlibc_dns_encode_name(name, query + 12, sizeof(query) - 12 - 4);
    if (name_len == 0)
        return NLIBC_EAI_NONAME;
    size_t qlen = 12 + name_len;
    query[2] = 0x01;   // RD: ask the server to recurse
    query[5] = 0x01;   // QDCOUNT = 1
    query[qlen++] = (uint8_t) (qtype >> 8);
    query[qlen++] = (uint8_t) (qtype & 0xff);
    query[qlen++] = 0;
    query[qlen++] = NLIBC_DNS_CLASS_IN;

    for (int attempt = 0; attempt < rc->attempts; attempt++) {
        for (int s = 0; s < rc->server_count; s++) {
            // A fresh ID per exchange, so a late reply to the previous attempt
            // cannot be mistaken for this one's.
            uint16_t id = (uint16_t) arc4random();
            query[0] = (uint8_t) (id >> 8);
            query[1] = (uint8_t) (id & 0xff);

            ssize_t n = nlibc_dns_exchange(&rc->servers[s], query, qlen,
                    rc->timeout_ms, reply, reply_size);
            if (n < 12 || reply[0] != query[0] || reply[1] != query[1])
                continue;
            if ((reply[2] & 0x80) == 0)   // not a response
                continue;
            int rcode = reply[3] & 0x0f;
            if (rcode == 3)               // NXDOMAIN: an answer, and it is "no"
                return NLIBC_EAI_NONAME;
            if (rcode != 0)               // SERVFAIL and friends: try the next
                continue;
            return n;
        }
    }
    return NLIBC_EAI_AGAIN;
}

// The address form of that: how many addresses were written (0 means "the name
// is fine, it just has no record of this type"), or a negative NLIBC_EAI_*.
static int nlibc_dns_query(const struct nlibc_resolv *rc, const char *name, int qtype,
        struct nlibc_addr *out, int max) {
    uint8_t reply[2048];
    ssize_t n = nlibc_dns_transact(rc, name, qtype, reply, sizeof(reply));
    if (n < 0)
        return (int) n;
    return nlibc_dns_answers(reply, (size_t) n, name, qtype, out, max);
}

// The name -> addresses path: resolv.conf, the search list, then A and AAAA.
// Returns a count, or a negative NLIBC_EAI_*.
static int nlibc_dns_lookup(const char *node, int family, struct nlibc_addr *out, int max) {
    struct nlibc_resolv rc;
    nlibc_read_resolv_conf(&rc);
    if (rc.server_count == 0)
        return NLIBC_EAI_NONAME;   // nothing configured to ask

    size_t node_len = strlen(node);
    if (node_len == 0 || node_len > 255)
        return NLIBC_EAI_NONAME;
    bool absolute = node[node_len - 1] == '.';
    int dots = 0;
    for (const char *p = node; *p != '\0'; p++)
        if (*p == '.')
            dots++;

    // Candidate order, as a resolver does it: a name with at least `ndots`
    // dots (or a trailing one) is tried as written first, a shorter one goes
    // through the search list first.
    char candidates[NLIBC_DNS_MAX_SEARCH + 1][320];
    int n = 0;
    bool bare_first = absolute || dots >= rc.ndots;
    if (bare_first)
        snprintf(candidates[n++], sizeof(candidates[0]), "%s", node);
    if (!absolute)
        for (int i = 0; i < rc.search_count && n < NLIBC_DNS_MAX_SEARCH + 1; i++)
            snprintf(candidates[n++], sizeof(candidates[0]), "%s.%s", node, rc.search[i]);
    if (!bare_first && n < NLIBC_DNS_MAX_SEARCH + 1)
        snprintf(candidates[n++], sizeof(candidates[0]), "%s", node);

    int last = NLIBC_EAI_NONAME;
    for (int i = 0; i < n; i++) {
        int count = 0;
        bool nxdomain = false;
        // A before AAAA: the list is handed back in this order and a caller
        // walks it in order, so a guest without working IPv6 connects on the
        // first address rather than after a timeout.
        if (family == AF_UNSPEC || family == AF_INET) {
            int got = nlibc_dns_query(&rc, candidates[i], NLIBC_DNS_TYPE_A, out, max);
            // Two answers that make the rest of the work pointless: nothing
            // answered (the AAAA would wait out the same timeouts twice over),
            // and NXDOMAIN (the name does not exist for ANY type). Both are
            // the difference between a bounded failure and a long one.
            if (got == NLIBC_EAI_AGAIN)
                return got;
            if (got == NLIBC_EAI_NONAME)
                nxdomain = true;
            if (got < 0)
                last = got;
            else
                count += got;
        }
        if (!nxdomain && (family == AF_UNSPEC || family == AF_INET6) && count < max) {
            int got = nlibc_dns_query(&rc, candidates[i], NLIBC_DNS_TYPE_AAAA,
                    out + count, max - count);
            if (got == NLIBC_EAI_AGAIN && count == 0)
                return got;
            if (got < 0)
                last = got;
            else
                count += got;
        }
        if (count > 0)
            return count;
    }
    return last;
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

    // The port first: a bad service is a cheaper "no" than a DNS round trip.
    // Numeric, or a name from the GUEST's /etc/services through the walker at
    // the end of this file -- the Mac's list would be a different answer for
    // the same reason its resolver would be.
    unsigned port = 0;
    if (service != NULL && service[0] != '\0') {
        char *end = NULL;
        unsigned long parsed = strtoul(service, &end, 10);
        if (end != NULL && *end == '\0' && parsed <= 65535) {
            port = (unsigned) parsed;
        } else {
            const char *want_proto = socktype == SOCK_DGRAM ? "udp" : "tcp";
            int resolved = -1;
            nlibc_setservent(1);
            for (struct servent *se = nlibc_getservent(); se != NULL;
                    se = nlibc_getservent()) {
                if (se->s_name != NULL && strcmp(se->s_name, service) == 0 &&
                        (se->s_proto == NULL || strcmp(se->s_proto, want_proto) == 0)) {
                    resolved = ntohs((uint16_t) se->s_port);
                    break;
                }
            }
            nlibc_endservent();
            if (resolved < 0)
                return NLIBC_EAI_NONAME;
            port = (unsigned) resolved;
        }
    }

    // Then the address. Three sources, in the order they take precedence: a
    // numeric address, then the guest's /etc/hosts, then the guest's
    // nameservers. /etc/hosts winning over DNS is the same rule every other
    // resolver follows, and the reason an entry there is how a guest overrides
    // a name.
    struct nlibc_addr addrs[NLIBC_DNS_MAX_ADDRS];
    memset(addrs, 0, sizeof(addrs));
    int count;
    if (nlibc_parse_numeric(node, family, addrs[0].raw, &addrs[0].family) == 0) {
        count = 1;
    } else if (hints != NULL && (hints->ai_flags & AI_NUMERICHOST)) {
        return NLIBC_EAI_NONAME;
    } else if (nlibc_hosts_lookup(node, family, addrs[0].raw, &addrs[0].family) == 0) {
        count = 1;
    } else {
        count = nlibc_dns_lookup(node, family, addrs, NLIBC_DNS_MAX_ADDRS);
        if (count < 0)
            return count;
        if (count == 0)
            return NLIBC_EAI_NONAME;
    }

    // One addrinfo per address, chained -- the same shape the numeric case
    // always produced, so nlibc_freeaddrinfo is unchanged and a caller that
    // walks ai_next now has more than one thing to walk.
    struct addrinfo *head = NULL, **tail = &head;
    for (int i = 0; i < count; i++) {
        struct addrinfo *ai = calloc(1, sizeof(*ai));
        if (ai == NULL) {
            nlibc_freeaddrinfo(head);
            return NLIBC_EAI_MEMORY;
        }
        if (addrs[i].family == AF_INET) {
            struct sockaddr_in *sa = calloc(1, sizeof(*sa));
            if (sa == NULL) { free(ai); nlibc_freeaddrinfo(head); return NLIBC_EAI_MEMORY; }
#if !defined(__linux__)   // BSD-only sockaddr length field
            sa->sin_len = sizeof(*sa);
#endif
            sa->sin_family = AF_INET;
            sa->sin_port = htons((uint16_t) port);
            memcpy(&sa->sin_addr, addrs[i].raw, sizeof(sa->sin_addr));
            ai->ai_addr = (struct sockaddr *) sa;
            ai->ai_addrlen = sizeof(*sa);
        } else {
            struct sockaddr_in6 *sa = calloc(1, sizeof(*sa));
            if (sa == NULL) { free(ai); nlibc_freeaddrinfo(head); return NLIBC_EAI_MEMORY; }
#if !defined(__linux__)   // BSD-only sockaddr length field
            sa->sin6_len = sizeof(*sa);
#endif
            sa->sin6_family = AF_INET6;
            sa->sin6_port = htons((uint16_t) port);
            memcpy(&sa->sin6_addr, addrs[i].raw, sizeof(sa->sin6_addr));
            ai->ai_addr = (struct sockaddr *) sa;
            ai->ai_addrlen = sizeof(*sa);
        }
        ai->ai_family = addrs[i].family;
        ai->ai_socktype = socktype;
        ai->ai_protocol = protocol;
        *tail = ai;
        tail = &ai->ai_next;
    }
    *res = head;
    return head != NULL ? 0 : NLIBC_EAI_NONAME;
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
        case NLIBC_EAI_AGAIN:  return "temporary failure in name resolution";
        case NLIBC_EAI_MEMORY: return "memory allocation failure";
        default:               return "non-recoverable failure in name resolution";
    }
}

// The reverse direction is a PTR query, which the resolver above could serve
// -- but nothing native asks for a name back: ssh prints and compares the
// numeric form, and a wrong reverse name is worse than none. So this stays
// numeric-only. NI_NAMEREQD asks for a name or nothing, which is exactly what
// has to fail here.
//
// Either half may be asked for alone. ssh's get_sock_port() wants only the
// port (host NULL, NI_NUMERICSERV) and treats a failure as fatal, so refusing
// a NULL host -- which this did -- killed a connection that had already been
// made.
int nlibc_getnameinfo(const void *addr, socklen_t addrlen, char *host, socklen_t hostlen,
        char *serv, socklen_t servlen, int flags) {
    const struct sockaddr *sa = addr;
    bool want_host = host != NULL && hostlen > 0;
    bool want_serv = serv != NULL && servlen > 0;
    if (sa == NULL || (!want_host && !want_serv))
        return NLIBC_EAI_FAIL;
    if (want_host && (flags & NI_NAMEREQD))
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
    if (want_host && inet_ntop(sa->sa_family, raw, host, hostlen) == NULL)
        return NLIBC_EAI_FAIL;
    // The service name would come from the guest's /etc/services; numeric is
    // what every caller here asks for (NI_NUMERICSERV) and what a port with no
    // entry gets anyway.
    if (want_serv)
        snprintf(serv, servlen, "%u", port);
    return 0;
}

// getifaddrs enumerates the HOST's interfaces -- en0, the Mac's addresses --
// and that is the RIGHT answer here, which is not obvious and was got wrong
// once: this used to refuse with ENOSYS to keep the Mac's interfaces from
// being reported as the guest's.
//
// But AOK has no interfaces of its own. It has no network stack: a guest
// socket is a host socket (fs/sock.c), so the addresses a guest program can
// bind to and the routes its packets take are the host's. The kernel already
// says so out loud -- fs/proc/net.c builds /proc/net/dev, /proc/net/if_inet6
// and /proc/net/route by calling this very function -- so `cat /proc/net/dev`
// in the guest already lists en0 and awdl0. Refusing here did not hide the
// host's interfaces; it only made the native `ipaddr` disagree with /proc.
//
// A straight passthrough is therefore the same data /proc/net/dev is built
// from, reached the same way, and struct ifaddrs is the host's own layout on
// both sides of the call since native code is compiled against these headers.
int nlibc_getifaddrs(struct ifaddrs **ifap) {
    if (ifap == NULL) {
        errno = EINVAL;
        return -1;
    }
    *ifap = NULL;
    // The real one: this file is built with NATIVE_LIBC_NO_REDIRECT, so the
    // name is not the macro that brought us here.
    return getifaddrs(ifap);
}
void nlibc_freeifaddrs(struct ifaddrs *ifa) {
    if (ifa != NULL)
        freeifaddrs(ifa);
}

// ------------------------------------------------------- the hostent family
//
// The OLDER resolver interface, and it was reaching the HOST's while
// getaddrinfo directly above already asked the guest's -- one binary, two
// resolvers, free to disagree. zsh's zsh/net/tcp and zsh/zftp are the callers:
// `ztcp host port` resolves through getipnodebyname, because config.h defines
// HAVE_GETIPNODEBYNAME and that makes zsh_getipnodebyname a plain #define onto
// the host's (Src/Modules/tcp.c compiles its own portable version out), and the
// bare `ztcp` session listing reverse-resolves each end with gethostbyaddr.
// zftp adds getprotobyname, which is at the end of this file with the other
// /etc database readers.
//
// Everything here is built ON the machinery above rather than beside it, so
// there is one resolver in this file and one precedence rule: a numeric
// address as itself, then the guest's /etc/hosts, then the guest's
// /etc/resolv.conf nameservers over guest sockets.
//
// getipnodebyname and freehostent had to move TOGETHER, and that is the
// load-bearing part of this block rather than an aside. getipnodebyname's
// contract is that the CALLER frees the result with freehostent; route one and
// not the other and the program hands a pointer from one allocator to the
// other's free path. That is heap corruption rather than a wrong answer -- the
// failure this file is least equipped to debug. gethostbyname and gethostbyaddr
// have the opposite contract (a buffer the caller must NOT free), so both
// shapes exist below and freehostent has to tell them apart, which is the only
// reason `heap` is a field rather than an assumption.

#define NLIBC_DNS_TYPE_PTR 12

// "1.2.0.192.in-addr.arpa", or the nibble-reversed ip6.arpa form -- the name a
// reverse lookup actually asks for.
static bool nlibc_reverse_name(const void *raw, int family, char *out, size_t out_size) {
    const uint8_t *b = raw;
    if (family == AF_INET)
        return (size_t) snprintf(out, out_size, "%u.%u.%u.%u.in-addr.arpa",
                b[3], b[2], b[1], b[0]) < out_size;
    if (family != AF_INET6 || out_size < 74)
        return false;
    static const char hex[] = "0123456789abcdef";
    size_t at = 0;
    for (int i = 15; i >= 0; i--) {
        out[at++] = hex[b[i] & 0x0f];
        out[at++] = '.';
        out[at++] = hex[(b[i] >> 4) & 0x0f];
        out[at++] = '.';
    }
    snprintf(out + at, out_size - at, "ip6.arpa");
    return true;
}

// The first PTR record in the answer section whose owner is the name asked.
// The same walk nlibc_dns_answers does, reading a NAME out of the rdata rather
// than an address -- and with the same protection, since every name goes
// through nlibc_dns_decode_name, whose backward-only pointer rule is what stops
// a crafted reply from spinning here.
static bool nlibc_dns_ptr_answer(const uint8_t *msg, size_t len, const char *qname,
        char *out, size_t out_size) {
    if (len < 12)
        return false;
    unsigned qdcount = ((unsigned) msg[4] << 8) | msg[5];
    unsigned ancount = ((unsigned) msg[6] << 8) | msg[7];
    size_t pos = 12;
    for (unsigned i = 0; i < qdcount; i++) {
        if (!nlibc_dns_decode_name(msg, len, pos, NULL, 0, &pos))
            return false;
        pos += 4;   // QTYPE + QCLASS
        if (pos > len)
            return false;
    }
    for (unsigned i = 0; i < ancount && pos < len; i++) {
        char owner[256];
        if (!nlibc_dns_decode_name(msg, len, pos, owner, sizeof(owner), &pos))
            return false;
        if (pos + 10 > len)
            return false;
        unsigned type = ((unsigned) msg[pos] << 8) | msg[pos + 1];
        unsigned class = ((unsigned) msg[pos + 2] << 8) | msg[pos + 3];
        unsigned rdlen = ((unsigned) msg[pos + 8] << 8) | msg[pos + 9];
        size_t rdata = pos + 10;
        pos = rdata + rdlen;
        if (pos > len)
            return false;
        if (class != NLIBC_DNS_CLASS_IN || type != NLIBC_DNS_TYPE_PTR ||
                strcasecmp(owner, qname) != 0)
            continue;
        size_t ignored = 0;
        if (nlibc_dns_decode_name(msg, len, rdata, out, out_size, &ignored))
            return true;
    }
    return false;
}

// The GUEST's /etc/hosts in the reverse direction. It comes before DNS here for
// the same reason it does in getaddrinfo: an entry in that file is how a guest
// overrides a name, and a resolver that consulted it only one way round would
// honour the override for connect and ignore it for display.
static bool nlibc_hosts_reverse(const void *raw, int family, char *out, size_t out_size) {
    FILE *f = nlibc_fopen("/etc/hosts", "r");
    if (f == NULL)
        return false;
    size_t width = family == AF_INET6 ? 16 : 4;
    char line[512];
    bool found = false;
    while (!found && fgets(line, sizeof(line), f) != NULL) {
        char *hash = strchr(line, '#');
        if (hash != NULL)
            *hash = '\0';
        char *save = NULL;
        char *addr = strtok_r(line, " \t\r\n", &save);
        if (addr == NULL)
            continue;
        uint8_t bytes[16];
        int af = 0;
        memset(bytes, 0, sizeof(bytes));
        if (nlibc_parse_numeric(addr, family, bytes, &af) != 0 || af != family)
            continue;
        if (memcmp(bytes, raw, width) != 0)
            continue;
        char *name = strtok_r(NULL, " \t\r\n", &save);
        if (name != NULL) {
            snprintf(out, out_size, "%s", name);
            found = true;
        }
    }
    fclose(f);
    return found;
}

// One hostent and everything it points at, in a single block: the heap form is
// then one calloc and one free, and the static form one thread-local object
// rather than a scatter of buffers with different lifetimes. `he` is FIRST on
// purpose -- freehostent is handed that address and casts back.
struct nlibc_hostent {
    struct hostent he;
    bool heap;
    char name[256];
    char *aliases[1];
    char *addrs[NLIBC_DNS_MAX_ADDRS + 1];
    uint8_t raw[NLIBC_DNS_MAX_ADDRS][16];
};

static struct hostent *nlibc_hostent_fill(struct nlibc_hostent *h, const char *name,
        int family, const struct nlibc_addr *addrs, int count) {
    int width = family == AF_INET6 ? 16 : 4;
    if (count > NLIBC_DNS_MAX_ADDRS)
        count = NLIBC_DNS_MAX_ADDRS;
    snprintf(h->name, sizeof(h->name), "%s", name);
    h->aliases[0] = NULL;
    for (int i = 0; i < count; i++) {
        memcpy(h->raw[i], addrs[i].raw, (size_t) width);
        h->addrs[i] = (char *) h->raw[i];
    }
    h->addrs[count] = NULL;
    h->he.h_name = h->name;
    h->he.h_aliases = h->aliases;
    h->he.h_addrtype = family;
    h->he.h_length = width;
    h->he.h_addr_list = h->addrs;
    return &h->he;
}

// name -> addresses, through nlibc_getaddrinfo so the precedence rule lives in
// exactly one place. A hostent carries ONE address family, so a lookup that
// asked for AF_UNSPEC keeps whichever family came back first -- and A is
// emitted before AAAA above, which is the same order a caller walking the list
// would have used anyway.
static int nlibc_host_addrs(const char *name, int family, struct nlibc_addr *out,
        int max, int *out_family, int *error_num) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    int err = nlibc_getaddrinfo(name, NULL, &hints, &res);
    if (err != 0) {
        *error_num = err == NLIBC_EAI_AGAIN ? TRY_AGAIN : HOST_NOT_FOUND;
        return -1;
    }
    int count = 0, af = 0;
    for (struct addrinfo *ai = res; ai != NULL && count < max; ai = ai->ai_next) {
        if (af == 0)
            af = ai->ai_family;
        if (ai->ai_family != af || ai->ai_addr == NULL)
            continue;
        memset(&out[count], 0, sizeof(out[count]));
        out[count].family = af;
        if (af == AF_INET)
            memcpy(out[count].raw, &((struct sockaddr_in *) ai->ai_addr)->sin_addr, 4);
        else
            memcpy(out[count].raw, &((struct sockaddr_in6 *) ai->ai_addr)->sin6_addr, 16);
        count++;
    }
    nlibc_freeaddrinfo(res);
    if (count == 0) {
        *error_num = HOST_NOT_FOUND;
        return -1;
    }
    *out_family = af;
    return count;
}

// The reverse direction, shared by gethostbyaddr and getipnodebyaddr.
static bool nlibc_host_name(const void *addr, int family, char *out, size_t out_size,
        int *error_num) {
    if (nlibc_hosts_reverse(addr, family, out, out_size))
        return true;
    struct nlibc_resolv rc;
    nlibc_read_resolv_conf(&rc);
    char qname[80];
    if (rc.server_count == 0 || !nlibc_reverse_name(addr, family, qname, sizeof(qname))) {
        *error_num = HOST_NOT_FOUND;
        return false;
    }
    uint8_t reply[2048];
    ssize_t n = nlibc_dns_transact(&rc, qname, NLIBC_DNS_TYPE_PTR, reply, sizeof(reply));
    if (n < 0 || !nlibc_dns_ptr_answer(reply, (size_t) n, qname, out, out_size)) {
        *error_num = n == NLIBC_EAI_AGAIN ? TRY_AGAIN : HOST_NOT_FOUND;
        return false;
    }
    return true;
}

// The static-buffer half of the family. Thread-local rather than file-scope,
// which is the lesson getopt taught one level up in this file: a native program
// is a function on a task's thread, so "one copy per process" means two guest
// processes sharing a result.
static __thread struct nlibc_hostent nlibc_he;

struct hostent *nlibc_gethostbyname2(const char *name, int af) {
    if (name == NULL || (af != AF_INET && af != AF_INET6)) {
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    struct nlibc_addr addrs[NLIBC_DNS_MAX_ADDRS];
    int family = af, error_num = HOST_NOT_FOUND;
    int count = nlibc_host_addrs(name, af, addrs, NLIBC_DNS_MAX_ADDRS,
            &family, &error_num);
    if (count < 0) {
        h_errno = error_num;
        return NULL;
    }
    memset(&nlibc_he, 0, sizeof(nlibc_he));
    return nlibc_hostent_fill(&nlibc_he, name, family, addrs, count);
}

struct hostent *nlibc_gethostbyname(const char *name) {
    return nlibc_gethostbyname2(name, AF_INET);
}

struct hostent *nlibc_gethostbyaddr(const void *addr, socklen_t len, int af) {
    size_t width = af == AF_INET6 ? 16 : 4;
    if (addr == NULL || (af != AF_INET && af != AF_INET6) || len != (socklen_t) width) {
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }
    char name[256];
    int error_num = HOST_NOT_FOUND;
    if (!nlibc_host_name(addr, af, name, sizeof(name), &error_num)) {
        h_errno = error_num;
        return NULL;
    }
    struct nlibc_addr one;
    memset(&one, 0, sizeof(one));
    one.family = af;
    memcpy(one.raw, addr, width);
    memset(&nlibc_he, 0, sizeof(nlibc_he));
    return nlibc_hostent_fill(&nlibc_he, name, af, &one, 1);
}

// The caller-frees half. `flags` is accepted and not acted on: AI_V4MAPPED and
// AI_ALL ask for a v4 answer dressed up as v6, which this resolver does not
// synthesise, so AF_INET6 here means real AAAA records or nothing. Said out
// loud because silently mapping would be the more dangerous of the two.
struct hostent *nlibc_getipnodebyname(const char *name, int af, int flags,
        int *error_num) {
    int discard = 0;
    if (error_num == NULL)
        error_num = &discard;
    (void) flags;
    *error_num = HOST_NOT_FOUND;
    if (name == NULL || (af != AF_INET && af != AF_INET6))
        return NULL;
    struct nlibc_addr addrs[NLIBC_DNS_MAX_ADDRS];
    int family = af;
    int count = nlibc_host_addrs(name, af, addrs, NLIBC_DNS_MAX_ADDRS,
            &family, error_num);
    if (count < 0)
        return NULL;
    struct nlibc_hostent *h = calloc(1, sizeof(*h));
    if (h == NULL) {
        *error_num = NO_RECOVERY;
        return NULL;
    }
    h->heap = true;
    return nlibc_hostent_fill(h, name, family, addrs, count);
}

struct hostent *nlibc_getipnodebyaddr(const void *addr, size_t len, int af,
        int *error_num) {
    int discard = 0;
    if (error_num == NULL)
        error_num = &discard;
    *error_num = HOST_NOT_FOUND;
    size_t width = af == AF_INET6 ? 16 : 4;
    if (addr == NULL || (af != AF_INET && af != AF_INET6) || len != width)
        return NULL;
    char name[256];
    if (!nlibc_host_name(addr, af, name, sizeof(name), error_num))
        return NULL;
    struct nlibc_hostent *h = calloc(1, sizeof(*h));
    if (h == NULL) {
        *error_num = NO_RECOVERY;
        return NULL;
    }
    h->heap = true;
    struct nlibc_addr one;
    memset(&one, 0, sizeof(one));
    one.family = af;
    memcpy(one.raw, addr, width);
    return nlibc_hostent_fill(h, name, af, &one, 1);
}

// Frees what getipnodeby* returned and NOTHING else. The `heap` flag is what
// makes that safe: a hostent from gethostbyname is the thread-local above, and
// a caller that passes it here -- which the API says it must not, and which a
// program porting between the two families will do sooner or later -- gets a
// no-op instead of free() on a static object.
void nlibc_freehostent(struct hostent *he) {
    if (he == NULL)
        return;
    struct nlibc_hostent *h = (struct nlibc_hostent *) he;   // he is the first member
    if (h->heap)
        free(h);
}

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

// Indexed by HOST signal number, and PER TASK.
//
// This was a plain static, with a note saying that two native programs running
// at once share other things too. It stopped being defensible when a native
// shell started spawning native children: zsh ignores SIGQUIT for itself at
// startup and then asks `sigaction(SIGQUIT, NULL, &act)` whether it was ALREADY
// ignored, to decide whether to record an inherited `trap -- '' QUIT`. Shared,
// the answer it got was its own parent's, so every subshell reported a trap
// that a real forked subshell does not have -- and, less visibly, one shell's
// SIG_IGN was another's disposition for every signal.
//
// __thread rather than per-task-struct because a native program IS a host
// thread here, and everything else the shim keeps for it (the delivery
// re-entry guard, the held set) is reached the same way.
// SA_SIGINFO handlers, which take (sig, siginfo_t *, void *). Kept beside the
// one-argument table rather than replacing it so that every "is something
// installed here" test in this file keeps working unchanged: a three-argument
// handler puts a marker in the one-argument table and the real function here.
typedef void (*nlibc_sigaction_handler)(int, siginfo_t *, void *);

struct nlibc_sigtable {
    nlibc_sighandler h[NSIG];
    nlibc_sigaction_handler h3[NSIG];
};

// PER TASK, not per thread, and that distinction is the whole point.
//
// It was __thread, on the reasoning that a native program IS a host thread.
// True of the shells; false of anything with a runtime. A native program can
// create threads -- nlibc_pthread_create hands each one the creator's task --
// and then the thread that INSTALLS a handler is not the thread that reaches
// a checkpoint and delivers it. With the table per thread the deliverer sees
// an empty one, decides no signal here is interesting, and takes nothing: the
// signal stays pending forever.
//
// Measured: tokio registers its SIGCHLD handler from one thread and waits in
// the I/O driver on another, so a native Rust program that spawned a child
// never learned it had exited. It hung rather than failing, which is the
// worse half.
//
// Per task is also what the shells wanted all along -- two of them are two
// TASKS, so they still cannot see each other's dispositions.
static pthread_mutex_t nlibc_sigtable_lock = PTHREAD_MUTEX_INITIALIZER;

static struct nlibc_sigtable *nlibc_sigtable(void) {
    // The fallback for code reached with no task at all. Per thread because
    // there is nothing else to hang it on, and unreachable from a native
    // program, which always has one.
    static __thread struct nlibc_sigtable no_task;
    if (current == NULL)
        return &no_task;
    struct nlibc_sigtable *t = __atomic_load_n(&current->native_sigtable,
                                               __ATOMIC_ACQUIRE);
    if (t != NULL)
        return t;
    // The lock covers creation only. Entries are plain aligned pointer stores
    // afterwards: a thread installing a handler while a sibling delivers reads
    // either the old function or the new one, never a torn value, and a
    // program that needs those two ordered has to order them itself -- which
    // is exactly as true on a real kernel.
    pthread_mutex_lock(&nlibc_sigtable_lock);
    t = current->native_sigtable;
    if (t == NULL) {
        t = calloc(1, sizeof(*t));
        __atomic_store_n(&current->native_sigtable, t, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&nlibc_sigtable_lock);
    return t != NULL ? t : &no_task;
}

#define nlibc_handlers      (nlibc_sigtable()->h)
#define nlibc_info_handlers (nlibc_sigtable()->h3)

// Never called. Its address is the marker, and taking one keeps the compiler
// from folding it onto something else.
static void nlibc_siginfo_marker_fn(int sig) { (void) sig; }
#define NLIBC_SIGINFO_MARKER (&nlibc_siginfo_marker_fn)

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

static void nlibc_update_held_signals(void);
static sigset_t_ nlibc_shim_held_signals(void);

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
    int err = nlibc_rt_sigprocmask(ours ? SIG_BLOCK_ : SIG_UNBLOCK_,
            (sigset_t_) 1 << (guest_sig - 1), NULL);
    // The held set just changed; the kernel needs to know, or a wait would go
    // on ignoring a signal this program now handles.
    nlibc_update_held_signals();
    return err;
}

int nlibc_sigaction(int host_sig, const struct sigaction *act, struct sigaction *oact) {
    if (host_sig <= 0 || host_sig >= NSIG)
        return nlibc_fail(_EINVAL);
    int guest_sig = nlibc_signal_to_guest(host_sig);
    if (guest_sig == 0)
        return nlibc_fail(_EINVAL);

    if (oact != NULL) {
        memset(oact, 0, sizeof(*oact));
        if (nlibc_handlers[host_sig] == NLIBC_SIGINFO_MARKER) {
            oact->sa_sigaction = nlibc_info_handlers[host_sig];
            oact->sa_flags = SA_SIGINFO;
        } else {
            oact->sa_handler = nlibc_handlers[host_sig];
        }
    }
    if (act == NULL)
        return 0;

    // SA_SIGINFO used to be refused, on the grounds that there was no siginfo
    // to hand host code. There was: rt_sigtimedwait writes one, and this file
    // was passing NULL for it. Refusing it stopped anything built on
    // signal_hook -- which is to say tokio's process and signal drivers, and
    // so every async Rust program that spawns a child.
    bool want_info = (act->sa_flags & SA_SIGINFO) != 0;
    nlibc_sighandler entry = want_info
            ? (act->sa_sigaction != NULL ? NLIBC_SIGINFO_MARKER : SIG_DFL)
            : act->sa_handler;
    // SIG_DFL and SIG_IGN are spelled in sa_handler even when SA_SIGINFO is
    // set, and they are not function pointers to call.
    if (want_info && (act->sa_handler == SIG_DFL || act->sa_handler == SIG_IGN))
        entry = act->sa_handler;

    int err = nlibc_set_disposition(guest_sig, entry);
    if (err < 0)
        return nlibc_fail(err);
    nlibc_handlers[host_sig] = entry;
    // AFTER the table is written, not before. nlibc_set_disposition also
    // updates the held set, but it runs first and computes it from a table
    // that does not yet contain this handler -- so the held set was always one
    // registration behind. Invisible in a shell, which installs several and
    // has the next one paper over the last, and fatal for a program with only
    // one: tokio installs a single SIGCHLD handler, the held set stayed empty,
    // and so a blocking poll in the guest was never interrupted for it. It
    // waited for a child that had already exited.
    nlibc_update_held_signals();
    nlibc_info_handlers[host_sig] = entry == NLIBC_SIGINFO_MARKER
            ? act->sa_sigaction : NULL;
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
    nlibc_update_held_signals();   // same ordering fix as nlibc_sigaction
    return previous;
}

// exec, for the stand-in that has to imitate one (see nlibc_exec_standin).
// A real exec resets every CAUGHT signal to its default and preserves the
// ignored ones, and the stand-in has to do it by hand because the C handlers
// it is still carrying belong to the program that is supposed to be gone.
//
// Not cosmetic. A shell's SIGCHLD handler is a reaper: leave it installed and
// the checkpoint in that wait loop can call it, it reaps the exec'd program
// with its own wait3, and the stand-in's wait comes back ECHILD with the
// status gone -- the same "reaped out from under the bookkeeping that needed
// it" the loop exists to prevent, one layer down. SIG_IGN survives, which is
// what makes `nohup cmd` still mean what it says.
static void nlibc_exec_reset_handlers(void) {
    for (int host_sig = 1; host_sig < NSIG; host_sig++) {
        nlibc_sighandler h = nlibc_handlers[host_sig];
        if (h != NULL && h != SIG_DFL && h != SIG_IGN)
            nlibc_signal(host_sig, SIG_DFL);
    }
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
    sigset_t_ prev_prog = current != NULL ? current->native_prog_blocked : 0;
    sigset_t_ old = 0;
    int err = nlibc_rt_sigprocmask(guest_how, guest_set, oldset != NULL ? &old : NULL);
    if (err < 0)
        return nlibc_fail(err);

    // What the PROGRAM believes its mask is, which is the requested set before
    // the forcing above -- the kernel's own mask has the shim's blocking mixed
    // into it. Tracked so a signal the program deliberately blocked goes on not
    // interrupting its waits, while one the shim blocked behind its back still
    // does (struct task's native_held).
    if (current != NULL && set != NULL) {
        sigset_t_ want = nlibc_sigset_to_guest(set);
        if (guest_how == SIG_BLOCK_)
            current->native_prog_blocked |= want;
        else if (guest_how == SIG_UNBLOCK_)
            current->native_prog_blocked &= ~want;
        else
            current->native_prog_blocked = want;
        nlibc_update_held_signals();
    }

    // OLDSET is the program's own mask, NOT the kernel's, and that distinction
    // is the whole point rather than a nicety. Handing back the kernel's mask
    // told the program it had blocked every signal the shim was holding -- and
    // bash, which saves a mask and restores it (`sigprocmask(SIG_BLOCK, &set,
    // &oset)` then SIG_SETMASK of oset), thereby adopted the shim's blocking as
    // its own. From then on nothing could interrupt it: the held set was empty,
    // ^C at a prompt did not end the read it was waiting in, and the shell only
    // noticed at the next keystroke, which the interrupted read then ate.
    (void) old;
    if (oldset != NULL)
        nlibc_sigset_from_guest(prev_prog, oldset);
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
// The guest's siginfo as rt_sigtimedwait writes it. Two shapes, because it
// follows the TASK's ABI -- the same split the timespec below has. Only the
// fields a handler can act on are mirrored; the rest is padding this side
// never reads, which is why each is followed by the size the kernel asserts.
struct nlibc_guest_siginfo64 {
    sdword_t sig, sig_errno, code, _pad0;
    sdword_t pid;
    dword_t  uid;
    sdword_t status;
} __attribute__((packed));

struct nlibc_guest_siginfo32 {
    sdword_t sig, sig_errno, code;
    sdword_t pid;
    dword_t  uid;
    sdword_t status;
} __attribute__((packed));

// What a handler is actually given. Filled from whichever shape the task uses.
struct nlibc_taken_signal {
    int sig, sig_errno, code, status;
    int pid;
    unsigned uid;
    bool have_info;
};

// rt_sigtimedwait's second argument is the siginfo out-pointer, and it used to
// be passed as NULL here -- which is why SA_SIGINFO could only be refused. The
// information was always available; nothing was asking for it.
static int nlibc_sigtake_info(sigset_t_ set, bool wait,
                              struct nlibc_taken_signal *info) {
    NATIVE_FRAME;
    if (info != NULL)
        memset(info, 0, sizeof(*info));
    guest_addr_t guest_set = native_scratch_put(&set, sizeof(set));
    if (guest_set == 0)
        return _ENOMEM;

    bool abi64 = current != NULL && guest_abi_is_64bit(current->abi);
    size_t info_size = abi64 ? sizeof(struct nlibc_guest_siginfo64)
                             : sizeof(struct nlibc_guest_siginfo32);
    // The kernel writes the FULL siginfo, not just the prefix mirrored above,
    // so the scratch has to be the size it expects or the write runs off the
    // end of the allocation.
    guest_addr_t guest_info = 0;
    if (info != NULL) {
        guest_info = native_scratch_alloc(abi64 ? 128 : 128);
        if (guest_info == 0)
            return _ENOMEM;
    }

    guest_addr_t guest_timeout = 0;
    if (!wait) {
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
    int res = (int) native_syscall(NATIVE_SYS_rt_sigtimedwait, guest_set, guest_info,
            guest_timeout, sizeof(sigset_t_));
    if (res > 0 && info != NULL) {
        union {
            struct nlibc_guest_siginfo64 a;
            struct nlibc_guest_siginfo32 b;
        } raw;
        memset(&raw, 0, sizeof(raw));
        if (native_scratch_get(&raw, guest_info, info_size) == 0) {
            if (abi64) {
                info->sig = raw.a.sig; info->sig_errno = raw.a.sig_errno;
                info->code = raw.a.code; info->pid = raw.a.pid;
                info->uid = raw.a.uid;  info->status = raw.a.status;
            } else {
                info->sig = raw.b.sig; info->sig_errno = raw.b.sig_errno;
                info->code = raw.b.code; info->pid = raw.b.pid;
                info->uid = raw.b.uid;  info->status = raw.b.status;
            }
            info->have_info = true;
        }
    }
    return res;
}

// The plain form, for callers with nothing to do with the detail. Note the
// timeout above is the only timespec in this file whose width follows the
// TASK's ABI rather than being fixed at 64-bit: sys_rt_sigtimedwait_common
// reads it through guest_abi_is_64bit(current->abi), where utimensat, ppoll
// and pselect all take the 64-bit shape whatever the task is.
static int nlibc_sigtake(sigset_t_ set, bool wait) {
    return nlibc_sigtake_info(set, wait, NULL);
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
    (void) nlibc_deliver_signals_count();
}

// How many handlers ran. The count is what nlibc_sigsuspend needs: "a signal
// arrived while you were waiting" is exactly the thing sigsuspend reports, and
// a caller that goes straight back to sleep without being told would never see
// the state the handler changed.
int nlibc_deliver_signals_count(void) {
    // A handler makes syscalls of its own, each of which checkpoints again.
    // __thread because two native programs can be live at once: a process-wide
    // flag meant one shell delivering suppressed the other's delivery entirely.
    static __thread bool delivering;
    int ran = 0;
    if (delivering)
        return 0;

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
        return 0;

    delivering = true;
    for (;;) {
        struct nlibc_taken_signal taken;
        int guest_sig = nlibc_sigtake_info(ours, false, &taken);
        if (guest_sig <= 0)
            break;
        int host_sig = nlibc_signal_to_host(guest_sig);
        nlibc_sighandler handler = host_sig > 0 && host_sig < NSIG ?
                nlibc_handlers[host_sig] : NULL;
        if (handler != NULL && handler != SIG_DFL && handler != SIG_IGN) {
            // The mask a handler is entered with is the mask it comes back
            // with, because on Linux a handler runs on a signal FRAME and
            // sigreturn reinstalls the mask saved in it -- so a sigprocmask
            // the handler makes does not outlive the handler. Here the
            // handler is a plain C call with no frame and no sigreturn, so
            // nothing undid it, and one very ordinary handler leaked a
            // permanently wrong mask:
            //
            //   zsh's zhandler (Src/signals.c) blocks everything on entry,
            //   remembers the old mask, and calls signal_setmask(oldmask) on
            //   the way out. When it runs during nlibc_sigsuspend the "old
            //   mask" it reads is sigsuspend's TEMPORARY mask -- for a shell
            //   waiting on a job that is {SIGINT}, added by zsh's
            //   signal_suspend so a ^C reaches the foreground command first.
            //   The setmask then wrote SIGINT into the task's real blocked
            //   set, sigsuspend restored only its own bookkeeping, and SIGINT
            //   stayed blocked for the rest of the shell's life. Masks are
            //   inherited across spawn AND exec, so every child from the
            //   second external command onwards ignored ^C: `sh -c 'kill -INT
            //   $$'` returned 0 instead of 130, and the process survived.
            //
            // Restoring here rather than in sigsuspend closes the class: any
            // wait that installs a temporary mask (pselect, ppoll,
            // epoll_pwait, sigtimedwait) had the same hole, and so did any
            // handler that changes the mask deliberately.
            sigset_t_ entry_mask = 0;
            bool have_mask = nlibc_rt_sigprocmask(SIG_BLOCK_, 0, &entry_mask) == 0;
            sigset_t_ entry_prog = current != NULL ? current->native_prog_blocked : 0;
            if (handler == NLIBC_SIGINFO_MARKER) {
                // The three-argument form. si_code travels as the guest wrote
                // it: the SI_* values agree between the two on every code a
                // handler tests (SI_USER 0, SI_QUEUE -1, and CLD_* 1..6).
                //
                // The context argument is NULL, and that is the one thing
                // this cannot supply: a ucontext_t describes the interrupted
                // machine state, and a native program's handler is a plain
                // call at a checkpoint rather than a frame the kernel built.
                // Nothing that runs here reads it -- signal_hook and tokio
                // ignore it -- and a fabricated one would be worse than none.
                siginfo_t info;
                memset(&info, 0, sizeof(info));
                info.si_signo = host_sig;
                if (taken.have_info) {
                    info.si_errno = taken.sig_errno;
                    info.si_code = taken.code;
                    info.si_pid = (pid_t) taken.pid;
                    info.si_uid = (uid_t) taken.uid;
                    info.si_status = taken.status;
                }
                nlibc_sigaction_handler h3 = nlibc_info_handlers[host_sig];
                if (h3 != NULL)
                    h3(host_sig, &info, NULL);
            } else {
                handler(host_sig);
            }
            if (have_mask)
                nlibc_rt_sigprocmask(SIG_SETMASK_, entry_mask, NULL);
            if (current != NULL) {
                current->native_prog_blocked = entry_prog;
                nlibc_update_held_signals();
            }
            ran++;
        }
    }
    delivering = false;
    return ran;
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

// Darwin has no `environ` symbol to link against in a library: <crt_externs.h>
// hands out _NSGetEnviron(), and that is what a runtime built for Apple calls.
// It answers about the host process, so it is the same bug as getenv() wearing
// a different name, and gets the same slot. Likewise _NSGetArgv/_NSGetArgc,
// which is how Rust's std::env::args() reads its arguments -- unrouted, a
// native program saw the iSH app's command line instead of its own.
char ***nlibc_NSGetEnviron(void) { return native_env_slot(); }
char ***nlibc_NSGetArgv(void) { return native_argv_slot(); }
int *nlibc_NSGetArgc(void) { return native_argc_slot(); }

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

// ------------------------------------------------------------ option parsing
//
// getopt keeps its scanning position in process-global storage: one optind,
// one optarg, one `place` for the whole of libSystem. A native program is a C
// function on a task's thread, not a process, so two of them parsing their
// argv at the same moment read and clobber each other's. Six concurrent
// `smallclue ssh-keygen -q -t ed25519 -N "" -f /tmp/cN` lost the -f in three
// of them and fell back to prompting for a filename -- the parse had walked
// off into another invocation's argv.
//
// So the state is __thread and the scanning functions are ours. Routing only
// the five variables would not have been enough: `place`, and the permutation
// bookkeeping behind getopt_long, are just as global and just as fatal.
//
// The semantics are the platform's, not an approximation of them. These
// programs parse real user command lines, and a getopt that differs in some
// corner is worse than a shared one -- it fails quietly on an unusual flag
// instead of loudly. nlibc_getopt is the 4.4BSD getopt macOS actually ships
// (non-permuting, `::` optional arguments, optreset); nlibc_getopt_long is the
// FreeBSD getopt_long macOS ships next to it, built GNU_COMPATIBLE as macOS
// builds it (permuting by default, leading `+`/`-`, `W;`, abbreviation
// matching, GNU's message wording). They are two different vintages of two
// different files and they keep SEPARATE scanning positions -- which is
// libSystem's arrangement, not a simplification of it.
//
// Which they are was settled by measurement, not by reading: see
// tools/difftest-native-getopt.sh, which lifts the marked region below
// verbatim out of this file, links it beside the host's own getopt, and runs
// the same argv through both. Three of the behaviours encoded here contradict
// the obvious reading of the BSD sources and were found only that way.

// >>> native getopt: BEGIN (extracted verbatim by tools/difftest-native-getopt.sh)
static __thread char *nlibc_optarg_v;
static __thread int nlibc_optind_v = 1;
static __thread int nlibc_opterr_v = 1;
static __thread int nlibc_optopt_v = '?';
static __thread int nlibc_optreset_v;

char **nlibc_optargp(void)   { return &nlibc_optarg_v; }
int   *nlibc_optindp(void)   { return &nlibc_optind_v; }
int   *nlibc_opterrp(void)   { return &nlibc_opterr_v; }
int   *nlibc_optoptp(void)   { return &nlibc_optopt_v; }
int   *nlibc_optresetp(void) { return &nlibc_optreset_v; }

// Shorthands so the ported bodies below read like their originals.
#define nl_optarg   nlibc_optarg_v
#define nl_optind   nlibc_optind_v
#define nl_opterr   nlibc_opterr_v
#define nl_optopt   nlibc_optopt_v
#define nl_optreset nlibc_optreset_v

#define NL_BADCH  ((int) '?')
#define NL_EMSG   ""

// argv[0]'s basename. libSystem's getopt uses the same helper rather than the
// global __progname, which is what makes the message name the applet ("ls:")
// and not the app.
static const char *nlibc_getopt_progname(const char *nargv0) {
    const char *tmp;
    if (nargv0 == NULL)
        return "";
    tmp = strrchr(nargv0, '/');
    if (tmp != NULL)
        tmp++;
    else
        tmp = nargv0;
    return tmp;
}

// --- getopt: the 4.4BSD one, ported from Libc's gen/FreeBSD/getopt.c --------
//
// Non-permuting on purpose: BSD getopt stops at the first non-option, and
// every applet here was written against that. `place` is the per-call scanning
// pointer into a clustered argument (-abc), and is the state that made six
// concurrent ssh-keygens step on each other.
int nlibc_getopt(int nargc, char *const nargv[], const char *ostr) {
    static __thread const char *place = NL_EMSG;    /* option letter processing */
    const char *oli;                                /* option letter list index */

    if (nl_optreset || *place == 0) {       /* update scanning pointer */
        nl_optreset = 0;
        /* The bound is tested BEFORE the load, which the original does the
         * other way round. Observationally identical -- the value read is
         * discarded on that path -- but the original reads one past the end
         * of argv whenever it is re-entered after a missing-argument error,
         * which leaves optind at nargc + 1. */
        if (nl_optind >= nargc ||
                (place = nargv[nl_optind], *place++ != '-')) {
            /* Argument is absent or is not an option */
            place = NL_EMSG;
            return -1;
        }
        nl_optopt = *place++;
        if (nl_optopt == '-' && *place == 0) {
            /* "--" => end of options */
            ++nl_optind;
            place = NL_EMSG;
            return -1;
        }
        if (nl_optopt == 0) {
            /* Solitary '-', treat as a '-' option if the program (eg su) is
               looking for it. */
            place = NL_EMSG;
            if (strchr(ostr, '-') == NULL)
                return -1;
            nl_optopt = '-';
        }
    } else {
        nl_optopt = *place++;
    }

    /* See if option letter is one the caller wanted... */
    if (nl_optopt == ':' || (oli = strchr(ostr, nl_optopt)) == NULL) {
        if (*place == 0)
            ++nl_optind;
        if (nl_opterr && *ostr != ':')
            fprintf(nlibc_stderr(), "%s: illegal option -- %c\n",
                    nlibc_getopt_progname(nargv[0]), nl_optopt);
        return NL_BADCH;
    }

    /* Does this option need an argument? */
    if (oli[1] != ':') {
        /* don't need argument */
        nl_optarg = NULL;
        if (*place == 0)
            ++nl_optind;
    } else {
        /* Option-argument is either the rest of this argument or the entire
           next argument. */
        if (*place) {
            nl_optarg = (char *) place;
        } else if (oli[2] == ':') {
            /* GNU extension: for optional arguments, if the rest of the
               argument is empty we return NULL rather than consuming the next
               word. */
            nl_optarg = NULL;
        } else if (nargc > ++nl_optind) {
            nl_optarg = nargv[nl_optind];
        } else {
            /* Option-argument absent. optarg is CLEARED and optind lands one
             * past the end (nargc + 1), not on it -- both observable, and
             * both differed from a straight reading of the 4.4BSD source
             * until the differential harness caught them. A caller that
             * inspects optarg after a '?' would otherwise see the PREVIOUS
             * option's argument. */
            place = NL_EMSG;
            nl_optarg = NULL;
            ++nl_optind;
            if (*ostr == ':')
                return (int) ':';
            if (nl_opterr)
                fprintf(nlibc_stderr(),
                        "%s: option requires an argument -- %c\n",
                        nlibc_getopt_progname(nargv[0]), nl_optopt);
            return NL_BADCH;
        }
        place = NL_EMSG;
        ++nl_optind;
    }
    return nl_optopt;                       /* return option letter */
}

// --- getopt_long: the FreeBSD one macOS ships, built GNU_COMPATIBLE ---------
//
// This one permutes: non-options are shuffled to the end of argv so options
// after them are still seen. That is GNU behaviour, and it is what ssh, sftp
// and scp are written against. The permutation bookkeeping
// (nonopt_start/nonopt_end) is per-parse state, so it is per-thread here too.
//
// GNU_COMPATIBLE is not cosmetic and the differential harness proved each of
// these against the host rather than assuming them:
//   - an EXACT long-option match wins even when earlier entries partially
//     matched, so {veal,vest,ve} + `--ve` returns ve instead of "ambiguous";
//   - partial matches that agree on has_arg/flag/val are not ambiguous;
//   - a lone "-" is always a non-option, even with '-' in the optstring;
//   - `--flag=x` on a no_argument option returns '?' even under a leading
//     ':' optstring, where the non-GNU build returns ':';
//   - the messages are GNU's wording ("unrecognized option `--x'"), which is
//     also what the guest's own Linux tools print.
//
// The one deliberate divergence is the program name in those messages. The
// host's getopt_long uses the process's __progname, which inside AOK is the
// app -- every applet would introduce itself as "ish". These use argv[0]'s
// basename, so ls says "ls", which is both what the guest expects and what
// the host's plain getopt already does.

#define NL_FLAG_PERMUTE   0x01  /* permute non-options to the end of argv */
#define NL_FLAG_ALLARGS   0x02  /* treat non-options as args to option "-1" */
#define NL_FLAG_LONGONLY  0x04  /* operate as getopt_long_only */

#define NL_NO_PREFIX  (-1)
#define NL_D_PREFIX   0
#define NL_DD_PREFIX  1
#define NL_W_PREFIX   2

#define NL_INORDER  ((int) 1)
#define NL_BADARG   ((*options == ':') ? (int) ':' : (int) '?')
#define NL_PRINT_ERROR ((nl_opterr) && (*options != ':'))

static __thread const char *nl_place = NL_EMSG;  /* option letter processing */
static __thread int nl_nonopt_start = -1;  /* first non option argument */
static __thread int nl_nonopt_end = -1;    /* first option after non options */
static __thread int nl_dash_prefix = NL_NO_PREFIX;

static const char nl_recargchar[] = "option requires an argument -- %c";
/* P1003.2's wording, which is what the FreeBSD getopt_long macOS ships uses
   and what nlibc_getopt above prints. "unknown option -- %c" is the OpenBSD
   file's string and leaked in from reading it as reference; the differential
   harness caught the difference in stderr text. */
static const char nl_illoptchar[] = "illegal option -- %c";
static const char nl_gnuoptchar[] = "invalid option -- %c";
static const char nl_recargstring[] = "option `%s%s' requires an argument";
static const char nl_ambig[] = "option `%s%.*s' is ambiguous";
static const char nl_noarg[] = "option `%s%.*s' doesn't allow an argument";
static const char nl_illoptstring[] = "unrecognized option `%s%s'";

static void nl_warnx(const char *prog, const char *fmt, ...) {
    va_list ap;
    fprintf(nlibc_stderr(), "%s: ", nlibc_getopt_progname(prog));
    va_start(ap, fmt);
    vfprintf(nlibc_stderr(), fmt, ap);
    va_end(ap);
    fputc('\n', nlibc_stderr());
}

/* Greatest common divisor of a and b. */
static int nl_gcd(int a, int b) {
    int c = a % b;
    while (c != 0) {
        a = b;
        b = c;
        c = a % b;
    }
    return b;
}

/*
 * Exchange the block from nonopt_start to nonopt_end with the block from
 * nonopt_end to opt_end (keeping the same order of arguments in each block).
 */
static void nl_permute_args(int panonopt_start, int panonopt_end, int opt_end,
        char *const *nargv) {
    int cstart, cyclelen, i, j, ncycle, nnonopts, nopts, pos;
    char *swap;

    nnonopts = panonopt_end - panonopt_start;
    nopts = opt_end - panonopt_end;
    ncycle = nl_gcd(nnonopts, nopts);
    cyclelen = (opt_end - panonopt_start) / ncycle;

    for (i = 0; i < ncycle; i++) {
        cstart = panonopt_end + i;
        pos = cstart;
        for (j = 0; j < cyclelen; j++) {
            if (pos >= panonopt_end)
                pos -= nnonopts;
            else
                pos += nopts;
            swap = nargv[pos];
            ((char **) nargv)[pos] = nargv[cstart];
            ((char **) nargv)[cstart] = swap;
        }
    }
}

/*
 * Parse long options in argc/argv argument vector.
 * Returns -1 if short_too is set and the option does not match long_options.
 */
static int nl_parse_long_options(char *const *nargv, const char *options,
        const struct option *long_options, int *idx, int short_too,
        int flags) {
    const char *current_argv;
    const char *current_dash;
    char *has_equal;
    size_t current_argv_len;
    int i, match, exact_match, second_partial_match;

    current_argv = nl_place;
    switch (nl_dash_prefix) {
        case NL_D_PREFIX:  current_dash = "-";   break;
        case NL_DD_PREFIX: current_dash = "--";  break;
        case NL_W_PREFIX:  current_dash = "-W "; break;
        default:           current_dash = "";    break;
    }
    match = -1;
    exact_match = 0;
    second_partial_match = 0;

    nl_optind++;

    if ((has_equal = strchr(current_argv, '=')) != NULL) {
        /* argument found (--option=arg) */
        current_argv_len = (size_t) (has_equal - current_argv);
        has_equal++;
    } else {
        current_argv_len = strlen(current_argv);
    }

    for (i = 0; long_options[i].name; i++) {
        /* find matching long option */
        if (strncmp(current_argv, long_options[i].name, current_argv_len))
            continue;

        if (strlen(long_options[i].name) == current_argv_len) {
            /* exact match */
            match = i;
            exact_match = 1;
            break;
        }
        /*
         * If this is a known short option, don't allow a partial match of a
         * single character.
         */
        if (short_too && current_argv_len == 1)
            continue;

        if (match == -1) {              /* first partial match */
            match = i;
        } else if ((flags & NL_FLAG_LONGONLY) ||
                long_options[i].has_arg != long_options[match].has_arg ||
                long_options[i].flag != long_options[match].flag ||
                long_options[i].val != long_options[match].val) {
            second_partial_match = 1;
        }
    }
    if (!exact_match && second_partial_match) {
        /* ambiguous abbreviation */
        if (NL_PRINT_ERROR)
            nl_warnx(nargv[0], nl_ambig, current_dash,
                    (int) current_argv_len, current_argv);
        nl_optopt = 0;
        return NL_BADCH;
    }
    if (match != -1) {          /* option found */
        if (long_options[match].has_arg == no_argument && has_equal) {
            if (NL_PRINT_ERROR)
                nl_warnx(nargv[0], nl_noarg, current_dash,
                        (int) current_argv_len, current_argv);
            /* XXX: GNU sets optopt to val regardless of flag */
            if (long_options[match].flag == NULL)
                nl_optopt = long_options[match].val;
            else
                nl_optopt = 0;
            return NL_BADCH;
        }
        if (long_options[match].has_arg == required_argument ||
                long_options[match].has_arg == optional_argument) {
            if (has_equal)
                nl_optarg = has_equal;
            else if (long_options[match].has_arg == required_argument)
                /* optional argument doesn't use next nargv */
                nl_optarg = nargv[nl_optind++];
        }
        if ((long_options[match].has_arg == required_argument) &&
                nl_optarg == NULL) {
            /*
             * Missing argument; leading ':' indicates no error should be
             * generated.
             */
            if (NL_PRINT_ERROR)
                nl_warnx(nargv[0], nl_recargstring, current_dash,
                        current_argv);
            /* XXX: GNU sets optopt to val regardless of flag */
            if (long_options[match].flag == NULL)
                nl_optopt = long_options[match].val;
            else
                nl_optopt = 0;
            --nl_optind;
            return NL_BADARG;
        }
    } else {                    /* unknown option */
        if (short_too) {
            --nl_optind;
            return -1;
        }
        if (NL_PRINT_ERROR)
            nl_warnx(nargv[0], nl_illoptstring, current_dash, current_argv);
        nl_optopt = 0;
        return NL_BADCH;
    }
    if (idx)
        *idx = match;
    if (long_options[match].flag) {
        *long_options[match].flag = long_options[match].val;
        return 0;
    }
    return long_options[match].val;
}

/*
 * Parse argc/argv argument vector.  Called by the user-level routines below.
 */
static int nl_getopt_internal(int nargc, char *const *nargv,
        const char *options, const struct option *long_options, int *idx,
        int flags) {
    const char *oli;                        /* option letter list index */
    int optchar, short_too;
    /* Not cached: POSIXLY_CORRECT is read on every call, so a program that
     * sets it between parses is obeyed -- and, here, so that one task's
     * environment cannot decide another's parse. */
    int posixly_correct;

    if (options == NULL)
        return -1;

    /*
     * Disable GNU extensions if POSIXLY_CORRECT is set or the options string
     * begins with a '+'.
     */
    posixly_correct = (nlibc_getenv("POSIXLY_CORRECT") != NULL);
    if (*options == '-')
        flags |= NL_FLAG_ALLARGS;
    else if (posixly_correct || *options == '+')
        flags &= ~NL_FLAG_PERMUTE;
    if (*options == '+' || *options == '-')
        options++;

    /*
     * XXX Some GNU programs (like cvs) set optind to 0 instead of using
     * XXX optreset.  Work around this braindamage.
     */
    if (nl_optind == 0)
        nl_optind = nl_optreset = 1;

    nl_optarg = NULL;
    if (nl_optreset)
        nl_nonopt_start = nl_nonopt_end = -1;
start:
    if (nl_optreset || !*nl_place) {        /* update scanning pointer */
        nl_optreset = 0;
        if (nl_optind >= nargc) {           /* end of argument vector */
            nl_place = NL_EMSG;
            if (nl_nonopt_end != -1) {
                /* do permutation, if we have to */
                nl_permute_args(nl_nonopt_start, nl_nonopt_end, nl_optind,
                        nargv);
                nl_optind -= nl_nonopt_end - nl_nonopt_start;
            } else if (nl_nonopt_start != -1) {
                /*
                 * If we skipped non-options, set optind to the first of them.
                 */
                nl_optind = nl_nonopt_start;
            }
            nl_nonopt_start = nl_nonopt_end = -1;
            return -1;
        }
        /* A lone "-" is a non-option even when '-' appears in the optstring:
         * GNU_COMPATIBLE drops the strchr(options, '-') escape the plain BSD
         * build has. nlibc_getopt above keeps it, because the plain getopt
         * macOS ships keeps it. */
        if (*(nl_place = nargv[nl_optind]) != '-' || nl_place[1] == '\0') {
            nl_place = NL_EMSG;             /* found non-option */
            if (flags & NL_FLAG_ALLARGS) {
                /*
                 * GNU extension: return non-option as argument to option 1
                 */
                nl_optarg = nargv[nl_optind++];
                return NL_INORDER;
            }
            if (!(flags & NL_FLAG_PERMUTE)) {
                /*
                 * If no permutation wanted, stop parsing at first non-option.
                 */
                return -1;
            }
            /* do permutation */
            if (nl_nonopt_start == -1) {
                nl_nonopt_start = nl_optind;
            } else if (nl_nonopt_end != -1) {
                nl_permute_args(nl_nonopt_start, nl_nonopt_end, nl_optind,
                        nargv);
                nl_nonopt_start = nl_optind -
                        (nl_nonopt_end - nl_nonopt_start);
                nl_nonopt_end = -1;
            }
            nl_optind++;
            /* process next argument */
            goto start;
        }
        if (nl_nonopt_start != -1 && nl_nonopt_end == -1)
            nl_nonopt_end = nl_optind;

        /*
         * If we have "-" do nothing, if "--" we are done.
         */
        if (nl_place[1] != '\0' && *++nl_place == '-' && nl_place[1] == '\0') {
            nl_optind++;
            nl_place = NL_EMSG;
            /*
             * We found an option (--), so if we skipped non-options, we have
             * to permute.
             */
            if (nl_nonopt_end != -1) {
                nl_permute_args(nl_nonopt_start, nl_nonopt_end, nl_optind,
                        nargv);
                nl_optind -= nl_nonopt_end - nl_nonopt_start;
            }
            nl_nonopt_start = nl_nonopt_end = -1;
            return -1;
        }
    }

    /*
     * Check long options if:
     *  1) we were passed some
     *  2) the arg is not just "-"
     *  3) either the arg starts with -- or we are getopt_long_only()
     */
    if (long_options != NULL && nl_place != nargv[nl_optind] &&
            (*nl_place == '-' || (flags & NL_FLAG_LONGONLY))) {
        short_too = 0;
        nl_dash_prefix = NL_D_PREFIX;
        if (*nl_place == '-') {
            nl_place++;                     /* --foo long option */
            nl_dash_prefix = NL_DD_PREFIX;
        } else if (*nl_place != ':' && strchr(options, *nl_place) != NULL) {
            short_too = 1;                  /* could be short option too */
        }

        optchar = nl_parse_long_options(nargv, options, long_options, idx,
                short_too, flags);
        if (optchar != -1) {
            nl_place = NL_EMSG;
            return optchar;
        }
    }

    if ((optchar = (int) *nl_place++) == (int) ':' ||
            (optchar == (int) '-' && *nl_place != '\0') ||
            (oli = strchr(options, optchar)) == NULL) {
        /*
         * If the user specified "-" and '-' isn't listed in options, return -1
         * (non-option) as per POSIX.  Otherwise, it is an unknown option
         * character (or ':').
         */
        if (optchar == (int) '-' && *nl_place == '\0')
            return -1;
        if (!*nl_place)
            ++nl_optind;
        if (NL_PRINT_ERROR)
            nl_warnx(nargv[0], posixly_correct ? nl_illoptchar : nl_gnuoptchar,
                    optchar);
        nl_optopt = optchar;
        return NL_BADCH;
    }
    if (long_options != NULL && optchar == 'W' && oli[1] == ';') {
        /* -W long-option */
        if (*nl_place) {                    /* no space */
            /* NOTHING */;
        } else if (++nl_optind >= nargc) {  /* no arg */
            nl_place = NL_EMSG;
            if (NL_PRINT_ERROR)
                nl_warnx(nargv[0], nl_recargchar, optchar);
            nl_optopt = optchar;
            return NL_BADARG;
        } else {                            /* white space */
            nl_place = nargv[nl_optind];
        }
        nl_dash_prefix = NL_W_PREFIX;
        optchar = nl_parse_long_options(nargv, options, long_options, idx, 0,
                flags);
        nl_place = NL_EMSG;
        return optchar;
    }
    if (*++oli != ':') {                    /* doesn't take argument */
        if (!*nl_place)
            ++nl_optind;
    } else {                                /* takes (optional) argument */
        nl_optarg = NULL;
        if (*nl_place) {                    /* no white space */
            nl_optarg = (char *) nl_place;
        } else if (oli[1] != ':') {         /* arg not optional */
            if (++nl_optind >= nargc) {     /* no arg */
                nl_place = NL_EMSG;
                if (NL_PRINT_ERROR)
                    nl_warnx(nargv[0], nl_recargchar, optchar);
                nl_optopt = optchar;
                return NL_BADARG;
            }
            nl_optarg = nargv[nl_optind];
        }
        nl_place = NL_EMSG;
        ++nl_optind;
    }
    /* dump back option letter */
    return optchar;
}

int nlibc_getopt_long(int nargc, char *const *nargv, const char *options,
        const struct option *long_options, int *idx) {
    return nl_getopt_internal(nargc, nargv, options, long_options, idx,
            NL_FLAG_PERMUTE);
}

int nlibc_getopt_long_only(int nargc, char *const *nargv, const char *options,
        const struct option *long_options, int *idx) {
    return nl_getopt_internal(nargc, nargv, options, long_options, idx,
            NL_FLAG_PERMUTE | NL_FLAG_LONGONLY);
}
// <<< native getopt: END

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

// The effective half of the same family, as setresuid/setresgid with -1 for the
// real and saved ids -- which is what glibc's seteuid and setegid are.
//
// setegid is the one that was actually live, and it is worth saying how it got
// missed, because the evidence points the wrong way. Both functions appear in
// uidswap.c, which is server-only and is NOT linked; that is a per-FILE reading
// and it is wrong for setegid, because openbsd-compat/bsd-setres_id.c ALSO
// calls it -- config.h defines BROKEN_SETREGID, so its setresgid() takes the
// `setegid(egid); setgid(rgid);` branch, and misc.c's subprocess() (the runner
// behind KnownHostsCommand, `Match exec` and ssh-keygen -Y) pulls that in.
// setgid was routed and setegid was not, so a pair that must move together was
// half on the guest and half on the host. Its twin seteuid really is dead --
// config.h's SETEUID_BREAKS_SETUID skips the matching branch in setresuid --
// and is routed anyway, since a family split is exactly how this happened.
//
// The consequence of leaving them is not a wrong answer but a wrong SUBJECT: a
// native "child" is a host thread of this app, so a host setegid that succeeded
// would move the credentials of every other guest task at once.
int nlibc_seteuid(uid_t uid) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_setresuid,
            (uid_t) -1, uid, (uid_t) -1));
}
int nlibc_setegid(gid_t gid) {
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_setresgid,
            (gid_t) -1, gid, (gid_t) -1));
}

int nlibc_setgroups(int size, const gid_t *list) {
    NATIVE_FRAME;
    if (size < 0)
        return nlibc_fail(_EINVAL);
    guest_addr_t guest_list = 0;
    if (size > 0) {
        if (list == NULL)
            return nlibc_fail(_EFAULT);
        // The guest's gid_t is 32 bits, as Darwin's is -- the same assumption
        // nlibc_getgroups makes going the other way.
        guest_list = native_scratch_put(list, (size_t) size * sizeof(uint32_t));
        if (guest_list == 0)
            return nlibc_fail(_ENOMEM);
    }
    return (int) nlibc_ret(native_syscall(NATIVE_SYS_setgroups, size, guest_list));
}

int nlibc_initgroups(const char *user, gid_t group) {
    (void) user; (void) group;
    return 0;   // no supplementary groups to set; succeeding is the honest no-op
}

// /etc/passwd and /etc/group, read from the GUEST. One entry is cached at a
// time, which is what getpwuid's contract allows -- the returned pointer is
// only valid until the next call.
static __thread char nlibc_pw_line[512];
static __thread struct passwd nlibc_pw;
static __thread char nlibc_gr_line[512];
static __thread struct group nlibc_gr;
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

// nlibc_pw and its line buffer below are PER TASK. getpwuid's contract lets it
// return a pointer to storage the next call may reuse -- per THREAD, which is
// what callers assume. Shared across native programs it is worse than stale:
// two concurrent ssh runs crashed the whole app about 8 times in 20, strlen(0)
// inside pwcopy(), because one program's scan cleared the struct while the
// other was copying out of it. Native bash crashed the same way through
// get_current_user_info.

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

// The _r forms, which is what a thread-aware runtime calls -- Rust's std uses
// getpwuid_r for home_dir(), and unrouted it read the Mac's /etc/passwd and
// handed a native program the developer's home directory.
//
// Built on the same per-task scan rather than a second parser: the copy out to
// the caller's buffer is the only part that differs, and duplicating the
// /etc/passwd parsing is how the two would come to disagree.
static int nlibc_pw_copy_out(struct passwd *src, struct passwd *out, char *buf,
                             size_t buflen, struct passwd **result) {
    *result = NULL;
    if (src == NULL)
        return 0;               // not found is success with a NULL result
    // Zeroed rather than filled field by field, because Darwin's struct passwd
    // carries pw_change, pw_class and pw_expire that Linux's does not and that
    // /etc/passwd has no column for. Leaving them as the caller's stack is how
    // a BSD-shaped consumer ends up reading a garbage pointer.
    memset(out, 0, sizeof(*out));
    const char *fields[] = { src->pw_name, src->pw_passwd, src->pw_gecos,
                             src->pw_dir, src->pw_shell,
#ifdef __APPLE__
                             src->pw_class,
#endif
    };
    char **slots[] = { &out->pw_name, &out->pw_passwd, &out->pw_gecos,
                       &out->pw_dir, &out->pw_shell,
#ifdef __APPLE__
                       // Empty rather than NULL: BSD callers pass it to
                       // strlen without checking.
                       &out->pw_class,
#endif
    };
    size_t used = 0;
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        const char *f = fields[i] != NULL ? fields[i] : "";
        size_t n = strlen(f) + 1;
        if (used + n > buflen)
            return ERANGE;      // the caller's cue to retry with more room
        memcpy(buf + used, f, n);
        *slots[i] = buf + used;
        used += n;
    }
    out->pw_uid = src->pw_uid;
    out->pw_gid = src->pw_gid;
    *result = out;
    return 0;
}

int nlibc_getpwuid_r(uid_t uid, struct passwd *out, char *buf, size_t buflen,
                     struct passwd **result) {
    if (out == NULL || buf == NULL || result == NULL)
        return EINVAL;
    return nlibc_pw_copy_out(nlibc_getpwuid(uid), out, buf, buflen, result);
}

int nlibc_getpwnam_r(const char *name, struct passwd *out, char *buf,
                     size_t buflen, struct passwd **result) {
    if (out == NULL || buf == NULL || result == NULL)
        return EINVAL;
    return nlibc_pw_copy_out(nlibc_getpwnam(name), out, buf, buflen, result);
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

// uid/gid -> NAME, for anything that renders an owner rather than checking one.
//
// getpwuid and getgrgid were routed and these two were not, which is the same
// whole-family miss as execlp and setegid. What it would have cost is the
// `whoami` bug in a different costume: sftp-common.c's ls_file() renders a
// remote listing, so the SERVER's uid 501 would have been resolved against the
// MAC's account database and printed as the Mac user's name, with root matching
// only by coincidence.
//
// That branch is not reached in this build -- every ls_file() caller compiled
// in passes remote = 1, and the only remote = 0 caller is sftp-server.c, which
// meson excludes. Not reached is not the same as not wrong, and a native sshd
// brings sftp-server.c with it.
static __thread char nlibc_uid_name[64];
static __thread char nlibc_gid_name[64];

const char *nlibc_user_from_uid(uid_t uid, int nouser) {
    struct passwd *pw = nlibc_getpwuid(uid);
    if (pw != NULL && pw->pw_name != NULL) {
        snprintf(nlibc_uid_name, sizeof(nlibc_uid_name), "%s", pw->pw_name);
        return nlibc_uid_name;
    }
    if (nouser)
        return NULL;   // the contract: NULL rather than a number, if asked
    snprintf(nlibc_uid_name, sizeof(nlibc_uid_name), "%u", (unsigned) uid);
    return nlibc_uid_name;
}

const char *nlibc_group_from_gid(gid_t gid, int nogroup) {
    struct group *gr = nlibc_getgrgid(gid);
    if (gr != NULL && gr->gr_name != NULL) {
        snprintf(nlibc_gid_name, sizeof(nlibc_gid_name), "%s", gr->gr_name);
        return nlibc_gid_name;
    }
    if (nogroup)
        return NULL;
    snprintf(nlibc_gid_name, sizeof(nlibc_gid_name), "%u", (unsigned) gid);
    return nlibc_gid_name;
}

// Which groups a user belongs to, from the GUEST's /etc/group.
//
// nlibc_getgrent cannot answer this: its parser fills gr_mem with an empty
// list, so a walk over it would report the base gid and nothing else. So this
// reads the fourth field itself -- the comma-separated member list -- through
// the same index-keyed scan the getgrent walker uses, which re-reads the file
// once per group. That is O(n^2) over /etc/group and entirely fine at its size;
// it is also what makes the answer reflect the file as it is now.
//
// Only groupaccess.c calls this, and groupaccess.c is sshd's AllowGroups
// matching, so nothing in the current link reaches it. Written anyway for the
// same reason as openpty: the day sshd is compiled in, the host's answer would
// be the iOS account's group membership, and the failure would be silent and
// security-bearing.
struct nlibc_grouplist_key {
    size_t want, seen;
    gid_t gid;
    char members[512];
};

static bool nlibc_gr_list_match(char **f, size_t n, const void *keyv) {
    struct nlibc_grouplist_key *key = (struct nlibc_grouplist_key *) keyv;
    if (n < 3)
        return false;
    if (key->seen++ != key->want)
        return false;
    key->gid = (gid_t) strtoul(f[2], NULL, 10);
    snprintf(key->members, sizeof(key->members), "%s", n > 3 && f[3] ? f[3] : "");
    return true;
}

int nlibc_getgrouplist(const char *name, int basegid, int *groups, int *ngroups) {
    if (name == NULL || ngroups == NULL) {
        errno = EFAULT;
        return -1;
    }
    int room = *ngroups, found = 0;
    // The primary group is always first, whether or not /etc/group lists it.
    if (groups != NULL && found < room)
        groups[found] = basegid;
    found++;

    char line[512];
    for (size_t index = 0; ; index++) {
        struct nlibc_grouplist_key key = { index, 0, 0, { 0 } };
        if (!nlibc_scan_db("/etc/group", line, sizeof(line),
                           nlibc_gr_list_match, &key))
            break;              // walked off the end of the file
        if ((int) key.gid == basegid)
            continue;           // already reported
        bool member = false;
        for (char *cursor = key.members; cursor != NULL && *cursor != '\0'; ) {
            char *comma = strchr(cursor, ',');
            if (comma != NULL)
                *comma = '\0';
            if (strcmp(cursor, name) == 0) {
                member = true;
                break;
            }
            cursor = comma != NULL ? comma + 1 : NULL;
        }
        if (!member)
            continue;
        bool already = false;   // a gid listed twice is one membership
        for (int i = 0; groups != NULL && i < found && i < room; i++)
            if (groups[i] == (int) key.gid)
                already = true;
        if (already)
            continue;
        if (groups != NULL && found < room)
            groups[found] = (int) key.gid;
        found++;
    }

    // getgrouplist's contract: *ngroups is always the number needed, and the
    // return is -1 when that exceeds the room offered.
    bool overflow = found > room;
    *ngroups = found;
    return overflow ? -1 : found;
}

// Password hashing, and the one entry here that is a REFUSAL rather than an
// implementation.
//
// Darwin's crypt() is DES-only: it cannot produce the $6$ SHA-512, $5$, $1$ or
// $2b$ hashes a Linux /etc/shadow holds, so its answer could never match the
// guest's stored one. That is not a case of the host reading the wrong file --
// it is the host computing a different function.
//
// The only caller in the tree is openbsd-compat/xcrypt.c, reached from
// auth-passwd.c, which is sshd's password check and is not compiled in. So
// nothing calls this today; it is here because the gate reads the archive, and
// because a native sshd is planned. Failing closed is the only safe answer for
// an authentication primitive: a plausible-looking wrong hash that HAPPENED to
// match a locked account's "*" or "!" field would be an authentication bypass,
// so NULL it is -- what glibc returns for an unsupported salt, and what
// xcrypt's callers must already tolerate.
//
// Making this real means implementing the crypt algorithms over CommonCrypto's
// SHA-2, against the guest's /etc/shadow. That belongs with the sshd work, not
// ahead of it.
char *nlibc_crypt(const char *key, const char *salt) {
    (void) key; (void) salt;
    errno = ENOSYS;
    return NULL;
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

// sync(2). The guest's answer is a known constant rather than a call: AOK's
// arm64 table has `[81] = syscall_success_stub, // sync` (kernel/calls.c, and
// the same at 36 on i386 and 162 on amd64), because a fakefs has no device
// buffers of its own to flush. So this returns having done nothing, which is
// precisely what the guest's own sync(2) does.
//
// It is written here rather than dispatched because the number is not in the
// generated header: tools/gen-native-syscalls.py excludes the stub handlers on
// purpose, and success_stub is one of them. That exclusion is right for
// "unimplemented" and merely inconvenient for "deliberately a no-op", which is
// what this comment exists to record -- the value below is not a guess about
// the guest, it is the guest's table read out.
//
// Unrouted this was the HOST's sync(2), which flushes the DEVICE's buffers:
// `zmodload zsh/files; sync` in a guest shell made the Mac write out its
// filesystems.
void nlibc_sync(void) {
}

// The extended-attribute family, all eight of Darwin's spellings.
//
// The guest's answer is again a constant, and again read out of the table
// rather than guessed: kernel/calls.c's arm64 entry is
// `[5 ... 16] = (syscall_t) sys_xattr_stub` -- twelve numbers, the whole
// get/set/list/remove x plain/l/f matrix -- and kernel/fs.c:2763 makes
// sys_xattr_stub `return _ENOTSUP;` unconditionally, for any path. AOK's fakefs
// stores no xattrs, so "this filesystem does not support them" is the true
// answer and not a refusal to answer.
//
// These are absent from the generated header for a different reason than sync
// above: the numbers arrive as a RANGE initialiser and the generator's regex
// only matches single-index entries. Worth knowing before assuming a missing
// NATIVE_SYS_* means a missing syscall.
//
// What this replaces is the sharpest result the triage turned up, and it is
// worth keeping: `zmodload zsh/attr` (one line in a .zshrc) plus
// `zlistattr /Users` returned SUCCESS in a guest where /Users does not exist,
// while `zlistattr /etc/debian_version` failed on a file the guest could list.
// The module was resolving guest paths against the Mac's root -- a read and
// write primitive aimed at the device, two words from any prompt.
//
// If AOK ever implements xattrs, these move to native_syscall together with the
// kernel change; the l-forms are Darwin's XATTR_NOFOLLOW option rather than
// separate entry points, which is why there are eight names here and twelve in
// the table.
#if defined(__linux__)
// Linux's xattr calls take neither a position nor an options word, and split
// "do not follow symlinks" into separate l-prefixed entry points. Every one of
// these fails with ENOTSUP exactly as the Darwin set does -- see the note in
// native_libc.h -- so this is the same answer in the platform's own shape.
ssize_t nlibc_getxattr(const char *path, const char *name, void *value, size_t size) {
    (void) path; (void) name; (void) value; (void) size;
    return nlibc_fail(_ENOTSUP);
}
ssize_t nlibc_fgetxattr(int fd_no, const char *name, void *value, size_t size) {
    (void) fd_no; (void) name; (void) value; (void) size;
    return nlibc_fail(_ENOTSUP);
}
int nlibc_setxattr(const char *path, const char *name, const void *value,
        size_t size, int flags) {
    (void) path; (void) name; (void) value; (void) size; (void) flags;
    return nlibc_fail(_ENOTSUP);
}
int nlibc_fsetxattr(int fd_no, const char *name, const void *value,
        size_t size, int flags) {
    (void) fd_no; (void) name; (void) value; (void) size; (void) flags;
    return nlibc_fail(_ENOTSUP);
}
ssize_t nlibc_listxattr(const char *path, char *names, size_t size) {
    (void) path; (void) names; (void) size;
    return nlibc_fail(_ENOTSUP);
}
ssize_t nlibc_flistxattr(int fd_no, char *names, size_t size) {
    (void) fd_no; (void) names; (void) size;
    return nlibc_fail(_ENOTSUP);
}
int nlibc_removexattr(const char *path, const char *name) {
    (void) path; (void) name;
    return nlibc_fail(_ENOTSUP);
}
int nlibc_fremovexattr(int fd_no, const char *name) {
    (void) fd_no; (void) name;
    return nlibc_fail(_ENOTSUP);
}
#else
ssize_t nlibc_getxattr(const char *path, const char *name, void *value,
        size_t size, uint32_t position, int options) {
    (void) path; (void) name; (void) value; (void) size;
    (void) position; (void) options;
    return nlibc_fail(_ENOTSUP);
}
ssize_t nlibc_fgetxattr(int fd_no, const char *name, void *value,
        size_t size, uint32_t position, int options) {
    (void) fd_no; (void) name; (void) value; (void) size;
    (void) position; (void) options;
    return nlibc_fail(_ENOTSUP);
}
int nlibc_setxattr(const char *path, const char *name, const void *value,
        size_t size, uint32_t position, int options) {
    (void) path; (void) name; (void) value; (void) size;
    (void) position; (void) options;
    return nlibc_fail(_ENOTSUP);
}
int nlibc_fsetxattr(int fd_no, const char *name, const void *value,
        size_t size, uint32_t position, int options) {
    (void) fd_no; (void) name; (void) value; (void) size;
    (void) position; (void) options;
    return nlibc_fail(_ENOTSUP);
}
ssize_t nlibc_listxattr(const char *path, char *names, size_t size, int options) {
    (void) path; (void) names; (void) size; (void) options;
    return nlibc_fail(_ENOTSUP);
}
ssize_t nlibc_flistxattr(int fd_no, char *names, size_t size, int options) {
    (void) fd_no; (void) names; (void) size; (void) options;
    return nlibc_fail(_ENOTSUP);
}
int nlibc_removexattr(const char *path, const char *name, int options) {
    (void) path; (void) name; (void) options;
    return nlibc_fail(_ENOTSUP);
}
int nlibc_fremovexattr(int fd_no, const char *name, int options) {
    (void) fd_no; (void) name; (void) options;
    return nlibc_fail(_ENOTSUP);
}
#endif

// nice(3). The host's renices the THREAD this emulator is running on -- so a
// guest background job would deprioritise the whole of iSH-AOK, while the
// guest's own view of its nice level (what guest `ps` and `nice` report) never
// moved. Wrong subject, in the same shape as the setegid entry further up.
//
// The request is issued to the GUEST so that it lands where a guest process's
// priority lives, and so it follows automatically if AOK ever grows real
// scheduling. The ANSWER is written here because AOK has no priorities today:
// kernel/resource.c's sys_setpriority does nothing and sys_getpriority returns
// a constant, since guest tasks run on host threads the app does not get to
// renice. The guest's nice level is therefore 0 before the call and 0 after it,
// and 0 is what a caller must be told.
//
// setpriority takes an ABSOLUTE nice value where nice() takes an increment. The
// two coincide only because the current value is fixed at 0; the day that stops
// being true, this has to read the current value first, and this paragraph is
// the note saying so.
int nlibc_nice(int incr) {
    // PRIO_PROCESS is 0 on the guest as on Darwin, and `who` 0 means "me".
    sqword_t res = native_syscall(NATIVE_SYS_setpriority, 0, 0, (dword_t) incr);
    if (res < 0)
        return nlibc_fail((int) res);
    return 0;
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
// klogctl(2): the GUEST's kernel ring buffer, which AOK answers itself.
//
// SmallCLUE's dmesg has had a real implementation all along, behind
// `#if defined(__linux__)`. A native program is compiled for the HOST, so that
// test was false and it fell to the #else -- printing "not supported on this
// platform" while the very same guest's /usr/bin/dmesg printed AOK's boot line
// perfectly, because AOK implements syslog(2). The platform test was asking
// about the compiler's target when what matters is which kernel the call
// reaches, and for a native program those are two different systems.
//
// Only the three READ types take a buffer; the rest (SIZE_BUFFER, CLEAR,
// CONSOLE_*) pass a null guest address, exactly as the guest's own dmesg does.
int nlibc_klogctl(int type, char *bufp, int len) {
    NATIVE_FRAME;
    if (len < 0)
        return nlibc_fail(_EINVAL);
    int reads_buffer = type == 2 || type == 3 || type == 4;  // READ, READ_ALL, READ_CLEAR
    guest_addr_t guest_buf = 0;
    if (reads_buffer) {
        if (bufp == NULL)
            return nlibc_fail(_EFAULT);
        guest_buf = native_scratch_alloc((size_t) len);
        if (guest_buf == 0 && len > 0)
            return nlibc_fail(_ENOMEM);
    }
    sqword_t res = native_syscall(NATIVE_SYS_syslog, type, guest_buf, len);
    if (reads_buffer && res > 0 &&
            native_scratch_get(bufp, guest_buf, (size_t) res) < 0)
        return nlibc_fail(_EFAULT);
    return (int) nlibc_ret(res);
}

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

// --------------------------------------------------------------- string bits
//
// strchrnul is a GNU extension. Darwin grew one, but only in iOS 18.4 and macOS
// 15.4, and this app deploys to iOS 15.0 -- so building against a current SDK
// turns the call into a WEAK import that is NULL on every older device, and
// calling it jumps to address 0.
//
// That is not hypothetical. Reported on an A10X iPad Pro (iPad7,2) running
// iOS 17.7.11: the app launched, and died the moment the terminal started.
//
//     jit_crash_fn                      <- EXC_BAD_ACCESS, "let it crash"
//     xdupmbstowcs2   xmbsrtowcs.c:173  <- end_or_backslash = strchrnul(p, ...)
//     remove_pattern  "${REPLY%/*}"
//     ... sourcing /usr/share/bash-completion/bash_completion from /etc/profile
//
// The same binary is fine on anything from 18.4 up, which is exactly what makes
// this the kind of bug that ships: it cannot be reproduced on a current device.
//
// bash carries its own replacement (lib/sh/strchrnul.c) for platforms without
// one, but its configure ran against an SDK that DECLARES the function, so
// generated/config.h says HAVE_STRCHRNUL and the replacement is never compiled.
// Answering it here instead fixes it for every native program at once, and for
// any future one, without a vendored-tree change: the force-included header
// renames the callers, so nothing reaches libSystem for it.
//
// Nothing else in the binary is gated above the deployment target -- all 455
// imported symbols were checked against the SDK's availability annotations, and
// strchrnul was the only one.
char *nlibc_strchrnul(const char *s, int c) {
    // A NUL search terminates at the NUL, which is the same answer either way.
    while (*s != '\0' && *s != (char) c)
        s++;
    return (char *) s;
}

// ----------------------------------------------------------- temporary names
//
// mkdtemp and mktemp share mkstemp's contract: the pattern is the caller's
// buffer and the name used has to be written back into it.
// The template is validated ONCE and refilled by position thereafter, because
// the six characters stop being "XXXXXX" the moment they are first filled.
// Checking the literal on every attempt -- as this did -- made the retry loops
// below unreachable: the alphabet is lowercase and digits, so a filled template
// can never match "XXXXXX" again, every retry failed EINVAL, and mkdtemp
// answered NULL for the first collision instead of trying a second name.
static bool nlibc_template_offset(const char *template, size_t *off) {
    size_t len = strlen(template);
    if (len < 6 || strcmp(template + len - 6, "XXXXXX") != 0)
        return false;
    *off = len - 6;
    return true;
}

// arc4random, not rand, and the difference is the whole point of the retry.
//
// rand() is one sequence for the whole host process, and a re-launched shell
// RESTORES ITS PARENT'S SEED on purpose: $RANDOM has to keep answering what a
// forked child would have answered, so AOK_ZSH_INHERIT carries the seed and the
// draw count and the child calls srand with them. Every other caller of rand()
// in that task then replays the parent's sequence too -- so a parent and its
// child asked for a temporary name and were handed the SAME one, measured as
// /tmp/zsht0rkmr in both. Collisions were not rare here, they were the norm,
// which is exactly when a retry loop matters.
//
// arc4random is not seeded from the guest and is not replayed, so the child
// draws its own names. It reaches the host untouched (the shim header does not
// rewrite it) and that is correct rather than a leak: entropy has no guest
// semantics to diverge from.
static void nlibc_fill_template_at(char *template, size_t off) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (int i = 0; i < 6; i++)
        template[off + i] = alphabet[arc4random_uniform(sizeof(alphabet) - 1)];
}

char *nlibc_mkdtemp(char *template) {
    if (template == NULL) {
        nlibc_fail(_EINVAL);
        return NULL;
    }
    size_t off;
    if (!nlibc_template_offset(template, &off)) {
        nlibc_fail(_EINVAL);
        return NULL;
    }
    for (int attempt = 0; attempt < 128; attempt++) {
        nlibc_fill_template_at(template, off);
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
    size_t off;
    if (!nlibc_template_offset(template, &off)) {
        nlibc_fail(_EINVAL);
        return NULL;
    }
    for (int attempt = 0; attempt < 128; attempt++) {
        nlibc_fill_template_at(template, off);
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

// ------------------------------------------------------- the login database
//
// getutxent and its relatives read the HOST's utmpx, and this is the leak that
// prints rather than merely misreports: with `log` or $WATCH set, a guest shell
// listed the MAC's sessions by name -- "mke has logged on console from ." and
// one line per open Terminal tab. Nothing in the guest put them there.
//
// The trap that makes this more than a redirection is the LAYOUT. The guest's
// /var/run/utmp is Linux's:
//
//   offset  0  int32   ut_type          (USER_PROCESS == 7)
//           4  int32   ut_pid
//           8  char    ut_line[32]
//          40  char    ut_id[4]
//          44  char    ut_user[32]
//          76  char    ut_host[256]
//         340  int32   ut_tv.tv_sec, then int32 tv_usec
//                                      384 bytes per record
//
// Darwin's struct utmpx is a different shape entirely -- ut_user first and 256
// bytes wide, ut_type after ut_pid -- so pointing the host's header at the
// guest's file parses nonsense. That is exactly what un-defining HAVE_GETUTXENT
// would have done, because zsh's fallback (Src/Modules/watch.c) fread()s the
// file straight into WATCH_STRUCT_UTMP. So the file is parsed here, by offset,
// and a Darwin struct utmpx is BUILT from it.
//
// The offsets are not derived again: they are the ones deps/smallclue/src/
// core.c's smallclueUtmpUserCount() already established and ships against, and
// the two readers agree field for field so a guest cannot see `uptime` and
// `log` disagree about who is logged in.
//
// Byte order is the guest's, and the guest's is the host's: every AOK guest ABI
// (i386, amd64, arm64, riscv64) is little-endian, as is every device this runs
// on, so the int32 fields are memcpy'd rather than assembled.

#define NLIBC_UTMP_RECORD    384
#define NLIBC_UTMP_OFF_TYPE    0
#define NLIBC_UTMP_OFF_PID     4
#define NLIBC_UTMP_OFF_LINE    8
#define NLIBC_UTMP_OFF_ID     40
#define NLIBC_UTMP_OFF_USER   44
#define NLIBC_UTMP_OFF_HOST   76
#define NLIBC_UTMP_OFF_TV    340
#define NLIBC_UTMP_LEN_LINE   32
#define NLIBC_UTMP_LEN_ID      4
#define NLIBC_UTMP_LEN_USER   32
#define NLIBC_UTMP_LEN_HOST  256

// Where a Linux guest keeps it. Both spellings, because /var/run is a symlink
// to /run on a systemd-era rootfs and a plain directory on others, and a
// distribution that has moved it should not silently read nothing.
static const char *const nlibc_utmp_paths[] = { "/var/run/utmp", "/run/utmp", NULL };

static __thread FILE *nlibc_utx_file;
static __thread struct utmpx nlibc_utx;

// The two numbering systems agree on every value except 3 and 4, which are
// SWAPPED: Linux has NEW_TIME 3 and OLD_TIME 4, Darwin has OLD_TIME 3 and
// NEW_TIME 4. Everything else -- EMPTY 0 through ACCOUNTING 9, and USER_PROCESS
// 7, the only one anything here tests -- lines up.
static short nlibc_utmp_type(int32_t guest_type) {
    switch (guest_type) {
        case 3:  return NEW_TIME;
        case 4:  return OLD_TIME;
        default: break;
    }
    if (guest_type < 0 || guest_type > 9)
        return EMPTY;
    return (short) guest_type;
}

static void nlibc_utmp_field(char *dst, size_t dst_size, const uint8_t *rec,
        size_t offset, size_t width) {
    size_t copy = width < dst_size - 1 ? width : dst_size - 1;
    memcpy(dst, rec + offset, copy);
    dst[copy] = '\0';
}

// Opening and reading are separate from the ENUMERATION STATE on purpose:
// getlogin below needs to walk the same file without disturbing a walk the
// program may be in the middle of, and a getutxent that saved and restored a
// position could not do that -- there is no position to save on a FILE*. So it
// gets a handle of its own instead.
static FILE *nlibc_utmp_open(void) {
    for (int i = 0; nlibc_utmp_paths[i] != NULL; i++) {
        FILE *f = nlibc_fopen(nlibc_utmp_paths[i], "r");
        if (f != NULL)
            return f;
    }
    return NULL;
}

static bool nlibc_utmp_next(FILE *f, struct utmpx *out) {
    uint8_t rec[NLIBC_UTMP_RECORD];
    if (f == NULL || fread(rec, sizeof(rec), 1, f) != 1)
        return false;
    int32_t type = 0, pid = 0, sec = 0, usec = 0;
    memcpy(&type, rec + NLIBC_UTMP_OFF_TYPE, sizeof(type));
    memcpy(&pid, rec + NLIBC_UTMP_OFF_PID, sizeof(pid));
    memcpy(&sec, rec + NLIBC_UTMP_OFF_TV, sizeof(sec));
    memcpy(&usec, rec + NLIBC_UTMP_OFF_TV + 4, sizeof(usec));
    memset(out, 0, sizeof(*out));
    out->ut_type = nlibc_utmp_type(type);
    out->ut_pid = (pid_t) pid;
    out->ut_tv.tv_sec = sec;
    out->ut_tv.tv_usec = usec;
    nlibc_utmp_field(out->ut_user, sizeof(out->ut_user), rec,
            NLIBC_UTMP_OFF_USER, NLIBC_UTMP_LEN_USER);
    nlibc_utmp_field(out->ut_line, sizeof(out->ut_line), rec,
            NLIBC_UTMP_OFF_LINE, NLIBC_UTMP_LEN_LINE);
    nlibc_utmp_field(out->ut_host, sizeof(out->ut_host), rec,
            NLIBC_UTMP_OFF_HOST, NLIBC_UTMP_LEN_HOST);
    memcpy(out->ut_id, rec + NLIBC_UTMP_OFF_ID, NLIBC_UTMP_LEN_ID);
    return true;
}

void nlibc_setutxent(void) {
    if (nlibc_utx_file != NULL)
        fclose(nlibc_utx_file);
    nlibc_utx_file = nlibc_utmp_open();
}

void nlibc_endutxent(void) {
    if (nlibc_utx_file != NULL) {
        fclose(nlibc_utx_file);
        nlibc_utx_file = NULL;
    }
}

struct utmpx *nlibc_getutxent(void) {
    // A real getutxent opens on first use; a caller that never calls setutxent
    // still gets the first record rather than nothing.
    if (nlibc_utx_file == NULL)
        nlibc_setutxent();
    return nlibc_utmp_next(nlibc_utx_file, &nlibc_utx) ? &nlibc_utx : NULL;
}

// Scan forward for a line, as the API says: from where the walk currently is,
// not from the start.
struct utmpx *nlibc_getutxline(const struct utmpx *want) {
    if (want == NULL)
        return NULL;
    for (struct utmpx *u = nlibc_getutxent(); u != NULL; u = nlibc_getutxent())
        if ((u->ut_type == LOGIN_PROCESS || u->ut_type == USER_PROCESS) &&
                strncmp(u->ut_line, want->ut_line, sizeof(u->ut_line)) == 0)
            return u;
    return NULL;
}

struct utmpx *nlibc_getutxid(const struct utmpx *want) {
    if (want == NULL)
        return NULL;
    for (struct utmpx *u = nlibc_getutxent(); u != NULL; u = nlibc_getutxent()) {
        switch (want->ut_type) {
            case RUN_LVL: case BOOT_TIME: case OLD_TIME: case NEW_TIME:
                if (u->ut_type == want->ut_type)
                    return u;
                break;
            case INIT_PROCESS: case LOGIN_PROCESS:
            case USER_PROCESS: case DEAD_PROCESS:
                if ((u->ut_type == INIT_PROCESS || u->ut_type == LOGIN_PROCESS ||
                     u->ut_type == USER_PROCESS || u->ut_type == DEAD_PROCESS) &&
                        memcmp(u->ut_id, want->ut_id, sizeof(u->ut_id)) == 0)
                    return u;
                break;
            default:
                return NULL;
        }
    }
    return NULL;
}

// Writing is refused rather than implemented. A record appended here would have
// to be built in the GUEST's layout, and nothing native has ever asked -- so an
// implementation would be untested code holding a lock on a file the guest's
// own login programs also write. Refusing is what a real system tells an
// unprivileged process anyway, and it is a failure the caller can see, which is
// the whole difference from appending to the DEVICE's database.
struct utmpx *nlibc_pututxline(const struct utmpx *line) {
    (void) line;
    errno = EPERM;
    return NULL;
}

// getlogin answered with the MAC's login session, and not even consistently
// with the host's own credentials: a probe running as uid 501 (mke) got "root"
// back. zsh takes this and makes it $LOGNAME (Src/params.c), so every native
// shell started with a $LOGNAME nobody in the guest chose.
//
// The algorithm is glibc's, over the guest's data: find the controlling
// terminal, then the utmp record that claims it. The fallback is the one place
// this differs, and deliberately -- glibc gives up when utmp has nothing, but
// an AOK guest usually has no utmp at all (neither test rootfs ships one), and
// "the passwd entry for the uid this task is running as" is both a true
// statement about the guest and the answer a user expects `su - aok` to change.
char *nlibc_getlogin(void) {
    static __thread char name[64];
    char *tty = nlibc_ttyname(STDIN_FILENO);
    if (tty != NULL) {
        const char *line = strrchr(tty, '/');
        line = line != NULL ? line + 1 : tty;
        // Its own handle, so this cannot move a getutxent walk the program is
        // in the middle of. See nlibc_utmp_open.
        FILE *f = nlibc_utmp_open();
        struct utmpx u;
        bool found = false;
        while (!found && nlibc_utmp_next(f, &u)) {
            if (u.ut_type == USER_PROCESS && u.ut_user[0] != '\0' &&
                    strncmp(u.ut_line, line, sizeof(u.ut_line)) == 0) {
                snprintf(name, sizeof(name), "%s", u.ut_user);
                found = true;
            }
        }
        if (f != NULL)
            fclose(f);
        if (found)
            return name;
    }
    struct passwd *pw = nlibc_getpwuid(nlibc_getuid());
    if (pw == NULL || pw->pw_name == NULL) {
        errno = ENXIO;
        return NULL;
    }
    snprintf(name, sizeof(name), "%s", pw->pw_name);
    return name;
}

int nlibc_getlogin_r(char *buf, size_t len) {
    if (buf == NULL || len == 0)
        return EINVAL;
    char *who = nlibc_getlogin();
    if (who == NULL)
        return ENXIO;
    if (strlen(who) + 1 > len)
        return ERANGE;
    memcpy(buf, who, strlen(who) + 1);
    return 0;
}

// ------------------------------------------------------------------ execve
//
// execv with the environment given explicitly rather than taken from the task.
// Same spawn-then-exit shape, and the same caveat: the pid changes.
int nlibc_execve(const char *path, char *const argv[], char *const envp[]) {
    if (path == NULL || argv == NULL)
        return nlibc_fail(_EFAULT);
    // native_spawn_opts, not native_spawn, and ONLY for the signal mask -- the
    // same reason nlibc_exec_common does it, and this function was the one
    // place that did not. Measured in the guest before the fix:
    //
    //   /bin/sh          -c 'exec grep SigBlk /proc/self/status' -> 00000000
    //   /AOK/native/zsh  -c 'exec grep SigBlk /proc/self/status' -> 08010001
    //
    // 0x08010001 is SIGHUP, SIGCHLD and SIGWINCH blocked in a program that
    // never asked -- the shim's own "block it and drain at a checkpoint"
    // holding, inherited by whatever the caller exec'd. zsh's zexecve goes
    // through execve rather than execv, so every external command a native zsh
    // ran started life with those three blocked.
    struct native_spawn_opts opts = { .pgid = NATIVE_SPAWN_PGID_INHERIT };
    nlibc_spawn_default_sigmask(&opts);
    dword_t pid = 0;
    int err = native_spawn_opts(path, argv,
            envp != NULL ? envp : native_env_vector(), &opts, &pid);
    if (err < 0)
        return nlibc_fail(err);
    nlibc_exec_standin(pid);
}

// --------------------------------------------------------------- sigsuspend
//
// Wait until a signal is delivered, with MASK installed for the duration.
//
// Expressed as pselect with no descriptors and no timeout, which is exactly
// what sigsuspend is -- and which means it inherits nlibc_pselect's handling of
// the shim's own held signals rather than needing a second copy of it.
//
// Unrouted, this reached the HOST's sigsuspend and parked the task's pthread on
// a mask no guest signal can break. zsh's signal_suspend (Src/signals.c) is
// what waitforpid spins in, so `x=$(cmd)` would have hung after the child
// exited rather than collecting its status.
int nlibc_sigsuspend(const sigset_t *mask) {
    struct timespec *forever = NULL;
    // MASK is the program's blocked set FOR THE DURATION, and saying so to the
    // shim is the whole point rather than bookkeeping.
    //
    // struct task's native_held is `shim_held & ~native_prog_blocked` -- the
    // signals the shim is holding behind the program's back, which
    // task_wake_blocked() subtracts so that a wait still ends for them. A
    // signal the PROGRAM blocked is deliberately not in there: it asked not to
    // be interrupted.
    //
    // zsh waits for a child inside child_block(), which blocks SIGCHLD, and
    // then calls sigsuspend with an (almost) empty mask -- "unblock SIGCHLD
    // while I sleep". Without this, native_prog_blocked still said SIGCHLD was
    // the program's own choice, native_held did not include it, and the
    // pselect6 the wait rides on was never interrupted by the child's exit.
    // The shell hung after its first external command, forever, with a zombie
    // sitting there.
    // Deliver first, and do not block if anything was waiting.
    //
    // This is the other half of the hang, and it is a sequencing problem rather
    // than a masking one. Handlers run at a syscall CHECKPOINT, so a SIGCHLD
    // that interrupts this wait is not delivered until the NEXT syscall the
    // program makes -- and for zsh that next syscall is the following
    // sigsuspend, which checkpoints (running the handler, which reaps the
    // child and marks the job done) and then blocks anyway, on a condition
    // that has just stopped being true and will never be signalled again.
    //
    // Delivering before the wait and reporting it as EINTR gives the caller its
    // chance to re-test.
    if (nlibc_deliver_signals_count() > 0) {
        errno = EINTR;
        return -1;
    }
    sigset_t_ saved = 0;
    bool tracked = current != NULL;
    if (tracked) {
        saved = current->native_prog_blocked;
        current->native_prog_blocked = mask != NULL ? nlibc_sigset_to_guest(mask) : 0;
        nlibc_update_held_signals();
    }
    nlibc_pselect(0, NULL, NULL, NULL, forever, mask);
    if (tracked) {
        current->native_prog_blocked = saved;
        nlibc_update_held_signals();
    }
    nlibc_deliver_signals_count();
    // sigsuspend has no success return: it comes back only when a handler has
    // run, and always as -1/EINTR.
    errno = EINTR;
    return -1;
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
        // The shim's own blocking goes in too, for the same reason
        // nlibc_sigprocmask forces it: a signal this shim handles must stay
        // blocked whatever the program asks, or the kernel takes the default
        // action for something the program believes it has a handler for. This
        // mask is installed for the duration of the wait, so leaving the bits
        // out meant ^C at a bash prompt killed the shell outright instead of
        // running readline's handler -- the shell simply vanished, with no
        // message, exactly where a fresh prompt belonged.
        sigset_t_ guest_mask = nlibc_sigset_to_guest(sigmask) | nlibc_shim_held_signals();
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
#if !defined(__linux__)
_Static_assert(sizeof(posix_spawn_file_actions_t) == sizeof(void *),
        "posix_spawn_file_actions_t is not a plain pointer on this platform");
_Static_assert(sizeof(posix_spawnattr_t) == sizeof(void *),
        "posix_spawnattr_t is not a plain pointer on this platform");
#else
// glibc declares both as STRUCTS, so the scheme above genuinely does not hold
// here and the assert is right to say so. Native-program spawn is therefore
// not supported on Linux; that build exists to compile-check the emulator, and
// nothing in it launches a native program. Stated rather than asserted away, so
// the next person does not read the silence as "it works".
#endif
// native_libc.h spells this flag for callers that cannot include <spawn.h>.
_Static_assert(NLIBC_SPAWN_SETPGROUP == POSIX_SPAWN_SETPGROUP,
        "NLIBC_SPAWN_SETPGROUP has drifted from the platform's value");
_Static_assert(NLIBC_SPAWN_SETSIGDEF == POSIX_SPAWN_SETSIGDEF,
        "NLIBC_SPAWN_SETSIGDEF has drifted from the platform's value");

struct nlibc_spawn_actions {
    struct native_spawn_action *actions;
    size_t count, cap;
    char **paths;          // OPEN copies the path; freed with the object
    size_t path_count, path_cap;
};

struct nlibc_spawn_attr {
    short flags;
    pid_t pgroup;
    sigset_t_ sigmask;     // guest bits; meaningful with POSIX_SPAWN_SETSIGMASK
    sigset_t_ sigdefault;  // guest bits; meaningful with POSIX_SPAWN_SETSIGDEF
};

// The signals this shim is holding a handler for, as guest mask bits.
//
// Every one of them is BLOCKED in the kernel whether the program asked for that
// or not -- a native program cannot give the kernel a handler, so the shim
// blocks the signal and drains it at syscall checkpoints instead. The blocking
// is the shim's, not the program's, and a spawned child must not inherit it:
// exec preserves the mask, so `sleep 30` under a native bash ran with SIGINT
// blocked and ^C did nothing whatsoever. A forked child clears this by calling
// sigprocmask itself, which is the moment a spawn does not have.
static sigset_t_ nlibc_shim_held_signals(void) {
    sigset_t_ held = 0;
    for (int sig = 1; sig < NSIG; sig++) {
        nlibc_sighandler h = nlibc_handlers[sig];
        if (h == NULL || h == SIG_DFL || h == SIG_IGN)
            continue;
        int guest_sig = nlibc_signal_to_guest(sig);
        if (guest_sig != 0)
            held |= (sigset_t_) 1 << (guest_sig - 1);
    }
    return held;
}

// The mask a child must start with, and it is not an attribute a caller has to
// ask for -- it is a correction. What the kernel holds for this task includes
// whatever the shim blocked in order to hold a handler
// (nlibc_shim_held_signals), and a child inheriting that gets a set of signals
// it can never receive and never asked to block. Taking those bits back out
// leaves what the program believes its own mask to be, which is what fork would
// have given it.
//
// Applies to exec as much as to spawn: exec here is spawn-then-exit, so the
// same leak reaches the same place. That is how a native bash handing over to
// the guest's /bin/bash produced a shell with SIGINT and SIGCHLD blocked.
static void nlibc_spawn_default_sigmask(struct native_spawn_opts *opts) {
    sigset_t_ mask = 0;
    if (nlibc_rt_sigprocmask(SIG_BLOCK_, 0, &mask) != 0)
        return;
    opts->set_sigmask = true;
    opts->sigmask = mask & ~nlibc_shim_held_signals();
}

// Tell the kernel which of this task's blocked signals are blocked by the shim
// rather than by the program, so a wait can still be interrupted by one. See
// the field comment on struct task's native_held.
//
// Called from both places that can change the answer: installing a handler
// (which adds to the held set) and the program's own sigprocmask (which
// decides which of those the program ALSO wants blocked, and so must not be
// woken for).
static void nlibc_update_held_signals(void) {
    if (current == NULL)
        return;
    __atomic_store_n(&current->native_held,
            nlibc_shim_held_signals() & ~current->native_prog_blocked,
            __ATOMIC_RELEASE);
}

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
    if (attr == NULL || *attr == NULL)
        return EINVAL;
    struct nlibc_spawn_attr *a = *attr;
    a->sigdefault = set != NULL ? nlibc_sigset_to_guest(set) : 0;
    return 0;
}
int nlibc_posix_spawnattr_setsigmask(void **attr, const sigset_t *set) {
    if (attr == NULL || *attr == NULL)
        return EINVAL;
    struct nlibc_spawn_attr *a = *attr;
    a->sigmask = set != NULL ? nlibc_sigset_to_guest(set) : 0;
    return 0;
}

static int nlibc_posix_spawn_common(pid_t *pid_out, const char *file,
        void **fa, void **attr,
        char *const argv[], char *const envp[], bool search_path) {
    if (file == NULL || argv == NULL)
        return EINVAL;

    struct native_spawn_opts opts = { .pgid = NATIVE_SPAWN_PGID_INHERIT };
    nlibc_spawn_default_sigmask(&opts);

    if (attr != NULL && *attr != NULL) {
        const struct nlibc_spawn_attr *a = *attr;
        // SETPGROUP and SETSIGMASK change what the child IS; the rest are
        // either already true of a fresh task or have no meaning here.
        if (a->flags & POSIX_SPAWN_SETPGROUP)
            opts.pgid = a->pgroup;
        if (a->flags & POSIX_SPAWN_SETSIGMASK) {
            opts.set_sigmask = true;
            opts.sigmask = a->sigmask;
        }
        if (a->flags & POSIX_SPAWN_SETSIGDEF) {
            opts.set_sigdefault = true;
            opts.sigdefault = a->sigdefault;
        }
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
        void **fa, void **attr,
        char *const argv[], char *const envp[]) {
    return nlibc_posix_spawn_common(pid, path, fa, attr, argv, envp, false);
}

int nlibc_posix_spawnp(pid_t *pid, const char *file,
        void **fa, void **attr,
        char *const argv[], char *const envp[]) {
    return nlibc_posix_spawn_common(pid, file, fa, attr, argv, envp, true);
}

// ------------------------------------------------------------------ locale
//
// The guest's locale NAMES are Linux's; the host's locale database is Darwin's,
// and the two do not hold the same set. C.UTF-8 is the one that bites: it is
// the default on most modern Linux userlands, macOS happens to have it, and iOS
// does not -- so a shell that starts fine on the CLI greets you on a device
// with
//
//     setlocale: LC_ALL: cannot change locale (C.UTF-8): Bad file descriptor
//
// (the errno being unrelated leftovers, which is its own story).
//
// What a program wants from a locale is overwhelmingly the character encoding,
// and the encoding it is asking for is right there in the name. So a request
// the host cannot honour falls back to a host locale with the SAME encoding
// rather than failing: C.UTF-8 becomes en_US.UTF-8, which is UTF-8 either way.
// Collation differs between them in ways almost nothing observes; a warning on
// every shell start is observed by everyone.
//
// setlocale(cat, NULL) is a QUERY and must never fall back -- it would answer a
// question by changing the thing it was asked about.
// The environment variable a single category answers to, or NULL for LC_ALL
// and anything this platform has that POSIX does not name.
static const char *nlibc_locale_env_for(int category) {
    switch (category) {
    case LC_CTYPE:    return "LC_CTYPE";
    case LC_NUMERIC:  return "LC_NUMERIC";
    case LC_TIME:     return "LC_TIME";
    case LC_COLLATE:  return "LC_COLLATE";
    case LC_MONETARY: return "LC_MONETARY";
#ifdef LC_MESSAGES
    case LC_MESSAGES: return "LC_MESSAGES";
#endif
    default:          return NULL;
    }
}

char *nlibc_setlocale(int category, const char *name) {
    // setlocale(cat, "") means "take it from the environment" -- and the host's
    // environment is the wrong one. A native program is a function call inside
    // the app, so the C library resolves "" against the HOST process's LANG,
    // which on a device is whatever iOS is set to and has nothing to do with the
    // guest. The guest's own shells read the guest's environment and answer
    // differently, which is the whole test this shim exists to apply.
    //
    // Measured with the host at en_US.UTF-8 and the guest's LANG unset, which is
    // exactly the device shape: a two-byte UTF-8 character measured
    //     ${#e}  ->  1  in native bash and native zsh
    //     ${#e}  ->  2  in the guest's own bash and zsh
    // so every native program silently disagreed with the guest about what a
    // character is -- string length, case, collation and pattern matching all
    // follow from it.
    //
    // Resolved here, from the GUEST environment, in POSIX's order: LC_ALL wins,
    // then the category's own variable, then LANG, then "C". Simplification
    // worth knowing: for LC_ALL this picks ONE name rather than resolving each
    // category separately, which differs from POSIX only when the guest sets
    // some but not all of the per-category variables without LC_ALL. Both shells
    // set the individual categories themselves afterwards, so that case does not
    // arise in practice -- but it is a simplification, not an equivalence.
    char resolved[128];
    if (name != NULL && name[0] == '\0') {
        const char *env = nlibc_getenv("LC_ALL");
        if (env == NULL || *env == '\0') {
            const char *catname = nlibc_locale_env_for(category);
            if (catname != NULL)
                env = nlibc_getenv(catname);
            if (env == NULL || *env == '\0')
                env = nlibc_getenv("LANG");
        }
        if (env == NULL || *env == '\0')
            env = "C";
        snprintf(resolved, sizeof(resolved), "%s", env);
        name = resolved;
    }

    char *res = setlocale(category, name);
    if (res != NULL || name == NULL)
        return res;

    // Same encoding first. Anything ending .UTF-8 or .utf8 is UTF-8 whatever
    // the territory in front of it.
    size_t len = strlen(name);
    bool utf8 = (len >= 6 && strcasecmp(name + len - 6, ".UTF-8") == 0) ||
                (len >= 5 && strcasecmp(name + len - 5, ".utf8") == 0);
    if (utf8) {
        // Darwin spells the charset-only UTF-8 locale "UTF-8", and on iOS that
        // is the ONLY one there is. Measured on an iPad running this build:
        //
        //     LANG=C.UTF-8      native len=2   guest len=1
        //     LANG=en_US.UTF-8  native len=2
        //     LANG=UTF-8        native len=1
        //
        // so trying en_US.UTF-8 alone -- which works on macOS and made this
        // look fixed there -- fell through to "C" on the device, and every
        // native program went single-byte while the guest was UTF-8. C.UTF-8
        // is what modern guests actually set, and "UTF-8" is also its closest
        // match: the C locale's collation and messages with a UTF-8 charset,
        // where en_US.UTF-8 would additionally change collation.
        res = setlocale(category, "UTF-8");
        if (res != NULL)
            return res;
        res = setlocale(category, "en_US.UTF-8");
        if (res != NULL)
            return res;

        // Both of those can fail for ONE category and still be available for
        // another, because Darwin's "UTF-8" is a CHARSET, not a locale: it names
        // an encoding with no territory, so LC_CTYPE takes it and a whole-locale
        // request refuses it. Probed on the host:
        //
        //     setlocale(LC_ALL,   "UTF-8") = NULL         MB_CUR_MAX=1
        //     setlocale(LC_CTYPE, "UTF-8") = UTF-8        MB_CUR_MAX=4
        //
        // which is the whole of why this was still broken on a device after the
        // fallback above was added. A shell asks for LC_ALL. On macOS that never
        // gets here -- macOS HAS C.UTF-8, so the first call succeeds and the bug
        // is invisible on the CLI build. On iOS C.UTF-8 does not exist, "UTF-8"
        // is rejected as an LC_ALL, en_US.UTF-8 does not exist either, and the
        // request fell all the way to "C". Measured on an iPad, build 548, with
        // the guest at LC_ALL=C.UTF-8, counting the characters in a 2-byte "e"
        // with an acute accent:
        //
        //     /usr/bin/bash     (guest)   1     <- the oracle
        //     /AOK/native/bash            2
        //     /AOK/native/zsh             2
        //
        // So put the charset where the charset lives: take "C" as the base for
        // everything and give LC_CTYPE the UTF-8 it will accept. Encoding is what
        // the caller was asking for -- every other category differs between
        // C.UTF-8 and C only in collation and messages, which is the same trade
        // the fallback above already makes.
        if (category == LC_ALL) {
            char *base = setlocale(LC_ALL, "C");
            if (setlocale(LC_CTYPE, "UTF-8") != NULL ||
                    setlocale(LC_CTYPE, "en_US.UTF-8") != NULL)
                // The composite name, which is now genuinely mixed. Callers that
                // print it get the truth rather than a name nothing is set to.
                return setlocale(LC_ALL, NULL);
            return base;
        }
    }
    // Then the one locale every C library is required to have.
    return setlocale(category, "C");
}

// -------------------------------------------------------- the service database
//
// readline's completion offers service names, which it takes from /etc/services
// -- and left to the host that is the MAC's list, not the guest's. Same shape as
// the passwd and group walkers above: an index-keyed scan, so the enumeration
// sees the file as it is now and there is one parser rather than two.
//
// /etc/services is whitespace-separated rather than colon-separated
// ("ssh 22/tcp SSH"), so it cannot reuse nlibc_scan_db, which splits on colons
// for passwd and group.
static char nlibc_se_line[512];
static struct servent nlibc_se;
static char *nlibc_se_aliases[1];
static char nlibc_se_proto[16];
static size_t nlibc_se_pos;

void nlibc_setservent(int stayopen) { (void) stayopen; nlibc_se_pos = 0; }
void nlibc_endservent(void) { nlibc_se_pos = 0; }

struct servent *nlibc_getservent(void) {
    struct fd *fd = NULL;
    if (native_open("/etc/services", O_RDONLY_, &fd) < 0)
        return NULL;

    char buf[4096];
    size_t held = 0, seen = 0;
    struct servent *found = NULL;
    for (;;) {
        ssize_t n = native_read(fd, buf + held, sizeof(buf) - held - 1);
        if (n < 0)
            break;
        size_t avail = held + (size_t) n;
        buf[avail] = '\0';
        char *line = buf, *nl;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = '\0';
            char *hash = strchr(line, '#');
            if (hash != NULL)
                *hash = '\0';
            // name, then port/proto. Anything after is an alias, which is not
            // reported: readline completes on the NAME.
            char name[128], portproto[64];
            if (sscanf(line, "%127s %63s", name, portproto) == 2) {
                char *slash = strchr(portproto, '/');
                if (slash != NULL && seen++ == nlibc_se_pos) {
                    *slash = '\0';
                    snprintf(nlibc_se_line, sizeof(nlibc_se_line), "%s", name);
                    snprintf(nlibc_se_proto, sizeof(nlibc_se_proto), "%s", slash + 1);
                    nlibc_se_aliases[0] = NULL;
                    nlibc_se.s_name = nlibc_se_line;
                    nlibc_se.s_aliases = nlibc_se_aliases;
                    // s_port is network order, as getservent's contract says.
                    nlibc_se.s_port = htons((uint16_t) atoi(portproto));
                    nlibc_se.s_proto = nlibc_se_proto;
                    found = &nlibc_se;
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
    if (found != NULL)
        nlibc_se_pos++;
    return found;
}

// The LOOKUPS over that same guest database, which is where the misses were.
// setservent/getservent/endservent were routed for readline's completion and
// getservbyname was not -- the entry point ssh actually uses. readconf.c's
// default_ssh_port() is getservbyname("ssh", "tcp"), and ssh.c calls it at six
// places whenever no Port is configured, which is the common case; misc.c's
// a2port() is the other caller, behind `-p ssh`.
//
// Usually harmless, because the Mac's /etc/services and the guest's both say
// ssh 22 -- and "usually harmless" is precisely the class of bug this file
// exists to remove. A guest that moves the port, or one with no /etc/services
// at all, silently got the Mac's answer instead of its own.
//
// Built on the walker above rather than a second parser, so there is one
// /etc/services reader. The enumeration position is saved and restored: a
// lookup in the middle of somebody's getservent walk must not move it, which is
// what setservent(1) buys on a real libc.
static struct servent *nlibc_serv_lookup(const char *name, int port,
        const char *proto) {
    size_t saved = nlibc_se_pos;
    struct servent *found = NULL;
    nlibc_se_pos = 0;
    for (;;) {
        struct servent *se = nlibc_getservent();
        if (se == NULL)
            break;
        if (proto != NULL && se->s_proto != NULL && strcmp(se->s_proto, proto) != 0)
            continue;
        if (name != NULL ? strcmp(se->s_name, name) == 0 : se->s_port == port) {
            found = se;
            break;
        }
    }
    nlibc_se_pos = saved;
    return found;
}

// Aliases are not matched, because nlibc_getservent does not parse them -- it
// reports the canonical name, which is what readline's completion wanted. A
// lookup by alias therefore misses where a real libc would hit; that is a
// smaller wrong than answering from the Mac's file, and it is written down here
// rather than left to be discovered.
struct servent *nlibc_getservbyname(const char *name, const char *proto) {
    if (name == NULL)
        return NULL;
    return nlibc_serv_lookup(name, 0, proto);
}

// port is in network order, as getservbyport's contract says.
struct servent *nlibc_getservbyport(int port, const char *proto) {
    return nlibc_serv_lookup(NULL, port, proto);
}

// ------------------------------------------------------ the protocol database
//
// /etc/protocols, the neighbour of the file above, and the pair produced the
// clearest single line of split brain in this whole file. On a rootfs with
// NEITHER file, zftp's getprotobyname("tcp") SUCCEEDED -- out of the Mac's
// /etc/protocols -- and the very next statement, the already-routed
// getservbyname("ftp", "tcp"), failed because the guest has no /etc/services.
// Protocols from the device and services from the guest, in adjacent calls.
//
// Same walker shape as getservent above, over "tcp 6 TCP" instead of
// "ssh 22/tcp SSH".
static char nlibc_pe_line[512];
static struct protoent nlibc_pe;
static char *nlibc_pe_aliases[1];
static size_t nlibc_pe_pos;

void nlibc_setprotoent(int stayopen) { (void) stayopen; nlibc_pe_pos = 0; }
void nlibc_endprotoent(void) { nlibc_pe_pos = 0; }

struct protoent *nlibc_getprotoent(void) {
    struct fd *fd = NULL;
    if (native_open("/etc/protocols", O_RDONLY_, &fd) < 0)
        return NULL;

    char buf[4096];
    size_t held = 0, seen = 0;
    struct protoent *found = NULL;
    for (;;) {
        ssize_t n = native_read(fd, buf + held, sizeof(buf) - held - 1);
        if (n < 0)
            break;
        size_t avail = held + (size_t) n;
        buf[avail] = '\0';
        char *line = buf, *nl;
        while ((nl = strchr(line, '\n')) != NULL) {
            *nl = '\0';
            char *hash = strchr(line, '#');
            if (hash != NULL)
                *hash = '\0';
            // name, then number. Aliases follow and are not reported, on the
            // same terms as the service walker: the canonical name is what a
            // lookup asks by.
            char name[128], number[32];
            if (sscanf(line, "%127s %31s", name, number) == 2 &&
                    number[strspn(number, "0123456789")] == '\0' &&
                    seen++ == nlibc_pe_pos) {
                snprintf(nlibc_pe_line, sizeof(nlibc_pe_line), "%s", name);
                nlibc_pe_aliases[0] = NULL;
                nlibc_pe.p_name = nlibc_pe_line;
                nlibc_pe.p_aliases = nlibc_pe_aliases;
                nlibc_pe.p_proto = atoi(number);
                found = &nlibc_pe;
                goto done;
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
    if (found != NULL)
        nlibc_pe_pos++;
    return found;
}

// The well-known assignments, used only when the guest has NO /etc/protocols.
//
// This is the one answer in this file that comes from neither the guest nor the
// host, and it needs the governing test applied out loud rather than waved
// past: a protocol number is an IANA assignment, so "tcp is 6" cannot differ
// between a guest and a device, and there is no system where it does. musl's
// getprotoent answers from exactly such a table and reads no file at all, so a
// guest running musl already behaves this way. The FILE still wins where it
// exists, because that is where a guest renames or adds one -- the table is the
// floor, not an override.
static const struct { const char *name; int proto; } nlibc_proto_wellknown[] = {
    {"ip", 0}, {"icmp", 1}, {"igmp", 2}, {"ggp", 3}, {"ipencap", 4},
    {"tcp", 6}, {"egp", 8}, {"pup", 12}, {"udp", 17}, {"hmp", 20},
    {"xns-idp", 22}, {"rdp", 27}, {"iso-tp4", 29}, {"dccp", 33}, {"xtp", 36},
    {"ddp", 37}, {"idpr-cmtp", 38}, {"ipv6", 41}, {"ipv6-route", 43},
    {"ipv6-frag", 44}, {"idrp", 45}, {"rsvp", 46}, {"gre", 47}, {"esp", 50},
    {"ah", 51}, {"skip", 57}, {"ipv6-icmp", 58}, {"ipv6-nonxt", 59},
    {"ipv6-opts", 60}, {"rspf", 73}, {"vmtp", 81}, {"eigrp", 88}, {"ospf", 89},
    {"ipip", 94}, {"encap", 98}, {"pim", 103}, {"ipcomp", 108}, {"vrrp", 112},
    {"l2tp", 115}, {"isis", 124}, {"sctp", 132}, {"fc", 133}, {"udplite", 136},
    {"mpls-in-ip", 137}, {"manet", 138}, {"hip", 139}, {"shim6", 140},
    {"wesp", 141}, {"rohc", 142},
};

static struct protoent *nlibc_proto_lookup(const char *name, int proto) {
    size_t saved = nlibc_pe_pos;
    struct protoent *found = NULL;
    nlibc_pe_pos = 0;
    for (;;) {
        struct protoent *pe = nlibc_getprotoent();
        if (pe == NULL)
            break;
        if (name != NULL ? strcmp(pe->p_name, name) == 0 : pe->p_proto == proto) {
            found = pe;
            break;
        }
    }
    nlibc_pe_pos = saved;
    if (found != NULL)
        return found;
    // "No match" and "no file" are different answers and must not be conflated:
    // a guest that HAS the file and leaves a protocol out has said something,
    // and the table below must not overrule it. The walk above cannot tell them
    // apart (it returns NULL for an empty file as readily as a missing one), so
    // the presence of the file is asked separately.
    struct fd *probe = NULL;
    if (native_open("/etc/protocols", O_RDONLY_, &probe) >= 0) {
        native_close(probe);
        return NULL;
    }
    for (size_t i = 0; i < sizeof(nlibc_proto_wellknown) / sizeof(nlibc_proto_wellknown[0]); i++) {
        if (name != NULL ? strcmp(nlibc_proto_wellknown[i].name, name) == 0
                         : nlibc_proto_wellknown[i].proto == proto) {
            snprintf(nlibc_pe_line, sizeof(nlibc_pe_line), "%s",
                    nlibc_proto_wellknown[i].name);
            nlibc_pe_aliases[0] = NULL;
            nlibc_pe.p_name = nlibc_pe_line;
            nlibc_pe.p_aliases = nlibc_pe_aliases;
            nlibc_pe.p_proto = nlibc_proto_wellknown[i].proto;
            return &nlibc_pe;
        }
    }
    return NULL;
}

struct protoent *nlibc_getprotobyname(const char *name) {
    if (name == NULL)
        return NULL;
    return nlibc_proto_lookup(name, 0);
}

struct protoent *nlibc_getprotobynumber(int proto) {
    return nlibc_proto_lookup(NULL, proto);
}

// ------------------------------------------------------------------- logging
//
// openlog/syslog/closelog reach the DEVICE's log: os_log or ASL on iOS, the
// Mac's syslog on the CLI. The guest has its own /dev/log and AOK already
// serves it (fs/sock.c contemplates "a syslogd reading /dev/log"), so this is
// not a missing capability -- it is a message delivered to the wrong machine.
//
// It is also live on EVERY run rather than on some unusual path: log.c's
// log_init() calls openlog() and then closelog() unconditionally, and every
// entry point calls log_init -- ssh, scp, sftp and ssh-keygen alike. syslog()
// itself fires whenever logging is not on stderr (`ssh -y`, or a SyslogFacility
// and LogLevel that route it away).
//
// The second half of the problem is worse than the destination. openlog() sets
// a PROCESS-WIDE identity on the host, and a native program is a task inside
// one process: one guest task running ssh would have renamed the syslog
// identity for the whole iSH-AOK app, including whatever logged next. Here the
// state is per-thread, which is per native program.
//
// The wire format is RFC 3164, which is what a Linux syslogd expects on
// /dev/log: "<PRI>Mmm dd hh:mm:ss ident[pid]: message". The facility and level
// NUMBERS need no translation -- both sides inherited BSD's, so LOG_USER is 8,
// LOG_AUTH 32, LOG_AUTHPRIV 80 and LOG_DEBUG 7 on Darwin and Linux alike --
// which is worth stating because almost nothing else in this file gets to pass
// a constant through.
static __thread char nlibc_log_ident[64];
static __thread int nlibc_log_facility = LOG_USER;
static __thread int nlibc_log_option;
static __thread int nlibc_log_fd = -1;

void nlibc_openlog(const char *ident, int option, int facility) {
    // Not `ident != NULL ? ident : ""` straight into snprintf: nlibc_vsyslog
    // below re-opens a dropped connection by calling this with the stored
    // ident, and a snprintf whose source and destination are the same buffer
    // is undefined.
    if (ident != nlibc_log_ident)
        snprintf(nlibc_log_ident, sizeof(nlibc_log_ident), "%s",
                 ident != NULL ? ident : "");
    nlibc_log_option = option;
    if (facility != 0)
        nlibc_log_facility = facility;
    // LOG_NDELAY asks for the connection now; otherwise it waits for the first
    // message, exactly as a real syslog does.
    if ((option & LOG_NDELAY) && nlibc_log_fd < 0) {
        int fd = nlibc_socket(AF_UNIX, SOCK_DGRAM, 0);
        if (fd >= 0) {
            struct sockaddr_un addr = { .sun_family = AF_UNIX };
            snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", "/dev/log");
            if (nlibc_connect(fd, &addr, sizeof(addr)) < 0)
                nlibc_close(fd);
            else
                nlibc_log_fd = fd;
        }
    }
}

void nlibc_closelog(void) {
    if (nlibc_log_fd >= 0)
        nlibc_close(nlibc_log_fd);
    nlibc_log_fd = -1;
    nlibc_log_ident[0] = '\0';
    nlibc_log_option = 0;
    nlibc_log_facility = LOG_USER;
}

void nlibc_vsyslog(int priority, const char *fmt, va_list ap) {
    char body[1024];
    // %m is a glibc extension and Darwin's vsnprintf does not know it; OpenSSH
    // does not use it, and expanding it here would mean parsing the format.
    vsnprintf(body, sizeof(body), fmt, ap);

    if ((priority & ~0x7) == 0)          // a bare level: use the stored facility
        priority |= nlibc_log_facility;

    char stamp[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%b %e %H:%M:%S", &tm);

    char line[1200];
    int n = snprintf(line, sizeof(line), "<%d>%s %s[%d]: %s", priority, stamp,
            nlibc_log_ident[0] != '\0' ? nlibc_log_ident : "native",
            (int) nlibc_getpid(), body);
    if (n < 0)
        return;
    if ((size_t) n > sizeof(line) - 1)
        n = sizeof(line) - 1;

    if (nlibc_log_fd < 0)
        nlibc_openlog(nlibc_log_ident, nlibc_log_option | LOG_NDELAY,
                nlibc_log_facility);
    if (nlibc_log_fd >= 0 && nlibc_write(nlibc_log_fd, line, (size_t) n) < 0) {
        // A syslogd that went away leaves a dead socket behind; drop it so the
        // next message reconnects.
        nlibc_close(nlibc_log_fd);
        nlibc_log_fd = -1;
    }
    // No /dev/log in the guest means the message is dropped, which is what
    // syslog() does everywhere when nothing is listening. LOG_PERROR still
    // reaches the guest's stderr, because that is routed too.
    if (nlibc_log_option & LOG_PERROR)
        fprintf(nlibc_stderr(), "%s: %s\n",
                nlibc_log_ident[0] != '\0' ? nlibc_log_ident : "native", body);
}

void nlibc_syslog(int priority, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    nlibc_vsyslog(priority, fmt, ap);
    va_end(ap);
}

// ------------------------------------------------------------------- mapping
//
// mmap is the only symbol in this file that must give two DIFFERENT answers in
// the same build, which is why it is a wrapper and not a rename:
//
//  - sshkey.c's sshkey_prekey_alloc() maps MAP_ANON|MAP_PRIVATE with fd -1, on
//    every private-key load, to hold the shielding prekey. That is ordinary
//    host memory for host code to dereference, and it must STAY on the host: a
//    guest mmap would return a guest address that this program cannot touch.
//  - misc.c's lib_contains_symbol() maps a descriptor that came from the routed
//    open() -- a GUEST fd. A host mmap on that number maps whatever host file
//    happens to hold the same descriptor, or fails. It is reached from
//    ssh-sk.c's sshsk_open(), i.e. any FIDO/-sk key.
//
// So the branch is on what the mapping IS, not on who called: anonymous memory
// goes straight to the host, and a file mapping is satisfied by reading the
// guest's file into host anonymous memory. That read is the honest way to give
// host code a pointer to guest file contents -- there is no shared page to hand
// out, because the guest's file lives behind AOK's VFS rather than in the host
// filesystem.
//
// MAP_SHARED on a guest fd is refused rather than faked. A private read-only
// map is a snapshot and copying gives exactly that; a shared one promises that
// writes reach the file and that another mapper's writes become visible, and
// neither is true of a copy. The one caller wants PROT_READ|MAP_PRIVATE.
void *nlibc_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
    if ((flags & MAP_ANON) || fd < 0)
        return mmap(addr, len, prot, flags, fd, offset);

    if (flags & MAP_SHARED) {
        errno = ENOTSUP;
        return MAP_FAILED;
    }
    if (len == 0) {
        errno = EINVAL;
        return MAP_FAILED;
    }
    // Writable to fill, then narrowed to what the caller asked for.
    void *mem = mmap(NULL, len, PROT_READ | PROT_WRITE,
            MAP_ANON | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED)
        return MAP_FAILED;
    size_t at = 0;
    while (at < len) {
        ssize_t got = nlibc_pread(fd, (char *) mem + at, len - at,
                offset + (off_t) at);
        if (got < 0) {
            int saved = errno;
            munmap(mem, len);
            errno = saved;
            return MAP_FAILED;
        }
        if (got == 0)
            break;      // short file: the rest stays zero, as mmap promises
        at += (size_t) got;
    }
    if ((prot & (PROT_READ | PROT_WRITE)) != (PROT_READ | PROT_WRITE) &&
            mprotect(mem, len, prot) < 0) {
        int saved = errno;
        munmap(mem, len);
        errno = saved;
        return MAP_FAILED;
    }
    return mem;
}

// Both kinds of mapping above are host anonymous memory underneath, so one
// unmap serves both.
int nlibc_munmap(void *addr, size_t len) {
    return munmap(addr, len);
}

// msync, and it is here to be COUPLED to the branch above rather than because
// it does anything.
//
// Every mapping nlibc_mmap can return is host anonymous memory: an anonymous
// request goes straight through, and a file request is satisfied by reading the
// guest's file into a private anonymous copy. Neither has anything to write
// back -- a private mapping never does, by definition -- so "flush this to the
// file" is genuinely a no-op here, and the host's msync over the host's own
// anonymous pages is the right authority for the range and alignment checks
// that remain. The flags are Darwin's on both sides (MS_SYNC is 0x10 there and
// 4 on Linux), and a native program passes the value its own headers gave it,
// so they are passed through rather than translated.
//
// The trap this must not become: nlibc_mmap REFUSES file-backed MAP_SHARED. If
// that refusal is ever lifted, msync stops being a no-op and this function
// becomes silent data loss -- it would report success having written the
// caller's changes to a private copy that no one will ever read back. The two
// move together; that is the entire content of this entry.
int nlibc_msync(void *addr, size_t len, int flags) {
    return msync(addr, len, flags);
}

// _NSGetExecutablePath: the path of the running executable image, which on the
// host is the app -- /tmp/libc-build/ish on the CLI build, the .app bundle on
// iOS. zsh calls it from getmypath() (Src/init.c, behind `#if defined(__APPLE__)`,
// which native code always satisfies) to set $ZSH_EXEPATH, so a guest shell
// advertised a host path with no counterpart in the guest filesystem; anything
// re-execing "$ZSH_EXEPATH" would run nothing.
//
// This one cannot be answered, and reporting failure is the answer rather than
// a shrug. The honest guest path would be /AOK/native/<name>, and the shim has
// no way to know <name>: native_exec_run_pending frees the record naming the
// program before it calls into it (kernel/native.c), and /proc/self/exe is no
// help either -- it still names the image the exec REPLACED, which a probe in
// the guest confirms (`/AOK/native/zsh` reports /usr/bin/dash).
//
// Failing is not a dead end for the caller. It is a documented outcome of this
// API, and every consumer has a fallback that reaches guest-visible sources
// instead: zsh's getmypath falls through to argv[0], the guest's cwd and the
// guest's $PATH, all of which arrive through routed calls, so `/AOK/native/zsh`
// sets ZSH_EXEPATH=/AOK/native/zsh and a symlinked one resolves through the
// guest's own PATH. That is a better answer than this function could give.
//
// *bufsize is left alone deliberately: the contract sets it only to report the
// size a larger buffer would need, and a caller that retries (zsh does, once)
// must not be sent round a growing loop for a call that will never succeed.
int nlibc_NSGetExecutablePath(char *buf, uint32_t *bufsize) {
    (void) buf; (void) bufsize;
    return -1;
}

// ------------------------------------------------- Darwin's copy fast paths
//
// std::fs::copy on Apple does not read and write: it tries fclonefileat, then
// fcopyfile, and only those. Both are libc, not syscalls, and unrouted they
// copied one host file to another while the guest saw nothing happen.
//
// clonefile is refused rather than emulated. It asks the *filesystem* to share
// extents, which fakefs cannot do, and ENOTSUP is a documented outcome that
// every caller already handles -- Rust's copy() explicitly falls through to
// fcopyfile on it, which is the path that works.
int nlibc_fclonefileat(int srcfd, int dstdirfd, const char *dst, int flags) {
    (void) srcfd; (void) dstdirfd; (void) dst; (void) flags;
    errno = ENOTSUP;
    return -1;
}

// The state object exists so the caller can ask how many bytes moved. Only
// COPYFILE_STATE_COPIED is answered, because it is the only key a copy that
// went through this path can know, and guessing at the others would be worse
// than saying no.
struct nlibc_copyfile_state { off_t copied; };

void *nlibc_copyfile_state_alloc(void) {
    struct nlibc_copyfile_state *st = calloc(1, sizeof(*st));
    return st;
}

int nlibc_copyfile_state_free(void *state) {
    free(state);
    return 0;
}

#define NLIBC_COPYFILE_STATE_COPIED 8
int nlibc_copyfile_state_get(void *state, uint32_t flag, void *dst) {
    struct nlibc_copyfile_state *st = state;
    if (st == NULL || dst == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (flag != NLIBC_COPYFILE_STATE_COPIED) {
        errno = EINVAL;
        return -1;
    }
    *(off_t *) dst = st->copied;
    return 0;
}

// A plain read/write loop over the two guest descriptors. It is what fcopyfile
// degrades to on a filesystem without copy acceleration anyway, and both fds
// are already routed, so nothing here needs to know about fakefs.
//
// COPYFILE_DATA is all that is honoured. The metadata bits (ACLs, xattrs,
// resource forks) have no guest counterpart, and quietly claiming to have
// copied them would be the silent-wrong-answer this file exists to avoid --
// but a caller asking for COPYFILE_ALL on a copy of a plain file expects data
// to move, so the flags are not rejected either. Rust asks for ALL.
int nlibc_fcopyfile(int from_fd, int to_fd, void *state, uint32_t flags) {
    struct nlibc_copyfile_state *st = state;
    (void) flags;
    if (st != NULL)
        st->copied = 0;
    if (nlibc_lseek(from_fd, 0, SEEK_SET) < 0 && errno != ESPIPE)
        return -1;

    char buf[64 * 1024];
    for (;;) {
        ssize_t n = nlibc_read(from_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = nlibc_write(to_fd, buf + off, (size_t) (n - off));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            off += w;
        }
        if (st != NULL)
            st->copied += n;
    }
    return 0;
}

// setattrlist is Darwin's bulk attribute setter, and the only thing that
// reaches it here is std's File::set_times on an older deployment target.
// Refused rather than half-parsed: the attribute list is a variable-length
// description of which fields the caller packed, and answering it wrongly
// would write the wrong timestamps rather than none. A caller that gets
// ENOTSUP reports a failed set_times, which is true.
int nlibc_setattrlist(const char *path, void *attrs, void *buf, size_t size, unsigned long options) {
    (void) path; (void) attrs; (void) buf; (void) size; (void) options;
    errno = ENOTSUP;
    return -1;
}

int nlibc_fsetattrlist(int fd_no, void *attrs, void *buf, size_t size, unsigned long options) {
    (void) fd_no; (void) attrs; (void) buf; (void) size; (void) options;
    errno = ENOTSUP;
    return -1;
}
