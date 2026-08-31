#include <sys/stat.h>
#include <unistd.h>
#include "kernel/calls.h"
#include "fs/fd.h"
#include "fs/real.h"
#include "debug.h"

static fd_t pipe_f_create(int pipe_fd, int flags) {
    struct fd *fd = adhoc_fd_create(&realfs_fdops);
    if (fd == NULL)
        return _ENOMEM;
    fd->real_fd = pipe_fd;
    // fd->type as well as stat.mode: it is the field the rest of the kernel
    // asks "what kind of thing is this fd", and it was left 0 here, so a pipe
    // answered "not a fifo, not a directory, not a regular file" to anything
    // that checked -- fallocate on a pipe came back ENODEV instead of ESPIPE.
    fd->type = S_IFIFO;
    fd->stat.mode = S_IFIFO | 0660;
    fd->stat.uid = current->uid;
    fd->stat.gid = current->gid;
    fd->stat.ctime = (dword_t)time(NULL);
    return f_install(fd, flags);
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
    err = fp[0] = pipe_f_create(p[0], flags);
    if (fp[0] < 0)
        goto close_pipe;
    err = fp[1] = pipe_f_create(p[1], flags);
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
