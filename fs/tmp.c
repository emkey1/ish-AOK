#include <sys/stat.h>
#include <string.h>
#include "kernel/task.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/path.h"
#include "util/refcount.h"
#include "util/timer.h"
#include "debug.h"

// ========================
// ======== INODES ========
// ========================

struct tmp_inode {
    struct refcount refcount;
    lock_t lock;

    struct statbuf stat;
    union {
        void *file_data;
        //char *symlink_data;
    };
};

static bool tmpfs_is_cgroup2_mount(struct mount *mount) {
    return strcmp(mount->fs->name, "cgroup2") == 0;
}

static struct tmp_inode *tmp_inode_new(mode_t_ mode) {
    struct tmp_inode *node = malloc(sizeof(struct tmp_inode));
    if (node == NULL)
        return NULL;
    refcount_init(node);
    lock_init(&node->lock, "tmp_inode_new\0");

    node->stat = (struct statbuf) {};
    static _Atomic ino_t next_inode = 1;
    node->stat.inode = next_inode++;

    node->stat.mode = mode;
    node->stat.uid = current->euid;
    node->stat.gid = current->egid;
    if (S_ISREG(mode)) {
        node->file_data = malloc(0);
        if (node->file_data == NULL) {
            free(node);
            return NULL;
        }
    }
    return node;
}

DEFINE_REFCOUNT_STATIC(tmp_inode)

static void tmp_inode_cleanup(struct tmp_inode *inode) {
    if (S_ISREG(inode->stat.mode)) {
        free(inode->file_data);
    }
    free(inode);
}

static void tmpfs_update_ctime(struct tmp_inode *inode) {
    struct timespec now = timespec_now(CLOCK_REALTIME);
    inode->stat.ctime = now.tv_sec;
    inode->stat.ctime_nsec = now.tv_nsec;
}

static void tmpfs_update_mtime_and_ctime(struct tmp_inode *inode) {
    struct timespec now = timespec_now(CLOCK_REALTIME);
    inode->stat.mtime = now.tv_sec;
    inode->stat.mtime_nsec = now.tv_nsec;
    inode->stat.ctime = now.tv_sec;
    inode->stat.ctime_nsec = now.tv_nsec;
}

// ===================================
// ======== DIRECTORY ENTRIES ========
// ===================================

struct tmp_dirent {
    char name[MAX_NAME + 1];
    struct tmp_inode *inode;
    unsigned long index;

    struct tmp_dirent *parent;
    struct list children;
    unsigned long next_index;

    struct refcount refcount;
    lock_t lock;
    struct list dir;
};

DEFINE_REFCOUNT_STATIC(tmp_dirent)

static void tmp_dirent_cleanup(struct tmp_dirent *dirent) {
    if (dirent->parent != NULL)
        tmp_dirent_release(dirent->parent);
    tmp_inode_release(dirent->inode);
    free(dirent);
}

static void tmp_dirent_init(struct tmp_dirent *dirent) {
    refcount_init(dirent);
    list_init(&dirent->children);
    dirent->next_index = 0;
    lock_init(&dirent->lock, "tmp_dirent_init\0");
}

// Frees the child inode on failure, so you don't need to! But be careful you don't free it yourself.
// In other words: Takes ownership of `child`
static int tmpfs_dir_link(struct tmp_dirent *dir, const char *name, struct tmp_inode *child, struct tmp_dirent **dirent_out) {
    if (!S_ISDIR(dir->inode->stat.mode)) {
        tmp_inode_release(child);
        return _ENOTDIR;
    }
    struct tmp_dirent *new_dirent = malloc(sizeof(struct tmp_dirent));
    if (new_dirent == NULL) {
        tmp_inode_release(child);
        return _ENOMEM;
    }

    tmp_dirent_init(new_dirent);
    strncpy(new_dirent->name, name, sizeof(new_dirent->name) - 1);
    new_dirent->name[sizeof(new_dirent->name) - 1] = '\0';
    new_dirent->inode = tmp_inode_retain(child);
    new_dirent->index = dir->next_index++;
    new_dirent->parent = tmp_dirent_retain(dir);
    list_add_tail(&dir->children, &new_dirent->dir);

    if (dirent_out)
        *dirent_out = tmp_dirent_retain(new_dirent);
    return 0;
}

