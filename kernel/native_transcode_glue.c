// The host-side half of the transcode native program: see native_transcode.h.
// Compiled WITHOUT the native libc shim, so open/read/fcntl here are the host's.
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kernel/task.h"
#include "kernel/fs.h"
#include "fs/fd.h"
#include "kernel/native_transcode.h"

int (*transcode_hook)(struct transcode_job *job);

int transcode_fd_host_path(int fd, char *buf, size_t size) {
#ifdef F_GETPATH
    struct fd *f = f_get_retain(fd);
    if (f == NULL)
        return -1;
    int res = -1;
    if (f->type == S_IFREG && f->real_fd >= 0 && size >= MAXPATHLEN) {
        // The host file may be the guest's under another name (fakefs keeps
        // its data in a host tree), but its contents are the guest file's,
        // which is all AVFoundation reads.
        struct stat st;
        if (fstat(f->real_fd, &st) == 0 && S_ISREG(st.st_mode) &&
                fcntl(f->real_fd, F_GETPATH, buf) != -1)
            res = 0;
    }
    fd_close(f);
    return res;
#else
    (void) fd; (void) buf; (void) size;
    return -1;
#endif
}

int transcode_host_open(const char *path) {
    return open(path, O_RDONLY | O_CLOEXEC);
}

ssize_t transcode_host_read(int host_fd, void *buf, size_t size) {
    ssize_t n;
    do
        n = read(host_fd, buf, size);
    while (n < 0 && errno == EINTR);
    return n;
}

void transcode_host_close(int host_fd) {
    close(host_fd);
}

void transcode_host_unlink(const char *path) {
    unlink(path);
}
