// Extended attributes: setxattr(2), getxattr(2), listxattr(2) and
// removexattr(2), each with its l* form (the final symlink is not followed)
// and its f* form (an open file).
//
// All four answered ENOTSUP for every file, so nothing that keeps metadata
// beside a file could work: setcap and getcap (security.capability), rsync -X,
// tar --xattrs, cp -a's attribute copy, systemd's user.* and trusted.* markers.
//
// Everything Linux decides above the filesystem is decided here, in Linux's
// order; a filesystem only stores (struct xattr_ops in kernel/fs.h). The order
// is observable, and was measured on Linux 6.12 (tests/manual/xattr_ops.c):
//
//   1. the arguments: flags (EINVAL), the name (EFAULT; ERANGE for empty or
//      longer than 255), the value (E2BIG past 64 KiB; EFAULT)
//   2. the lookup, and EROFS for a change on a read-only mount
//   3. permission, by namespace: user.* only on files and directories, and by
//      the mode bits; trusted.* only with CAP_SYS_ADMIN (EPERM to write,
//      ENODATA to read -- it does not exist for anyone else); security.* and
//      system.* are not the mode bits' business
//   4. the security checks: security.* needs CAP_SYS_ADMIN to change, except
//      security.capability, which needs a well-formed value and CAP_SETFCAP
//   5. whether the filesystem has attributes at all (EOPNOTSUPP), and only
//      then whether the name is one it keeps: a bare "user." is EINVAL, a
//      namespace it has no handler for EOPNOTSUPP
//
// So an unprivileged write to a 0444 /proc file is EACCES while root's is
// EOPNOTSUPP, and a pipe answers as any non-file does without its filesystem
// being asked.
//
// system.* is POSIX ACLs, which AOK does not keep: every system.* name is
// EOPNOTSUPP, which is what a filesystem mounted noacl answers.

#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "kernel/inotify.h"
#include "kernel/task.h"
#include "kernel/xattr.h"
#include "fs/fd.h"
#include "fs/path.h"

// security.capability's on-disk layouts (linux/capability.h). Little-endian
// on disk, like every guest AOK runs.
#define VFS_CAP_REVISION_MASK_ 0xff000000u
#define VFS_CAP_FLAGS_EFFECTIVE_ 0x000001u
#define VFS_CAP_REVISION_1_ 0x01000000u
#define VFS_CAP_REVISION_2_ 0x02000000u
#define VFS_CAP_REVISION_3_ 0x03000000u
#define XATTR_CAPS_SZ_1_ 12
#define XATTR_CAPS_SZ_2_ 20
#define XATTR_CAPS_SZ_3_ 24

struct vfs_ns_cap_data_ {
    dword_t magic_etc;
    struct {
        dword_t permitted;
        dword_t inheritable;
    } data[2];
    dword_t rootid;
};

const struct xattr_ops *xattr_ops_for_mount(struct mount *mount) {
    if (mount == NULL)
        return NULL;
    if (mount->fs == &fakefs)
        return &fakefs_xattr_ops;
    // tmpfs's own inodes back /dev and cgroup2 as well; Linux keeps
    // attributes on all three (devtmpfs is a tmpfs, and kernfs has them for
    // cgroup2 -- systemd tags delegated cgroups with trusted.delegate).
    if (mount->fs == &tmpfs || mount->fs == &devtmpfs || mount->fs == &cgroup2fs)
        return &tmpfs_xattr_ops;
    return NULL;
}

static bool has_prefix(const char *name, const char *prefix) {
    return strncmp(name, prefix, strlen(prefix)) == 0;
}

// Linux's xattr_resolve_name, for a filesystem that has attributes: a
// namespace it keeps is fine unless the name is nothing but the prefix.
static int xattr_resolve(const char *name) {
    static const char *const kept[] = {"user.", "trusted.", "security."};
    for (unsigned i = 0; i < sizeof(kept) / sizeof(kept[0]); i++)
        if (has_prefix(name, kept[i]))
            return name[strlen(kept[i])] == '\0' ? _EINVAL : 0;
    return _EOPNOTSUPP;
}

