#ifndef KERNEL_NATIVE_IO_H
#define KERNEL_NATIVE_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "kernel/fs.h"
#include "fs/fd.h"

// Guest-filesystem access for natively-implemented programs (kernel/native.h).
//
// This is the seam that makes native programs useful rather than merely fast.
// A native program is compiled into iSH-AOK and linked against the HOST libc,
// so calling open()/read()/opendir() directly would resolve against the iOS
// filesystem -- silently reading the wrong files rather than failing, which is
// the worst possible failure mode. Everything here goes through iSH's own VFS
// instead, resolving relative to the calling task's cwd and root, so a native
// program sees exactly the filesystem the guest sees.
//
// Every call returns 0 or a negative guest errno, except where noted.

// Opens a guest path. flags are guest O_* values. On success *fd_out is a
// reference the caller must release with native_close.
int native_open(const char *path, int flags, struct fd **fd_out);
void native_close(struct fd *fd);

// Bytes read, 0 at EOF, or a negative errno.
ssize_t native_read(struct fd *fd, void *buf, size_t size);

// Reads one directory entry. 1 on success, 0 at end of directory, negative
// errno on failure -- matching the fs_ops readdir convention.
int native_readdir(struct fd *fd, struct dir_entry *entry);

// stat/lstat against a guest path, resolved like native_open.
int native_stat(const char *path, struct statbuf *stat, bool follow_links);

// The calling task's cwd as a guest path, into a buffer of at least MAX_PATH.
int native_getcwd(char *buf);

// Writes a host buffer to one of the calling task's guest fds (1 and 2 being
// the interesting ones). Bytes written, or a negative errno.
ssize_t native_write(fd_t fd_no, const void *buf, size_t size);

// printf onto a guest fd. Output longer than the internal buffer is truncated,
// which is fine for the diagnostics this exists to carry.
void native_printf(fd_t fd_no, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif
