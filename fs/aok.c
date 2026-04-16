#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "fs/fd.h"

#define AOKFS_MAGIC 0x414f4b31

enum aokfs_node_kind {
    aokfs_root = 1,
    aokfs_readme,
    aokfs_version,
    aokfs_tests_dir,
    aokfs_tests_audio_dir,
    aokfs_audio_raw,
    aokfs_audio_wav,
};

static enum aokfs_node_kind aokfs_decode_node(void *fs_data) {
    return (enum aokfs_node_kind) (uintptr_t) fs_data;
}

static void *aokfs_encode_node(enum aokfs_node_kind node) {
    return (void *) (uintptr_t) node;
}

static bool aokfs_node_is_dir(enum aokfs_node_kind node) {
    return node == aokfs_root || node == aokfs_tests_dir || node == aokfs_tests_audio_dir;
}

static bool aokfs_node_is_bundled_file(enum aokfs_node_kind node) {
    return node == aokfs_audio_raw || node == aokfs_audio_wav;
}

static mode_t_ aokfs_node_mode(enum aokfs_node_kind node) {
    if (aokfs_node_is_dir(node))
        return S_IFDIR | 0555;
    return S_IFREG | 0444;
}

static qword_t aokfs_node_inode(enum aokfs_node_kind node) {
    return (qword_t) node;
}

static const char *aokfs_node_path(enum aokfs_node_kind node) {
    switch (node) {
        case aokfs_root:
            return "";
        case aokfs_readme:
            return "/README.txt";
        case aokfs_version:
            return "/VERSION";
        case aokfs_tests_dir:
            return "/tests";
        case aokfs_tests_audio_dir:
            return "/tests/audio";
        case aokfs_audio_raw:
            return "/tests/audio/test-tone-48k-s16le-stereo.raw";
        case aokfs_audio_wav:
            return "/tests/audio/test-tone-48k-s16le-stereo.wav";
    }
    return "";
}

static const char *aokfs_node_basename(enum aokfs_node_kind node) {
    const char *path = aokfs_node_path(node);
    const char *slash = strrchr(path, '/');
    return slash == NULL ? path : slash + 1;
}

static bool aokfs_lookup_node(const char *path, enum aokfs_node_kind *node_out) {
    static const enum aokfs_node_kind nodes[] = {
        aokfs_root,
        aokfs_readme,
        aokfs_version,
        aokfs_tests_dir,
        aokfs_tests_audio_dir,
        aokfs_audio_raw,
        aokfs_audio_wav,
    };

    for (size_t i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++) {
        enum aokfs_node_kind node = nodes[i];
        if (strcmp(path, aokfs_node_path(node)) == 0) {
            *node_out = node;
            return true;
        }
    }
    return false;
}

static const char *aokfs_inline_file_data(enum aokfs_node_kind node, size_t *size_out) {
    static const char readme[] =
        "iSH-AOK support files\n"
        "\n"
        "This is a small read-only pseudo-filesystem provided by iSH-AOK.\n"
        "It is mounted at /AOK regardless of the installed Linux rootfs.\n";
    static const char version[] = "iSH-AOK\n";

    switch (node) {
        case aokfs_readme:
            *size_out = sizeof(readme) - 1;
            return readme;
        case aokfs_version:
            *size_out = sizeof(version) - 1;
            return version;
        default:
            *size_out = 0;
            return "";
    }
}

static int aokfs_open_backing_file(struct mount *mount, enum aokfs_node_kind node) {
    if (!aokfs_node_is_bundled_file(node))
        return -1;

    char path[MAX_PATH + 1];
    int err = snprintf(path, sizeof(path), "%s/%s", mount->source, aokfs_node_basename(node));
    if (err < 0 || err >= (int) sizeof(path))
        return -1;

    return open(path, O_RDONLY);
}

static int aokfs_inline_stat(enum aokfs_node_kind node, struct statbuf *stat) {
    size_t size = 0;
    aokfs_inline_file_data(node, &size);
    stat->size = size;
    stat->blksize = 4096;
    stat->blocks = (size + 511) / 512;
    return 0;
}

static int aokfs_host_stat(struct mount *mount, enum aokfs_node_kind node, struct statbuf *stat) {
    int fd = aokfs_open_backing_file(mount, node);
    if (fd < 0)
        return _ENOENT;

    struct stat host_stat;
    if (fstat(fd, &host_stat) < 0) {
        close(fd);
        return errno_map();
    }
    close(fd);

    stat->size = host_stat.st_size;
    stat->blksize = host_stat.st_blksize;
    stat->blocks = host_stat.st_blocks;
    return 0;
}

static int aokfs_stat_common(struct mount *mount, enum aokfs_node_kind node, struct statbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    stat->inode = aokfs_node_inode(node);
    stat->mode = aokfs_node_mode(node);
    stat->nlink = aokfs_node_is_dir(node) ? 2 : 1;

    if (aokfs_node_is_dir(node)) {
        stat->blksize = 4096;
        return 0;
    }

    if (aokfs_node_is_bundled_file(node))
        return aokfs_host_stat(mount, node, stat);
    return aokfs_inline_stat(node, stat);
}

