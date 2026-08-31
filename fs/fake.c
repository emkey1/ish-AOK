#include <stdarg.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sqlite3.h>

#include "debug.h"
#include "kernel/errno.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/dev.h"
#include "fs/inode.h"
#include "fs/poll.h"
#include "fs/real.h"
#include "fs/tty.h"
#include "fs/fifo.h"
#define ISH_INTERNAL
#include "fs/fake.h"
#include "fs/fake-path.h"

// TODO document database

// The metadata DB speaks unescaped guest paths; the host filesystem speaks
// the escaped on-disk form (see fs/fake-path.h). Every path that reaches
// realfs or a raw host syscall below must go through fakefs_host_path first.
typedef char host_path_t[MAX_PATH + 1];
static int fakefs_host_path(const char *path, host_path_t host) {
    if (fake_path_to_host(path, host, sizeof(host_path_t)) == NULL)
        return _ENAMETOOLONG;
    return 0;
}

// this exists only to override readdir to fix the returned inode numbers
static struct fd_ops fakefs_fdops;
static struct fd_ops initctl_fdops;

#define INITCTL_MODE (S_IFIFO | 0666)
#define INITCTL_RUN_PATH "run/initctl"
#define INITCTL_DEV_PATH "dev/initctl"
#define INITCTL_RUN_INODE ((ino_t) 0x495348696e697401ULL)
#define INITCTL_DEV_INODE ((ino_t) 0x495348696e697402ULL)

// Both initctl paths (run/initctl and the legacy dev/initctl) are one logical
// init control channel, so they share a single named pipe.
static struct fifo_file *fakefs_initctl_fifo;
static lock_t fakefs_initctl_lock = LOCK_INITIALIZER;

static struct fifo_file *fakefs_get_initctl_fifo(void) {
    lock(&fakefs_initctl_lock, 0);
    if (fakefs_initctl_fifo == NULL)
        fakefs_initctl_fifo = fifo_file_new();
    struct fifo_file *fifo = fakefs_initctl_fifo;
    unlock(&fakefs_initctl_lock);
    return fifo;
}

static bool fakefs_initctl_info(const char *path, const char **virtual_path, ino_t *inode) {
    while (*path == '/')
        path++;
    if (strcmp(path, INITCTL_RUN_PATH) == 0) {
        if (virtual_path != NULL)
            *virtual_path = INITCTL_RUN_PATH;
        if (inode != NULL)
            *inode = INITCTL_RUN_INODE;
        return true;
    }
    if (strcmp(path, INITCTL_DEV_PATH) == 0) {
        if (virtual_path != NULL)
            *virtual_path = INITCTL_DEV_PATH;
        if (inode != NULL)
            *inode = INITCTL_DEV_INODE;
        return true;
    }
    return false;
}

static void fakefs_initctl_statbuf(struct statbuf *stat, ino_t inode) {
    dword_t now = (dword_t) time(NULL);
    *stat = (struct statbuf) {
        .inode = inode,
        .mode = INITCTL_MODE,
        .nlink = 1,
        .uid = 0,
        .gid = 0,
        .blksize = 4096,
        .atime = now,
        .mtime = now,
        .ctime = now,
    };
}

static bool fakefs_is_initctl_fd(struct fd *fd) {
    return fd->ops == &initctl_fdops;
}

static bool fakefs_dpkg_trace_enabled(void) {
    static int enabled = -1;
    if (enabled < 0)
        enabled = getenv("ISH_TRACE_DPKG_FAKEFS") != NULL ? 1 : 0;
    if (!enabled)
        return false;
    if (current == NULL)
        return false;
    return strcmp(current->comm, "dpkg") == 0 ||
           strcmp(current->comm, "apt") == 0 ||
           strcmp(current->comm, "apt-get") == 0;
}

static void fakefs_apply_stat_override(struct statbuf *stat, ino_t inode, const struct ish_stat *ishstat) {
    // The caller filled this in from the host stat, whose st_dev names the
    // volume the backing store happens to sit on. We are about to replace the
    // inode with one from this mount's SQLite metadata, and those two numbers
    // must come from the SAME namespace or the (dev, ino) pair is meaningless.
    // Every fakefs mount on one host volume reports that same host dev, while
    // each numbers its own inodes from 1, so two roots collide constantly:
    // three files, one per /AOK/roots mount, all reported dev=265 ino=308, and
    // cp/mv/rsync/install then treat unrelated files as the same file. Leave
    // dev 0 and let generic_statat/generic_fstat stamp mount->fake_dev, the
    // per-mount anonymous device (Linux 0:xx) every other backing-less
    // filesystem here already gets.
    stat->dev = 0;
    stat->inode = inode;
    stat->mode = ishstat->mode;
    stat->uid = ishstat->uid;
    stat->gid = ishstat->gid;
    stat->rdev = ishstat->rdev;
    if (S_ISCHR(stat->mode))
        tty_stat_rdev(stat->rdev, stat);
}