static void tmpfs_fd_seekdir(struct fd *fd, struct tmp_dirent *dirent) {
    if (dirent != NULL)
        tmp_dirent_retain(dirent);
    if (fd->tmpfs.dir_pos != NULL)
        tmp_dirent_release(fd->tmpfs.dir_pos);
    fd->tmpfs.dir_pos = dirent;
}

static struct tmp_dirent *tmpfs_dir_lookup(struct tmp_dirent *dir, const char *name) {
    if (!S_ISDIR(dir->inode->stat.mode))
        return ERR_PTR(_ENOTDIR);
    struct tmp_dirent *dirent = NULL;
    struct tmp_dirent *d;
    list_for_each_entry(&dir->children, d, dir) {
        if (d->inode == NULL)
            continue;
        if (strcmp(d->name, name) == 0) {
            dirent = d;
            break;
        }
    }
    if (dirent == NULL)
        return ERR_PTR(_ENOENT);
    return tmp_dirent_retain(dirent);
}

// TODO: should this function even exist? can't tmpfs_dir_link check for existence?
static int tmpfs_dir_lookup_existence(struct tmp_dirent *dir, const char *name) {
    struct tmp_dirent *dirent = tmpfs_dir_lookup(dir, name);
    if (dirent == ERR_PTR(_ENOENT))
        return 0;
    if (IS_ERR(dirent))
        return PTR_ERR(dirent);
    tmp_dirent_release(dirent);
    return _EEXIST;
}

static int tmpfs_init_regular_file(struct tmp_inode *inode, const char *contents) {
    assert(S_ISREG(inode->stat.mode));
    if (contents == NULL || contents[0] == '\0')
        return 0;

    size_t len = strlen(contents);
    void *new_data = realloc(inode->file_data, len);
    if (new_data == NULL)
        return _ENOMEM;
    inode->file_data = new_data;
    memcpy(inode->file_data, contents, len);
    inode->stat.size = len;
    tmpfs_update_mtime_and_ctime(inode);
    return 0;
}

static int tmpfs_add_file(struct tmp_dirent *dir, const char *name, mode_t_ mode, const char *contents) {
    int err = tmpfs_dir_lookup_existence(dir, name);
    if (err == _EEXIST)
        return 0;
    if (err < 0)
        return err;

    struct tmp_inode *inode = tmp_inode_new(S_IFREG | mode);
    if (inode == NULL)
        return _ENOMEM;

    err = tmpfs_init_regular_file(inode, contents);
    if (err == 0)
        err = tmpfs_dir_link(dir, name, inode, NULL);
    tmp_inode_release(inode);
    return err;
}

static int tmpfs_populate_cgroup2_dir(struct tmp_dirent *dir) {
    int err = tmpfs_add_file(dir, "cgroup.procs", 0644, "");
    if (err < 0)
        return err;
    err = tmpfs_add_file(dir, "cgroup.threads", 0644, "");
    if (err < 0)
        return err;
    err = tmpfs_add_file(dir, "cgroup.controllers", 0444, "cpu io memory pids\n");
    if (err < 0)
        return err;
    err = tmpfs_add_file(dir, "cgroup.subtree_control", 0644, "");
    if (err < 0)
        return err;
    err = tmpfs_add_file(dir, "cgroup.events", 0444, "populated 1\nfrozen 0\n");
    if (err < 0)
        return err;
    err = tmpfs_add_file(dir, "cgroup.stat", 0444, "nr_descendants 0\nnr_dying_descendants 0\n");
    if (err < 0)
        return err;
    err = tmpfs_add_file(dir, "cgroup.type", 0444, "domain\n");
    if (err < 0)
        return err;
    return 0;
}

static struct tmp_dirent *__tmpfs_lookup(struct mount *mount, const char *path, bool parent, const char **filename_out) {
    struct tmp_dirent *root = mount->data;
    struct tmp_dirent *dirent = tmp_dirent_retain(root); // strong reference

    char component[MAX_NAME + 1] = {};
    int err = 0;
    while (path_next_component(&path, component, &err)) {
        if (parent && *path == '\0')
            break;

        lock(&dirent->lock, 0);
        struct tmp_dirent *child = tmpfs_dir_lookup(dirent, component);
        unlock(&dirent->lock);

        tmp_dirent_release(dirent);
        if (IS_ERR(child))
            return child;
        dirent = child;
    }

    if (parent && filename_out)
        *filename_out = path - strlen(component);

