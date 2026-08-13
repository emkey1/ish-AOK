#include <string.h>
#include <sys/stat.h>
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/path.h"
#include "fs/real.h"
#include "fs/dev.h"

// Sized for the static set below plus the two the iOS app registers at
// startup (iosfs and iosfs_unsafe) with room to spare -- overflowing this
// asserts at boot rather than failing gracefully, so don't run it to the rim.
#define MAX_FILESYSTEMS 14
static const struct fs_ops *filesystems[MAX_FILESYSTEMS] = {
    &realfs,
    &procfs,
    &aokfs,
    &devptsfs,
    &tmpfs,
    &devtmpfs,
    &sysfs,
    &cgroupfs,
    &cgroup2fs,
    &fakefs,
};

static bool mount_trace_elogind(void) {
    return false;
}

void fs_register(const struct fs_ops *fs) {
    for (unsigned i = 0; i < MAX_FILESYSTEMS; i++) {
        if (filesystems[i] == NULL) {
            filesystems[i] = fs;
            return;
        }
    }
    assert(!"reached filesystem limit");
}

char * get_filesystems(void) {
    unsigned int i;
    size_t total_len = 0;

    // Pass 1: Calculate the exact length required
    for (i = 0; i < MAX_FILESYSTEMS; i++) {
        if (filesystems[i] != NULL) {
            total_len += strlen("nodev    ") + strlen(filesystems[i]->name) + 1; // +1 for newline
        }
    }

    // Pass 2: Allocate and populate the buffer
    char *fs_list = malloc(total_len + 1); // +1 for null terminator
    if (fs_list == NULL)
        return NULL;

    char *ptr = fs_list;
    for (i = 0; i < MAX_FILESYSTEMS; i++) {
        if (filesystems[i] != NULL) {
            size_t name_len = strlen(filesystems[i]->name);
            memcpy(ptr, "nodev    ", 9);
            ptr += 9;
            memcpy(ptr, filesystems[i]->name, name_len);
            ptr += name_len;
            *ptr++ = '\n';
        }
    }
    *ptr = '\0';

    return fs_list;
}

struct mount *mount_find(char *path) {
    assert(path_is_normalized(path));
    lock(&mounts_lock, 0);
    struct mount *mount = NULL;
    if (list_empty(&mounts)) {
        unlock(&mounts_lock);
        return NULL;
    }
    list_for_each_entry(&mounts, mount, mounts) {
        // Optimization: Use cached point_len instead of strlen(mount->point)
        size_t n = mount->point_len;
        if (strncmp(path, mount->point, n) == 0 && (path[n] == '/' || path[n] == '\0'))
            break;
    }
    if (&mount->mounts == &mounts) {
        unlock(&mounts_lock);
        return NULL;
    }
    mount->refcount++;
    unlock(&mounts_lock);
    return mount;
}

void mount_retain(struct mount *mount) {
    lock(&mounts_lock, 0);
    mount->refcount++;
    unlock(&mounts_lock);
}

void mount_release(struct mount *mount) {
    lock(&mounts_lock, 0);
    mount->refcount--;
    unlock(&mounts_lock);
}

// Mount ID as exposed in /proc/self/mountinfo and statx's stx_mnt_id:
// 1-based position in the mounts list. The two consumers must agree --
// systemd cross-checks statx STATX_MNT_ID against mountinfo.
int mount_id(struct mount *target) {
    lock(&mounts_lock, 0);
    int id = 1;
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (mount == target) {
            unlock(&mounts_lock);
            return id;
        }
        id++;
    }
    unlock(&mounts_lock);
    return 1;
}

// The device files on this mount report through stat(2)'s st_dev, which is
// also /proc/self/mountinfo's field 3 (major:minor). On Linux those two are
// the same number by construction, so they must be produced from one place
// here as well.
//
// There is no single field to read: a filesystem with no backing device
// (tmpfs, proc, devpts, sysfs, cgroup, and fakefs since a3ea924e) leaves
// stat.dev at 0 and gets mount->fake_dev stamped on, while realfs derives a
// real device from the host volume (fs/real.c copy_stat). Printing fake_dev
// unconditionally would therefore be wrong for exactly the realfs mounts. So
// ask the filesystem, by stat'ing the mount's own root: path "" is what
// find_mount_and_trim_path leaves behind for a path that is exactly the mount
// point, making this literally the call `stat /mountpoint` makes. Falling back
// to fake_dev when the filesystem leaves dev 0 is fs/stat.c's
// stat_stamp_fake_dev rule; the two must stay in step.
//
// Deliberately not under inodes_lock, which generic_statat holds across the
// same call to stop fakefs's SQLite metadata and host stat halves from being
// combined torn. Only stat.dev is read here, and that field comes wholly from
// one side -- fakefs pins it to 0, realfs takes it from the host stat -- so
// there is no torn combination to protect against, and the proc read path
// this runs on has not been shown to be free of inodes_lock (it is not
// recursive).
dev_t_ mount_dev(struct mount *mount) {
    // A bind carries no backing of its own (root_fd -1, data NULL), so its
    // fs's stat fails here for the same reason its statfs does; report the
    // origin's superblock, which is what bind->fake_dev already copies. See
    // mount_statfs (kernel/fs.c) for why this deref needs no lock.
    if (mount->bind_origin != NULL)
        mount = mount->bind_origin;
    struct statbuf stat = {};
    if (mount->fs->stat != NULL && mount->fs->stat(mount, "", &stat) >= 0 && stat.dev != 0)
        return stat.dev;
    return mount->fake_dev;
}