static void fakefs_snapshot_fd_stat(struct fd *fd) {
    if (fd == NULL || fd->mount == NULL || fd->fake_inode == 0)
        return;
    struct statbuf real_stat;
    if (realfs.fstat(fd, &real_stat) < 0)
        return;

    struct fakefs_db *fs = fakefs_db_thread(&fd->mount->fakefs);
    FAKEFS_LOCK_READ(fs);
    struct ish_stat ishstat;
    bool found = inode_read_stat(fs, fd->fake_inode, &ishstat);
    FAKEFS_UNLOCK_READ(fs);
    if (!found)
        return;

    fakefs_apply_stat_override(&real_stat, fd->fake_inode, &ishstat);
    fd->stat = real_stat;
}

static void fakefs_trace_symlink_result(struct mount *mount, struct fakefs_db *fs, const char *target, const char *link, int result, int open_errno) {
    if (!fakefs_dpkg_trace_enabled())
        return;

    host_path_t host_link;
    if (fakefs_host_path(link, host_link) < 0)
        return;
    const char *fixed = fix_path(host_link);
    struct stat st;
    int host_err = fstatat(mount->root_fd, fixed, &st, AT_SYMLINK_NOFOLLOW);
    if (host_err < 0) {
        printk("ish-dpkg-fakefs:symlink pid=%d comm=%s link=%s fix=%s target=%s result=%d errno=%d host=-1 host_errno=%d\n",
                current->pid, current->comm, link, fixed, target, result, open_errno, errno);
    } else {
        printk("ish-dpkg-fakefs:symlink pid=%d comm=%s link=%s fix=%s target=%s result=%d errno=%d host_mode=%#o host_size=%lld host_ino=%llu\n",
                current->pid, current->comm, link, fixed, target, result, open_errno,
                st.st_mode, (long long) st.st_size, (unsigned long long) st.st_ino);
    }

    struct ish_stat ishstat;
    inode_t inode;
    bool found = path_read_stat(fs, link, &ishstat, &inode);
    if (!found) {
        printk("ish-dpkg-fakefs:symlink-db pid=%d comm=%s link=%s found=0\n",
                current->pid, current->comm, link);
        return;
    }
    printk("ish-dpkg-fakefs:symlink-db pid=%d comm=%s link=%s found=1 inode=%llu mode=%#o uid=%u gid=%u rdev=%u\n",
            current->pid, current->comm, link, (unsigned long long) inode,
            ishstat.mode, ishstat.uid, ishstat.gid, ishstat.rdev);
}

static ssize_t initctl_read(struct fd *fd, void *buf, size_t bufsize) {
    return fifo_file_read(fakefs_initctl_fifo, fd, buf, bufsize);
}

static ssize_t initctl_write(struct fd *fd, const void *buf, size_t bufsize) {
    return fifo_file_write(fakefs_initctl_fifo, fd, buf, bufsize);
}

static ssize_t initctl_pread(struct fd *UNUSED(fd), void *UNUSED(buf), size_t UNUSED(bufsize), off_t UNUSED(off)) {
    return _ESPIPE;
}

static ssize_t initctl_pwrite(struct fd *UNUSED(fd), const void *UNUSED(buf), size_t UNUSED(bufsize), off_t UNUSED(off)) {
    return _ESPIPE;
}

static off_t_ initctl_lseek(struct fd *UNUSED(fd), off_t_ UNUSED(off), int UNUSED(whence)) {
    return _ESPIPE;
}

static int initctl_poll(struct fd *fd) {
    return fifo_file_poll(fakefs_initctl_fifo, fd);
}

static int fakefs_close(struct fd *fd) {
    if (fakefs_is_initctl_fd(fd)) {
        if (fakefs_initctl_fifo != NULL)
            fifo_file_close(fakefs_initctl_fifo, fd);
        free(fd->fs_data);
        fd->fs_data = NULL;
        return 0;
    }
    return realfs_close(fd);
}