// What a call acts on.
struct xattr_target {
    struct fd *fd;          // an open file, for the f* calls and /proc/PID/fd
    bool fd_retained;       // ...holding a reference of our own on it
    struct mount *mount;    // otherwise a mount, retained,
    char path[MAX_PATH];    // ...and the path on it
    char guest_path[MAX_PATH];
    int mount_flags;
    struct statbuf stat;
};

static struct mount *target_mount(const struct xattr_target *t) {
    return t->fd != NULL ? t->fd->mount : t->mount;
}

static const char *target_path(const struct xattr_target *t) {
    return t->fd != NULL ? NULL : t->path;
}

static void target_init(struct xattr_target *t) {
    t->fd = NULL;
    t->fd_retained = false;
    t->mount = NULL;
    t->path[0] = '\0';
    t->guest_path[0] = '\0';
    t->mount_flags = 0;
}

static void target_release(struct xattr_target *t) {
    if (t->fd != NULL && t->fd_retained)
        fd_close(t->fd);
    if (t->mount != NULL)
        mount_release(t->mount);
    t->fd = NULL;
    t->mount = NULL;
}

// The f* forms take an open file -- not an O_PATH one, which Linux refuses
// here as it refuses I/O (measured: EBADF on 6.12).
static struct fd *xattr_fd(fd_t fd_no) {
    struct fd *fd = f_get(fd_no);
    if (fd == NULL || (fd->flags & O_PATH_))
        return NULL;
    return fd;
}

static int target_from_fd(struct xattr_target *t, struct fd *fd) {
    target_init(t);
    t->fd = fd;
    t->mount_flags = fd->mount_flags;
    return generic_fstat(fd, &t->stat);
}

static int target_from_path(struct xattr_target *t, const char *path, bool follow) {
    target_init(t);
    struct fd *fd = NULL;
    int err = generic_xattr_lookup(AT_PWD, path, follow, &fd, &t->mount, t->path,
            &t->mount_flags, t->guest_path, &t->stat);
    if (err < 0)
        return err;
    if (fd != NULL) {
        t->fd = fd;
        t->fd_retained = true;
    }
    return 0;
}

// Linux's xattr_permission.
static int xattr_permission(const struct xattr_target *t, const char *name, bool write) {
    if (has_prefix(name, "security.") || has_prefix(name, "system."))
        return 0;
    if (has_prefix(name, "trusted.")) {
        // Not even named to anyone else: reading one is ENODATA, as if it
        // were not there.
        if (!current_capable(CAP_SYS_ADMIN_))
            return write ? _EPERM : _ENODATA;
        return 0;
    }
    if (has_prefix(name, "user.")) {
        mode_t_ mode = t->stat.mode;
        // Symlinks, devices, fifos and sockets carry no user attributes. The
        // same answers for a pipe or a socket descriptor.
        if (!S_ISREG(mode) && !S_ISDIR(mode))
            return write ? _EPERM : _ENODATA;
        // In a sticky directory -- /tmp -- a user may not tag the directory
        // itself unless it is theirs.
        if (write && S_ISDIR(mode) && (mode & S_ISVTX_) &&
                current->fsuid != t->stat.uid && !current_capable(CAP_FOWNER_))
            return _EPERM;
    }
    struct statbuf stat = t->stat;
    return access_check(&stat, write ? AC_W : AC_R);
}

static bool caps_valid_header(const void *value, size_t size) {
    if (size != XATTR_CAPS_SZ_2_ && size != XATTR_CAPS_SZ_3_)
        return false;
    dword_t magic;
    memcpy(&magic, value, sizeof(magic));
    dword_t revision = magic & ~VFS_CAP_FLAGS_EFFECTIVE_;
    return size == XATTR_CAPS_SZ_2_ ? revision == VFS_CAP_REVISION_2_
                                    : revision == VFS_CAP_REVISION_3_;
}

