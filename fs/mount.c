#include <string.h>
#include <sys/stat.h>
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/path.h"
#include "fs/real.h"
#include "fs/dev.h"
#include "kernel/binfmt_misc.h"
#include "kernel/mnt_ns.h"
#include "kernel/user_ns.h"
#include "kernel/ipc_ns.h" // ns_alloc_inum

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
static void mount_leave_group_locked(struct mount *mount);

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
const char *mount_listed_point(const struct mount *mount) {
    if (mount->covered_by != 0 && mount->covered_point != NULL && !mount->lazy_umount)
        return mount->covered_point;
    if (mount->stacked_point != NULL && !mount->detached && !mount->lazy_umount)
        return mount->stacked_point;
    return mount->point;
}

int mount_id(struct mount *mount) {
    return mount->id > 0 ? mount->id : MOUNT_ID_HIDDEN;
}

// Re-express `path`, relative to `origin`, as it is seen through `bind`, a
// bind of `origin` that the caller holds a reference on (a descriptor's
// bind_mount): the bind's point, then what follows the bind's source in
// `path`. In place; `path` is a MAX_PATH buffer. False, leaving `path` alone,
// when `path` is no longer under the source because the file was renamed out
// from under it.
//
// A bind that has been lazily unmounted is out of the mount table, so no path
// reaches it. With `detached` NULL that is false too, and the caller keeps
// the path on the origin, which still does. Otherwise the path is from the
// bind's own root -- what Linux prints for a file in a detached mount -- and
// *detached is set.
//
// Under mounts_lock: MS_MOVE replaces bind->point, and umount -l marks the
// bind. bind_prefix is fixed for the bind's life.
bool mount_path_through_bind(struct mount *bind, const struct mount *origin, char *path,
                             bool *detached) {
    if (bind->bind_origin != origin)
        return false;
    size_t prefix_len = strlen(bind->bind_prefix);
    if (strncmp(path, bind->bind_prefix, prefix_len) != 0 ||
            (path[prefix_len] != '\0' && path[prefix_len] != '/'))
        return false;
    bool done = false;
    lock(&mounts_lock, 0);
    bool gone = bind->lazy_umount;
    if (!gone || detached != NULL) {
        size_t point_len = gone ? 0 : bind->point_len;
        size_t rest_len = strlen(path + prefix_len);
        if (point_len + rest_len < MAX_PATH) {
            memmove(path + point_len, path + prefix_len, rest_len + 1);
            memcpy(path, bind->point, point_len);
            if (detached != NULL)
                *detached = gone;
            done = true;
        }
    }
    unlock(&mounts_lock);
    return done;
}

// Put the point of the mount with ID `id` in front of `path`, a path on that
// mount, in place; `path` is a MAX_PATH buffer. False, leaving `path` alone,
// when no mount in the table has that ID any more, when it is detached, or
// when the result would not fit. Takes mounts_lock.
bool mount_path_by_id(int id, char *path) {
    bool done = false;
    lock(&mounts_lock, 0);
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (mount->id != id)
            continue;
        // Detached: its point is a staging point no restore will have.
        if (mount->detached)
            break;
        size_t len = strlen(path);
        if (mount->point_len + len < MAX_PATH) {
            memmove(path + mount->point_len, path, len + 1);
            memcpy(path, mount->point, mount->point_len);
            done = true;
        }
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
    return mounts_listed_locked_covered(count_out, false);
}

// With `covered`, the mounts that are covered too, as Linux lists them in
// /proc/<pid>/mounts and mountinfo: mounted, and out of sight under another
// (kernel/fs.h). mount_listed_point says where each is.
struct mount **mounts_listed_locked_covered(size_t *count_out, bool covered) {
    size_t count = 0;
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts)
        if (!mount->detached || (covered && mount->covered_by != 0 && mount->covered_point != NULL && !mount->lazy_umount))
            count++;
    struct mount **listed = malloc((count > 0 ? count : 1) * sizeof(*listed));
    if (listed == NULL)
        return NULL;
    size_t i = 0;
    list_for_each_entry(&mounts, mount, mounts)
        if (!mount->detached || (covered && mount->covered_by != 0 && mount->covered_point != NULL && !mount->lazy_umount))
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
static void mount_uncover_locked(const struct mount *top);

static void mount_destroy(struct mount *mount) {
    // What it covered is in view again (and, when a whole namespace is going,
    // is found there and taken in its turn).
    mount_uncover_locked(mount);
    // Its peers and slaves go on without it (see "Propagation").
    mount_leave_group_locked(mount);
    // A mount umount -l parked at a staging point goes as the mount it was:
    // its filesystem is told the point it was attached at. So does one that
    // was covered.
    if (mount->attached_point == NULL && mount->covered_point != NULL) {
        mount->attached_point = mount->covered_point;
        mount->covered_point = NULL;
    }
    free((void *) mount->covered_point);
    mount->covered_point = NULL;
    free((void *) mount->stacked_point);
    mount->stacked_point = NULL;
    if (mount->attached_point != NULL) {
        free((void *) mount->point);
        mount->point = mount->attached_point;
        mount->point_len = strlen(mount->point);
        mount->attached_point = NULL;
    }
    struct mount *origin = mount->bind_origin;
    if (origin != NULL) {
        // A bind owns one reference on its origin. We hold mounts_lock here
        // (do_umount / exit unmount), so drop it directly rather than via
        // mount_release, which would re-acquire the same non-recursive lock.
        origin->refcount--;
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
    // ...and if that was the last use of a lazily unmounted origin, finish
    // its unmount, as mount_release would have. A bind now lives as long as
    // anything is open in it, so it can be the last user.
    if (origin != NULL && origin->lazy_umount && origin->refcount == 0)
        mount_destroy(origin);
}

int mount_remove(struct mount *mount) {
    if (mount->refcount != 0)
        return _EBUSY;
    mount_destroy(mount);
    return 0;
}

// One counter for every staging point, fsmount()'s and umount -l's, so no two
// detached mounts are ever parked at the same one.
static _Atomic unsigned next_staging_id = 0;

static void mount_staging_point(char *buf, size_t size) {
    snprintf(buf, size, MOUNT_STAGING_DIR "/%u", next_staging_id++);
}

// Put `mount` into `mounts` where it belongs: the list is kept longest point
// first, so mount_find's first match is the deepest mount. Caller holds
// mounts_lock.
static void mount_list_insert(struct mount *mount) {
    struct mount *after;
    list_for_each_entry(&mounts, after, mounts) {
        if (after->point_len <= mount->point_len)
            break;
    }
    list_add_before(&after->mounts, &mount->mounts);
}

// ---- Covered mounts (struct mount's covered_by, kernel/fs.h) ---------------

// Are two points in one mount tree? Each namespace's tree is under a staging
// point of its own, and the initial one's is under none.
static bool mount_points_same_tree(const char *a, size_t a_len, const char *b, size_t b_len) {
    size_t tree_a = mount_staging_point_len(a, a_len), tree_b = mount_staging_point_len(b, b_len);
    return tree_a == tree_b && memcmp(a, b, tree_a) == 0;
}

#define MOUNT_COVER_ALL 0x7fffffff

// `top` has just been mounted: every mount below its point with an ID under
// `older_than` is underneath it now, and out of view. (The limit is for a
// recursive bind, whose copies of the submounts are made below it and are
// part of it; they are newer than it is.) Caller holds mounts_lock.
static void mount_cover_below_locked(struct mount *top, int older_than) {
    bool again = true;
    while (again) {
        again = false;
        struct mount *below;
        list_for_each_entry(&mounts, below, mounts) {
            if (below == top || below->detached || below->lazy_umount || below->id >= older_than)
                continue;
            if (below->point_len <= top->point_len ||
                    strncmp(below->point, top->point, top->point_len) != 0 ||
                    below->point[top->point_len] != '/')
                continue;
            if (!mount_points_same_tree(below->point, below->point_len, top->point, top->point_len))
                continue;
            char staging[MAX_PATH];
            mount_staging_point(staging, sizeof(staging));
            char *point = strdup(staging);
            if (point == NULL)
                return;
            below->covered_point = below->point;
            below->covered_by = top->id;
            below->point = point;
            below->point_len = strlen(point);
            below->detached = true;
            list_remove(&below->mounts);
            mount_list_insert(below);
            again = true;
            break;
        }
    }
}

// The mount on top at `point`, covering what is below it. Caller holds
// mounts_lock.
static void mount_cover_at_locked(const char *point, bool keep_newer) {
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (!mount->detached && !mount->lazy_umount && strcmp(mount->point, point) == 0) {
            mount_cover_below_locked(mount, keep_newer ? mount->id : MOUNT_COVER_ALL);
            return;
        }
    }
}

// `top` is leaving its point: what it covered is where it was again. Not one
// that was unmounted in the meantime, which stays parked until its last user
// lets go. Caller holds mounts_lock.
static void mount_uncover_locked(const struct mount *top) {
    if (top->id <= 0)
        return;
    bool again = true;
    while (again) {
        again = false;
        struct mount *under;
        list_for_each_entry(&mounts, under, mounts) {
            if (under->covered_by != top->id)
                continue;
            under->covered_by = 0;
            if (under->lazy_umount || under->covered_point == NULL)
                continue;
            free((void *) under->point);
            under->point = under->covered_point;
            under->point_len = strlen(under->point);
            under->covered_point = NULL;
            under->detached = false;
            list_remove(&under->mounts);
            mount_list_insert(under);
            again = true;
            break;
        }
    }
}

// ---- Moving a mount with what is mounted in it -----------------------------
//
// MS_MOVE and move_mount move a tree: the mount and every mount in view below
// its point, which are mounted IN it and go where it goes. They used to move
// the one mount, and left its submounts behind at paths that no longer led
// anywhere.
//
// In three steps, because the mount itself uncovers what it was over and
// covers what it lands on in between, and neither of those is its submounts'
// business: collect (which takes them out of view), then the caller moves
// the top, then place. Caller holds mounts_lock for the first two.
struct mount_subtree {
    size_t n;
    struct mount **subs;
    char **points;      // where each goes; then, for the notice, where it was
};

static void mount_subtree_free(struct mount_subtree *tree) {
    for (size_t i = 0; tree->points != NULL && i < tree->n; i++)
        free(tree->points[i]);
    free(tree->points);
    free(tree->subs);
    *tree = (struct mount_subtree) {};
}

static bool mount_point_below(const char *point, const char *top, size_t top_len) {
    return strncmp(point, top, top_len) == 0 && point[top_len] == '/';
}

static int mount_subtree_collect_locked(const struct mount *top, const char *old_point,
        const char *new_point, struct mount_subtree *tree) {
    *tree = (struct mount_subtree) {};
    size_t old_len = strlen(old_point), new_len = strlen(new_point);
    struct mount *mount;
    size_t n = 0;
    list_for_each_entry(&mounts, mount, mounts)
        if (mount != top && !mount->detached && !mount->lazy_umount &&
                mount_point_below(mount->point, old_point, old_len))
            n++;
    if (n == 0)
        return 0;
    tree->subs = calloc(n, sizeof(*tree->subs));
    tree->points = calloc(n, sizeof(*tree->points));
    if (tree->subs == NULL || tree->points == NULL) {
        mount_subtree_free(tree);
        return _ENOMEM;
    }
    list_for_each_entry(&mounts, mount, mounts) {
        if (mount == top || mount->detached || mount->lazy_umount ||
                !mount_point_below(mount->point, old_point, old_len))
            continue;
        const char *rest = mount->point + old_len;
        size_t len = new_len + strlen(rest);
        char *point = len < MAX_PATH ? malloc(len + 1) : NULL;
        if (point == NULL) {
            tree->n++;  // (so the free below reaches every point made so far)
            mount_subtree_free(tree);
            return len < MAX_PATH ? _ENOMEM : _ENAMETOOLONG;
        }
        snprintf(point, len + 1, "%s%s", new_point, rest);
        tree->subs[tree->n] = mount;
        tree->points[tree->n] = point;
        tree->n++;
    }
    // Out of view while the top moves: what it covers where it lands is what
    // was there, not these.
    for (size_t i = 0; i < tree->n; i++)
        tree->subs[i]->detached = true;
    return 0;
}

static void mount_subtree_place_locked(struct mount_subtree *tree, const char *old_point,
        const char *new_point) {
    size_t old_len = strlen(old_point), new_len = strlen(new_point);
    // A mount one of them covers comes back, when it does, below the tree's
    // new point. (Best effort, as in pivot_root: one whose point cannot be
    // had stays parked.)
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (mount->covered_by == 0 || mount->covered_point == NULL ||
                !mount_point_below(mount->covered_point, old_point, old_len))
            continue;
        bool ours = false;
        for (size_t i = 0; i < tree->n && !ours; i++)
            ours = tree->subs[i]->id == mount->covered_by;
        if (!ours)
            continue;
        const char *rest = mount->covered_point + old_len;
        size_t len = new_len + strlen(rest);
        char *point = len < MAX_PATH ? malloc(len + 1) : NULL;
        if (point != NULL)
            snprintf(point, len + 1, "%s%s", new_point, rest);
        free((void *) mount->covered_point);
        mount->covered_point = point;
    }
    for (size_t i = 0; i < tree->n; i++) {
        struct mount *sub = tree->subs[i];
        char *was = (char *) sub->point;
        sub->point = tree->points[i];
        sub->point_len = strlen(sub->point);
        tree->points[i] = was;
        sub->detached = false;
        list_remove(&sub->mounts);
        mount_list_insert(sub);
        sub->refcount++;  // for mount_notify_relocated
    }
}

