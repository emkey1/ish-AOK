// fuse_threaded_daemon.c — the FUSE daemon is a THREAD of the process using
// the mount, rather than a separate process.
//
// That shape is ordinary: libfuse's multi-threaded loop produces it, and so
// does any program that mounts a filesystem for its own use rather than for
// somebody else's. It is also the shape that finds every place AOK does
// filesystem work while holding a lock the daemon needs, because a thread
// shares its process's fd table and address space with the caller. A separate
// daemon process shares neither, so none of these bugs are visible there --
// tests/manual/fuse_basic.c forks its daemon and passed throughout.
//
// Three real hangs were found this way and are locked in here:
//
//   close(2) held the fd table's lock across the filesystem's ->close. For a
//   FUSE file that ->close sends FUSE_FLUSH and waits for the daemon, and the
//   daemon's next read of /dev/fuse needs that same lock to look up its own
//   descriptor. The daemon answered the READ, never received the FLUSH, and
//   both threads stopped for good.
//
//   mmap fetches a FUSE file's contents, and fd_ops.mmap runs under the
//   address-space write lock with the process's other threads quiesced --
//   including the daemon. The fetch moved to fd_ops.mmap_prepare, which runs
//   before that lock is taken, and msync defers its writeback until after it
//   is released.
//
//   Unmapping drops the mapping's reference to the descriptor, and when the
//   guest closed its own first that is the LAST one -- so the filesystem's
//   ->close ran from inside the page-table teardown, under that same lock.
//   pt_unmap parks those closes now and the unlock runs them. Only the
//   close-before-unmap case below reaches it: unmap first, as every other
//   test in the tree does, and the descriptor is still open.
//
// Speaks the raw protocol so the suite needs no packages, and passes
// identically on a real Linux kernel run as root, where mmap does no I/O and
// close takes no such lock.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "test_common.h"

#define FUSE_LOOKUP 1
#define FUSE_FORGET 2
#define FUSE_GETATTR 3
#define FUSE_SETATTR 4
#define FUSE_OPEN 14
#define FUSE_READ 15
#define FUSE_WRITE 16
#define FUSE_STATFS 17
#define FUSE_RELEASE 18
#define FUSE_FSYNC 20
#define FUSE_FLUSH 25
#define FUSE_INIT 26
#define FUSE_OPENDIR 27
#define FUSE_READDIR 28
#define FUSE_RELEASEDIR 29
#define FUSE_CREATE 35
#define FUSE_DESTROY 38
#define FUSE_BATCH_FORGET 42

struct in_hdr {
    uint32_t len, opcode;
    uint64_t unique, nodeid;
    uint32_t uid, gid, pid, pad;
};
struct out_hdr { uint32_t len; int32_t error; uint64_t unique; };
struct wattr {
    uint64_t ino, size, blocks, atime, mtime, ctime;
    uint32_t atimensec, mtimensec, ctimensec, mode, nlink, uid, gid, rdev, blksize, flags;
};
struct entry_out {
    uint64_t nodeid, generation, entry_valid, attr_valid;
    uint32_t entry_valid_nsec, attr_valid_nsec;
    struct wattr attr;
};
struct attr_out { uint64_t attr_valid; uint32_t attr_valid_nsec, dummy; struct wattr attr; };
struct init_in { uint32_t major, minor, max_readahead, flags; };
struct init_out {
    uint32_t major, minor, max_readahead, flags;
    uint16_t max_background, congestion_threshold;
    uint32_t max_write, time_gran;
    uint16_t max_pages, map_alignment;
    uint32_t unused[8];
};
struct open_out { uint64_t fh; uint32_t open_flags, padding; };
struct read_in { uint64_t fh, offset; uint32_t size, read_flags; uint64_t lock_owner; uint32_t flags, pad; };
struct write_in { uint64_t fh, offset; uint32_t size, write_flags; uint64_t lock_owner; uint32_t flags, pad; };
struct write_out { uint32_t size, padding; };

