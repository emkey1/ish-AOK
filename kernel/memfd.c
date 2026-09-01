#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "kernel/calls.h"
#include "kernel/fs.h"
#include "fs/poll.h"
#include "fs/real.h"

// Note: MFD_* flag values are their own namespace, NOT O_* values.
// MFD_CLOEXEC is 0x0001 in the Linux ABI (it was O_CLOEXEC_ here, so every
// caller passing the real MFD_CLOEXEC got EINVAL from the unknown-flags check).
#define MFD_CLOEXEC_ 0x0001
#define MFD_ALLOW_SEALING_ 0x0002
#define MFD_HUGETLB_ 0x0004
#define MFD_NOEXEC_SEAL_ 0x0008
#define MFD_EXEC_ 0x0010
#define MEMFD_MAX_NAME 249

#define F_SEAL_SEAL_ 0x0001
#define F_SEAL_SHRINK_ 0x0002
#define F_SEAL_GROW_ 0x0004
#define F_SEAL_WRITE_ 0x0008
#define F_SEAL_FUTURE_WRITE_ 0x0010
#define F_SEAL_EXEC_ 0x0020
#define MEMFD_ALL_SEALS_ (F_SEAL_SEAL_ | F_SEAL_SHRINK_ | F_SEAL_GROW_ | \
        F_SEAL_WRITE_ | F_SEAL_FUTURE_WRITE_ | F_SEAL_EXEC_)

// A memfd's contents live in an unlinked host temp file (host_fd), created
// eagerly at memfd_create time — a memfd exists specifically to be mmapped, so
// unlike tmpfs there's no point in a malloc-backed fast path. Guest mappings
// are host mmaps of that file (host_fd_mmap, same machinery as realfs/tmpfs),
// so the host kernel provides MAP_SHARED write-back, mmap<->read/write
// coherence, MAP_PRIVATE COW, and ftruncate interaction. File I/O goes through
// pread/pwrite/ftruncate on the same fd. The logical size is fd->stat.size,
// kept in sync with the host file's size (guarded by lock).
struct memfd_state {
    char *name;
    int host_fd;
    int seals; // F_SEAL_*, guarded by lock; F_SEAL_SEAL_ set = sealing forbidden
    // How many live SHARED mappings of this memfd exist. F_SEAL_WRITE is
    // refused with EBUSY while it is nonzero -- sealing against writes while
    // somebody holds a shared window onto the data would be a guarantee the
    // kernel cannot keep.
    //
    // Any shared mapping counts, not just a currently-writable one: a
    // PROT_READ MAP_SHARED mapping of a memfd can be mprotected to writable
    // afterwards, so it is just as much of a hole. Measured -- Linux refuses
    // for a read-only shared mapping too, and allows it for a writable PRIVATE
    // one, which is the opposite of what "writable" would suggest. Guarded by
    // lock.
    unsigned shared_maps;
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
    .fake_dev = FAKE_DEV_MINOR_MEMFD, // Linux memfds live on the internal shm tmpfs (anon dev)
};

static struct memfd_state *memfd_state_get(struct fd *fd) {
    return fd->fs_data;
}

static int memfd_resize_locked(struct fd *fd, size_t new_size) {
    struct memfd_state *state = memfd_state_get(fd);
    // ftruncate keeps live guest mappings coherent, and the host zero-fills
    // growth.
    if (ftruncate(state->host_fd, new_size) < 0)
        return errno_map();
    fd->stat.size = new_size;
    return 0;
}

static ssize_t memfd_pread(struct fd *fd, void *buf, size_t bufsize, off_t off) {
    struct memfd_state *state = memfd_state_get(fd);
    // The host file's size always matches fd->stat.size, so the host clamps
    // reads at EOF (and returns 0 past it) exactly like the guest expects.
    ssize_t n = pread(state->host_fd, buf, bufsize, off);
    if (n < 0)
        return errno_map();
    return n;
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
        off = fd->stat.size;
    if (off < 0) {
        unlock(&state->lock);
        return _EINVAL;
    }
    if ((qword_t) off + bufsize > UINT32_MAX) {
        unlock(&state->lock);
        return _EFBIG;
    }
    // Linux (shmem_file_write_iter): sealed-for-write is EPERM, as is a write
    // that would grow a GROW-sealed file.
    if (state->seals & (F_SEAL_WRITE_ | F_SEAL_FUTURE_WRITE_)) {
        unlock(&state->lock);
        return _EPERM;
    }
    if ((state->seals & F_SEAL_GROW_) && (qword_t) off + bufsize > fd->stat.size) {
        unlock(&state->lock);
        return _EPERM;
    }
    ssize_t n = pwrite(state->host_fd, buf, bufsize, off);
    if (n < 0) {
        unlock(&state->lock);
        return errno_map();
    }
    if (fd->stat.size < (qword_t) off + n)
        fd->stat.size = off + n;
    unlock(&state->lock);
    return n;
}