// After the unlock: each filesystem hears where it went.
static void mount_subtree_notify(struct mount_subtree *tree) {
    for (size_t i = 0; i < tree->n; i++) {
        char now[MAX_PATH];
        lock(&mounts_lock, 0);
        snprintf(now, sizeof(now), "%s", tree->subs[i]->point);
        unlock(&mounts_lock);
        mount_notify_relocated(tree->subs[i], tree->points[i], now);
    }
    mount_subtree_free(tree);
}

// ---- Propagation ------------------------------------------------------------
//
// Linux's shared subtrees. A mount is private, or in a peer group
// (shared_id), or a slave of one (master_id), or both; a mount made or
// unmounted under a member of a peer group is repeated at the same place
// under every other member, and under every slave of the group, and so on
// down through any slave that is itself shared. A bind of a shared mount is
// its peer, and so is a mount namespace's copy of one -- unless the
// namespace belongs to another user namespace, where the copy is a slave.
//
// "The same place" is by filesystem and path within it, which is all a
// mount is here: a bind of the same origin with a prefix (kernel/fs.h). A
// point under mount A is A's prefix plus the rest of the point; under a
// receiver that shows that directory -- whose own prefix is a prefix of it
// -- it is the receiver's point plus what is left.
//
// Groups are numbers handed out once. A slave names a group rather than a
// mount, so nothing has to be re-linked when a master goes: when the last
// member of a group leaves it, its slaves pass to that member's own master,
// as Linux hands them on.
//
// A mount that is covered (parked under the one mounted over it) receives
// all the same, as on Linux: what arrives is parked with it, under the same
// mount, and comes back into view when it does. Not here: mountinfo has no
// propagate_from.

static _Atomic int next_peer_group = 1;

static struct mnt_namespace *mnt_namespaces;    // all but the initial one; mounts_lock

static struct mount *mount_fs_origin(struct mount *mount) {
    return mount->bind_origin != NULL ? mount->bind_origin : mount;
}
static const char *mount_fs_prefix(const struct mount *mount) {
    return mount->bind_prefix != NULL ? mount->bind_prefix : "";
}
static bool mount_in_view(const struct mount *mount) {
    return !mount->detached && !mount->lazy_umount;
}
// Mounted, but under one mounted over it later (covered_by, kernel/fs.h):
// parked at a staging point, and still in its tree at covered_point.
static bool mount_is_covered(const struct mount *mount) {
    return mount->covered_by != 0 && mount->covered_point != NULL && !mount->lazy_umount;
}
// Where a mount is in its tree, covered or not.
static const char *mount_place(const struct mount *mount) {
    return mount_is_covered(mount) ? mount->covered_point : mount->point;
}

// What a mount's tree is under, if anything: the ID of the mount covering
// it, or for one in view the one stacked on top of it at its own point (the
// table keeps both in view there, the newer answering). 0 for a mount that
// is on top. What is mounted under one that is not on top is out of sight
// as surely as under one that is parked.
static int mount_under_locked(const struct mount *mount) {
    if (mount_is_covered(mount))
        return mount->covered_by;
    struct mount *top;
    list_for_each_entry(&mounts, top, mounts) {
        if (mount_in_view(top) && strcmp(top->point, mount->point) == 0)
            return top == mount ? 0 : top->id;
    }
    return 0;
}

// The namespace whose tree `point` is in: NULL for one in no tree.
static struct mnt_namespace *mnt_ns_of_point_locked(const char *point) {
    if (mount_staging_point_len(point, strlen(point)) == 0)
        return &init_mnt_ns;
    for (struct mnt_namespace *ns = mnt_namespaces; ns != NULL; ns = ns->next_ns)
        if (mnt_ns_has_point(ns, point))
            return ns;
    return NULL;
}

// What `child` is mounted on: the mount directly beneath it at its own
// point, or the one whose tree its point is in. NULL for the root of a tree.
// (The list keeps equal points newest first and one length together.)
static struct mount *mount_parent_locked(const struct mount *child) {
    for (struct list *item = child->mounts.next; item != &mounts; item = item->next) {
        struct mount *mount = list_entry(item, struct mount, mounts);
        if (mount->point_len != child->point_len)
            break;
        if (mount_in_view(mount) && strcmp(mount->point, child->point) == 0)
            return mount;
    }
    size_t tree = mount_staging_point_len(child->point, child->point_len);
    if (tree == child->point_len && child->point_len != 0)
        return NULL;
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (!mount_in_view(mount) || mount->point_len >= child->point_len)
            continue;
        if (mount_staging_point_len(mount->point, mount->point_len) != tree)
            continue;
        if (strncmp(child->point, mount->point, mount->point_len) == 0 &&
                child->point[mount->point_len] == '/')
            return mount;
    }
    return NULL;
}

// Out of its peer group. If that leaves the group empty, its slaves are the
// slaves of whatever this mount was a slave of.
static void mount_leave_group_locked(struct mount *mount) {
    int group = mount->shared_id;
    if (group == 0)
        return;
    mount->shared_id = 0;
    struct mount *other;
    list_for_each_entry(&mounts, other, mounts)
        if (other->shared_id == group)
            return;
    list_for_each_entry(&mounts, other, mounts)
        if (other->master_id == group)
            other->master_id = mount->master_id;
}

// mount --make-{shared,slave,private,unbindable} for one mount.
static void mount_set_propagation_locked(struct mount *mount, int type) {
    switch (type) {
        case MS_SHARED_:
            if (mount->shared_id == 0)
                mount->shared_id = next_peer_group++;
            break;
        case MS_SLAVE_: {
            // A slave of the peers it leaves; with none, of what it was a
            // slave of already.
            int group = mount->shared_id;
            mount_leave_group_locked(mount);
            struct mount *other;
            list_for_each_entry(&mounts, other, mounts) {
                if (group != 0 && other->shared_id == group) {
                    mount->master_id = group;
                    break;
                }
            }
            break;
        }
        case MS_PRIVATE_:
        case MS_UNBINDABLE_:
            mount_leave_group_locked(mount);
            mount->master_id = 0;
            break;
    }
    mount->unbindable = type == MS_UNBINDABLE_;
}

// The mount at exactly `point` in the caller's tree gets propagation `type`,
// and with `recursive` so does every mount below it. EINVAL for a point that
// is not a mount point.
static int mount_change_propagation(const char *point, int type, bool recursive) {
    size_t point_len = strlen(point);
    lock(&mounts_lock, 0);
    struct mount *top = NULL, *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (mount_in_view(mount) && strcmp(mount->point, point) == 0) {
            top = mount;
            break;
        }
    }
    if (top == NULL) {
        unlock(&mounts_lock);
        return _EINVAL;
    }
    list_for_each_entry(&mounts, mount, mounts) {
        if (mount != top) {
            if (!recursive || !mount_in_view(mount))
                continue;
            if (strncmp(mount->point, point, point_len) != 0 || mount->point[point_len] != '/')
                continue;
            // (The initial root's "" is a prefix of every other tree's
            // points too, and those are not below it.)
            if (!mnt_ns_has_point(mnt_ns_current(), mount->point))
                continue;
        }
        mount_set_propagation_locked(mount, type);
    }
    unlock(&mounts_lock);
    proc_mountinfo_notify_changed();
    return 0;
}

// Where `point`, which is under `parent`, is under `receiver`. False when
// the receiver shows another filesystem, or a part of this one that the
// point is not in.
static bool mount_propagated_point(struct mount *parent, const char *point, struct mount *receiver,
        char *out, size_t size) {
    if (mount_fs_origin(parent) != mount_fs_origin(receiver))
        return false;
    char in_fs[MAX_PATH];
    int n = snprintf(in_fs, sizeof(in_fs), "%s%s", mount_fs_prefix(parent), point + parent->point_len);
    if (n < 0 || (size_t) n >= sizeof(in_fs))
        return false;
    const char *shown = mount_fs_prefix(receiver);
    size_t shown_len = strlen(shown);
    if (strncmp(in_fs, shown, shown_len) != 0 || (in_fs[shown_len] != '/' && in_fs[shown_len] != '\0'))
        return false;
    n = snprintf(out, size, "%s%s", mount_place(receiver), in_fs + shown_len);
    return n >= 0 && (size_t) n < size;
}

// Everything that receives what happens under `parent`: the other members
// of its peer group, the slaves of that group, the peers of any slave that
// is shared as well, their slaves, and on down. For a mount being
// propagated, each comes with what its copy is to be: `shared` and `master`
// (0 for none), given the mount's own group and master. For an unmount
// (`mount_group` 0) those are not asked for, and no group is made.
struct propagation_target {
    struct mount *receiver;
    int shared, master;
};

static struct propagation_target *mount_propagation_targets_locked(struct mount *parent,
        int mount_group, int mount_master, size_t *count_out) {
    *count_out = 0;
    if (parent == NULL || parent->shared_id == 0)
        return NULL;
    struct { int group, copy_group, copy_master; } levels[32];
    size_t n_levels = 1, count = 0, cap = 0;
    levels[0].group = parent->shared_id;
    levels[0].copy_group = mount_group;
    levels[0].copy_master = mount_master;
    struct propagation_target *targets = NULL;
    for (size_t i = 0; i < n_levels; i++) {
        struct mount *mount;
        list_for_each_entry(&mounts, mount, mounts) {
            if (mount == parent || (!mount_in_view(mount) && !mount_is_covered(mount)))
                continue;
            int shared, master;
            if (mount->shared_id == levels[i].group) {
                shared = levels[i].copy_group;
                master = levels[i].copy_master;
            } else if (mount->master_id == levels[i].group && mount->shared_id == 0) {
                shared = 0;
                master = levels[i].copy_group;
            } else if (mount->master_id == levels[i].group) {
                // A slave that is shared: it and its peers are a level of
                // their own, whose copies share a group between them.
                bool seen = false;
                for (size_t k = 0; k < n_levels && !seen; k++)
                    seen = levels[k].group == mount->shared_id;
                if (!seen && n_levels < sizeof(levels) / sizeof(levels[0])) {
                    levels[n_levels].group = mount->shared_id;
                    levels[n_levels].copy_group = mount_group != 0 ? next_peer_group++ : 0;
                    levels[n_levels].copy_master = levels[i].copy_group;
                    n_levels++;
                }
                continue;
            } else {
                continue;
            }
            if (count == cap) {
                size_t new_cap = cap == 0 ? 8 : cap * 2;
                struct propagation_target *grown = realloc(targets, new_cap * sizeof(*targets));
                if (grown == NULL)
                    break;
                targets = grown;
                cap = new_cap;
            }
            targets[count++] = (struct propagation_target) {mount, shared, master};
        }
    }
    *count_out = count;
    return targets;
}

// `mount` again at `point`: a bind of the same filesystem showing the same
// part of it. Caller holds mounts_lock.
static struct mount *mount_clone_at_locked(struct mount *mount, const char *point) {
    struct mount *origin = mount_fs_origin(mount);
    // calloc for the same reason as do_mount.
    struct mount *copy = calloc(1, sizeof(struct mount));
    if (copy == NULL)
        return NULL;
    copy->point = strdup(point);
    copy->point_len = strlen(point);
    copy->source = strdup(mount->source != NULL ? mount->source : "");
    copy->display_source = mount->display_source != NULL ? strdup(mount->display_source) : NULL;
    copy->info = strdup(mount->info != NULL ? mount->info : "");
    copy->bind_prefix = strdup(mount_fs_prefix(mount));
    if (copy->point == NULL || copy->source == NULL || copy->info == NULL || copy->bind_prefix == NULL ||
            (mount->display_source != NULL && copy->display_source == NULL)) {
        free((void *) copy->point);
        free((void *) copy->source);
        free((void *) copy->display_source);
        free((void *) copy->info);
        free((void *) copy->bind_prefix);
        free(copy);
        return NULL;
    }
    copy->flags = mount->flags;
    copy->fs = origin->fs;
    copy->root_fd = -1;
    copy->fake_dev = origin->fake_dev;
    copy->bind_origin = origin;
    origin->refcount++; // a bind owns one reference on its origin (mount_destroy)
    copy->id = mount_new_id();
    mount_list_insert(copy);
    return copy;
}