// cap_convert_nscap: a v2 or v3 value of exactly its size, then CAP_SETFCAP.
// There is one user namespace here, so a v3 naming its root (0) is the v2 it
// would read back as, and is stored as one; a v3 naming any other root keeps
// it, reads back as that v3, and gives nothing at exec.
static int caps_convert(void *value, size_t *size) {
    if (!caps_valid_header(value, *size))
        return _EINVAL;
    if (!current_capable(CAP_SETFCAP_))
        return _EPERM;
    if (*size == XATTR_CAPS_SZ_3_) {
        struct vfs_ns_cap_data_ caps;
        memcpy(&caps, value, sizeof(caps));
        if (caps.rootid == 0) {
            caps.magic_etc = VFS_CAP_REVISION_2_ | (caps.magic_etc & VFS_CAP_FLAGS_EFFECTIVE_);
            memcpy(value, &caps, XATTR_CAPS_SZ_2_);
            *size = XATTR_CAPS_SZ_2_;
        }
    }
    return 0;
}

// cap_inode_getsecurity: what getxattr hands back for security.capability.
// Only a v2, or a v3 for a root other than ours, was ever valid to store; a
// value that is neither (an empty one, which Linux lets anyone write) reads as
// EINVAL. A v3 for our own root reads back as the v2 it is.
static ssize_t caps_get(const struct xattr_target *t, const struct xattr_ops *ops,
        void *value, size_t size) {
    unsigned char raw[64];
    ssize_t len = ops->get(target_mount(t), target_path(t), t->fd, XATTR_NAME_CAPS_,
            raw, sizeof(raw));
    if (len == _ERANGE)
        return _EINVAL;
    if (len < 0)
        return len;
    if (!caps_valid_header(raw, (size_t) len))
        return _EINVAL;
    if (len == XATTR_CAPS_SZ_3_) {
        struct vfs_ns_cap_data_ caps;
        memcpy(&caps, raw, sizeof(caps));
        if (caps.rootid == 0) {
            caps.magic_etc = VFS_CAP_REVISION_2_ | (caps.magic_etc & VFS_CAP_FLAGS_EFFECTIVE_);
            memcpy(raw, &caps, XATTR_CAPS_SZ_2_);
            len = XATTR_CAPS_SZ_2_;
        }
    }
    if (size == 0)
        return len;
    if ((size_t) len > size)
        return _ERANGE;
    memcpy(value, raw, (size_t) len);
    return len;
}

static void xattr_changed(const struct xattr_target *t) {
    if (t->guest_path[0] != '\0')
        inotify_notify_attrib(t->guest_path);
}

static ssize_t do_getxattr(struct xattr_target *t, const char *name, void *value, size_t size) {
    int err = xattr_permission(t, name, false);
    if (err < 0)
        return err;
    const struct xattr_ops *ops = xattr_ops_for_mount(target_mount(t));
    if (ops == NULL)
        return _EOPNOTSUPP;
    err = xattr_resolve(name);
    if (err < 0)
        return err;
    if (strcmp(name, XATTR_NAME_CAPS_) == 0)
        return caps_get(t, ops, value, size);
    return ops->get(target_mount(t), target_path(t), t->fd, name, value, size);
}

// `value` may be rewritten in place (security.capability's conversion), and
// so must be the caller's own copy.
static int do_setxattr(struct xattr_target *t, const char *name, void *value, size_t size,
        int flags) {
    if (t->mount_flags & MS_READONLY_)
        return _EROFS;
    int err = xattr_permission(t, name, true);
    if (err < 0)
        return err;
    if (has_prefix(name, "security.")) {
        if (strcmp(name, XATTR_NAME_CAPS_) == 0) {
            // An empty value is not converted -- and so not checked at all,
            // which lets anyone put an empty security.capability on a file
            // they may write. It then reads back EINVAL and makes the file
            // unexecutable (EINVAL); measured on 6.12, as uid 1000.
            if (size != 0) {
                err = caps_convert(value, &size);
                if (err < 0)
                    return err;
            }
        } else if (!current_capable(CAP_SYS_ADMIN_)) {
            return _EPERM;
        }
    }
    const struct xattr_ops *ops = xattr_ops_for_mount(target_mount(t));
    if (ops == NULL)
        return _EOPNOTSUPP;
    err = xattr_resolve(name);
    if (err < 0)
        return err;
    err = ops->set(target_mount(t), target_path(t), t->fd, name, value, size, flags);
    if (err >= 0)
        xattr_changed(t);
    return err;
}

