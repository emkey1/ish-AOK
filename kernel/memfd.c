#include <string.h>
#include <sys/stat.h>
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/poll.h"

#define MFD_CLOEXEC_ O_CLOEXEC_
#define MFD_ALLOW_SEALING_ 0x0002
#define MFD_HUGETLB_ 0x0004
#define MEMFD_MAX_NAME 249

struct memfd_state {
    char *name;
    byte_t *data;
    size_t size;
    bool sealing_allowed;
    lock_t lock;
};

static struct fd_ops memfd_ops;

static int memfd_fstat(struct fd *fd, struct statbuf *stat);
static int memfd_fsetattr(struct fd *fd, struct attr attr);
static int memfd_getpath(struct fd *fd, char *buf);

static const struct fs_ops memfd_fs = {
    .name = "memfd",
    .magic = 0x4d454d46,
    .fstat = memfd_fstat,
    .fsetattr = memfd_fsetattr,
    .getpath = memfd_getpath,
};

static struct mount memfd_mount = {
    .point = "",
    .fs = &memfd_fs,
};

static struct memfd_state *memfd_state_get(struct fd *fd) {
    return fd->fs_data;
}

static int memfd_resize_locked(struct fd *fd, size_t new_size) {
    struct memfd_state *state = memfd_state_get(fd);
    size_t old_size = state->size;
    void *new_data = realloc(state->data, new_size);
    if (new_data == NULL && new_size != 0)
        return _ENOMEM;
    state->data = new_data;
    state->size = new_size;
    fd->stat.size = new_size;
    if (new_size > old_size)
        memset(state->data + old_size, 0, new_size - old_size);
    return 0;
}

static ssize_t memfd_pread(struct fd *fd, void *buf, size_t bufsize, off_t off) {
    struct memfd_state *state = memfd_state_get(fd);
    lock(&state->lock, 0);
    if ((qword_t) off >= state->size) {
        unlock(&state->lock);
        return 0;
    }
    if (bufsize > state->size - off)
        bufsize = state->size - off;
    memcpy(buf, state->data + off, bufsize);
    unlock(&state->lock);
    return bufsize;
}

static ssize_t memfd_read(struct fd *fd, void *buf, size_t bufsize) {
    ssize_t res = memfd_pread(fd, buf, bufsize, fd->offset);
    if (res > 0)
        fd->offset += res;
    return res;
}

static ssize_t memfd_pwrite(struct fd *fd, const void *buf, size_t bufsize, off_t off) {
    struct memfd_state *state = memfd_state_get(fd);
    lock(&state->lock, 0);
    if (fd->flags & O_APPEND_)
        off = state->size;
    if (off < 0) {
        unlock(&state->lock);
        return _EINVAL;
    }
    if ((qword_t) off + bufsize > UINT32_MAX) {
        unlock(&state->lock);
        return _EFBIG;
    }
    if (state->size < (size_t) off + bufsize) {
        int err = memfd_resize_locked(fd, off + bufsize);
        if (err < 0) {
            unlock(&state->lock);
            return err;
        }
    }
    memcpy(state->data + off, buf, bufsize);
    unlock(&state->lock);
    return bufsize;
}

static ssize_t memfd_write(struct fd *fd, const void *buf, size_t bufsize) {
    ssize_t res = memfd_pwrite(fd, buf, bufsize, fd->offset);
    if (res > 0) {
        struct memfd_state *state = memfd_state_get(fd);
        lock(&state->lock, 0);
        if (fd->flags & O_APPEND_)
            fd->offset = state->size;
        else
            fd->offset += res;
        unlock(&state->lock);
    }
    return res;
}

static off_t_ memfd_lseek(struct fd *fd, off_t_ off, int whence) {
    struct memfd_state *state = memfd_state_get(fd);
    lock(&state->lock, 0);
    qword_t size = state->size;
    unlock(&state->lock);
    int err = generic_seek(fd, off, whence, size);
    if (err < 0)
        return err;
    return fd->offset;
}

static int memfd_poll(struct fd *UNUSED(fd)) {
    return POLL_READ | POLL_WRITE;
}

static int memfd_close(struct fd *fd) {
    struct memfd_state *state = memfd_state_get(fd);
    free(state->data);
    free(state->name);
    free(state);
    fd->fs_data = NULL;
    return 0;
}

static int memfd_fsync(struct fd *UNUSED(fd)) {
    return 0;
}

static int memfd_fstat(struct fd *fd, struct statbuf *stat) {
    struct memfd_state *state = memfd_state_get(fd);
    lock(&state->lock, 0);
    *stat = fd->stat;
    unlock(&state->lock);
    return 0;
}

static int memfd_fsetattr(struct fd *fd, struct attr attr) {
    struct memfd_state *state = memfd_state_get(fd);
    lock(&state->lock, 0);
    int err = 0;
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
            if (attr.size < 0)
                err = _EINVAL;
            else
                err = memfd_resize_locked(fd, attr.size);
            break;
    }
    unlock(&state->lock);
    return err;
}

static int memfd_getpath(struct fd *fd, char *buf) {
    struct memfd_state *state = memfd_state_get(fd);
    lock(&state->lock, 0);
    snprintf(buf, MAX_PATH, "memfd:%s", state->name);
    unlock(&state->lock);
    return 0;
}

static struct fd_ops memfd_ops = {
    .read = memfd_read,
    .write = memfd_write,
    .pread = memfd_pread,
    .pwrite = memfd_pwrite,
    .lseek = memfd_lseek,
    .poll = memfd_poll,
    .fsync = memfd_fsync,
    .close = memfd_close,
};

int_t sys_memfd_create(addr_t name_addr, uint_t flags) {
    char name[MEMFD_MAX_NAME + 1];
    if (user_read_string(name_addr, name, sizeof(name)))
        return _EFAULT;
    STRACE("memfd_create(\"%s\", %#x)", name, flags);

    if (flags & MFD_HUGETLB_)
        return _ENOSYS;
    if (flags & ~(MFD_CLOEXEC_ | MFD_ALLOW_SEALING_ | MFD_HUGETLB_))
        return _EINVAL;

    struct memfd_state *state = malloc(sizeof(struct memfd_state));
    if (state == NULL)
        return _ENOMEM;
    *state = (struct memfd_state) {};
    state->name = strdup(name);
    if (state->name == NULL) {
        free(state);
        return _ENOMEM;
    }
    state->sealing_allowed = (flags & MFD_ALLOW_SEALING_) != 0;
    lock_init(&state->lock, "memfd_state\0");

    struct fd *fd = fd_create(&memfd_ops);
    if (fd == NULL) {
        free(state->name);
        free(state);
        return _ENOMEM;
    }
    static _Atomic ino_t next_inode = 1;
    mount_retain(&memfd_mount);
    fd->mount = &memfd_mount;
    fd->type = S_IFREG;
    fd->stat = (struct statbuf) {};
    fd->stat.inode = next_inode++;
    fd->stat.mode = S_IFREG | 0777;
    fd->stat.uid = current->euid;
    fd->stat.gid = current->egid;
    fd->fs_data = state;
    return f_install(fd, flags);
}