// `mount` has just been attached somewhere under `parent`: it appears at the
// same place under everything that receives from `parent`. As on Linux the
// mount itself becomes shared for it, if it was not.
static void mount_propagate_locked(struct mount *parent, struct mount *mount) {
    if (parent == NULL || parent->shared_id == 0 || !mount_in_view(mount))
        return;
    if (mount->shared_id == 0)
        mount->shared_id = next_peer_group++;
    mount->unbindable = false;
    size_t count;
    struct propagation_target *targets = mount_propagation_targets_locked(parent,
            mount->shared_id, mount->master_id, &count);
    struct mnt_namespace *from_ns = mnt_ns_of_point_locked(mount->point);
    for (size_t i = 0; i < count; i++) {
        char point[MAX_PATH];
        if (!mount_propagated_point(parent, mount->point, targets[i].receiver, point, sizeof(point)))
            continue;
        if (strcmp(point, mount->point) == 0)
            continue;
        // Under a covered receiver: the copy is covered too, by what covers
        // the receiver, at the place it will have when that goes.
        int under = mount_under_locked(targets[i].receiver);
        bool covered = under != 0;
        char staging[MAX_PATH];
        if (covered)
            mount_staging_point(staging, sizeof(staging));
        struct mount *copy = mount_clone_at_locked(mount, covered ? staging : point);
        if (copy == NULL)
            continue;
        if (covered) {
            copy->covered_point = strdup(point);
            copy->covered_by = under;
            copy->detached = true;
        }
        copy->shared_id = targets[i].shared;
        copy->master_id = targets[i].master;
        // In a tree that belongs to another user namespace it keeps the
        // restrictions it arrived with.
        struct mnt_namespace *to_ns = mnt_ns_of_point_locked(point);
        if (to_ns != NULL && from_ns != NULL && to_ns->user_ns != from_ns->user_ns)
            copy->locked_flags = mount->flags & (MS_READONLY_ | MS_NOSUID_ | MS_NODEV_ | MS_NOEXEC_);
        // It covers what was mounted below its point there.
        if (!covered)
            mount_cover_below_locked(copy, MOUNT_COVER_ALL);
    }
    free(targets);
}

// The mount at `point` has just been made by a guest or the app: propagate
// it, and with `newer_below` whatever was mounted under it with it (a
// recursive bind's copies), shallowest first.
static void mount_propagate_at_locked(const char *point, bool newer_below) {
    size_t point_len = strlen(point);
    struct mount *top = NULL, *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (mount_in_view(mount) && strcmp(mount->point, point) == 0) {
            top = mount;
            break;
        }
    }
    if (top == NULL)
        return;
    struct mount *parent = mount_parent_locked(top);
    if (parent == NULL || parent->shared_id == 0)
        return;
    // What is below it, before any copy is made: a copy may land below it
    // too (a bind into its own source's tree).
    size_t n = 0, cap = 0;
    struct mount **below = NULL;
    for (struct list *item = mounts.prev; newer_below && item != &mounts; item = item->prev) {
        mount = list_entry(item, struct mount, mounts);
        if (!mount_in_view(mount) || mount->id <= top->id || !mount_point_below(mount->point, point, point_len))
            continue;
        if (n == cap) {
            size_t new_cap = cap == 0 ? 8 : cap * 2;
            struct mount **grown = realloc(below, new_cap * sizeof(*below));
            if (grown == NULL)
                break;
            below = grown;
            cap = new_cap;
        }
        below[n++] = mount;
    }
    mount_propagate_locked(parent, top);
    for (size_t i = 0; i < n; i++)
        mount_propagate_locked(parent, below[i]);
    free(below);
}

// A mount that has just been moved to a new place, with what is mounted in
// it (already placed): the same, for a tree.
static void mount_propagate_tree_locked(struct mount *top, struct mount_subtree *tree) {
    struct mount *parent = mount_parent_locked(top);
    if (parent == NULL || parent->shared_id == 0)
        return;
    mount_propagate_locked(parent, top);
    // (The list was collected deepest first.)
    for (size_t i = tree->n; i-- > 0;)
        mount_propagate_locked(parent, tree->subs[i]);
}

// What an unmount of `mount` takes with it: the mount at the same place
// under each thing that receives from what it is mounted on, when that is
// the same filesystem showing the same part of it. The caller frees the
// list. Caller holds mounts_lock.
static struct mount **mount_propagated_twins_locked(struct mount *mount, size_t *count_out) {
    *count_out = 0;
    if (!mount_in_view(mount))
        return NULL;
    struct mount *parent = mount_parent_locked(mount);
    size_t n_targets, count = 0;
    struct propagation_target *targets = mount_propagation_targets_locked(parent, 0, 0, &n_targets);
    if (targets == NULL)
        return NULL;
    struct mount **twins = calloc(n_targets, sizeof(*twins));
    for (size_t i = 0; twins != NULL && i < n_targets; i++) {
        char point[MAX_PATH];
        if (!mount_propagated_point(parent, mount->point, targets[i].receiver, point, sizeof(point)))
            continue;
        bool covered = mount_under_locked(targets[i].receiver) != 0;
        struct mount *other;
        list_for_each_entry(&mounts, other, mounts) {
            // (Under a covered receiver, the copy parked with it.)
            if (covered ? !mount_is_covered(other) || strcmp(other->covered_point, point) != 0
                        : !mount_in_view(other) || strcmp(other->point, point) != 0)
                continue;
            // (The one on top there, and only if it is this mount again.)
            bool again = other != mount && mount_fs_origin(other) == mount_fs_origin(mount) &&
                strcmp(mount_fs_prefix(other), mount_fs_prefix(mount)) == 0;
            for (size_t k = 0; again && k < count; k++)
                again = twins[k] != other;
            if (again)
                twins[count++] = other;
            break;
        }
    }
    free(targets);
    *count_out = count;
    return twins;
}

static bool mount_has_submounts_locked(const struct mount *mount) {
    struct mount *below;
    list_for_each_entry(&mounts, below, mounts)
        if (below != mount && mount_in_view(below) &&
                mount_point_below(below->point, mount->point, mount->point_len) &&
                mount_points_same_tree(below->point, below->point_len, mount->point, mount->point_len))
            return true;
    return false;
}

// Take one mount out of the tree for umount -l, as the last step of
// mount_remove_lazy. Caller holds mounts_lock.
static void mount_detach_one(struct mount *mount) {
    if (mount->refcount == 0) {
        mount_destroy(mount);
        return;
    }
    mount_uncover_locked(mount);
    // Mounted nowhere: it gives and receives nothing more.
    mount_leave_group_locked(mount);
    mount->master_id = 0;
    mount->lazy_umount = true;
    // Already parked: fsmount() made it and nothing placed it.
    if (mount->detached)
        return;
    // A bind has no storage of its own, and a descriptor opened through it
    // holds its origin, which is still where it was: out of the table is
    // enough. Re-initialising the node keeps the later list_remove in
    // mount_destroy harmless. So is any mount the staging point cannot be
    // allocated for -- it is still out of every lookup, only no longer
    // reachable from inside itself either, as before there was a staging
    // point at all.
    char staging[MAX_PATH];
    mount_staging_point(staging, sizeof(staging));
    char *point = mount->bind_origin == NULL ? strdup(staging) : NULL;
    if (point == NULL) {
        list_remove(&mount->mounts);
        list_init(&mount->mounts);
        return;
    }
    // Anything else is still where descriptors, cwds and roots inside it
    // start their lookups from -- their path is its point plus the path
    // inside it -- so it moves to a staging point, which reaches it and
    // nothing else. It keeps its old point to go with: see mount_destroy.
    mount->attached_point = mount->point;
    mount->point = point;
    mount->point_len = strlen(point);
    mount->detached = true;
    list_remove(&mount->mounts);
    mount_list_insert(mount);
}

// Detach every mount below `point`, each on its own (mount_detach_one), and
// say whether there was any. Not one already detached: a staging point is
// under no other point, except under the root's "". Caller holds mounts_lock.
static bool mount_detach_below(const char *point) {
    size_t len = strlen(point);
    bool any = false;
    // Restart after each one: detaching a mount re-sorts or frees entries of
    // the list being walked, even ones other than itself (the last user of a
    // bind's origin takes the origin with it). Each pass takes one match out
    // of the running, so this ends.
    bool again = true;
    while (again) {
        again = false;
        struct mount *below;
        list_for_each_entry(&mounts, below, mounts) {
            if (below->detached || below->point_len <= len ||
                    strncmp(below->point, point, len) != 0 || below->point[len] != '/')
                continue;
            mount_detach_one(below);
            any = again = true;
            break;
        }
    }
    return any;
}

// umount2(MNT_DETACH) -- `umount -l`. The point of a lazy unmount is that it
// works on a BUSY mount: it comes out of the tree at once so nothing new can
// reach it, and the filesystem is released when whoever is still holding it
// lets go. Without it a busy mount can only be unmounted by finding and
// stopping every user, which is exactly the situation `umount -l` exists for
// -- and the flag was ignored, so it answered EBUSY like a plain umount.
//
// Whoever is still holding it is still IN it: a cwd there lists it, creates
// files on it, and finds `..` at its root is its root. That is why a busy
// one is parked at a staging point rather than dropped from the table; see
// mount_detach_one. Dropping it left every lookup from inside it resolving
// the same string on the mount underneath, so `ls` in a cwd there listed
// the directory it had covered and writes landed on the wrong filesystem.
//
// Everything mounted below it goes too, as Linux's umount_tree takes the
// whole subtree -- and each one DISCONNECTED from the mount it sat on
// (disconnect_mount): from the detached mount's root, a name that was a
// submount's point is the directory it covered, and a cwd in a submount finds
// `..` at the submount's own root. So each is detached on its own, the same
// way: destroyed if unused, parked if not. The subtree goes first, while the
// mount above it still has the point that names it. Caller holds mounts_lock.
int mount_remove_lazy(struct mount *mount) {
    char point[MAX_PATH];
    if (mount->point_len < sizeof(point)) {
        memcpy(point, mount->point, mount->point_len + 1);
        mount_detach_below(point);
    }
    mount_detach_one(mount);
    return 0;
}

// Set around a guest's umount2() by sys_umount2_guest, under mounts_lock: the
// kernel's own unmounts are not held to a mount's lock.
static bool guest_umount;

static int do_umount_one(struct mount *mount, bool lazy);
static bool mount_busy_beyond_locked(const struct mount *mount, unsigned held);
static int do_umount_flags(const char *point, bool lazy) {
    struct mount *mount;
    bool found = false;
    list_for_each_entry(&mounts, mount, mounts) {
        // A detached mount is mounted nowhere, so no umount names it -- Linux
        // answers EINVAL for one outside the caller's namespace. A cwd in
        // one can still spell its staging point as ".".
        if (!mount->detached && strcmp(point, mount->point) == 0) {
            found = true;
            break;
        }
    }
    if (!found)
        return _EINVAL;
    // The new root after pivot_root(".", "."), with the old root still
    // stacked over it: what an unmount of that point takes is the old root,
    // the mount on top there on Linux (see struct mount's pivot_over).
    struct mount *stacked;
    list_for_each_entry(&mounts, stacked, mounts) {
        if (stacked->pivot_over == mount->id && !stacked->detached && !stacked->lazy_umount) {
            mount = stacked;
            break;
        }
    }
    // Not by itself, to show what it covers; it goes with the tree it came
    // in when the mount above it is unmounted (Linux's MNT_LOCKED).
    if (mount->locked && guest_umount)
        return _EINVAL;
    // Under a shared mount: the same mount under each of its peers and
    // slaves goes too (Linux's propagate_umount). They go as one: each copy
    // is a bind of the original and holds it, so every one of them would
    // find the next busy. One that is in use by anything else refuses the
    // whole unmount, as a busy mount does, unless this is a lazy one; one
    // with something mounted in it is left where it is.
    size_t n_twins;
    struct mount **twins = mount_propagated_twins_locked(mount, &n_twins);
    if (n_twins == 0) {
        free(twins);
        return do_umount_one(mount, lazy);
    }
    size_t n = n_twins + 1;
    struct mount **set = calloc(n, sizeof(*set));
    if (set == NULL) {
        free(twins);
        return _ENOMEM;
    }
    // Binds before what they are binds of. Those staying are left out.
    size_t members = 0;
    for (int binds = 1; binds >= 0; binds--) {
        for (size_t i = 0; i < n; i++) {
            struct mount *member = i == 0 ? mount : twins[i - 1];
            if ((member->bind_origin != NULL) != (binds == 1))
                continue;
            if (member != mount && !lazy && mount_has_submounts_locked(member))
                continue;
            set[members++] = member;
        }
    }
    free(twins);
    int err = 0;
    for (size_t i = 0; !lazy && err == 0 && i < members; i++) {
        unsigned held = 0;
        for (size_t k = 0; k < members; k++)
            held += set[k]->bind_origin == set[i];
        if (mount_busy_beyond_locked(set[i], held))
            err = _EBUSY;
    }
    for (size_t i = 0; err == 0 && i < members; i++) {
        int one = do_umount_one(set[i], lazy);
        if (set[i] == mount)
            err = one;
    }
    free(set);
    return err;
}

// Is `mount` in use by more than `held` references and the copies other
// mount namespaces have of it (which do not keep it from being unmounted
// here: see do_umount_one)?
static bool mount_busy_beyond_locked(const struct mount *mount, unsigned held) {
    if (mount->refcount <= held)
        return false;
    unsigned foreign = 0;
    size_t own = mount_staging_point_len(mount->point, mount->point_len);
    struct mount *other;
    list_for_each_entry(&mounts, other, mounts) {
        if (other->bind_origin != mount)
            continue;
        size_t theirs = mount_staging_point_len(other->point, other->point_len);
        if (theirs != own || memcmp(other->point, mount->point, own) != 0)
            foreign++;
    }
    return foreign == 0 || mount->refcount - held != foreign;
}