static ssize_t do_listxattr(struct xattr_target *t, char *list, size_t size) {
    // No permission is asked: a file's attribute names are anybody's to see,
    // except trusted.* ones, which are left out below.
    const struct xattr_ops *ops = xattr_ops_for_mount(target_mount(t));
    if (ops == NULL)
        return 0;
    char *all = NULL;
    ssize_t len;
    // The list can grow between the two calls; ask again if it did.
    for (int tries = 0; ; tries++) {
        len = ops->list(target_mount(t), target_path(t), t->fd, NULL, 0);
        if (len <= 0)
            return len;
        char *buf = realloc(all, (size_t) len);
        if (buf == NULL) {
            free(all);
            return _ENOMEM;
        }
        all = buf;
        len = ops->list(target_mount(t), target_path(t), t->fd, all, (size_t) len);
        if (len != _ERANGE || tries >= 4)
            break;
    }
    if (len < 0) {
        free(all);
        return len;
    }
    bool show_trusted = current_capable(CAP_SYS_ADMIN_);
    size_t out = 0;
    for (ssize_t i = 0; i < len; ) {
        size_t n = strnlen(all + i, (size_t) (len - i));
        if (show_trusted || !has_prefix(all + i, "trusted.")) {
            memmove(all + out, all + i, n);
            all[out + n] = '\0';
            out += n + 1;
        }
        i += (ssize_t) n + 1;
    }
    ssize_t result;
    if (size == 0)
        result = (ssize_t) out;
    else if (out > size)
        // A list longer than any buffer the call can be given.
        result = size >= XATTR_LIST_MAX_ ? _E2BIG : _ERANGE;
    else {
        memcpy(list, all, out);
        result = (ssize_t) out;
    }
    free(all);
    return result;
}

static int do_removexattr(struct xattr_target *t, const char *name) {
    if (t->mount_flags & MS_READONLY_)
        return _EROFS;
    int err = xattr_permission(t, name, true);
    if (err < 0)
        return err;
    if (has_prefix(name, "security.")) {
        if (!current_capable(strcmp(name, XATTR_NAME_CAPS_) == 0 ? CAP_SETFCAP_ : CAP_SYS_ADMIN_))
            return _EPERM;
    }
    const struct xattr_ops *ops = xattr_ops_for_mount(target_mount(t));
    if (ops == NULL)
        return _EOPNOTSUPP;
    err = xattr_resolve(name);
    if (err < 0)
        return err;
    err = ops->remove(target_mount(t), target_path(t), t->fd, name);
    if (err >= 0)
        xattr_changed(t);
    return err;
}

// ---------------------------------------------------------------- the syscalls

// strncpy_from_user into a 256-byte buffer: EFAULT if it cannot be read, and
// ERANGE if it is empty or does not end within 255 bytes.
static int xattr_import_name(guest_addr_t addr, char name[XATTR_NAME_MAX_ + 1]) {
    if (user_read_string(addr, name, XATTR_NAME_MAX_ + 1) == 0)
        return name[0] == '\0' ? _ERANGE : 0;
    // It failed either because 256 bytes held no terminator or because a byte
    // before the terminator could not be read. Only the first leaves all 256
    // readable.
    char probe[XATTR_NAME_MAX_ + 1];
    if (user_read(addr, probe, sizeof(probe)) == 0)
        return _ERANGE;
    return _EFAULT;
}

