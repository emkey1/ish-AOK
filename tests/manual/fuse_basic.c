// fuse_basic.c — self-checking regression lock for iSH-AOK's FUSE support
// (fs/fuse.c): the /dev/fuse device, the "fuse" filesystem type, and the
// protocol conversation between them.
//
// Deliberately speaks the RAW protocol rather than using libfuse, so the
// suite needs no packages: the child process is a minimal single-threaded
// FUSE daemon (flat root, 8 entries) reading /dev/fuse and answering
// LOOKUP/GETATTR/CREATE/OPEN/READ/WRITE/SETATTR/READDIR/MKDIR/RMDIR/
// RENAME/UNLINK/STATFS, while the parent exercises the mount with ordinary
// syscalls and asserts what comes back.
//
// Portable: passes identically on a real Linux kernel run as root (the
// daemon replies -ENOSYS to opcodes iSH-AOK never sends but real kernels
// do, and ignores the no-reply opcodes FORGET/BATCH_FORGET/DESTROY). An
// unprivileged real-Linux run skips at mount(2)'s EPERM.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include "test_common.h"

// -- protocol (linux/fuse.h ABI) --

#define FUSE_LOOKUP 1
#define FUSE_FORGET 2
#define FUSE_GETATTR 3
#define FUSE_SETATTR 4
#define FUSE_MKDIR 9
#define FUSE_UNLINK 10
#define FUSE_RMDIR 11
#define FUSE_RENAME 12
#define FUSE_OPEN 14
#define FUSE_READ 15
#define FUSE_WRITE 16
#define FUSE_STATFS 17
#define FUSE_RELEASE 18
#define FUSE_FLUSH 25
#define FUSE_INIT 26
#define FUSE_OPENDIR 27
#define FUSE_READDIR 28
#define FUSE_RELEASEDIR 29
#define FUSE_ACCESS 34
#define FUSE_CREATE 35
#define FUSE_DESTROY 38
#define FUSE_BATCH_FORGET 42

struct fuse_in_header {
    uint32_t len, opcode;
    uint64_t unique, nodeid;
    uint32_t uid, gid, pid, padding;
};
struct fuse_out_header {
    uint32_t len;
    int32_t error;
    uint64_t unique;
};
struct fuse_wire_attr {
    uint64_t ino, size, blocks, atime, mtime, ctime;
    uint32_t atimensec, mtimensec, ctimensec;
    uint32_t mode, nlink, uid, gid, rdev, blksize, padding;
};
struct fuse_entry_out {
    uint64_t nodeid, generation, entry_valid, attr_valid;
    uint32_t entry_valid_nsec, attr_valid_nsec;
    struct fuse_wire_attr attr;
};
struct fuse_attr_out {
    uint64_t attr_valid;
    uint32_t attr_valid_nsec, dummy;
    struct fuse_wire_attr attr;
};
struct fuse_init_in {
    uint32_t major, minor, max_readahead, flags;
};
struct fuse_init_out {
    uint32_t major, minor, max_readahead, flags;
    uint16_t max_background, congestion_threshold;
    uint32_t max_write, time_gran;
    uint16_t max_pages, map_alignment;
    uint32_t unused[8];
};
struct fuse_open_out {
    uint64_t fh;
    uint32_t open_flags, padding;
};
struct fuse_create_in {
    uint32_t flags, mode, umask, padding;
};
struct fuse_read_in {
    uint64_t fh, offset;
    uint32_t size, read_flags;
    uint64_t lock_owner;
    uint32_t flags, padding;
};
struct fuse_write_in {
    uint64_t fh, offset;
    uint32_t size, write_flags;
    uint64_t lock_owner;
    uint32_t flags, padding;
};
struct fuse_write_out {
    uint32_t size, padding;
};
struct fuse_setattr_in {
    uint32_t valid, padding;
    uint64_t fh, size, lock_owner, atime, mtime, ctime;
    uint32_t atimensec, mtimensec, ctimensec, mode, unused4, uid, gid, unused5;
};
#define FATTR_MODE_ (1 << 0)
#define FATTR_SIZE_ (1 << 3)
struct fuse_mkdir_in {
    uint32_t mode, umask;
};
struct fuse_rename_in {
    uint64_t newdir;
};
struct fuse_kstatfs {
    uint64_t blocks, bfree, bavail, files, ffree;
    uint32_t bsize, namelen, frsize, padding, spare[6];
};
struct fuse_dirent {
    uint64_t ino, off;
    uint32_t namelen, type;
};
#define DIRENT_ALIGN(x) (((x) + 7) & ~7ul)