static int aokfs_statfs(struct mount *UNUSED(mount), struct statfsbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    stat->type = AOKFS_MAGIC;
    stat->bsize = 4096;
    stat->files = 7;
    stat->ffree = 0;
    stat->namelen = NAME_MAX;
    stat->flags = MS_READONLY_;
    return 0;
}

static int aokfs_stat(struct mount *mount, const char *path, struct statbuf *stat) {
    enum aokfs_node_kind node;
    if (!aokfs_lookup_node(path, &node))
        return _ENOENT;
    return aokfs_stat_common(mount, node, stat);
}

static int aokfs_fstat(struct fd *fd, struct statbuf *stat) {
    return aokfs_stat_common(fd->mount, aokfs_decode_node(fd->fs_data), stat);
}

static int aokfs_getpath(struct fd *fd, char *buf) {
    strcpy(buf, aokfs_node_path(aokfs_decode_node(fd->fs_data)));
    return 0;
}

static ssize_t aokfs_pread(struct fd *fd, void *buf, size_t bufsize, off_t off) {
    enum aokfs_node_kind node = aokfs_decode_node(fd->fs_data);
    if (aokfs_node_is_dir(node))
        return _EISDIR;

    if (fd->real_fd >= 0) {
        ssize_t res = pread(fd->real_fd, buf, bufsize, off);
        if (res < 0)
            return errno_map();
        return res;
    }

    size_t size = 0;
    const char *data = aokfs_inline_file_data(node, &size);
    if ((size_t) off > size)
        return 0;
    size_t remaining = size - off;
    if (bufsize > remaining)
        bufsize = remaining;
    memcpy(buf, data + off, bufsize);
    return bufsize;
}

static ssize_t aokfs_read(struct fd *fd, void *buf, size_t bufsize) {
    ssize_t res = aokfs_pread(fd, buf, bufsize, fd->offset);
    if (res > 0)
        fd->offset += res;
    return res;
}

static ssize_t aokfs_write(struct fd *UNUSED(fd), const void *UNUSED(buf), size_t UNUSED(bufsize)) {
    return _EROFS;
}

static ssize_t aokfs_pwrite(struct fd *UNUSED(fd), const void *UNUSED(buf), size_t UNUSED(bufsize), off_t UNUSED(off)) {
    return _EROFS;
}

static off_t_ aokfs_lseek(struct fd *fd, off_t_ off, int whence) {
    enum aokfs_node_kind node = aokfs_decode_node(fd->fs_data);
    if (aokfs_node_is_dir(node))
        return _EINVAL;

    struct statbuf stat;
    int err = aokfs_stat_common(fd->mount, node, &stat);
    if (err < 0)
        return err;
    return generic_seek(fd, off, whence, stat.size);
}

static int aokfs_readdir(struct fd *fd, struct dir_entry *entry) {
    enum aokfs_node_kind node = aokfs_decode_node(fd->fs_data);
    enum aokfs_node_kind child;

    switch (node) {
        case aokfs_root:
            switch (fd->offset++) {
                case 0: child = aokfs_readme; break;
                case 1: child = aokfs_version; break;
                case 2: child = aokfs_tests_dir; break;
                default: return 0;
            }
            break;
        case aokfs_tests_dir:
            if (fd->offset++ != 0)
                return 0;
            child = aokfs_tests_audio_dir;
            break;
        case aokfs_tests_audio_dir:
            switch (fd->offset++) {
                case 0: child = aokfs_audio_raw; break;
                case 1: child = aokfs_audio_wav; break;
                default: return 0;
            }
            break;
        default:
            return _ENOTDIR;
    }

    entry->inode = aokfs_node_inode(child);
    strncpy(entry->name, aokfs_node_basename(child), sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    return 1;
}

static int aokfs_close(struct fd *fd) {
    if (fd->real_fd >= 0) {
        if (close(fd->real_fd) < 0)
            return errno_map();
        fd->real_fd = -1;
    }
    return 0;
}

static const struct fd_ops aokfs_fdops = {
    .read = aokfs_read,
    .write = aokfs_write,
    .pread = aokfs_pread,
    .pwrite = aokfs_pwrite,
    .lseek = aokfs_lseek,
    .readdir = aokfs_readdir,
    .close = aokfs_close,
};

static struct fd *aokfs_open(struct mount *mount, const char *path, int UNUSED(flags), int UNUSED(mode)) {
    enum aokfs_node_kind node;
    if (!aokfs_lookup_node(path, &node))
        return ERR_PTR(_ENOENT);

    struct fd *fd = fd_create(&aokfs_fdops);
    if (fd == NULL)
        return ERR_PTR(_ENOMEM);
    mount_retain(mount);
    fd->mount = mount;
    fd->type = aokfs_node_mode(node) & S_IFMT;
    fd->fs_data = aokfs_encode_node(node);
    fd->real_fd = -1;

    if (aokfs_node_is_bundled_file(node)) {
        fd->real_fd = aokfs_open_backing_file(mount, node);
        if (fd->real_fd < 0) {
            fd_close(fd);
            return ERR_PTR(_ENOENT);
        }
    }

    return fd;
}

const struct fs_ops aokfs = {
    .name = "aokfs",
    .magic = AOKFS_MAGIC,
    .statfs = aokfs_statfs,
    .open = aokfs_open,
    .stat = aokfs_stat,
    .fstat = aokfs_fstat,
    .getpath = aokfs_getpath,
};