// setxattr_copy: the flags, the name, then the value.
static int xattr_import_set(guest_addr_t name_addr, guest_addr_t value_addr, qword_t size,
        int flags, char name[XATTR_NAME_MAX_ + 1], void **value_out) {
    *value_out = NULL;
    if (flags & ~(XATTR_CREATE_ | XATTR_REPLACE_))
        return _EINVAL;
    int err = xattr_import_name(name_addr, name);
    if (err < 0)
        return err;
    if (size == 0)
        return 0;
    if (size > XATTR_SIZE_MAX_)
        return _E2BIG;
    void *value = malloc((size_t) size);
    if (value == NULL)
        return _ENOMEM;
    if (user_read(value_addr, value, (size_t) size)) {
        free(value);
        return _EFAULT;
    }
    *value_out = value;
    return 0;
}

static dword_t setxattr_common(struct xattr_target *t, const char *name, void *value,
        qword_t size, int flags) {
    return (dword_t) do_setxattr(t, name, value, (size_t) size, flags);
}

static dword_t sys_setxattr_path(guest_addr_t path_addr, guest_addr_t name_addr,
        guest_addr_t value_addr, qword_t size, dword_t flags, bool follow) {
    char name[XATTR_NAME_MAX_ + 1];
    void *value;
    int err = xattr_import_set(name_addr, value_addr, size, (int) flags, name, &value);
    if (err < 0)
        return err;
    char path[MAX_PATH];
    err = user_read_path(path_addr, path, sizeof(path));
    if (err < 0) {
        free(value);
        return err;
    }
    STRACE("%ssetxattr(\"%s\", \"%s\", %#llx, %llu, %#x)", follow ? "" : "l", path, name,
            (unsigned long long) value_addr, (unsigned long long) size, flags);
    struct xattr_target t;
    err = target_from_path(&t, path, follow);
    if (err >= 0)
        err = (int) setxattr_common(&t, name, value, size, (int) flags);
    target_release(&t);
    free(value);
    return err;
}

dword_t sys_setxattr_guest(guest_addr_t path_addr, guest_addr_t name_addr,
        guest_addr_t value_addr, qword_t size, dword_t flags) {
    return sys_setxattr_path(path_addr, name_addr, value_addr, size, flags, true);
}

dword_t sys_lsetxattr_guest(guest_addr_t path_addr, guest_addr_t name_addr,
        guest_addr_t value_addr, qword_t size, dword_t flags) {
    return sys_setxattr_path(path_addr, name_addr, value_addr, size, flags, false);
}

dword_t sys_fsetxattr_guest(fd_t fd_no, guest_addr_t name_addr, guest_addr_t value_addr,
        qword_t size, dword_t flags) {
    // The descriptor first: a bad one is EBADF whatever else is wrong.
    struct fd *fd = xattr_fd(fd_no);
    if (fd == NULL)
        return _EBADF;
    char name[XATTR_NAME_MAX_ + 1];
    void *value;
    int err = xattr_import_set(name_addr, value_addr, size, (int) flags, name, &value);
    if (err < 0)
        return err;
    STRACE("fsetxattr(%d, \"%s\", %#llx, %llu, %#x)", fd_no, name,
            (unsigned long long) value_addr, (unsigned long long) size, flags);
    struct xattr_target t;
    err = target_from_fd(&t, fd);
    if (err >= 0)
        err = (int) setxattr_common(&t, name, value, size, (int) flags);
    target_release(&t);
    free(value);
    return err;
}

// The value comes back through a kernel buffer, so a short user buffer is
// ERANGE before anything is written to it. A size past the ceiling is
// clamped rather than refused.
static dword_t getxattr_common(struct xattr_target *t, const char *name,
        guest_addr_t value_addr, qword_t size) {
    if (size > XATTR_SIZE_MAX_)
        size = XATTR_SIZE_MAX_;
    void *value = NULL;
    if (size != 0) {
        value = malloc((size_t) size);
        if (value == NULL)
            return _ENOMEM;
    }
    ssize_t res = do_getxattr(t, name, value, (size_t) size);
    if (res > 0 && size != 0 && user_write(value_addr, value, (size_t) res))
        res = _EFAULT;
    free(value);
    return (dword_t) res;
}