// -- the in-daemon filesystem: a flat root of up to 8 entries --

#define MAXENT 8
#define MAXDATA 8192
static struct dent {
    int used, is_dir;
    char name[64];
    char data[MAXDATA];
    size_t size;
    unsigned mode;
} dents[MAXENT];

static uint64_t dent_nodeid(struct dent *d) {
    return (uint64_t) (d - dents) + 2; // root is 1
}
static struct dent *dent_by_nodeid(uint64_t nodeid) {
    if (nodeid < 2 || nodeid >= 2 + MAXENT || !dents[nodeid - 2].used)
        return NULL;
    return &dents[nodeid - 2];
}
static struct dent *dent_by_name(const char *name) {
    for (int i = 0; i < MAXENT; i++)
        if (dents[i].used && strcmp(dents[i].name, name) == 0)
            return &dents[i];
    return NULL;
}
static void fill_attr(struct fuse_wire_attr *attr, uint64_t nodeid) {
    memset(attr, 0, sizeof(*attr));
    attr->ino = nodeid;
    attr->blksize = 4096;
    if (nodeid == 1) {
        attr->mode = S_IFDIR | 0755;
        attr->nlink = 2;
        return;
    }
    struct dent *d = dent_by_nodeid(nodeid);
    attr->mode = (d->is_dir ? S_IFDIR : S_IFREG) | d->mode;
    attr->nlink = 1;
    attr->size = d->size;
}

static int devfd;

static void reply(uint64_t unique, int error, const void *body, size_t body_len) {
    char buf[sizeof(struct fuse_out_header) + 512];
    struct fuse_out_header *out = (struct fuse_out_header *) buf;
    out->len = (uint32_t) (sizeof(*out) + body_len);
    out->error = error;
    out->unique = unique;
    if (body_len > 0)
        memcpy(buf + sizeof(*out), body, body_len);
    if (write(devfd, buf, out->len) < 0)
        _exit(3);
}
static void reply_entry(uint64_t unique, uint64_t nodeid) {
    struct fuse_entry_out entry;
    memset(&entry, 0, sizeof(entry));
    entry.nodeid = nodeid;
    fill_attr(&entry.attr, nodeid);
    reply(unique, 0, &entry, sizeof(entry));
}