static int fakefs_getpath(struct fd *fd, char *buf) {
    if (fakefs_is_initctl_fd(fd) && fd->fs_data != NULL) {
        snprintf(buf, MAX_PATH + 1, "%s", (char *) fd->fs_data);
        return 0;
    }
    if (fd->mount != NULL && fd->fake_inode != 0) {
        struct fakefs_db *fs = fakefs_db_thread(&fd->mount->fakefs);
        FAKEFS_LOCK_READ(fs);
        sqlite3_stmt *stmt = fs->stmt.path_from_inode;
        sqlite3_bind_int64(stmt, 1, fd->fake_inode);
        bool found = db_exec(fs, stmt);
        if (found) {
            const void *path_blob = sqlite3_column_blob(stmt, 0);
            int path_len = sqlite3_column_bytes(stmt, 0);
            if (path_len < 0 || path_len > MAX_PATH) {
                db_reset(fs, stmt);
                FAKEFS_UNLOCK_READ(fs);
                return _ENAMETOOLONG;
            }
            // sqlite3_column_blob returns NULL for a zero-length blob, and
            // memcpy from NULL is undefined even for zero bytes (UBSan flags
            // it: "null pointer passed as argument 2").
            if (path_len > 0)
                memcpy(buf, path_blob, (size_t) path_len);
            buf[path_len] = '\0';
            db_reset(fs, stmt);
            FAKEFS_UNLOCK_READ(fs);
            return 0;
        }
        db_reset(fs, stmt);
        FAKEFS_UNLOCK_READ(fs);
    }
    int err = realfs_getpath(fd, buf);
    if (err >= 0)
        // The host path is in escaped on-disk form.
        fake_path_from_host(buf);
    return err;
}

static struct fd *fakefs_open_initctl(const char *path, int flags) {
    const char *virtual_path;
    ino_t inode;
    if (!fakefs_initctl_info(path, &virtual_path, &inode))
        return ERR_PTR(_ENOENT);
    if ((flags & (O_CREAT_ | O_EXCL_)) == (O_CREAT_ | O_EXCL_))
        return ERR_PTR(_EEXIST);
    if (flags & O_DIRECTORY_)
        return ERR_PTR(_ENOTDIR);

    struct fd *fd = fd_create(&initctl_fdops);
    if (fd == NULL)
        return ERR_PTR(_ENOMEM);
    fd->real_fd = -1;
    fd->dir = NULL;
    fd->flags = flags;
    fd->type = S_IFIFO;
    fd->fake_inode = inode;
    fakefs_initctl_statbuf(&fd->stat, inode);
    fd->fs_data = strdup(virtual_path);
    if (fd->fs_data == NULL) {
        fd_close(fd);
        return ERR_PTR(_ENOMEM);
    }
    struct fifo_file *fifo = fakefs_get_initctl_fifo();
    if (fifo == NULL) {
        fd_close(fd);
        return ERR_PTR(_ENOMEM);
    }
    int ferr = fifo_file_open(fifo, fd); // fd->flags was set above
    if (ferr < 0) {
        fd_close(fd);
        return ERR_PTR(ferr);
    }
    return fd;
}

// Linux's inode_init_owner: a newly created object takes the creating
// process's group -- UNLESS the directory it is created in is setgid, in which
// case it takes the DIRECTORY's group, and a new subdirectory inherits the
// setgid bit as well.
//
// That rule is the whole mechanism behind a shared group directory: everything
// created inside it belongs to the group whoever made it, so the next person
// can read and write it. AOK stamped the creator's own egid on every new
// object and never copied the bit down, so a shared directory stopped being
// shared the moment anybody with a different primary group created something
// in it -- and the subdirectory they made was not shared either, so the damage
// spread downwards.
//
// `path` is the mount-relative path of the object about to be created; the
// parent is everything before its last slash. Caller is inside the same db
// transaction, so this is a read against uncommitted-but-consistent state.
static void fakefs_inherit_group(struct fakefs_db *fs, const char *path,
                                 uint32_t *mode, uint32_t *gid) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL)
        return;
    char parent[MAX_PATH];
    size_t len = (size_t) (slash - path);
    // A child of the mount root: the parent path is "", which is how this
    // filesystem spells its own root.
    if (len >= sizeof(parent))
        return;
    memcpy(parent, path, len);
    parent[len] = '\0';

    struct ish_stat parent_stat;
    if (!path_read_stat(fs, parent, &parent_stat, NULL))
        return;
    if (!S_ISDIR(parent_stat.mode) || !(parent_stat.mode & S_ISGID))
        return;

    *gid = parent_stat.gid;
    if (S_ISDIR(*mode))
        *mode |= S_ISGID;
}