int do_mount(const struct fs_ops *fs, const char *source, const char *point, const char *info, int flags) {
    struct mount *new_mount = malloc(sizeof(struct mount));
    if (new_mount == NULL)
        return _ENOMEM;
    new_mount->point = strdup(point);
    new_mount->point_len = strlen(point);
    new_mount->source = strdup(source);
    new_mount->display_source = NULL;
    new_mount->info = strdup(info);
    new_mount->flags = flags;
    new_mount->fs = fs;
    new_mount->data = NULL;
    new_mount->refcount = 0;
    new_mount->bind_origin = NULL;
    new_mount->bind_prefix = NULL;
    // Unique anonymous device per mount (Linux 0:xx); minors below
    // FAKE_DEV_MINOR_DYNAMIC are reserved for the static pseudo-mounts
    // (adhoc pipes/sockets, memfd).
    static _Atomic unsigned next_fake_dev_minor = FAKE_DEV_MINOR_DYNAMIC;
    new_mount->fake_dev = dev_make(0, next_fake_dev_minor++);
    if (fs->mount) {
        int err = fs->mount(new_mount);
        if (err < 0) {
            free((void *) new_mount->point);
            free((void *) new_mount->source);
            free(new_mount);
            return err;
        }
    }

    // the list must stay in descending order of mount point length
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        // Optimization: Use cached point_len to avoid O(N) calculations in list traversal
        if (mount->point_len <= new_mount->point_len)
            break;
    }
    list_add_before(&mount->mounts, &new_mount->mounts);
    return 0;
}

// See kernel/fs.h: renames a mount for display only. `display_source` NULL or
// empty restores the real source.
int mount_set_display_source(const char *point, const char *display_source) {
    char *name = NULL;
    if (display_source != NULL && display_source[0] != '\0') {
        name = strdup(display_source);
        if (name == NULL)
            return _ENOMEM;
    }

    lock(&mounts_lock, 0);
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        // The root's point is "" internally, so accept its guest spelling too
        // (same accommodation as the MS_REMOUNT lookup below).
        bool is_root = strcmp(point, "/") == 0 && mount->point[0] == '\0';
        if (strcmp(point, mount->point) == 0 || is_root) {
            free((void *) mount->display_source);
            mount->display_source = name;
            unlock(&mounts_lock);
            return 0;
        }
    }
    unlock(&mounts_lock);
    free(name);
    return _ENOENT;
}

int mount_remove(struct mount *mount) {
    if (mount->refcount != 0)
        return _EBUSY;

    if (mount->bind_origin != NULL) {
        // A bind owns one reference on its origin. We hold mounts_lock here
        // (do_umount / exit unmount), so drop it directly rather than via
        // mount_release, which would re-acquire the same non-recursive lock.
        mount->bind_origin->refcount--;
        free((void *) mount->bind_prefix);
    } else if (mount->fs->umount) {
        mount->fs->umount(mount);
    }
    list_remove(&mount->mounts);
    free((void *) mount->info);
    free((void *) mount->source);
    free((void *) mount->display_source);
    free((void *) mount->point);
    free(mount);
    return 0;
}

int do_umount(const char *point) {
    struct mount *mount;
    bool found = false;
    list_for_each_entry(&mounts, mount, mounts) {
        if (strcmp(point, mount->point) == 0) {
            found = true;
            break;
        }
    }
    if (!found)
        return _EINVAL;
    return mount_remove(mount);
}