static void daemon_loop(void) {
    static char buf[160 * 1024]; // >= max_write + headers, per protocol
    for (;;) {
        ssize_t n = read(devfd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            _exit(errno == ENODEV ? 0 : 4); // ENODEV: unmounted, clean exit
        }
        if ((size_t) n < sizeof(struct fuse_in_header))
            _exit(5);
        struct fuse_in_header *in = (struct fuse_in_header *) buf;
        char *arg = buf + sizeof(*in);
        switch (in->opcode) {
        case FUSE_INIT: {
            struct fuse_init_in *init = (void *) arg;
            struct fuse_init_out out;
            memset(&out, 0, sizeof(out));
            out.major = 7;
            out.minor = init->minor < 31 ? init->minor : 31;
            out.max_readahead = init->max_readahead;
            out.max_write = 128 * 1024;
            reply(in->unique, 0, &out, sizeof(out));
            break;
        }
        case FUSE_LOOKUP: {
            struct dent *d = dent_by_name(arg);
            if (d == NULL)
                reply(in->unique, -ENOENT, NULL, 0);
            else
                reply_entry(in->unique, dent_nodeid(d));
            break;
        }
        case FUSE_GETATTR: {
            if (in->nodeid != 1 && dent_by_nodeid(in->nodeid) == NULL) {
                reply(in->unique, -ENOENT, NULL, 0);
                break;
            }
            struct fuse_attr_out out;
            memset(&out, 0, sizeof(out));
            fill_attr(&out.attr, in->nodeid);
            reply(in->unique, 0, &out, sizeof(out));
            break;
        }
        case FUSE_SETATTR: {
            struct fuse_setattr_in *sa = (void *) arg;
            struct dent *d = dent_by_nodeid(in->nodeid);
            if (d == NULL && in->nodeid != 1) {
                reply(in->unique, -ENOENT, NULL, 0);
                break;
            }
            if (d != NULL) {
                if (sa->valid & FATTR_MODE_)
                    d->mode = sa->mode & 0777;
                if (sa->valid & FATTR_SIZE_) {
                    if (sa->size > MAXDATA) {
                        reply(in->unique, -EFBIG, NULL, 0);
                        break;
                    }
                    if (sa->size > d->size)
                        memset(d->data + d->size, 0, sa->size - d->size);
                    d->size = sa->size;
                }
            }
            struct fuse_attr_out out;
            memset(&out, 0, sizeof(out));
            fill_attr(&out.attr, in->nodeid);
            reply(in->unique, 0, &out, sizeof(out));
            break;
        }
        case FUSE_CREATE: {
            struct fuse_create_in *cr = (void *) arg;
            const char *name = arg + sizeof(*cr);
            if (dent_by_name(name) != NULL) {
                reply(in->unique, -EEXIST, NULL, 0);
                break;
            }
            struct dent *d = NULL;
            for (int i = 0; i < MAXENT; i++)
                if (!dents[i].used) { d = &dents[i]; break; }
            if (d == NULL) {
                reply(in->unique, -ENOSPC, NULL, 0);
                break;
            }
            memset(d, 0, sizeof(*d));
            d->used = 1;
            snprintf(d->name, sizeof(d->name), "%s", name);
            d->mode = cr->mode & 0777;
            struct {
                struct fuse_entry_out entry;
                struct fuse_open_out open;
            } out;
            memset(&out, 0, sizeof(out));
            out.entry.nodeid = dent_nodeid(d);
            fill_attr(&out.entry.attr, out.entry.nodeid);
            out.open.fh = out.entry.nodeid;
            reply(in->unique, 0, &out, sizeof(out));
            break;
        }
        case FUSE_MKDIR: {
            struct fuse_mkdir_in *mk = (void *) arg;
            const char *name = arg + sizeof(*mk);
            if (dent_by_name(name) != NULL) {
                reply(in->unique, -EEXIST, NULL, 0);
                break;
            }
            struct dent *d = NULL;
            for (int i = 0; i < MAXENT; i++)
                if (!dents[i].used) { d = &dents[i]; break; }
            if (d == NULL) {
                reply(in->unique, -ENOSPC, NULL, 0);
                break;
            }
            memset(d, 0, sizeof(*d));
            d->used = 1;
            d->is_dir = 1;
            snprintf(d->name, sizeof(d->name), "%s", name);
            d->mode = mk->mode & 0777;
            reply_entry(in->unique, dent_nodeid(d));
            break;
        }
        case FUSE_UNLINK:
        case FUSE_RMDIR: {
            struct dent *d = dent_by_name(arg);
            if (d == NULL)
                reply(in->unique, -ENOENT, NULL, 0);
            else {
                d->used = 0;
                reply(in->unique, 0, NULL, 0);
            }
            break;
        }
        case FUSE_RENAME: {
            struct fuse_rename_in *rn = (void *) arg;
            (void) rn; // flat root: newdir is always 1
            const char *oldname = arg + sizeof(*rn);
            const char *newname = oldname + strlen(oldname) + 1;
            struct dent *d = dent_by_name(oldname);
            if (d == NULL) {
                reply(in->unique, -ENOENT, NULL, 0);
                break;
            }
            struct dent *t = dent_by_name(newname);
            if (t != NULL)
                t->used = 0;
            snprintf(d->name, sizeof(d->name), "%s", newname);
            reply(in->unique, 0, NULL, 0);
            break;
        }
        case FUSE_OPEN:
        case FUSE_OPENDIR: {
            struct fuse_open_out out;
            memset(&out, 0, sizeof(out));
            out.fh = in->nodeid;
            reply(in->unique, 0, &out, sizeof(out));
            break;
        }
        case FUSE_READ: {
            struct fuse_read_in *rd = (void *) arg;
            struct dent *d = dent_by_nodeid(in->nodeid);
            if (d == NULL) {
                reply(in->unique, -ENOENT, NULL, 0);
                break;
            }
            size_t off = rd->offset, size = rd->size;
            if (off >= d->size)
                size = 0;
            else if (off + size > d->size)
                size = d->size - off;
            reply(in->unique, 0, d->data + off, size);
            break;
        }
        case FUSE_WRITE: {
            struct fuse_write_in *wr = (void *) arg;
            struct dent *d = dent_by_nodeid(in->nodeid);
            if (d == NULL) {
                reply(in->unique, -ENOENT, NULL, 0);
                break;
            }
            if (wr->offset + wr->size > MAXDATA) {
                reply(in->unique, -EFBIG, NULL, 0);
                break;
            }
            memcpy(d->data + wr->offset, arg + sizeof(*wr), wr->size);
            if (wr->offset + wr->size > d->size)
                d->size = wr->offset + wr->size;
            struct fuse_write_out out = {.size = wr->size};
            reply(in->unique, 0, &out, sizeof(out));
            break;
        }
        case FUSE_READDIR: {
            struct fuse_read_in *rd = (void *) arg;
            if (rd->offset != 0) {
                reply(in->unique, 0, NULL, 0); // one-chunk streams: EOF
                break;
            }
            char stream[512];
            size_t pos = 0;
            uint64_t off = 0;
            const char *dots[2] = {".", ".."};
            for (int i = 0; i < 2; i++) {
                struct fuse_dirent de = {.ino = 1, .off = ++off,
                    .namelen = (uint32_t) strlen(dots[i]), .type = DT_DIR};
                memcpy(stream + pos, &de, sizeof(de));
                memcpy(stream + pos + sizeof(de), dots[i], de.namelen);
                pos = DIRENT_ALIGN(pos + sizeof(de) + de.namelen);
            }
            for (int i = 0; i < MAXENT; i++) {
                if (!dents[i].used)
                    continue;
                struct fuse_dirent de = {.ino = dent_nodeid(&dents[i]), .off = ++off,
                    .namelen = (uint32_t) strlen(dents[i].name),
                    .type = dents[i].is_dir ? DT_DIR : DT_REG};
                if (pos + sizeof(de) + de.namelen > sizeof(stream))
                    break;
                memcpy(stream + pos, &de, sizeof(de));
                memcpy(stream + pos + sizeof(de), dents[i].name, de.namelen);
                pos = DIRENT_ALIGN(pos + sizeof(de) + de.namelen);
            }
            reply(in->unique, 0, stream, pos);
            break;
        }
        case FUSE_STATFS: {
            struct fuse_kstatfs out;
            memset(&out, 0, sizeof(out));
            out.blocks = 1000;
            out.bfree = 500;
            out.bavail = 500;
            out.files = MAXENT;
            out.bsize = 4096;
            out.namelen = 255;
            out.frsize = 4096;
            reply(in->unique, 0, &out, sizeof(out));
            break;
        }
        case FUSE_FLUSH:
        case FUSE_RELEASE:
        case FUSE_RELEASEDIR:
        case FUSE_ACCESS:
            reply(in->unique, 0, NULL, 0);
            break;
        case FUSE_DESTROY:
            _exit(0); // no reply, session over
        case FUSE_FORGET:
        case FUSE_BATCH_FORGET:
            break; // no reply by protocol
        default:
            reply(in->unique, -ENOSYS, NULL, 0);
            break;
        }
    }
}

