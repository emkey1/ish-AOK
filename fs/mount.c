#include <string.h>
#include <sys/stat.h>
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/path.h"
#include "fs/real.h"
#include "fs/dev.h"
#include "kernel/binfmt_misc.h"

// Sized for the static set below plus the two the iOS app registers at
// startup (iosfs and iosfs_unsafe) with room to spare -- overflowing this
// asserts at boot rather than failing gracefully, so don't run it to the rim.
#define MAX_FILESYSTEMS 16
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
    &fusefs,
    &mqueuefs,
};

static bool mount_trace_elogind(void) {
    return false;
}

// Filesystem by registered name. fsopen has always done this inline; native
// programs (kernel/smallclue_shim.c) need the same lookup from outside this
// file, so it is exported rather than copied.
const struct fs_ops *fs_lookup(const char *name) {
    if (name == NULL)
        return NULL;
    for (size_t i = 0; i < sizeof(filesystems) / sizeof(filesystems[0]); i++)
        if (filesystems[i] != NULL && strcmp(filesystems[i]->name, name) == 0)
            return filesystems[i];
    return NULL;
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

#define BINFMT_MISC_FS_LINE "nodev    binfmt_misc\n"

char * get_filesystems(void) {
    unsigned int i;
    size_t total_len = 0;

    // Pass 1: Calculate the exact length required
    for (i = 0; i < MAX_FILESYSTEMS; i++) {
        if (filesystems[i] != NULL) {
            total_len += strlen("nodev    ") + strlen(filesystems[i]->name) + 1; // +1 for newline
        }
    }
    // binfmt_misc is a filesystem TYPE the kernel accepts but not an entry in
    // filesystems[]: mounting it does not create a mount, it brings
    // /proc/sys/fs/binfmt_misc's `register` and `status` into being (fs/proc/sys.c).
    // It has to be listed here all the same, because mount(8) reads this file
    // and refuses a type it does not find WITHOUT EVER CALLING mount(2) --
    // which is exactly why the accept-the-mount branch further down was
    // unreachable for years.
    total_len += strlen(BINFMT_MISC_FS_LINE);

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
    memcpy(ptr, BINFMT_MISC_FS_LINE, strlen(BINFMT_MISC_FS_LINE));
    ptr += strlen(BINFMT_MISC_FS_LINE);
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

// Is a filesystem mounted at exactly this point?
//
// Not the same question as mount_find, which resolves a path to the mount
// CONTAINING it and so answers "yes" for every path under a mount. This is an
// exact match on the mount point itself, which is what tells a root that
// actually mounted from one that silently did not: the app creates
// /AOK/roots/<name> and only then calls do_mount, so the directory exists
// either way and its presence proves nothing.
//
// The root's point is stored as "" (its guest path normalizes away), so accept
// the "/" spelling callers naturally use, the same way /proc/mounts prints it.
bool mount_exists_at_point(const char *point) {
    if (strcmp(point, "/") == 0)
        point = "";
    lock(&mounts_lock, 0);
    struct mount *mount;
    bool found = false;
    list_for_each_entry(&mounts, mount, mounts) {
        if (strcmp(mount->point, point) == 0) {
            found = true;
            break;
        }
    }
    unlock(&mounts_lock);
    return found;
}

void mount_retain(struct mount *mount) {
    lock(&mounts_lock, 0);
    mount->refcount++;
    unlock(&mounts_lock);
}

static void mount_destroy(struct mount *mount);

void mount_release(struct mount *mount) {
    lock(&mounts_lock, 0);
    mount->refcount--;
    // The last user of a lazily-unmounted mount finishes the unmount. Nothing
    // else can be about to take a reference: it has not been reachable through
    // the mount table since the umount2 call.
    if (mount->lazy_umount && mount->refcount == 0)
        mount_destroy(mount);
    unlock(&mounts_lock);
}

// Tell a mount's filesystem it has moved (fs_ops.relocated). The caller has
// just re-pointed it under mounts_lock and taken a reference there; this runs
// after the unlock, since a filesystem may do real work in the hook (iosfs
// hands its bookmark to the main queue), and gives the reference back.
static void mount_notify_relocated(struct mount *mount, const char *old_point,
        const char *new_point) {
    if (mount->fs->relocated != NULL)
        mount->fs->relocated(mount, old_point, new_point);
    mount_release(mount);
}

// Mount IDs, as /proc/self/mountinfo and statx's stx_mnt_id report them. The
// two must agree -- systemd cross-checks STATX_MNT_ID against mountinfo --
// which they do by both calling mount_id.
//
// An ID used to be the mount's 1-based position in `mounts`. That list is
// kept longest point first, so the root, with the shortest point of all, was
// numbered LAST, and every mount or umount renumbered whatever sat after it
// in the list. mountinfo's parent column turned that into a loop: the root
// said its parent was 1, and 1 was the deepest mount on the list, whose own
// parent was the root (`5 1 ... / /` beside `1 5 ... / /dev/pts`). libmount
// finds the root of the tree by walking parent IDs up until one is missing
// from the table (mnt_table_get_root_fs), so plain `findmnt` spun at 100% CPU
// forever; only `findmnt -l`, which never builds the tree, worked.
//
// Linux numbers a mount when it is made and keeps the number until the mount
// is gone, and so does this, from a counter. MOUNT_ID_HIDDEN is never handed
// out; see its definition in kernel/fs.h. Numbers are not reused: a caller
// holding an ID across an umount then learns the mount is gone rather than
// finding someone else's.
static _Atomic int next_mount_id = MOUNT_ID_HIDDEN + 1;

static int mount_new_id(void) {
    return next_mount_id++;
}

// No lock: a mount's ID is set before it is published in the list and never
// changes after, and /proc/self/mountinfo asks for IDs from inside its own
// walk of the list, under mounts_lock.
int mount_id(struct mount *mount) {
    return mount->id > 0 ? mount->id : MOUNT_ID_HIDDEN;
}

// Re-express `path`, relative to `origin`, as it is seen through the bind
// mount with ID `bind_id`: the bind's point, then what follows the bind's
// source in `path`. In place; `path` is a MAX_PATH buffer. False, leaving
// `path` alone, when there is no such bind any more (unmounted, lazily or
// not: a descriptor holds its origin, not the bind), when it is not a bind of
// `origin`, or when `path` is no longer under its source because the file was
// renamed out from under it.
//
// By ID under mounts_lock rather than by a pointer the descriptor keeps,
// since a descriptor holds no reference on the bind, and IDs are never
// reused. The lock also covers bind->point, which MS_MOVE replaces.
bool mount_path_through_bind(int bind_id, const struct mount *origin, char *path) {
    bool done = false;
    lock(&mounts_lock, 0);
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (mount->id != bind_id)
            continue;
        if (mount->bind_origin != origin)
            break;
        size_t prefix_len = strlen(mount->bind_prefix);
        if (strncmp(path, mount->bind_prefix, prefix_len) != 0 ||
                (path[prefix_len] != '\0' && path[prefix_len] != '/'))
            break;
        size_t rest_len = strlen(path + prefix_len);
        if (mount->point_len + rest_len >= MAX_PATH)
            break;
        memmove(path + mount->point_len, path + prefix_len, rest_len + 1);
        memcpy(path, mount->point, mount->point_len);
        done = true;
        break;
    }
    unlock(&mounts_lock);
    return done;
}

static int mount_compare_id(const void *a, const void *b) {
    int x = (*(struct mount *const *) a)->id;
    int y = (*(struct mount *const *) b)->id;
    return (x > y) - (x < y);
}

// Every listed mount -- all but a detached fsmount() -- in the order Linux
// lists a namespace's: by mount ID (6.8 keeps a namespace's mounts in an
// rbtree on the ID, and /proc/self/mountinfo walks it), which here is the
// order they were made in. `mounts` itself is longest point first and newest
// first among equal points, and was printed as it stood, root last. A reader
// takes the LAST entry for a point as the mount on top -- libmount keeps
// "later mounted filesystems" (mnt_table_uniq_fs), df keeps the last -- so
// for a point with two mounts on it, that named the one underneath.
// Caller holds mounts_lock; free() the result. NULL only when out of memory.
struct mount **mounts_listed_locked(size_t *count_out) {
    size_t count = 0;
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts)
        if (!mount->detached)
            count++;
    struct mount **listed = malloc((count > 0 ? count : 1) * sizeof(*listed));
    if (listed == NULL)
        return NULL;
    size_t i = 0;
    list_for_each_entry(&mounts, mount, mounts)
        if (!mount->detached)
            listed[i++] = mount;
    qsort(listed, count, sizeof(*listed), mount_compare_id);
    *count_out = count;
    return listed;
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
    // calloc, not malloc: every field below is assigned by hand, and a field
    // added to struct mount later that nobody remembers to add HERE is
    // whatever the allocator left behind. That is not hypothetical -- adding
    // `lazy_umount` did exactly this, and under MALLOC_PERTURB_ (which meson
    // sets for the e2e suite) it read back as true on a brand-new mount, so
    // the first mount_release tore down a mount that was still in use. Zero
    // is the right default for every field here, so start from it.
    struct mount *new_mount = calloc(1, sizeof(struct mount));
    if (new_mount == NULL)
        return _ENOMEM;
    // Not a valid descriptor until the filesystem's own mount() opens one.
    new_mount->root_fd = -1;
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
    new_mount->id = mount_new_id();

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

// Snapshot the mount table for a caller outside this file. Every traversal of
// `mounts` lives in here because the lock discipline does -- kernel/fs.h says
// the lock must be held while traversing, and mount_statfs must NOT be called
// under it. A native `df` (kernel/smallclue_shim.c) got that wrong in both
// directions before this existed, and hung.
//
// Two passes on purpose: collect the mounts under the lock, then statfs each
// with the lock dropped. Entries are copied rather than referenced so nothing
// outlives the lock.
int mount_snapshot(struct mount_info **out, size_t *count_out) {
    if (out == NULL || count_out == NULL)
        return _EINVAL;
    *out = NULL;
    *count_out = 0;

    lock(&mounts_lock, 0);
    // Detached fsmount()s are not part of any mount listing until move_mount
    // places them -- the same rule /proc/mounts and mountinfo follow. This is
    // the list a native `df` walks (kernel/native_libc.c's getmntinfo), and
    // it was showing the private 0700 staging path: df then tried to statfs a
    // directory an unprivileged guest cannot enter and printed
    // "df: /.ish-fsmount/N: Permission denied" for a mount Linux never lists.
    // In the same order as those two files, too.
    size_t count = 0;
    struct mount **listed = mounts_listed_locked(&count);
    struct mount_info *info = listed != NULL ? calloc(count > 0 ? count : 1, sizeof(*info)) : NULL;
    if (info == NULL) {
        unlock(&mounts_lock);
        free(listed);
        return _ENOMEM;
    }
    size_t i = 0;
    for (; i < count; i++) {
        struct mount *mount = listed[i];
        const char *from = mount->display_source != NULL ? mount->display_source
                                                         : mount->source;
        snprintf(info[i].source, sizeof(info[i].source), "%s",
                 from != NULL ? from : "none");
        snprintf(info[i].point, sizeof(info[i].point), "%s",
                 (mount->point != NULL && mount->point[0] != '\0') ? mount->point : "/");
        snprintf(info[i].type, sizeof(info[i].type), "%s",
                 mount->fs != NULL && mount->fs->name != NULL ? mount->fs->name : "none");
        info[i].mount = mount;
    }
    unlock(&mounts_lock);
    free(listed);

    // Lock dropped: mount_statfs reaches into the filesystem and must not run
    // under mounts_lock.
    for (size_t j = 0; j < i; j++) {
        struct statfsbuf sb = {};
        if (mount_statfs(info[j].mount, &sb) == 0)
            info[j].statfs = sb;
    }

    *out = info;
    *count_out = i;
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

// The teardown half of mount_remove, once nothing is using the mount. Caller
// holds mounts_lock; list_remove on an already-removed, re-initialised node is
// a no-op, so this is safe for both entry points below.
static void mount_destroy(struct mount *mount) {
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
}

int mount_remove(struct mount *mount) {
    if (mount->refcount != 0)
        return _EBUSY;
    mount_destroy(mount);
    return 0;
}

// umount2(MNT_DETACH) -- `umount -l`. The point of a lazy unmount is that it
// works on a BUSY mount: it comes out of the tree at once so nothing new can
// reach it, and the filesystem is released when whoever is still holding it
// lets go. Without it a busy mount can only be unmounted by finding and
// stopping every user, which is exactly the situation `umount -l` exists for
// -- and the flag was ignored, so it answered EBUSY like a plain umount.
int mount_remove_lazy(struct mount *mount) {
    if (mount->refcount == 0) {
        mount_destroy(mount);
        return 0;
    }
    // Out of the namespace now; re-initialising the node keeps the later
    // list_remove in mount_destroy harmless.
    list_remove(&mount->mounts);
    list_init(&mount->mounts);
    mount->lazy_umount = true;
    return 0;
}

static int do_umount_flags(const char *point, bool lazy) {
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
    return lazy ? mount_remove_lazy(mount) : mount_remove(mount);
}

#define MNT_DETACH_ 2

int do_umount(const char *point) {
    return do_umount_flags(point, false);
}

// umount2(MNT_DETACH) for kernel callers: out of the mount table now, torn down
// when its last reference goes. What undoing a mount wants -- the checkpoint's
// failed-restore unwind -- because "nothing may still hold it" is exactly the
// guarantee an unwind cannot make.
int do_umount_lazy(const char *point) {
    return do_umount_flags(point, true);
}



// Mount and unmount for callers outside fs/, where do_mount/do_umount's
// "caller holds mounts_lock" contract is easy to miss. The app is exactly such
// a caller: it attaches every installed root at /AOK/roots/<name> and detaches
// one again when that root is renamed or deleted, from its own threads, while
// guest CPU threads are walking this same list on every path lookup. Doing
// that unlocked means list_add_before/list_remove -- and mount_remove's free()
// -- running against a list somebody else is mid-traversal of.
//
// These take the lock and post the mount-table change, so a caller outside
// this file never has to know about either. Same shape as mount_snapshot and
// mount_exists_at_point: call them WITHOUT mounts_lock held.
int mount_attach(const struct fs_ops *fs, const char *source, const char *point, const char *info, int flags) {
    lock(&mounts_lock, 0);
    int err = do_mount(fs, source, point, info, flags);
    unlock(&mounts_lock);
    if (err >= 0)
        proc_mountinfo_notify_changed();
    return err;
}

int mount_detach(const char *point) {
    lock(&mounts_lock, 0);
    int err = do_umount(point);
    unlock(&mounts_lock);
    if (err >= 0)
        proc_mountinfo_notify_changed();
    return err;
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
        // Deliberately a TYPE test and not mere existence: the regular file a
        // tarball leaves at /dev/null is exactly what this must not accept.
        // Compared against the entry's own type rather than S_ISCHR, now that
        // the table can describe a block device -- otherwise a match on
        // /dev/aokswap0 would be ignored (harmless today, since a char entry
        // precedes it, and wrong the moment the table is reordered).
        mode_t_ want = dev_standard_nodes[i].is_block ? S_IFBLK : S_IFCHR;
        if (generic_statat(AT_PWD, path, &stat, false) == 0 &&
            (stat.mode & S_IFMT) == want)
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
        // The entry's own type, since the table can describe a block device.
        mode_t_ fmt = dev_standard_nodes[i].is_block ? S_IFBLK : S_IFCHR;
        struct statbuf stat;
        int err = generic_statat(AT_PWD, path, &stat, false);
        if (err == 0 && (stat.mode & S_IFMT) == fmt && stat.rdev == dev)
            continue;
        if (err == 0 && (stat.mode & S_IFMT) != fmt)
            generic_unlinkat(AT_PWD, path); // the regular-file stand-in case
        else if (err == 0)
            continue; // right type, some other rdev: leave it alone
        generic_mknodat(AT_PWD, path, fmt | dev_standard_nodes[i].mode, dev);
    }
}

#define MS_SUPPORTED (MS_READONLY_|MS_NOSUID_|MS_NODEV_|MS_NOEXEC_|MS_REMOUNT_|MS_NOATIME_|MS_NODIRATIME_|MS_SILENT_|MS_RELATIME_|MS_STRICTATIME_)
#define MS_FLAGS (MS_READONLY_|MS_NOSUID_|MS_NODEV_|MS_NOEXEC_|MS_NOATIME_|MS_NODIRATIME_|MS_RELATIME_|MS_STRICTATIME_)

// Operation-selector bits: like Linux's do_mount(), these pick *what* the call
// does rather than acting as plain mount options.
#define MS_PROPAGATION (MS_SHARED_|MS_PRIVATE_|MS_SLAVE_|MS_UNBINDABLE_)
#define MS_OP (MS_REMOUNT_|MS_BIND_|MS_MOVE_|MS_PROPAGATION)
// Flags iSH-AOK accepts but does not act on: durability/atime/symlink-resolution
// niceties. Without mount namespaces these are effectively no-ops, so we strip
// them rather than reject the whole mount. MS_REC stays in this set so a
// recursive propagation change (mount --make-rprivate) is still accepted as a
// no-op, but the bind path does act on it (bind_replicate_submounts).
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

    // calloc for the same reason as do_mount above.
    struct mount *bind = calloc(1, sizeof(struct mount));
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
    // A mount of its own, so an ID of its own, as a Linux bind has.
    bind->id = mount_new_id();

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

// mount --rbind: replicate every mount already sitting under the source
// subtree at the corresponding path under the new bind. Linux clones the
// subtree atomically; here each submount is re-bound one by one, best-effort
// -- a submount that fails (ENOMEM, over-long path) is skipped rather than
// unwinding the whole operation, matching this file's lenient treatment of
// mount niceties. The snapshot is taken before any replica is created, so a
// target inside the source subtree (mount --rbind / /mnt/x) terminates
// instead of finding and copying its own copies. The one post-attach mount the
// snapshot can still see is the parent bind itself, created just before this
// runs -- when the target sits inside the source subtree it matches the scan
// and would clone itself (/mnt/x/mnt/x), which Linux's clone-then-attach
// order never produces, so it is excluded by exact point.
static void bind_replicate_submounts(const char *norm_source, const char *point, const char *info, int flags) {
    size_t src_len = strlen(norm_source);
    // Collect the submount points under the lock, then bind with it dropped:
    // do_bind_mount resolves paths and takes mounts_lock itself.
    size_t count = 0, cap = 0;
    char **subs = NULL;
    lock(&mounts_lock, 0);
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (mount->point_len <= src_len || strncmp(mount->point, norm_source, src_len) != 0 ||
                mount->point[src_len] != '/')
            continue;
        if (strcmp(mount->point, point) == 0)
            continue;
        if (count == cap) {
            size_t new_cap = cap == 0 ? 8 : cap * 2;
            char **grown = realloc(subs, new_cap * sizeof(*subs));
            if (grown == NULL)
                break;
            subs = grown;
            cap = new_cap;
        }
        subs[count] = strdup(mount->point);
        if (subs[count] == NULL)
            break;
        count++;
    }
    unlock(&mounts_lock);

    for (size_t i = 0; i < count; i++) {
        char sub_point[MAX_PATH];
        int n = snprintf(sub_point, sizeof(sub_point), "%s%s", point, subs[i] + src_len);
        if (n > 0 && (size_t) n < sizeof(sub_point))
            do_bind_mount(subs[i], sub_point, info, flags);
        free(subs[i]);
    }
    free(subs);
}