// Checks whether `flag` appears as a whole comma-separated token in `info`
// (a mount option string like "rw,noexec,relatime"), not just as a substring.
bool mount_param_flag(const char *info, const char *flag) {
    // Optimization: Hoist strlen(flag) to avoid recalculating it inside the loop
    size_t flag_len = strlen(flag);
    while (*info != '\0') {
        // Corrected logic: Verify exact prefix match and then properly advance past the comma
        if (strncmp(info, flag, flag_len) == 0 && (info[flag_len] == ',' || info[flag_len] == '\0'))
            return true;
        info += strcspn(info, ",");
        if (*info == ',')
            info++;
    }
    return false;
}

// -- devtmpfs over an already-working /dev --
//
// A devtmpfs mount normally gets the real filesystem (fs/tmp.c), which
// publishes iSH's device nodes into the fresh mount. The one case that must
// NOT is a mount over a /dev that already works: iSH's main /dev is created
// on the root filesystem before init ever runs (AppDelegate.m on the app,
// main.c on the command line) and accumulates live state -- POSIX shared
// memory under /dev/shm, the devpts mount, a bound /dev/log, whatever the
// user put there -- none of which a fresh mount can carry over. Covering it
// with an empty-but-for-our-nodes tmpfs would hide all of that to publish
// device nodes that are, by definition, already present.
//
// So: if the target already holds real device nodes, keep the mount a no-op
// (the behavior that has been shipping since e1b56bf9, when rejecting it with
// EINVAL was found to freeze systemd's boot outright) and merely repair any
// missing nodes in place. Otherwise -- an empty /dev, or one holding the
// regular-file stand-ins a Docker-exported tarball ships instead of real
// nodes -- do a real mount, which is the case that was silently broken:
// mounting devtmpfs there used to "succeed" and leave the directory empty.
static bool devtmpfs_target_is_populated(const char *point) {
    for (size_t i = 0; i < dev_standard_nodes_count; i++) {
        char path[MAX_PATH];
        if (snprintf(path, sizeof(path), "%s/%s", point, dev_standard_nodes[i].name) >= (int) sizeof(path))
            continue;
        struct statbuf stat;
        // Deliberately S_ISCHR and not mere existence: the regular file a
        // tarball leaves at /dev/null is exactly what this must not accept.
        if (generic_statat(AT_PWD, path, &stat, false) == 0 && S_ISCHR(stat.mode))
            return true;
    }
    return false;
}

// Best-effort in-place equivalent of what the devtmpfs population does for a
// fresh mount. Errors are ignored on purpose: this runs on a /dev that is
// already working, so a node we cannot create is a node the caller was living
// without anyway, and failing the mount over it would be a regression.
static void devtmpfs_repair_nodes(const char *point) {
    for (size_t i = 0; i < dev_standard_nodes_count; i++) {
        char path[MAX_PATH];
        if (snprintf(path, sizeof(path), "%s/%s", point, dev_standard_nodes[i].name) >= (int) sizeof(path))
            continue;
        dev_t_ dev = dev_make(dev_standard_nodes[i].major, dev_standard_nodes[i].minor);
        struct statbuf stat;
        int err = generic_statat(AT_PWD, path, &stat, false);
        if (err == 0 && S_ISCHR(stat.mode) && stat.rdev == dev)
            continue;
        if (err == 0 && !S_ISCHR(stat.mode))
            generic_unlinkat(AT_PWD, path); // the regular-file stand-in case
        else if (err == 0)
            continue; // a char device with some other rdev: leave it alone
        generic_mknodat(AT_PWD, path, S_IFCHR | dev_standard_nodes[i].mode, dev);
    }
}

#define MS_SUPPORTED (MS_READONLY_|MS_NOSUID_|MS_NODEV_|MS_NOEXEC_|MS_REMOUNT_|MS_NOATIME_|MS_NODIRATIME_|MS_SILENT_|MS_RELATIME_|MS_STRICTATIME_)
#define MS_FLAGS (MS_READONLY_|MS_NOSUID_|MS_NODEV_|MS_NOEXEC_|MS_NOATIME_|MS_NODIRATIME_|MS_RELATIME_|MS_STRICTATIME_)

// Operation-selector bits: like Linux's do_mount(), these pick *what* the call
// does rather than acting as plain mount options.
#define MS_PROPAGATION (MS_SHARED_|MS_PRIVATE_|MS_SLAVE_|MS_UNBINDABLE_)
#define MS_OP (MS_REMOUNT_|MS_BIND_|MS_MOVE_|MS_PROPAGATION)
// Flags iSH accepts but does not act on: durability/atime/symlink-resolution
// niceties plus the recursive-bind modifier. Without mount namespaces these are
// effectively no-ops, so we strip them rather than reject the whole mount.
#define MS_IGNORED (MS_SYNCHRONOUS_|MS_MANDLOCK_|MS_DIRSYNC_|MS_NOSYMFOLLOW_|MS_REC_|MS_POSIXACL_|MS_I_VERSION_|MS_KERNMOUNT_|MS_LAZYTIME_)

