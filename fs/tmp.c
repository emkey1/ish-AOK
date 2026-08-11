#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/path.h"
#include "fs/fifo.h"
#include "fs/poll.h"
#include "fs/real.h"
#include "fs/dev.h"
#include "fs/devices.h"
#include "fs/dyndev.h"
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
        struct fifo_file *fifo; // S_IFIFO: the shared named-pipe buffer
    };
    // S_IFREG only: once the file has been mmapped, its contents live in this
    // unlinked host temp file instead of the malloc'd file_data buffer, so
    // guest mappings can be host mmaps of it (see tmpfs_inode_host_backing).
    // -1 while still malloc-backed.
    int host_fd;
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
    // Linux semantics: a fresh directory has 2 links ("." plus its parent's
    // entry), everything else 1. Leaving this 0 broke systemd-journald:
    // journal_file_fstat() treats st_nlink == 0 as "file already deleted"
    // (-EIDRM, part of its journal-corruption error set), so every journal
    // file created on the /run tmpfs was instantly declared "corrupted or
    // uncleanly shut down" and the Journal Service crash-looped.
    node->stat.nlink = S_ISDIR(mode) ? 2 : 1;
    node->stat.uid = current->euid;
    node->stat.gid = current->egid;
    node->file_data = NULL; // also clears the ->fifo union slot for S_IFIFO
    node->host_fd = -1;
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
    // Regular files keep their contents in file_data; symlinks keep their
    // target string there too (same union slot).
    if (S_ISREG(inode->stat.mode) || S_ISLNK(inode->stat.mode)) {
        free(inode->file_data);
        if (inode->host_fd >= 0)
            close(inode->host_fd);
    } else if (S_ISFIFO(inode->stat.mode)) {
        fifo_file_free(inode->fifo);
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

// ============================
// ======== DEVTMPFS ==========
// ============================
//
// devtmpfs is tmpfs that publishes the emulator's device nodes at mount time,
// the way the real one mirrors the kernel's device model. iSH's device nodes
// are dispatched purely by rdev major/minor (fs/dev.c) regardless of which
// filesystem they live on, so a node created here is fully functional.
//
// This exists because a mount that silently produced an EMPTY /dev was worse
// than useless: the first `echo x > /dev/null` on it creates a REGULAR FILE
// that accumulates every byte ever written to it, forever, on the backing
// store. That is the state any guest reached when it mounted devtmpfs
// somewhere other than an already-populated /dev -- switch_root and chroot
// boots (iSH's own /AOK/roots multi-arch chroots included), and any rootfs
// whose tarball could not carry real device nodes.

static bool tmpfs_is_devtmpfs_mount(struct mount *mount) {
    return strcmp(mount->fs->name, "devtmpfs") == 0;
}

// Device nodes are root-owned on a real devtmpfs no matter who mounted it,
// while tmp_inode_new() inherits the caller's ids -- stamp them explicitly.
static int tmpfs_add_dev_node(struct tmp_dirent *dir, const char *name, mode_t_ mode, dev_t_ dev) {
    int err = tmpfs_dir_lookup_existence(dir, name);
    if (err == _EEXIST)
        return 0;
    if (err < 0)
        return err;

    struct tmp_inode *inode = tmp_inode_new(S_IFCHR | mode);
    if (inode == NULL)
        return _ENOMEM;
    inode->stat.rdev = dev;
    inode->stat.uid = 0;
    inode->stat.gid = 0;

    err = tmpfs_dir_link(dir, name, inode, NULL);
    tmp_inode_release(inode);
    return err;
}

static int tmpfs_add_dev_subdir(struct tmp_dirent *dir, const char *name, mode_t_ mode) {
    int err = tmpfs_dir_lookup_existence(dir, name);
    if (err == _EEXIST)
        return 0;
    if (err < 0)
        return err;

    struct tmp_inode *inode = tmp_inode_new(S_IFDIR | mode);
    if (inode == NULL)
        return _ENOMEM;
    inode->stat.uid = 0;
    inode->stat.gid = 0;

    err = tmpfs_dir_link(dir, name, inode, NULL);
    if (err == 0)
        dir->inode->stat.nlink++; // the new subdir's ".." link
    tmp_inode_release(inode);
    return err;
}

static int tmpfs_add_dev_symlink(struct tmp_dirent *dir, const char *name, const char *target) {
    int err = tmpfs_dir_lookup_existence(dir, name);
    if (err == _EEXIST)
        return 0;
    if (err < 0)
        return err;

    struct tmp_inode *inode = tmp_inode_new(S_IFLNK | 0777);
    if (inode == NULL)
        return _ENOMEM;
    // Same storage a tmpfs symlink uses: raw target bytes in file_data.
    size_t target_len = strlen(target);
    inode->file_data = malloc(target_len + 1);
    if (inode->file_data == NULL) {
        tmp_inode_release(inode);
        return _ENOMEM;
    }
    memcpy(inode->file_data, target, target_len + 1);
    inode->stat.size = target_len;
    inode->stat.uid = 0;
    inode->stat.gid = 0;

    err = tmpfs_dir_link(dir, name, inode, NULL);
    tmp_inode_release(inode);
    return err;
}

static int tmpfs_populate_devtmpfs_root(struct tmp_dirent *dir) {
    for (size_t i = 0; i < dev_standard_nodes_count; i++) {
        int err = tmpfs_add_dev_node(dir, dev_standard_nodes[i].name, dev_standard_nodes[i].mode,
                dev_make(dev_standard_nodes[i].major, dev_standard_nodes[i].minor));
        if (err < 0)
            return err;
    }
    for (size_t i = 0; i < dev_dynamic_nodes_count; i++) {
        if (!dyn_dev_is_registered(dev_dynamic_nodes[i].major, dev_dynamic_nodes[i].minor))
            continue;
        int err = tmpfs_add_dev_node(dir, dev_dynamic_nodes[i].name, dev_dynamic_nodes[i].mode,
                dev_make(dev_dynamic_nodes[i].major, dev_dynamic_nodes[i].minor));
        if (err < 0)
            return err;
    }

    // Mount points Linux ships as directories on /dev: devpts goes on pts,
    // and shm is where POSIX shared memory lands (1777, or every non-root
    // shm_open() fails with EACCES -- see the matching AppDelegate.m fix).
    int err = tmpfs_add_dev_subdir(dir, "pts", 0755);
    if (err < 0)
        return err;
    err = tmpfs_add_dev_subdir(dir, "shm", 01777);
    if (err < 0)
        return err;

    err = tmpfs_add_dev_symlink(dir, "fd", "/proc/self/fd");
    if (err < 0)
        return err;
    err = tmpfs_add_dev_symlink(dir, "stdin", "/proc/self/fd/0");
    if (err < 0)
        return err;
    err = tmpfs_add_dev_symlink(dir, "stdout", "/proc/self/fd/1");
    if (err < 0)
        return err;
    err = tmpfs_add_dev_symlink(dir, "stderr", "/proc/self/fd/2");
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

// Lazily switch a regular file's backing from the malloc'd file_data buffer to
// an unlinked host temp file. The malloc buffer moves on every realloc
// (tmpfs_file_resize / tmpfs_write), which would leave a guest mapping pointing
// at freed memory, so it can never be mmapped directly; a host file makes every
// guest mapping a host mmap of the same file, and the host kernel provides
// MAP_SHARED write-back and mmap<->read/write coherence exactly as it does for
// realfs. Only mmapped files pay the host-fd cost. Call with inode->lock held.
// Returns the host fd, or a negative errno.
static int tmpfs_inode_host_backing(struct tmp_inode *inode) {
    assert(S_ISREG(inode->stat.mode));
    if (inode->host_fd >= 0)
        return inode->host_fd;
    int host_fd = host_unlinked_tmpfd();
    if (host_fd < 0)
        return host_fd;
    size_t size = inode->stat.size;
    size_t done = 0;
    while (done < size) {
        ssize_t n = pwrite(host_fd, (char *) inode->file_data + done, size - done, done);
        if (n < 0) {
            int err = errno_map();
            close(host_fd);
            return err;
        }
        done += n;
    }
    free(inode->file_data);
    inode->file_data = NULL;
    inode->host_fd = host_fd;
    return host_fd;
}

static int tmpfs_file_resize(struct tmp_inode *file, size_t size) {
    assert(S_ISREG(file->stat.mode));
    if (file->host_fd >= 0) {
        // Host-file-backed (has been mmapped): ftruncate keeps live guest
        // mappings coherent, and the host zero-fills growth.
        if (ftruncate(file->host_fd, size) < 0)
            return errno_map();
        file->stat.size = size;
        tmpfs_update_mtime_and_ctime(file);
        return 0;
    }
    size_t old_size = file->stat.size;
    void *new_data = realloc(file->file_data, size);
    // realloc(ptr, 0) may legitimately return NULL (it frees ptr); only treat
    // NULL as an error for a non-zero request.
    if (new_data == NULL && size != 0)
        return _ENOMEM;
    file->file_data = new_data;
    file->stat.size = size;
    // Only zero newly-grown bytes. When shrinking (e.g. ftruncate to a smaller
    // size, or to 0) size < old_size, and size - old_size is an unsigned
    // underflow -> a huge memset that writes far past the buffer -> SIGSEGV.
    if (size > old_size)
        memset((char *) file->file_data + old_size, 0, size - old_size);
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
    // nlink bookkeeping (no hardlinks on tmpfs, so an unlinked file drops
    // straight to 0 -- open fds keep seeing the Linux-accurate "deleted"
    // state). A removed directory also releases its ".." link on the parent.
    if (S_ISDIR(dirent->inode->stat.mode)) {
        dirent->inode->stat.nlink = 0;
        parent->inode->stat.nlink--;
    } else {
        dirent->inode->stat.nlink--;
    }
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
    // Linux tmpfs honors mode=/uid=/gid= mount options on its root inode
    // (mm/shmem.c shmem_parse_one). They matter beyond cosmetics:
    // systemd-user-runtime-dir mounts /run/user/<uid> as tmpfs with
    // "mode=0700,uid=N,gid=N", and pam_systemd then refuses to export
    // XDG_RUNTIME_DIR unless the directory is owned by that uid ("Runtime
    // directory '/run/user/1000' is not owned by UID 1000"). With the
    // options ignored the dir stayed root-owned, so systemd --user exited
    // ("Trying to run as user instance, but $XDG_RUNTIME_DIR is not set")
    // and every user@ start failed. Other options (size=, nr_inodes=,
    // smackfsroot=, ...) remain accepted no-ops as before.
    mode_t_ root_mode = S_IFDIR | 0777;
    uid_t_ root_uid = 0;
    uid_t_ root_gid = 0;
    const char *opt = mount->info;
    while (opt != NULL && *opt != '\0') {
        const char *end = strchr(opt, ',');
        size_t len = end != NULL ? (size_t) (end - opt) : strlen(opt);
        if (len > 5 && strncmp(opt, "mode=", 5) == 0)
            root_mode = S_IFDIR | (mode_t_) (strtoul(opt + 5, NULL, 8) & 07777);
        else if (len > 4 && strncmp(opt, "uid=", 4) == 0)
            root_uid = (uid_t_) strtoul(opt + 4, NULL, 10);
        else if (len > 4 && strncmp(opt, "gid=", 4) == 0)
            root_gid = (uid_t_) strtoul(opt + 4, NULL, 10);
        opt = end != NULL ? end + 1 : "";
    }

    struct tmp_inode *root_inode = tmp_inode_new(root_mode);
    if (root_inode == NULL)
        return _ENOMEM;
    root_inode->stat.uid = root_uid;
    root_inode->stat.gid = root_gid;

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
    if (tmpfs_is_devtmpfs_mount(mount)) {
        lock(&root->lock, 0);
        int err = tmpfs_populate_devtmpfs_root(root);
        unlock(&root->lock);
        if (err < 0)
            return err;
    }
    return 0;
}


// Post-order teardown of the whole directory tree. Each dirent carries one
// "tree reference" (the refcount it was created with in tmpfs_mount /
// tmpfs_dir_link); dropping it runs tmp_dirent_cleanup, which in turn releases
// the dirent's parent and inode references, so the refcounts unwind cleanly up
// to the root. mount_remove() already rejected the umount with EBUSY if any fd
// still referenced this mount, so there are no live fd references to race with.
static void tmpfs_free_tree(struct tmp_dirent *dirent) {
    while (!list_empty(&dirent->children)) {
        struct tmp_dirent *child = list_first_entry(&dirent->children, struct tmp_dirent, dir);
        list_remove(&child->dir);
        tmpfs_free_tree(child);
    }
    tmp_dirent_release(dirent);
}

static int tmpfs_umount(struct mount *mount) {
    struct tmp_dirent *root = mount->data;
    if (root != NULL) {
        tmpfs_free_tree(root);
        mount->data = NULL;
    }
    return 0;
}

static struct fd *tmpfs_open(struct mount *mount, const char *path, int flags, int mode) {
    struct tmp_dirent *dirent;
    if (flags & O_CREAT_) {
        // Stale: `path` never has a trailing slash here. tmpfs_open is only
        // ever reached through generic_openat (the sole caller of
        // mount->fs->open), which already resolves a trailing slash in
        // path_raw against the ENOENT/non-directory cases (EISDIR / ENOTDIR)
        // before calling into this backend. Verified against real Linux:
        // O_CREAT on "nonexistent/" -> EISDIR, on "existingfile/" -> ENOTDIR,
        // neither reaches or corrupts this function.
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

    // O_TRUNC was silently ignored (realfs/fakefs get it from the host
    // open(2); tmpfs never implemented it), so open(path, O_TRUNC) and
    // procfd magic-link reopens with O_TRUNC left the old contents in
    // place. Linux truncates a regular file on O_TRUNC regardless of the
    // access mode.
    if ((flags & O_TRUNC_) && S_ISREG(dirent->inode->stat.mode)) {
        struct tmp_inode *inode = dirent->inode;
        lock(&inode->lock, 0);
        int terr = tmpfs_file_resize(inode, 0);
        unlock(&inode->lock);
        if (terr < 0) {
            fd_close(fd);
            return ERR_PTR(terr);
        }
    }

    fd->tmpfs.dir_pos = NULL;
    fd->tmpfs.dots_pos = 0; // start readdir at "."
    lock(&dirent->lock, 0);
    if (!list_empty(&dirent->children)) {
        tmpfs_fd_seekdir(fd, list_first_entry(&dirent->children, struct tmp_dirent, dir));
    }
    unlock(&dirent->lock);

    // A FIFO inode shares one named-pipe buffer across all opens; create it
    // lazily and run the POSIX open rendezvous. generic_openat drops the global
    // inodes_lock around this open (open_may_block) so blocking here is safe.
    if (S_ISFIFO(dirent->inode->stat.mode)) {
        struct tmp_inode *inode = dirent->inode;
        lock(&inode->lock, 0);
        if (inode->fifo == NULL)
            inode->fifo = fifo_file_new();
        struct fifo_file *fifo = inode->fifo;
        unlock(&inode->lock);
        if (fifo == NULL) {
            fd_close(fd);
            return ERR_PTR(_ENOMEM);
        }
        // generic_openat only assigns fd->flags after fs->open returns, but the
        // FIFO open rendezvous needs the access mode now.
        fd->flags = flags;
        int ferr = fifo_file_open(fifo, fd);
        if (ferr < 0) {
            fd_close(fd);
            return ERR_PTR(ferr);
        }
    }
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

// Move src -> dst entirely within the tmpfs tree. generic_renameat has already
// rejected cross-mount (EXDEV), unknown flags (EINVAL), and RENAME_NOREPLACE
// onto an existing dst (EEXIST); this handles the in-tree relocation plus
// overwrite/type/cycle rules. Without this op tmpfs had no ->rename, so
// generic_renameat returned EPERM for every rename on a tmpfs mount.
static int tmpfs_rename(struct mount *mount, const char *src, const char *dst) {
    const char *src_name;
    struct tmp_dirent *src_parent = tmpfs_lookup_parent(mount, src, &src_name);
    if (IS_ERR(src_parent))
        return PTR_ERR(src_parent);
    if (src_parent == NULL)
        return _EBUSY; // can't rename the mount root
    const char *dst_name;
    struct tmp_dirent *dst_parent = tmpfs_lookup_parent(mount, dst, &dst_name);
    if (IS_ERR(dst_parent)) {
        tmp_dirent_release(src_parent);
        return PTR_ERR(dst_parent);
    }
    if (dst_parent == NULL) {
        tmp_dirent_release(src_parent);
        return _EBUSY;
    }

    // Lock both parents in a stable (address) order so two concurrent renames
    // can't deadlock; same parent -> lock once. This is the only op that
    // relocates an entry, and two renames touching any common directory
    // serialize on that directory's lock, so parent-lock ordering alone keeps
    // the tree consistent without a separate moved-entry lock.
    bool same_parent = src_parent == dst_parent;
    struct tmp_dirent *lo = src_parent, *hi = dst_parent;
    if (lo > hi) { struct tmp_dirent *t = lo; lo = hi; hi = t; }
    lock(&lo->lock, 0);
    if (!same_parent)
        lock(&hi->lock, 0);

    int err = 0;
    struct tmp_dirent *dst_dirent = NULL;
    struct tmp_dirent *src_dirent = tmpfs_dir_lookup(src_parent, src_name);
    if (IS_ERR(src_dirent)) {
        err = PTR_ERR(src_dirent);
        src_dirent = NULL;
        goto out;
    }
    dst_dirent = tmpfs_dir_lookup(dst_parent, dst_name);
    if (IS_ERR(dst_dirent)) {
        if (dst_dirent != ERR_PTR(_ENOENT)) {
            err = PTR_ERR(dst_dirent);
            dst_dirent = NULL;
            goto out;
        }
        dst_dirent = NULL;
    }

    // rename(x, x) (same entry, including src==dst path) is a no-op success.
    if (src_dirent == dst_dirent)
        goto out;

    bool src_is_dir = S_ISDIR(src_dirent->inode->stat.mode);

    // Refuse to move a directory into itself or one of its own descendants,
    // which would splice a subtree into an unreachable cycle.
    if (src_is_dir) {
        for (struct tmp_dirent *p = dst_parent; p != NULL; p = p->parent) {
            if (p == src_dirent) {
                err = _EINVAL;
                goto out;
            }
        }
    }

    if (dst_dirent != NULL) {
        bool dst_is_dir = S_ISDIR(dst_dirent->inode->stat.mode);
        if (dst_is_dir && !src_is_dir) {
            err = _EISDIR;
            goto out;
        }
        if (!dst_is_dir && src_is_dir) {
            err = _ENOTDIR;
            goto out;
        }
        if (dst_is_dir) {
            lock(&dst_dirent->lock, 0);
            bool nonempty = !list_empty(&dst_dirent->children);
            unlock(&dst_dirent->lock);
            if (nonempty) {
                err = _ENOTEMPTY;
                goto out;
            }
        }
        // Overwrite: drop dst's tree reference (its lookup ref is released at
        // out, freeing it once no fd holds it).
        list_remove(&dst_dirent->dir);
        // nlink bookkeeping for the replaced inode, mirroring
        // tmpfs_dir_unlink: a replaced (empty) directory drops to 0 and
        // releases its ".." link on dst_parent; a replaced file loses its
        // one link.
        if (dst_is_dir) {
            dst_dirent->inode->stat.nlink = 0;
            dst_parent->inode->stat.nlink--;
        } else {
            dst_dirent->inode->stat.nlink--;
        }
        tmp_dirent_release(dst_dirent);
    }

    // Relocate src_dirent under dst_parent/dst_name, keeping its one tree
    // reference (it stays in the tree, just in a new list). Publish the new
    // parent pointer before releasing the old one so a concurrent reader of
    // ->parent (tmpfs_readdir's "..", tmpfs_getpath) never sees a dangling
    // field; the old parent is a non-empty directory holding its own tree
    // reference, so it stays alive regardless.
    list_remove(&src_dirent->dir);
    strncpy(src_dirent->name, dst_name, sizeof(src_dirent->name) - 1);
    src_dirent->name[sizeof(src_dirent->name) - 1] = '\0';
    if (src_dirent->parent != dst_parent) {
        struct tmp_dirent *old_parent = src_dirent->parent;
        src_dirent->parent = tmp_dirent_retain(dst_parent);
        // A directory moving between parents takes its ".." link with it.
        if (src_is_dir) {
            old_parent->inode->stat.nlink--;
            dst_parent->inode->stat.nlink++;
        }
        tmp_dirent_release(old_parent);
    }
    src_dirent->index = dst_parent->next_index++;
    list_add_tail(&dst_parent->children, &src_dirent->dir);

    tmpfs_update_mtime_and_ctime(src_parent->inode);
    tmpfs_update_mtime_and_ctime(dst_parent->inode);

out:
    if (dst_dirent != NULL)
        tmp_dirent_release(dst_dirent); // lookup ref
    if (src_dirent != NULL)
        tmp_dirent_release(src_dirent); // lookup ref
    if (!same_parent)
        unlock(&hi->lock);
    unlock(&lo->lock);
    tmp_dirent_release(dst_parent);
    tmp_dirent_release(src_parent);
    return err;
}

static int tmpfs_apply_attr(struct tmp_inode *inode, struct attr attr) {
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
    return err;
}

static int tmpfs_setattr(struct mount *mount, const char *path, struct attr attr) {
    struct tmp_dirent *dirent = tmpfs_lookup(mount, path);
    if (IS_ERR(dirent))
        return PTR_ERR(dirent);
    int err = tmpfs_apply_attr(dirent->inode, attr);
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
    struct tmp_inode *inode = fd->tmpfs.dirent->inode;
    if (S_ISFIFO(inode->stat.mode) && inode->fifo != NULL)
        fifo_file_close(inode->fifo, fd);
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
        parent->inode->stat.nlink++; // the new subdir's ".." link
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

static int tmpfs_symlink(struct mount *mount, const char *target, const char *link) {
    const char *filename;
    struct tmp_dirent *parent = tmpfs_lookup_parent(mount, link, &filename);
    if (IS_ERR(parent))
        return PTR_ERR(parent);
    if (parent == NULL)
        return _EEXIST;
    lock(&parent->lock, 0);

    int err = tmpfs_dir_lookup_existence(parent, filename);
    if (err < 0)
        goto out;

    struct tmp_inode *inode = tmp_inode_new(S_IFLNK | 0777);
    err = _ENOMEM;
    if (inode == NULL)
        goto out;
    // Store the link target in file_data (the union slot files use for content).
    size_t target_len = strlen(target);
    inode->file_data = malloc(target_len + 1);
    if (inode->file_data == NULL) {
        tmp_inode_release(inode);
        goto out;
    }
    memcpy(inode->file_data, target, target_len);
    inode->stat.size = target_len;

    err = tmpfs_dir_link(parent, filename, inode, NULL);
    tmp_inode_release(inode);
    if (err == 0)
        tmpfs_update_mtime_and_ctime(parent->inode);
out:
    unlock(&parent->lock);
    tmp_dirent_release(parent);
    return err;
}

static ssize_t tmpfs_readlink(struct mount *mount, const char *path, char *buf, size_t bufsize) {
    struct tmp_dirent *dirent = tmpfs_lookup(mount, path);
    if (IS_ERR(dirent))
        return PTR_ERR(dirent);
    struct tmp_inode *inode = dirent->inode;
    ssize_t res;
    lock(&inode->lock, 0);
    if (!S_ISLNK(inode->stat.mode)) {
        res = _EINVAL;
    } else {
        res = inode->stat.size;
        if ((size_t) res > bufsize)
            res = bufsize;
        memcpy(buf, inode->file_data, res);
    }
    unlock(&inode->lock);
    tmp_dirent_release(dirent);
    return res;
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
    memmove(buf, p, (size_t)((buf + MAX_PATH) - p));
    return 0;
}

static int tmpfs_fstat(struct fd *fd, struct statbuf *stat) {
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    lock(&inode->lock, 0);
    *stat = inode->stat;
    unlock(&inode->lock);
    return 0;
}

static int tmpfs_fsetattr(struct fd *fd, struct attr attr) {
    return tmpfs_apply_attr(tmpfs_fd_inode(fd), attr);
}

// fd-direct futimens/utimensat(fd, NULL): unlike tmpfs_utime there is no path
// lookup, so this works on an unlinked-but-open file like real Linux.
static int tmpfs_futime(struct fd *fd, struct timespec atime, struct timespec mtime) {
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    lock(&inode->lock, 0);
    inode->stat.atime = atime.tv_sec;
    inode->stat.atime_nsec = atime.tv_nsec;
    inode->stat.mtime = mtime.tv_sec;
    inode->stat.mtime_nsec = mtime.tv_nsec;
    tmpfs_update_ctime(inode);
    unlock(&inode->lock);
    return 0;
}

static ssize_t tmpfs_read(struct fd *fd, void *buf, size_t bufsize) {
    ssize_t res;
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    if (S_ISFIFO(inode->stat.mode))
        return fifo_file_read(inode->fifo, fd, buf, bufsize);
    lock(&inode->lock, 0);
    res = _EISDIR;
    if (S_ISDIR(inode->stat.mode))
        goto out;
    assert(S_ISREG(inode->stat.mode));

    // Snapshot fd->offset once (see tmpfs_write): a concurrent lseek/pwrite on
    // a shared fd could otherwise move it between the clamp and the memcpy,
    // making the memcpy read out of bounds.
    size_t off = fd->offset;

    if (inode->host_fd >= 0) {
        // Host-file-backed (has been mmapped): read the host file so writes
        // made through a MAP_SHARED guest mapping are visible.
        ssize_t n = pread(inode->host_fd, buf, bufsize, off);
        if (n < 0) {
            res = errno_map();
            goto out;
        }
        fd->offset = off + n;
        res = n;
        goto out;
    }

    // Clamp to the bytes actually available. The past-EOF check must come
    // first: stat.size and off are unsigned, so computing stat.size - off when
    // off > size underflows to a huge value, leaving bufsize unclamped and
    // making the memcpy read wildly out of bounds (a pread past EOF on tmpfs ->
    // SIGSEGV; Linux just returns 0).
    if (off >= inode->stat.size)
        bufsize = 0;
    else if (bufsize > inode->stat.size - off)
        bufsize = inode->stat.size - off;
    memcpy(buf, (char *) inode->file_data + off, bufsize);
    fd->offset = off + bufsize;
    res = bufsize;

out:
    unlock(&inode->lock);
    return res;
}

// Positional read: same clamping rules as tmpfs_read, but the caller supplies
// the offset and fd->offset is left alone. This must exist: kernel/exec.c's
// loader calls fd->ops->pread directly for the residual file bytes of an
// EOF-straddling PT_LOAD, so execve of any binary living on a tmpfs (e.g.
// test binaries built under /tmp once tmp.mount works) jumped through a NULL
// pread and took the whole app down with EXC_BAD_ACCESS.
static ssize_t tmpfs_pread(struct fd *fd, void *buf, size_t bufsize, off_t off) {
    ssize_t res;
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    if (S_ISFIFO(inode->stat.mode))
        return _ESPIPE;
    if (off < 0)
        return _EINVAL;
    lock(&inode->lock, 0);
    res = _EISDIR;
    if (S_ISDIR(inode->stat.mode))
        goto out;
    assert(S_ISREG(inode->stat.mode));
    if (inode->host_fd >= 0) {
        ssize_t n = pread(inode->host_fd, buf, bufsize, off);
        res = n < 0 ? errno_map() : n;
        goto out;
    }
    // Past-EOF first: unsigned math (see tmpfs_read).
    if ((size_t) off >= inode->stat.size)
        bufsize = 0;
    else if (bufsize > inode->stat.size - (size_t) off)
        bufsize = inode->stat.size - (size_t) off;
    memcpy(buf, (char *) inode->file_data + off, bufsize);
    res = (ssize_t) bufsize;
out:
    unlock(&inode->lock);
    return res;
}

static void tmpfs_cgroup2_note_procs_write(struct fd *fd, const void *buf, size_t bufsize);

// Positional write, mirroring tmpfs_write minus the fd->offset update.
static ssize_t tmpfs_pwrite(struct fd *fd, const void *buf, size_t bufsize, off_t off) {
    ssize_t res;
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    if (S_ISFIFO(inode->stat.mode))
        return _ESPIPE;
    if (off < 0)
        return _EINVAL;
    lock(&inode->lock, 0);
    res = _EISDIR;
    if (S_ISDIR(inode->stat.mode))
        goto out;
    assert(S_ISREG(inode->stat.mode));
    size_t end;
    if (__builtin_add_overflow((size_t) off, bufsize, &end)) {
        res = _EFBIG;
        goto out;
    }
    if (inode->host_fd >= 0) {
        ssize_t n = pwrite(inode->host_fd, buf, bufsize, off);
        if (n < 0) {
            res = errno_map();
            goto out;
        }
        if (inode->stat.size < (size_t) off + (size_t) n)
            inode->stat.size = off + n;
        if (n > 0)
            tmpfs_update_mtime_and_ctime(inode);
        res = n;
        goto out;
    }
    if (inode->stat.size < end) {
        res = tmpfs_file_resize(inode, end);
        if (res < 0)
            goto out;
    }
    memcpy((char *) inode->file_data + off, buf, bufsize);
    res = (ssize_t) bufsize;
out:
    unlock(&inode->lock);
    if (res >= 0)
        tmpfs_cgroup2_note_procs_write(fd, buf, bufsize);
    return res;
}

// A pid written to a cgroup2 hierarchy's cgroup.procs moves that process
// into the cgroup. The fake hierarchy stores the write like any tmpfs file;
// this additionally records the membership on the process's tgroup so
// /proc/<pid>/cgroup can report it. systemd --user depends on that: it
// derives its own delegated subtree from /proc/self/cgroup, and the
// previously hardcoded "0::/" made it try to create init.scope at the
// hierarchy ROOT -- EACCES for a non-root user manager, so every user@
// start died with "Failed to allocate manager object: Permission denied".
static void tmpfs_cgroup2_note_procs_write(struct fd *fd, const void *buf, size_t bufsize) {
    if (!tmpfs_is_cgroup2_mount(fd->mount))
        return;
    if (strcmp(fd->tmpfs.dirent->name, "cgroup.procs") != 0)
        return;
    char num[24];
    if (bufsize == 0 || bufsize >= sizeof(num))
        return;
    memcpy(num, buf, bufsize);
    num[bufsize] = '\0';
    pid_t_ pid = (pid_t_) atoi(num);
    if (pid <= 0)
        return;
    // The cgroup is cgroup.procs's parent directory; its mount-relative path
    // IS the hierarchy-relative path (the mount root is the cgroup root).
    char path[MAX_PATH];
    if (tmpfs_getpath(fd, path) < 0)
        return;
    size_t plen = strlen(path);
    const char suffix[] = "/cgroup.procs";
    if (plen < sizeof(suffix) - 1)
        return;
    path[plen - (sizeof(suffix) - 1)] = '\0';
    if (path[0] == '\0')
        strcpy(path, "/");

    complex_lockt(&pids_lock, 0);
    struct task *task = pid_get_task(pid);
    if (task != NULL && task->group != NULL) {
        lock(&task->group->lock, 0);
        free(task->group->cgroup_path);
        task->group->cgroup_path = strdup(path);
        unlock(&task->group->lock);
    }
    unlock(&pids_lock);
}

static ssize_t tmpfs_write(struct fd *fd, const void *buf, size_t bufsize) {
    ssize_t res;
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    if (S_ISFIFO(inode->stat.mode))
        return fifo_file_write(inode->fifo, fd, buf, bufsize);
    lock(&inode->lock, 0);
    res = _EISDIR;
    if (S_ISDIR(inode->stat.mode))
        goto out;
    assert(S_ISREG(inode->stat.mode));

    // Snapshot fd->offset ONCE. lseek/pwrite mutate fd->offset without holding
    // inode->lock, and a struct fd is shared across dup/fork/threads. If the
    // offset moved between sizing the buffer (below) and the memcpy, the memcpy
    // would land past the just-resized buffer -> OOB write / SIGBUS. Using a
    // single snapshot for the grow, the memcpy target, and the advance keeps
    // them internally consistent regardless of a concurrent offset change.
    size_t off = fd->offset;
    // Guard against off + bufsize wrapping (a write at an offset near SIZE_MAX
    // would otherwise skip the grow and memcpy far past the buffer).
    size_t end;
    if (__builtin_add_overflow(off, bufsize, &end)) {
        res = _EFBIG;
        goto out;
    }
    if (inode->host_fd >= 0) {
        // Host-file-backed (has been mmapped): write the host file so the data
        // is visible through any MAP_SHARED guest mapping.
        ssize_t n = pwrite(inode->host_fd, buf, bufsize, off);
        if (n < 0) {
            res = errno_map();
            goto out;
        }
        fd->offset = off + n;
        if (inode->stat.size < off + (size_t) n)
            inode->stat.size = off + n;
        if (n > 0)
            tmpfs_update_mtime_and_ctime(inode);
        res = n;
        goto out;
    }
    if (inode->stat.size < end) {
        res = tmpfs_file_resize(inode, end);
        if (res < 0)
            goto out;
    }
    memcpy((char *) inode->file_data + off, buf, bufsize);
    fd->offset = off + bufsize;
    res = bufsize;

out:
    unlock(&inode->lock);
    // After dropping inode->lock: the hook takes pids_lock, which must not
    // nest inside it.
    if (res >= 0)
        tmpfs_cgroup2_note_procs_write(fd, buf, bufsize);
    return res;
}

static unsigned long tmpfs_telldir(struct fd *fd);
static void tmpfs_seekdir(struct fd *fd, unsigned long ptr);

static off_t_ tmpfs_lseek(struct fd *fd, off_t_ off, int whence) {
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    if (S_ISDIR(inode->stat.mode)) {
        // A directory's "offset" is the opaque telldir/seekdir cookie, not a
        // byte position. seekdir()/rewinddir() reach the kernel as
        // lseek(SEEK_SET) (rewinddir is seekdir(0)); route them through the
        // readdir cursor so they actually reposition instead of only moving
        // the unused fd->offset.
        off_t_ base;
        switch (whence) {
            case LSEEK_SET: base = 0; break;
            case LSEEK_CUR: base = (off_t_) tmpfs_telldir(fd); break;
            default: return _EINVAL; // SEEK_END is meaningless for dir cookies
        }
        off_t_ target = base + off;
        if (target < 0)
            return _EINVAL;
        tmpfs_seekdir(fd, (unsigned long) target);
        fd->offset = target;
        return target;
    }

    qword_t size = 0;
    if (whence == LSEEK_END) {
        lock(&inode->lock, 0);
        size = inode->stat.size;
        unlock(&inode->lock);
    }

    int err = generic_seek(fd, off, whence, size);
    if (err < 0)
        return err;

    return fd->offset;
}

// Directory offsets (telldir/seekdir) encode the readdir phase so "." and ".."
// round-trip: 0 = ".", 1 = "..", 2 + child->index = a real child, and
// 2 + next_index (past every current child) = end. Child indices are monotonic
// (next_index++, never reused), so 2+index never collides with the 0/1 dot
// slots, and all cookies stay non-negative as an off_t.
#define TMPFS_DIROFF_DOT     0
#define TMPFS_DIROFF_DOTDOT  1
#define TMPFS_DIROFF_CHILD0  2

// Point dir_pos at the first child (or NULL if empty). Caller holds dir->lock.
static void tmpfs_dir_pos_first(struct fd *fd, struct tmp_dirent *dir) {
    struct tmp_dirent *first = NULL;
    if (!list_empty(&dir->children))
        first = list_first_entry(&dir->children, struct tmp_dirent, dir);
    tmpfs_fd_seekdir(fd, first);
}

static int tmpfs_readdir(struct fd *fd, struct dir_entry *entry) {
    struct tmp_dirent *parent = fd->tmpfs.dirent;
    int res = _ENOTDIR;
    if (!S_ISDIR(parent->inode->stat.mode))
        return res;

    lock(&fd->lock, 0);
    lock(&parent->lock, 0);

    // Synthesize "." and ".." up front, like every real directory. tmpfs used
    // to emit neither, so getdents on an empty tmpfs dir returned 0 (and a
    // too-small buffer wrongly reported EOF instead of EINVAL); nftw-style
    // walkers also mis-handled the missing dots. ".."  of the mount root has no
    // parent dirent here, so it reflects the root's own inode.
    if (fd->tmpfs.dots_pos <= TMPFS_DIROFF_DOTDOT) {
        struct tmp_dirent *self;
        const char *name;
        if (fd->tmpfs.dots_pos == TMPFS_DIROFF_DOT) {
            self = parent;
            name = ".";
        } else {
            self = parent->parent != NULL ? parent->parent : parent;
            name = "..";
        }
        entry->inode = self->inode->stat.inode;
        entry->type = dir_entry_type_for_mode(self->inode->stat.mode);
        strcpy(entry->name, name);
        fd->tmpfs.dots_pos++;
        res = 1;
        goto out;
    }

    struct tmp_dirent *dirent = fd->tmpfs.dir_pos;
    if (dirent == NULL) {
        res = 0;
        goto out;
    }

    // Fill the entry from the current cursor BEFORE advancing. Advancing calls
    // tmpfs_fd_seekdir, which releases this dirent's dir_pos reference; if the
    // dirent was already unlink()'d (its tree reference gone, dir_pos holding
    // the last one) that release frees it, so touching dirent->inode afterward
    // is a use-after-free (the tmpfs_readdir SIGSEGV under stress-ng
    // --filerace: dirent->inode read as garbage/NULL). dir_pos keeps it alive
    // until we advance, so reading it here is safe.
    entry->inode = dirent->inode->stat.inode;
    entry->type = dir_entry_type_for_mode(dirent->inode->stat.mode);
    strncpy(entry->name, dirent->name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    res = 1;

    // Advance the cursor. dir_pos is retained, so it can't be freed here, but a
    // concurrent unlink may have list_remove'd it (NULLing its links). In that
    // case list_next_entry would follow a NULL link to a bogus address; fall
    // back to resuming by index, which is stable across removals.
    struct tmp_dirent *next_dirent;
    if (list_null(&dirent->dir)) {
        next_dirent = NULL;
        struct tmp_dirent *c;
        list_for_each_entry(&parent->children, c, dir) {
            if (c->index > dirent->index) {
                next_dirent = c;
                break;
            }
        }
    } else {
        next_dirent = list_next_entry(dirent, dir);
        if (&next_dirent->dir == &parent->children) // end of list
            next_dirent = NULL;
    }
    tmpfs_fd_seekdir(fd, next_dirent);

out:
    unlock(&parent->lock);
    unlock(&fd->lock);
    return res;
}

static unsigned long tmpfs_telldir(struct fd *fd) {
    if (fd->tmpfs.dots_pos == TMPFS_DIROFF_DOT)
        return TMPFS_DIROFF_DOT;
    if (fd->tmpfs.dots_pos == TMPFS_DIROFF_DOTDOT)
        return TMPFS_DIROFF_DOTDOT;
    if (fd->tmpfs.dir_pos == NULL)
        // Past the last child: a cookie beyond every current index. It must be
        // non-negative (this value round-trips through lseek(SEEK_SET) as an
        // off_t, and pread/pwrite's fallback asserts the restored offset is
        // >= 0), so we can't use ~0. next_index only grows, so this stays past
        // the end even as entries are added; seekdir maps it back to NULL.
        return TMPFS_DIROFF_CHILD0 + fd->tmpfs.dirent->next_index;
    return TMPFS_DIROFF_CHILD0 + fd->tmpfs.dir_pos->index;
}

static void tmpfs_seekdir(struct fd *fd, unsigned long ptr) {
    struct tmp_dirent *dir = fd->tmpfs.dirent;
    lock(&dir->lock, 0);
    assert(S_ISDIR(dir->inode->stat.mode));
    if (ptr == TMPFS_DIROFF_DOT || ptr == TMPFS_DIROFF_DOTDOT) {
        fd->tmpfs.dots_pos = (unsigned) ptr;
        tmpfs_dir_pos_first(fd, dir);
    } else {
        // Any child cookie (>= CHILD0) lands on the first child whose index is
        // at least the target; a cookie past the last index finds none and
        // parks the cursor at end (NULL).
        fd->tmpfs.dots_pos = TMPFS_DIROFF_CHILD0;
        unsigned long target = ptr - TMPFS_DIROFF_CHILD0;
        struct tmp_dirent *child;
        list_for_each_entry(&dir->children, child, dir) {
            if (child->index >= target)
                break;
        }
        if (&child->dir == &dir->children)
            child = NULL;
        tmpfs_fd_seekdir(fd, child);
    }
    unlock(&dir->lock);
}

#define TMPFS_BLOCK_SIZE 4096

static void tmpfs_count_tree(struct tmp_dirent *dir, uint64_t *pages, uint64_t *inodes) {
    struct tmp_dirent *child;
    lock(&dir->lock, 0);
    list_for_each_entry(&dir->children, child, dir) {
        if (child->inode == NULL)
            continue;
        (*inodes)++;
        if (S_ISREG(child->inode->stat.mode))
            *pages += ((uint64_t) child->inode->stat.size + TMPFS_BLOCK_SIZE - 1) / TMPFS_BLOCK_SIZE;
        if (S_ISDIR(child->inode->stat.mode))
            tmpfs_count_tree(child, pages, inodes);
    }
    unlock(&dir->lock);
}

static int tmpfs_statfs(struct mount *mount, struct statfsbuf *stat) {
    // Linux tmpfs reports the mount's size limit as f_blocks (default: half
    // of RAM) and decrements f_bfree/f_bavail by pages actually stored; the
    // inode cap defaults to the same count as the block cap. There is no
    // size= limit enforcement here, so report the Linux default cap derived
    // from host RAM and subtract what the mount's inodes actually hold.
    uint64_t total_pages = 0;
    long phys_pages = sysconf(_SC_PHYS_PAGES);
    long host_page_size = sysconf(_SC_PAGESIZE);
    if (phys_pages > 0 && host_page_size > 0)
        total_pages = (uint64_t) phys_pages * host_page_size / 2 / TMPFS_BLOCK_SIZE;
    if (total_pages == 0)
        total_pages = 1 << 18; // 1 GiB fallback

    uint64_t used_pages = 0, used_inodes = 1; // root inode
    struct tmp_dirent *root = mount->data;
    if (root != NULL)
        tmpfs_count_tree(root, &used_pages, &used_inodes);

    stat->bsize = TMPFS_BLOCK_SIZE;
    stat->frsize = TMPFS_BLOCK_SIZE;
    stat->blocks = total_pages;
    stat->bfree = stat->bavail = used_pages < total_pages ? total_pages - used_pages : 0;
    stat->files = total_pages;
    stat->ffree = used_inodes < total_pages ? total_pages - used_inodes : 0;
    stat->namelen = 255;
    return 0;
}

// Linux reports zero block and inode counts for cgroup filesystems, just the
// block size and name limit.
static int cgroupfs_statfs(struct mount *UNUSED(mount), struct statfsbuf *stat) {
    stat->bsize = TMPFS_BLOCK_SIZE;
    stat->frsize = TMPFS_BLOCK_SIZE;
    stat->namelen = 255;
    return 0;
}

const struct fs_ops tmpfs = {
    .name = "tmpfs", .magic = 0x01021994,
    .mount = tmpfs_mount,
    .umount = tmpfs_umount,
    .statfs = tmpfs_statfs,
    .open = tmpfs_open,
    .close = tmpfs_close,
    .stat = tmpfs_stat,
    .unlink = tmpfs_unlink,
    .rmdir = tmpfs_rmdir,
    .rename = tmpfs_rename,
    .fstat = tmpfs_fstat,
    .setattr = tmpfs_setattr,
    .fsetattr = tmpfs_fsetattr,
    .utime = tmpfs_utime,
    .futime = tmpfs_futime,
    .getpath = tmpfs_getpath,
    .mkdir = tmpfs_mkdir,
    .mknod = tmpfs_mknod,
    .symlink = tmpfs_symlink,
    .readlink = tmpfs_readlink,
};

// Real devtmpfs is tmpfs underneath and reports TMPFS_MAGIC from statfs, so
// this differs from tmpfs only in the name (which drives the mount-time
// population above, and is what shows up in /proc/filesystems and mountinfo).
const struct fs_ops devtmpfs = {
    .name = "devtmpfs", .magic = 0x01021994,
    .mount = tmpfs_mount,
    .umount = tmpfs_umount,
    .statfs = tmpfs_statfs,
    .open = tmpfs_open,
    .close = tmpfs_close,
    .stat = tmpfs_stat,
    .unlink = tmpfs_unlink,
    .rmdir = tmpfs_rmdir,
    .rename = tmpfs_rename,
    .fstat = tmpfs_fstat,
    .setattr = tmpfs_setattr,
    .fsetattr = tmpfs_fsetattr,
    .utime = tmpfs_utime,
    .futime = tmpfs_futime,
    .getpath = tmpfs_getpath,
    .mkdir = tmpfs_mkdir,
    .mknod = tmpfs_mknod,
    .symlink = tmpfs_symlink,
    .readlink = tmpfs_readlink,
};

const struct fs_ops cgroupfs = {
    .name = "cgroup", .magic = 0x27e0eb,
    .mount = tmpfs_mount,
    .umount = tmpfs_umount,
    .statfs = cgroupfs_statfs,
    .open = tmpfs_open,
    .close = tmpfs_close,
    .stat = tmpfs_stat,
    .unlink = tmpfs_unlink,
    .rmdir = tmpfs_rmdir,
    .fstat = tmpfs_fstat,
    .setattr = tmpfs_setattr,
    .fsetattr = tmpfs_fsetattr,
    .utime = tmpfs_utime,
    .futime = tmpfs_futime,
    .getpath = tmpfs_getpath,
    .mkdir = tmpfs_mkdir,
    .mknod = tmpfs_mknod,
    .symlink = tmpfs_symlink,
    .readlink = tmpfs_readlink,
};

const struct fs_ops cgroup2fs = {
    .name = "cgroup2", .magic = 0x63677270,
    .mount = tmpfs_mount,
    .umount = tmpfs_umount,
    .statfs = cgroupfs_statfs,
    .open = tmpfs_open,
    .close = tmpfs_close,
    .stat = tmpfs_stat,
    .unlink = tmpfs_unlink,
    .rmdir = tmpfs_rmdir,
    .fstat = tmpfs_fstat,
    .setattr = tmpfs_setattr,
    .fsetattr = tmpfs_fsetattr,
    .utime = tmpfs_utime,
    .futime = tmpfs_futime,
    .getpath = tmpfs_getpath,
    .mkdir = tmpfs_mkdir,
    .mknod = tmpfs_mknod,
    .symlink = tmpfs_symlink,
    .readlink = tmpfs_readlink,
};

static int tmpfs_poll(struct fd *fd) {
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    if (S_ISFIFO(inode->stat.mode))
        return fifo_file_poll(inode->fifo, fd);
    return POLL_READ | POLL_WRITE;
}

static int tmpfs_mmap(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags) {
    struct tmp_inode *inode = tmpfs_fd_inode(fd);
    // Linux (verified): mmap of a tmpfs directory or FIFO fails with ENODEV.
    if (!S_ISREG(inode->stat.mode))
        return _ENODEV;
    // Linux mmap access checks (verified): the fd must be readable for any
    // mapping, and MAP_SHARED + PROT_WRITE additionally needs it writable.
    // realfs gets these for free from the host mmap of real_fd (opened with
    // the same access mode); here the backing host fd is always O_RDWR, so
    // check the guest fd's mode explicitly.
    int accmode = fd->flags & O_ACCMODE_;
    if (accmode == O_WRONLY_)
        return _EACCES;
    if ((flags & MMAP_SHARED) && (prot & P_WRITE) && accmode != O_RDWR_)
        return _EACCES;
    // Linux (verified): a file offset that isn't page-aligned is EINVAL.
    if (offset % PAGE_SIZE != 0)
        return _EINVAL;
    lock(&inode->lock, 0);
    int host_fd = tmpfs_inode_host_backing(inode);
    unlock(&inode->lock);
    if (host_fd < 0)
        return host_fd;
    return host_fd_mmap(host_fd, mem, start, pages, offset, prot, flags);
}

const struct fd_ops tmpfs_fdops = {
    .read = tmpfs_read,
    .write = tmpfs_write,
    .pread = tmpfs_pread,
    .pwrite = tmpfs_pwrite,
    .poll = tmpfs_poll,
    .lseek = tmpfs_lseek,
    .mmap = tmpfs_mmap,
    .readdir = tmpfs_readdir,
    .telldir = tmpfs_telldir,
    .seekdir = tmpfs_seekdir,
};