static struct fd *fakefs_open(struct mount *mount, const char *path, int flags, int mode) {
    if (fakefs_initctl_info(path, NULL, NULL))
        return fakefs_open_initctl(path, flags);
    struct fakefs_db *fs = fakefs_db_thread(&mount->fakefs);
    host_path_t host_path;
    int name_err = fakefs_host_path(path, host_path);
    if (name_err < 0)
        return ERR_PTR(name_err);
    // For O_CREAT, take the metadata transaction lock *before* the real
    // host create, so the host-side create and the SQLite metadata write
    // that describes it are atomic with each other (previously the host
    // create happened with no fakefs lock held at all, leaving a window
    // where a concurrent mkdir/unlink/symlink/mknod on the same path could
    // run its own real-op+metadata pair and interleave with this one,
    // leaving ish_stat.mode describing a different entry type than what's
    // actually on the host filesystem). This is still paired with
    // inodes_lock in generic_openat/generic_mkdirat/etc, which is what
    // actually serializes against those other syscalls; this ordering
    // just keeps fakefs_open internally consistent as well.
    int open_flags = flags;
retry:
    if (open_flags & O_CREAT_) {
        db_begin_write(fs);
        // Deadlock guard: opening an existing FIFO without O_NONBLOCK blocks
        // in the host openat() until a peer opens the other end. Holding the
        // write transaction (fs->lock) across that wedges every fakefs
        // operation in the process -- the peer's own open can't even stat the
        // path, so nothing ever unblocks us (observed: a shell's `cmd > fifo`
        // redirect, which is O_WRONLY|O_CREAT, racing the reader's `< fifo`
        // in start-wayland.sh's FIFO-logged spawns; whole emulator froze).
        // O_CREAT on an existing path creates nothing, so there is no host
        // create + metadata write pair to keep atomic here -- drop the
        // transaction and take the non-creating (unlocked) open path below.
        // If the FIFO is unlinked between this check and the real open,
        // realfs ENOENT retries from the top with create semantics restored.
        if (!(open_flags & O_NONBLOCK_)) {
            struct ish_stat existing_stat;
            if (path_read_stat(fs, path, &existing_stat, NULL) &&
                    S_ISFIFO(existing_stat.mode)) {
                db_rollback(fs);
                open_flags &= ~O_CREAT_;
            }
        }
    }
    struct fd *fd = realfs.open(mount, host_path, open_flags, 0666);
    if (IS_ERR(fd)) {
        if (open_flags & O_CREAT_)
            db_rollback(fs);
        // The FIFO this open was demoted for (see the deadlock guard above)
        // was unlinked before the real open; retry with O_CREAT restored.
        if (PTR_ERR(fd) == _ENOENT && (flags & O_CREAT_) && !(open_flags & O_CREAT_)) {
            open_flags = flags;
            goto retry;
        }
        return fd;
    }
    if (!(open_flags & O_CREAT_))
        db_begin_read(fs);
    fd->fake_inode = path_get_inode(fs, path);
    if (open_flags & O_CREAT_) {
        struct ish_stat ishstat;
        ishstat.mode = mode | S_IFREG;
        ishstat.uid = current->euid;
        ishstat.gid = current->egid;
        fakefs_inherit_group(fs, path, &ishstat.mode, &ishstat.gid);
        ishstat.rdev = 0;
        if (fd->fake_inode == 0)
            fd->fake_inode = path_create(fs, path, &ishstat);
    }
    db_commit(fs);
    if (fd->fake_inode == 0) {
        // File exists on the real FS but has no fakefs metadata (pre-existing inconsistency).
        fd_close(fd);
        return ERR_PTR(_ENOENT);
    }
    fakefs_snapshot_fd_stat(fd);
    fd->ops = &fakefs_fdops;
    return fd;
}

// WARNING: giant hack, just for file providerws
struct fd *fakefs_open_inode(struct mount *mount, ino_t inode) {
    struct fakefs_db *fs = fakefs_db_thread(&mount->fakefs);
    db_begin_read(fs);
    sqlite3_stmt *stmt = fs->stmt.path_from_inode;
    sqlite3_bind_int64(stmt, 1, inode);
step:
    if (!db_exec(fs, stmt)) {
        db_reset(fs, stmt);
        db_rollback(fs);
        return ERR_PTR(_ENOENT);
    }
    const char *path = (const char *) sqlite3_column_text(stmt, 0);
    host_path_t host_path;
    if (fakefs_host_path(path, host_path) < 0)
        goto step;
    struct fd *fd = realfs.open(mount, host_path, O_RDWR_, 0);
    if (PTR_ERR(fd) == _EISDIR)
        fd = realfs.open(mount, host_path, O_RDONLY_, 0);
    if (PTR_ERR(fd) == _ENOENT)
        goto step;
    if (IS_ERR(fd)) {
        db_reset(fs, stmt);
        db_rollback(fs);
        return fd;
    }
    db_reset(fs, stmt);
    db_commit(fs);
    fd->fake_inode = inode;
    fakefs_snapshot_fd_stat(fd);
    fd->ops = &fakefs_fdops;
    return fd;
}

static int fakefs_link(struct mount *mount, const char *src, const char *dst) {
    struct fakefs_db *fs = fakefs_db_thread(&mount->fakefs);
    host_path_t host_src, host_dst;
    int name_err = fakefs_host_path(src, host_src);
    if (name_err >= 0)
        name_err = fakefs_host_path(dst, host_dst);
    if (name_err < 0)
        return name_err;
    db_begin_write(fs);
    int err = realfs.link(mount, host_src, host_dst);
    if (err == _EEXIST && path_get_inode(fs, dst) == 0) {
        // Recover from a stale host-side temp path that is missing from fakefs.
        int cleanup_err = realfs.unlink(mount, host_dst);
        if (cleanup_err == 0 || cleanup_err == _ENOENT)
            err = realfs.link(mount, host_src, host_dst);
    }
    if (err < 0) {
        db_rollback(fs);
        return err;
    }
    path_link(fs, src, dst);
    db_commit(fs);
    return 0;
}