    if (err < 0)
        return ERR_PTR(err);
    return dirent;
}
static struct tmp_dirent *tmpfs_lookup(struct mount *mount, const char *path) {
    return __tmpfs_lookup(mount, path, false, NULL);
}
static struct tmp_dirent *tmpfs_lookup_parent(struct mount *mount, const char *path, const char **filename_out) {
    if (strcmp(path, "/") == 0)
        return NULL;
    return __tmpfs_lookup(mount, path, true, filename_out);
}

static int tmpfs_file_resize(struct tmp_inode *file, size_t size) {
    assert(S_ISREG(file->stat.mode));
    size_t old_size = file->stat.size;
    void *new_data = realloc(file->file_data, size);
    if (new_data == NULL)
        return _ENOMEM;
    file->file_data = new_data;
    file->stat.size = size;
    memset((char *) file->file_data + old_size, 0, file->stat.size - old_size);
    tmpfs_update_mtime_and_ctime(file);
    return 0;
}

static int tmpfs_dir_unlink(struct tmp_dirent *parent, const char *name, bool remove_dir) {
    struct tmp_dirent *dirent = tmpfs_dir_lookup(parent, name);
    if (IS_ERR(dirent))
        return PTR_ERR(dirent);

    int err = 0;
    lock(&dirent->lock, 0);
    if (S_ISDIR(dirent->inode->stat.mode)) {
        if (!remove_dir)
            err = _EISDIR;
        else if (!list_empty(&dirent->children))
            err = _ENOTEMPTY;
    } else if (remove_dir) {
        err = _ENOTDIR;
    }
    unlock(&dirent->lock);
    if (err < 0) {
        tmp_dirent_release(dirent);
        return err;
    }

    list_remove(&dirent->dir);
    tmpfs_update_mtime_and_ctime(parent->inode);
    tmp_dirent_release(dirent); // drop tree reference
    tmp_dirent_release(dirent); // drop lookup reference
    return 0;
}

// ========================
// ======== FS OPS ========
// ========================

extern const struct fd_ops tmpfs_fdops;

static int tmpfs_mount(struct mount *mount) {
    struct tmp_inode *root_inode = tmp_inode_new(S_IFDIR | 0777);
    if (root_inode == NULL)
        return _ENOMEM;

    struct tmp_dirent *root = malloc(sizeof(struct tmp_dirent));
    if (root == NULL) {
        free(root_inode);
        return _ENOMEM;
    }

    tmp_dirent_init(root);
    memcpy(root->name, "", 1);
    root->inode = root_inode;
    root->parent = NULL;

    mount->data = root;
    if (tmpfs_is_cgroup2_mount(mount)) {
        lock(&root->lock, 0);
        int err = tmpfs_populate_cgroup2_dir(root);
        unlock(&root->lock);
        if (err < 0)
            return err;
    }
    return 0;
}


static int tmpfs_umount(struct mount *UNUSED(mount)) {
    // big fat fuckin TODO
    // struct tmp_inode *root = mount->data;
    // tmpfs_unmount_tree(root);
    TODO("tmpfs umount");
    return 0;
}

static struct fd *tmpfs_open(struct mount *mount, const char *path, int flags, int mode) {
    struct tmp_dirent *dirent;
    if (flags & O_CREAT_) {
        // FIXME: will create a file when given a path that ends with a slash
        const char *filename;
        struct tmp_dirent *parent = tmpfs_lookup_parent(mount, path, &filename);
        if (IS_ERR(parent))
            return ERR_PTR(PTR_ERR(parent));
        lock(&parent->lock, 0);
        int err = 0;

        dirent = tmpfs_dir_lookup(parent, filename);
        if (flags & O_EXCL_ && !IS_ERR(dirent)) {
            err = _EEXIST;
            goto out_creat;
        }

        if (dirent == ERR_PTR(_ENOENT)) {
            struct tmp_inode *inode = tmp_inode_new(S_IFREG | mode);
            if (inode == NULL) {
                err = _ENOMEM;
                goto out_creat;
            }
            err = tmpfs_dir_link(parent, filename, inode, &dirent);
            tmp_inode_release(inode);
            if (err < 0) {
                goto out_creat;
            }
        }

out_creat:
        if (err < 0) {
            tmp_dirent_release(dirent);
            dirent = ERR_PTR(err);
        }
        unlock(&parent->lock);
        tmp_dirent_release(parent);
    } else {
        dirent = tmpfs_lookup(mount, path);
    }
    if (IS_ERR(dirent))
        return ERR_PTR(PTR_ERR(dirent));