static int do_umount_one(struct mount *mount, bool lazy) {
    if (lazy)
        return mount_remove_lazy(mount);
    // What other mount namespaces hold of it is no business of an unmount
    // here: their copies are mounts of their own (binds of this one, each
    // with a reference on it), and on Linux this one goes while those stay.
    // So if nothing BUT such copies is using it, it leaves this tree the way
    // a lazy unmount takes it out, and lives on for them. Anything else
    // holding it -- a descriptor, a working directory, a bind in this same
    // tree -- is EBUSY as before.
    if (mount->refcount != 0) {
        unsigned foreign = 0;
        size_t own = mount_staging_point_len(mount->point, mount->point_len);
        struct mount *other;
        list_for_each_entry(&mounts, other, mounts) {
            if (other->bind_origin != mount)
                continue;
            size_t theirs = mount_staging_point_len(other->point, other->point_len);
            if (theirs != own || memcmp(other->point, mount->point, own) != 0)
                foreign++;
        }
        if (foreign != 0 && foreign == mount->refcount)
            return mount_remove_lazy(mount);
    }
    return mount_remove(mount);
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
    if (err >= 0)
        mount_propagate_at_locked(point, false);
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

// The app, before it renames or deletes the root exposed at `point`: unmount
// it and everything mounted inside it, which is mount-root.sh's binds of
// /proc, /sys, /dev, /dev/pts, /run and /AOK/{tools,tests,fakefs} and
// whatever else the guest put there. EBUSY, touching nothing, while the root
// itself is in use -- a chroot into it, a cwd or a descriptor in it -- so a
// session still running in there keeps all of them.
//
// What is mounted inside goes lazily (umount -l). A bind is busy while
// anything is open in it, and one that is only in use from outside the root
// has nothing to do with the root's own files; it leaves the table now and
// goes with its last user. The app used to unmount a fixed list of binds one
// by one, first: a busy one stayed behind under a root that had gone, the
// two the list had fallen behind on stayed behind every time, and a session
// in the root lost the ones that did go before the root was found busy.
//
// All under one hold of mounts_lock, so nothing new can start using the root
// between the check and the unmount. EINVAL when nothing is mounted at
// `point`, after still detaching what is mounted below it.
int mount_detach_tree(const char *point) {
    lock(&mounts_lock, 0);
    struct mount *mount, *root = NULL;
    list_for_each_entry(&mounts, mount, mounts) {
        if (!mount->detached && strcmp(mount->point, point) == 0) {
            root = mount;
            break;
        }
    }
    // What other trees have of it: a mount namespace's copy of the mount
    // (every namespace made while the root was exposed has one), and the
    // copy propagation put under a peer or a slave of what it is mounted
    // on. They are the same mount to the user, who is asking for the root
    // to go; and each holds the root, so with one long-lived namespace -- a
    // sandbox someone left running, every service of a systemd root -- the
    // root was "in use" for good and could not be renamed or deleted.
    // Whoever really is in the root through one of them (a descriptor, a
    // working directory) holds that copy, and that is still EBUSY.
    size_t n_copies = 0, cap = 0;
    struct mount **copies = NULL;
    bool busy = false;
    if (root != NULL) {
        size_t n_twins;
        struct mount **twins = mount_propagated_twins_locked(root, &n_twins);
        size_t tree = mount_staging_point_len(root->point, root->point_len);
        list_for_each_entry(&mounts, mount, mounts) {
            if (mount == root || mount->lazy_umount || mount->bind_origin != root)
                continue;
            bool copy = false;
            for (size_t i = 0; i < n_twins && !copy; i++)
                copy = twins[i] == mount;
            // (A covered copy is parked at a staging point of its own, which
            // is another tree too.)
            if (!copy && mount->bind_prefix != NULL && mount->bind_prefix[0] == '\0') {
                size_t theirs = mount_staging_point_len(mount->point, mount->point_len);
                copy = theirs != tree || memcmp(mount->point, root->point, tree) != 0;
            }
            if (!copy)
                continue;
            if (mount->refcount != 0)
                busy = true;
            if (n_copies == cap) {
                size_t new_cap = cap == 0 ? 8 : cap * 2;
                struct mount **grown = realloc(copies, new_cap * sizeof(*copies));
                if (grown == NULL) {
                    busy = true;    // (not safely countable: leave it alone)
                    break;
                }
                copies = grown;
                cap = new_cap;
            }
            copies[n_copies++] = mount;
        }
        free(twins);
        if (root->refcount != n_copies)
            busy = true;
    }
    if (busy) {
        free(copies);
        unlock(&mounts_lock);
        return _EBUSY;
    }
    // The copies first, each with what is mounted in it where it is: they
    // let go of the root as they go.
    for (size_t i = 0; i < n_copies; i++)
        mount_remove_lazy(copies[i]);
    free(copies);
    bool changed = mount_detach_below(point) || n_copies != 0;
    // Still unused: a bind of something in the root would have held it (its
    // origin) and made it busy above, and the lock has kept everyone else out.
    int err = root != NULL ? mount_remove(root) : _EINVAL;
    unlock(&mounts_lock);
    if (changed || err >= 0)
        proc_mountinfo_notify_changed();
    return err;
}

// ---- Mount namespaces (kernel/mnt_ns.h) ------------------------------------

struct mnt_namespace init_mnt_ns = {
    .refcount = 1,
    // Linux's PROC_MNT_INIT_INO... which it has no constant for: the first
    // dynamic one a boot hands out, and what /proc/<pid>/ns/mnt has read here.
    .inode = 4026531840ul,
};

struct mnt_namespace *mnt_ns_current(void) {
    return mnt_ns_or_init(current != NULL ? current->mnt_ns : NULL);
}

// Linux's may_mount(): CAP_SYS_ADMIN in the user namespace the caller's
// mount namespace belongs to. In the machine's own that is the machine's
// CAP_SYS_ADMIN, as it always was; a mount namespace made inside a user
// namespace is its maker's to arrange.
static bool current_may_mount(void) {
    return ns_capable(mnt_ns_current()->user_ns, CAP_SYS_ADMIN_);
}

// The filesystems a caller without the machine's CAP_SYS_ADMIN may mount
// afresh (Linux's FS_USERNS_MOUNT): ones that are made of nothing but memory
// and the kernel's own state, and FUSE, whose files are whatever a daemon of
// the caller's own says (fs/fuse.c speaks to it in the caller's user
// namespace, and lets no one else in without allow_other, and then only that
// namespace). Never one that is a window onto storage -- that is how a
// sandbox would mount its way to files it was not given.
static bool fs_type_unprivileged_ok(const char *type) {
    return strcmp(type, "tmpfs") == 0 || strcmp(type, "proc") == 0 ||
        strcmp(type, "sysfs") == 0 || strcmp(type, "devpts") == 0 ||
        strcmp(type, "fuse") == 0;
}

bool mnt_ns_has_point(const struct mnt_namespace *ns, const char *point) {
    size_t staging = mount_staging_point_len(point, strlen(point));
    if (ns->root_len == 0)
        return staging == 0;
    return staging == ns->root_len && memcmp(point, ns->root, ns->root_len) == 0;
}

// A mount that is in `ns`'s tree now: one a lazy unmount has parked is in no
// tree, wherever it is. Caller holds mounts_lock.
static bool mount_in_ns_locked(const struct mount *mount, const struct mnt_namespace *ns) {
    return !mount->detached && !mount->lazy_umount && mnt_ns_has_point(ns, mount->point);
}

struct mnt_namespace *mnt_ns_retain(struct mnt_namespace *ns) {
    if (ns == NULL || ns == &init_mnt_ns)
        return ns;
    lock(&mounts_lock, 0);
    ns->refcount++;
    unlock(&mounts_lock);
    return ns;
}

// The last task, or the last descriptor naming it, is gone: its tree goes
// with it, each mount as a lazy unmount takes one -- destroyed if nothing is
// open in it, kept alive off every tree for whoever still holds a descriptor
// there (one passed to a process in another namespace, say).
void mnt_ns_release(struct mnt_namespace *ns) {
    if (ns == NULL || ns == &init_mnt_ns)
        return;
    lock(&mounts_lock, 0);
    if (--ns->refcount != 0) {
        unlock(&mounts_lock);
        return;
    }
    for (struct mnt_namespace **link = &mnt_namespaces; *link != NULL; link = &(*link)->next_ns) {
        if (*link == ns) {
            *link = ns->next_ns;
            break;
        }
    }
    // Deepest first, which is the list's order; and from the top each time,
    // since taking one mount out can take others with it.
    bool again = true;
    while (again) {
        again = false;
        struct mount *mount;
        list_for_each_entry(&mounts, mount, mounts) {
            if (!mount_in_ns_locked(mount, ns))
                continue;
            mount_detach_one(mount);
            again = true;
            break;
        }
    }
    unlock(&mounts_lock);
    proc_mountinfo_notify_changed();
    user_ns_release(ns->user_ns);
    free(ns);
}

// `mount`, which is in `from`, as it appears in `to`: a bind of the same
// filesystem at the same place in the tree. Caller holds mounts_lock.
static int mount_copy_to_ns_locked(struct mount *mount, const struct mnt_namespace *from,
        const struct mnt_namespace *to, bool locked, struct mount **copy_out) {
    struct mount *origin = mount->bind_origin != NULL ? mount->bind_origin : mount;
    // A covered one is copied covered: parked, with the place it returns to
    // in the new tree (the caller says by what).
    bool covered = mount_is_covered(mount);
    const char *rel = mount_place(mount) + from->root_len;
    // calloc for the same reason as do_mount: a field nobody names starts zero.
    struct mount *copy = calloc(1, sizeof(struct mount));
    if (copy == NULL)
        return _ENOMEM;
    size_t point_len = to->root_len + strlen(rel);
    char *point = malloc(point_len + 1);
    if (point != NULL) {
        memcpy(point, to->root, to->root_len);
        strcpy(point + to->root_len, rel);
    }
    if (covered && point != NULL) {
        char staging[MAX_PATH];
        mount_staging_point(staging, sizeof(staging));
        copy->covered_point = point;
        point = strdup(staging);
        point_len = point != NULL ? strlen(point) : 0;
        copy->detached = true;
    }
    copy->point = point;
    copy->point_len = point_len;
    copy->source = strdup(mount->source != NULL ? mount->source : "");
    // What the mount is listed as is the same in both namespaces.
    copy->display_source = mount->display_source != NULL ? strdup(mount->display_source) : NULL;
    copy->info = strdup(mount->info != NULL ? mount->info : "");
    copy->bind_prefix = strdup(mount->bind_prefix != NULL ? mount->bind_prefix : "");
    if (copy->point == NULL || copy->source == NULL || copy->info == NULL || copy->bind_prefix == NULL ||
            (mount->display_source != NULL && copy->display_source == NULL)) {
        free((void *) copy->point);
        free((void *) copy->covered_point);
        free((void *) copy->source);
        free((void *) copy->display_source);
        free((void *) copy->info);
        free((void *) copy->bind_prefix);
        free(copy);
        return _ENOMEM;
    }
    copy->flags = mount->flags;
    // (Not the root of the tree: there is nothing under it to show, and
    // pivot_root has to be able to put it aside.)
    copy->locked = (locked && mount->point_len != from->root_len) || mount->locked;
    copy->locked_flags = mount->locked_flags |
        (locked ? mount->flags & (MS_READONLY_ | MS_NOSUID_ | MS_NODEV_ | MS_NOEXEC_) : 0);
    copy->fs = origin->fs;
    copy->refcount = 0;
    copy->root_fd = -1;
    copy->data = NULL;
    copy->fake_dev = origin->fake_dev;
    copy->bind_origin = origin;
    origin->refcount++; // a bind owns one reference on its origin (mount_destroy)
    copy->id = mount_new_id();
    // A peer of the mount it copies, and a slave of the same master -- or,
    // in a namespace that belongs to another user namespace, a slave of a
    // shared mount and never its peer: what is mounted there stays there.
    if (locked) {
        copy->master_id = mount->shared_id != 0 ? mount->shared_id : mount->master_id;
    } else {
        copy->shared_id = mount->shared_id;
        copy->master_id = mount->master_id;
    }
    copy->unbindable = mount->unbindable;
    mount_list_insert(copy);
    if (copy_out != NULL)
        *copy_out = copy;
    return 0;
}

struct mnt_namespace *mnt_ns_copy(struct mnt_namespace *from, struct user_namespace *owner) {
    from = mnt_ns_or_init(from);
    struct mnt_namespace *ns = calloc(1, sizeof(*ns));
    if (ns == NULL)
        return NULL;
    ns->user_ns = user_ns_retain(owner);
    bool locked = owner != from->user_ns;
    ns->refcount = 1;
    ns->inode = ns_alloc_inum();
    mount_staging_point(ns->root, sizeof(ns->root));
    ns->root_len = strlen(ns->root);

    lock(&mounts_lock, 0);
    // From the tail: shallowest first, and of mounts stacked on one point the
    // lowest first, so that inserting each (which puts it ahead of the others
    // at its point) leaves the stack in the order it was. The copies land
    // elsewhere in the list and are not in `from`, so the walk passes them.
    int err = 0;
    struct list *item = mounts.prev;
    while (item != &mounts) {
        struct list *prev = item->prev;
        struct mount *mount = list_entry(item, struct mount, mounts);
        if (mount_in_ns_locked(mount, from)) {
            err = mount_copy_to_ns_locked(mount, from, ns, locked, NULL);
            if (err < 0)
                break;
        }
        item = prev;
    }
    // And what is covered in `from`, covered the same way in the copy: by
    // the copy of what covers it. That is found by place and ID -- the copy
    // of the mount whose ID the original names, which is at the copy's place
    // of the coverer -- so a coverer that is covered in turn is copied first.
    size_t n_covered = 0;
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts)
        if (mount_is_covered(mount) && mnt_ns_has_point(from, mount->covered_point))
            n_covered++;
    struct { struct mount *original, *copy; } *pairs = NULL;
    size_t n_pairs = 0;
    if (err >= 0 && n_covered != 0) {
        pairs = calloc(n_covered, sizeof(*pairs));
        if (pairs == NULL)
            err = _ENOMEM;
        list_for_each_entry(&mounts, mount, mounts) {
            if (err < 0 || n_pairs == n_covered)
                break;
            if (!mount_is_covered(mount) || !mnt_ns_has_point(from, mount->covered_point))
                continue;
            struct mount *copy = NULL;
            err = mount_copy_to_ns_locked(mount, from, ns, locked, &copy);
            if (err >= 0) {
                pairs[n_pairs].original = mount;
                pairs[n_pairs].copy = copy;
                n_pairs++;
            }
        }
        // Each copy is covered by the copy of its original's coverer: the
        // mount in the new tree made from the mount with that ID.
        for (size_t i = 0; err >= 0 && i < n_pairs; i++) {
            int by = pairs[i].original->covered_by;
            struct mount *coverer = NULL, *other;
            list_for_each_entry(&mounts, other, mounts) {
                if (other->id == by) {
                    coverer = other;
                    break;
                }
            }
            int copy_by = 0;
            if (coverer != NULL) {
                for (size_t k = 0; k < n_pairs && copy_by == 0; k++)
                    if (pairs[k].original == coverer)
                        copy_by = pairs[k].copy->id;
                // In view: its copy is the bind of the same filesystem and
                // part of it at the same place in the new tree.
                char place[MAX_PATH];
                if (copy_by == 0 && snprintf(place, sizeof(place), "%s%s", ns->root,
                            coverer->point + from->root_len) < (int) sizeof(place)) {
                    list_for_each_entry(&mounts, other, mounts) {
                        if (mount_in_view(other) && strcmp(other->point, place) == 0 &&
                                mount_fs_origin(other) == mount_fs_origin(coverer) &&
                                strcmp(mount_fs_prefix(other), mount_fs_prefix(coverer)) == 0) {
                            copy_by = other->id;
                            break;
                        }
                    }
                }
            }
            // (Nothing to come back under: in view.)
            if (copy_by == 0) {
                struct mount *copy = pairs[i].copy;
                free((void *) copy->point);
                copy->point = copy->covered_point;
                copy->point_len = strlen(copy->point);
                copy->covered_point = NULL;
                copy->detached = false;
                list_remove(&copy->mounts);
                mount_list_insert(copy);
                continue;
            }
            pairs[i].copy->covered_by = copy_by;
        }
    }
    free(pairs);
    if (err >= 0) {
        ns->next_ns = mnt_namespaces;
        mnt_namespaces = ns;
    }
    unlock(&mounts_lock);
    if (err < 0) {
        mnt_ns_release(ns);
        return NULL;
    }
    return ns;
}