// Create a bind mount at `point` aliasing the already-normalized guest path
// `norm_source`. The bind carries no backing of its own: it shares the source's
// real mount via bind_origin/bind_prefix, and find_mount_and_trim_path() does the
// redirect for every path operation.
static int do_bind_mount(const char *norm_source, const char *point, const char *info, int flags) {
    char src[MAX_PATH];
    if (strlen(norm_source) >= sizeof(src))
        return _ENAMETOOLONG;
    strcpy(src, norm_source);

    // Resolve the source to its real backing mount and the path relative to that
    // mount. find_mount_and_trim_path applies any existing bind redirect, so the
    // origin captured here is always a real (non-bind) mount and `src` becomes the
    // source path relative to it. The returned reference is handed to the bind.
    struct mount *origin = find_mount_and_trim_path(src);
    if (origin == NULL)
        return _EINVAL;

    struct mount *bind = malloc(sizeof(struct mount));
    if (bind == NULL) {
        mount_release(origin);
        return _ENOMEM;
    }
    bind->point = strdup(point);
    bind->point_len = strlen(point);
    bind->source = strdup(norm_source);
    // A bind reports the guest path it was bound from, which is source; it must
    // NOT alias the origin's display_source, or umounting the bind would free
    // the name out from under the origin (`mount --bind / /mnt/x`).
    bind->display_source = NULL;
    bind->info = strdup(info);
    bind->bind_prefix = strdup(src);
    if (bind->point == NULL || bind->source == NULL || bind->info == NULL || bind->bind_prefix == NULL) {
        free((void *) bind->point);
        free((void *) bind->source);
        free((void *) bind->info);
        free((void *) bind->bind_prefix);
        free(bind);
        mount_release(origin);
        return _ENOMEM;
    }
    bind->flags = flags;
    bind->fs = origin->fs;
    bind->refcount = 0;
    bind->root_fd = -1;
    bind->data = NULL;
    bind->fake_dev = origin->fake_dev; // a bind shares the origin's superblock
    bind->bind_origin = origin; // keeps the reference returned above

    lock(&mounts_lock, 0);
    // keep the mounts list ordered by descending mount-point length
    struct mount *after;
    list_for_each_entry(&mounts, after, mounts) {
        if (after->point_len <= bind->point_len)
            break;
    }
    list_add_before(&after->mounts, &bind->mounts);
    unlock(&mounts_lock);
    return 0;
}

