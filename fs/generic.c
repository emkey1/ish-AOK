#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "kernel/fs.h"
#include "kernel/calls.h"   // MS_READONLY_ and the other mount flags
#include "fs/fd.h"
#include "fs/inode.h"
#include "fs/path.h"
#include "fs/proc.h"
#include "fs/dev.h"
#include "kernel/inotify.h"
#include "kernel/task.h"
#include "kernel/errno.h"

// The caller holds a reference on the task, so never a plain lock: see
// task_lock_unless_exiting in kernel/task.c.
static struct fdtable *procfd_task_files_retain(struct task *task) {
    struct fdtable *files = NULL;
    if (!task_lock_unless_exiting(task))
        return NULL;
    if (!task->exiting && task->files != NULL)
        files = fdtable_retain(task->files);
    unlock(&task->general_lock);
    return files;
}

static struct fd *procfd_reopen_regular(struct fd *fd, int flags) {
    if (fd->mount == NULL || fd->mount->fs == &procfs || !S_ISREG(fd->type))
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
    return generic_reopen_by_path(fd, flags & ~(O_CLOEXEC_ | O_NOFOLLOW_ | O_CREAT_ | O_EXCL_));
}

// True when an fd opened with `have` flags can stand in for a description
// requested with `want` flags, access-mode-wise.
static bool procfd_accmode_ok(int have, int want) {
    have &= O_ACCMODE_;
    want &= O_ACCMODE_;
    return have == want || have == O_RDWR_;
}

// A procfs-relative path naming a magic link that holds a file: "/PID/fd/N"
// or "/PID/exe", each also under "/PID/task/TID" (where /proc/thread-self
// leads), which is the thread's own. *pid is the task to ask: TID when there
// is one.
static bool procfd_parse(const char *path, int *pid, int *fd_no, bool *exe) {
    int n = 0, tid = 0;
    if (sscanf(path, "/%d%n", pid, &n) != 1 || n == 0)
        return false;
    path += n;
    n = 0;
    if (sscanf(path, "/task/%d%n", &tid, &n) == 1 && n > 0) {
        *pid = tid;
        path += n;
    }
    n = 0;
    if (sscanf(path, "/fd/%d%n", fd_no, &n) == 1 && n > 0 && path[n] == '\0') {
        *exe = false;
        return true;
    }
    if (strcmp(path, "/exe") == 0) {
        *exe = true;
        return true;
    }
    return false;
}

// The executable a process is running (mm->exefile), retained; NULL if none.
static struct fd *procfd_task_exe_retain(struct task *task) {
    struct fd *exe = NULL;
    if (!task_lock_unless_exiting(task))
        return NULL;
    if (!task->exiting && task->mm != NULL && task->mm->exefile != NULL)
        exe = fd_retain(task->mm->exefile);
    unlock(&task->general_lock);
    return exe;
}