// -- the checks, run against the mount with ordinary syscalls --

static int is_ok(const char *label, long r) {
    if (r >= 0) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s: ret=%ld errno=%d (%s)\n", label, r, errno, strerror(errno));
    failures_total++;
    return 0;
}
static int eq_errno(const char *label, long r, int want) {
    int e = (r < 0) ? errno : 0;
    if (r < 0 && e == want) { test_logf("ok   %s (errno %d)\n", label, e); return 1; }
    printf("FAIL %s: ret=%ld errno=%d, wanted -1/errno=%d\n", label, r, e, want);
    failures_total++;
    return 0;
}
static int check(const char *label, int cond) {
    if (cond) { test_logf("ok   %s\n", label); return 1; }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60)); // a wedged conversation must not hang the suite

    devfd = open("/dev/fuse", O_RDWR);
    if (devfd < 0) {
        printf("fuse_basic: SKIP (no /dev/fuse: %s)\n", strerror(errno));
        return 0;
    }

    char base[64];
    snprintf(base, sizeof(base), "/tmp/fusebasic_XXXXXX");
    if (!mkdtemp(base)) { perror("mkdtemp"); return 2; }
    char mnt[96];
    snprintf(mnt, sizeof(mnt), "%s/mnt", base);
    mkdir(mnt, 0755);

    char opts[128];
    snprintf(opts, sizeof(opts), "fd=%d,rootmode=40000,user_id=0,group_id=0", devfd);
    if (mount("fuse_basic", mnt, "fuse", 0, opts) != 0) {
        if (errno == EPERM || errno == EACCES) {
            printf("fuse_basic: SKIP (mount not permitted: %s)\n", strerror(errno));
            return 0;
        }
        printf("FAIL mount: errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }
    test_logf("ok   mount\n");

    pid_t daemon_pid = fork();
    if (daemon_pid == 0) {
        daemon_loop();
        _exit(0);
    }

    char p1[128], p2[128], pd[128], missing[128];
    snprintf(p1, sizeof(p1), "%s/f1", mnt);
    snprintf(p2, sizeof(p2), "%s/f2", mnt);
    snprintf(pd, sizeof(pd), "%s/d", mnt);
    snprintf(missing, sizeof(missing), "%s/nope", mnt);

    // GETATTR on the root
    struct stat st;
    is_ok("stat.root", stat(mnt, &st));
    check("stat.root_is_dir", S_ISDIR(st.st_mode));

    // CREATE + WRITE + RELEASE
    int fd = open(p1, O_CREAT | O_WRONLY, 0644);
    is_ok("create", fd);
    is_ok("write", write(fd, "hello fuse", 10) == 10 ? 0 : -1);
    close(fd);

    // LOOKUP + GETATTR
    is_ok("stat.file", stat(p1, &st));
    check("stat.size", st.st_size == 10);
    check("stat.mode", (st.st_mode & 0777) == 0644);

    // OPEN + READ
    fd = open(p1, O_RDONLY);
    is_ok("open.read", fd);
    char buf[64] = "";
    is_ok("read", read(fd, buf, sizeof(buf)) == 10 ? 0 : -1);
    check("read.content", memcmp(buf, "hello fuse", 10) == 0);
    close(fd);

    // pwrite/pread at an offset
    fd = open(p1, O_RDWR);
    is_ok("pwrite", pwrite(fd, "FUSE", 4, 6) == 4 ? 0 : -1);
    is_ok("pread", pread(fd, buf, 4, 6) == 4 ? 0 : -1);
    check("pread.content", memcmp(buf, "FUSE", 4) == 0);
    close(fd);

    // SETATTR: chmod and truncate
    is_ok("chmod", chmod(p1, 0600));
    is_ok("stat.chmod", stat(p1, &st));
    check("chmod.applied", (st.st_mode & 0777) == 0600);
    is_ok("truncate", truncate(p1, 4));
    is_ok("stat.trunc", stat(p1, &st));
    check("truncate.applied", st.st_size == 4);

    // READDIR sees the file
    DIR *dir = opendir(mnt);
    int saw_f1 = 0;
    if (check("opendir", dir != NULL)) {
        struct dirent *de;
        while ((de = readdir(dir)) != NULL)
            if (strcmp(de->d_name, "f1") == 0)
                saw_f1 = 1;
        closedir(dir);
    }
    check("readdir.saw_file", saw_f1);

    // MKDIR + RMDIR
    is_ok("mkdir", mkdir(pd, 0755));
    is_ok("stat.dir", stat(pd, &st));
    check("mkdir.is_dir", S_ISDIR(st.st_mode));
    is_ok("rmdir", rmdir(pd));

    // RENAME, then the old name is a negative LOOKUP
    is_ok("rename", rename(p1, p2));
    eq_errno("rename.old_gone", stat(p1, &st), ENOENT);
    is_ok("rename.new_stat", stat(p2, &st));

    // STATFS comes from the daemon
    struct statfs sfs;
    is_ok("statfs", statfs(mnt, &sfs));
    check("statfs.blocks", sfs.f_blocks == 1000);

    // UNLINK, then gone
    is_ok("unlink", unlink(p2));
    eq_errno("unlink.gone", stat(p2, &st), ENOENT);

    // negative LOOKUP for a name that never existed
    eq_errno("lookup.negative", stat(missing, &st), ENOENT);

    // umount ends the session and the daemon exits cleanly
    is_ok("umount", umount(mnt));
    int wstatus = 0;
    is_ok("daemon.reaped", waitpid(daemon_pid, &wstatus, 0) == daemon_pid ? 0 : -1);
    check("daemon.clean_exit", WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);

    close(devfd);
    rmdir(mnt);
    rmdir(base);
    return finish_suite("fuse_basic");
}
