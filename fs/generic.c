#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "kernel/fs.h"
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/path.h"
#include "fs/proc.h"
#include "fs/dev.h"
#include "kernel/inotify.h"
#include "kernel/task.h"
#include "kernel/errno.h"

static struct fdtable *procfd_task_files_retain(struct task *task) {
    struct fdtable *files = NULL;
    lock(&task->general_lock, 0);
    if (!task->exiting && task->files != NULL)
        files = fdtable_retain(task->files);
    unlock(&task->general_lock);
    return files;
}

static struct fd *procfd_reopen_regular(struct fd *fd, int flags) {
    if (fd->mount == NULL || fd->mount->fs == &procfs || !S_ISREG(fd->type))
        return NULL;

    char path[MAX_PATH];
    int err = generic_getpath(fd, path);
    if (err < 0)
        return NULL;

    // Linux reopens a /proc/<pid>/fd/N magic link's target with the CALLER's
    // flags -- that is exactly how systemd's fd_reopen() upgrades an O_PATH
    // fd to O_RDWR (xopenat_full with path=NULL). This used to reopen with
    // the ORIGINAL descriptor's flags instead, so an O_RDWR request against
    // an O_PATH fd silently produced a read-only description: ftruncate then
    // failed EINVAL, which killed systemd-machine-id-setup on a fresh Arch
    // rootfs and cascaded into the dbus-broker 90s death loop that froze the
    // whole boot. O_CREAT/O_EXCL are dropped: the target exists (we hold an
    // fd to it), and if its path was meanwhile unlinked, creating a NEW file
    // at the stale path would be wrong.
    // The stored target path is fully normalized (chroot prefix included);
    // open it against the real root or a chrooted caller re-prefixes it.
    struct fd *reopened = generic_open_realroot(path,
            flags & ~(O_CLOEXEC_ | O_NOFOLLOW_ | O_CREAT_ | O_EXCL_), 0);
    if (IS_ERR(reopened))
        return NULL;
    return reopened;
}

// True when an fd opened with `have` flags can stand in for a description
// requested with `want` flags, access-mode-wise.
static bool procfd_accmode_ok(int have, int want) {
    have &= O_ACCMODE_;
    want &= O_ACCMODE_;
    return have == want || have == O_RDWR_;
}

// Resolves path_raw down to a /proc/PID/fd/N entry, following symlinks by
// hand rather than through path_normalize's N_SYMLINK_FOLLOW, and returns a
// retained reference to the underlying struct fd if so. False (nothing
// retained) when path_raw doesn't ultimately name such an entry.
//
// A single path_normalize(..., N_SYMLINK_NOFOLLOW) call is not enough: it
// leaves the RAW INPUT's own final component unresolved, which is exactly
// right when that input directly names "/proc/PID/fd/N" (intermediate
// components like "PID"/"self" are still followed normally, only the
// final "N" stays as symlink text -- so its descriptive readlink target,
// "pipe:[12345]" or similar, never gets chased as if it were a real path).
// But /dev/stdin is a DIFFERENT symlink whose own target merely HAPPENS to
// end in "/proc/self/fd/0" -- NOFOLLOW on ITS final component stops at
// "/dev/stdin" itself, never reaching procfs at all, so callers taking
// that indirection (install(1) statting /dev/stdin among them -- GH #527)
// saw this fail even though a direct /proc/self/fd/0 argument worked.
// Chase one hop at a time instead, re-checking the procfs-fd/N shape
// after each, so any number of symlink hops on the way in still lands
// correctly once they bottom out at a real fd/N entry.
static bool procfd_resolve(struct fd *at, const char *path_raw, struct fd **fd_out) {
    char path[MAX_PATH];
    strncpy(path, path_raw, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    for (int hops = 0; hops < 10; hops++) {
        char normalized[MAX_PATH];
        // `at` (not reassigned across hops): path_normalize ignores it for
        // an absolute path, which every hop's target is in the real-world
        // case this exists for. See the note below the readlink call for
        // why a relative-target hop isn't handled more precisely than this.
        int err = path_normalize(at, path, normalized, N_SYMLINK_NOFOLLOW);
        if (err < 0)
            return false;

        struct mount *mount = find_mount_and_trim_path(normalized);
        if (mount == NULL)
            return false;
        bool is_procfs = mount->fs == &procfs;
        int pid = 0, fd_no = 0, n = 0;
        bool matched = is_procfs &&
            sscanf(normalized, "/%d/fd/%d%n", &pid, &fd_no, &n) == 2 && normalized[n] == '\0';
        if (matched) {
            mount_release(mount);
            struct task *task = pid_get_task_ref(pid);
            if (task == NULL)
                return false;
            struct fdtable *files = procfd_task_files_retain(task);
            if (files == NULL) {
                task_ref_cnt_mod(task, -1);
                return false;
            }
            lock(&files->lock, 0);
            struct fd *fd = fdtable_get(files, fd_no);
            if (fd != NULL)
                fd = fd_retain(fd);
            unlock(&files->lock);
            fdtable_release(files);
            task_ref_cnt_mod(task, -1);
            if (fd == NULL)
                return false;
            *fd_out = fd;
            return true;
        }
        // Not (yet) a procfs fd/N entry: if the fully-intermediate-resolved
        // final component is itself a symlink, follow it by hand and retry.
        char target[MAX_PATH];
        ssize_t target_len = mount->fs->readlink != NULL
            ? mount->fs->readlink(mount, normalized, target, sizeof(target) - 1)
            : _EINVAL;
        mount_release(mount);
        if (target_len < 0)
            return false; // not a symlink (or unreadable) -- genuinely not this pattern
        target[target_len] = '\0';
        // path_normalize ignores `at` for an absolute path (leading '/'), so
        // hop_at only matters for a relative target -- which the real-world
        // case this exists for (/dev/stdin -> /proc/self/fd/0) never is. A
        // relative target left as-is resolves against the ORIGINAL caller's
        // `at`/cwd rather than the symlink's own containing directory,
        // which is wrong in the general case but not a regression: this
        // whole multi-hop path is new, and no caller could reach a procfs
        // fd/N entry through a relative-target hop before this existed.
        strncpy(path, target, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }
    return false; // too many hops -- treat like ELOOP by just not matching
}

static struct fd *procfd_openat(struct fd *at, const char *path_raw, int flags) {
    // O_NOFOLLOW must fail the open with ELOOP and O_PATH|O_NOFOLLOW must
    // open the magic symlink itself; both operate on the link, not the
    // target, so leave them to normal path resolution.
    if (flags & (O_NOFOLLOW_ | O_PATH_))
        return NULL;
    struct fd *fd;
    if (!procfd_resolve(at, path_raw, &fd))
        return NULL;

    // Linux procfd opens give regular files a fresh file position and the
    // CALLER's flags, which shell script loaders rely on when they execute
    // /proc/self/fd/N after the parent has already inspected the script FD.
    // Prefer a reopen for normal file-backed descriptors.
    struct fd *reopened = procfd_reopen_regular(fd, flags);
    if (reopened != NULL) {
        fd_close(fd);
        return reopened;
    }
    // Deleted or anonymous regular files may not have a stable path we can
    // reopen. We cannot cheaply create a distinct open-file description here,
    // but resetting the retained descriptor keeps shell interpreters from
    // starting mid-script after apk has read the shebang. Never hand back a
    // descriptor WEAKER than the caller asked for, though -- a silently
    // read-only "O_RDWR" fd fails much later and much more confusingly than
    // an up-front error (see procfd_reopen_regular's machine-id war story).
    if (!procfd_accmode_ok(fd_getflags(fd), flags)) {
        fd_close(fd);
        return ERR_PTR(_EACCES);
    }
    if (S_ISREG(fd->type) && fd->ops != NULL && fd->ops->lseek != NULL)
        fd->ops->lseek(fd, 0, SEEK_SET);
    return fd;
}

// Like procfd_openat, but for stat(2)/lstat(2)-following-the-final-symlink
// on a /proc/PID/fd/N entry: Linux gives these their own getattr that
// reports the pointee's real attributes directly. The generic symlink-
// chasing path resolution instead takes the descriptive readlink target
// ("pipe:[12345]", "socket:[67890]", "anon_inode:[eventfd]") and tries to
// re-resolve THAT as a path -- which it isn't, so it always fails ENOENT.
// Real-world hit: /dev/stdin is a plain symlink to /proc/self/fd/0; any
// tool that stat()s its source before reading it (install(1) is one) fails
// statting a pipe/socket/anon-inode fd reached that way (GH #527).
// Returns false (result untouched) when path isn't a /proc/PID/fd/N entry,
// so the caller falls through to normal resolution.
bool procfd_statat(struct fd *at, const char *path_raw, struct statbuf *stat, int *err_out) {
    struct fd *fd;
    if (!procfd_resolve(at, path_raw, &fd))
        return false;
    *err_out = generic_fstat(fd, stat);
    fd_close(fd);
    return true;
}

struct mount *find_mount_and_trim_path(char *path) {
    struct mount *mount = mount_find(path);
    if (mount == NULL)
        return NULL;
    char *dst = path;
    const char *src = path + mount->point_len;
    while (*src != '\0')
        *dst++ = *src++;
    *dst = '\0';

    // Bind mount: it has no backing of its own, so redirect to the origin mount.
    // Rewrite the (now mount-relative) path to bind_prefix + path and return the
    // origin instead. bind_origin/bind_prefix are immutable for the mount's life,
    // and the reference taken by mount_find keeps the bind alive (and thus those
    // fields valid) while we read them. The caller's buffer is MAX_PATH (every
    // caller normalizes into one), so a redirect that fits is safe to copy back;
    // an over-long result resolves to "not found" rather than overflowing.
    if (mount->bind_origin != NULL) {
        struct mount *origin = mount->bind_origin;
        char redirected[MAX_PATH];
        int n = snprintf(redirected, sizeof(redirected), "%s%s", mount->bind_prefix, path);
        if (n < 0 || (size_t) n >= sizeof(redirected)) {
            mount_release(mount);
            return NULL;
        }
        strcpy(path, redirected);
        mount_retain(origin);
        mount_release(mount);
        return origin;
    }
    return mount;
}

bool contains_mount_point(const char *path) {
    struct mount *mount;
    // Optimization: hoist strlen(path) outside the loop to avoid redundant O(N) recalculations
    int n = strlen(path);
    list_for_each_entry(&mounts, mount, mounts) {
        if (strncmp(path, mount->point, n) == 0 &&
                (mount->point[n] == '\0' || mount->point[n] == '/'))
            return true;
    }
    return false;
}

// fd referring to a symlink itself, from openat(O_PATH|O_NOFOLLOW) on a
// final symlink component (the primitive systemd's chase() is built on).
// No read/write/... ops -> those fail with EBADF, matching Linux O_PATH.
// Owns one mount reference and a malloc'd copy of the mount-relative path;
// fd->mount is deliberately left NULL (fd_close must not run the backend's
// close on an fd the backend never opened).
static int opath_link_close(struct fd *fd) {
    mount_release(fd->opath_link.mount);
    free(fd->opath_link.path);
    return 0;
}

static const struct fd_ops opath_link_ops = {
    .close = opath_link_close,
};

bool fd_is_opath_link(struct fd *fd) {
    // AT_PWD (fs/path.h) is a non-dereferenceable sentinel meaning "current
    // directory", not a real struct fd* -- generic_statat_full's
    // AT_EMPTY_PATH branch runs whenever a caller passes AT_FDCWD together
    // with AT_EMPTY_PATH and an empty path (e.g. systemd's chase() internals
    // do this), so `at` can legitimately be AT_PWD here. Without this check
    // fd->ops dereferenced (struct fd *)-2 + offsetof(ops), which wraps
    // (mod 2^64) to a low, easily-reached address and crashed with SIGSEGV
    // during Arch aarch64 boot.
    return fd != NULL && fd != AT_PWD && fd->ops == &opath_link_ops;
}

struct fd *opath_link_fd_create(struct mount *mount, const char *path) {
    struct fd *fd = adhoc_fd_create(&opath_link_ops);
    if (fd == NULL)
        return NULL;
    fd->opath_link.path = strdup(path);
    if (fd->opath_link.path == NULL) {
        fd->ops = NULL; // nothing to clean up; don't run opath_link_close
        fd_close(fd);
        return NULL;
    }
    fd->opath_link.mount = mount; // takes over the caller's reference
    fd->type = S_IFLNK;
    return fd;
}

// Live stat of the symlink an O_PATH-link fd refers to (fstat/AT_EMPTY_PATH).
int opath_link_fstat(struct fd *fd, struct statbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    struct mount *mount = fd->opath_link.mount;
    int err = mount->fs->stat(mount, fd->opath_link.path, stat);
    if (err >= 0 && stat->dev == 0)
        stat->dev = mount->fake_dev;
    return err;
}

struct mount *opath_link_get_mount(struct fd *fd) {
    return fd->opath_link.mount;
}

// readlinkat(fd, "", ...) on an O_PATH symlink fd (Linux allows exactly this).
ssize_t opath_link_readlink(struct fd *fd, char *buf, size_t bufsize) {
    struct mount *mount = fd->opath_link.mount;
    return mount->fs->readlink(mount, fd->opath_link.path, buf, bufsize);
}

// /proc/pid/ns/* entries are nsfs magic links (readlink text "mnt:[inode]"
// is an identity token, not a path). Linux opens them into a namespace fd;
// letting normal resolution follow the link text as a path yields ENOENT,
// which nix >= 2.30 treats as fatal ("saving parent mount namespace").
static struct fd *procns_open_path(struct fd *at, const char *path_raw) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return NULL;
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return NULL;
    if (mount->fs != &procfs) {
        mount_release(mount);
        return NULL;
    }
    int pid;
    char name[32];
    int n = 0;
    if (sscanf(path, "/%d/ns/%31[a-z_]%n", &pid, name, &n) != 2 || path[n] != '\0') {
        mount_release(mount);
        return NULL;
    }
    mount_release(mount);
    return proc_ns_open(pid, name);
}

static struct fd *procns_openat(struct fd *at, const char *path_raw, int flags) {
    // O_NOFOLLOW must fail the open with ELOOP and O_PATH|O_NOFOLLOW must
    // open the magic symlink itself; both are the link's own semantics, so
    // leave them to normal path resolution (same rule as procfd_openat).
    if (flags & (O_NOFOLLOW_ | O_PATH_))
        return NULL;
    return procns_open_path(at, path_raw);
}

// stat(2)/access(2) following the final component of a /proc/PID/ns/* magic
// link, for the same reason procfd_statat exists: the readlink text is an
// identity token, not a path, so the generic symlink chase re-resolves
// "uts:[4026531838]" as a path and always fails ENOENT. Linux answers from
// the nsfs inode instead, which is what the matching open() already returns
// here -- so borrow it.
//
// systemd probes support for a namespace type with exactly
// access("/proc/self/ns/<type>", F_OK) (namespace_type_supported()). Failing
// it made systemd skip unshare(CLONE_NEWUTS) altogether and log that the
// kernel has no UTS namespaces, which kept our UTS support invisible to the
// one consumer that motivated it (GH #527).
bool procns_statat(struct fd *at, const char *path_raw, struct statbuf *stat, int *err_out) {
    struct fd *fd = procns_open_path(at, path_raw);
    if (fd == NULL)
        return false; // not a /proc/PID/ns/* path -- fall through to normal resolution
    if (IS_ERR(fd)) {
        *err_out = (int) PTR_ERR(fd);
        return true;
    }
    *err_out = generic_fstat(fd, stat);
    fd_close(fd);
    return true;
}

static struct fd *generic_openat_norm(struct fd *at, const char *path_raw, int flags, int mode, int extra_norm) {
    if (flags & O_RDWR_ && flags & O_WRONLY_)
        return ERR_PTR(_EINVAL);

    struct fd *procfd = procfd_openat(at, path_raw, flags);
    if (procfd != NULL)
        return procfd;

    struct fd *nsfd = procns_openat(at, path_raw, flags);
    if (nsfd != NULL)
        return nsfd;

    // TODO really, really, seriously reconsider what I'm doing with the strings
    char path[MAX_PATH];
    // O_NOFOLLOW: do not resolve a *final* symlink component (intermediate
    // components are still followed), so opening one fails with ELOOP below.
    int norm = ((flags & O_NOFOLLOW_) ? N_SYMLINK_NOFOLLOW : N_SYMLINK_FOLLOW) | extra_norm;
    // N_PARENT_DIR_WRITE is deliberately NOT used here even though O_CREAT is
    // set: at this point we don't yet know whether the target already
    // exists. O_CREAT is very commonly passed defensively on an open() of an
    // existing file (e.g. open(path, O_CREAT|O_WRONLY, mode)), and Linux
    // only requires write access to the parent directory when a new dentry
    // is actually about to be created -- if the target exists, only the
    // target's own permissions matter. So the parent-write check below is
    // deferred until after we know (via the ENOENT-from-stat below) that we
    // are really creating something.
    int err = path_normalize(at, path_raw, path, norm);
    if (err < 0)
        return ERR_PTR(err);
    // find_mount_and_trim_path rewrites `path` in place to be MOUNT-RELATIVE
    // (and bind-redirected), but inotify watches are registered by full
    // normalized guest path (sys_inotify_add_watch) -- so notifications must
    // use the untrimmed path. On the root mount the two are identical, which
    // hid this: on any other mount (/run, /tmp, ...) trimmed notifications
    // matched nothing, and e.g. sd-bus clients parked in WATCH_BIND watching
    // /run/dbus never saw the bus socket appear. Same pattern in every
    // generic_* below that notifies.
    char guest_path[MAX_PATH];
    strncpy(guest_path, path, sizeof(guest_path) - 1);
    guest_path[sizeof(guest_path) - 1] = '\0';
    // A trailing slash demands a directory; open() must not create through it.
    size_t raw_len = strlen(path_raw);
    bool trailing_slash = raw_len > 0 && path_raw[raw_len - 1] == '/';
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return ERR_PTR(_ENOENT);

    bool created = false;

    struct statbuf stat;
    lock(&inodes_lock, 0); // TODO: don't do this

    // Stat before open so permission checks happen before backends can truncate
    // or otherwise mutate an existing file as a side effect of open.
    err = mount->fs->stat(mount, path, &stat);
    if (err < 0) {
        if ((flags & O_CREAT_) && err == _ENOENT) {
            // "newname/" names a directory; open() cannot create one (EISDIR).
            if (trailing_slash) {
                unlock(&inodes_lock);
                mount_release(mount);
                return ERR_PTR(_EISDIR);
            }
            // The target does not exist, so O_CREAT is really about to create
            // a new directory entry: this is the point (unlike the "target
            // already exists" branch below) where Linux requires write+exec
            // permission on the parent directory. path is mount-relative and
            // normalized (find_mount_and_trim_path only trims the mount
            // prefix), so strip the final component to get the parent.
            {
                char parent[MAX_PATH];
                size_t last_slash = 0;
                char *slash = strrchr(path, '/');
                if (slash != NULL) {
                    last_slash = (size_t)(slash - path);
                }
                if (last_slash == 0)
                    // Root directory: this codebase's mount-relative
                    // representation of the root is "" (see
                    // generic_getpath, fix_path), not "/".
                    parent[0] = '\0';
                else {
                    memcpy(parent, path, last_slash);
                    parent[last_slash] = '\0';
                }
                struct statbuf parent_stat;
                int perr = mount->fs->stat(mount, parent, &parent_stat);
                if (perr >= 0)
                    perr = access_check(&parent_stat, AC_W | AC_X);
                if (perr < 0) {
                    unlock(&inodes_lock);
                    mount_release(mount);
                    return ERR_PTR(perr);
                }
            }
            created = true;
        } else {
            unlock(&inodes_lock);
            mount_release(mount);
            return ERR_PTR(err);
        }
    } else {
        // O_NOFOLLOW: a final symlink we deliberately did not resolve is an
        // error -- unless O_PATH is also set, in which case Linux opens the
        // symlink ITSELF (fstat sees S_IFLNK, readlinkat(fd, "") returns the
        // target, read/write give EBADF). systemd's chase() opens every path
        // component this way, so without this any chase ending on a symlink
        // failed with ELOOP.
        if ((flags & O_NOFOLLOW_) && S_ISLNK(stat.mode)) {
            unlock(&inodes_lock);
            if (flags & O_PATH_) {
                if (flags & O_DIRECTORY_) {
                    mount_release(mount);
                    return ERR_PTR(_ENOTDIR);
                }
                struct fd *lfd = opath_link_fd_create(mount, path);
                if (lfd == NULL) {
                    mount_release(mount);
                    return ERR_PTR(_ENOMEM);
                }
                // opath_link_fd_create took over the mount reference.
                lfd->flags = flags;
                return lfd;
            }
            mount_release(mount);
            return ERR_PTR(_ELOOP);
        }
        // O_PATH ignores the access mode: Linux performs no read/write
        // permission check for O_PATH opens (the fd can't do I/O anyway).
        if (!(flags & O_PATH_)) {
            int accmode;
            if (flags & O_RDWR_) accmode = AC_R | AC_W;
            else if (flags & O_WRONLY_) accmode = AC_W;
            else accmode = AC_R;
            err = access_check(&stat, accmode);
            if (err < 0) {
                unlock(&inodes_lock);
                mount_release(mount);
                return ERR_PTR(err);
            }
        }
        // O_PATH on a socket or FIFO must not open the object itself: Linux
        // creates a pure path handle (a real open of a socket is ENXIO, and
        // a FIFO open would block on a peer -- both wrong under O_PATH).
        // Reuse the opath pseudo-fd built for symlinks, with the real file
        // type. Concretely: systemd's recursive chown of an existing
        // RuntimeDirectory= (exec-invoke's "special execution directory")
        // O_PATH-opens every entry; on systemd-resolved's
        // /run/systemd/resolve -- which already holds its two varlink
        // LISTENING SOCKETS from the socket units -- the ENXIO from the
        // fallthrough real open aborted the spawn at step RUNTIME_DIRECTORY
        // ("Failed to set up special execution directory in /run: No such
        // device or address") and resolved could never start. Directories
        // and regular files keep the real-open path: their O_PATH fds are
        // routinely used as dirfds, which the pseudo-fd doesn't support.
        if ((flags & O_PATH_) && (S_ISSOCK(stat.mode) || S_ISFIFO(stat.mode))) {
            unlock(&inodes_lock);
            if (flags & O_DIRECTORY_) {
                mount_release(mount);
                return ERR_PTR(_ENOTDIR);
            }
            struct fd *pfd = opath_link_fd_create(mount, path);
            if (pfd == NULL) {
                mount_release(mount);
                return ERR_PTR(_ENOMEM);
            }
            // opath_link_fd_create took over the mount reference.
            pfd->type = stat.mode & S_IFMT;
            pfd->flags = flags;
            return pfd;
        }
    }

    // mount->fs->open can issue a host open() that blocks indefinitely -- most
    // notably opening a FIFO (e.g. syslog-ng's /dev/xconsole) with no peer,
    // which blocks until the other end is opened. inodes_lock is a single global
    // lock (see the "don't do this" above), so holding it across such an open
    // wedges every other open() in the emulator -- the whole app appears to
    // freeze. Drop the lock around the open for files that can block, and
    // re-acquire it for the inode bookkeeping below.
    bool open_may_block = !created && S_ISFIFO(stat.mode) && !(flags & O_NONBLOCK_);
    if (open_may_block)
        unlock(&inodes_lock);
    // Strip O_PATH before handing flags to the backend: its bit value
    // (0x200000) is Darwin's O_SYMLINK, so realfs would otherwise pass a
    // meaningfully different flag to the host open(). A non-symlink O_PATH
    // open behaves like an ordinary open downstream (a deliberate
    // simplification: read() on it succeeds where Linux gives EBADF).
    struct fd *fd = mount->fs->open(mount, path, flags & ~O_PATH_, mode);
    if (open_may_block)
        lock(&inodes_lock, 0);
    if (IS_ERR(fd)) {
        unlock(&inodes_lock);
        // if an error happens after this point, fd_close will release the
        // mount, but right now we need to do it manually
        mount_release(mount);
        return fd;
    }
    fd->mount = mount;

    err = fd->mount->fs->fstat(fd, &stat);
    if (err < 0) {
        unlock(&inodes_lock);
        goto error;
    }
    fd->inode = inode_get_unlocked(mount, stat.inode);
    unlock(&inodes_lock);
    fd->type = stat.mode & S_IFMT;
    fd->flags = flags;

    // path_normalize should have already followed every symlink component
    // (including the final one, unless O_NOFOLLOW), so fd->type should never
    // land here as S_IFLNK. It did on-device under heavy concurrent load
    // (700+ threads, many processes opening shared libraries): fakefs's
    // per-syscall metadata reads (fakefs_readlink at path_normalize time vs.
    // fakefs_fstat here, both against the same SQLite ish_stat row) are not
    // atomic with each other, so a concurrent writer can flip a path's
    // recorded type between the two reads. That is a narrow, rare
    // inconsistency in fakefs locking, not a corrupted filesystem -- but this
    // used to be an assert(), which aborted the whole app on every occurrence.
    // Fail just this open() instead, like Linux does when open() loses a
    // symlink race (ELOOP), and let the caller (sshd's dlopen, in the crash
    // that motivated this) retry or report an ordinary error.
    if (S_ISLNK(fd->type)) {
        err = _ELOOP;
        goto error;
    }
    if (S_ISBLK(fd->type) || S_ISCHR(fd->type)) {
        int type;
        if (S_ISBLK(fd->type))
            type = DEV_BLOCK;
        else
            type = DEV_CHAR;
        err = dev_open(dev_major((dev_t_)stat.rdev), dev_minor((dev_t_)stat.rdev), type, fd);
        if (err < 0)
            goto error;
    }
    err = _ENXIO;
    if (S_ISSOCK(fd->type))
        goto error;
    err = _EISDIR;
    if (S_ISDIR(fd->type) && flags & (O_RDWR_ | O_WRONLY_))
        goto error;
    err = _ENOTDIR;
    if (!S_ISDIR(fd->type) && flags & O_DIRECTORY_)
        goto error;
    inotify_notify_open(guest_path);
    if (created)
        inotify_notify_create(guest_path, S_ISDIR(fd->type));
    return fd;

error:
    fd_close(fd);
    return ERR_PTR(err);
}

struct fd *generic_openat(struct fd *at, const char *path_raw, int flags, int mode) {
    return generic_openat_norm(at, path_raw, flags, mode, 0);
}

struct fd *generic_open(const char *path, int flags, int mode) {
    return generic_openat(AT_PWD, path, flags, mode);
}

// Open a stored, already-normalized guest path (one produced by
// path_normalize/generic_getpath earlier, possibly by another task). Such a
// path already contains any chroot prefix, so it must anchor at the REAL
// root -- re-resolving it through the caller's chroot double-applied the
// prefix and broke procfd reopen and fsmount inside chroots.
struct fd *generic_open_realroot(const char *path, int flags, int mode) {
    return generic_openat_norm(AT_PWD, path, flags, mode, N_REALROOT);
}

int generic_getpath(struct fd *fd, char *buf) {
    if (fd_is_opath_link(fd)) {
        struct mount *mount = fd->opath_link.mount;
        size_t point_len = mount->point_len;
        size_t path_len = strlen(fd->opath_link.path);
        if (point_len + path_len >= MAX_PATH)
            return _ENAMETOOLONG;
        memcpy(buf, mount->point, point_len);
        memcpy(buf + point_len, fd->opath_link.path, path_len + 1);
        if (buf[0] == '\0')
            memcpy(buf, "/", 2);
        return 0;
    }
    if(fd->ops != NULL) {
        int err = fd->mount->fs->getpath(fd, buf);
        if (err < 0)
            return err;
        size_t point_len = fd->mount->point_len;
        size_t buf_len = strlen(buf);
        if (buf_len + point_len >= MAX_PATH)
            return _ENAMETOOLONG;
        memmove(buf + point_len, buf, buf_len + 1);
        memcpy(buf, fd->mount->point, point_len);
        if (buf[0] == '\0')
            memcpy(buf, "/", 2);
        return 0;
    } else {
        return -EBADF;
    }
}

int generic_accessat(struct fd *dirfd, const char *path_raw, int mode) {
    // access() follows the final symlink, so the procfs magic links need the
    // same bypass stat() gets -- see procns_statat.
    struct statbuf ns_stat;
    int ns_err;
    if (procns_statat(dirfd, path_raw, &ns_stat, &ns_err))
        return ns_err < 0 ? ns_err : access_check(&ns_stat, mode);

    char path[MAX_PATH];
    int err = path_normalize(dirfd, path_raw, path, N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;

    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    struct statbuf stat = {};
    err = mount->fs->stat(mount, path, &stat);
    mount_release(mount);
    if (err < 0)
        return err;
    return access_check(&stat, mode);
}

int generic_linkat(struct fd *src_at, const char *src_raw, struct fd *dst_at, const char *dst_raw) {
    char src[MAX_PATH];
    int err = path_normalize(src_at, src_raw, src, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    char dst[MAX_PATH];
    err = path_normalize(dst_at, dst_raw, dst, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(src);
    struct mount *dst_mount = find_mount_and_trim_path(dst);
    if (mount == NULL || dst_mount == NULL) {
        if (mount != NULL)
            mount_release(mount);
        if (dst_mount != NULL)
            mount_release(dst_mount);
        return _ENOENT;
    }
    // Serialize against generic_openat/generic_mkdirat/etc. on the same path:
    // see the inodes_lock comment in generic_openat for why fakefs needs this
    // (a mutating fs op is a real-host-op + SQLite-metadata-update pair that
    // isn't atomic against a concurrent one of these on its own).
    lock(&inodes_lock, 0); // TODO: don't do this
    if (mount != dst_mount)
        err = _EXDEV;
    else if (mount->fs->link == NULL)
        err = _EPERM;
    else
        err = mount->fs->link(mount, src, dst);
    unlock(&inodes_lock);
    mount_release(mount);
    mount_release(dst_mount);
    return err;
}

// Linux's check_sticky(): in a directory with the sticky bit set (S_ISVTX,
// as /tmp has), you may only remove or rename an entry if you own the entry,
// own the directory, or are privileged. Without this a world-writable /tmp is
// not safe -- any user can delete anyone else's files.
#define S_ISVTX_ 01000
static int sticky_check(struct mount *mount, const char *path, struct statbuf *entry_stat) {
    if (superuser())
        return 0;
    // Derive the parent from the already mount-trimmed path; the entry and its
    // parent are necessarily on the same mount.
    char parent[MAX_PATH];
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (slash == NULL)
        return 0;
    if (slash == parent)
        parent[1] = '\0'; // entry sits directly in the mount root
    else
        *slash = '\0';
    struct statbuf dir_stat;
    if (mount->fs->stat(mount, parent, &dir_stat) < 0)
        return 0;
    if (!(dir_stat.mode & S_ISVTX_))
        return 0;
    if (current->fsuid == entry_stat->uid)
        return 0;
    if (current->fsuid == dir_stat.uid)
        return 0;
    return _EPERM;
}

int generic_unlinkat(struct fd *at, const char *path_raw) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    char guest_path[MAX_PATH]; // pre-trim path for inotify; see generic_openat
    strcpy(guest_path, path);
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    // See the inodes_lock comment in generic_openat: this serializes the
    // stat-check + unlink pair against a concurrent open(O_CREAT)/mkdir/etc.
    // on the same path, so fakefs's real-op + metadata-update pair can't
    // interleave with another one and leave the metadata mismatched with
    // what's actually on the host filesystem.
    lock(&inodes_lock, 0); // TODO: don't do this
    // Linux reports EISDIR for unlink of a directory. Enforce it here so the
    // host's own errno (EPERM on Darwin/iOS hosts) does not leak to the guest.
    struct statbuf ust;
    bool have_ust = mount->fs->stat(mount, path, &ust) >= 0;
    if (have_ust && S_ISDIR(ust.mode)) {
        unlock(&inodes_lock);
        mount_release(mount);
        return _EISDIR;
    }
    if (have_ust) {
        err = sticky_check(mount, path, &ust);
        if (err < 0) {
            unlock(&inodes_lock);
            mount_release(mount);
            return err;
        }
    }
    err = _EPERM;
    if (mount->fs->unlink)
        err = mount->fs->unlink(mount, path);
    unlock(&inodes_lock);
    mount_release(mount);
    if (err >= 0)
        inotify_notify_delete(guest_path, false);
    return err;
}

int generic_renameat(struct fd *src_at, const char *src_raw, struct fd *dst_at, const char *dst_raw, int flags) {
    // RENAME_NOREPLACE is implemented; RENAME_EXCHANGE/WHITEOUT and any unknown
    // flag are rejected with EINVAL (Linux's response for unsupported flags).
    if (flags & ~RENAME_NOREPLACE_)
        return _EINVAL;
    char src[MAX_PATH];
    // Linux requires write+exec on both the source and destination parent
    // directories for rename (removing the entry from one, adding it to the
    // other), not just the destination.
    int err = path_normalize(src_at, src_raw, src, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    char dst[MAX_PATH];
    err = path_normalize(dst_at, dst_raw, dst, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    if (contains_mount_point(src))
        return _EBUSY;
    char guest_src[MAX_PATH], guest_dst[MAX_PATH]; // pre-trim paths for inotify
    strcpy(guest_src, src);
    strcpy(guest_dst, dst);
    struct mount *mount = find_mount_and_trim_path(src);
    struct mount *dst_mount = find_mount_and_trim_path(dst);
    if (mount == NULL || dst_mount == NULL) {
        if (mount != NULL)
            mount_release(mount);
        if (dst_mount != NULL)
            mount_release(dst_mount);
        return _ENOENT;
    }
    // See the inodes_lock comment in generic_openat: serialize the
    // stat-check(s) + rename pair against a concurrent open(O_CREAT)/mkdir/
    // unlink/etc. on either path.
    lock(&inodes_lock, 0); // TODO: don't do this
    bool is_dir = false;
    if (mount != dst_mount)
        err = _EXDEV;
    else if (mount->fs->rename == NULL)
        err = _EPERM;
    else {
        struct statbuf stat;
        if ((flags & RENAME_NOREPLACE_) && mount->fs->stat(mount, dst, &stat) >= 0) {
            err = _EEXIST;
        } else {
            err = 0;
            // Rename removes the source entry from its parent, and replaces
            // the destination if it exists, so sticky applies to both.
            if (mount->fs->stat(mount, src, &stat) >= 0) {
                is_dir = S_ISDIR(stat.mode);
                err = sticky_check(mount, src, &stat);
            }
            struct statbuf dst_stat;
            if (err >= 0 && mount->fs->stat(mount, dst, &dst_stat) >= 0)
                err = sticky_check(mount, dst, &dst_stat);
            if (err >= 0)
                err = mount->fs->rename(mount, src, dst);
        }
    }
    unlock(&inodes_lock);
    mount_release(mount);
    mount_release(dst_mount);
    if (err >= 0)
        inotify_notify_move(guest_src, guest_dst, is_dir);
    return err;
}

int generic_symlinkat(const char *target, struct fd *at, const char *link_raw) {
    char link[MAX_PATH];
    int err = path_normalize(at, link_raw, link, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    char guest_path[MAX_PATH]; // pre-trim path for inotify; see generic_openat
    strcpy(guest_path, link);
    struct mount *mount = find_mount_and_trim_path(link);
    if (mount == NULL)
        return _ENOENT;
    // See the inodes_lock comment in generic_openat: serializes the
    // real-symlink-create + metadata-write pair against a concurrent
    // open(O_CREAT)/mkdir/unlink/etc. on the same path.
    lock(&inodes_lock, 0); // TODO: don't do this
    err = _EPERM;
    if (mount->fs->symlink)
        err = mount->fs->symlink(mount, target, link);
    unlock(&inodes_lock);
    mount_release(mount);
    if (err >= 0)
        inotify_notify_create(guest_path, false);
    return err;
}

int generic_mknodat(struct fd *at, const char *path_raw, mode_t_ mode, dev_t_ dev) {
    if (S_ISDIR(mode) || S_ISLNK(mode))
        return _EINVAL;
    if (!superuser() && (S_ISBLK(mode) || S_ISCHR(mode)))
        return _EPERM;

    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    char guest_path[MAX_PATH]; // pre-trim path for inotify; see generic_openat
    strcpy(guest_path, path);
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    // See the inodes_lock comment in generic_openat: serializes the
    // real-mknod + metadata-write pair against a concurrent
    // open(O_CREAT)/mkdir/unlink/etc. on the same path.
    lock(&inodes_lock, 0); // TODO: don't do this
    err = _EPERM;
    if (mount->fs->mknod)
        err = mount->fs->mknod(mount, path, mode, dev);
    unlock(&inodes_lock);
    mount_release(mount);
    if (err >= 0)
        inotify_notify_create(guest_path, false);
    return err;
}

int generic_setattrat(struct fd *at, const char *path_raw, struct attr attr, bool follow_links) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, follow_links ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    char guest_path[MAX_PATH]; // pre-trim path for inotify; see generic_openat
    strcpy(guest_path, path);
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    struct statbuf stat = {};
    err = mount->fs->stat(mount, path, &stat);
    if (err >= 0)
        err = setattr_check(&stat, attr);
    if (err < 0) {
        mount_release(mount);
        return err;
    }
    err = _EPERM;
    if (mount->fs->setattr)
        err = mount->fs->setattr(mount, path, attr);
    mount_release(mount);
    if (err >= 0) {
        if (attr.type == attr_size)
            inotify_notify_modify(guest_path);
        else
            inotify_notify_attrib(guest_path);
    }
    return err;
}

int generic_utime(struct fd *at, const char *path_raw, struct timespec atime, struct timespec mtime, bool follow_links) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, follow_links ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    err = _EPERM;
    if (mount->fs->utime)
        err = mount->fs->utime(mount, path, atime, mtime, follow_links);
    mount_release(mount);
    return err;
}

ssize_t generic_readlinkat(struct fd *at, const char *path_raw, char *buf, size_t bufsize) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    err = _EINVAL;
    if (mount->fs->readlink)
        err = mount->fs->readlink(mount, path, buf, bufsize);
    mount_release(mount);
    return err;
}

int generic_mkdirat(struct fd *at, const char *path_raw, mode_t_ mode) {
    char path[MAX_PATH];
    // The final component is the name being created and is never followed, so
    // mkdir over an existing (even dangling) symlink reports EEXIST like Linux.
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    char guest_path[MAX_PATH]; // pre-trim path for inotify; see generic_openat
    strcpy(guest_path, path);
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    // See the inodes_lock comment in generic_openat: serializes the
    // exists-check + real-mkdir + metadata-write against a concurrent
    // open(O_CREAT)/unlink/mkdir/etc. on the same path.
    lock(&inodes_lock, 0); // TODO: don't do this
    struct statbuf stat;
    err = mount->fs->stat(mount, path, &stat);
    if (err == 0) {
        unlock(&inodes_lock);
        mount_release(mount);
        return _EEXIST;
    }
    if (err < 0 && err != _ENOENT) {
        unlock(&inodes_lock);
        mount_release(mount);
        return err;
    }
    err = _EPERM;
    if (mount->fs->mkdir)
        err = mount->fs->mkdir(mount, path, mode);
    unlock(&inodes_lock);
    mount_release(mount);
    if (err >= 0)
        inotify_notify_create(guest_path, true);
    return err;
}

int generic_rmdirat(struct fd *at, const char *path_raw) {
    char path[MAX_PATH];
    // rmdir does not follow a final symlink: rmdir("symlink-to-dir") is ENOTDIR.
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    if (contains_mount_point(path))
        return _EBUSY;
    char guest_path[MAX_PATH]; // pre-trim path for inotify; see generic_openat
    strcpy(guest_path, path);
    struct mount *mount = find_mount_and_trim_path(path);
    if (mount == NULL)
        return _ENOENT;
    // See the inodes_lock comment in generic_openat: serializes the
    // real-rmdir + metadata-update against a concurrent
    // open(O_CREAT)/mkdir/unlink/etc. on the same path.
    lock(&inodes_lock, 0); // TODO: don't do this
    struct statbuf dst_stat;
    if (mount->fs->stat(mount, path, &dst_stat) >= 0) {
        err = sticky_check(mount, path, &dst_stat);
        if (err < 0) {
            unlock(&inodes_lock);
            mount_release(mount);
            return err;
        }
    }
    err = _EPERM;
    if (mount->fs->rmdir)
        err = mount->fs->rmdir(mount, path);
    unlock(&inodes_lock);
    mount_release(mount);
    if (err >= 0)
        inotify_notify_delete(guest_path, true);
    return err;
}

int generic_seek(struct fd *fd, off_t_ off, int whence, size_t size) {
    off_t_ new_off = fd->offset;
    if (whence == LSEEK_SET) {
        fd->offset = off;
    } else if (whence == LSEEK_CUR) {
        if (__builtin_add_overflow(new_off, off, &new_off) || new_off < 0)
            return _EINVAL;
        fd->offset = new_off;
    } else if (whence == LSEEK_END) {
        new_off = size + off;
        if (new_off < 0)
            return _EINVAL;
        fd->offset = new_off;
    } else {
        return _EINVAL;
    }
    return 0;
}
