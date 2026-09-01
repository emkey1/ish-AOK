#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"
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

struct linux_dirent_amd64_ {
    qword_t inode;
    qword_t offset;
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

// Every record is padded so the NEXT one starts aligned. Linux rounds
// getdents64 to 8 and legacy getdents to sizeof(long) -- 4 on a 32-bit ABI,
// 8 on a 64-bit one -- and it matters because the documented way to walk the
// buffer is to cast each record in place and step by d_reclen. Unpadded
// records left every record after the first at an address struct
// linux_dirent64 is not aligned for, which on a strict-alignment target is
// an unaligned load and everywhere else is a size no reader computing
// lengths for itself would predict.
//
// The padding bytes are zeroed: the legacy layout puts d_type in the LAST
// byte of the record, so it moves out to the new end and the gap between the
// name and it has to be defined rather than whatever was on the stack.
static size_t dirent_align(size_t len, size_t align) {
    return (len + align - 1) & ~(align - 1);
}

size_t fill_dirent_32(void *dirent_data, ino_t inode, off_t_ offset, const char *name, size_t namelen, int type) {
    struct linux_dirent_ *dirent = dirent_data;
    size_t reclen = dirent_align(offsetof(struct linux_dirent_, name) + namelen + 1, 4);
    memset(dirent_data, 0, reclen);
    dirent->inode = inode;
    dirent->offset = offset;
    dirent->reclen = reclen;
    memcpy(dirent->name, name, namelen);
    *((char *) dirent + reclen - 1) = type;
    return reclen;
}

size_t fill_dirent_amd64(void *dirent_data, ino_t inode, off_t_ offset, const char *name, size_t namelen, int type) {
    struct linux_dirent_amd64_ *dirent = dirent_data;
    size_t reclen = dirent_align(offsetof(struct linux_dirent_amd64_, name) + namelen + 1, 8);
    memset(dirent_data, 0, reclen);
    dirent->inode = inode;
    dirent->offset = offset;
    dirent->reclen = reclen;
    memcpy(dirent->name, name, namelen);
    *((char *) dirent + reclen - 1) = type;
    return reclen;
}

size_t fill_dirent_64(void *dirent_data, ino_t inode, off_t_ offset, const char *name, size_t namelen, int type) {
    struct linux_dirent64_ *dirent = dirent_data;
    size_t reclen = dirent_align(offsetof(struct linux_dirent64_, name) + namelen, 8);
    memset(dirent_data, 0, reclen);
    dirent->inode = inode;
    dirent->offset = offset;
    dirent->reclen = reclen;
    dirent->type = type;
    memcpy(dirent->name, name, namelen);
    return reclen;
}

static bool ldconfig_dir_trace_enabled(void) {
    return current != NULL && strcmp(current->comm, "ldconfig") == 0;
}

static bool ldconfig_dir_trace_path(const char *path) {
    return path != NULL &&
        (strncmp(path, "/lib/i386-linux-gnu", strlen("/lib/i386-linux-gnu")) == 0 ||
         strncmp(path, "/usr/lib/i386-linux-gnu", strlen("/usr/lib/i386-linux-gnu")) == 0);
}

static void ldconfig_trace_dirent(struct fd *fd, const struct dir_entry *entry, off_t_ offset, size_t reclen) {
    enum { LDCONFIG_DIRENT_TRACE_BUDGET = 512 };
    static unsigned trace_count;
    char path[MAX_PATH];

    if (!ldconfig_dir_trace_enabled() || entry == NULL || trace_count >= LDCONFIG_DIRENT_TRACE_BUDGET)
        return;
    if (generic_getpath(fd, path) < 0 || !ldconfig_dir_trace_path(path))
        return;
    trace_count++;
    fprintf(stderr,
            "ish-ldconfig-dir: pid=%d dir=%s inode=%llu offset=%lld type=%d reclen=%zu name=%s\n",
            current->pid, path,
            (unsigned long long) entry->inode,
            (long long) offset,
            entry->type, reclen, entry->name);
}

int_t sys_getdents_common(fd_t f, guest_addr_t dirents, dword_t count,
        size_t (*fill_dirent)(void *, ino_t, off_t_, const char *, size_t, int)) {
    STRACE("getdents(%d, %#llx, %#x)", f, (unsigned long long) dirents, count);
    // EBADF on an O_PATH directory fd, like Linux: without this any user could
    // enumerate a directory whose permissions refused a normal open.
    struct fd *fd = f_get_io(f);
    if (fd == NULL)
        return _EBADF;
    if (!S_ISDIR(fd->type) || fd->ops->readdir == NULL)
        return _ENOTDIR;

    dword_t orig_count = count;

    // One getdents at a time on this descriptor. The loop below is a
    // read-modify-write of the stream position -- tell, read, tell -- and two
    // threads sharing the fd interleaved it: entries came back twice and
    // others were skipped. Measured on a 400-entry directory read by four
    // threads: 37 duplicates and one lost entry, where Linux delivers every
    // entry exactly once to exactly one caller.
    lock(&fd->dir_pos_lock, 0);

    if (fd->ops->readdir_begin)
        fd->ops->readdir_begin(fd);

    // The position BEFORE the entry about to be read. It used to be taken
    // afresh at the top of every iteration, so each entry cost two host
    // telldir() calls instead of one -- and on Darwin a telldir allocates a
    // cookie onto a list that later telldir/seekdir calls walk, which is what
    // made a directory scan cost more per entry the larger the directory got.
    // The position after entry N is by definition the position before entry
    // N+1, so it is carried forward.
    long ptr = fd_telldir(fd);
    int err;
    int printed = 0;
    while (true) {
        extern time_t boot_time;
        fd->stat.atime = (dword_t)boot_time;
        struct dir_entry entry = {.type = DT_UNKNOWN};
        err = fd->ops->readdir(fd, &entry);
        if (err < 0)
            goto out;
        if (err == 0)
            break;

        size_t name_len = strlen(entry.name);
        // Room for the largest layout plus its alignment padding.
        size_t max_reclen = sizeof(struct linux_dirent_amd64_) + name_len + 16;
        char dirent_data[max_reclen];
        dirent_data[0] = 0;
        ino_t inode = entry.inode;
        off_t_ offset = fd_telldir(fd);
        const char *name = entry.name;
        int type = entry.type;
        size_t reclen = fill_dirent(dirent_data, inode, offset, name, name_len + 1, type);
        ldconfig_trace_dirent(fd, &entry, offset, reclen);
        if (printed < 20) {
            STRACE(" {inode=%d, offset=%d, name=%s, type=%d, reclen=%d}",
                    inode, offset, name, type, reclen);
            printed++;
        }

        if (reclen > count) {
            // Leave the directory position at the start of the entry that did
            // not fit so the next call can return it.
            fd_seekdir(fd, ptr);
            // Linux returns EINVAL when the buffer is too small to hold even the
            // first entry, rather than reporting a spurious end-of-directory.
            if (count == orig_count) {
                err = _EINVAL;
                goto out;
            }
            break;
        }
        if (user_write(dirents, dirent_data, reclen)) {
            // Entries already stored are NOT lost. Linux returns a short but
            // valid byte count and leaves the directory position exactly
            // after the last stored entry, so a follow-up call returns the
            // rest; reporting EFAULT here threw away everything copied so far
            // AND advanced past it, so those entries were gone for good. Only
            // a fault before anything was stored is EFAULT.
            fd_seekdir(fd, ptr);
            if (count == orig_count) {
                err = _EFAULT;
                goto out;
            }
            break;
        }
        dirents += reclen;
        count -= reclen;
        ptr = offset;
    }

    err = orig_count - count;

out:
    if (fd->ops->readdir_end)
        fd->ops->readdir_end(fd);
    unlock(&fd->dir_pos_lock);
    return err;
}

int_t sys_getdents(fd_t f, addr_t dirents, uint_t count) {
    return sys_getdents_common(f, dirents, count, fill_dirent_32);
}

int_t sys_getdents64(fd_t f, addr_t dirents, uint_t count) {
    return sys_getdents_common(f, dirents, count, fill_dirent_64);
}

int_t sys_getdents_guest(fd_t f, guest_addr_t dirents, dword_t count) {
    return sys_getdents_common(f, dirents, count, fill_dirent_32);
}

int_t sys_getdents_amd64(fd_t f, addr_t dirents, uint_t count) {
    return sys_getdents_common(f, dirents, count, fill_dirent_amd64);
}

int_t sys_getdents_amd64_guest(fd_t f, guest_addr_t dirents, dword_t count) {
    return sys_getdents_common(f, dirents, count, fill_dirent_amd64);
}

int_t sys_getdents64_guest(fd_t f, guest_addr_t dirents, dword_t count) {
    return sys_getdents_common(f, dirents, count, fill_dirent_64);
}