static dword_t sys_getxattr_path(guest_addr_t path_addr, guest_addr_t name_addr,
        guest_addr_t value_addr, qword_t size, bool follow) {
    char path[MAX_PATH];
    int err = user_read_path(path_addr, path, sizeof(path));
    if (err < 0)
        return err;
    struct xattr_target t;
    err = target_from_path(&t, path, follow);
    if (err < 0) {
        target_release(&t);
        return err;
    }
    char name[XATTR_NAME_MAX_ + 1];
    err = xattr_import_name(name_addr, name);
    STRACE("%sgetxattr(\"%s\", \"%s\", %#llx, %llu)", follow ? "" : "l", path,
            err < 0 ? "?" : name, (unsigned long long) value_addr, (unsigned long long) size);
    dword_t res = err < 0 ? (dword_t) err : getxattr_common(&t, name, value_addr, size);
    target_release(&t);
    return res;
}

dword_t sys_getxattr_guest(guest_addr_t path_addr, guest_addr_t name_addr,
        guest_addr_t value_addr, qword_t size) {
    return sys_getxattr_path(path_addr, name_addr, value_addr, size, true);
}

dword_t sys_lgetxattr_guest(guest_addr_t path_addr, guest_addr_t name_addr,
        guest_addr_t value_addr, qword_t size) {
    return sys_getxattr_path(path_addr, name_addr, value_addr, size, false);
}

dword_t sys_fgetxattr_guest(fd_t fd_no, guest_addr_t name_addr, guest_addr_t value_addr,
        qword_t size) {
    struct fd *fd = xattr_fd(fd_no);
    if (fd == NULL)
        return _EBADF;
    char name[XATTR_NAME_MAX_ + 1];
    int err = xattr_import_name(name_addr, name);
    if (err < 0)
        return err;
    STRACE("fgetxattr(%d, \"%s\", %#llx, %llu)", fd_no, name,
            (unsigned long long) value_addr, (unsigned long long) size);
    struct xattr_target t;
    err = target_from_fd(&t, fd);
    dword_t res = err < 0 ? (dword_t) err : getxattr_common(&t, name, value_addr, size);
    target_release(&t);
    return res;
}

static dword_t listxattr_common(struct xattr_target *t, guest_addr_t list_addr, qword_t size) {
    if (size > XATTR_LIST_MAX_)
        size = XATTR_LIST_MAX_;
    char *list = NULL;
    if (size != 0) {
        list = malloc((size_t) size);
        if (list == NULL)
            return _ENOMEM;
    }
    ssize_t res = do_listxattr(t, list, (size_t) size);
    if (res > 0 && size != 0 && user_write(list_addr, list, (size_t) res))
        res = _EFAULT;
    free(list);
    return (dword_t) res;
}

static dword_t sys_listxattr_path(guest_addr_t path_addr, guest_addr_t list_addr,
        qword_t size, bool follow) {
    char path[MAX_PATH];
    int err = user_read_path(path_addr, path, sizeof(path));
    if (err < 0)
        return err;
    STRACE("%slistxattr(\"%s\", %#llx, %llu)", follow ? "" : "l", path,
            (unsigned long long) list_addr, (unsigned long long) size);
    struct xattr_target t;
    err = target_from_path(&t, path, follow);
    dword_t res = err < 0 ? (dword_t) err : listxattr_common(&t, list_addr, size);
    target_release(&t);
    return res;
}

dword_t sys_listxattr_guest(guest_addr_t path_addr, guest_addr_t list_addr, qword_t size) {
    return sys_listxattr_path(path_addr, list_addr, size, true);
}

dword_t sys_llistxattr_guest(guest_addr_t path_addr, guest_addr_t list_addr, qword_t size) {
    return sys_listxattr_path(path_addr, list_addr, size, false);
}