    struct fd *fd = fd_create(&tmpfs_fdops);
    if (fd == NULL) {
        tmp_dirent_release(dirent);
        return ERR_PTR(_ENOMEM);
    }
    fd->tmpfs.dirent = dirent;

    fd->tmpfs.dir_pos = NULL;
    lock(&dirent->lock, 0);
    if (!list_empty(&dirent->children)) {
        tmpfs_fd_seekdir(fd, list_first_entry(&dirent->children, struct tmp_dirent, dir));
    }
    unlock(&dirent->lock);
    return fd;
}

static int tmpfs_stat(struct mount *mount, const char *path, struct statbuf *stat) {
    struct tmp_dirent *dirent = tmpfs_lookup(mount, path);
    if (IS_ERR(dirent))
        return PTR_ERR(dirent);
    struct tmp_inode *inode = dirent->inode;
    lock(&inode->lock, 0);
    *stat = dirent->inode->stat;
    unlock(&inode->lock);
    tmp_dirent_release(dirent);
    return 0;
}

static int tmpfs_unlink(struct mount *mount, const char *path) {
    const char *filename;
    struct tmp_dirent *parent = tmpfs_lookup_parent(mount, path, &filename);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    if (parent == NULL)
        return _EPERM;
    lock(&parent->lock, 0);
    int err = tmpfs_dir_unlink(parent, filename, false);
    unlock(&parent->lock);
    tmp_dirent_release(parent);
    return err;
}

static int tmpfs_rmdir(struct mount *mount, const char *path) {
    const char *filename;
    struct tmp_dirent *parent = tmpfs_lookup_parent(mount, path, &filename);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    if (parent == NULL)
        return _EBUSY;
    lock(&parent->lock, 0);
    int err = tmpfs_dir_unlink(parent, filename, true);
    unlock(&parent->lock);
    tmp_dirent_release(parent);
    return err;
}

static int tmpfs_setattr(struct mount *mount, const char *path, struct attr attr) {
    struct tmp_dirent *dirent = tmpfs_lookup(mount, path);
    if (IS_ERR(dirent))
        return PTR_ERR(dirent);
    struct tmp_inode *inode = dirent->inode;
    int err = 0;
    lock(&inode->lock, 0);
    switch (attr.type) {
        case attr_uid:
            inode->stat.uid = attr.uid;
            tmpfs_update_ctime(inode);
            break;
        case attr_gid:
            inode->stat.gid = attr.gid;
            tmpfs_update_ctime(inode);
            break;
        case attr_mode:
            inode->stat.mode = (inode->stat.mode & S_IFMT) | (attr.mode & ~S_IFMT);
            tmpfs_update_ctime(inode);
            break;
        case attr_size:
            if (S_ISDIR(inode->stat.mode))
                err = _EISDIR;
            else
                err = tmpfs_file_resize(inode, attr.size);
            break;
        default:
            err = _EPERM;
    }
    unlock(&inode->lock);
    tmp_dirent_release(dirent);
    return err;
}

static int tmpfs_utime(struct mount *mount, const char *path, struct timespec atime, struct timespec mtime, bool follow_links) {
    struct tmp_dirent *dirent = tmpfs_lookup(mount, path);
    if (IS_ERR(dirent))
        return PTR_ERR(dirent);
    struct tmp_inode *inode = dirent->inode;
    (void) follow_links;
    lock(&inode->lock, 0);
    inode->stat.atime = atime.tv_sec;
    inode->stat.atime_nsec = atime.tv_nsec;
    inode->stat.mtime = mtime.tv_sec;
    inode->stat.mtime_nsec = mtime.tv_nsec;
    tmpfs_update_ctime(inode);
    unlock(&inode->lock);
    tmp_dirent_release(dirent);
    return 0;
}

static int tmpfs_close(struct fd *fd) {
    // shouldn't need locking as this is the last reference to the fd
    tmp_dirent_release(fd->tmpfs.dirent);
    fd->tmpfs.dirent = NULL;
    return 0;
}

