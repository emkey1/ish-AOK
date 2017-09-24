#ifndef FS_H
#define FS_H

#include "misc.h"
#include "util/list.h"
#include "fs/stat.h"
#include <dirent.h>

#define MAX_PATH 4096
#define MAX_NAME 256

struct poll;

struct fd {
    unsigned refcnt;
    unsigned flags;
    const struct fd_ops *ops;

    struct list poll_fds;
    struct pollable *pollable;
    struct list pollable_other_fds;

    union {
        struct {
            DIR *dir;
        };
        struct tty *tty;
    };

    // "inode"
    struct mount *mount;
    union {
        int real_fd;
        struct statbuf *stat;
    };

    pthread_mutex_t lock;
};
typedef sdword_t fd_t;
struct fd *fd_create();
fd_t fd_next();
#define MAX_FD 1024 // dynamically expanding fd table coming soon:tm:
#define AT_FDCWD_ -100
#define FD_CLOEXEC_ 1

struct fd *generic_open(const char *path, int flags, int mode);
struct fd *generic_openat(struct fd *at, const char *path, int flags, int mode);
struct fd *generic_dup(struct fd *fd);
int generic_close(struct fd *fd);
int generic_unlinkat(struct fd *at, const char *path);
#define AC_R 4
#define AC_W 2
#define AC_X 1
#define AC_F 0
int generic_access(const char *path, int mode);
int generic_statat(struct fd *at, const char *path, struct statbuf *stat, bool follow_links);
int generic_fstat(struct fd *fd, struct statbuf *stat);
ssize_t generic_readlink(const char *path, char *buf, size_t bufsize);

// Converts an at argument to a system call to a struct fd *, returns NULL if you pass a bad fd
struct fd *at_fd(fd_t fd);

struct mount {
    const char *point;
    const char *source;
    const struct fs_ops *fs;
    struct mount *next;
};
struct mount *mounts;

// open flags
#define O_RDONLY_ 0
#define O_WRONLY_ (1 << 0)
#define O_RDWR_ (1 << 1)
#define O_CREAT_ (1 << 6)

struct fs_ops {
    int (*statfs)(struct mount *mount, struct statfsbuf *stat);
    // the path parameter points to MAX_PATH bytes of allocated memory, which
    // you can do whatever you want with (but make sure to return _ENAMETOOLONG
    // instead of overflowing the buffer)
    struct fd *(*open)(struct mount *mount, char *path, int flags, int mode);
    int (*stat)(struct mount *mount, char *path, struct statbuf *stat, bool follow_links);
    int (*unlink)(struct mount *mount, char *path);
    int (*access)(struct mount *mount, char *path, int mode);
    ssize_t (*readlink)(struct mount *mount, char *path, char *buf, size_t bufsize);
    // i'm considering removing stat, and just having fstat, which would then be called stat
    // but that wouldn't work because links
    int (*fstat)(struct fd *fd, struct statbuf *stat);
    int (*flock)(struct fd *fd, int operation);
    int (*utime)(struct mount *mount, char *path, struct timespec atime, struct timespec mtime);
};

#define NAME_MAX 255
struct dir_entry {
    qword_t inode;
    qword_t offset;
    char name[NAME_MAX + 1];
};

struct fd_ops {
    ssize_t (*read)(struct fd *fd, void *buf, size_t bufsize);
    ssize_t (*write)(struct fd *fd, const void *buf, size_t bufsize);
    off_t (*lseek)(struct fd *fd, off_t off, int whence);

    // Reads a directory entry from the stream
    int (*readdir)(struct fd *fd, struct dir_entry *entry);

    // memory returned must be allocated with mmap, as it is freed with munmap
    int (*mmap)(struct fd *fd, off_t offset, size_t len, int prot, int flags, void **mem_out);

    // returns a bitmask of operations that won't block
    int (*poll)(struct fd *fd);

    // returns the size needed for the output of ioctl, 0 if the arg is not a
    // pointer, -1 for invalid command
    ssize_t (*ioctl_size)(struct fd *fd, int cmd);
    // if ioctl_size returns non-zero, arg must point to ioctl_size valid bytes
    int (*ioctl)(struct fd *fd, int cmd, void *arg);

    // Returns the path of the file descriptor, buf must be at least MAX_PATH
    int (*getpath)(struct fd *fd, char *buf);

    int (*close)(struct fd *fd);
};

struct mount *find_mount(char *path);
struct mount *find_mount_and_trim_path(char *path);

struct pollable {
    struct list fds;
    pthread_mutex_t lock;
};

struct poll {
    struct list poll_fds;
    struct list real_poll_fds;
    int notify_pipe[2];
    pthread_mutex_t lock;
};

struct poll_fd {
    // locked by containing struct poll
    struct fd *fd;
    struct list fds;
    int types;

    // locked by containing struct fd
    struct poll *poll;
    struct list polls;
};

#define POLL_READ 1
#define POLL_WRITE 4
struct poll_event {
    struct fd *fd;
    int types;
};
struct poll *poll_create();
int poll_add_fd(struct poll *poll, struct fd *fd, int types);
int poll_del_fd(struct poll *poll, struct fd *fd);
void poll_wake_pollable(struct pollable *pollable);
int poll_wait(struct poll *poll, struct poll_event *event, int timeout);
void poll_destroy(struct poll *poll);

// Normalizes the path specified and writes the result into the out buffer.
//
// Normalization means:
//  - prepending the current or root directory
//  - converting multiple slashes into one
//  - resolving . and ..
//  - resolving symlinks, skipping the last path component if the follow_links
//    argument is true
// The result will always begin with a slash.
//
// If the normalized path plus the null terminator would be longer than
// MAX_PATH, _ENAMETOOLONG is returned. The out buffer is expected to be at
// least MAX_PATH in size.
int path_normalize(struct fd *at, const char *path, char *out, bool follow_links);

// real fs
extern const struct fs_ops realfs;
extern const struct fd_ops realfs_fdops;
ssize_t realfs_read(struct fd *fd, void *buf, size_t bufsize);
ssize_t realfs_write(struct fd *fd, const void *buf, size_t bufsize);
int realfs_close(struct fd *fd);

// TODO put this somewhere else
char *strnprepend(char *str, const char *prefix, size_t max);

#endif