// Resolves path_raw down to a /proc/PID/fd/N entry, or to /proc/PID/exe,
// following symlinks by hand rather than through path_normalize's
// N_SYMLINK_FOLLOW, and returns a retained reference to the underlying struct
// fd if so -- for exe, the process's mm->exefile. False (nothing retained)
// when path_raw doesn't ultimately name such an entry. True with *err_out set
// (and nothing retained) when it does, but the caller may not look at that
// process's files. *is_exe, when non-NULL, says which of the two it was.
//
// exe is here for the same reason fd/N is: it is a magic link, and walking its
// text reaches the file only while that text still names it. It does not for
// an image started from a memfd ("/memfd:name (deleted)" names nothing) or an
// unlinked file (whose name another file may have taken since), and runc
// opens /proc/self/exe of exactly such an image to ask F_GET_SEALS
// (is_self_cloned). Linux's proc_exe_link is gated like fd/N:
// ptrace_may_access(PTRACE_MODE_READ_FSCREDS).
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
static bool procfd_resolve(struct fd *at, const char *path_raw, struct fd **fd_out, int *err_out,
                           bool *is_exe) {
    *fd_out = NULL;
    *err_out = 0;
    if (is_exe != NULL)
        *is_exe = false;
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
        int pid = 0, fd_no = 0;
        bool exe = false;
        if (mount->fs == &procfs && procfd_parse(normalized, &pid, &fd_no, &exe)) {
            mount_release(mount);
            struct task *task = pid_get_task_ref(pid);
            if (task == NULL)
                return false;
            // Another process's descriptors are not the caller's to open. A
            // reopen by path would at least face the file's own permissions,
            // but a pipe, a socket, an unlinked or anonymous file has no path
            // and was handed back as the very description its owner holds --
            // a root daemon's pipe, or the deleted temp file it keeps its
            // secrets in, to any user. Linux's proc_fd_access_allowed:
            // ptrace_may_access(PTRACE_MODE_READ_FSCREDS), EACCES otherwise.
            if (!task_ptrace_may_access(task, PTRACE_MODE_READ_ | PTRACE_MODE_FSCREDS_)) {
                task_ref_cnt_mod(task, -1);
                *err_out = _EACCES;
                return true;
            }
            if (exe) {
                struct fd *exefile = procfd_task_exe_retain(task);
                task_ref_cnt_mod(task, -1);
                if (exefile == NULL)
                    return false;
                if (is_exe != NULL)
                    *is_exe = true;
                *fd_out = exefile;
                return true;
            }
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

// O_PATH through a magic link: a handle on the file the link holds. Linux's
// walk jumps to that file (nd_jump_link); walking the link's text instead
// reached nothing for a memfd, and for an unlinked file whatever had taken its
// name since. By path while the path still names the file, otherwise through
// the filesystem, as for any other open of it -- but with no permission check,
// as for any O_PATH open (procfd_resolve's ptrace gate still stands).
//
// NULL leaves it to the walk, as before: a file in procfs, whose name always
// names it, and what has no name at all -- a pipe, a socket, an anonymous
// inode, which Linux opens too (docs/TODO.md).
static struct fd *procfd_open_path(struct fd *fd, int flags) {
    if (fd->mount == NULL || fd->mount->fs == &procfs || !(S_ISREG(fd->type) || S_ISDIR(fd->type)))
        return NULL;
    if ((flags & O_DIRECTORY_) && !S_ISDIR(fd->type))
        return ERR_PTR(_ENOTDIR);
    flags &= O_PATH_FLAGS_ & ~O_CLOEXEC_;
    struct fd *reopened = generic_reopen_by_path(fd, flags);
    if (reopened == NULL && S_ISREG(fd->type))
        reopened = generic_reopen_pathless(fd, flags, false);
    // Not by the walk: the name is not this file's any more, so the walk
    // would reach nothing, or somebody else's. A directory removed while
    // held -- which Linux still opens -- is the case left.
    if (reopened == NULL)
        return ERR_PTR(_ENOENT);
    // What F_GETFL reports: not the O_NOFOLLOW the reopen by path adds.
    if (!IS_ERR(reopened))
        reopened->flags = flags;
    return reopened;
}

static struct fd *procfd_openat(struct fd *at, const char *path_raw, int flags) {
    // O_NOFOLLOW must fail the open with ELOOP and O_PATH|O_NOFOLLOW must
    // open the magic symlink itself; both operate on the link, not the
    // target, so leave them to normal path resolution.
    if (flags & O_NOFOLLOW_)
        return NULL;
    struct fd *fd;
    int err;
    bool exe;
    if (!procfd_resolve(at, path_raw, &fd, &err, &exe))
        return NULL;
    if (err < 0)
        return ERR_PTR(err);
    if (flags & O_PATH_) {
        struct fd *handle = procfd_open_path(fd, flags);
        fd_close(fd);
        return handle;
    }

    // Linux procfd opens give regular files a fresh file position and the
    // CALLER's flags, which shell script loaders rely on when they execute
    // /proc/self/fd/N after the parent has already inspected the script FD.
    // Prefer a reopen for normal file-backed descriptors: by path while the
    // path still names the file, and otherwise -- a memfd, an unlinked file --
    // through the filesystem, which makes a description of its own for it.
    struct fd *reopened = procfd_reopen_regular(fd, flags);
    if (reopened == NULL && fd->mount != NULL && fd->mount->fs != &procfs)
        reopened = generic_reopen_pathless(fd, flags, true);
    // A directory, likewise a description of its own, by path. The one held
    // can be an O_PATH handle, and reopening one of those as a directory that
    // reads is what the open is for; handed back, getdents through it was
    // EBADF. Opened for writing it is EISDIR, as any directory is.
    if (reopened == NULL && S_ISDIR(fd->type) && fd->mount != NULL && fd->mount->fs != &procfs) {
        if (flags & (O_WRONLY_ | O_RDWR_))
            reopened = ERR_PTR(_EISDIR);
        else
            reopened = generic_reopen_by_path(fd, flags & ~(O_CLOEXEC_ | O_NOFOLLOW_ | O_CREAT_ | O_EXCL_));
    }
    if (reopened != NULL) {
        fd_close(fd);
        return reopened;
    }
    // A running image is never handed out: the description behind exe can be
    // the very one the process that exec'd it still holds (exec shares it
    // when nothing else can be had; kernel/exec.c open_exec_descriptor).
    if (exe) {
        fd_close(fd);
        return ERR_PTR(_ENOENT);
    }
    // What is left has no description of its own to give: not a regular file,
    // or one on a filesystem that cannot make one. Resetting the retained
    // descriptor keeps shell interpreters from starting mid-script after apk
    // has read the shebang. Never hand back a descriptor WEAKER than the
    // caller asked for, though -- a silently read-only "O_RDWR" fd fails much
    // later and much more confusingly than an up-front error (see
    // procfd_reopen_regular's machine-id war story).
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
    int err;
    if (!procfd_resolve(at, path_raw, &fd, &err, NULL))
        return false;
    if (err < 0) {
        *err_out = err;
        return true;
    }
    *err_out = generic_fstat(fd, stat);
    fd_close(fd);
    return true;
}

// See kernel/fs.h. The same resolution generic_statat_full makes, including
// the /proc/PID/fd/N case: getxattr("/proc/self/fd/3", ...) reaches the file
// descriptor 3 has open -- unlinked or not -- rather than chasing the link's
// descriptive text as a path.
int generic_xattr_lookup(struct fd *at, const char *path_raw, bool follow,
        struct fd **fd_out, struct mount **mount_out, char *path_out,
        int *mount_flags_out, char *guest_path_out, struct statbuf *stat) {
    *fd_out = NULL;
    *mount_out = NULL;
    *mount_flags_out = 0;
    guest_path_out[0] = '\0';
    if (follow) {
        struct fd *fd;
        int err;
        if (procfd_resolve(at, path_raw, &fd, &err, NULL)) {
            if (err < 0)
                return err;
            err = generic_fstat(fd, stat);
            if (err < 0) {
                fd_close(fd);
                return err;
            }
            *fd_out = fd;
            *mount_flags_out = fd->mount_flags;
            return 0;
        }
    }
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, follow ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    strcpy(guest_path_out, path);
    struct mount *mount = find_mount_and_trim_path_flags(path, mount_flags_out);
    if (mount == NULL)
        return _ENOENT;
    memset(stat, 0, sizeof(*stat));
    // Under inodes_lock, as generic_statat_full takes the stat: fakefs's is
    // two unlocked reads that a concurrent create could otherwise tear.
    if (!mount->fs->may_block)
        lock(&inodes_lock, 0);
    err = mount->fs->stat(mount, path, stat);
    if (!mount->fs->may_block)
        unlock(&inodes_lock);
    if (err < 0) {
        mount_release(mount);
        return err;
    }
    strcpy(path_out, path);
    *mount_out = mount;
    return 0;
}

// The mount flags in force for `path`, which are NOT always the returned
// mount's own: see find_mount_and_trim_path_flags.
struct mount *find_mount_and_trim_path(char *path) {
    return find_mount_and_trim_path_flags(path, NULL);
}

// `mount_flags`, when non-NULL, receives the flags governing this path.
//
// ro/nosuid/nodev/noexec are per-MOUNT in Linux, not per-superblock: two binds
// of the same directory can differ, which is the entire point of
// `mount -o remount,bind,ro`. A bind here resolves to its origin for storage,
// and the origin's flags are not the bind's -- reading them off the returned
// mount answered every permission question about the ORIGIN. A read-only bind
// was therefore writable, and a nosuid or nodev one was neither.
//
// The bind's restrictions are added to the origin's rather than replacing
// them: a bind can be more restrictive than what it aliases, never less. A
// bind of a read-only filesystem stays read-only.
struct mount *find_mount_and_trim_path_flags(char *path, int *mount_flags) {
    return find_mount_and_trim_path_seen(path, mount_flags, NULL, NULL, NULL);
}

// The bind's own identity has to be read here, before the redirect below
// replaces it with the origin's: the path is on the bind, so statx's
// STATX_MNT_ID and fdinfo's mnt_id name the bind, and the bind's point is a
// mount root however deep in the origin its source sits. Reporting the
// origin's made a bind point look like an ordinary directory of its parent's
// mount to systemd's path_is_mount_point() and to mountpoint(1), which compare
// a path's mount ID with its parent's and read STATX_ATTR_MOUNT_ROOT.
//
// `seen_bind` hands back the bind itself, still referenced, for a descriptor
// to hold: see fd->bind_mount.
struct mount *find_mount_and_trim_path_seen(char *path, int *mount_flags, int *seen_id,
                                            bool *seen_root, struct mount **seen_bind) {
    if (mount_flags != NULL)
        *mount_flags = 0;
    if (seen_bind != NULL)
        *seen_bind = NULL;
    struct mount *mount = mount_find(path);
    if (mount == NULL)
        return NULL;
    if (mount_flags != NULL)
        *mount_flags = mount->flags;
    char *dst = path;
    const char *src = path + mount->point_len;
    while (*src != '\0')
        *dst++ = *src++;
    *dst = '\0';
    if (seen_id != NULL)
        *seen_id = mount_id(mount);
    if (seen_root != NULL)
        *seen_root = path[0] == '\0';

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
        if (seen_bind != NULL)
            *seen_bind = mount; // mount_find's reference goes with it
        else
            mount_release(mount);
        if (mount_flags != NULL)
            *mount_flags |= origin->flags;
        return origin;
    }
    return mount;
}

// Linux fails every modifying operation on a read-only mount with EROFS. The
// flag was recorded at mount time and never consulted anywhere, so read-only
// was purely cosmetic: only /proc/mounts said "ro" while creates, writes,
// unlinks and renames all went through.
//
// Takes the flags rather than the mount because for a bind they differ; see
// find_mount_and_trim_path_flags.
static bool mount_flags_readonly(int mount_flags) {
    return (mount_flags & MS_READONLY_) != 0;
}


bool contains_mount_point(const char *path) {
    struct mount *mount;
    // Optimization: hoist strlen(path) outside the loop to avoid redundant O(N) recalculations
    int n = strlen(path);
    // mounts_lock, like every other walk of this list: it is mutated under
    // that lock, and rmdir/rename ask this question about a path while another
    // thread may be mounting or unmounting.
    bool found = false;
    lock(&mounts_lock, 0);
    list_for_each_entry(&mounts, mount, mounts) {
        if (strncmp(path, mount->point, n) == 0 &&
                (mount->point[n] == '\0' || mount->point[n] == '/')) {
            found = true;
            break;
        }
    }
    unlock(&mounts_lock);
    return found;
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
    .name = "opath_link",
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
// That is readlink(2), so a /proc/<pid> link answers as it does to a reader;
// see generic_readlinkat_shown.
ssize_t opath_link_readlink(struct fd *fd, char *buf, size_t bufsize) {
    struct mount *mount = fd->opath_link.mount;
    if (mount->fs == &procfs)
        return proc_readlink_shown(fd->opath_link.path, buf, bufsize);
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

// O_TMPFILE: create an unnamed file on the filesystem holding the named
// directory. AOK cannot do this, and says so.
//
// It used to arrive as O_DIRECTORY plus an unrecognised bit, so the directory
// was opened and the write mode then failed it with EISDIR -- an errno that
// tells the caller it passed a directory, which is exactly what it meant to
// do, and gives it nothing to act on.
//
// EOPNOTSUPP is the answer Linux gives when the filesystem has no ->tmpfile,
// and it is the state AOK is actually in. It matters that this is a REFUSAL
// rather than a partial implementation: the anonymous file itself would be
// easy (create a hidden name, open it, unlink it), but a tmpfile opened
// without O_EXCL can be given a name afterwards with
// linkat("/proc/self/fd/N", ..., AT_SYMLINK_FOLLOW), and that needs linking an
// inode that has none -- machinery AOK does not have. Callers commit to the
// whole contract the moment open succeeds: systemd's open_tmpfile_linkable()
// falls back to a named temporary file if the open fails, and calls
// link_tmpfile() if it does not. Succeeding at open and failing at linkat
// would break exactly the callers that handle the refusal correctly today.
static struct fd *generic_open_tmpfile(struct fd *at, const char *path_raw, int flags) {
    // Linux checks the access mode in the open flags before the filesystem is
    // consulted: an unnamed file you cannot write is of no use to anyone.
    if (!(flags & (O_WRONLY_ | O_RDWR_)))
        return ERR_PTR(_EINVAL);
    // ...and then that the path really is a directory, so the caller can tell
    // "you named the wrong thing" from "this filesystem cannot do it".
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_FOLLOW);
    if (err < 0)
        return ERR_PTR(err);
    struct statbuf stat;
    err = generic_statat(at, path_raw, &stat, 0);
    if (err < 0)
        return ERR_PTR(err);
    if (!S_ISDIR(stat.mode))
        return ERR_PTR(_ENOTDIR);
    return ERR_PTR(_EOPNOTSUPP);
}

// The open by path, from generic_openat_norm below. `*bind` receives the bind
// mount the path is on, referenced, or NULL; generic_openat_norm gives it to
// the descriptor, or drops it if the open failed. It is not handed over in
// here because every failure below would then have to drop it too.
static struct fd *generic_openat_path(struct fd *at, const char *path_raw, int flags, int mode,
                                      int extra_norm, struct mount **bind) {
    // TODO really, really, seriously reconsider what I'm doing with the strings
    char path[MAX_PATH];
    // O_NOFOLLOW: do not resolve a *final* symlink component (intermediate
    // components are still followed), so opening one fails with ELOOP below.
    int norm = ((flags & O_NOFOLLOW_) ? N_SYMLINK_NOFOLLOW : N_SYMLINK_FOLLOW) | extra_norm;
    // O_CREAT plus a name spelled with a trailing slash is EISDIR on Linux --
    // open() cannot create a directory -- and that answer does not depend on
    // the name existing, so N_SLASH_EISDIR decides it inside the resolution,
    // right after the parent walk. Doing it out here instead only covered the
    // name-is-missing case: `open("file/", O_CREAT|O_WRONLY)` came back
    // ENOTDIR from the resolution before this function got a say, and
    // `open("dir/", O_CREAT|O_RDONLY)` just opened the directory.
    // O_PATH is exempt: it ignores O_CREAT altogether, so neither this nor
    // the existing-directory rule below applies to it (measured:
    // open("dir/", O_CREAT|O_PATH) succeeds on Linux).
    if ((flags & (O_CREAT_ | O_PATH_)) == O_CREAT_)
        norm |= N_SLASH_EISDIR;
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
    strcpy(guest_path, path);
    int mflags;
    int seen_id;
    bool seen_root;
    struct mount *mount = find_mount_and_trim_path_seen(path, &mflags, &seen_id, &seen_root, bind);
    if (mount == NULL)
        return ERR_PTR(_ENOENT);
    // Refusing the write-mode open is what Linux does for a read-only mount,
    // and it is sufficient: an fd that cannot be opened for writing cannot be
    // written through. O_TRUNC counts as a modification even with O_RDONLY,
    // for the same reason it needs write permission.
    if (mount_flags_readonly(mflags) &&
            ((flags & (O_WRONLY_ | O_RDWR_ | O_CREAT_ | O_TRUNC_)) != 0)) {
        mount_release(mount);
        return ERR_PTR(_EROFS);
    }

    bool created = false;

    // A may_block filesystem (fusefs) waits on a userspace daemon inside its
    // ops, so this function's inodes_lock serialization is skipped for it: the
    // lock guards fakefs's real-op/metadata pairing, which such a filesystem
    // doesn't have, and holding it across a daemon wait deadlocks (see
    // kernel/fs.h). It is still taken briefly for the inode-table lookup at
    // the end.
    bool fs_blocks = mount->fs->may_block;

    struct statbuf stat;
    if (!fs_blocks)
        lock(&inodes_lock, 0); // TODO: don't do this

    // Stat before open so permission checks happen before backends can truncate
    // or otherwise mutate an existing file as a side effect of open.
    err = mount->fs->stat(mount, path, &stat);
    if (err < 0) {
        if ((flags & O_CREAT_) && err == _ENOENT) {
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
                    if (!fs_blocks)
                        unlock(&inodes_lock);
                    mount_release(mount);
                    return ERR_PTR(perr);
                }
            }
            created = true;
        } else {
            if (!fs_blocks)
                unlock(&inodes_lock);
            mount_release(mount);
            return ERR_PTR(err);
        }
    } else {
        // The target exists and O_EXCL says it must not: that is EEXIST, and
        // Linux reports it before may_open() looks at permissions at all
        // (do_last() bails on the excl check first). AOK left O_EXCL to the
        // host open below, which sits after the target's own access check --
        // so an O_CREAT|O_EXCL|O_WRONLY open of an existing file the caller
        // cannot write reported EACCES where every Linux says EEXIST.
        if ((flags & (O_CREAT_ | O_EXCL_)) == (O_CREAT_ | O_EXCL_)) {
            if (!fs_blocks)
                unlock(&inodes_lock);
            mount_release(mount);
            return ERR_PTR(_EEXIST);
        }
        // ...and if it is a DIRECTORY, O_CREAT is EISDIR: open() cannot
        // create one, so asking it to is a request that can never be granted.
        // Linux's do_open() says so in one line --
        // `if (open_flag & O_CREAT) { ... if (d_is_dir(nd->path.dentry)) return -EISDIR; }`
        // -- with no regard for the access mode, which is why AOK refused only
        // the write-mode form (from the S_ISDIR check after the open, below)
        // and let `open("dir", O_CREAT|O_RDONLY)` hand back an fd on the
        // directory. It is also why this sits HERE: EEXIST above answers
        // first, and everything below answers later, including the target's
        // own access check -- measured, `open(dir-with-no-read-permission,
        // O_CREAT|O_RDONLY)` is EISDIR where the same open without O_CREAT is
        // EACCES.
        //
        // O_PATH is exempt because it ignores O_CREAT entirely (Linux keeps
        // only O_CLOEXEC/O_DIRECTORY/O_NOFOLLOW with it): measured,
        // open("dir", O_CREAT|O_PATH) succeeds.
        //
        // Distinct from N_SLASH_EISDIR, which is about the SPELLING: a
        // trailing slash is EISDIR whatever the name holds and even if it
        // holds nothing, and is decided inside path_normalize before any of
        // this. The two overlap only on a directory named with a trailing
        // slash, where both say EISDIR.
        if ((flags & (O_CREAT_ | O_PATH_)) == O_CREAT_ && S_ISDIR(stat.mode)) {
            if (!fs_blocks)
                unlock(&inodes_lock);
            mount_release(mount);
            return ERR_PTR(_EISDIR);
        }
        // O_NOFOLLOW: a final symlink we deliberately did not resolve is an
        // error -- unless O_PATH is also set, in which case Linux opens the
        // symlink ITSELF (fstat sees S_IFLNK, readlinkat(fd, "") returns the
        // target, read/write give EBADF). systemd's chase() opens every path
        // component this way, so without this any chase ending on a symlink
        // failed with ELOOP.
        if ((flags & O_NOFOLLOW_) && S_ISLNK(stat.mode)) {
            if (!fs_blocks)
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
                lfd->mnt_id = seen_id;
                lfd->mnt_root = seen_root;
                return lfd;
            }
            mount_release(mount);
            return ERR_PTR(_ELOOP);
        }
        // O_PATH ignores the access mode: Linux performs no read/write
        // permission check for O_PATH opens (the fd can't do I/O anyway).
        // O_NOACCESS_CHECK_ is internal (open_dir, for chdir/chroot): the
        // caller has already applied the permission rule that governs it, and
        // it is not this one.
        if (!(flags & (O_PATH_ | O_NOACCESS_CHECK_))) {
            int accmode;
            if (flags & O_RDWR_) accmode = AC_R | AC_W;
            else if (flags & O_WRONLY_) accmode = AC_W;
            else accmode = AC_R;
            // O_TRUNC destroys the contents, so it needs write permission even
            // when the open itself is read-only. Without this, open(path,
            // O_RDONLY|O_TRUNC) emptied any file the caller could merely READ
            // -- a root-owned 0755 binary truncated to zero by an ordinary
            // user. truncate(2) on the same file already returns EACCES, so
            // this was the only way in. Guarded on S_ISREG because O_TRUNC is
            // meaningless on other types, which is Linux's rule too.
            if ((flags & O_TRUNC_) && S_ISREG(stat.mode))
                accmode |= AC_W;
            err = access_check(&stat, accmode);
            if (err < 0) {
                if (!fs_blocks)
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
            if (!fs_blocks)
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
            pfd->mnt_id = seen_id;
            pfd->mnt_root = seen_root;
            return pfd;
        }

        // MS_NODEV: on such a mount a device node is not a device, and Linux
        // refuses to open it at all (may_open_dev -> EACCES). AOK recorded the
        // flag and printed it in /proc/mounts but never consulted it, so
        // `mount -o nodev` on untrusted media -- and every container runtime
        // that relies on it -- got a mount that opened character and block
        // devices exactly as if the flag had never been passed. O_PATH is
        // exempt because it opens no device: it makes a path handle, and
        // Linux skips may_open for it entirely.
        if ((mflags & MS_NODEV_) && !(flags & O_PATH_) &&
                (S_ISCHR(stat.mode) || S_ISBLK(stat.mode))) {
            if (!fs_blocks)
                unlock(&inodes_lock);
            mount_release(mount);
            return ERR_PTR(_EACCES);
        }
    }

    // mount->fs->open can issue a host open() that blocks indefinitely -- most
    // notably opening a FIFO (e.g. syslog-ng's /dev/xconsole) with no peer,
    // which blocks until the other end is opened. inodes_lock is a single global
    // lock (see the "don't do this" above), so holding it across such an open
    // wedges every other open() in the emulator -- the whole app appears to
    // freeze. Drop the lock around the open for files that can block, and
    // re-acquire it for the inode bookkeeping below.
    bool open_may_block = !fs_blocks && !created && S_ISFIFO(stat.mode) && !(flags & O_NONBLOCK_);
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
        if (!fs_blocks)
            unlock(&inodes_lock);
        // if an error happens after this point, fd_close will release the
        // mount, but right now we need to do it manually
        mount_release(mount);
        return fd;
    }
    fd->mount = mount;
    fd->mount_flags = mflags;
    fd->mnt_id = seen_id;
    fd->mnt_root = seen_root;

    err = fd->mount->fs->fstat(fd, &stat);
    if (err < 0) {
        if (!fs_blocks)
            unlock(&inodes_lock);
        goto error;
    }
    if (fs_blocks)
        lock(&inodes_lock, 0);
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
    // Creation is reported before the open that caused it, as in Linux.
    if (created)
        inotify_notify_create(guest_path, S_ISDIR(fd->type));
    inotify_notify_open(guest_path);
    return fd;