static int fakefs_unlink(struct mount *mount, const char *path) {
    struct fakefs_db *fs = fakefs_db_thread(&mount->fakefs);
    host_path_t host_path;
    int name_err = fakefs_host_path(path, host_path);
    if (name_err < 0)
        return name_err;
    db_begin_write(fs);
    int err = realfs.unlink(mount, host_path);
    if (err < 0) {
        db_rollback(fs);
        return err;
    }
    ino_t ino = path_unlink(fs, path);
    db_commit(fs);
    // generic_unlinkat holds inodes_lock across this whole call (to serialize
    // against a concurrent open(O_CREAT)/mkdir/etc. on the same path), so use
    // the _unlocked variant here -- inodes_lock is a plain non-recursive
    // mutex, and calling the locking inode_check_orphaned() from within that
    // critical section would self-deadlock the calling thread.
    inode_check_orphaned_unlocked(mount, ino);
    return 0;
}

static int fakefs_rmdir(struct mount *mount, const char *path) {
    struct fakefs_db *fs = fakefs_db_thread(&mount->fakefs);
    host_path_t host_path;
    int name_err = fakefs_host_path(path, host_path);
    if (name_err < 0)
        return name_err;
    db_begin_write(fs);
    int err = realfs.rmdir(mount, host_path);
    if (err == _ENOTDIR) {
        // The host says this is not a directory while the metadata says it is.
        // That pair is unreachable through any sequence of guest operations,
        // and it used to be a dead end: rmdir(2) fails here on the host's
        // ENOTDIR, while unlink(2) never gets this far because generic_unlinkat
        // reads the metadata and answers EISDIR. The name could then never be
        // removed or reused -- fatal for a guest gcc, which reuses temporary
        // names and aborts with "Cannot create temporary file in /tmp/: Is a
        // directory". fs/fake-db.c's path_create no longer manufactures this
        // (an inode collision used to alias one path onto another file's stat
        // blob) and the v7 pass in fs/fake-migrate.c repairs roots already
        // carrying it, but a root can still arrive here from an older build, so
        // give rmdir the power to finish what the guest asked.
        //
        // Gated on the metadata's directory bit alone. fakefs stores symlinks,
        // sockets and device nodes as ordinary host files whose real type lives
        // only in the metadata, so rmdir("symlink-to-dir") and rmdir("file")
        // still take the normal ENOTDIR path below -- as they must.
        struct ish_stat ishstat;
        if (path_read_stat(fs, path, &ishstat, NULL) && S_ISDIR(ishstat.mode)) {
            err = realfs.unlink(mount, host_path);
            if (err >= 0)
                printk("fakefs: removed type-mismatched entry %s (metadata said directory, host did not)\n", path);
        }
    }
    if (err < 0) {
        db_rollback(fs);
        return err;
    }
    ino_t ino = path_unlink(fs, path);
    db_commit(fs);
    // See the comment in fakefs_unlink: generic_rmdirat holds inodes_lock
    // across this call, so use the _unlocked variant to avoid self-deadlock.
    inode_check_orphaned_unlocked(mount, ino);
    return 0;
}

static int fakefs_rename(struct mount *mount, const char *src, const char *dst) {
    struct fakefs_db *fs = fakefs_db_thread(&mount->fakefs);
    host_path_t host_src, host_dst;
    int name_err = fakefs_host_path(src, host_src);
    if (name_err >= 0)
        name_err = fakefs_host_path(dst, host_dst);
    if (name_err < 0)
        return name_err;
    db_begin_write(fs);
    path_rename(fs, src, dst);
    int err = realfs.rename(mount, host_src, host_dst);
    if (err < 0) {
        db_rollback(fs);
        return err;
    }
    db_commit(fs);
    return 0;
}