#define FILESZ 16384
#define NODE_F 2

static char filedata[FILESZ];
static size_t filesize;
static int devfd = -1;

// The daemon runs on its own thread and touches only these; the main thread
// reads filedata once, after the session has ended.
static void reply(uint64_t unique, int error, const void *body, size_t len) {
    static char buf[192 * 1024];
    struct out_hdr *o = (struct out_hdr *) buf;
    o->len = (uint32_t) (sizeof(*o) + len);
    o->error = error;
    o->unique = unique;
    if (len > 0)
        memcpy(buf + sizeof(*o), body, len);
    ssize_t w = write(devfd, buf, o->len);
    (void) w;
}

static void fill_attr(struct wattr *a, uint64_t nodeid) {
    memset(a, 0, sizeof(*a));
    a->ino = nodeid;
    a->blksize = 4096;
    a->nlink = 1;
    // Owned by whoever is running the test: a root-owned tree refuses a
    // non-root caller's create, here and on real Linux both.
    a->uid = (uint32_t) getuid();
    a->gid = (uint32_t) getgid();
    if (nodeid == 1) {
        a->mode = S_IFDIR | 0755;
        a->nlink = 2;
    } else {
        a->mode = S_IFREG | 0644;
        a->size = filesize;
    }
}

static void *daemon_thread(void *arg) {
    (void) arg;
    static char buf[192 * 1024];
    for (;;) {
        ssize_t n = read(devfd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return NULL;    // ENODEV: unmounted, which is how the session ends
        }
        if (n == 0)
            return NULL;
        struct in_hdr *in = (struct in_hdr *) buf;
        char *a = buf + sizeof(*in);
        switch (in->opcode) {
        case FUSE_INIT: {
            struct init_in *i = (void *) a;
            struct init_out o;
            memset(&o, 0, sizeof(o));
            o.major = 7;
            o.minor = i->minor < 31 ? i->minor : 31;
            o.max_readahead = i->max_readahead;
            o.max_write = 128 * 1024;
            reply(in->unique, 0, &o, sizeof(o));
            break;
        }
        case FUSE_LOOKUP:
            if (strcmp(a, "f") == 0) {
                struct entry_out e;
                memset(&e, 0, sizeof(e));
                e.nodeid = NODE_F;
                fill_attr(&e.attr, NODE_F);
                reply(in->unique, 0, &e, sizeof(e));
            } else {
                reply(in->unique, -ENOENT, NULL, 0);
            }
            break;
        case FUSE_CREATE: {
            struct { struct entry_out e; struct open_out o; } out;
            memset(&out, 0, sizeof(out));
            out.e.nodeid = NODE_F;
            fill_attr(&out.e.attr, NODE_F);
            out.o.fh = NODE_F;
            reply(in->unique, 0, &out, sizeof(out));
            break;
        }
        case FUSE_GETATTR:
        case FUSE_SETATTR: {
            struct attr_out o;
            memset(&o, 0, sizeof(o));
            fill_attr(&o.attr, in->nodeid);
            reply(in->unique, 0, &o, sizeof(o));
            break;
        }
        case FUSE_OPEN:
        case FUSE_OPENDIR: {
            struct open_out o;
            memset(&o, 0, sizeof(o));
            o.fh = in->nodeid;
            reply(in->unique, 0, &o, sizeof(o));
            break;
        }
        case FUSE_READ: {
            struct read_in *r = (void *) a;
            size_t off = r->offset, want = r->size;
            if (off >= filesize)
                want = 0;
            else if (off + want > filesize)
                want = filesize - off;
            reply(in->unique, 0, filedata + off, want);
            break;
        }
        case FUSE_WRITE: {
            struct write_in *w = (void *) a;
            size_t need = (size_t) w->offset + w->size;
            if (need > FILESZ) {
                reply(in->unique, -EFBIG, NULL, 0);
                break;
            }
            memcpy(filedata + w->offset, a + sizeof(*w), w->size);
            if (need > filesize)
                filesize = need;
            struct write_out o = {.size = w->size};
            reply(in->unique, 0, &o, sizeof(o));
            break;
        }
        case FUSE_STATFS: {
            char zero[80];
            memset(zero, 0, sizeof(zero));
            reply(in->unique, 0, zero, sizeof(zero));
            break;
        }
        case FUSE_READDIR:
            reply(in->unique, 0, NULL, 0);
            break;
        case FUSE_FLUSH:
        case FUSE_RELEASE:
        case FUSE_RELEASEDIR:
        case FUSE_FSYNC:
            reply(in->unique, 0, NULL, 0);
            break;
        case FUSE_FORGET:
        case FUSE_BATCH_FORGET:
            break;              // no reply by protocol
        case FUSE_DESTROY:
            return NULL;
        default:
            reply(in->unique, -ENOSYS, NULL, 0);
            break;
        }
    }
}