error:
    fd_close(fd);
    return ERR_PTR(err);
}

struct fd *generic_openat_norm(struct fd *at, const char *path_raw, int flags, int mode, int extra_norm) {
    if (flags & O_RDWR_ && flags & O_WRONLY_)
        return ERR_PTR(_EINVAL);
    if (flags & O_TMPFILE_)
        return generic_open_tmpfile(at, path_raw, flags);

    struct fd *procfd = procfd_openat(at, path_raw, flags);
    if (procfd != NULL)
        return procfd;

    struct fd *nsfd = procns_openat(at, path_raw, flags);
    if (nsfd != NULL)
        return nsfd;

    // A descriptor opened through a bind holds the bind as well as the origin
    // behind it, so the bind is busy while it is open: see fd->bind_mount.
    struct mount *bind = NULL;
    struct fd *fd = generic_openat_path(at, path_raw, flags, mode, extra_norm, &bind);
    if (IS_ERR(fd)) {
        if (bind != NULL)
            mount_release(bind);
        return fd;
    }
    fd->bind_mount = bind;
    return fd;
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
//
// And it may enter a detached mount's staging point (N_DETACHED_OK): a path
// that starts there is the path of a descriptor in that mount, since nothing
// else can produce one. Without it a /proc/self/fd/N reopen of such a file
// fell back to handing out the caller's own description, and refused a mode
// that description did not have -- an O_RDWR reopen of an O_RDONLY
// descriptor was EACCES where Linux opens the file afresh.
struct fd *generic_open_realroot(const char *path, int flags, int mode) {
    return generic_openat_norm(AT_PWD, path, flags, mode, N_REALROOT | N_DETACHED_OK);
}

static bool same_file(const struct statbuf *a, const struct statbuf *b) {
    return a->dev == b->dev && a->inode == b->inode;
}

// Linux reaches a descriptor's file by its inode; AOK has
// paths, so it takes the descriptor's -- and a path is a name, which can
// outlive the file or be taken by another. The one an unlinked file's
// descriptor reports is the name it HAD, and when something has been created
// there since, the path is that: `cat /proc/self/fd/N` of a deleted file
// printed the new file's contents (and an O_TRUNC reopen emptied it), fexecve
// ran the new file, and linkat(fd, "", AT_EMPTY_PATH) gave it a second name.
static bool path_names_file(const char *path, const struct statbuf *held) {
    struct statbuf named;
    return path_is_normalized(path) && generic_lstat_realroot(path, &named) >= 0 &&
        same_file(&named, held);
}

// See kernel/fs.h.
bool generic_path_names_fd(const char *path, struct fd *fd) {
    struct statbuf held;
    return generic_fstat(fd, &held) >= 0 && path_names_file(path, &held);
}

// See kernel/fs.h. The name is looked at before it is opened, and what opened
// is checked after. Looked at first, with a stat, because an open can act on
// what it finds: O_TRUNC empties it, a FIFO blocks the opener until a writer
// comes, a terminal can become the caller's controlling tty. Checked after,
// because the name can change hands in between. O_NOFOLLOW because the path
// names the file the descriptor holds; a symlink there now is not it. The
// stored path is fully normalized (chroot prefix included), so it is opened
// against the real root, or a chrooted caller would re-prefix it.
struct fd *generic_reopen_by_path(struct fd *fd, int flags) {
    char path[MAX_PATH];
    struct statbuf held, got;
    if (generic_getpath(fd, path) < 0 || generic_fstat(fd, &held) < 0 ||
            !path_names_file(path, &held))
        return NULL;
    struct fd *reopened = generic_open_realroot(path, flags | O_NOFOLLOW_, 0);
    if (IS_ERR(reopened))
        return NULL;
    if (generic_fstat(reopened, &got) < 0 || !same_file(&got, &held)) {
        fd_close(reopened);
        return NULL;
    }
    return reopened;
}

// See kernel/fs.h. What generic_openat_path does for a file it has found by
// name, done for one it has in hand: the open's questions first -- the
// file's permissions for the access asked (write too for O_TRUNC, which
// empties it), a read-only mount -- then the filesystem's new description of
// it, given the mount, inode and flags every description has, then O_TRUNC.
//
// Linux gets this by opening the inode behind a /proc/<pid>/fd/N or exe magic
// link. Without it the only description a file with no path had was the one
// it was reached through: /proc/self/fd/N handed out the caller's own,
// rewound, so whoever opened it moved the caller's offset, and could not open
// it for any access the caller's lacked; /proc/self/exe of an image started
// from a memfd or an unlinked file did not open at all.
struct fd *generic_reopen_pathless(struct fd *fd, int flags, bool check_access) {
    if (fd->ops == NULL || fd->ops->reopen == NULL || fd->mount == NULL || !S_ISREG(fd->type))
        return NULL;
    flags &= ~(O_CREAT_ | O_EXCL_ | O_NOFOLLOW_ | O_CLOEXEC_);
    if (flags & O_DIRECTORY_)
        return ERR_PTR(_ENOTDIR);
    if (check_access) {
        struct statbuf stat;
        int err = generic_fstat(fd, &stat);
        if (err < 0)
            return ERR_PTR(err);
        bool writes = (flags & (O_WRONLY_ | O_RDWR_ | O_TRUNC_)) != 0;
        if (writes && mount_flags_readonly(fd->mount_flags))
            return ERR_PTR(_EROFS);
        int accmode = AC_R;
        if (flags & O_RDWR_)
            accmode = AC_R | AC_W;
        else if (flags & O_WRONLY_)
            accmode = AC_W;
        if (flags & O_TRUNC_)
            accmode |= AC_W;
        err = access_check(&stat, accmode);
        if (err < 0)
            return ERR_PTR(err);
    }