// The directory `dir` is, opened where it is in `to`: `from`'s root replaced
// by `to`'s in its path. NULL when it has no place in `from`'s tree (a
// directory in a detached mount) or cannot be opened there.
static struct fd *mnt_ns_same_dir(struct fd *dir, const struct mnt_namespace *from, const struct mnt_namespace *to) {
    char path[MAX_PATH], moved[MAX_PATH];
    if (generic_getpath(dir, path) < 0)
        return NULL;
    const char *rel = strcmp(path, "/") == 0 ? "" : path;
    if (!mnt_ns_has_point(from, rel))
        return NULL;
    rel += from->root_len;
    if (to->root_len + strlen(rel) + 1 >= sizeof(moved))
        return NULL;
    snprintf(moved, sizeof(moved), "%s%s", to->root, rel);
    if (moved[0] == '\0')
        strcpy(moved, "/");
    struct fd *fd = generic_open_realroot(moved, O_RDONLY_ | O_DIRECTORY_ | O_NOACCESS_CHECK_, 0);
    return IS_ERR(fd) ? NULL : fd;
}

int mnt_ns_move_fs(struct fs_info *fs, struct mnt_namespace *from, struct mnt_namespace *to, bool same_place) {
    from = mnt_ns_or_init(from);
    to = mnt_ns_or_init(to);
    lock(&fs->lock, 0);
    struct fd *old_root = fs->root != NULL ? fd_retain(fs->root) : NULL;
    struct fd *old_pwd = fs->pwd != NULL ? fd_retain(fs->pwd) : NULL;
    unlock(&fs->lock);

    struct fd *new_root = NULL, *new_pwd = NULL;
    if (same_place) {
        if (old_root != NULL)
            new_root = mnt_ns_same_dir(old_root, from, to);
        if (old_pwd != NULL)
            new_pwd = mnt_ns_same_dir(old_pwd, from, to);
    }
    // The namespace's own root for a root that has no counterpart there, and
    // for every setns: Linux's mntns_install sets both to the tree's root.
    if (new_root == NULL) {
        new_root = generic_open_realroot(to->root_len != 0 ? to->root : "/",
                O_RDONLY_ | O_DIRECTORY_ | O_NOACCESS_CHECK_, 0);
        if (IS_ERR(new_root)) {
            int err = (int) PTR_ERR(new_root);
            if (new_pwd != NULL)
                fd_close(new_pwd);
            if (old_root != NULL)
                fd_close(old_root);
            if (old_pwd != NULL)
                fd_close(old_pwd);
            return err;
        }
    }
    if (new_pwd == NULL && !same_place)
        new_pwd = fd_retain(new_root);

    lock(&fs->lock, 0);
    struct fd *dead_root = fs->root;
    fs->root = new_root;
    struct fd *dead_pwd = NULL;
    if (new_pwd != NULL) {
        dead_pwd = fs->pwd;
        fs->pwd = new_pwd;
    }
    unlock(&fs->lock);
    if (dead_root != NULL)
        fd_close(dead_root);
    if (dead_pwd != NULL)
        fd_close(dead_pwd);
    if (old_root != NULL)
        fd_close(old_root);
    if (old_pwd != NULL)
        fd_close(old_pwd);
    return 0;
}

// pivot_root: the mount at new_root becomes the root of the caller's
// namespace, and what was the root, with everything else in the tree, hangs
// at put_old inside it. In a table of points that is every point in the
// namespace rewritten at once:
//
//     under new_root:   <ns><new_root><x>  ->  <ns><x>
//     everything else:  <ns><y>            ->  <ns><put_old below new_root><y>
//
// Descriptors follow by themselves, since a descriptor's path is its mount's
// point plus the path inside it. put_old may be new_root itself --
// pivot_root(".", "."), the form that needs no directory for the old root --
// and then the old root is stacked on top of the new one at "/", where the
// umount2(".", MNT_DETACH) that always follows takes it off. On Linux nothing
// looks up through it meanwhile: the caller's "/" and "." are the new root,
// with its own mounts in it. So here the old root's tree goes to a hidden
// point in the new root, MOUNT_PIVOT_OLD plus the new root's ID, and is listed
// at "/" (struct mount's stacked_point and pivot_over, kernel/fs.h); runc's
// fchdir to an old-root descriptor, and its MS_SLAVE|MS_REC on ".", find it
// there.
//
// The caller's root and working directory move to the new root where they
// were the old one, as Linux's chroot_fs_refs does for it. (Linux does that
// for every process in the namespace; other processes here keep the old
// root, now at put_old, until they chroot. A sandbox pivots before it has
// anyone else in it.)
int mnt_ns_pivot_root(const char *new_root, const char *put_old) {
    struct mnt_namespace *ns = mnt_ns_current();
    // The initial namespace's root is the machine's: the app's own paths
    // (/AOK, the other roots, a saved session's files) are all in terms of
    // it. Linux says EINVAL for a root that is the kernel's own rootfs.
    if (ns->root_len == 0)
        return _EINVAL;
    size_t new_len = strlen(new_root);
    if (!mnt_ns_has_point(ns, new_root) || !mnt_ns_has_point(ns, put_old))
        return _EINVAL;
    // put_old must be at or underneath new_root.
    if (strncmp(put_old, new_root, new_len) != 0 || (put_old[new_len] != '/' && put_old[new_len] != '\0'))
        return _EINVAL;
    // new_root must not be the current root.
    if (new_len == ns->root_len)
        return _EBUSY;
    const char *old_below_new = put_old + new_len; // "" or "/dir..."

    // The caller's root must be the namespace's root (a mount point, as
    // pivot_root wants it); and the directory that will be its root, held
    // before any point changes, while new_root still names it.
    char root_path[MAX_PATH], pwd_path[MAX_PATH] = "";
    int err = fs_root_path(current->fs, root_path);
    if (err < 0)
        return err;
    if (strcmp(root_path, ns->root) != 0)
        return _EINVAL;
    lock(&current->fs->lock, 0);
    struct fd *pwd = current->fs->pwd != NULL ? fd_retain(current->fs->pwd) : NULL;
    unlock(&current->fs->lock);
    if (pwd != NULL) {
        if (generic_getpath(pwd, pwd_path) < 0)
            pwd_path[0] = '\0';
        fd_close(pwd);
    }
    struct fd *root_fd = generic_open_realroot(new_root, O_RDONLY_ | O_DIRECTORY_ | O_NOACCESS_CHECK_, 0);
    if (IS_ERR(root_fd))
        return (int) PTR_ERR(root_fd);

    lock(&mounts_lock, 0);
    // new_root has to be a mount point.
    struct mount *mount, *new_mount = NULL, *old_holder = NULL;
    bool is_point = false, new_is_locked = false;
    size_t count = 0;
    size_t put_old_len = strlen(put_old);
    list_for_each_entry(&mounts, mount, mounts) {
        if (!mount_in_ns_locked(mount, ns))
            continue;
        count++;
        // (The first match is the one on top there.)
        if (strcmp(mount->point, new_root) == 0 && !is_point) {
            is_point = true;
            new_is_locked = mount->locked;
            new_mount = mount;
        }
        // The mount put_old is on: the longest point that holds it.
        if (old_holder == NULL && mount->point_len <= put_old_len &&
                strncmp(put_old, mount->point, mount->point_len) == 0 &&
                (put_old[mount->point_len] == '/' || put_old[mount->point_len] == '\0'))
            old_holder = mount;
    }
    if (!is_point || new_is_locked) {
        unlock(&mounts_lock);
        fd_close(root_fd);
        return _EINVAL;
    }
    // Not while what moves would have to be propagated: the mount put_old
    // is on, and the one the new root is mounted on, may not be shared
    // (which is why a sandbox makes its tree private or slave first).
    struct mount *new_parent = mount_parent_locked(new_mount);
    if ((old_holder != NULL && old_holder->shared_id != 0) ||
            (new_parent != NULL && new_parent->shared_id != 0)) {
        unlock(&mounts_lock);
        fd_close(root_fd);
        return _EINVAL;
    }
    // Where the old root's tree goes: put_old below the new root, or for
    // pivot_root(".", ".") the hidden point the stacked old root is kept at.
    char hidden[64];
    snprintf(hidden, sizeof(hidden), MOUNT_PIVOT_OLD "%d", new_mount->id);
    bool stacked = old_below_new[0] == '\0';
    const char *old_prefix = stacked ? hidden : old_below_new;
    struct mount **moved = calloc(count, sizeof(*moved));
    char **points = calloc(count, sizeof(*points));
    char **listed = calloc(count, sizeof(*listed));
    bool *under_new = calloc(count, sizeof(*under_new));
    err = moved == NULL || points == NULL || listed == NULL || under_new == NULL ? _ENOMEM : 0;
    // Tail to head, as in mnt_ns_copy: reinserting in this order keeps each
    // stack of mounts on one point in the order it was.
    size_t n = 0;
    for (struct list *item = mounts.prev; err == 0 && item != &mounts; item = item->prev) {
        mount = list_entry(item, struct mount, mounts);
        if (!mount_in_ns_locked(mount, ns))
            continue;
        bool under = strncmp(mount->point, new_root, new_len) == 0 &&
            (mount->point[new_len] == '/' || mount->point[new_len] == '\0');
        const char *rest = under ? mount->point + new_len : mount->point + ns->root_len;
        size_t len = ns->root_len + (under ? 0 : strlen(old_prefix)) + strlen(rest);
        char *point = len < MAX_PATH ? malloc(len + 1) : NULL;
        if (point == NULL) {
            err = len < MAX_PATH ? _ENOMEM : _ENAMETOOLONG;
            break;
        }
        snprintf(point, len + 1, "%s%s%s", ns->root, under ? "" : old_prefix, rest);
        // (A stacked old root's mounts are listed where they are stacked.)
        if (stacked && !under) {
            size_t listed_len = ns->root_len + strlen(rest);
            listed[n] = malloc(listed_len + 1);
            if (listed[n] == NULL) {
                free(point);
                err = _ENOMEM;
                break;
            }
            snprintf(listed[n], listed_len + 1, "%s%s", ns->root, rest);
        }
        moved[n] = mount;
        points[n] = point;
        under_new[n] = under;
        n++;
    }
    if (err < 0) {
        for (size_t i = 0; i < n; i++) {
            free(points[i]);
            if (listed != NULL)
                free(listed[i]);
        }
        free(moved);
        free(points);
        free(listed);
        free(under_new);
        unlock(&mounts_lock);
        fd_close(root_fd);
        return err;
    }
    // Nothing can fail from here. The new root's tree first, then the old
    // one's, so that an old root put at "/" (put_old == new_root) is the one
    // on top.
    //
    // Mounts covered in this tree move with it: the point each returns to is
    // rewritten by the same rule. (Best effort: one whose new point cannot be
    // had stays parked for good.)
    list_for_each_entry(&mounts, mount, mounts) {
        if (mount->covered_by == 0 || mount->covered_point == NULL ||
                !mnt_ns_has_point(ns, mount->covered_point))
            continue;
        const char *was = mount->covered_point;
        bool under = strncmp(was, new_root, new_len) == 0 && (was[new_len] == '/' || was[new_len] == '\0');
        const char *rest = under ? was + new_len : was + ns->root_len;
        size_t len = ns->root_len + (under ? 0 : strlen(old_prefix)) + strlen(rest);
        char *point = len < MAX_PATH ? malloc(len + 1) : NULL;
        if (point != NULL)
            snprintf(point, len + 1, "%s%s%s", ns->root, under ? "" : old_prefix, rest);
        free((void *) was);
        mount->covered_point = point;
    }
    char **old_points = calloc(n, sizeof(*old_points));
    for (size_t i = 0; i < n; i++)
        list_remove(&moved[i]->mounts);
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < n; i++) {
            if (under_new[i] != (pass == 0))
                continue;
            if (old_points != NULL)
                old_points[i] = (char *) moved[i]->point;
            else
                free((void *) moved[i]->point);
            moved[i]->point = points[i];
            moved[i]->point_len = strlen(points[i]);
            if (listed[i] != NULL) {
                free((void *) moved[i]->stacked_point);
                moved[i]->stacked_point = listed[i];
                listed[i] = NULL;
                // The old root itself: stacked over the new one.
                if (moved[i]->point_len == ns->root_len + strlen(hidden))
                    moved[i]->pivot_over = new_mount->id;
            }
            mount_list_insert(moved[i]);
            moved[i]->refcount++; // for mount_notify_relocated, below
        }
    }
    // Where each is now, as its filesystem is to hear it: for one just
    // covered, the point it returns to.
    char **new_points = old_points != NULL ? calloc(n, sizeof(*new_points)) : NULL;
    for (size_t i = 0; new_points != NULL && i < n; i++)
        new_points[i] = strdup(moved[i]->covered_point != NULL ? moved[i]->covered_point : moved[i]->point);
    unlock(&mounts_lock);
    // A filesystem with something keyed to its point hears of the move
    // (fs_ops.relocated), outside the lock as for MS_MOVE.
    for (size_t i = 0; i < n; i++) {
        if (new_points != NULL && new_points[i] != NULL)
            mount_notify_relocated(moved[i], old_points[i], new_points[i]);
        else
            mount_release(moved[i]);
        if (old_points != NULL)
            free(old_points[i]);
        if (new_points != NULL)
            free(new_points[i]);
    }
    free(new_points);
    free(old_points);
    free(moved);
    free(points);
    for (size_t i = 0; i < n; i++)
        free(listed[i]);
    free(listed);
    free(under_new);
    proc_mountinfo_notify_changed();

    // The caller's root was the old root: it is the new one now. So is its
    // working directory, if that was the old root too.
    struct fd *pwd_fd = strcmp(pwd_path, ns->root) == 0 ? fd_retain(root_fd) : NULL;
    lock(&current->fs->lock, 0);
    struct fd *dead_root = current->fs->root;
    current->fs->root = root_fd;
    struct fd *dead_pwd = NULL;
    if (pwd_fd != NULL) {
        dead_pwd = current->fs->pwd;
        current->fs->pwd = pwd_fd;
    }
    unlock(&current->fs->lock);
    if (dead_root != NULL)
        fd_close(dead_root);
    if (dead_pwd != NULL)
        fd_close(dead_pwd);
    return 0;
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
// Flags AOK-iSH accepts but does not act on: durability/atime/symlink-resolution
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

    // The mount the source is on, for what a bind takes from it: its peer
    // group and its master (a bind of a shared mount is its peer). An
    // unbindable one is what the name says.
    int source_shared = 0, source_master = 0;
    bool source_unbindable = false;
    lock(&mounts_lock, 0);
    struct mount *holder;
    list_for_each_entry(&mounts, holder, mounts) {
        size_t n = holder->point_len;
        if (strncmp(norm_source, holder->point, n) == 0 && (norm_source[n] == '/' || norm_source[n] == '\0')) {
            source_shared = holder->shared_id;
            source_master = holder->master_id;
            source_unbindable = holder->unbindable;
            break;
        }
    }
    unlock(&mounts_lock);
    if (source_unbindable) {
        mount_release(origin);
        return _EINVAL;
    }

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
    bind->shared_id = source_shared;
    bind->master_id = source_master;

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
// The flags the mount holding `path` is locked with (kernel/fs.h): a bind of
// anything on it carries them, and they stay locked on the bind, or binding
// a read-only mount somewhere else would be the way to write to it.
static int mount_locked_flags_at(const char *path) {
    char copy[MAX_PATH];
    if (strlen(path) >= sizeof(copy))
        return 0;
    strcpy(copy, path);
    struct mount *mount = mount_find(copy);
    if (mount == NULL)
        return 0;
    int locked_flags = mount->locked_flags;
    mount_release(mount);
    return locked_flags;
}