static int tmpfs_mkdir(struct mount *mount, const char *path, mode_t_ mode) {
    const char *filename;
    struct tmp_dirent *parent = tmpfs_lookup_parent(mount, path, &filename);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    lock(&parent->lock, 0);

    int err = tmpfs_dir_lookup_existence(parent, filename);
    if (err < 0)
        goto out;

    struct tmp_inode *inode = tmp_inode_new(S_IFDIR | mode);
    err = _ENOMEM;
    if (inode == NULL)
        goto out;

    struct tmp_dirent *new_dirent = NULL;
    err = tmpfs_dir_link(parent, filename, inode, &new_dirent);
    if (err == 0) {
        if (tmpfs_is_cgroup2_mount(mount)) {
            lock(&new_dirent->lock, 0);
            err = tmpfs_populate_cgroup2_dir(new_dirent);
            unlock(&new_dirent->lock);
        }
        tmpfs_update_mtime_and_ctime(parent->inode);
    }
    if (new_dirent != NULL)
        tmp_dirent_release(new_dirent);
out:
    unlock(&parent->lock);
    tmp_dirent_release(parent);
    return err;
}

static int tmpfs_mknod(struct mount *mount, const char *path, mode_t_ mode, dev_t_ dev) {
    const char *filename;
    struct tmp_dirent *parent = tmpfs_lookup_parent(mount, path, &filename);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    if (parent == NULL)
        return _EPERM;
    lock(&parent->lock, 0);

    int err = tmpfs_dir_lookup_existence(parent, filename);
    if (err < 0)
        goto out;

    struct tmp_inode *inode = tmp_inode_new(mode);
    err = _ENOMEM;
    if (inode == NULL)
        goto out;
    inode->stat.rdev = dev;

    err = tmpfs_dir_link(parent, filename, inode, NULL);
    tmp_inode_release(inode);
    if (err == 0)
        tmpfs_update_mtime_and_ctime(parent->inode);
out:
    unlock(&parent->lock);
    tmp_dirent_release(parent);
    return err;
}

// ========================
// ======== FD OPS ========
// ========================

static struct tmp_inode *tmpfs_fd_inode(struct fd *fd) {
    return fd->tmpfs.dirent->inode;
}

static int tmpfs_getpath(struct fd *fd, char *buf) {
    struct tmp_dirent *dirent = fd->tmpfs.dirent;
    struct tmp_dirent *root_dirent = fd->mount->data;
    char *p = buf + MAX_PATH - 1;
    *p = '\0';
    while (dirent != root_dirent) {
        size_t name_len = strlen(dirent->name);
        p -= name_len + 1;
        if (p < buf)
            return _ENAMETOOLONG;
        p[0] = '/';
        memcpy(&p[1], dirent->name, name_len);
        dirent = dirent->parent;
    }
    // Bolt: eliminate redundant O(N) string traversal by calculating length via pointer arithmetic
    memmove(buf, p, (buf + MAX_PATH) - p);
    return 0;
}

static int tmpfs_fstat(struct fd *fd, struct statbuf *stat) {
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    lock(&inode->lock, 0);
    *stat = inode->stat;
    unlock(&inode->lock);
    return 0;
}

static ssize_t tmpfs_read(struct fd *fd, void *buf, size_t bufsize) {
    ssize_t res;
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    lock(&inode->lock, 0);
    res = _EISDIR;
    if (S_ISDIR(inode->stat.mode))
        goto out;
    assert(S_ISREG(inode->stat.mode));

    if (bufsize > inode->stat.size - fd->offset) {
        bufsize = inode->stat.size - fd->offset;
        if (fd->offset >= inode->stat.size)
            bufsize = 0;
    }
    memcpy(buf, inode->file_data + fd->offset, bufsize);
    fd->offset += bufsize;
    res = bufsize;

out:
    unlock(&inode->lock);
    return res;
}

static ssize_t tmpfs_write(struct fd *fd, const void *buf, size_t bufsize) {
    ssize_t res;
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    lock(&inode->lock, 0);
    res = _EISDIR;
    if (S_ISDIR(inode->stat.mode))
        goto out;
    assert(S_ISREG(inode->stat.mode));

    if (inode->stat.size < fd->offset + bufsize) {
        res = tmpfs_file_resize(inode, fd->offset + bufsize);
        if (res < 0)
            goto out;
    }
    memcpy(inode->file_data + fd->offset, buf, bufsize);
    fd->offset += bufsize;
    res = bufsize;

out:
    unlock(&inode->lock);
    return res;
}