    struct fd *reopened = fd->ops->reopen(fd, flags);
    if (IS_ERR(reopened))
        return reopened;
    mount_retain(fd->mount);
    reopened->mount = fd->mount;
    reopened->mount_flags = fd->mount_flags;
    reopened->mnt_id = fd->mnt_id;
    reopened->mnt_root = fd->mnt_root;
    if (fd->bind_mount != NULL)
        mount_retain(fd->bind_mount);
    reopened->bind_mount = fd->bind_mount;
    if (fd->inode != NULL)
        inode_retain(fd->inode);
    reopened->inode = fd->inode;
    reopened->type = fd->type;
    reopened->flags = flags & ~O_TRUNC_;

    if ((flags & O_TRUNC_) && reopened->mount->fs->fsetattr != NULL) {
        int err = reopened->mount->fs->fsetattr(reopened, make_attr(size, 0));
        if (err < 0) {
            fd_close(reopened);
            return ERR_PTR(err);
        }
    }
    return reopened;
}

// A descriptor opened through a bind mount has the bind's ORIGIN as its mount
// (find_mount_and_trim_path_seen redirects to it for storage), so the path its
// filesystem reports is relative to the origin, and joining it to the
// origin's point named the bind's source: after `mount --bind /tmp/bp-src
// /tmp/bp-dst; cd /tmp/bp-dst`, getcwd and /proc/self/cwd said /tmp/bp-src
// where Linux says /tmp/bp-dst. fd->bind_mount is the bind, so the path is
// re-expressed through it. That matters beyond what gets printed: every
// relative lookup starts from this path (fs/path.c path_normalize), and
// starting from the source's put it on the source's mount -- a read-only bind
// was writable from a cwd inside it, a mount inside the bind was hidden from
// relative names, and a bind of an outside directory into a chroot left a cwd
// in it outside the jail, where `..` walked on out.
//
// A bind that has been lazily unmounted is still held by the descriptor but
// has left the mount table, and there the two uses part. Linux names the file
// from the detached bind's root -- "/f" for $B/dst/f after `umount -l $B/dst`
// -- which is what a reader is SHOWN. But no path reaches a detached mount
// here, and "/f" walked from the real root is somebody else's file: a lookup
// from a cwd there has to go through the bind's source, which still reaches
// the file. So GETPATH_LOOKUP falls back to the path on the origin, and only
// GETPATH_SHOWN, which is never walked, takes Linux's answer.
//
// An ordinary mount that umount -l detached while it was busy has no source
// to fall back to. It is parked at a staging point instead (kernel/fs.h), so
// its point is still the start of a path that reaches it and only it, and
// GETPATH_LOOKUP is that path. GETPATH_SHOWN is the path from the mount's own
// root, "/f" for $B/f after `cd $B; umount -l $B`, as Linux shows it, unless
// the caller's root is in the same detached mount (a chroot into it): then
// the path is under the root like any other, and the caller's rebase against
// the root makes it the ordinary answer.
enum getpath_how {
    GETPATH_BACKING, // the origin's path, ignoring any bind
    GETPATH_LOOKUP,  // through the bind while it is mounted
    GETPATH_SHOWN,   // through the bind, mounted or lazily unmounted
};

// Is the calling process's root inside the detached mount parked at the first
// `staging` bytes of `path`? Only asked for a path in one, so the extra lookup
// is paid only there.
static bool root_in_detached(const char *path, size_t staging) {
    if (current == NULL || current->fs == NULL)
        return false;
    lock(&current->fs->lock, 0);
    struct fd *root = current->fs->root != NULL ? fd_retain(current->fs->root) : NULL;
    unlock(&current->fs->lock);
    if (root == NULL)
        return false;
    char root_path[MAX_PATH];
    bool in = generic_getpath(root, root_path) >= 0 &&
        strncmp(root_path, path, staging) == 0 &&
        (root_path[staging] == '\0' || root_path[staging] == '/');
    fd_close(root);
    return in;
}

static int getpath_common(struct fd *fd, char *buf, enum getpath_how how, bool *unreachable) {
    struct mount *mount;
    if (fd_is_opath_link(fd)) {
        mount = fd->opath_link.mount;
        size_t path_len = strlen(fd->opath_link.path);
        if (mount->point_len + path_len >= MAX_PATH)
            return _ENAMETOOLONG;
        memcpy(buf, fd->opath_link.path, path_len + 1);
    } else if (fd->ops != NULL) {
        mount = fd->mount;
        int err = mount->fs->getpath(fd, buf);
        if (err < 0)
            return err;
    } else {
        return _EBADF;
    }
    // Also the origin's path once the file has been renamed out from under
    // the bind's source, where no path through the bind names it.
    bool on_bind = how != GETPATH_BACKING && fd->bind_mount != NULL;
    if (!on_bind || !mount_path_through_bind(fd->bind_mount, mount, buf,
                                             how == GETPATH_SHOWN ? unreachable : NULL)) {
        // Under mounts_lock: umount -l and MS_MOVE replace the point.
        lock(&mounts_lock, 0);
        size_t point_len = mount->point_len;
        size_t buf_len = strlen(buf);
        if (buf_len + point_len >= MAX_PATH) {
            unlock(&mounts_lock);
            return _ENAMETOOLONG;
        }
        memmove(buf + point_len, buf, buf_len + 1);
        memcpy(buf, mount->point, point_len);
        unlock(&mounts_lock);
    }
    if (buf[0] == '\0')
        memcpy(buf, "/", 2);
    if (how == GETPATH_SHOWN && !*unreachable) {
        size_t len = strlen(buf);
        size_t staging = mount_staging_point_len(buf, len);
        if (staging != 0 && !root_in_detached(buf, staging)) {
            memmove(buf, buf + staging, len - staging + 1);
            if (buf[0] == '\0')
                memcpy(buf, "/", 2);
            *unreachable = true;
        }
    }
    return 0;
}

int generic_getpath(struct fd *fd, char *buf) {
    return getpath_common(fd, buf, GETPATH_LOOKUP, NULL);
}

int generic_getpath_shown(struct fd *fd, char *buf, bool *unreachable) {
    *unreachable = false;
    return getpath_common(fd, buf, GETPATH_SHOWN, unreachable);
}

int generic_getpath_backing(struct fd *fd, char *buf) {
    return getpath_common(fd, buf, GETPATH_BACKING, NULL);
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

int generic_linkat(struct fd *src_at, const char *src_raw, struct fd *dst_at, const char *dst_raw, int src_norm) {
    char src[MAX_PATH];
    int err = path_normalize(src_at, src_raw, src, src_norm);
    if (err < 0)
        return err;
    // Only the destination: link("dir/.", new) is a different error entirely.
    // After the SOURCE lookup (do_linkat() resolves it before it calls
    // filename_create at all) and after the destination's own parent walk,
    // never before either -- see path_parent_walk().
    if (path_final_dot(dst_raw) != 0) {
        int walk = path_parent_walk(dst_at, dst_raw);
        return walk < 0 ? walk : _EEXIST;
    }
    char dst[MAX_PATH];
    err = path_normalize(dst_at, dst_raw, dst, N_SYMLINK_NOFOLLOW | N_PARENT_ONLY | N_PARENT_DIR_WRITE | N_CREATE_EEXIST_FIRST | N_SLASH_NOT_A_DIR);
    if (err < 0)
        return err;
    // Pre-trim copies for inotify; see generic_openat.
    char guest_src[MAX_PATH], guest_dst[MAX_PATH];
    strcpy(guest_src, src);
    strcpy(guest_dst, dst);
    int mflags;
    struct mount *mount = find_mount_and_trim_path_flags(src, &mflags);
    int dst_mflags;
    struct mount *dst_mount = find_mount_and_trim_path_flags(dst, &dst_mflags);
    if (mount == NULL || dst_mount == NULL) {
        if (mount != NULL)
            mount_release(mount);
        if (dst_mount != NULL)
            mount_release(dst_mount);
        return _ENOENT;
    }
    if (mount_flags_readonly(mflags) || mount_flags_readonly(dst_mflags)) {
        mount_release(mount);
        mount_release(dst_mount);
        return _EROFS;
    }
    // Serialize against generic_openat/generic_mkdirat/etc. on the same path:
    // see the inodes_lock comment in generic_openat for why fakefs needs this
    // (a mutating fs op is a real-host-op + SQLite-metadata-update pair that
    // isn't atomic against a concurrent one of these on its own).
    bool fs_blocks = mount->fs->may_block; // see generic_openat
    if (!fs_blocks)
        lock(&inodes_lock, 0); // TODO: don't do this
    if (mount != dst_mount)
        err = _EXDEV;
    else if (mount->fs->link == NULL)
        err = _EPERM;
    else
        err = mount->fs->link(mount, src, dst);
    if (!fs_blocks)
        unlock(&inodes_lock);
    mount_release(mount);
    mount_release(dst_mount);
    if (err >= 0) {
        // Linux reports a new link two ways: IN_ATTRIB on the inode whose link
        // count changed, and IN_CREATE in the directory that gained the name.
        // link() emitted neither, so a file grew a second name with no event.
        inotify_notify_attrib(guest_src);
        inotify_notify_create(guest_dst, false);
    }
    return err;
}

// Linux's check_sticky(): in a directory with the sticky bit set (S_ISVTX,
// as /tmp has), you may only remove or rename an entry if you own the entry,
// own the directory, or are privileged. Without this a world-writable /tmp is
// not safe -- any user can delete anyone else's files.
static int sticky_check(struct mount *mount, const char *path, struct statbuf *entry_stat) {
    if (superuser())
        return 0;
    // Derive the parent from the already mount-trimmed path; the entry and its
    // parent are necessarily on the same mount.
    char parent[MAX_PATH];
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    // The mount root is spelled "" -- what find_mount_and_trim_path leaves for
    // a path that IS the mount point -- not "/". Asking for "/" made the stat
    // below return ENOENT, so sticky_check bailed and allowed the unlink: any
    // user could delete another's files sitting directly in the root of a
    // sticky mount, while entries one level deeper were correctly refused.
    // Both spellings of "no directory part" mean the mount root, because a
    // trimmed path may or may not keep its leading slash depending on the
    // mount's point_len.
    if (slash == NULL || slash == parent)
        parent[0] = '\0';
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
    // Linux: unlink(".") is EISDIR, not a permission failure. Same ordering
    // point as the create family -- after the parent walk (path_parent_walk),
    // before the parent's permissions.
    if (path_final_dot(path_raw) != 0) {
        int walk = path_parent_walk(at, path_raw);
        return walk < 0 ? walk : _EISDIR;
    }
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path,
            N_SYMLINK_NOFOLLOW | N_PARENT_ONLY | N_PARENT_DIR_WRITE |
            N_REMOVE_ENOENT_FIRST | N_SLASH_UNLINK);
    if (err < 0)
        return err;
    char guest_path[MAX_PATH]; // pre-trim path for inotify; see generic_openat
    strcpy(guest_path, path);
    int mflags;
    struct mount *mount = find_mount_and_trim_path_flags(path, &mflags);
    if (mount == NULL)
        return _ENOENT;
    if (mount_flags_readonly(mflags)) {
        mount_release(mount);
        return _EROFS;
    }
    // See the inodes_lock comment in generic_openat: this serializes the
    // stat-check + unlink pair against a concurrent open(O_CREAT)/mkdir/etc.
    // on the same path, so fakefs's real-op + metadata-update pair can't
    // interleave with another one and leave the metadata mismatched with
    // what's actually on the host filesystem.
    bool fs_blocks = mount->fs->may_block; // see generic_openat
    if (!fs_blocks)
        lock(&inodes_lock, 0); // TODO: don't do this
    // Linux reports EISDIR for unlink of a directory. Enforce it here so the
    // host's own errno (EPERM on Darwin/iOS hosts) does not leak to the guest.
    struct statbuf ust;
    bool have_ust = mount->fs->stat(mount, path, &ust) >= 0;
    if (have_ust && S_ISDIR(ust.mode)) {
        if (!fs_blocks)
            unlock(&inodes_lock);
        mount_release(mount);
        return _EISDIR;
    }
    if (have_ust) {
        err = sticky_check(mount, path, &ust);
        if (err < 0) {
            if (!fs_blocks)
                unlock(&inodes_lock);
            mount_release(mount);
            return err;
        }
    }
    err = _EPERM;
    if (mount->fs->unlink)
        err = mount->fs->unlink(mount, path);
    if (!fs_blocks)
        unlock(&inodes_lock);
    mount_release(mount);
    if (err >= 0)
        inotify_notify_delete(guest_path, false);
    return err;
}

int generic_renameat(struct fd *src_at, const char *src_raw, struct fd *dst_at, const char *dst_raw, int flags) {
    // Linux answers EBUSY when either operand ends in "." or "..", rather than
    // the create family's EEXIST. Same ordering point: do_renameat2() runs
    // filename_parentat() on BOTH operands before it looks at either one's
    // last_type, so a dotty operand whose parent cannot be walked reports the
    // walk's error. Source first, then destination, which is Linux's order.
    if (path_final_dot(src_raw) != 0 || path_final_dot(dst_raw) != 0) {
        // BOTH parents, even the operand that is not the dotty one: measured,
        // rename("dir/.", "missing/x") is ENOENT and not EBUSY.
        int walk = path_parent_walk(src_at, src_raw);
        if (walk >= 0)
            walk = path_parent_walk(dst_at, dst_raw);
        return walk < 0 ? walk : _EBUSY;
    }
    // RENAME_NOREPLACE is implemented; RENAME_EXCHANGE/WHITEOUT and any unknown
    // flag are rejected with EINVAL (Linux's response for unsupported flags).
    if (flags & ~RENAME_NOREPLACE_)
        return _EINVAL;
    // A trailing slash on EITHER name asks for a directory, and do_renameat2()
    // spends both of them on the SOURCE's type:
    //
    //     /* unless the source is a directory trailing slashes give -ENOTDIR */
    //     if (!d_is_dir(old_dentry)) {
    //             error = -ENOTDIR;
    //             if (old_last.name[old_last.len])
    //                     goto exit5;
    //             if (!(flags & RENAME_EXCHANGE) && new_last.name[new_last.len])
    //                     goto exit5;
    //     }
    //
    // So rename("file/", x) and rename(file, "anything/") are both ENOTDIR --
    // even when the destination IS a directory, where the same call without
    // the slash is EISDIR -- while a directory source spends the slash on
    // nothing at all and rename("dir/", "gone/") simply succeeds. AOK honoured
    // neither slash: it renamed "file/" and it CREATED "gone/", so a guest
    // asking to move something onto a directory got a new plain file instead.
    //
    // The rule reads the type of a name the slash need not be on, so it cannot
    // live in either path_normalize() call. Here it can still be given Linux's
    // order: after both parent walks, after the source must exist, after
    // RENAME_NOREPLACE's EEXIST (measured: renameat2(file, "dir/", NOREPLACE)
    // is EEXIST, and only a destination that is NOT there reaches ENOTDIR) --
    // and before either parent's write permission, which vfs_rename() asks for
    // afterwards, so rename("unwritable/f/", x) is ENOTDIR and not EACCES.
    if (path_trailing_slash(src_raw) || path_trailing_slash(dst_raw)) {
        int walk = path_parent_walk(src_at, src_raw);
        if (walk >= 0)
            walk = path_parent_walk(dst_at, dst_raw);
        if (walk < 0)
            return walk;
        struct statbuf src_stat;
        int err = path_lookup_final(src_at, src_raw, &src_stat);
        if (err < 0)
            return err;     // a source that is not there is ENOENT, as always
        struct statbuf dst_stat;
        if ((flags & RENAME_NOREPLACE_) &&
                path_lookup_final(dst_at, dst_raw, &dst_stat) >= 0)
            return _EEXIST;
        if (!S_ISDIR(src_stat.mode))
            return _ENOTDIR;
    }
    char src[MAX_PATH];
    // Linux requires write+exec on both the source and destination parent
    // directories for rename (removing the entry from one, adding it to the
    // other), not just the destination.
    // ENOENT-first on the SOURCE only: a source that is not there is ENOENT
    // even from an unwritable parent, while a destination that is not there is
    // the ordinary case and leaves the permission error standing.
    int err = path_normalize(src_at, src_raw, src,
            N_SYMLINK_NOFOLLOW | N_PARENT_ONLY | N_PARENT_DIR_WRITE | N_REMOVE_ENOENT_FIRST);
    if (err < 0)
        return err;
    char dst[MAX_PATH];
    err = path_normalize(dst_at, dst_raw, dst, N_SYMLINK_NOFOLLOW | N_PARENT_ONLY | N_PARENT_DIR_WRITE);
    if (err < 0)
        return err;
    if (contains_mount_point(src))
        return _EBUSY;
    char guest_src[MAX_PATH], guest_dst[MAX_PATH]; // pre-trim paths for inotify
    strcpy(guest_src, src);
    strcpy(guest_dst, dst);
    int mflags;
    struct mount *mount = find_mount_and_trim_path_flags(src, &mflags);
    int dst_mflags;
    struct mount *dst_mount = find_mount_and_trim_path_flags(dst, &dst_mflags);
    if (mount == NULL || dst_mount == NULL) {
        if (mount != NULL)
            mount_release(mount);
        if (dst_mount != NULL)
            mount_release(dst_mount);
        return _ENOENT;
    }
    if (mount_flags_readonly(mflags) || mount_flags_readonly(dst_mflags)) {
        mount_release(mount);
        mount_release(dst_mount);
        return _EROFS;
    }
    // See the inodes_lock comment in generic_openat: serialize the
    // stat-check(s) + rename pair against a concurrent open(O_CREAT)/mkdir/
    // unlink/etc. on either path.
    bool fs_blocks = mount->fs->may_block; // see generic_openat
    if (!fs_blocks)
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
    if (!fs_blocks)
        unlock(&inodes_lock);
    mount_release(mount);
    mount_release(dst_mount);
    if (err >= 0)
        inotify_notify_move(guest_src, guest_dst, is_dir);
    return err;
}

int generic_symlinkat(const char *target, struct fd *at, const char *link_raw) {
    char link[MAX_PATH];
    // A final "." or ".." names no creatable entry; Linux reports this from
    // filename_create() before touching the parent's permissions. Doing it the
    // other way round gave a normal user EACCES where root got EEXIST -- see
    // path_final_dot(). It comes after the parent walk, though, which is what
    // path_parent_walk() is for.
    if (path_final_dot(link_raw) != 0) {
        int walk = path_parent_walk(at, link_raw);
        return walk < 0 ? walk : _EEXIST;
    }
    int err = path_normalize(at, link_raw, link, N_SYMLINK_NOFOLLOW | N_PARENT_ONLY | N_PARENT_DIR_WRITE | N_CREATE_EEXIST_FIRST | N_SLASH_NOT_A_DIR);
    if (err < 0)
        return err;
    char guest_path[MAX_PATH]; // pre-trim path for inotify; see generic_openat
    strcpy(guest_path, link);
    int mflags;
    struct mount *mount = find_mount_and_trim_path_flags(link, &mflags);
    if (mount == NULL)
        return _ENOENT;
    if (mount_flags_readonly(mflags)) {
        mount_release(mount);
        return _EROFS;
    }
    // See the inodes_lock comment in generic_openat: serializes the
    // real-symlink-create + metadata-write pair against a concurrent
    // open(O_CREAT)/mkdir/unlink/etc. on the same path.
    bool fs_blocks = mount->fs->may_block; // see generic_openat
    if (!fs_blocks)
        lock(&inodes_lock, 0); // TODO: don't do this
    err = _EPERM;
    if (mount->fs->symlink)
        err = mount->fs->symlink(mount, target, link);
    if (!fs_blocks)
        unlock(&inodes_lock);
    mount_release(mount);
    if (err >= 0)
        inotify_notify_create(guest_path, false);
    return err;
}

int generic_mknodat(struct fd *at, const char *path_raw, mode_t_ mode, dev_t_ dev) {
    if (S_ISDIR(mode) || S_ISLNK(mode))
        return _EINVAL;
    // After the parent walk, before anything else; see path_parent_walk().
    if (path_final_dot(path_raw) != 0) {
        int walk = path_parent_walk(at, path_raw);
        return walk < 0 ? walk : _EEXIST;
    }
    if (!superuser() && (S_ISBLK(mode) || S_ISCHR(mode)))
        return _EPERM;

    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_ONLY | N_PARENT_DIR_WRITE | N_CREATE_EEXIST_FIRST | N_SLASH_NOT_A_DIR);
    if (err < 0)
        return err;
    char guest_path[MAX_PATH]; // pre-trim path for inotify; see generic_openat
    strcpy(guest_path, path);
    int mflags;
    struct mount *mount = find_mount_and_trim_path_flags(path, &mflags);
    if (mount == NULL)
        return _ENOENT;
    if (mount_flags_readonly(mflags)) {
        mount_release(mount);
        return _EROFS;
    }
    // See the inodes_lock comment in generic_openat: serializes the
    // real-mknod + metadata-write pair against a concurrent
    // open(O_CREAT)/mkdir/unlink/etc. on the same path.
    bool fs_blocks = mount->fs->may_block; // see generic_openat
    if (!fs_blocks)
        lock(&inodes_lock, 0); // TODO: don't do this
    err = _EPERM;
    if (mount->fs->mknod)
        err = mount->fs->mknod(mount, path, mode, dev);
    if (!fs_blocks)
        unlock(&inodes_lock);
    mount_release(mount);
    if (err >= 0)
        inotify_notify_create(guest_path, false);
    return err;
}

