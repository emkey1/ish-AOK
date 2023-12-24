#include <string.h>
#include <sys/stat.h>
#include "debug.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "kernel/errno.h"

static struct mount adhoc_mount;

struct fd *adhoc_fd_create(const struct fd_ops *ops) {
    struct fd *fd = fd_create(ops);
    if (fd == NULL)
        return NULL;
    mount_retain(&adhoc_mount);
    fd->mount = &adhoc_mount;
    fd->stat = (struct statbuf) {};
    return fd;
}

static int adhoc_fstat(struct fd *fd, struct statbuf *stat) {
    *stat = fd->stat;
    return 0;
}

static int adhoc_fsetattr(struct fd *fd, struct attr attr) {
    switch (attr.type) {
        case attr_uid:
            fd->stat.uid = attr.uid;
            break;
        case attr_gid:
            fd->stat.gid = attr.gid;
            break;
        case attr_mode:
            fd->stat.mode = (fd->stat.mode & S_IFMT) | (attr.mode & ~S_IFMT);
            break;
        case attr_size:
            return _EINVAL;
    }
    return 0;
}

static int adhoc_getpath(struct fd *fd, char *buf) {
    // Need to specify max path size
    const char *type = "unknown"; // TODO allow this to be customized
    size_t buf_size = 4096; // A size that should be sufficient for the formatted string

    if (fd->stat.inode == 0)
        snprintf(buf, buf_size, "anon_inode:[%s]", type);
    else
        snprintf(buf, buf_size, "%s:[%lu]", type, (unsigned long) fd->stat.inode);
    return 0;
}

bool is_adhoc_fd(struct fd *fd) {
    return fd->mount == &adhoc_mount;
}

static const struct fs_ops adhoc_fs = {
    .magic = 0x09041934, // FIXME wrong for pipes and sockets
    .fstat = adhoc_fstat,
    .fsetattr = adhoc_fsetattr,
    .getpath = adhoc_getpath,
};

static struct mount adhoc_mount = {
    .fs = &adhoc_fs,
    .point = "",
};
