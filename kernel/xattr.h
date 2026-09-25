#ifndef KERNEL_XATTR_H
#define KERNEL_XATTR_H

#include "misc.h"

struct fd;
struct mount;

// linux/xattr.h
#define XATTR_CREATE_ 1
#define XATTR_REPLACE_ 2
#define XATTR_NAME_MAX_ 255
#define XATTR_SIZE_MAX_ 65536
#define XATTR_LIST_MAX_ 65536

#define XATTR_NAME_CAPS_ "security.capability"

// A file's capabilities (security.capability), as exec applies them: Linux's
// cpu_vfs_cap_data.
struct file_caps {
    bool effective;
    dword_t permitted[2];
    dword_t inheritable[2];
};

// The capabilities an exec of the open file `fd` takes from it: Linux's
// get_vfs_caps_from_disk. 0 with *caps filled for a well-formed attribute of
// revision 1, 2 or 3 whose root id is this namespace's root; _ENODATA for none
// that apply (no attribute, or one written for another root); _EINVAL for an
// attribute that is no valid revision at all, which fails the exec.
int xattr_exec_file_caps(struct fd *fd, struct file_caps *caps);

// Linux's killpriv: a file that is written, truncated or chowned loses its
// capabilities, whoever does it. No permission is asked -- the change that
// triggers it was already allowed. Harmless on a file without them.
void xattr_kill_caps_fd(struct fd *fd);
void xattr_kill_caps_path(struct mount *mount, const char *path);
// By path, resolved as the xattr calls resolve it.
void xattr_kill_caps_at(struct fd *at, const char *path, bool follow);

#endif