static int generic_setattrat_checked(struct fd *at, const char *path_raw, struct attr attr,
        bool follow_links, bool check);

int generic_setattrat(struct fd *at, const char *path_raw, struct attr attr, bool follow_links) {
    return generic_setattrat_checked(at, path_raw, attr, follow_links, true);
}

// See kernel/fs.h: for a change the kernel makes on its own account.
int generic_setattrat_force(struct fd *at, const char *path_raw, struct attr attr, bool follow_links) {
    return generic_setattrat_checked(at, path_raw, attr, follow_links, false);
}

static int generic_setattrat_checked(struct fd *at, const char *path_raw, struct attr attr,
        bool follow_links, bool check) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, follow_links ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    char guest_path[MAX_PATH]; // pre-trim path for inotify; see generic_openat
    strcpy(guest_path, path);
    int mflags;
    struct mount *mount = find_mount_and_trim_path_flags(path, &mflags);
    if (mount == NULL)
        return _ENOENT;
    if (mount_flags_readonly(mflags)) {
        mount_release(mount);
        return _EROFS;
    }
    struct statbuf stat = {};
    err = mount->fs->stat(mount, path, &stat);
    if (err >= 0 && check)
        err = setattr_check(&stat, &attr);
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

// chown(path, -1, -1) -- "change neither the owner nor the group". Linux does
// not shortcut it: chown_common() is reached only after the full lookup, so
// every error the resolution can raise is still raised, and only then does it
// find there is no uid and no gid to set. Measured on Linux 6.12, unprivileged
// and as root, 64-bit and -m32 all identical:
//
//   lchown("gone", -1, -1)            ENOENT      chown("file/", -1, -1)  ENOTDIR
//   chown("unsearchable/f", -1, -1)   EACCES      chown("dir/", -1, -1)   0
//   chown("symlink-loop", -1, -1)     ELOOP       chown("dangling", -1, -1) ENOENT
//
// Two things it does NOT do, both measured rather than assumed. There is no
// ownership check -- an unprivileged chown(-1, -1) on a root-owned file
// succeeds, because setattr_prepare() only tests ATTR_UID/ATTR_GID and neither
// is set. And it raises no inotify event, even though it does bump ctime:
// fsnotify_change() maps ATTR_UID/GID/MODE to IN_ATTRIB and a lone ATTR_CTIME
// to nothing. So this is a lookup and nothing else.
//
// Both of those hold for the file this sees, which is one with no setuid bits.
// Stripping them is a mode change, and that brings back the ownership check
// AND the IN_ATTRIB -- but it is not this function's job: the caller decides
// the strip from a stat it takes first, and applies it through the ordinary
// generic_setattrat() afterwards. See chown_privs_to_drop() in kernel/fs.c.
//
// (AOK bumps ctime for no setattr at all -- a plain chown and a chmod both
// leave it alone -- so there is nothing to bump here, and the Linux ctime
// behaviour is a separate gap rather than one this skips.)
int generic_setattrat_nochange(struct fd *at, const char *path_raw, bool follow_links) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, follow_links ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    int mflags;
    struct mount *mount = find_mount_and_trim_path_flags(path, &mflags);
    if (mount == NULL)
        return _ENOENT;
    // Linux takes the write reference before it touches the inode, so a
    // read-only mount answers EROFS here too. This keeps the position AOK
    // already gives EROFS for every other setattr rather than inventing a new
    // one; see the note in 562a4eb5 about that ordering.
    if (mount_flags_readonly(mflags)) {
        mount_release(mount);
        return _EROFS;
    }
    // path_normalize() has resolved and vetted every component it walked, but
    // a NOFOLLOW caller's final component is only checked for existence here,
    // which is where lchown("gone", -1, -1) gets its ENOENT.
    struct statbuf stat = {};
    err = mount->fs->stat(mount, path, &stat);
    mount_release(mount);
    return err < 0 ? err : 0;
}

