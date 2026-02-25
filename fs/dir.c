#include <sys/stat.h>
#include <string.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/fd.h"

static unsigned long fd_telldir(struct fd *fd) {
    unsigned long off = fd->offset;
    if (fd->ops->telldir)
        off = fd->ops->telldir(fd);
    return off;
}

static void fd_seekdir(struct fd *fd, unsigned long off) {
    fd->offset = off;

    if (fd->ops->seekdir)
        fd->ops->seekdir(fd, off);
}

struct linux_dirent_ {
    dword_t inode;
    dword_t offset;
    word_t reclen;
    char name[];
} __attribute__((packed));

struct linux_dirent64_ {
    qword_t inode;
    qword_t offset;
    word_t reclen;
    byte_t type;
    char name[];
} __attribute__((packed));

size_t fill_dirent_32(void *dirent_data, ino_t inode, off_t_ offset, const char *name, int type) {
    struct linux_dirent_ *dirent = dirent_data;
    dirent->inode = inode;
    dirent->offset = offset;
    const size_t namelen = strlen(name) + 1; // Include null terminator
    dirent->reclen = offsetof(struct linux_dirent_, name) + namelen + 1; // name, null terminator, type
    memcpy(dirent->name, name, namelen);
    *((char *) dirent + dirent->reclen - 1) = type;
    return dirent->reclen;
}

size_t fill_dirent_64(void *dirent_data, ino_t inode, off_t_ offset, const char *name, int type) {
    struct linux_dirent64_ *dirent = dirent_data;
    dirent->inode = inode;
    dirent->offset = offset;
    const size_t namelen = strlen(name) + 1; // Include null terminator
    dirent->reclen = offsetof(struct linux_dirent64_, name) + namelen; // name, null terminator
    dirent->type = type;
    memcpy(dirent->name, name, namelen);
    return dirent->reclen;
}

int_t sys_getdents_common(fd_t f, addr_t dirents, dword_t count,
        size_t (*fill_dirent)(void *, ino_t, off_t_, const char *, int)) {
    STRACE("getdents(%d, %#x, %#x)", f, dirents, count);
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    if (!S_ISDIR(fd->type) || fd->ops->readdir == NULL)
        return _ENOTDIR;

    dword_t orig_count = count;

    if (fd->ops->readdir_begin)
        fd->ops->readdir_begin(fd);

    long ptr;
    int err;
    int printed = 0;
    while (true) {
        ptr = fd_telldir(fd);
        extern time_t boot_time;
        fd->stat.atime = (dword_t)boot_time;
        struct dir_entry entry;
        err = fd->ops->readdir(fd, &entry);
        if (err < 0)
            goto out;
        if (err == 0)
            break;

        size_t max_reclen = sizeof(struct linux_dirent64_) + strlen(entry.name) + 4;
        char dirent_data[max_reclen];
        dirent_data[0] = 0;
        ino_t inode = entry.inode;
        off_t_ offset = fd_telldir(fd);
        const char *name = entry.name;
        int type = 0;
        size_t reclen = fill_dirent(dirent_data, inode, offset, name, type);
        if (printed < 20) {
            STRACE(" {inode=%d, offset=%d, name=%s, type=%d, reclen=%d}",
                    inode, offset, name, type, reclen);
            printed++;
        }

        if (reclen > count)
            break;
        if (user_write(dirents, dirent_data, reclen)) {
            err = _EFAULT;
            goto out;
        }
        dirents += reclen;
        count -= reclen;
    }

    fd_seekdir(fd, ptr);
    err = orig_count - count;

out:
    if (fd->ops->readdir_end)
        fd->ops->readdir_end(fd);
    return err;
}

int_t sys_getdents(fd_t f, addr_t dirents, uint_t count) {
    return sys_getdents_common(f, dirents, count, fill_dirent_32);
}

int_t sys_getdents64(fd_t f, addr_t dirents, uint_t count) {
    return sys_getdents_common(f, dirents, count, fill_dirent_64);
}