dword_t sys_mount_guest(guest_addr_t source_addr, guest_addr_t point_addr, guest_addr_t type_addr, dword_t flags, guest_addr_t data_addr) {
    // source/data/type are copy_mount_string() args in Linux (strndup_user,
    // EINVAL when over-long), NOT getname() pathnames -- so they keep the plain
    // user_read_string path. Only the mount point below is a real getname path.
    char source[MAX_PATH] = "";
    if (source_addr != 0 && user_read_string(source_addr, source, sizeof(source)))
        return _EFAULT;
    char point_raw[MAX_PATH];
    int path_err = user_read_path(point_addr, point_raw, sizeof(point_raw));
    if (path_err)
        return path_err;
    char data[MAX_PATH] = "";
    if (data_addr != 0 && user_read_string(data_addr, data, sizeof(data)))
        return _EFAULT;
    char type[100] = "";
    if (type_addr != 0 && user_read_string(type_addr, type, sizeof(type)))
        return _EFAULT;
    STRACE("mount(\"%s\", \"%s\", \"%s\", %#x, \"%s\")", source, point_raw, type, flags, data_addr != 0 ? data : NULL);
    if (mount_trace_elogind())
        printk("INFO: elogind mount pid=%d comm=%s source=%s target=%s type=%s flags=%#x data=%s\n",
               current->pid, current->comm, source, point_raw, type, flags, data_addr != 0 ? data : "");

    dword_t unhandled = flags & ~MS_SUPPORTED & ~MS_OP & ~MS_IGNORED;
    if (unhandled) {
        FIXME("missing mount flags %#x from %s[%d]", unhandled, current->comm, current->pid);
        return _EINVAL;
    }

    // A propagation-type change (mount --make-[r]private/shared/slave/unbindable)
    // is its own operation that carries no new filesystem. iSH has no mount
    // namespaces or peer groups, so the change has no observable effect -- accept
    // it as a no-op rather than rejecting the caller (systemd issues these at boot).
    if ((flags & MS_PROPAGATION) && !(flags & MS_BIND_))
        return 0;

    struct statbuf stat;
    int err = generic_statat(AT_PWD, point_raw, &stat, 0);
    if (err < 0)
        return err;
    if (!S_ISDIR(stat.mode))
        return _ENOTDIR;

    char point[MAX_PATH];
    err = path_normalize(AT_PWD, point_raw, point, N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;

    // MS_MOVE and MS_BIND name an existing path as their "source"; resolve it
    // before taking mounts_lock (path_normalize / find_mount_and_trim_path
    // re-enter mount_find, which takes the lock).
    char op_source[MAX_PATH];
    if (flags & (MS_MOVE_ | MS_BIND_)) {
        err = path_normalize(AT_PWD, source, op_source, N_SYMLINK_FOLLOW);
        if (err < 0)
            return err;
    }

    // A bind shares the source mount's backing; do_bind_mount resolves the source
    // and takes mounts_lock itself, so dispatch it before we lock here.
    if (flags & MS_BIND_) {
        err = do_bind_mount(op_source, point, data, flags & MS_FLAGS);
        if (err >= 0)
            proc_mountinfo_notify_changed();
        return err;
    }

    // Before taking the lock: both helpers resolve guest paths, which re-enters
    // mount_find() and takes mounts_lock (it isn't recursive). MS_MOVE/MS_REMOUNT
    // name an existing mount rather than creating one, so they skip this.
    if (strcmp(type, "devtmpfs") == 0 && !(flags & (MS_MOVE_ | MS_REMOUNT_)) &&
            devtmpfs_target_is_populated(point)) {
        devtmpfs_repair_nodes(point);
        return 0;
    }

    lock(&mounts_lock, 0);
    if (flags & MS_MOVE_) {
        struct mount *mount, *found = NULL;
        list_for_each_entry(&mounts, mount, mounts) {
            if (strcmp(mount->point, op_source) == 0) {
                found = mount;
                break;
            }
        }
        if (found == NULL) {
            unlock(&mounts_lock);
            return _EINVAL;
        }
        const char *new_point = strdup(point);
        if (new_point == NULL) {
            unlock(&mounts_lock);
            return _ENOMEM;
        }
        free((void *) found->point);
        found->point = new_point;
        found->point_len = strlen(new_point);
        // keep the mounts list ordered by descending mount-point length
        list_remove(&found->mounts);
        struct mount *after;
        list_for_each_entry(&mounts, after, mounts) {
            if (after->point_len <= found->point_len)
                break;
        }
        list_add_before(&after->mounts, &found->mounts);
        unlock(&mounts_lock);
        proc_mountinfo_notify_changed();
        return 0;
    }

    if (flags & MS_REMOUNT_) {
        struct mount *mount;
        bool found = false;
        list_for_each_entry(&mounts, mount, mounts) {
            bool is_root_remount = strcmp(point, "/") == 0 && mount->point[0] == '\0';
            if (strcmp(point, mount->point) == 0 || is_root_remount) {
                mount->flags = (mount->flags & ~MS_FLAGS) | (flags & MS_FLAGS);
                found = true;
                break;
            }
        }
        unlock(&mounts_lock);
        if (found)
            proc_mountinfo_notify_changed();
        return found ? 0 : _EINVAL;
    }

    const struct fs_ops *fs = NULL;
    for (size_t i = 0; i < sizeof(filesystems)/sizeof(filesystems[0]); i++) {
        if (filesystems[i] && (strcmp(filesystems[i]->name, type) == 0)) {
            fs = filesystems[i];
            break;
        }
    }
    if (fs == NULL &&
            strcmp(point, "/proc/sys/fs/binfmt_misc") == 0 &&
            (strcmp(type, "binfmt_misc") == 0 ||
             strcmp(source, "binfmt_misc") == 0 ||
             strcmp(source, "none") == 0)) {
        unlock(&mounts_lock);
        return 0;
    }
    if (fs == NULL) {
        unlock(&mounts_lock);
        return _EINVAL;
    }

    err = do_mount(fs, source, point, data, flags & MS_FLAGS);
    unlock(&mounts_lock);
    if (err >= 0)
        proc_mountinfo_notify_changed();
    if (mount_trace_elogind())
        printk("INFO: elogind mount-result pid=%d comm=%s target=%s result=%d\n",
               current->pid, current->comm, point, err);
    return err;
}

dword_t sys_mount(addr_t source_addr, addr_t point_addr, addr_t type_addr, dword_t flags, addr_t data_addr) {
    return sys_mount_guest(source_addr, point_addr, type_addr, flags, data_addr);
}

#define UMOUNT_NOFOLLOW_ 8

dword_t sys_umount2_guest(guest_addr_t target_addr, dword_t flags) {
    char target_raw[MAX_PATH];
    int path_err = user_read_path(target_addr, target_raw, sizeof(target_raw));
    if (path_err)
        return path_err;
    char target[MAX_PATH];
    int err = path_normalize(AT_PWD, target_raw, target,
            flags & UMOUNT_NOFOLLOW_ ? N_SYMLINK_NOFOLLOW : N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;

    lock(&mounts_lock, 0);
    err = do_umount(target);
    unlock(&mounts_lock);
    if (err >= 0)
        proc_mountinfo_notify_changed();
    return err;
}

dword_t sys_umount2(addr_t target_addr, dword_t flags) {
    return sys_umount2_guest(target_addr, flags);
}

struct list mounts = {&mounts, &mounts};
lock_t mounts_lock = LOCK_INITIALIZER;

// -- new mount API (fsopen/fsconfig/fsmount/move_mount) --
//
// systemd >= 254 unconditionally sets up a private credentials tmpfs for
// EVERY service it spawns (not just ones with LoadCredential=), via exactly
// this sequence: fsopen("tmpfs") -> fsconfig(..., FSCONFIG_CMD_CREATE) ->
// fsmount() -> [write credential files through the returned fd] ->
// fsconfig(..., FSCONFIG_CMD_RECONFIGURE) [apply "ro"] -> move_mount() to
// place it at /run/credentials/<unit>. Before this, all four syscalls were
// silent ENOSYS stubs, so every unit's credential setup failed with "Failed
// to set up credentials: Function not implemented" and the unit never ran.
//
// iSH has no per-task mount namespaces, so "detached mount, not yet visible
// anywhere" is modeled as a real mount at a private, hidden mountpoint
// (/.ish-fsmount/<n>) -- fsmount()'s returned fd is an ordinary, fully
// functional directory fd on it (so callers can write into it immediately,
// exactly as real Linux's detached-mount semantics allow), and move_mount()
// simply relocates that mount's point in the global mounts list, matching
// sys_mount's own MS_MOVE handling above. This only supports the one usage
// pattern real callers of this API actually exercise (a fresh fsopen'd
// filesystem, immediately fsmount'd and then move_mount'd into place via
// MOVE_MOUNT_F_EMPTY_PATH) -- not open_tree-sourced moves or in-place
// (MOVE_MOUNT_T_EMPTY_PATH) placement, which remain unimplemented.
struct fscontext_data {
    const struct fs_ops *fs;
    bool created;
    bool readonly;
    char point[MAX_PATH];
};

static int fscontext_close(struct fd *fd) {
    free(fd->data);
    return 0;
}

static struct fd_ops fscontext_ops = {
    .anon_inode_class = "[fscontext]",
    .close = fscontext_close,
};

#define FSOPEN_CLOEXEC_ 1

fd_t sys_fsopen_guest(guest_addr_t fsname_addr, dword_t flags) {
    char fsname[100] = "";
    if (user_read_string(fsname_addr, fsname, sizeof(fsname)))
        return _EFAULT;
    STRACE("fsopen(\"%s\", %#x)", fsname, flags);
    if (flags & ~FSOPEN_CLOEXEC_)
        return _EINVAL;

    const struct fs_ops *fs = NULL;
    for (size_t i = 0; i < sizeof(filesystems) / sizeof(filesystems[0]); i++) {
        if (filesystems[i] != NULL && strcmp(filesystems[i]->name, fsname) == 0) {
            fs = filesystems[i];
            break;
        }
    }
    if (fs == NULL)
        return _ENODEV;

    struct fscontext_data *data = malloc(sizeof(struct fscontext_data));
    if (data == NULL)
        return _ENOMEM;
    *data = (struct fscontext_data) {.fs = fs};
    struct fd *fd = adhoc_fd_create(&fscontext_ops);
    if (fd == NULL) {
        free(data);
        return _ENOMEM;
    }
    fd->data = data;
    return f_install(fd, (flags & FSOPEN_CLOEXEC_) ? O_CLOEXEC_ : 0);
}

fd_t sys_fsopen(addr_t fsname_addr, dword_t flags) {
    return sys_fsopen_guest(fsname_addr, flags);
}

#define FSCONFIG_SET_FLAG_ 0
#define FSCONFIG_SET_STRING_ 1
#define FSCONFIG_SET_BINARY_ 2
#define FSCONFIG_SET_PATH_ 3
#define FSCONFIG_SET_PATH_EMPTY_ 4
#define FSCONFIG_SET_FD_ 5
#define FSCONFIG_CMD_CREATE_ 6
#define FSCONFIG_CMD_RECONFIGURE_ 7

dword_t sys_fsconfig_guest(fd_t f, dword_t cmd, guest_addr_t key_addr, guest_addr_t value_addr, int_t aux) {
    char key[100] = "";
    if (key_addr != 0 && user_read_string(key_addr, key, sizeof(key)))
        return _EFAULT;
    STRACE("fsconfig(%d, %u, \"%s\", %#llx, %d)", f, cmd, key,
            (unsigned long long) value_addr, aux);

    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &fscontext_ops)
        return _EINVAL;
    struct fscontext_data *data = fd->data;

    switch (cmd) {
        case FSCONFIG_SET_FLAG_:
        case FSCONFIG_SET_STRING_:
            // We only act on the one option this codebase's mount model has
            // an equivalent for ("ro"); everything else (size=, mode=,
            // source=, SELinux context=, ...) is silently accepted, matching
            // this project's existing "don't model X" precedent for mount
            // options iSH has no backing concept for (see do_mount's
            // MS_IGNORED).
            if (strcmp(key, "ro") == 0)
                data->readonly = true;
            return 0;
        case FSCONFIG_SET_BINARY_:
        case FSCONFIG_SET_PATH_:
        case FSCONFIG_SET_PATH_EMPTY_:
        case FSCONFIG_SET_FD_:
            return 0; // accepted, not modeled
        case FSCONFIG_CMD_CREATE_: {
            if (data->created)
                return _EBUSY;
            static _Atomic unsigned next_fscontext_id = 0;
            snprintf(data->point, sizeof(data->point), "/.ish-fsmount/%u",
                    next_fscontext_id++);
            // generic_mkdirat resolves its path via mount_find, which takes
            // mounts_lock itself -- must run before we take the lock below,
            // or this self-deadlocks (mounts_lock isn't recursive).
            int mkerr = generic_mkdirat(AT_PWD, "/.ish-fsmount", 0700);
            if (mkerr < 0 && mkerr != _EEXIST)
                return mkerr;
            lock(&mounts_lock, 0);
            int err = do_mount(data->fs, "", data->point, "",
                    data->readonly ? MS_READONLY_ : 0);
            unlock(&mounts_lock);
            if (err < 0)
                return err;
            data->created = true;
            proc_mountinfo_notify_changed();
            return 0;
        }
        case FSCONFIG_CMD_RECONFIGURE_: {
            if (!data->created)
                return _EINVAL;
            lock(&mounts_lock, 0);
            struct mount *mount;
            bool found = false;
            list_for_each_entry(&mounts, mount, mounts) {
                if (strcmp(mount->point, data->point) == 0) {
                    mount->flags = (mount->flags & ~MS_READONLY_) |
                        (data->readonly ? MS_READONLY_ : 0);
                    found = true;
                    break;
                }
            }
            unlock(&mounts_lock);
            if (found)
                proc_mountinfo_notify_changed();
            return found ? 0 : _EINVAL;
        }
        default:
            return 0; // lenient: unrecognized cmd accepted as a no-op
    }
}

dword_t sys_fsconfig(fd_t f, dword_t cmd, addr_t key_addr, addr_t value_addr, int_t aux) {
    return sys_fsconfig_guest(f, cmd, key_addr, value_addr, aux);
}

fd_t sys_fsmount_guest(fd_t f, dword_t flags, dword_t attr_flags) {
    STRACE("fsmount(%d, %#x, %#x)", f, flags, attr_flags);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &fscontext_ops)
        return _EINVAL;
    struct fscontext_data *data = fd->data;
    if (!data->created)
        return _EINVAL;

    // data->point is a real-root staging path (/.ish-fsmount/<n>) that is
    // not visible inside a chroot; open it against the real root, or
    // util-linux mount(8)'s new-API path fails ENOENT in every chroot.
    struct fd *dirfd = generic_open_realroot(data->point,
            O_RDONLY_ | O_DIRECTORY_ | O_CLOEXEC_, 0);
    if (IS_ERR(dirfd))
        return PTR_ERR(dirfd);
    return f_install(dirfd, O_CLOEXEC_);
}