int generic_utime(struct fd *at, const char *path_raw, struct timespec atime, struct timespec mtime, bool follow_links) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, follow_links ? N_SYMLINK_FOLLOW : N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    int mflags;
    struct mount *mount = find_mount_and_trim_path_flags(path, &mflags);
    if (mount == NULL)
        return _ENOENT;
    if (mount_flags_readonly(mflags)) {
        mount_release(mount);
        return _EROFS;
    }
    err = _EPERM;
    if (mount->fs->utime)
        err = mount->fs->utime(mount, path, atime, mtime, follow_links);
    mount_release(mount);
    return err;
}

static ssize_t readlinkat_common(struct fd *at, const char *path_raw, char *buf, size_t bufsize,
                                 bool shown) {
    char path[MAX_PATH];
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW);
    if (err < 0)
        return err;
    int mflags;
    struct mount *mount = find_mount_and_trim_path_flags(path, &mflags);
    if (mount == NULL)
        return _ENOENT;
    err = _EINVAL;
    if (shown && mount->fs == &procfs)
        err = proc_readlink_shown(path, buf, bufsize);
    else if (mount->fs->readlink)
        err = mount->fs->readlink(mount, path, buf, bufsize);
    mount_release(mount);
    return err;
}

ssize_t generic_readlinkat(struct fd *at, const char *path_raw, char *buf, size_t bufsize) {
    return readlinkat_common(at, path_raw, buf, bufsize, false);
}