// Is a locked mount mounted below `path`? A bind that is not recursive would
// show what it covers (Linux's has_locked_children).
static bool mount_locked_below(const char *path) {
    size_t len = strlen(path);
    bool found = false;
    lock(&mounts_lock, 0);
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (mount->locked && !mount->detached && !mount->lazy_umount && mount->point_len > len &&
                strncmp(mount->point, path, len) == 0 && mount->point[len] == '/') {
            found = true;
            break;
        }
    }
    unlock(&mounts_lock);
    return found;
}

// The mount on top at `point` takes this lock state. (A bind just made.)
static void mount_set_lock_at(const char *point, bool locked, int locked_flags) {
    lock(&mounts_lock, 0);
    struct mount *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (!mount->detached && !mount->lazy_umount && strcmp(mount->point, point) == 0) {
            mount->locked = locked;
            mount->locked_flags = locked_flags;
            break;
        }
    }
    unlock(&mounts_lock);
}

static void bind_replicate_submounts(const char *norm_source, const char *point, const char *info, int flags) {
    size_t src_len = strlen(norm_source);
    // Collect the submount points under the lock, then bind with it dropped:
    // do_bind_mount resolves paths and takes mounts_lock itself.
    size_t count = 0, cap = 0;
    char **subs = NULL;
    // Each copy is locked as the mount it is a copy of is, flags included.
    struct { bool locked; int flags; } *locks = NULL;
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
            void *grown_locks = realloc(locks, new_cap * sizeof(*locks));
            if (grown_locks == NULL)
                break;
            locks = grown_locks;
            cap = new_cap;
        }
        subs[count] = strdup(mount->point);
        if (subs[count] == NULL)
            break;
        locks[count].locked = mount->locked;
        locks[count].flags = mount->locked_flags;
        count++;
    }
    unlock(&mounts_lock);

    for (size_t i = 0; i < count; i++) {
        char sub_point[MAX_PATH];
        int n = snprintf(sub_point, sizeof(sub_point), "%s%s", point, subs[i] + src_len);
        if (n > 0 && (size_t) n < sizeof(sub_point) &&
                do_bind_mount(subs[i], sub_point, info, flags | locks[i].flags) >= 0 &&
                (locks[i].locked || locks[i].flags != 0))
            mount_set_lock_at(sub_point, locks[i].locked, locks[i].flags);
        free(subs[i]);
    }
    free(subs);
    free(locks);
}

// A directory bind made by the kernel itself, as `mount --bind SOURCE POINT`
// would make it but with no capability check -- for kernel/foreign_exec.c,
// which gives a root the /proc, /dev, ... a program run inside it needs. Both
// must be directories; POINT is resolved from the current task's view.
int mount_bind_dir(const char *source, const char *point) {
    struct statbuf stat;
    int err = generic_statat(AT_PWD, source, &stat, 0);
    if (err < 0)
        return err;
    if (!S_ISDIR(stat.mode))
        return _ENOTDIR;
    if ((err = generic_statat(AT_PWD, point, &stat, 0)) < 0)
        return err;
    if (!S_ISDIR(stat.mode))
        return _ENOTDIR;
    char norm_source[MAX_PATH], norm_point[MAX_PATH];
    if ((err = path_normalize(AT_PWD, source, norm_source, N_SYMLINK_FOLLOW)) < 0)
        return err;
    if ((err = path_normalize(AT_PWD, point, norm_point, N_SYMLINK_FOLLOW)) < 0)
        return err;
    err = do_bind_mount(norm_source, norm_point, "", 0);
    if (err >= 0) {
        lock(&mounts_lock, 0);
        mount_propagate_at_locked(norm_point, false);
        unlock(&mounts_lock);
        proc_mountinfo_notify_changed();
    }
    return err;
}