static off_t_ tmpfs_lseek(struct fd *fd, off_t_ off, int whence) {
    qword_t size = 0;
    if (whence == LSEEK_END) {
        struct tmp_inode *inode = tmpfs_fd_inode(fd);
        lock(&inode->lock, 0);
        size = inode->stat.size;
        unlock(&inode->lock);
    }

    int err = generic_seek(fd, off, whence, size);
    if (err < 0)
        return err;

    return fd->offset;
}

static int tmpfs_readdir(struct fd *fd, struct dir_entry *entry) {
    struct tmp_dirent *parent = fd->tmpfs.dirent;
    int res = _ENOTDIR;
    if (!S_ISDIR(parent->inode->stat.mode))
        goto out;

    lock(&fd->lock, 0);
    lock(&parent->lock, 0);
    struct tmp_dirent *dirent = fd->tmpfs.dir_pos;
    if (dirent == NULL) {
        res = 0;
        goto out;
    }
    struct tmp_dirent *next_dirent = list_next_entry(dirent, dir);
    if (&next_dirent->dir == &parent->children) // end of list
        next_dirent = NULL;
    tmpfs_fd_seekdir(fd, next_dirent);

    entry->inode = dirent->inode->stat.inode;
    entry->type = dir_entry_type_for_mode(dirent->inode->stat.mode);
    strncpy(entry->name, dirent->name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    res = 1;

out:
    unlock(&parent->lock);
    unlock(&fd->lock);
    return res;
}

static unsigned long tmpfs_telldir(struct fd *fd) {
    if (fd->tmpfs.dir_pos == NULL)
        return (unsigned long) -1;
    return fd->tmpfs.dir_pos->index;
}

static void tmpfs_seekdir(struct fd *fd, unsigned long ptr) {
    struct tmp_dirent *dir = fd->tmpfs.dirent;
    lock(&dir->lock, 0);
    assert(S_ISDIR(dir->inode->stat.mode));
    struct tmp_dirent *child;
    list_for_each_entry(&dir->children, child, dir) {
        if (child->index >= ptr)
            break;
    }
    if (&child->dir == &dir->children)
        child = NULL;
    tmpfs_fd_seekdir(fd, child);
    unlock(&dir->lock);
}

const struct fs_ops tmpfs = {
    .name = "tmpfs", .magic = 0x01021994,
    .mount = tmpfs_mount,
    .umount = tmpfs_umount,
    .open = tmpfs_open,
    .close = tmpfs_close,
    .stat = tmpfs_stat,
    .unlink = tmpfs_unlink,
    .rmdir = tmpfs_rmdir,
    .fstat = tmpfs_fstat,
    .setattr = tmpfs_setattr,
    .utime = tmpfs_utime,
    .getpath = tmpfs_getpath,
    .mkdir = tmpfs_mkdir,
    .mknod = tmpfs_mknod,
};

const struct fs_ops cgroupfs = {
    .name = "cgroup", .magic = 0x27e0eb,
    .mount = tmpfs_mount,
    .umount = tmpfs_umount,
    .open = tmpfs_open,
    .close = tmpfs_close,
    .stat = tmpfs_stat,
    .unlink = tmpfs_unlink,
    .rmdir = tmpfs_rmdir,
    .fstat = tmpfs_fstat,
    .setattr = tmpfs_setattr,
    .utime = tmpfs_utime,
    .getpath = tmpfs_getpath,
    .mkdir = tmpfs_mkdir,
    .mknod = tmpfs_mknod,
};

const struct fs_ops cgroup2fs = {
    .name = "cgroup2", .magic = 0x63677270,
    .mount = tmpfs_mount,
    .umount = tmpfs_umount,
    .open = tmpfs_open,
    .close = tmpfs_close,
    .stat = tmpfs_stat,
    .unlink = tmpfs_unlink,
    .rmdir = tmpfs_rmdir,
    .fstat = tmpfs_fstat,
    .setattr = tmpfs_setattr,
    .utime = tmpfs_utime,
    .getpath = tmpfs_getpath,
    .mkdir = tmpfs_mkdir,
    .mknod = tmpfs_mknod,
};

const struct fd_ops tmpfs_fdops = {
    .read = tmpfs_read,
    .write = tmpfs_write,
    .lseek = tmpfs_lseek,
    .readdir = tmpfs_readdir,
    .telldir = tmpfs_telldir,
    .seekdir = tmpfs_seekdir,
};