dword_t sys_mount_guest(guest_addr_t source_addr, guest_addr_t point_addr, guest_addr_t type_addr, dword_t flags, guest_addr_t data_addr) {
    // Linux requires CAP_SYS_ADMIN for every door into the mount table.
    // The app's own boot-time mounts go through do_mount() directly and are
    // unaffected; this gate is only on the guest syscall path.
    if (!current_capable(CAP_SYS_ADMIN_))
        return _EPERM;
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
    // A bind is exempt from the directory requirement: Linux allows binding a
    // single file over another file. Dir-onto-dir or non-dir-onto-non-dir;
    // the mixed cases are rejected in the bind branch once the source has
    // been stat'd too.
    if (!S_ISDIR(stat.mode) && !(flags & MS_BIND_))
        return _ENOTDIR;

    char point[MAX_PATH];
    err = path_normalize(AT_PWD, point_raw, point, N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;

    // MS_MOVE and MS_BIND name an existing path as their "source"; resolve it
    // before taking mounts_lock (path_normalize / find_mount_and_trim_path
    // re-enter mount_find, which takes the lock).
    char op_source[MAX_PATH];
    // MS_REMOUNT names an existing mount by its POINT and ignores the source
    // -- callers pass NULL for it -- so resolving one would be resolving
    // nothing. MS_REMOUNT|MS_BIND is the common shape here: it means "change
    // the flags on the bind mounted at this point", not "make a bind".
    if ((flags & (MS_MOVE_ | MS_BIND_)) && !(flags & MS_REMOUNT_)) {
        err = path_normalize(AT_PWD, source, op_source, N_SYMLINK_FOLLOW);
        if (err < 0)
            return err;
    }

    // A bind shares the source mount's backing; do_bind_mount resolves the source
    // and takes mounts_lock itself, so dispatch it before we lock here.
    //
    // Not when MS_REMOUNT is also set. `mount -o remount,bind,ro <point>` --
    // how every caller makes an existing bind read-only, and what a container
    // runtime does for each of its read-only bind mounts -- took this branch
    // and tried to CREATE a bind from a source that was never given, so it
    // failed and the mount stayed writable. It belongs to the MS_REMOUNT
    // handling below, which changes the flags on the mount already at that
    // point.
    if ((flags & MS_BIND_) && !(flags & MS_REMOUNT_)) {
        struct statbuf source_stat;
        err = generic_statat(AT_PWD, source, &source_stat, 0);
        if (err < 0)
            return err;
        if (S_ISDIR(source_stat.mode) != S_ISDIR(stat.mode))
            return _ENOTDIR;
        err = do_bind_mount(op_source, point, data, flags & MS_FLAGS);
        if (err >= 0) {
            if (flags & MS_REC_)
                bind_replicate_submounts(op_source, point, data, flags & MS_FLAGS);
            proc_mountinfo_notify_changed();
        }
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
        found->refcount++;  // for mount_notify_relocated
        unlock(&mounts_lock);
        mount_notify_relocated(found, op_source, point);
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
    // libfuse mounts with a subtype ("fuse.sshfs", "fuse.rclone"); the part
    // after the dot only names the daemon for mtab display.
    if (fs == NULL && strncmp(type, "fuse.", 5) == 0)
        fs = &fusefs;
    // binfmt_misc is not a mount in AOK's table: the directory already exists
    // in procfs and mounting it brings `register` and `status` into being, the
    // way Linux does. So this accepts the mount and flips the flag procfs
    // reads, rather than creating a mount.
    //
    // This used to accept the mount and do NOTHING, which was unreachable in
    // practice -- mount(8) reads /proc/filesystems first and refused the type
    // before ever calling mount(2) -- but would have handed a direct mount(2)
    // caller (systemd does this) a successful mount of an empty directory, a
    // state Linux cannot produce.
    if (fs == NULL &&
            strcmp(point, "/proc/sys/fs/binfmt_misc") == 0 &&
            (strcmp(type, "binfmt_misc") == 0 ||
             strcmp(source, "binfmt_misc") == 0 ||
             strcmp(source, "none") == 0)) {
        binfmt_misc_set_mounted(true);
        unlock(&mounts_lock);
        proc_mountinfo_notify_changed();
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
    // Linux requires CAP_SYS_ADMIN for every door into the mount table.
    // The app's own boot-time mounts go through do_mount() directly and are
    // unaffected; this gate is only on the guest syscall path.
    if (!current_capable(CAP_SYS_ADMIN_))
        return _EPERM;
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
    err = do_umount_flags(target, (flags & MNT_DETACH_) != 0);
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
static struct mount *mount_at_point_locked(const char *point);

struct fscontext_data {
    const struct fs_ops *fs;
    bool created;
    bool readonly;
    // binfmt_misc is not a mount in AOK's table -- the directory already exists
    // in procfs and "mounting" it brings `register` and `status` into being
    // (fs/proc/sys.c). It still has to be reachable through the NEW mount API,
    // because util-linux 2.41's libmount tries fsopen FIRST and only falls back
    // to mount(2) on ENOSYS: an ENODEV from fsopen made it report "unknown
    // filesystem type" and never call mount(2) at all, so the working mount(2)
    // path below was dead for every caller that used the tool.
    bool binfmt_misc;
    char point[MAX_PATH];
};

static int fscontext_close(struct fd *fd) {
    free(fd->data);
    return 0;
}

static struct fd_ops fscontext_ops = {
    .name = "fscontext",
    .anon_inode_class = "[fscontext]",
    .close = fscontext_close,
};

#define FSOPEN_CLOEXEC_ 1

fd_t sys_fsopen_guest(guest_addr_t fsname_addr, dword_t flags) {
    // Linux requires CAP_SYS_ADMIN for every door into the mount table.
    // The app's own boot-time mounts go through do_mount() directly and are
    // unaffected; this gate is only on the guest syscall path.
    if (!current_capable(CAP_SYS_ADMIN_))
        return _EPERM;
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
    bool is_binfmt = fs == NULL && strcmp(fsname, "binfmt_misc") == 0;
    if (fs == NULL && !is_binfmt)
        return _ENODEV;

    struct fscontext_data *data = malloc(sizeof(struct fscontext_data));
    if (data == NULL)
        return _ENOMEM;
    *data = (struct fscontext_data) {.fs = fs, .binfmt_misc = is_binfmt};
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

// Option-name vocabularies for fsconfig().
//
// An unknown option has to be REFUSED, not swallowed. systemd decides whether a
// filesystem supports an option by asking (mount_option_supported() in
// src/basic/mountpoint-util.c), and it opens with a canary: FSCONFIG_SET_FD with
// the key "adefinitelynotexistingmountoption". A kernel whose fsconfig never
// fails is exactly what that canary is looking for, and finding one makes
// systemd give up on the whole question with EAGAIN -- "FSCONFIG_SET_FD worked
// unexpectedly for '%s', whoa!". That is where an openSUSE guest's "Unable to
// determine whether tmpfs supports 'usrquota' mount option, assuming not:
// Resource temporarily unavailable" came from, and the cgroupfs
// 'memory_hugetlb_accounting' one beside it.
//
// This is a check on the option's NAME, not a claim that AOK implements its
// effect -- AOK still models only "ro", and everything else here is accepted and
// ignored exactly as before. The vocabulary is the one a stock Linux build of
// the same filesystem parses, which is the question the caller is really asking:
// "would a kernel reject this spelling?". So size= and mode= on tmpfs still
// work, which systemd's own tmpfs setup needs, while usrquota (a CONFIG_TMPFS_
// QUOTA option, refused on a stock kernel -- measured on Linux 6.12) and
// outright nonsense are refused.
//
// A filesystem with no vocabulary here stays permissive. That keeps AOK's own
// filesystems (realfs, aokfs, fakefs, binfmt_misc) exactly as they were, so the
// tightening reaches only the filesystems whose real option sets are written
// down below.
static const char *const fsopt_generic[] = {
    "ro", "rw", "sync", "dirsync", "nosuid", "nodev", "noexec", "noatime",
    "nodiratime", "relatime", "strictatime", "lazytime", "silent", "source",
    NULL,
};
static const char *const fsopt_tmpfs[] = {
    "size", "nr_blocks", "nr_inodes", "mode", "uid", "gid", "huge", "mpol",
    "noswap", NULL,
};
static const char *const fsopt_proc[] = {"hidepid", "gid", "subset", NULL};
static const char *const fsopt_sysfs[] = {NULL};
static const char *const fsopt_devpts[] = {
    "uid", "gid", "mode", "ptmxmode", "newinstance", "max", NULL,
};
static const char *const fsopt_cgroup2[] = {
    "nsdelegate", "favordynmods", "memory_localevents", "memory_recursiveprot",
    "memory_hugetlb_accounting", "pids_localevents", NULL,
};
static const char *const fsopt_cgroup[] = {
    "none", "all", "noprefix", "xattr", "release_agent", "name",
    "clone_children", "cpu", "cpuacct", "cpuset", "memory", "devices",
    "freezer", "net_cls", "net_prio", "blkio", "perf_event", "hugetlb",
    "pids", "rdma", "misc", NULL,
};
static const char *const fsopt_fuse[] = {
    "fd", "rootmode", "user_id", "group_id", "default_permissions",
    "allow_other", "max_read", "blksize", NULL,
};

static const struct {
    const char *fs;
    const char *const *names;
} fsopt_vocab[] = {
    {"tmpfs", fsopt_tmpfs},
    {"devtmpfs", fsopt_tmpfs},   // a devtmpfs IS a tmpfs
    {"proc", fsopt_proc},
    {"sysfs", fsopt_sysfs},
    {"devpts", fsopt_devpts},
    {"cgroup2", fsopt_cgroup2},
    {"cgroup", fsopt_cgroup},
    {"fuse", fsopt_fuse},
};

static bool fsopt_listed(const char *const *names, const char *key) {
    for (size_t i = 0; names[i] != NULL; i++)
        if (strcmp(names[i], key) == 0)
            return true;
    return false;
}

// True when this option name is one the filesystem would parse. Permissive for
// anything with no vocabulary, and for an empty key (a caller asking nothing).
static bool fscontext_option_known(const struct fscontext_data *data, const char *key) {
    if (key[0] == '\0')
        return true;
    if (fsopt_listed(fsopt_generic, key))
        return true;
    if (data->fs == NULL || data->fs->name == NULL)
        return true;  // binfmt_misc and anything else without a table
    for (size_t i = 0; i < sizeof(fsopt_vocab) / sizeof(fsopt_vocab[0]); i++) {
        if (strcmp(fsopt_vocab[i].fs, data->fs->name) != 0)
            continue;
        // The fuse vocabulary also covers the fuseblk spelling, which shares it.
        return fsopt_listed(fsopt_vocab[i].names, key);
    }
    return true;  // no vocabulary written down for this filesystem
}

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
            // A name this filesystem would not parse is refused, the way Linux
            // refuses it; see fscontext_option_known. We still only ACT on the
            // one option this codebase's mount model has an equivalent for
            // ("ro") -- size=, mode=, SELinux context= and the rest are
            // accepted and ignored, the existing "don't model X" precedent for
            // mount options AOK has no backing concept for (see do_mount's
            // MS_IGNORED).
            if (!fscontext_option_known(data, key))
                return _EINVAL;
            if (strcmp(key, "ro") == 0)
                data->readonly = true;
            return 0;
        case FSCONFIG_SET_BINARY_:
        case FSCONFIG_SET_PATH_:
        case FSCONFIG_SET_PATH_EMPTY_:
        case FSCONFIG_SET_FD_:
            // AOK has no parameter of any of these kinds -- no filesystem here
            // takes a blob, a path or a descriptor through fsconfig -- so every
            // key is an unknown one, which is EINVAL. Linux answers the same for
            // a filesystem with no such parameter, and systemd's
            // mount_option_supported() canary reads exactly this: it wants
            // EINVAL here to conclude the filesystem is converted to the new
            // mount API and its later answers can be believed.
            return _EINVAL;
        case FSCONFIG_CMD_CREATE_: {
            if (data->created)
                return _EBUSY;
            static _Atomic unsigned next_fscontext_id = 0;
            snprintf(data->point, sizeof(data->point), "/.ish-fsmount/%u",
                    next_fscontext_id++);
            // generic_mkdirat resolves its path via mount_find, which takes
            // mounts_lock itself -- must run before we take the lock below,
            // or this self-deadlocks (mounts_lock isn't recursive).
            if (data->binfmt_misc) {
                // Nothing to stage. This is where Linux's fs_context creates
                // the superblock, and for binfmt_misc creating it IS the whole
                // effect -- `register` and `status` come into being. move_mount
                // then has nothing left to do but name where it already is.
                binfmt_misc_set_mounted(true);
                proc_mountinfo_notify_changed();
                data->created = true;
                return 0;
            }
            int mkerr = generic_mkdirat(AT_PWD, "/.ish-fsmount", 0700);
            if (mkerr < 0 && mkerr != _EEXIST)
                return mkerr;
            lock(&mounts_lock, 0);
            int err = do_mount(data->fs, "", data->point, "",
                    data->readonly ? MS_READONLY_ : 0);
            if (err >= 0) {
                // Detached until move_mount places it; see struct mount.
                struct mount *staged = mount_at_point_locked(data->point);
                if (staged != NULL)
                    staged->detached = true;
            }
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
    // Linux requires CAP_SYS_ADMIN for every door into the mount table.
    // The app's own boot-time mounts go through do_mount() directly and are
    // unaffected; this gate is only on the guest syscall path.
    if (!current_capable(CAP_SYS_ADMIN_))
        return _EPERM;
    STRACE("fsmount(%d, %#x, %#x)", f, flags, attr_flags);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &fscontext_ops)
        return _EINVAL;
    struct fscontext_data *data = fd->data;
    if (!data->created)
        return _EINVAL;

    if (data->binfmt_misc) {
        // There is no staged mount to hand back: fsconfig(CREATE) above already
        // did the whole of what "mounting" binfmt_misc means. Return a
        // directory fd on the real thing, which is what move_mount will be
        // given and what /proc/self/fdinfo would show.
        struct fd *dirfd = generic_open_realroot("/proc/sys/fs/binfmt_misc",
                O_RDONLY_ | O_DIRECTORY_ | O_CLOEXEC_, 0);
        if (IS_ERR(dirfd))
            return PTR_ERR(dirfd);
        return f_install(dirfd, O_CLOEXEC_);
    }

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
// Caller must hold mounts_lock.
static struct mount *mount_at_point_locked(const char *point) {
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (strcmp(mount->point, point) == 0)
            return mount;
    }
    return NULL;
}

// Clear the detached mark once a mount has a real mountpoint: from here on it
// belongs in /proc/mounts and mountinfo like any other.
static void mount_mark_attached(const char *point) {
    lock(&mounts_lock, 0);
    struct mount *mount = mount_at_point_locked(point);
    if (mount != NULL)
        mount->detached = false;
    unlock(&mounts_lock);
}

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
    found->refcount++;  // for mount_notify_relocated
    unlock(&mounts_lock);
    mount_notify_relocated(found, from_point, to_point);
    return 0;
}

#define MOVE_MOUNT_F_EMPTY_PATH_ 0x00000004

dword_t sys_move_mount_guest(fd_t from_dfd, guest_addr_t from_path_addr, fd_t to_dfd,
        guest_addr_t to_path_addr, dword_t flags) {
    // Linux requires CAP_SYS_ADMIN for every door into the mount table.
    // The app's own boot-time mounts go through do_mount() directly and are
    // unaffected; this gate is only on the guest syscall path.
    if (!current_capable(CAP_SYS_ADMIN_))
        return _EPERM;
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

    // binfmt_misc has already arrived: fsconfig(CMD_CREATE) brought `register`
    // and `status` into being, and there is no mount object to relocate. The
    // fd fsmount handed back is a plain directory fd on the target itself, so
    // the move is a no-op -- succeed rather than failing after the effect has
    // happened, which is what left mount(8) reporting "wrong fs type" over a
    // directory that had in fact just been populated.
    if (strcmp(from_point, "/proc/sys/fs/binfmt_misc") == 0 &&
            strcmp(to_point, "/proc/sys/fs/binfmt_misc") == 0 &&
            binfmt_misc_is_mounted())
        return 0;

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
    if (err >= 0) {
        mount_mark_attached(to_point);
        proc_mountinfo_notify_changed();
    }
    return err;
}

dword_t sys_move_mount(fd_t from_dfd, addr_t from_path_addr, fd_t to_dfd, addr_t to_path_addr,
        dword_t flags) {
    return sys_move_mount_guest(from_dfd, from_path_addr, to_dfd, to_path_addr, flags);
}
