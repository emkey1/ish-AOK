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
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
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
#define FUSE_INTERRUPT 36
#define FUSE_LSEEK 46
#define FUSE_DESTROY 38
#define FUSE_BATCH_FORGET 42

struct fuse_in_header {
    uint32_t len, opcode;
    uint64_t unique, nodeid;
    uint32_t uid, gid, pid, padding;
};
struct fuse_interrupt_in { uint64_t unique; };
struct fuse_lseek_in { uint64_t fh, offset; uint32_t whence, padding; };
struct fuse_lseek_out { uint64_t offset; };
struct fuse_forget_in { uint64_t nlookup; };
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

#define MAXENT 16
#define MAXDATA 8192
#define BIGMAX (8 * 1024 * 1024)
static struct dent {
    int used, is_dir;
    char name[64];
    char data[MAXDATA];
    size_t size;
    unsigned mode;
    // An entry too big for `data` -- the executable the exec check runs.
    // Served instead of `data` when set; never written to.
    char *big;
    size_t bigsize;
    // Never answer a read of this entry until an INTERRUPT arrives for it.
    int defer_reads;
} dents[MAXENT];

// What the daemon saw, for the parent to check after the session ends. A file
// rather than shared memory because shared memory is one of the things under
// test here.
static FILE *evlog;
static void event(const char *fmt, ...) {
    if (evlog == NULL)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(evlog, fmt, ap);
    va_end(ap);
    fputc('\n', evlog);
    fflush(evlog);
}

// Slots are handed out from a high-water mark and never reused, so a nodeid
// identifies one file for the whole run. Recycling them would let a deleted
// file's surplus references mask a genuine over-forget of a later one, since
// the balance below is indexed by nodeid.
static int dents_used;
static struct dent *dent_alloc(void) {
    if (dents_used >= MAXENT)
        return NULL;
    return &dents[dents_used++];
}
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
// Everything this daemon serves is owned by whoever is running it. Reporting
// uid 0 instead -- which a memset does by default -- makes the whole test fail
// for any non-root caller, since creating inside a root-owned 0755 directory is
// EACCES by ordinary permission rules, here and on real Linux both. FUSE exists
// precisely so an unprivileged user can serve a filesystem, and a session with
// "Open Everything as Default User" on is not root, so that is the case worth
// covering rather than the one worth skipping.
static void fill_attr(struct fuse_wire_attr *attr, uint64_t nodeid) {
    memset(attr, 0, sizeof(*attr));
    attr->ino = nodeid;
    attr->blksize = 4096;
    attr->uid = (uint32_t) getuid();
    attr->gid = (uint32_t) getgid();
    if (nodeid == 1) {
        attr->mode = S_IFDIR | 0755;
        attr->nlink = 2;
        return;
    }
    struct dent *d = dent_by_nodeid(nodeid);
    attr->mode = (d->is_dir ? S_IFDIR : S_IFREG) | d->mode;
    attr->nlink = 1;
    attr->size = d->big != NULL ? d->bigsize : d->size;
}

static int devfd;

// One write per reply: the kernel reads exactly one message per read, so a
// header write followed by a body write would be seen as two malformed
// messages. The buffer therefore has to hold the largest body this daemon can
// produce -- a FUSE_READ of a whole entry -- and it was 512 bytes, which a
// read larger than that would have walked straight off the stack. Nothing in
// the test read that much until one of them served a whole executable.
#define REPLYMAX (160 * 1024)
static void reply(uint64_t unique, int error, const void *body, size_t body_len) {
    static char buf[sizeof(struct fuse_out_header) + REPLYMAX];
    if (body_len > REPLYMAX)
        _exit(6);
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
    // The one place a nodeid is handed to the kernel, whatever asked for it
    // -- LOOKUP, CREATE, MKDIR. Each one is a reference the kernel owes back.
    event("REF %llu", (unsigned long long) nodeid);
    struct fuse_entry_out entry;
    memset(&entry, 0, sizeof(entry));
    entry.nodeid = nodeid;
    fill_attr(&entry.attr, nodeid);
    reply(unique, 0, &entry, sizeof(entry));
}