static int fakefs_symlink(struct mount *mount, const char *target, const char *link) {
    struct fakefs_db *fs = fakefs_db_thread(&mount->fakefs);
    host_path_t host_link;
    int name_err = fakefs_host_path(link, host_link);
    if (name_err < 0)
        return name_err;
    db_begin_write(fs);
    // fakefs historically stores symlinks as regular files containing the
    // link target, with metadata overridden to present them as S_IFLNK.
    // Restore that behavior to match the working branch semantics used by the
    // bundled roots and package-manager flows.
    int fd = openat(mount->root_fd, fix_path(host_link), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0) {
        fakefs_trace_symlink_result(mount, fs, target, link, -1, errno);
        db_rollback(fs);
        return errno_map();
    }
    size_t target_len = strlen(target);
    ssize_t res = write(fd, target, target_len);
    close(fd);
    if (res != (ssize_t) target_len) {
        int saved_errno = errno;
        unlinkat(mount->root_fd, fix_path(host_link), 0);
        db_rollback(fs);
        if (res < 0) {
            errno = saved_errno;
            return errno_map();
        }
        return _EIO;
    }

    // customize the stat info so it looks like a link
    struct ish_stat ishstat;
    ishstat.mode = S_IFLNK | 0777; // symlinks always have full permissions
    ishstat.uid = current->euid;
    ishstat.gid = current->egid;
    fakefs_inherit_group(fs, link, &ishstat.mode, &ishstat.gid);
    ishstat.rdev = 0;
    if (path_create(fs, link, &ishstat) == 0) {
        // Without its metadata row the host file is not a symlink at all, just
        // a file holding a path; see fakefs_mknod.
        db_rollback(fs);
        unlinkat(mount->root_fd, fix_path(host_link), 0);
        return _EIO;
    }
    fakefs_trace_symlink_result(mount, fs, target, link, 0, 0);
    db_commit(fs);
    return 0;
}

static int fakefs_mknod(struct mount *mount, const char *path, mode_t_ mode, dev_t_ dev) {
    struct fakefs_db *fs = fakefs_db_thread(&mount->fakefs);
    mode_t_ real_mode = 0666;
    if (S_ISBLK(mode) || S_ISCHR(mode) || S_ISSOCK(mode))
        real_mode |= S_IFREG;
    else
        real_mode |= mode & S_IFMT;
    host_path_t host_path;
    int name_err = fakefs_host_path(path, host_path);
    if (name_err < 0)
        return name_err;
    db_begin_write(fs);
    int err = realfs.mknod(mount, host_path, real_mode, 0);
    if (err < 0) {
        db_rollback(fs);
        return err;
    }
    struct ish_stat stat;
    stat.mode = mode;
    stat.uid = current->euid;
    stat.gid = current->egid;
    fakefs_inherit_group(fs, path, &stat.mode, &stat.gid);
    stat.rdev = 0;
    if (S_ISBLK(mode) || S_ISCHR(mode))
        stat.rdev = dev;
    if (path_create(fs, path, &stat) == 0) {
        // No metadata means no file, so take the host entry back out rather
        // than leave one the guest can see but never describe.
        db_rollback(fs);
        realfs.unlink(mount, host_path);
        return _EIO;
    }
    db_commit(fs);
    return err;
}

static int fakefs_stat(struct mount *mount, const char *path, struct statbuf *fake_stat) {
    ino_t initctl_inode;
    if (fakefs_initctl_info(path, NULL, &initctl_inode)) {
        fakefs_initctl_statbuf(fake_stat, initctl_inode);
        return 0;
    }
    struct fakefs_db *fs = fakefs_db_thread(&mount->fakefs);
    FAKEFS_LOCK_READ(fs);
    struct ish_stat ishstat;
    ino_t inode;
    if (!path_read_stat(fs, path, &ishstat, &inode)) {
        FAKEFS_UNLOCK_READ(fs);
        return _ENOENT;
    }
    FAKEFS_UNLOCK_READ(fs);

    host_path_t host_path;
    int err = fakefs_host_path(path, host_path);
    if (err < 0)
        return err;
    err = realfs.stat(mount, host_path, fake_stat);
    if (err < 0)
        return err;
    fakefs_apply_stat_override(fake_stat, inode, &ishstat);
    return 0;
}

static int fakefs_fstat(struct fd *fd, struct statbuf *fake_stat) {
    if (fakefs_is_initctl_fd(fd)) {
        *fake_stat = fd->stat;
        return 0;
    }
    struct fakefs_db *fs = fakefs_db_thread(&fd->mount->fakefs);
    int err = realfs.fstat(fd, fake_stat);
    if (err < 0)
        return err;
    FAKEFS_LOCK_READ(fs);
    struct ish_stat ishstat;
    bool found = inode_read_stat(fs, fd->fake_inode, &ishstat);
    FAKEFS_UNLOCK_READ(fs);
    if (!found) {
        // Linux still allows fstat() on an unlinked-but-open file.
        // Preserve the most recent fake metadata snapshot on the fd.
        *fake_stat = fd->stat;
        return 0;
    }
    fakefs_apply_stat_override(fake_stat, fd->fake_inode, &ishstat);
    fd->stat = *fake_stat;
    return 0;
}

static void fake_stat_setattr(struct ish_stat *ishstat, struct attr attr) {
    switch (attr.type) {
        case attr_uid:
            ishstat->uid = attr.uid;
            break;
        case attr_gid:
            ishstat->gid = attr.gid;
            break;
        case attr_mode:
            ishstat->mode = (attr.mode & S_IFMT ? attr.mode & S_IFMT : ishstat->mode & S_IFMT) |
                (attr.mode & ~S_IFMT);
            break;
    }
}