dword_t sys_mount_guest(guest_addr_t source_addr, guest_addr_t point_addr, guest_addr_t type_addr, dword_t flags, guest_addr_t data_addr) {
    // Linux requires CAP_SYS_ADMIN for every door into the mount table.
    // The app's own boot-time mounts go through do_mount() directly and are
    // unaffected; this gate is only on the guest syscall path.
    if (!current_may_mount())
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

    // The magic number 2.2-era callers put in the top half of the flags, and
    // some still do (bubblewrap passes MS_MGC_VAL with every bind): Linux's
    // path_mount discards it before it reads a single flag.
    if ((flags & 0xffff0000) == 0xc0ed0000)
        flags &= ~0xffff0000;
    dword_t unhandled = flags & ~MS_SUPPORTED & ~MS_OP & ~MS_IGNORED;
    if (unhandled) {
        FIXME("missing mount flags %#x from %s[%d]", unhandled, current->comm, current->pid);
        return _EINVAL;
    }

    // A propagation-type change (mount --make-[r]private/shared/slave/
    // unbindable) is its own operation that carries no new filesystem: see
    // "Propagation" above. systemd issues these at boot, and a sandbox makes
    // its tree private or slave before anything else.
    bool change_type = (flags & MS_PROPAGATION) && !(flags & (MS_BIND_ | MS_REMOUNT_));

    struct statbuf stat;
    int err = generic_statat(AT_PWD, point_raw, &stat, 0);
    if (err < 0)
        return err;
    // A bind is exempt from the directory requirement: Linux allows binding a
    // single file over another file. Dir-onto-dir or non-dir-onto-non-dir;
    // the mixed cases are rejected in the bind branch once the source has
    // been stat'd too. Nor does changing a mount's propagation ask what it
    // is mounted on.
    if (!S_ISDIR(stat.mode) && !(flags & MS_BIND_) && !change_type)
        return _ENOTDIR;

    char point[MAX_PATH];
    err = path_normalize(AT_PWD, point_raw, point, N_SYMLINK_FOLLOW);
    if (err < 0)
        return err;
    // A point inside a detached mount -- reachable only from a cwd or dirfd
    // already in it -- is in no mount tree for a mount to join or be changed
    // in. Linux's check_mnt() refuses it with EINVAL for every operation.
    // (A mount namespace's tree is under a staging point too, and that is
    // not this: its points are in the caller's tree. kernel/mnt_ns.h.)
    if (!mnt_ns_has_point(mnt_ns_current(), point))
        return _EINVAL;

    if (change_type) {
        // One kind at a time (Linux's flags_to_propagation_type).
        int kind = flags & MS_PROPAGATION;
        if (kind & (kind - 1))
            return _EINVAL;
        return mount_change_propagation(strcmp(point, "/") == 0 ? "" : point, kind, (flags & MS_REC_) != 0);
    }

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
        // Nor is anything bound or moved out of one.
        if ((flags & MS_BIND_) && !mnt_ns_has_point(mnt_ns_current(), op_source))
            return _EINVAL;
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
        if (!(flags & MS_REC_) && mount_locked_below(op_source))
            return _EINVAL;
        int inherited = mount_locked_flags_at(op_source);
        err = do_bind_mount(op_source, point, data, (flags & MS_FLAGS) | inherited);
        if (err >= 0) {
            if (inherited != 0)
                mount_set_lock_at(point, false, inherited);
            if (flags & MS_REC_)
                bind_replicate_submounts(op_source, point, data, flags & MS_FLAGS);
            // What was mounted below the point is under the bind now. Not the
            // copies a recursive bind has just made there: they are its own.
            lock(&mounts_lock, 0);
            mount_cover_at_locked(point, true);
            mount_propagate_at_locked(point, (flags & MS_REC_) != 0);
            unlock(&mounts_lock);
            proc_mountinfo_notify_changed();
        }
        return err;
    }

    // A new filesystem, by a caller whose CAP_SYS_ADMIN is a user namespace's
    // and not the machine's: only the kinds that give it nothing but memory.
    if (!(flags & (MS_MOVE_ | MS_REMOUNT_)) && !current_capable(CAP_SYS_ADMIN_) &&
            !fs_type_unprivileged_ok(type))
        return _EPERM;
    // A daemon's device nodes are not devices to anyone it was not mounted
    // by the machine for: Linux marks a mount made in a user namespace
    // SB_I_NODEV, and here that is nodev.
    if (!(flags & (MS_MOVE_ | MS_REMOUNT_)) && !current_capable(CAP_SYS_ADMIN_) &&
            strcmp(type, "fuse") == 0)
        flags |= MS_NODEV_;

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
            // Not one umount -l has detached: see do_umount_flags.
            if (!mount->lazy_umount && strcmp(mount->point, op_source) == 0) {
                found = mount;
                break;
            }
        }
        // (A locked mount stays where it is: moving it would show what is
        // under it.)
        if (found == NULL || found->locked) {
            unlock(&mounts_lock);
            return _EINVAL;
        }
        // Nor one that sits under a shared mount: taking it away there would
        // be an unmount to propagate, and Linux refuses instead.
        struct mount *moved_from = mount_parent_locked(found);
        if (moved_from != NULL && moved_from->shared_id != 0) {
            unlock(&mounts_lock);
            return _EINVAL;
        }
        // Not onto itself, nor into its own tree.
        size_t source_len = strlen(op_source);
        if (strncmp(point, op_source, source_len) == 0 &&
                (point[source_len] == '/' || point[source_len] == '\0')) {
            unlock(&mounts_lock);
            return _ELOOP;
        }
        const char *new_point = strdup(point);
        if (new_point == NULL) {
            unlock(&mounts_lock);
            return _ENOMEM;
        }
        // With what is mounted in it (mount_subtree_collect_locked).
        struct mount_subtree tree;
        int tree_err = mount_subtree_collect_locked(found, op_source, point, &tree);
        if (tree_err < 0) {
            free((void *) new_point);
            unlock(&mounts_lock);
            return tree_err;
        }
        // It uncovers what it was over, and covers what it lands on.
        mount_uncover_locked(found);
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
        mount_cover_below_locked(found, MOUNT_COVER_ALL);
        mount_subtree_place_locked(&tree, op_source, point);
        // Where it lands it is a new mount, to whatever receives from there.
        mount_propagate_tree_locked(found, &tree);
        found->refcount++;  // for mount_notify_relocated
        unlock(&mounts_lock);
        mount_notify_relocated(found, op_source, point);
        mount_subtree_notify(&tree);
        proc_mountinfo_notify_changed();
        return 0;
    }

    if (flags & MS_REMOUNT_) {
        struct mount *mount;
        bool found = false;
        list_for_each_entry(&mounts, mount, mounts) {
            bool is_root_remount = strcmp(point, "/") == 0 && mount->point[0] == '\0';
            if (strcmp(point, mount->point) == 0 || is_root_remount) {
                // The filesystem's own options (tmpfs's size=) first, so a
                // refusal changes nothing. MS_REMOUNT|MS_BIND changes the
                // mount's flags alone, as on Linux.
                // A bind has no filesystem of its own to reconfigure: its
                // origin's is the one (and every mount of a copied mount
                // namespace is a bind, so `mount -o remount /` gets here).
                if (mount->fs->remount != NULL && !(flags & MS_BIND_)) {
                    struct mount *sb = mount->bind_origin != NULL ? mount->bind_origin : mount;
                    int err = sb->fs->remount(sb, data);
                    if (err < 0) {
                        unlock(&mounts_lock);
                        return err;
                    }
                }
                // A flag the mount was locked with stays on (kernel/fs.h).
                if (mount->locked_flags & ~flags) {
                    unlock(&mounts_lock);
                    return _EPERM;
                }
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
    // The kernel's own mounts (do_mount from boot, the app, a restored
    // session) come in no particular order and cover nothing; a guest's
    // covers what was mounted below its point, as on Linux.
    if (err >= 0) {
        mount_cover_at_locked(point, false);
        mount_propagate_at_locked(point, false);
    }
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
    if (!current_may_mount())
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
    guest_umount = true;
    err = do_umount_flags(target, (flags & MNT_DETACH_) != 0);
    guest_umount = false;
    unlock(&mounts_lock);
    if (err >= 0)
        proc_mountinfo_notify_changed();
    return err;
}

dword_t sys_umount2(addr_t target_addr, dword_t flags) {
    return sys_umount2_guest(target_addr, flags);
}

// pivot_root(new_root, put_old): see mnt_ns_pivot_root.
dword_t sys_pivot_root_guest(guest_addr_t new_root_addr, guest_addr_t put_old_addr) {
    if (!current_may_mount())
        return _EPERM;
    char new_raw[MAX_PATH], old_raw[MAX_PATH];
    int path_err = user_read_path(new_root_addr, new_raw, sizeof(new_raw));
    if (path_err == 0)
        path_err = user_read_path(put_old_addr, old_raw, sizeof(old_raw));
    if (path_err)
        return path_err;
    STRACE("pivot_root(\"%s\", \"%s\")", new_raw, old_raw);
    // Both name directories that exist.
    char new_root[MAX_PATH], put_old[MAX_PATH];
    const char *raw[2] = {new_raw, old_raw};
    char *norm[2] = {new_root, put_old};
    for (int i = 0; i < 2; i++) {
        struct statbuf stat;
        int err = generic_statat(AT_PWD, raw[i], &stat, 0);
        if (err < 0)
            return err;
        if (!S_ISDIR(stat.mode))
            return _ENOTDIR;
        err = path_normalize(AT_PWD, raw[i], norm[i], N_SYMLINK_FOLLOW);
        if (err < 0)
            return err;
    }
    return mnt_ns_pivot_root(new_root, put_old);
}

dword_t sys_pivot_root(addr_t new_root_addr, addr_t put_old_addr) {
    return sys_pivot_root_guest(new_root_addr, put_old_addr);
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
    // What fsconfig set, as mount(2)'s option string: "size=5m,mode=1777".
    // Until FSCONFIG_CMD_CREATE these are the new filesystem's options; after
    // it, what the next FSCONFIG_CMD_RECONFIGURE applies.
    char options[1024];
    // The "source" parameter, the first column of /proc/mounts.
    char source[256];
    // The mount CREATE made, by its ID: move_mount changes its point, and
    // a reconfigure still has to find it.
    int mount_id;
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
    if (!current_may_mount())
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
    // As for mount(2): see fs_type_unprivileged_ok.
    if (!current_capable(CAP_SYS_ADMIN_) && !fs_type_unprivileged_ok(fsname))
        return _EPERM;

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
#define FSCONFIG_CMD_CREATE_EXCL_ 8

// Append one option to the context's string; EINVAL when it will not fit.
static int fscontext_add_option(struct fscontext_data *data, const char *key, const char *value) {
    size_t at = strlen(data->options);
    int n = snprintf(data->options + at, sizeof(data->options) - at, "%s%s%s%s",
            at != 0 ? "," : "", key, value != NULL ? "=" : "", value != NULL ? value : "");
    if (n < 0 || (size_t) n >= sizeof(data->options) - at) {
        data->options[at] = '\0';
        return _EINVAL;
    }
    return 0;
}

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
        case FSCONFIG_SET_STRING_: {
            // A name this filesystem would not parse is refused, the way Linux
            // refuses it; see fscontext_option_known.
            if (!fscontext_option_known(data, key))
                return _EINVAL;
            char value[256] = "";
            if (cmd == FSCONFIG_SET_STRING_) {
                if (value_addr == 0)
                    return _EINVAL;
                if (user_read_string(value_addr, value, sizeof(value)))
                    return _EFAULT;
            }
            if (strcmp(key, "ro") == 0 || strcmp(key, "rw") == 0) {
                data->readonly = key[1] == 'o';
                return 0;
            }
            if (strcmp(key, "source") == 0) {
                snprintf(data->source, sizeof(data->source), "%s", value);
                return 0;
            }
            // Everything else reaches the filesystem the way mount(2)'s option
            // string does, at FSCONFIG_CMD_CREATE or _RECONFIGURE. These were
            // accepted and dropped: `mount -t tmpfs -o size=5m` -- which
            // util-linux does through this API unless a flag like noexec sends
            // it back to mount(2) -- made a tmpfs with no size limit at all.
            return fscontext_add_option(data, key,
                    cmd == FSCONFIG_SET_STRING_ ? value : NULL);
        }
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
        case FSCONFIG_CMD_CREATE_:
        case FSCONFIG_CMD_CREATE_EXCL_: {
            // Every superblock here is new, so the exclusive form is the same.
            if (data->created)
                return _EBUSY;
            mount_staging_point(data->point, sizeof(data->point));
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
            // No directory to make: MOUNT_STAGING_DIR is on no filesystem
            // (kernel/fs.h). This used to mkdir a 0700 /.ish-fsmount in the
            // guest's root, which failed on a read-only one.
            lock(&mounts_lock, 0);
            // With no "source", Linux's /proc/mounts says "none".
            int err = do_mount(data->fs, data->source[0] != '\0' ? data->source : "none",
                    data->point, data->options, data->readonly ? MS_READONLY_ : 0);
            if (err >= 0) {
                // Detached until move_mount places it; see struct mount.
                struct mount *staged = mount_at_point_locked(data->point);
                if (staged != NULL) {
                    staged->detached = true;
                    data->mount_id = staged->id;
                }
            }
            unlock(&mounts_lock);
            if (err < 0)
                return err;
            data->created = true;
            // Consumed: what is set from here on is for a reconfigure.
            data->options[0] = '\0';
            proc_mountinfo_notify_changed();
            return 0;
        }
        case FSCONFIG_CMD_RECONFIGURE_: {
            if (!data->created)
                return _EINVAL;
            lock(&mounts_lock, 0);
            struct mount *mount;
            bool found = false;
            int err = 0;
            list_for_each_entry(&mounts, mount, mounts) {
                if (mount->id == data->mount_id) {
                    if (mount->fs->remount != NULL && data->options[0] != '\0')
                        err = mount->fs->remount(mount, data->options);
                    if (err == 0)
                        mount->flags = (mount->flags & ~MS_READONLY_) |
                            (data->readonly ? MS_READONLY_ : 0);
                    found = true;
                    break;
                }
            }
            unlock(&mounts_lock);
            if (err < 0)
                return err;
            data->options[0] = '\0';
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

#define FSMOUNT_CLOEXEC_ 0x1
#define MOUNT_ATTR_RDONLY_ 0x1
#define MOUNT_ATTR_NOSUID_ 0x2
#define MOUNT_ATTR_NODEV_ 0x4
#define MOUNT_ATTR_NOEXEC_ 0x8
#define MOUNT_ATTR__ATIME_ 0x70
#define MOUNT_ATTR_RELATIME_ 0x0
#define MOUNT_ATTR_NOATIME_ 0x10
#define MOUNT_ATTR_STRICTATIME_ 0x20
#define MOUNT_ATTR_NODIRATIME_ 0x80
#define MOUNT_ATTR_NOSYMFOLLOW_ 0x200000

// fsmount's attr_flags as the mount's MS_ flags, or -1 for a value Linux
// refuses: an unknown bit, two atime modes at once, or MOUNT_ATTR_IDMAP, which
// needs a user namespace only mount_setattr can name.
static int fsmount_attr_flags(dword_t attr) {
    const dword_t known = MOUNT_ATTR_RDONLY_ | MOUNT_ATTR_NOSUID_ | MOUNT_ATTR_NODEV_ |
            MOUNT_ATTR_NOEXEC_ | MOUNT_ATTR__ATIME_ | MOUNT_ATTR_NODIRATIME_ |
            MOUNT_ATTR_NOSYMFOLLOW_;
    if (attr & ~known)
        return -1;
    int flags = 0;
    switch (attr & MOUNT_ATTR__ATIME_) {
        case MOUNT_ATTR_RELATIME_: break;   // the default, as mount(2)'s
        case MOUNT_ATTR_NOATIME_: flags |= MS_NOATIME_; break;
        case MOUNT_ATTR_STRICTATIME_: flags |= MS_STRICTATIME_; break;
        default: return -1;
    }
    if (attr & MOUNT_ATTR_RDONLY_) flags |= MS_READONLY_;
    if (attr & MOUNT_ATTR_NOSUID_) flags |= MS_NOSUID_;
    if (attr & MOUNT_ATTR_NODEV_) flags |= MS_NODEV_;
    if (attr & MOUNT_ATTR_NOEXEC_) flags |= MS_NOEXEC_;
    if (attr & MOUNT_ATTR_NODIRATIME_) flags |= MS_NODIRATIME_;
    if (attr & MOUNT_ATTR_NOSYMFOLLOW_) flags |= MS_NOSYMFOLLOW_;
    return flags;
}

fd_t sys_fsmount_guest(fd_t f, dword_t flags, dword_t attr_flags) {
    // Linux requires CAP_SYS_ADMIN for every door into the mount table.
    // The app's own boot-time mounts go through do_mount() directly and are
    // unaffected; this gate is only on the guest syscall path.
    if (!current_may_mount())
        return _EPERM;
    STRACE("fsmount(%d, %#x, %#x)", f, flags, attr_flags);
    if (flags & ~FSMOUNT_CLOEXEC_)
        return _EINVAL;
    int mount_flags = fsmount_attr_flags(attr_flags);
    if (mount_flags < 0)
        return _EINVAL;
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

    // The attributes go on before anything can be opened through the mount:
    // an fd takes its mount's flags when it is opened. They were ignored, so
    // noexec, nosuid and nodev asked of fsmount did nothing.
    lock(&mounts_lock, 0);
    struct mount *staged = mount_at_point_locked(data->point);
    if (staged != NULL)
        staged->flags |= mount_flags;
    // A FUSE mount's root is not opened: that is a request to a daemon that,
    // as daemons do, mounts first and serves after -- fsmount waited on it
    // forever. Linux hands back a descriptor on the mount without asking the
    // filesystem anything; so does this, a handle naming the mount's root
    // (an opath_link with an empty path), which is all move_mount wants of it.
    if (staged != NULL && staged->fs == &fusefs) {
        staged->refcount++;             // (mount_retain, under the lock held here)
        unlock(&mounts_lock);
        struct fd *handle = opath_link_fd_create(staged, "");
        if (handle == NULL) {
            mount_release(staged);
            return _ENOMEM;
        }
        handle->type = S_IFDIR;
        handle->flags = O_PATH_;
        return f_install(handle, O_CLOEXEC_);
    }
    unlock(&mounts_lock);

    // data->point is a real-root staging path (/.ish-fsmount/<n>) that is
    // not visible inside a chroot; open it against the real root, or
    // util-linux mount(8)'s new-API path fails ENOENT in every chroot. And
    // as the one walk that may enter it (N_DETACHED_OK): this descriptor is
    // how the caller reaches the mount at all.
    struct fd *dirfd = generic_openat_norm(AT_PWD, data->point,
            O_RDONLY_ | O_DIRECTORY_ | O_CLOEXEC_, 0, N_REALROOT | N_DETACHED_OK);
    if (IS_ERR(dirfd))
        return PTR_ERR(dirfd);
    return f_install(dirfd, O_CLOEXEC_);
}

fd_t sys_fsmount(fd_t f, dword_t flags, dword_t attr_flags) {
    return sys_fsmount_guest(f, flags, attr_flags);
}

// ---- mount_setattr ----------------------------------------------------------
//
// The per-mount flags of the mount whose root a path names, and with
// AT_RECURSIVE of every mount below it. It is what MS_REMOUNT|MS_BIND does,
// said without a filesystem's options in the way, and how bubblewrap makes a
// tree read-only.

#define AT_RECURSIVE_ 0x8000
#define MOUNT_ATTR_IDMAP_ 0x100000

struct mount_attr_ {
    qword_t attr_set;
    qword_t attr_clr;
    qword_t propagation;
    qword_t userns_fd;
};

// The MS_ flags the MOUNT_ATTR_ bits in `attr` stand for, the atime ones
// apart.
static int mount_attr_ms_flags(qword_t attr) {
    int flags = 0;
    if (attr & MOUNT_ATTR_RDONLY_) flags |= MS_READONLY_;
    if (attr & MOUNT_ATTR_NOSUID_) flags |= MS_NOSUID_;
    if (attr & MOUNT_ATTR_NODEV_) flags |= MS_NODEV_;
    if (attr & MOUNT_ATTR_NOEXEC_) flags |= MS_NOEXEC_;
    if (attr & MOUNT_ATTR_NODIRATIME_) flags |= MS_NODIRATIME_;
    if (attr & MOUNT_ATTR_NOSYMFOLLOW_) flags |= MS_NOSYMFOLLOW_;
    return flags;
}

dword_t sys_mount_setattr_guest(fd_t dfd, guest_addr_t path_addr, dword_t flags,
        guest_addr_t attr_addr, guest_addr_t size) {
    char path_raw[MAX_PATH];
    if (user_read_string(path_addr, path_raw, sizeof(path_raw)))
        return _EFAULT;
    STRACE("mount_setattr(%d, \"%s\", %#x, %#llx, %llu)", dfd, path_raw, flags,
            (unsigned long long) attr_addr, (unsigned long long) size);
    if (flags & ~(AT_EMPTY_PATH_ | AT_RECURSIVE_ | AT_SYMLINK_NOFOLLOW_ | AT_NO_AUTOMOUNT_))
        return _EINVAL;
    // copy_struct_from_user: a struct shorter than the first version is
    // EINVAL, one larger than a page E2BIG, and anything past what is known
    // here must be zero. Between the size and the copy, may_mount().
    struct mount_attr_ attr;
    if (size > 4096)
        return _E2BIG;
    if (size < sizeof(attr))
        return _EINVAL;
    if (!current_may_mount())
        return _EPERM;
    if (user_read(attr_addr, &attr, sizeof(attr)))
        return _EFAULT;
    for (guest_addr_t offset = sizeof(attr); offset < size; offset++) {
        char extra;
        if (user_read(attr_addr + offset, &extra, 1))
            return _EFAULT;
        if (extra != 0)
            return _E2BIG;
    }
    // Nothing asked: nothing looked at, the path included.
    if (attr.attr_set == 0 && attr.attr_clr == 0 && attr.propagation == 0)
        return 0;

    const qword_t known = MOUNT_ATTR_RDONLY_ | MOUNT_ATTR_NOSUID_ | MOUNT_ATTR_NODEV_ |
            MOUNT_ATTR_NOEXEC_ | MOUNT_ATTR__ATIME_ | MOUNT_ATTR_NODIRATIME_ |
            MOUNT_ATTR_NOSYMFOLLOW_ | MOUNT_ATTR_IDMAP_;
    if ((attr.attr_set | attr.attr_clr) & ~known)
        return _EINVAL;
    switch (attr.propagation) {
        case 0: case MS_UNBINDABLE_: case MS_PRIVATE_: case MS_SLAVE_: case MS_SHARED_: break;
        default: return _EINVAL;
    }
    // The atime mode is one field, not three bits: it changes only when the
    // whole of it is cleared, and then to the one mode set.
    int set = mount_attr_ms_flags(attr.attr_set);
    int clear = mount_attr_ms_flags(attr.attr_clr);
    if (attr.attr_clr & MOUNT_ATTR__ATIME_) {
        if ((attr.attr_clr & MOUNT_ATTR__ATIME_) != MOUNT_ATTR__ATIME_)
            return _EINVAL;
        clear |= MS_NOATIME_ | MS_STRICTATIME_ | MS_RELATIME_;
        switch (attr.attr_set & MOUNT_ATTR__ATIME_) {
            case MOUNT_ATTR_RELATIME_: set |= MS_RELATIME_; break;
            case MOUNT_ATTR_NOATIME_: set |= MS_NOATIME_; break;
            case MOUNT_ATTR_STRICTATIME_: break;
            default: return _EINVAL;
        }
    } else if (attr.attr_set & MOUNT_ATTR__ATIME_) {
        return _EINVAL;
    }
    // An id-mapped mount needs a user namespace to map through, and there
    // are none of those to name.
    if ((attr.attr_set | attr.attr_clr) & MOUNT_ATTR_IDMAP_)
        return _EINVAL;

    char point[MAX_PATH];
    if (path_raw[0] == '\0') {
        if (!(flags & AT_EMPTY_PATH_))
            return _ENOENT;
        struct fd *fd = at_fd_for_path(dfd, "");
        if (fd == NULL)
            return _EBADF;
        lock(&current->fs->lock, 0);
        if (fd == AT_PWD)
            fd = current->fs->pwd;
        fd_retain(fd);
        unlock(&current->fs->lock);
        int err = generic_getpath(fd, point);
        fd_close(fd);
        if (err < 0)
            return err;
        if (!path_is_normalized(point))
            return _EINVAL;     // a pipe, a socket: on no mount a caller may change
    } else {
        struct fd *at = at_fd_for_path(dfd, path_raw);
        if (at == NULL)
            return _EBADF;
        int err = path_normalize(at, path_raw, point,
                (flags & AT_SYMLINK_NOFOLLOW_) ? N_SYMLINK_NOFOLLOW : N_SYMLINK_FOLLOW);
        if (err < 0)
            return err;
        struct statbuf stat;
        err = generic_statat(at, path_raw, &stat,
                (flags & AT_SYMLINK_NOFOLLOW_) ? AT_SYMLINK_NOFOLLOW_ : 0);
        if (err < 0)
            return err;
    }
    if (strcmp(point, "/") == 0)
        point[0] = '\0';
    // check_mnt(): a mount in the caller's tree -- or in none yet, the one
    // fsmount() has made and move_mount() not placed, which is how systemd
    // sets the flags of the /proc it is about to give a service. Not a mount
    // of another namespace, nor one unmounted or covered.
    bool in_tree = mnt_ns_has_point(mnt_ns_current(), point);

    size_t point_len = strlen(point);
    lock(&mounts_lock, 0);
    // path_mounted(): the path is the root of a mount, not somewhere in one.
    struct mount *top = NULL, *mount;
    list_for_each_entry(&mounts, mount, mounts) {
        if (mount->lazy_umount || mount->covered_by != 0 || strcmp(mount->point, point) != 0)
            continue;
        if (in_tree ? !mount->detached : mount->detached)
            top = mount;
        break;
    }
    if (top == NULL) {
        unlock(&mounts_lock);
        return _EINVAL;
    }
    // A flag a mount was locked with stays on: one such mount among them
    // and nothing changes (can_change_locked_flags).
    for (int pass = 0; pass < 2; pass++)
    list_for_each_entry(&mounts, mount, mounts) {
        if (mount != top) {
            if (!(flags & AT_RECURSIVE_) || mount->detached || mount->lazy_umount)
                continue;
            if (strncmp(mount->point, point, point_len) != 0 || mount->point[point_len] != '/')
                continue;
            // The whole table sorts under the initial namespace's root, the
            // trees of the others included, and those are not below it.
            if (!mnt_ns_has_point(mnt_ns_current(), mount->point))
                continue;
        }
        if (pass == 0) {
            if (mount->locked_flags & clear) {
                unlock(&mounts_lock);
                return _EPERM;
            }
            continue;
        }
        mount->flags = (mount->flags & ~clear) | set;
        if (attr.propagation != 0)
            mount_set_propagation_locked(mount, (int) attr.propagation);
    }
    unlock(&mounts_lock);
    proc_mountinfo_notify_changed();
    return 0;
}

dword_t sys_mount_setattr(fd_t dfd, addr_t path_addr, dword_t flags, addr_t attr_addr, dword_t size) {
    return sys_mount_setattr_guest(dfd, path_addr, flags, attr_addr, size);
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
        // An fsmount()'s mount, not one umount -l detached: that is gone
        // from every tree, and Linux's move_mount refuses it with EINVAL.
        if (!mount->lazy_umount && strcmp(mount->point, from_point) == 0) {
            found = mount;
            break;
        }
    }
    if (found == NULL) {
        unlock(&mounts_lock);
        return _EINVAL;
    }
    // Nor out from under a shared mount (see MS_MOVE). One fsmount() made
    // and nothing has placed yet sits on nothing.
    struct mount *moved_from = found->detached ? NULL : mount_parent_locked(found);
    if (moved_from != NULL && moved_from->shared_id != 0) {
        unlock(&mounts_lock);
        return _EINVAL;
    }
    // Not onto itself, nor into its own tree.
    size_t from_len = strlen(from_point);
    if (strncmp(to_point, from_point, from_len) == 0 &&
            (to_point[from_len] == '/' || to_point[from_len] == '\0')) {
        unlock(&mounts_lock);
        return _ELOOP;
    }
    const char *new_point = strdup(to_point);
    if (new_point == NULL) {
        unlock(&mounts_lock);
        return _ENOMEM;
    }
    // With what is mounted in it (mount_subtree_collect_locked).
    struct mount_subtree tree;
    int tree_err = mount_subtree_collect_locked(found, from_point, to_point, &tree);
    if (tree_err < 0) {
        free((void *) new_point);
        unlock(&mounts_lock);
        return tree_err;
    }
    // A covered mount, moved by a descriptor inside it, is covered no more;
    // and wherever it comes from, it covers what it lands on.
    mount_uncover_locked(found);
    if (found->covered_by != 0) {
        found->covered_by = 0;
        free((void *) found->covered_point);
        found->covered_point = NULL;
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
    bool was_detached = found->detached;
    found->detached = false;
    mount_cover_below_locked(found, MOUNT_COVER_ALL);
    found->detached = was_detached;
    mount_subtree_place_locked(&tree, from_point, to_point);
    found->detached = false;
    mount_propagate_tree_locked(found, &tree);
    found->detached = was_detached;
    found->refcount++;  // for mount_notify_relocated
    unlock(&mounts_lock);
    mount_notify_relocated(found, from_point, to_point);
    mount_subtree_notify(&tree);
    return 0;
}

#define MOVE_MOUNT_F_EMPTY_PATH_ 0x00000004

dword_t sys_move_mount_guest(fd_t from_dfd, guest_addr_t from_path_addr, fd_t to_dfd,
        guest_addr_t to_path_addr, dword_t flags) {
    // Linux requires CAP_SYS_ADMIN for every door into the mount table.
    // The app's own boot-time mounts go through do_mount() directly and are
    // unaffected; this gate is only on the guest syscall path.
    if (!current_may_mount())
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
    // A caller doing this for real still holds the fsmount fd (that is how
    // the API works), and that fd holds a reference, so the staged mount goes
    // lazily: it stays where it is parked until the fd is closed, and then
    // it is torn down, where a plain umount could only have answered EBUSY
    // and left it there for good.
    if (mount_fs_at(from_point) == &devtmpfs && devtmpfs_target_is_populated(to_point)) {
        lock(&mounts_lock, 0);
        struct mount *staged = mount_at_point_locked(from_point);
        if (staged != NULL && staged->detached)
            mount_remove_lazy(staged);
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