dword_t sys_flistxattr_guest(fd_t fd_no, guest_addr_t list_addr, qword_t size) {
    struct fd *fd = xattr_fd(fd_no);
    if (fd == NULL)
        return _EBADF;
    STRACE("flistxattr(%d, %#llx, %llu)", fd_no, (unsigned long long) list_addr,
            (unsigned long long) size);
    struct xattr_target t;
    int err = target_from_fd(&t, fd);
    dword_t res = err < 0 ? (dword_t) err : listxattr_common(&t, list_addr, size);
    target_release(&t);
    return res;
}

static dword_t sys_removexattr_path(guest_addr_t path_addr, guest_addr_t name_addr, bool follow) {
    char name[XATTR_NAME_MAX_ + 1];
    int err = xattr_import_name(name_addr, name);
    if (err < 0)
        return err;
    char path[MAX_PATH];
    err = user_read_path(path_addr, path, sizeof(path));
    if (err < 0)
        return err;
    STRACE("%sremovexattr(\"%s\", \"%s\")", follow ? "" : "l", path, name);
    struct xattr_target t;
    err = target_from_path(&t, path, follow);
    if (err >= 0)
        err = do_removexattr(&t, name);
    target_release(&t);
    return err;
}

dword_t sys_removexattr_guest(guest_addr_t path_addr, guest_addr_t name_addr) {
    return sys_removexattr_path(path_addr, name_addr, true);
}

dword_t sys_lremovexattr_guest(guest_addr_t path_addr, guest_addr_t name_addr) {
    return sys_removexattr_path(path_addr, name_addr, false);
}

dword_t sys_fremovexattr_guest(fd_t fd_no, guest_addr_t name_addr) {
    struct fd *fd = xattr_fd(fd_no);
    if (fd == NULL)
        return _EBADF;
    char name[XATTR_NAME_MAX_ + 1];
    int err = xattr_import_name(name_addr, name);
    if (err < 0)
        return err;
    STRACE("fremovexattr(%d, \"%s\")", fd_no, name);
    struct xattr_target t;
    err = target_from_fd(&t, fd);
    if (err >= 0)
        err = do_removexattr(&t, name);
    target_release(&t);
    return err;
}

// i386's table passes 32-bit words.
dword_t sys_setxattr(addr_t path, addr_t name, addr_t value, dword_t size, dword_t flags) {
    return sys_setxattr_guest(path, name, value, size, flags);
}
dword_t sys_lsetxattr(addr_t path, addr_t name, addr_t value, dword_t size, dword_t flags) {
    return sys_lsetxattr_guest(path, name, value, size, flags);
}
dword_t sys_fsetxattr(fd_t fd, addr_t name, addr_t value, dword_t size, dword_t flags) {
    return sys_fsetxattr_guest(fd, name, value, size, flags);
}
dword_t sys_getxattr(addr_t path, addr_t name, addr_t value, dword_t size) {
    return sys_getxattr_guest(path, name, value, size);
}
dword_t sys_lgetxattr(addr_t path, addr_t name, addr_t value, dword_t size) {
    return sys_lgetxattr_guest(path, name, value, size);
}
dword_t sys_fgetxattr(fd_t fd, addr_t name, addr_t value, dword_t size) {
    return sys_fgetxattr_guest(fd, name, value, size);
}
dword_t sys_listxattr(addr_t path, addr_t list, dword_t size) {
    return sys_listxattr_guest(path, list, size);
}
dword_t sys_llistxattr(addr_t path, addr_t list, dword_t size) {
    return sys_llistxattr_guest(path, list, size);
}
dword_t sys_flistxattr(fd_t fd, addr_t list, dword_t size) {
    return sys_flistxattr_guest(fd, list, size);
}
dword_t sys_removexattr(addr_t path, addr_t name) {
    return sys_removexattr_guest(path, name);
}
dword_t sys_lremovexattr(addr_t path, addr_t name) {
    return sys_lremovexattr_guest(path, name);
}
dword_t sys_fremovexattr(fd_t fd, addr_t name) {
    return sys_fremovexattr_guest(fd, name);
}

// ------------------------------------------------- exec and killpriv