static int fakefs_setattr(struct mount *mount, const char *path, struct attr attr) {
    struct fakefs_db *fs = fakefs_db_thread(&mount->fakefs);
    if (attr.type == attr_size) {
        host_path_t host_path;
        int name_err = fakefs_host_path(path, host_path);
        if (name_err < 0)
            return name_err;
        return realfs.setattr(mount, host_path, attr);
    }
    db_begin_write(fs);
    struct ish_stat ishstat;
    ino_t inode;
    if (!path_read_stat(fs, path, &ishstat, &inode)) {
        db_rollback(fs);
        return _ENOENT;
    }
    fake_stat_setattr(&ishstat, attr);
    inode_write_stat(fs, inode, &ishstat);
    db_commit(fs);
    return 0;
}

static int fakefs_fsetattr(struct fd *fd, struct attr attr) {
    struct fakefs_db *fs = fakefs_db_thread(&fd->mount->fakefs);
    if (attr.type == attr_size)
        return realfs.fsetattr(fd, attr);
    db_begin_write(fs);
    struct ish_stat ishstat;
    if (!inode_read_stat(fs, fd->fake_inode, &ishstat)) {
        db_rollback(fs);
        switch (attr.type) {
            case attr_uid:
                fd->stat.uid = attr.uid;
                return 0;
            case attr_gid:
                fd->stat.gid = attr.gid;
                return 0;
            case attr_mode:
                fd->stat.mode = (attr.mode & S_IFMT ? attr.mode & S_IFMT : fd->stat.mode & S_IFMT) |
                    (attr.mode & ~S_IFMT);
                return 0;
            default:
                return _ENOENT;
        }
    }
    fake_stat_setattr(&ishstat, attr);
    inode_write_stat(fs, fd->fake_inode, &ishstat);
    db_commit(fs);
    return 0;
}

static int fakefs_mkdir(struct mount *mount, const char *path, mode_t_ mode) {
    struct fakefs_db *fs = fakefs_db_thread(&mount->fakefs);
    host_path_t host_path;
    int name_err = fakefs_host_path(path, host_path);
    if (name_err < 0)
        return name_err;
    db_begin_write(fs);
    int err = realfs.mkdir(mount, host_path, 0777);
    if (err < 0) {
        db_rollback(fs);
        return err;
    }
    struct ish_stat ishstat;
    ishstat.mode = mode | S_IFDIR;
    ishstat.uid = current->euid;
    ishstat.gid = current->egid;
    fakefs_inherit_group(fs, path, &ishstat.mode, &ishstat.gid);
    ishstat.rdev = 0;
    if (path_create(fs, path, &ishstat) == 0) {
        // See fakefs_mknod: a host entry with no metadata row behind it is
        // worse than a failed mkdir.
        db_rollback(fs);
        realfs.rmdir(mount, host_path);
        return _EIO;
    }
    db_commit(fs);
    return 0;
}

static ssize_t file_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    // broken symlinks can't be included in an iOS app or else Xcode craps out
    int fd = openat(mount->root_fd, fix_path(path), O_RDONLY);
    if (fd < 0)
        return errno_map();
    int err = read(fd, buf, bufsize);
    close(fd);
    if (err < 0)
        return errno_map();
    return err;
}

static ssize_t fakefs_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    struct fakefs_db *fs = fakefs_db_thread(&mount->fakefs);
    db_begin_read(fs);
    struct ish_stat ishstat;
    if (!path_read_stat(fs, path, &ishstat, NULL)) {
        db_rollback(fs);
        return _ENOENT;
    }
    if (!S_ISLNK(ishstat.mode)) {
        db_rollback(fs);
        return _EINVAL;
    }

    host_path_t host_path;
    ssize_t err = fakefs_host_path(path, host_path);
    if (err >= 0) {
        err = realfs.readlink(mount, host_path, buf, bufsize);
        if (err == _EINVAL)
            err = file_readlink(mount, host_path, buf, bufsize);
    }
    if (err < 0)
        db_rollback(fs);
    else
        db_commit(fs);
    return err;
}

