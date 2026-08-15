// Guest-filesystem access for natively-implemented programs. See
// kernel/native_io.h for why native code must not use the host libc for any of
// this.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/native_io.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/path.h"

// Every entry point checks for a task first. A native program can be on a
// thread AOK never created (one the program spawned itself), where `current`
// is NULL -- and f_get/generic_openat dereference it. That took the whole app
// down with EXC_BAD_ACCESS when SmallCLUE ran its editor on its own pthread.
// kernel/native_libc.c now propagates the task into such threads, so this
// should not trigger; it is here because the failure mode is a dead app rather
// than a failed call, and that is not a thing to leave to one mechanism.
bool native_have_task(void) {
    return current != NULL && current->files != NULL;
}

int native_open(const char *path, int flags, struct fd **fd_out) {
    if (!native_have_task())
        return _EFAULT;
    if (path == NULL || fd_out == NULL)
        return _EINVAL;
    // AT_PWD, not an absolute-only open: relative paths have to resolve
    // against the calling task's cwd, exactly as they would for the guest.
    struct fd *fd = generic_openat(AT_PWD, path, flags, 0);
    if (IS_ERR(fd))
        return (int) PTR_ERR(fd);
    *fd_out = fd;
    return 0;
}

void native_close(struct fd *fd) {
    if (fd != NULL)
        fd_close(fd);
}

ssize_t native_read(struct fd *fd, void *buf, size_t size) {
    if (!native_have_task())
        return _EFAULT;

    if (fd == NULL || buf == NULL)
        return _EINVAL;
    if (S_ISDIR(fd->type))
        return _EISDIR;

    ssize_t res;
    // Same blocking discipline the read(2) path uses: a native program runs on
    // the guest task's own thread, so a blocking read has to be visible to the
    // scheduler as that task blocking.
    TASK_MAY_BLOCK {
        if (fd->ops->read != NULL) {
            res = fd->ops->read(fd, buf, size);
        } else if (fd->ops->pread != NULL) {
            res = fd->ops->pread(fd, buf, size, fd->offset);
            if (res > 0)
                fd->ops->lseek(fd, res, LSEEK_CUR);
        } else {
            res = _EBADF;
        }
    }
    return res;
}

int native_readdir(struct fd *fd, struct dir_entry *entry) {
    if (fd == NULL || entry == NULL)
        return _EINVAL;
    if (fd->ops->readdir == NULL)
        return _ENOTDIR;
    return fd->ops->readdir(fd, entry);
}

int native_stat(const char *path, struct statbuf *stat, bool follow_links) {
    if (!native_have_task())
        return _EFAULT;

    if (path == NULL || stat == NULL)
        return _EINVAL;
    return generic_statat(AT_PWD, path, stat,
            follow_links ? 0 : AT_SYMLINK_NOFOLLOW_);
}

int native_getcwd(char *buf) {
    if (!native_have_task())
        return _EFAULT;

    if (buf == NULL)
        return _EINVAL;
    lock(&current->fs->lock, 0);
    struct fd *pwd = current->fs->pwd;
    int err = pwd != NULL ? generic_getpath(pwd, buf) : _ENOENT;
    unlock(&current->fs->lock);
    return err;
}

ssize_t native_write(fd_t fd_no, const void *buf, size_t size) {
    if (!native_have_task())
        return _EFAULT;

    return fd_write_host_buf(fd_no, buf, size);
}

void native_printf(fd_t fd_no, const char *fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0)
        return;
    if ((size_t) n >= sizeof(buf))
        n = (int) sizeof(buf) - 1;
    native_write(fd_no, buf, (size_t) n);
}