static uint64_t deferred_unique;

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
                if (sa->valid & FATTR_MODE_) {
                    // The interrupt check's way of telling the daemon to stop
                    // answering reads of this entry. The daemon has its own
                    // copy of the table from before the fork, so the request
                    // has to travel over the wire like any other.
                    if (sa->mode & 01000)
                        d->defer_reads = 1;
                    d->mode = sa->mode & 0777;
                }
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
            struct dent *d = dent_alloc();
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
            // CREATE hands out a nodeid too, without going through
            // reply_entry -- it carries an open handle alongside the entry.
            event("REF %llu", (unsigned long long) out.entry.nodeid);
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
            if (d->defer_reads) {
                // Answer nothing. A real daemon doing slow work behaves this
                // way, and it is the only state in which an INTERRUPT means
                // anything -- the request has been read but not answered.
                event("SLOWREAD %llu", (unsigned long long) in->unique);
                deferred_unique = in->unique;
                break;
            }
            const char *src = d->big != NULL ? d->big : d->data;
            size_t have = d->big != NULL ? d->bigsize : d->size;
            size_t off = rd->offset, size = rd->size;
            if (off >= have)
                size = 0;
            else if (off + size > have)
                size = have - off;
            if (size > REPLYMAX)
                size = REPLYMAX;
            reply(in->unique, 0, src + off, size);
            break;
        }
        case FUSE_WRITE: {
            struct fuse_write_in *wr = (void *) arg;
            struct dent *d = dent_by_nodeid(in->nodeid);
            if (d == NULL) {
                reply(in->unique, -ENOENT, NULL, 0);
                break;
            }
            size_t need = (size_t) wr->offset + wr->size;
            if (need > BIGMAX) {
                reply(in->unique, -EFBIG, NULL, 0);
                break;
            }
            // Promote the entry to a heap buffer once it outgrows its inline
            // space: the exec check writes a whole executable through the
            // mount, which no inline entry could hold.
            if (d->big == NULL && need > MAXDATA) {
                d->big = calloc(1, BIGMAX);
                if (d->big == NULL) {
                    reply(in->unique, -ENOMEM, NULL, 0);
                    break;
                }
                memcpy(d->big, d->data, d->size);
                d->bigsize = d->size;
            }
            if (d->big != NULL) {
                memcpy(d->big + wr->offset, arg + sizeof(*wr), wr->size);
                if (need > d->bigsize)
                    d->bigsize = need;
            } else {
                memcpy(d->data + wr->offset, arg + sizeof(*wr), wr->size);
                if (need > d->size)
                    d->size = need;
            }
            event("WRITE %llu %llu %u", (unsigned long long) in->nodeid,
                  (unsigned long long) wr->offset, wr->size);
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
        case FUSE_FORGET: {
            struct fuse_forget_in *fg = (void *) arg;
            event("FORGET %llu %llu", (unsigned long long) in->nodeid,
                  (unsigned long long) fg->nlookup);
            break; // no reply by protocol
        }
        case FUSE_BATCH_FORGET:
            break; // no reply by protocol
        case FUSE_LSEEK: {
            // Implemented for one entry only, so the test can tell a daemon's
            // answer from the kernel's fallback -- and so the fallback is
            // still exercised by every other entry. "sparse" is a file whose
            // first 512 bytes are a hole.
            struct fuse_lseek_in *ls = (void *) arg;
            struct dent *d = dent_by_nodeid(in->nodeid);
            if (d == NULL || strcmp(d->name, "sparse") != 0) {
                reply(in->unique, -ENOSYS, NULL, 0);
                break;
            }
            if (ls->offset >= d->size) {
                reply(in->unique, -ENXIO, NULL, 0);
                break;
            }
            struct fuse_lseek_out out;
            if (ls->whence == SEEK_DATA)
                out.offset = ls->offset < 512 ? 512 : ls->offset;
            else
                out.offset = d->size;   // no hole before EOF
            reply(in->unique, 0, &out, sizeof(out));
            break;
        }
        case FUSE_INTERRUPT: {
            struct fuse_interrupt_in *iq = (void *) arg;
            event("INTERRUPT %llu", (unsigned long long) iq->unique);
            // What a real daemon does with one: stop the work and answer the
            // original request with EINTR. Doing that is what lets this run
            // on a real kernel -- Linux waits a read-but-unanswered request
            // out, so a daemon that never answered would wedge the caller
            // for good.
            if (deferred_unique != 0 && iq->unique == deferred_unique) {
                // Naming the right request is the entire content of the
                // feature: an implementation that sent FUSE_INTERRUPT with a
                // wrong or zero unique would still have "an interrupt
                // arrived" to show for it.
                event("INTMATCH %llu", (unsigned long long) iq->unique);
                reply(deferred_unique, -EINTR, NULL, 0);
                deferred_unique = 0;
            }
            break; // no reply of its own by protocol
        }
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

// Re-executed off the FUSE mount by the exec check below. Must come before
// anything else in main: the point is to prove the image was fetched from the
// daemon and run, and nothing else about this process matters.
#define EXEC_PROBE_ARG "--fuse-exec-probe"
#define EXEC_PROBE_STATUS 77

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], EXEC_PROBE_ARG) == 0)
        return EXEC_PROBE_STATUS;
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
    // Mount as the caller, matching the ownership fill_attr reports; hardcoding
    // 0 here works only when the test happens to run as root.
    snprintf(opts, sizeof(opts), "fd=%d,rootmode=40000,user_id=%u,group_id=%u",
             devfd, (unsigned) getuid(), (unsigned) getgid());
    if (mount("fuse_basic", mnt, "fuse", 0, opts) != 0) {
        if (errno == EPERM || errno == EACCES) {
            printf("fuse_basic: SKIP (mount not permitted: %s)\n", strerror(errno));
            return 0;
        }
        printf("FAIL mount: errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }
    test_logf("ok   mount\n");

    // The image the exec check runs, loaded before the fork so the daemon
    // inherits it. Skipped, not failed, where it cannot be read: the rest of
    // the checks do not depend on it.
    char *self_image = NULL;
    size_t self_len = 0;
    {
        int selffd = open("/proc/self/exe", O_RDONLY);
        if (selffd >= 0) {
            size_t cap = 4 * 1024 * 1024;
            self_image = malloc(cap);
            if (self_image != NULL) {
                ssize_t got = read(selffd, self_image, cap);
                // A short read is fine; a full one means the binary is bigger
                // than the cap and the copy would be truncated garbage.
                if (got > 0 && (size_t) got < cap)
                    self_len = (size_t) got;
                else
                    { free(self_image); self_image = NULL; }
            }
            close(selffd);
        }
    }

    char evpath[128];
    snprintf(evpath, sizeof(evpath), "%s/events", base);

    pid_t daemon_pid = fork();
    if (daemon_pid == 0) {
        evlog = fopen(evpath, "w");
        daemon_loop();
        _exit(0);
    }

    // The daemon reports each node's nodeid as its st_ino, so an ordinary
    // stat is enough to know which node the write log below is talking about.
    unsigned long long big_nodeid = 0;
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

    // ---- the daemon's own SEEK_DATA/SEEK_HOLE --------------------------
    // A daemon that knows where its holes are says so with FUSE_LSEEK, and
    // Linux asks -- measured on Devuan, which sends opcode 46 here. The
    // answer below is one the kernel's fallback could not produce: the
    // fallback says data starts at 0, and this daemon says 512. Done before
    // the checks on `mm`, whose ENOSYS latches the connection off asking
    // again -- exactly as Linux's fc->no_lseek does.
    {
        char psp[128];
        snprintf(psp, sizeof(psp), "%s/sparse", mnt);
        int sp = open(psp, O_CREAT | O_RDWR, 0644);
        if (is_ok("lseek.sparse_create", sp)) {
            char zeros[4096];
            memset(zeros, 0, sizeof(zeros));
            is_ok("lseek.sparse_fill",
                  write(sp, zeros, sizeof(zeros)) == (ssize_t) sizeof(zeros) ? 0 : -1);
            check("lseek.daemon_answers_data", lseek(sp, 0, SEEK_DATA) == 512);
            check("lseek.daemon_answers_hole", lseek(sp, 0, SEEK_HOLE) == 4096);
            eq_errno("lseek.daemon_answers_past_eof",
                     lseek(sp, 4096, SEEK_DATA) == -1 ? -1 : 0, ENXIO);
            close(sp);
        }
    }

    // ---- poll ----------------------------------------------------------
    // Linux has no ->poll for a regular file, so one goes through
    // DEFAULT_POLLMASK and is always both readable and writable. Measured on
    // Devuan (Linux 6.12) for a FUSE file: revents = POLLIN|POLLOUT. A
    // filesystem with no ->poll at all reads back as never-ready instead,
    // which is a silent hang for anything that polls before reading.
    char pmm[128];
    snprintf(pmm, sizeof(pmm), "%s/mm", mnt);
    fd = open(pmm, O_CREAT | O_RDWR, 0644);
    if (is_ok("mmap.create", fd)) {
        char pattern[4096];
        for (size_t i = 0; i < sizeof(pattern); i++)
            pattern[i] = (char) ('a' + (i % 26));
        is_ok("mmap.fill", write(fd, pattern, sizeof(pattern)) == (ssize_t) sizeof(pattern) ? 0 : -1);

        struct pollfd pf = {.fd = fd, .events = POLLIN | POLLOUT};
        int pr = poll(&pf, 1, 200);
        check("poll.returns_ready", pr == 1);
        check("poll.readable", (pf.revents & POLLIN) != 0);
        check("poll.writable", (pf.revents & POLLOUT) != 0);

        // ---- SEEK_DATA / SEEK_HOLE -------------------------------------
        // Measured on Devuan: a FUSE file with no FUSE_LSEEK support still
        // answers, because the kernel falls back to "the whole file is data"
        // -- DATA at 0 is 0, HOLE at 0 is the size, and either past the end
        // is ENXIO. EINVAL here would make anything probing for holes (cp,
        // tar, rsync) treat the file as unreadable.
        check("lseek.data_at_zero", lseek(fd, 0, SEEK_DATA) == 0);
        check("lseek.hole_is_eof", lseek(fd, 0, SEEK_HOLE) == (off_t) sizeof(pattern));
        eq_errno("lseek.data_past_eof", lseek(fd, sizeof(pattern), SEEK_DATA) == -1 ? -1 : 0, ENXIO);
        close(fd);
    }

    // ---- mmap ----------------------------------------------------------
    fd = open(pmm, O_RDONLY);
    if (is_ok("mmap.open_ro", fd)) {
        char expect[26];
        for (size_t i = 0; i < sizeof(expect); i++)
            expect[i] = (char) ('a' + i);
        void *m = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
        if (check("mmap.private", m != MAP_FAILED)) {
            check("mmap.private_contents", memcmp(m, expect, sizeof(expect)) == 0);
            munmap(m, 4096);
        }
        void *sh = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
        if (check("mmap.shared_ro", sh != MAP_FAILED)) {
            check("mmap.shared_ro_contents", memcmp(sh, expect, sizeof(expect)) == 0);
            munmap(sh, 4096);
        }
        // A writable shared mapping writes through to the file, so a
        // read-only fd cannot have one. Same rule as every other filesystem.
        void *bad = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        check("mmap.shared_write_needs_rdwr", bad == MAP_FAILED && errno == EACCES);
        if (bad != MAP_FAILED)
            munmap(bad, 4096);
        close(fd);
    }

    // A store through a writable shared mapping reaches the daemon. Measured
    // on Devuan: it does -- Linux writes the dirty pages back through
    // FUSE_WRITE. Refusing the mapping instead would break anything that
    // maps a file to write it, sqlite included.
    fd = open(pmm, O_RDWR);
    if (is_ok("mmap.open_rw", fd)) {
        void *w = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (check("mmap.shared_write", w != MAP_FAILED)) {
            memcpy(w, "STORED-THROUGH-MAPPING", 22);
            is_ok("mmap.msync", msync(w, 4096, MS_SYNC));
            // Read it back through a different descriptor, which goes to the
            // daemon rather than to the mapping.
            char back[32] = "";
            int rfd = open(pmm, O_RDONLY);
            if (is_ok("mmap.reopen", rfd)) {
                is_ok("mmap.readback", read(rfd, back, 22) == 22 ? 0 : -1);
                close(rfd);
            }
            check("mmap.store_reached_daemon", memcmp(back, "STORED-THROUGH-MAPPING", 22) == 0);

            // Two processes mapping one file share the pages, as they would
            // through Linux's page cache. The marker for this has to be one
            // the daemon has NOT been told about -- the first store was
            // msync'd and confirmed at the daemon above, so a child that
            // simply read it back through its own private copy would pass
            // while sharing nothing at all.
            memcpy((char *) w + 64, "UNSYNCED", 8);
            fflush(NULL);
            pid_t seer = fork();
            if (seer == 0) {
                int cfd = open(pmm, O_RDONLY);
                if (cfd < 0)
                    _exit(1);
                void *cm = mmap(NULL, 4096, PROT_READ, MAP_SHARED, cfd, 0);
                if (cm == MAP_FAILED)
                    _exit(2);
                int same = memcmp((char *) cm + 64, "UNSYNCED", 8) == 0;
                _exit(same ? 0 : 3);
            }
            int sst = 0;
            waitpid(seer, &sst, 0);
            check("mmap.shared_between_processes",
                  WIFEXITED(sst) && WEXITSTATUS(sst) == 0);
            munmap(w, 4096);
        }
        close(fd);
    }

    // A store into the middle of a multi-page mapping must write back that
    // page and leave every other one alone. A single-page file cannot tell a
    // correct run from one whose boundaries are off, and a writeback that
    // sends the wrong range corrupts data the guest never touched.
    {
        char pbig[128];
        snprintf(pbig, sizeof(pbig), "%s/big", mnt);
        size_t big_len = 16 * 1024;
        char *want = malloc(big_len);
        char *got = malloc(big_len);
        // The one store this makes, in the middle of the third page.
        size_t at = 2 * 4096 + 100;
        if (want != NULL && got != NULL) {
            for (size_t i = 0; i < big_len; i++)
                want[i] = (char) (i * 7 + (i >> 8));
            int bfd = open(pbig, O_CREAT | O_RDWR, 0644);
            struct stat bst;
            if (stat(pbig, &bst) == 0)
                big_nodeid = (unsigned long long) bst.st_ino;
            if (is_ok("mmap.multipage_create", bfd)) {
                size_t wrote = 0;
                while (wrote < big_len) {
                    ssize_t w = write(bfd, want + wrote, big_len - wrote);
                    if (w <= 0)
                        break;
                    wrote += (size_t) w;
                }
                check("mmap.multipage_fill", wrote == big_len);
                void *bm = mmap(NULL, big_len, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0);
                if (check("mmap.multipage_map", bm != MAP_FAILED)) {
                    check("mmap.multipage_contents", memcmp(bm, want, big_len) == 0);
                    memcpy((char *) bm + at, "MIDDLE", 6);
                    memcpy(want + at, "MIDDLE", 6);
                    is_ok("mmap.multipage_msync", msync(bm, big_len, MS_SYNC));
                    munmap(bm, big_len);
                }
                close(bfd);
                // Read the whole file back through the daemon.
                int rfd = open(pbig, O_RDONLY);
                if (is_ok("mmap.multipage_reopen", rfd)) {
                    size_t rd = 0;
                    while (rd < big_len) {
                        ssize_t r = read(rfd, got + rd, big_len - rd);
                        if (r <= 0)
                            break;
                        rd += (size_t) r;
                    }
                    close(rfd);
                    check("mmap.multipage_readback_len", rd == big_len);
                    // Both halves matter: the store landed, and nothing else
                    // moved.
                    check("mmap.multipage_store_landed",
                          memcmp(got + at, "MIDDLE", 6) == 0);
                    check("mmap.multipage_rest_untouched",
                          memcmp(got, want, big_len) == 0);
                }
            }
        }
        free(want);
        free(got);
    }

    // mmap of a directory is ENODEV, here and on Linux.
    fd = open(mnt, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        void *dm = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
        check("mmap.directory_enodev", dm == MAP_FAILED && errno == ENODEV);
        if (dm != MAP_FAILED)
            munmap(dm, 4096);
        close(fd);
    }

    // ---- executing a program stored on the mount -----------------------
    // This is what mmap unlocks: the ELF loader maps the image privately, so
    // without mmap a FUSE mount can hold programs but never run them.
    if (self_image != NULL && self_len > 0) {
        // The daemon has its own copy of the entry table from before the
        // fork, so the image cannot be pushed to it directly -- the file is
        // created and filled through the mount like any other.
        char pprog[128];
        snprintf(pprog, sizeof(pprog), "%s/prog", mnt);
        int pfd = open(pprog, O_CREAT | O_WRONLY, 0755);
        if (pfd >= 0) {
            // MAXDATA caps what this daemon stores, so write what fits and
            // let the check below skip if the image did not fit.
            size_t wrote = 0;
            while (wrote < self_len) {
                ssize_t w = write(pfd, self_image + wrote, self_len - wrote);
                if (w <= 0)
                    break;
                wrote += (size_t) w;
            }
            close(pfd);
            if (wrote == self_len) {
                fflush(NULL);
                pid_t ch = fork();
                if (ch == 0) {
                    execl(pprog, "prog", EXEC_PROBE_ARG, (char *) NULL);
                    _exit(120);
                }
                int est = 0;
                waitpid(ch, &est, 0);
                check("exec.from_fuse_mount",
                      WIFEXITED(est) && WEXITSTATUS(est) == EXEC_PROBE_STATUS);
            } else {
                printf("SKIP exec.from_fuse_mount (wrote %zu of %zu)\n",
                       wrote, self_len);
            }
        } else {
            printf("SKIP exec.from_fuse_mount (create failed: %s)\n", strerror(errno));
        }
        free(self_image);
        self_image = NULL;
    } else {
        // The one check proving mmap makes execution possible must never go
        // missing quietly -- printed, not test_logf'd, so it shows in a
        // default suite run.
        printf("SKIP exec.from_fuse_mount (no readable /proc/self/exe)\n");
    }

    // ---- INTERRUPT -----------------------------------------------------
    // A signal arriving while a request is outstanding. Linux queues a
    // FUSE_INTERRUPT naming that request -- but only once the daemon has
    // actually READ it; one still queued is simply dequeued instead, since
    // interrupting a request the daemon has never seen names a unique it
    // cannot match. The daemon here answers the interrupted request with
    // EINTR, which is what makes this portable: on a real kernel the caller
    // is waited out until the daemon answers.
    {
        char pslow[128];
        snprintf(pslow, sizeof(pslow), "%s/slow", mnt);
        int sfd = open(pslow, O_CREAT | O_RDWR, 0644);
        if (sfd >= 0) {
            is_ok("interrupt.fill", write(sfd, "0123456789", 10) == 10 ? 0 : -1);
            close(sfd);
            // Tell the daemon to stop answering reads of it. The daemon has
            // its own copy of the table, so this goes over the wire: a chmod
            // to a marker mode it watches for.
            is_ok("interrupt.arm", chmod(pslow, 01000 | 0644));
            fflush(NULL);
            pid_t victim = fork();
            if (victim == 0) {
                signal(SIGUSR1, SIG_DFL);
                int vfd = open(pslow, O_RDONLY);
                if (vfd < 0)
                    _exit(1);
                char vb[16];
                ssize_t vr = read(vfd, vb, sizeof(vb));
                _exit(vr < 0 ? 2 : 3);
            }
            // Give the read time to reach the daemon: an interrupt only means
            // something once the request has been read.
            usleep(400000);
            kill(victim, SIGUSR1);
            int vst = 0;
            waitpid(victim, &vst, 0);
            check("interrupt.caller_stopped_waiting",
                  WIFSIGNALED(vst) || (WIFEXITED(vst) && WEXITSTATUS(vst) == 2));
        }
    }

    // umount ends the session and the daemon exits cleanly
    is_ok("umount", umount(mnt));
    int wstatus = 0;
    is_ok("daemon.reaped", waitpid(daemon_pid, &wstatus, 0) == daemon_pid ? 0 : -1);
    check("daemon.clean_exit", WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);

    // ---- what the daemon saw -------------------------------------------
    // Every nodeid the kernel takes from a LOOKUP is a reference the daemon
    // is entitled to see returned. Counting them exactly is not portable --
    // a real kernel keeps nodes cached for as long as it likes -- but two
    // things are: the kernel must never forget a node more times than it
    // looked it up, and it must have sent the INTERRUPT above.
    FILE *ev = fopen(evpath, "r");
    if (check("events.log", ev != NULL)) {
        char line[256];
        int forgets = 0, interrupts = 0, over_forget = 0;
        long long balance[2 + MAXENT];
        memset(balance, 0, sizeof(balance));
        int int_matched = 0, big_write_bytes = 0;
        while (fgets(line, sizeof(line), ev) != NULL) {
            unsigned long long a = 0, b = 0;
            unsigned c = 0;
            if (sscanf(line, "INTMATCH %llu", &a) == 1) {
                int_matched++;
            } else if (sscanf(line, "WRITE %llu %llu %u", &a, &b, &c) == 3) {
                if (a == (unsigned long long) big_nodeid)
                    big_write_bytes += (int) c;
            } else if (sscanf(line, "REF %llu", &a) == 1) {
                if (a < 2 + MAXENT)
                    balance[a]++;
            } else if (sscanf(line, "FORGET %llu %llu", &a, &b) == 2) {
                forgets++;
                if (a < 2 + MAXENT) {
                    balance[a] -= (long long) b;
                    if (balance[a] < 0)
                        over_forget = 1;
                }
            } else if (sscanf(line, "INTERRUPT %llu", &a) == 1) {
                interrupts++;
            }
        }
        fclose(ev);
        test_logf("     daemon saw %d FORGET, %d INTERRUPT\n", forgets, interrupts);
        // A node forgotten more often than it was handed out is a daemon
        // use-after-free waiting to happen -- the failure mode that makes
        // reference accounting worth getting right at all.
        check("forget.never_over_forgets", !over_forget);
        // The over-forget check above is vacuously true when nothing is
        // forgotten at all, so on its own it passes on a kernel that never
        // sends FORGET -- which is what this one used to be. Both halves are
        // needed for it to lock anything down. Devuan sends 2 for this
        // session and AOK sends dozens; the assertion is only that the
        // references come back at all.
        check("forget.actually_sent", forgets >= 1);
        check("interrupt.reached_daemon", interrupts >= 1);
        check("interrupt.named_the_right_request", int_matched >= 1);
        // Writeback sends only the pages that changed. The 16K file was
        // filled once (16384 bytes) and then had six bytes stored into one
        // page through a mapping; an implementation that resent the whole
        // file on every sync would show at least twice the fill here.
        test_logf("     writes to the multipage node: %d bytes\n", big_write_bytes);
        check("mmap.writeback_sends_only_changed_pages",
              big_write_bytes > 0 && big_write_bytes <= 16384 + 8192);
    }
    unlink(evpath);

    close(devfd);
    rmdir(mnt);
    rmdir(base);
    return finish_suite("fuse_basic");
}