fd_t sys_fsmount(fd_t f, dword_t flags, dword_t attr_flags) {
    return sys_fsmount_guest(f, flags, attr_flags);
}

// The filesystem backing the mount rooted exactly at `point`, or NULL.
static const struct fs_ops *mount_fs_at(const char *point) {
    lock(&mounts_lock, 0);
    struct mount *mount;
    const struct fs_ops *fs = NULL;
    list_for_each_entry(&mounts, mount, mounts) {
        if (strcmp(mount->point, point) == 0) {
            fs = mount->fs;
            break;
        }
    }
    unlock(&mounts_lock);
    return fs;
}

// Relocates an existing mount's point in the mounts list -- the same
// list-resplice logic as sys_mount's MS_MOVE handling above, factored out
// separately (rather than shared) so this new code path can't regress the
// already-exercised classic mount(2) MS_MOVE behavior.
static int mount_relocate(const char *from_point, const char *to_point) {
    lock(&mounts_lock, 0);
    struct mount *mount, *found = NULL;
    list_for_each_entry(&mounts, mount, mounts) {
        if (strcmp(mount->point, from_point) == 0) {
            found = mount;
            break;
        }
    }
    if (found == NULL) {
        unlock(&mounts_lock);
        return _EINVAL;
    }
    const char *new_point = strdup(to_point);
    if (new_point == NULL) {
        unlock(&mounts_lock);
        return _ENOMEM;
    }
    free((void *) found->point);
    found->point = new_point;
    found->point_len = strlen(new_point);
    list_remove(&found->mounts);
    struct mount *after;
    list_for_each_entry(&mounts, after, mounts) {
        if (after->point_len <= found->point_len)
            break;
    }
    list_add_before(&after->mounts, &found->mounts);
    unlock(&mounts_lock);
    return 0;
}