static ssize_t memfd_write(struct fd *fd, const void *buf, size_t bufsize) {
    ssize_t res = memfd_pwrite(fd, buf, bufsize, fd->offset);
    if (res > 0) {
        struct memfd_state *state = memfd_state_get(fd);
        lock(&state->lock, 0);
        if (fd->flags & O_APPEND_)
            fd->offset = fd->stat.size;
        else
            fd->offset += res;
        unlock(&state->lock);
    }
    return res;
}

static off_t_ memfd_lseek(struct fd *fd, off_t_ off, int whence) {
    struct memfd_state *state = memfd_state_get(fd);
    lock(&state->lock, 0);
    qword_t size = fd->stat.size;
    unlock(&state->lock);
    int err = generic_seek(fd, off, whence, size);
    if (err < 0)
        return err;
    return fd->offset;
}

static int memfd_mmap(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags) {
    struct memfd_state *state = memfd_state_get(fd);
    // No access-mode checks: a memfd is always O_RDWR (Linux: F_GETFL reports
    // O_RDWR), so every prot/flags combination is permitted. Linux (verified)
    // even allows mapping past EOF (e.g. a zero-size memfd) — access faults,
    // but the mmap itself succeeds — which the host mmap matches.
    // Linux (verified): a file offset that isn't page-aligned is EINVAL.
    if (offset % PAGE_SIZE != 0)
        return _EINVAL;
    lock(&state->lock, 0);
    int seals = state->seals;
    unlock(&state->lock);
    // Linux: a writable shared mapping of a write-sealed memfd is EPERM.
    if ((flags & MMAP_SHARED) && (prot & P_WRITE) &&
            (seals & (F_SEAL_WRITE_ | F_SEAL_FUTURE_WRITE_)))
        return _EPERM;
    int err = host_fd_mmap(state->host_fd, mem, start, pages, offset, prot, flags);
    if (err < 0)
        return err;

    // Count a live writable shared mapping, so a later F_SEAL_WRITE can refuse
    // with EBUSY rather than granting a seal this mapping can walk straight
    // through. The marker goes on the mapping itself because the count has to
    // come off when the mapping does, and that happens in emu/memory.c.
    if (flags & MMAP_SHARED) {
        struct pt_entry *entry = mem_pt(mem, start);
        if (entry != NULL && entry->data != NULL && !entry->data->memfd_shared_mapped) {
            entry->data->memfd_shared_mapped = true;
            lock(&state->lock, 0);
            state->shared_maps++;
            unlock(&state->lock);
        }
    }
    return 0;
}

void memfd_mapping_released(struct fd *fd) {
    if (fd == NULL || fd->ops != &memfd_ops)
        return;
    struct memfd_state *state = memfd_state_get(fd);
    lock(&state->lock, 0);
    if (state->shared_maps > 0)
        state->shared_maps--;
    unlock(&state->lock);
}

static int memfd_poll(struct fd *UNUSED(fd)) {
    return POLL_READ | POLL_WRITE;
}

static int memfd_close(struct fd *fd) {
    struct memfd_state *state = memfd_state_get(fd);
    close(state->host_fd);
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
            // Linux: ftruncate on a sealed memfd is EPERM in the offending
            // direction (shrink under F_SEAL_SHRINK, grow under F_SEAL_GROW).
            else if ((qword_t) attr.size < fd->stat.size && (state->seals & F_SEAL_SHRINK_))
                err = _EPERM;
            else if ((qword_t) attr.size > fd->stat.size && (state->seals & F_SEAL_GROW_))
                err = _EPERM;
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
    // Linux (verified): readlink(/proc/self/fd/N) shows "/memfd:name (deleted)"
    snprintf(buf, MAX_PATH, "/memfd:%s (deleted)", state->name);
    unlock(&state->lock);
    return 0;
}

static struct fd_ops memfd_ops = {
    .read = memfd_read,
    .write = memfd_write,
    .pread = memfd_pread,
    .pwrite = memfd_pwrite,
    .lseek = memfd_lseek,
    .mmap = memfd_mmap,
    .poll = memfd_poll,
    .fsync = memfd_fsync,
    .close = memfd_close,
};

