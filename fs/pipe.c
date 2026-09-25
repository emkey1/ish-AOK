#include <sys/stat.h>
#include <unistd.h>
#include "kernel/calls.h"
#include "fs/fd.h"
#include "fs/real.h"
#include "debug.h"

static struct fd *pipe_fd_new(int pipe_fd, qword_t shared_inode, bool write_end) {
    struct fd *fd = adhoc_fd_create(&realfs_fdops);
    if (fd == NULL)
        return NULL;
    fd->real_fd = pipe_fd;
    // BOTH ENDS SHARE ONE INODE, as they do on Linux -- /proc/<pid>/fd shows
    // the same pipe:[N] at each end, which is how anything reading that file
    // (lsof, a process tree viewer) works out that two descriptors are two
    // ends of the same pipe. adhoc_fd_create hands out a unique inode per fd,
    // which made every end a pipe of its own.
    //
    // kernel/checkpoint.c depends on it for the same reason: it is the only
    // thing in the guest that says these two descriptors have to come back as
    // one host pipe rather than two.
    fd->stat.inode = shared_inode;
    // And the direction, which was also left at 0 (O_RDONLY) for both ends.
    // Linux opens the read end O_RDONLY and the write end O_WRONLY, and a
    // guest asking fcntl(F_GETFL) about a pipe it writes to was told it was
    // read-only.
    fd->flags |= write_end ? O_WRONLY_ : O_RDONLY_;
    // fd->type as well as stat.mode: it is the field the rest of the kernel
    // asks "what kind of thing is this fd", and it was left 0 here, so a pipe
    // answered "not a fifo, not a directory, not a regular file" to anything
    // that checked -- fallocate on a pipe came back ENODEV instead of ESPIPE.
    fd->type = S_IFIFO;
    // get_pipe_inode: S_IFIFO | S_IRUSR | S_IWUSR, owned by the creator's
    // FILESYSTEM ids. This had 0660 and the real uid, so a setuid program's
    // pipe belonged to the user who ran it. Measured on 6.12 as root after
    // setresuid(-1, 65534, -1): mode 10600, st_uid 65534. Sockets follow the
    // same rule; see sock_fd_adopt.
    fd->stat.mode = S_IFIFO | 0600;
    fd->stat.uid = current->fsuid;
    fd->stat.gid = current->fsgid;
    fd->stat.ctime = (dword_t)time(NULL);
    return fd;
}

static fd_t pipe_f_create(int pipe_fd, int flags, qword_t shared_inode, bool write_end) {
    struct fd *fd = pipe_fd_new(pipe_fd, shared_inode, write_end);
    if (fd == NULL)
        return _ENOMEM;
    return f_install(fd, flags);
}

// Build both ends of a pipe WITHOUT installing them in the descriptor table.
//
// For kernel/checkpoint.c, which has to place them at the numbers the image
// names, in whichever processes held them -- and has to create the host pipe
// exactly once for a pair that two different processes were holding the two
// ends of. sys_pipe2 above is the ordinary caller and still does its own
// installing.
//
// Returns 0 with both ends owned by the caller, or a negative errno.
int pipe_create_pair(struct fd **read_end, struct fd **write_end,
        qword_t shared_inode) {
    int p[2];
    if (pipe(p) < 0)
        return errno_map();
    *read_end = pipe_fd_new(p[0], shared_inode, false);
    *write_end = pipe_fd_new(p[1], shared_inode, true);
    if (*read_end == NULL || *write_end == NULL) {
        if (*read_end != NULL)
            fd_close(*read_end);
        else
            close(p[0]);
        if (*write_end != NULL)
            fd_close(*write_end);
        else
            close(p[1]);
        return _ENOMEM;
    }
    return 0;
}

int_t sys_pipe2(guest_addr_t pipe_addr, int_t flags) {
    STRACE("pipe2(%#llx, %#x)", (unsigned long long) pipe_addr, flags);
    if (flags & ~(O_CLOEXEC_|O_NONBLOCK_)) {
        FIXME("unsupported pipe2 flags %#x from %s[%d]", flags & ~(O_CLOEXEC_|O_NONBLOCK_), current->comm, current->pid);
        return _EINVAL;
    }

    int p[2];
    int err = pipe(p);
    if (err < 0)
        return err;

    int fp[2];
    qword_t shared_inode = adhoc_next_inode();
    err = fp[0] = pipe_f_create(p[0], flags, shared_inode, false);
    if (fp[0] < 0)
        goto close_pipe;
    err = fp[1] = pipe_f_create(p[1], flags, shared_inode, true);
    if (fp[1] < 0)
        goto close_fake_0;

    err = _EFAULT;
    if (user_put(pipe_addr, fp))
        goto close_fake_1;
    STRACE(" [%d %d]", fp[0], fp[1]);
    return 0;

close_fake_1:
    f_close(fp[1]);
close_fake_0:
    f_close(fp[0]);
close_pipe:
    close(p[0]);
    close(p[1]);
    return err;
}

int_t sys_pipe(guest_addr_t pipe_addr) {
    return sys_pipe2(pipe_addr, 0);
}