ssize_t generic_readlinkat_shown(struct fd *at, const char *path_raw, char *buf, size_t bufsize) {
    return readlinkat_common(at, path_raw, buf, bufsize, true);
}

int generic_mkdirat(struct fd *at, const char *path_raw, mode_t_ mode) {
    char path[MAX_PATH];
    // After the parent walk, before anything else; see path_parent_walk().
    if (path_final_dot(path_raw) != 0) {
        int walk = path_parent_walk(at, path_raw);
        return walk < 0 ? walk : _EEXIST;
    }
    // The final component is the name being created and is never followed, so
    // mkdir over an existing (even dangling) symlink reports EEXIST like Linux.
    int err = path_normalize(at, path_raw, path, N_SYMLINK_NOFOLLOW | N_PARENT_ONLY | N_PARENT_DIR_WRITE | N_CREATE_EEXIST_FIRST);
    if (err < 0)
        return err;
    char guest_path[MAX_PATH]; // pre-trim path for inotify; see generic_openat
    strcpy(guest_path, path);
    int mflags;
    struct mount *mount = find_mount_and_trim_path_flags(path, &mflags);
    if (mount == NULL)
        return _ENOENT;
    if (mount_flags_readonly(mflags)) {
        // EXISTENCE BEATS WRITABILITY, as on Linux: mkdir("/proc") is EEXIST
        // and not EROFS, even though procfs is read-only. mkdir -p depends on
        // it -- it walks the ancestors and treats "already there" as done, so
        // an EROFS where Linux says EEXIST stops it on a component it never
        // needed to create.
        //
        // That is what `mkdir -p /AOK/persist/pscal` hit: it failed on /AOK,
        // the read-only aokfs mount, while the directory it actually wanted
        // sits on the writable real-fs mount underneath -- so a plain
        // `mkdir /AOK/persist/pscal` worked and the -p form did not.
        struct statbuf existing;
        int exists = mount->fs->stat(mount, path, &existing);
        mount_release(mount);
        return exists == 0 ? _EEXIST : _EROFS;
    }
    // See the inodes_lock comment in generic_openat: serializes the
    // exists-check + real-mkdir + metadata-write against a concurrent
    // open(O_CREAT)/unlink/mkdir/etc. on the same path.
    bool fs_blocks = mount->fs->may_block; // see generic_openat
    if (!fs_blocks)
        lock(&inodes_lock, 0); // TODO: don't do this
    struct statbuf stat;
    err = mount->fs->stat(mount, path, &stat);
    if (err == 0) {
        if (!fs_blocks)
            unlock(&inodes_lock);
        mount_release(mount);
        return _EEXIST;
    }
    if (err < 0 && err != _ENOENT) {
        if (!fs_blocks)
            unlock(&inodes_lock);
        mount_release(mount);
        return err;
    }
    err = _EPERM;
    if (mount->fs->mkdir)
        err = mount->fs->mkdir(mount, path, mode);
    if (!fs_blocks)
        unlock(&inodes_lock);
    mount_release(mount);
    if (err >= 0)
        inotify_notify_create(guest_path, true);
    return err;
}