static int check(const char *label, int cond) {
    if (cond) {
        test_logf("ok   %s\n", label);
        return 1;
    }
    printf("FAIL %s\n", label);
    failures_total++;
    return 0;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    // Every failure this test exists to catch is a HANG, so the watchdog is
    // the failure signal rather than a safety net. Keep it short enough that
    // a regression costs the suite a minute, not an hour.
    alarm(test_watchdog_secs(90));

    devfd = open("/dev/fuse", O_RDWR);
    if (devfd < 0) {
        printf("fuse_threaded_daemon: SKIP (no /dev/fuse: %s)\n", strerror(errno));
        return 0;
    }

    char base[64];
    snprintf(base, sizeof(base), "/tmp/fusethr_XXXXXX");
    if (mkdtemp(base) == NULL) {
        perror("mkdtemp");
        return 2;
    }
    char mnt[96];
    snprintf(mnt, sizeof(mnt), "%s/mnt", base);
    mkdir(mnt, 0755);

    char opts[160];
    snprintf(opts, sizeof(opts), "fd=%d,rootmode=40755,user_id=%u,group_id=%u",
             devfd, (unsigned) getuid(), (unsigned) getgid());
    if (mount("fuse_threaded", mnt, "fuse", 0, opts) != 0) {
        if (errno == EPERM || errno == EACCES) {
            printf("fuse_threaded_daemon: SKIP (mount not permitted: %s)\n", strerror(errno));
            return 0;
        }
        printf("FAIL mount: errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }
    test_logf("ok   mount\n");

    // From here on the daemon is a sibling thread of this one, sharing this
    // process's fd table and address space.
    pthread_t th;
    if (!check("start the daemon on a thread of this process",
               pthread_create(&th, NULL, daemon_thread, NULL) == 0)) {
        umount(mnt);
        return finish_suite("fuse_threaded_daemon");
    }

    char path[160];
    snprintf(path, sizeof(path), "%s/f", mnt);

    char pattern[FILESZ];
    for (size_t i = 0; i < sizeof(pattern); i++)
        pattern[i] = (char) ('A' + (i % 26));

    int fd = open(path, O_CREAT | O_RDWR, 0644);
    check("open through the mount", fd >= 0);
    if (fd >= 0) {
        size_t wrote = 0;
        while (wrote < sizeof(pattern)) {
            ssize_t w = write(fd, pattern + wrote, sizeof(pattern) - wrote);
            if (w <= 0)
                break;
            wrote += (size_t) w;
        }
        check("write through the mount", wrote == sizeof(pattern));

        // A second descriptor, closed while the first is still open. This is
        // the close that used to hang: it sends FUSE_FLUSH and waits, and the
        // daemon thread could not reach the fd table to receive it.
        int second = open(path, O_RDONLY);
        check("a second descriptor opens", second >= 0);
        if (second >= 0) {
            char one[8] = "";
            check("it reads", pread(second, one, sizeof(one), 0) == (ssize_t) sizeof(one));
            check("closing it does not hang", close(second) == 0);
        }

        // mmap, with the daemon among the threads the mapping quiesces.
        void *m = mmap(NULL, FILESZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        check("mmap does not hang", m != MAP_FAILED);
        if (m != MAP_FAILED) {
            check("the mapping holds the file's contents",
                  memcmp(m, pattern, sizeof(pattern)) == 0);
            memcpy((char *) m + 8192, "THREADED", 8);
            check("msync does not hang", msync(m, FILESZ, MS_SYNC) == 0);
            check("munmap does not hang", munmap(m, FILESZ) == 0);

            char back[8] = "";
            int again = open(path, O_RDONLY);
            if (check("reopen after the mapping", again >= 0)) {
                check("the store reached the daemon",
                      pread(again, back, sizeof(back), 8192) == (ssize_t) sizeof(back) &&
                      memcmp(back, "THREADED", 8) == 0);
                check("closing that one does not hang either", close(again) == 0);
            }
        }
        check("closing the mapped descriptor does not hang", close(fd) == 0);
    }

    // The other order, which is the one the deferred-close path exists for:
    // map it, close the descriptor while the mapping still holds a reference,
    // store, then unmap. The unmap drops the LAST reference, so the
    // filesystem's ->close runs from inside the page-table teardown -- under
    // the address-space write lock, with the daemon thread quiesced. Every
    // check above unmaps before closing and never reaches it.
    {
        int mfd = open(path, O_RDWR);
        if (check("reopen for the close-then-unmap order", mfd >= 0)) {
            void *m2 = mmap(NULL, FILESZ, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
            if (check("map it again", m2 != MAP_FAILED)) {
                check("close while the mapping holds the last reference",
                      close(mfd) == 0);
                memcpy((char *) m2 + 12288, "LASTREF!", 8);
                check("unmapping the last reference does not hang",
                      munmap(m2, FILESZ) == 0);

                char back[8] = "";
                int r = open(path, O_RDONLY);
                if (check("reopen after that unmap", r >= 0)) {
                    check("the store still reached the daemon",
                          pread(r, back, sizeof(back), 12288) == (ssize_t) sizeof(back) &&
                          memcmp(back, "LASTREF!", 8) == 0);
                    close(r);
                }
            } else {
                close(mfd);
            }
        }
    }

    // A process that stores through a shared mapping and then just exits --
    // no msync, no munmap, no close. Measured on Devuan: those stores still
    // reach the daemon, so the exit path waits for them. AOK bounds that wait
    // rather than skipping it, because skipping loses data Linux keeps, and
    // waiting without a bound hangs when the daemon cannot answer.
    {
        fflush(NULL);
        pid_t c = fork();
        if (c == 0) {
            alarm(30);
            int efd = open(path, O_RDWR);
            if (efd < 0)
                _exit(90);
            void *em = mmap(NULL, FILESZ, PROT_READ | PROT_WRITE, MAP_SHARED, efd, 0);
            if (em == MAP_FAILED)
                _exit(91);
            memcpy((char *) em + 4096, "ATEXIT!!", 8);
            _exit(0);
        }
        int est = 0;
        waitpid(c, &est, 0);
        check("a process storing through a mapping can exit",
              WIFEXITED(est) && WEXITSTATUS(est) == 0);
        char back[8] = "";
        int r = open(path, O_RDONLY);
        if (check("reopen after that exit", r >= 0)) {
            check("the store survived the exit",
                  pread(r, back, sizeof(back), 4096) == (ssize_t) sizeof(back) &&
                  memcmp(back, "ATEXIT!!", 8) == 0);
            close(r);
        }
    }

    check("umount", umount(mnt) == 0);
    pthread_join(th, NULL);
    close(devfd);
    rmdir(mnt);
    rmdir(base);
    return finish_suite("fuse_threaded_daemon");
}