static int fakefs_readdir(struct fd *fd, struct dir_entry *entry) {
    assert(fd->ops == &fakefs_fdops);
    int res;
retry:
    res = realfs_fdops.readdir(fd, entry);
    if (res <= 0)
        return res;

    // this is annoying
    char entry_path[MAX_PATH + 1];
    realfs_getpath(fd, entry_path);
    // Cache strlen to avoid redundant calculations in bounds checking and loops
    size_t name_len = strlen(entry->name);
    if (name_len == 2 && entry->name[0] == '.' && entry->name[1] == '.') {
        if (entry_path[0] != '\0') {
            *strrchr(entry_path, '/') = '\0';
        }
    } else if (!(name_len == 1 && entry->name[0] == '.')) {
        size_t path_len = strlen(entry_path);
        // god I don't know what to do if this would overflow
        if (path_len + 1 + name_len >= sizeof(entry_path))
            goto retry;
        entry_path[path_len] = '/';
        memcpy(entry_path + path_len + 1, entry->name, name_len + 1);
    }

    // Both entry->name (from the host readdir) and entry_path (from the host
    // F_GETPATH) are in escaped on-disk form; the DB and the guest speak the
    // unescaped guest names.
    fake_path_from_host(entry_path);
    fake_path_from_host(entry->name);

    struct fakefs_db *fs = fakefs_db_thread(&fd->mount->fakefs);
    db_begin_read(fs);
    struct ish_stat ishstat;
    ino_t inode;
    bool found = path_read_stat(fs, entry_path, &ishstat, &inode);
    db_commit(fs);
    // it's quite possible that due to some mishap there's no metadata for this file
    // so just skip this entry, instead of crashing the program, so there's hope for recovery
    if (!found || inode == 0)
        goto retry;
    entry->inode = inode;
    entry->type = dir_entry_type_for_mode(ishstat.mode);
    return res;
}

static struct fd_ops fakefs_fdops;
static void __attribute__((constructor)) init_fake_fdops() {
    fakefs_fdops = realfs_fdops;
    fakefs_fdops.readdir = fakefs_readdir;
    fakefs_fdops.close = fakefs_close;
    initctl_fdops = (struct fd_ops) {
        .read = initctl_read,
        .write = initctl_write,
        .pread = initctl_pread,
        .pwrite = initctl_pwrite,
        .lseek = initctl_lseek,
        .poll = initctl_poll,
        .close = fakefs_close,
    };
}

static int fakefs_utime(struct mount *mount, const char *path, struct timespec atime, struct timespec mtime, bool follow_links) {
    host_path_t host_path;
    int name_err = fakefs_host_path(path, host_path);
    if (name_err < 0)
        return name_err;
    return realfs_utime(mount, host_path, atime, mtime, follow_links);
}

static int fakefs_mount(struct mount *mount) {
    char db_path[PATH_MAX];
    strncpy(db_path, mount->source, PATH_MAX - 1);
    db_path[PATH_MAX - 1] = '\0';
    char *slash = strrchr(db_path, '/');
    // The metadata DB lives next to the source dir as "meta.db", found by
    // swapping the final "data" path component -- an internal convention of
    // how this project's own root-installation code names its fakefs source
    // dirs, not something the mount(2) syscall enforces. Any guest process
    // with root can reach this via a raw `mount -t fake <source> <target>`
    // with an arbitrary source path, so a mismatch (or a source with no
    // directory component at all) must be a plain mount error, not an assert
    // that aborts the whole host process (found via a guest-side
    // `mount -t fake /some/other/name /AOK/roots/foo`, which used to crash
    // the entire app instead of just failing the mount).
    if (slash == NULL || strcmp(slash + 1, "data") != 0)
        return _EINVAL;
    strncpy(slash + 1, "meta.db", 8);

    // do this now so rebuilding can use root_fd
    int err = realfs.mount(mount);
    if (err < 0)
        return err;

    err = fake_db_init(&mount->fakefs, db_path, mount->root_fd);
    if (err < 0)
        return err;

    return 0;
}

static int fakefs_umount(struct mount *mount) {
    int err = fake_db_deinit(&mount->fakefs);
    if (err != SQLITE_OK) {
        printk("ERROR: sqlite failed to close: %d\n", err);
    }
    /* return realfs.umount(mount); */
    return 0;
}

static void fakefs_inode_orphaned(struct mount *mount, ino_t inode) {
    struct fakefs_db *fs = fakefs_db_thread(&mount->fakefs);
    db_begin_write(fs);
    sqlite3_bind_int64(fs->stmt.try_cleanup_inode, 1, inode);
    db_exec_reset(fs, fs->stmt.try_cleanup_inode);
    db_commit(fs);
}

const struct fs_ops fakefs = {
    .name = "fake", .magic = 0x66616b65,
    .mount = fakefs_mount,
    .umount = fakefs_umount,
    .statfs = realfs_statfs,
    .open = fakefs_open,
    .readlink = fakefs_readlink,
    .link = fakefs_link,
    .unlink = fakefs_unlink,
    .rename = fakefs_rename,
    .symlink = fakefs_symlink,
    .mknod = fakefs_mknod,

    .close = fakefs_close,
    .stat = fakefs_stat,
    .fstat = fakefs_fstat,
    .flock = realfs_flock,
    .setattr = fakefs_setattr,
    .fsetattr = fakefs_fsetattr,
    .getpath = fakefs_getpath,
    .utime = fakefs_utime,
    .futime = realfs_futime,

    .mkdir = fakefs_mkdir,
    .rmdir = fakefs_rmdir,

    .inode_orphaned = fakefs_inode_orphaned,
};