// fcntl(F_ADD_SEALS/F_GET_SEALS), dispatched from sys_fcntl. Only memfds
// support sealing here (Linux: shmem/hugetlb files); anything else is EINVAL.
int_t memfd_add_seals(struct fd *fd, uint_t arg) {
    if (fd->ops != &memfd_ops)
        return _EINVAL;
    if (arg & ~MEMFD_ALL_SEALS_)
        return _EINVAL;
    struct memfd_state *state = memfd_state_get(fd);
    lock(&state->lock, 0);
    if (state->seals & F_SEAL_SEAL_) {
        unlock(&state->lock);
        return _EPERM;
    }
    // F_SEAL_WRITE while a shared mapping is live is EBUSY. Granting it was
    // worse than not implementing sealing at all: F_GET_SEALS then reported a
    // guarantee that was not being kept, stores through the live mapping still
    // landed, and a receiver that mapped the "sealed" fd afterwards saw the
    // mutations.
    if ((arg & F_SEAL_WRITE_) && state->shared_maps > 0) {
        unlock(&state->lock);
        return _EBUSY;
    }
    state->seals |= arg;
    unlock(&state->lock);
    return 0;
}

int_t memfd_get_seals(struct fd *fd) {
    if (fd->ops != &memfd_ops)
        return _EINVAL;
    struct memfd_state *state = memfd_state_get(fd);
    lock(&state->lock, 0);
    int seals = state->seals;
    unlock(&state->lock);
    return seals;
}

int_t sys_memfd_create(addr_t name_addr, uint_t flags) {
    return sys_memfd_create_guest(name_addr, flags);
}

int_t sys_memfd_create_guest(guest_addr_t name_addr, uint_t flags) {
    // user_read_path rather than user_read_string, because it tells a name
    // that is merely too long apart from one that faulted -- the two are the
    // same return value from user_read_string, and reporting EFAULT for a
    // long name sent the caller looking for a bad pointer it did not have.
    // Linux's is a plain length check: strncpy_from_user into a
    // MFD_NAME_MAX_LEN+1 buffer, EINVAL if it does not fit.
    char name[MEMFD_MAX_NAME + 1];
    int name_err = user_read_path(name_addr, name, sizeof(name));
    if (name_err == _ENAMETOOLONG)
        return _EINVAL;
    if (name_err < 0)
        return _EFAULT;
    STRACE("memfd_create(\"%s\", %#x)", name, flags);

    if (flags & MFD_HUGETLB_)
        return _ENOSYS;
    if (flags & ~(MFD_CLOEXEC_ | MFD_ALLOW_SEALING_ | MFD_HUGETLB_ | MFD_NOEXEC_SEAL_ | MFD_EXEC_))
        return _EINVAL;
    if ((flags & MFD_NOEXEC_SEAL_) && (flags & MFD_EXEC_))
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
    state->host_fd = host_unlinked_tmpfd();
    if (state->host_fd < 0) {
        int err = state->host_fd;
        free(state->name);
        free(state);
        return err;
    }
    // Linux: without MFD_ALLOW_SEALING the file starts permanently sealed
    // against sealing (F_SEAL_SEAL). MFD_NOEXEC_SEAL implies sealing is
    // allowed and pre-applies F_SEAL_EXEC.
    if (flags & MFD_NOEXEC_SEAL_)
        state->seals = F_SEAL_EXEC_;
    else if (!(flags & MFD_ALLOW_SEALING_))
        state->seals = F_SEAL_SEAL_;
    lock_init(&state->lock, "memfd_state\0");

    struct fd *fd = fd_create(&memfd_ops);
    if (fd == NULL) {
        close(state->host_fd);
        free(state->name);
        free(state);
        return _ENOMEM;
    }
    static _Atomic ino_t next_inode = 1;
    mount_retain(&memfd_mount);
    fd->mount = &memfd_mount;
    fd->type = S_IFREG;
    // Linux (verified): a memfd is O_RDWR (F_GETFL) with mode 0777 (0666 under
    // MFD_NOEXEC_SEAL) and nlink 0.
    fd->flags = O_RDWR_;
    fd->stat = (struct statbuf) {};
    fd->stat.inode = next_inode++;
    fd->stat.mode = S_IFREG | ((flags & MFD_NOEXEC_SEAL_) ? 0666 : 0777);
    fd->stat.uid = current->euid;
    fd->stat.gid = current->egid;
    fd->fs_data = state;
    return f_install(fd, (flags & MFD_CLOEXEC_) ? O_CLOEXEC_ : 0);
}