int generic_rmdirat(struct fd *at, const char *path_raw) {
    char path[MAX_PATH];
    // Linux: rmdir(".") is EINVAL and rmdir("..") is ENOTEMPTY. Both are
    // decided before the parent's permissions and after the parent walk, same
    // as the create family -- do_rmdir() reaches its last_type switch only
    // once filename_parentat() has returned. See path_parent_walk().
    int dot = path_final_dot(path_raw);
    if (dot != 0) {
        int walk = path_parent_walk(at, path_raw);
        if (walk < 0)
            return walk;
        return dot == 1 ? _EINVAL : _ENOTEMPTY;
    }
    // rmdir does not follow a final symlink: rmdir("symlink-to-dir") is ENOTDIR.
    int err = path_normalize(at, path_raw, path,
            N_SYMLINK_NOFOLLOW | N_PARENT_ONLY | N_PARENT_DIR_WRITE | N_REMOVE_ENOENT_FIRST);
    if (err < 0)
        return err;
    if (contains_mount_point(path))
        return _EBUSY;
    char guest_path[MAX_PATH]; // pre-trim path for inotify; see generic_openat
    strcpy(guest_path, path);
    int mflags;
    struct mount *mount = find_mount_and_trim_path_flags(path, &mflags);
    if (mount == NULL)
        return _ENOENT;
    if (mount_flags_readonly(mflags)) {
        mount_release(mount);
        return _EROFS;
    }
    // See the inodes_lock comment in generic_openat: serializes the
    // real-rmdir + metadata-update against a concurrent
    // open(O_CREAT)/mkdir/unlink/etc. on the same path.
    bool fs_blocks = mount->fs->may_block; // see generic_openat
    if (!fs_blocks)
        lock(&inodes_lock, 0); // TODO: don't do this
    struct statbuf dst_stat;
    if (mount->fs->stat(mount, path, &dst_stat) >= 0) {
        err = sticky_check(mount, path, &dst_stat);
        if (err < 0) {
            if (!fs_blocks)
                unlock(&inodes_lock);
            mount_release(mount);
            return err;
        }
    }
    err = _EPERM;
    if (mount->fs->rmdir)
        err = mount->fs->rmdir(mount, path);
    if (!fs_blocks)
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