// get_vfs_caps_from_disk. Read straight from the filesystem, with none of
// getxattr's permission checks: exec does not need the caller to be able to
// read the attribute, only the file to have it.
//
// What it returns is Linux's: 0 and *caps filled for capabilities that apply;
// _ENODATA for none (no attribute, no attributes on this filesystem, or a v3
// written for a root other than ours); _EINVAL for an attribute that is not a
// valid one of any revision, which fails the exec. The last is reachable: an
// empty security.capability can be written by any user who may write the
// file (see do_setxattr).
static int read_file_caps(struct fd *fd, struct file_caps *caps) {
    const struct xattr_ops *ops = xattr_ops_for_mount(fd->mount);
    if (ops == NULL)
        return _ENODATA;
    unsigned char raw[64];
    ssize_t len = ops->get(fd->mount, NULL, fd, XATTR_NAME_CAPS_, raw, sizeof(raw));
    if (len == _ENODATA || len == _EOPNOTSUPP)
        return _ENODATA;
    if (len == _ERANGE)
        return _EINVAL;
    if (len < 0)
        return (int) len;
    if (len < (ssize_t) sizeof(dword_t))
        return _EINVAL;
    struct vfs_ns_cap_data_ data = {};
    memcpy(&data, raw, (size_t) len < sizeof(data) ? (size_t) len : sizeof(data));
    switch (data.magic_etc & VFS_CAP_REVISION_MASK_) {
        case VFS_CAP_REVISION_1_:
            if (len != XATTR_CAPS_SZ_1_)
                return _EINVAL;
            // One 32-bit word: the high halves are not there.
            data.data[1].permitted = data.data[1].inheritable = 0;
            break;
        case VFS_CAP_REVISION_2_:
            if (len != XATTR_CAPS_SZ_2_)
                return _EINVAL;
            break;
        case VFS_CAP_REVISION_3_:
            if (len != XATTR_CAPS_SZ_3_)
                return _EINVAL;
            // rootid_owns_currentns: only this namespace's root owns it.
            if (data.rootid != 0)
                return _ENODATA;
            break;
        default:
            return _EINVAL;
    }
    caps->effective = (data.magic_etc & VFS_CAP_FLAGS_EFFECTIVE_) != 0;
    caps->permitted[0] = data.data[0].permitted;
    caps->inheritable[0] = data.data[0].inheritable;
    // CAP_VALID_MASK: nothing past the last capability this kernel has.
    caps->permitted[1] = data.data[1].permitted & CAP_FULL_HIGH_;
    caps->inheritable[1] = data.data[1].inheritable & CAP_FULL_HIGH_;
    return 0;
}

int xattr_exec_file_caps(struct fd *fd, struct file_caps *caps) {
    return read_file_caps(fd, caps);
}

// Asked on every file's first write, so it looks before it writes: removing is
// a metadata transaction, and nearly every file has nothing to remove.
static void kill_caps(struct mount *mount, const char *path, struct fd *fd) {
    const struct xattr_ops *ops = xattr_ops_for_mount(mount);
    if (ops == NULL)
        return;
    if (ops->get(mount, path, fd, XATTR_NAME_CAPS_, NULL, 0) < 0)
        return;
    ops->remove(mount, path, fd, XATTR_NAME_CAPS_);
}

void xattr_kill_caps_fd(struct fd *fd) {
    kill_caps(fd->mount, NULL, fd);
}

void xattr_kill_caps_path(struct mount *mount, const char *path) {
    kill_caps(mount, path, NULL);
}

void xattr_kill_caps_at(struct fd *at, const char *path, bool follow) {
    struct xattr_target t;
    target_init(&t);
    struct fd *fd = NULL;
    if (generic_xattr_lookup(at, path, follow, &fd, &t.mount, t.path, &t.mount_flags,
                t.guest_path, &t.stat) < 0)
        return;
    if (fd != NULL) {
        kill_caps(fd->mount, NULL, fd);
        fd_close(fd);
    } else {
        kill_caps(t.mount, t.path, NULL);
    }
    target_release(&t);
}