#define MOVE_MOUNT_F_EMPTY_PATH_ 0x00000004

dword_t sys_move_mount_guest(fd_t from_dfd, guest_addr_t from_path_addr, fd_t to_dfd,
        guest_addr_t to_path_addr, dword_t flags) {
    char from_path[MAX_PATH] = "";
    if (from_path_addr != 0 && user_read_string(from_path_addr, from_path, sizeof(from_path)))
        return _EFAULT;
    char to_path_raw[MAX_PATH];
    int path_err = user_read_path(to_path_addr, to_path_raw, sizeof(to_path_raw));
    if (path_err)
        return path_err;
    STRACE("move_mount(%d, \"%s\", %d, \"%s\", %#x)", from_dfd, from_path, to_dfd,
            to_path_raw, flags);

    // Only the one usage pattern real callers exercise is supported: from_dfd
    // is a detached mount fd (from fsmount()) referenced via
    // MOVE_MOUNT_F_EMPTY_PATH, and to_path is resolved the same dfd-less way
    // sys_mount's point already is (classic mount(2) has no dfd concept
    // either, so to_dfd besides AT_FDCWD was never supported there).
    if (!(flags & MOVE_MOUNT_F_EMPTY_PATH_) || from_path[0] != '\0')
        return _EINVAL;

    struct fd *fd = f_get(from_dfd);
    if (fd == NULL)
        return _EBADF;
    char from_point[MAX_PATH];
    int err = generic_getpath(fd, from_point);
    if (err < 0)
        return err;

    struct statbuf stat;
    err = generic_statat(AT_PWD, to_path_raw, &stat, 0);
    if (err < 0)
        return err;
    if (!S_ISDIR(stat.mode))
        return _ENOTDIR;

    char to_point[MAX_PATH];
    err = path_normalize(AT_PWD, to_path_raw, to_point, N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;

    // The same rule sys_mount_guest applies to devtmpfs, or this API would be
    // a way around it: a devtmpfs must not be moved on top of a /dev that is
    // already populated. Leave the mount detached and repair in place instead.
    //
    // The umount is best-effort and normally fails: a caller doing this for
    // real still holds the fsmount fd (that is how the API works), and that fd
    // holds a reference, so mount_remove returns EBUSY. Forcing it would leave
    // the caller's fd pointing at freed memory, so the mount stays parked at
    // its private staging path and shows up in /proc/mounts there. That is
    // cosmetic -- it never reaches the target, which is the guarantee that
    // matters -- and removing it properly needs lazy-detach (Linux's
    // MNT_DETACH: unlink from the namespace now, free on last reference),
    // which is a mount-lifetime change worth doing on its own rather than as
    // a rider here.
    if (mount_fs_at(from_point) == &devtmpfs && devtmpfs_target_is_populated(to_point)) {
        lock(&mounts_lock, 0);
        do_umount(from_point);
        unlock(&mounts_lock);
        devtmpfs_repair_nodes(to_point);
        proc_mountinfo_notify_changed();
        return 0;
    }

    err = mount_relocate(from_point, to_point);
    if (err >= 0)
        proc_mountinfo_notify_changed();
    return err;
}

dword_t sys_move_mount(fd_t from_dfd, addr_t from_path_addr, fd_t to_dfd, addr_t to_path_addr,
        dword_t flags) {
    return sys_move_mount_guest(from_dfd, from_path_addr, to_dfd, to_path_addr, flags);
}
