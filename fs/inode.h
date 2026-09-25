#ifndef FS_INODE_H
#define FS_INODE_H
#include <sys/types.h>
#include "misc.h"
#include "util/list.h"
#include "util/sync.h"

struct mount;
struct fd;

struct inode_data {
    unsigned refcount;
    ino_t number;
    struct mount *mount;
    struct list chain;

    struct list posix_locks;
    cond_t posix_unlock;
    struct list flock_locks;
    cond_t flock_unlock;

    uint32_t socket_id;
    // The guest type (SOCK_STREAM_, ...) of the socket bound here, while one
    // is: connect() refuses a different type with EPROTOTYPE, as Linux's
    // unix_find_other does. 0 when nothing is bound.
    int socket_type;

    lock_t lock;
};

struct inode_data *inode_get(struct mount *mount, ino_t inode);
void inode_retain(struct inode_data *inode);
void inode_release(struct inode_data *inode);

// generic_open must lock out anything trying to destroy an inode between
// opening the file and acquiring a reference to its inode. For this purpose
// only, the inodes_lock and inode_get_unlocked are made available. Think
// carefully before using them for anything else.
// mount->lock nests inside this.
// To quote @dril: i despise this lock. id love nothing more than to kick it
// through the wall and shatter it into 100 deadlocks. But i need it
extern lock_t inodes_lock;
struct inode_data *inode_get_unlocked(struct mount *mount, ino_t inode);

// calls mount->fs->inode_orphaned if this inode is orphaned, while holding indoes_lock
void inode_check_orphaned(struct mount *mount, ino_t ino);
// same, but for a caller that already holds inodes_lock (e.g. fakefs_unlink/
// fakefs_rmdir, called from generic_unlinkat/generic_rmdirat while they hold
// it): inodes_lock is a plain non-recursive mutex, so calling the locking
// version here would self-deadlock.
void inode_check_orphaned_unlocked(struct mount *mount, ino_t ino);

// file locking stuff (maybe should go in kernel/calls.h?)

#define F_RDLCK_ 0
#define F_WRLCK_ 1
#define F_UNLCK_ 2

struct file_lock {
    off_t_ start;
    off_t_ end;
    int type;
    pid_t_ pid;
    char comm[16];
    void *owner;
    struct list locks;
};

struct flock_ {
    word_t type;
    word_t whence;
    off_t_ start;
    off_t_ len;
    pid_t_ pid;
    char comm[16];
} __attribute__((packed));
struct flock32_ {
    word_t type;
    word_t whence;
    dword_t start;
    dword_t len;
    pid_t_ pid;
    char comm[16];
} __attribute__((packed));
// The amd64 (x86_64) ABI struct flock: 64-bit l_start/l_len, with l_start
// 8-byte aligned -- i.e. 4 bytes of padding after the two leading shorts.
// This is NOT the same layout as struct flock_ above, which is packed to
// match the i386 struct flock64 (l_start at offset 4). The amd64 fcntl path
// must therefore translate field-by-field rather than reading the guest
// struct straight into a struct flock_.
struct flock_amd64 {
    word_t type;
    word_t whence;
    dword_t pad_;
    sqword_t start;
    sqword_t len;
    pid_t_ pid;
} __attribute__((packed));

// ofd selects an open-file-description lock (F_OFD_*) over a process POSIX lock
int fcntl_getlk(struct fd *fd, struct flock_ *flock, bool ofd);
// cmd should be either F_SETLK or F_SETLKW
int fcntl_setlk(struct fd *fd, struct flock_ *flock, bool block, bool ofd);
int flock_lock(struct fd *fd, int operation);

// locks the inode internally
void file_lock_remove_owned_by(struct fd *fd, void *owner);
void flock_remove_owned_by(struct fd *fd);

#endif
