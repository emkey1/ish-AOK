// Snapshot a fakefs root: a point-in-time copy you can boot, go back to, and
// throw away.
//
// A root is two things that have to agree with each other -- `<root>/data`,
// the host files, and `<root>/meta.db`, the SQLite database holding the
// uid/gid/mode/device-node metadata the host filesystem cannot carry. Copying
// them at different instants produces a root where a file exists in one and not
// the other, so the whole job is: stop the world briefly, copy both, start it
// again.
//
// WHAT MAKES THIS CHEAP, and it is a host primitive this tree did not use at
// all before: APFS clones. clonefile(2) copies a directory tree
// copy-on-write, so the clone shares every block with the original until one of
// them is written. Source and destination must be on the same volume, which
// they are -- both are inside the container (or, on the CLI, both under the
// same build directory).
//
// MEASURED, 2026-09-08, and the number is not the one the plan assumed.
// Snapshot cost is O(directory entries), NOT O(bytes):
//
//     215,504 entries /  77 GiB   ->  4676, 4781, 5050 ms   (~22 us/entry)
//      13,916 entries / 606 MiB   ->   357,  383,  440 ms   (~28 us/entry)
//
// 127x the data in the first root, and it took 12x the time -- which is its
// entry-count ratio, not its byte ratio. Interleaved A/B rounds, so machine
// drift cannot land on one arm. The container grew by 63 MiB cloning 77 GiB,
// so the SPACE claim holds exactly as expected; it is the "under a second"
// claim that does not, and it fails on file count. A stock Alpine root is
// roughly a second; anything carrying node_modules or a Rust target directory
// is several. So this reports progress and takes a deadline: it is not an
// operation to run synchronously on a UI thread and pretend is instant.
//
// meta.db IS NOT CLONED, and that is deliberate. It is SQLite in WAL mode, so
// the bytes on disk are a database plus a write-ahead log plus a shared-memory
// index; a raw three-file copy of that is only valid if nothing is mid-write,
// and `-shm` must never be copied at all (SQLite rebuilds it, and a stale one
// misleads recovery). sqlite3_backup_* is the primitive that means "a
// consistent copy" by construction, so the database goes through that while the
// files go through clonefile.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/attr.h>
#include <sys/clonefile.h>
#endif

#include "debug.h"
#include "misc.h"
#include "fs/fake-db.h"
#include "fs/fake-snapshot.h"
#include "fs/sqlutil.h"
#include "kernel/errno.h"

static unsigned long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long) t.tv_sec * 1000UL + (unsigned long) (t.tv_nsec / 1000000L);
}

// ---------------------------------------------------------------------------
// Copying the data directory
// ---------------------------------------------------------------------------

#if !defined(__APPLE__)
// Portable fallback. Not copy-on-write, so it costs real time and real space --
// which is the honest behaviour to have on a host without clones rather than
// pretending the operation is cheap. Linux's reflink (FICLONE) covers btrfs and
// xfs and is tried first per file; everything else is a read/write copy.
#if defined(__linux__)
#include <sys/ioctl.h>
#ifndef FICLONE
#define FICLONE _IOW(0x94, 9, int)
#endif
#endif

static int copy_one_file(int src_dirfd, const char *name, int dst_dirfd, mode_t mode) {
    int in = openat(src_dirfd, name, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -errno;
    int out = openat(dst_dirfd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 07777);
    if (out < 0) {
        int err = -errno;
        close(in);
        return err;
    }
#if defined(__linux__)
    if (ioctl(out, FICLONE, in) == 0) {
        close(in);
        close(out);
        return 0;
    }
#endif
    char buf[64 * 1024];
    ssize_t n;
    int err = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t) (n - off));
            if (w < 0) {
                err = -errno;
                goto done;
            }
            off += w;
        }
    }
    if (n < 0)
        err = -errno;
done:
    close(in);
    close(out);
    return err;
}

static int copy_tree(const char *src, const char *dst, unsigned long *entries) {
    struct stat st;
    if (lstat(src, &st) < 0)
        return -errno;
    if (mkdir(dst, st.st_mode & 07777) < 0 && errno != EEXIST)
        return -errno;

    DIR *d = opendir(src);
    if (d == NULL)
        return -errno;
    int src_fd = dirfd(d);
    int dst_fd = open(dst, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dst_fd < 0) {
        int err = -errno;
        closedir(d);
        return err;
    }

    int err = 0;
    struct dirent *ent;
    while (err == 0 && (ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        (*entries)++;
        struct stat ent_st;
        if (fstatat(src_fd, ent->d_name, &ent_st, AT_SYMLINK_NOFOLLOW) < 0) {
            err = -errno;
            break;
        }
        if (S_ISDIR(ent_st.st_mode)) {
            char sub_src[PATH_MAX], sub_dst[PATH_MAX];
            if (snprintf(sub_src, sizeof sub_src, "%s/%s", src, ent->d_name) >= (int) sizeof sub_src ||
                    snprintf(sub_dst, sizeof sub_dst, "%s/%s", dst, ent->d_name) >= (int) sizeof sub_dst) {
                err = -ENAMETOOLONG;
                break;
            }
            err = copy_tree(sub_src, sub_dst, entries);
        } else if (S_ISLNK(ent_st.st_mode)) {
            char target[PATH_MAX];
            ssize_t n = readlinkat(src_fd, ent->d_name, target, sizeof target - 1);
            if (n < 0) {
                err = -errno;
                break;
            }
            target[n] = '\0';
            if (symlinkat(target, dst_fd, ent->d_name) < 0)
                err = -errno;
        } else if (S_ISREG(ent_st.st_mode)) {
            err = copy_one_file(src_fd, ent->d_name, dst_fd, ent_st.st_mode);
        }
        // Anything else (a stray socket or device node in the host tree) is
        // skipped: fakefs represents guest device nodes as ordinary files plus
        // a meta.db row, so a real one here is not ours to reproduce.
    }
    close(dst_fd);
    closedir(d);
    return err;
}
#endif // !__APPLE__

// Copy <src>/data to <dst>/data. Returns 0 or a negative errno.
static int snapshot_copy_data(const char *src_data, const char *dst_data,
                              unsigned long *entries) {
#if defined(__APPLE__)
    // CLONE_NOFOLLOW: the source is a directory we were handed, and a symlink
    // standing where the data dir should be is a mount that should never have
    // been accepted -- refuse rather than clone whatever it points at.
    if (clonefile(src_data, dst_data, CLONE_NOFOLLOW) < 0)
        return -errno;
    *entries = 0;   // clonefile does the walk internally; it does not count for us
    return 0;
#else
    return copy_tree(src_data, dst_data, entries);
#endif
}

// ---------------------------------------------------------------------------
// Copying the database
// ---------------------------------------------------------------------------

// A consistent copy of meta.db, through SQLite's own backup API.
//
// Opened READONLY and separately from the live connection pool: this must not
// disturb the statements the running guest has cached, and a reader on a WAL
// database sees a committed snapshot without blocking anyone. Under the quiesce
// gate there is no writer to race with, so the backup completes in a single
// step; the loop is here because sqlite3_backup_step is allowed to return BUSY
// on principle and a snapshot that gives up on the first contention would be a
// flake rather than a feature.
static int snapshot_copy_db(const char *src_db, const char *dst_db, char *err_out, size_t err_size) {
    sqlite3 *src = NULL, *dst = NULL;
    int err = sqlite3_open_v2(src_db, &src, SQLITE_OPEN_READONLY, NULL);
    if (err != SQLITE_OK) {
        snprintf(err_out, err_size, "open source: %s", sqlite3_errmsg(src));
        sqlite3_close(src);
        return _EIO;
    }
    err = sqlite3_open_v2(dst_db, &dst, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (err != SQLITE_OK) {
        snprintf(err_out, err_size, "open destination: %s", sqlite3_errmsg(dst));
        sqlite3_close(dst);
        sqlite3_close(src);
        return _EIO;
    }

    sqlite3_backup *backup = sqlite3_backup_init(dst, "main", src, "main");
    if (backup == NULL) {
        snprintf(err_out, err_size, "backup_init: %s", sqlite3_errmsg(dst));
        sqlite3_close(dst);
        sqlite3_close(src);
        return _EIO;
    }
    int rc;
    int busy_retries = 0;
    do {
        rc = sqlite3_backup_step(backup, -1);   // -1: all remaining pages
        if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            if (++busy_retries > 100)
                break;
            sqlite3_sleep(10);
        }
    } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);
    sqlite3_backup_finish(backup);

    int result = 0;
    if (rc != SQLITE_DONE) {
        snprintf(err_out, err_size, "backup_step: %s", sqlite3_errmsg(dst));
        result = _EIO;
    }
    sqlite3_close(dst);
    sqlite3_close(src);
    return result;
}

// ---------------------------------------------------------------------------

int fakefs_snapshot(const char *src_data, const char *dst_root,
                    struct fakefs_snapshot_stats *stats) {
    memset(stats, 0, sizeof *stats);
    unsigned long started = now_ms();

    // The database sits next to the data dir, found by swapping the trailing
    // "data" component -- the same convention fakefs_mount enforces, and the
    // same reason to treat a mismatch as a plain error rather than an assert.
    char src_db[PATH_MAX];
    if (snprintf(src_db, sizeof src_db, "%s", src_data) >= (int) sizeof src_db)
        return _ENAMETOOLONG;
    char *slash = strrchr(src_db, '/');
    if (slash == NULL || strcmp(slash + 1, "data") != 0)
        return _EINVAL;
    strcpy(slash + 1, "meta.db");

    char dst_data[PATH_MAX], dst_db[PATH_MAX];
    if (snprintf(dst_data, sizeof dst_data, "%s/data", dst_root) >= (int) sizeof dst_data ||
            snprintf(dst_db, sizeof dst_db, "%s/meta.db", dst_root) >= (int) sizeof dst_db)
        return _ENAMETOOLONG;

    // Refuse an occupied destination rather than merge into it. clonefile would
    // refuse anyway; saying so here means the error is the same on every host.
    struct stat st;
    if (lstat(dst_root, &st) == 0)
        return _EEXIST;
    if (mkdir(dst_root, 0755) < 0)
        return err_map(errno);

    // Stop the world. Same gate the iOS suspension handler uses, for very
    // nearly the same reason: reach a state where no fakefs transaction is open
    // so the files and the database describe the same instant.
    //
    // A straggler does NOT abort the snapshot, and that is a deliberate choice
    // rather than an oversight: a transaction can stay open across a blocking
    // host operation (opening a FIFO with no peer), so requiring zero would
    // make snapshots fail unpredictably on a busy guest. The count is reported
    // instead, so a caller that cares can refuse -- and the caller that should
    // care is a UI offering to restore, not a developer taking a copy.
    stats->quiesced = fakefs_quiesce_begin(FAKEFS_SNAPSHOT_QUIESCE_MS,
                                           &stats->quiesce_stragglers);

    unsigned long t0 = now_ms();
    int err = snapshot_copy_data(src_data, dst_data, &stats->entries);
    if (err < 0)
        err = err_map(-err);        // the copy helpers work in host errnos
    stats->clone_ms = now_ms() - t0;

    if (err == 0) {
        t0 = now_ms();
        err = snapshot_copy_db(src_db, dst_db, stats->error, sizeof stats->error);
        stats->db_ms = now_ms() - t0;
    }

    fakefs_quiesce_end();
    stats->total_ms = now_ms() - started;

    if (err != 0) {
        // A half-written snapshot is worse than none: it looks like a root and
        // boots into something that is not what was snapshotted. There is no
        // recursive remove in this tree to call, so leave a marker the caller
        // and the user can both see, and say so in the log.
        char marker[PATH_MAX];
        if (snprintf(marker, sizeof marker, "%s/INCOMPLETE", dst_root) < (int) sizeof marker) {
            int fd = open(marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd >= 0) {
                const char *msg = "This snapshot failed partway through and must not be booted.\n";
                if (write(fd, msg, strlen(msg)) < 0) { /* best effort */ }
                close(fd);
            }
        }
        printk("fakefs snapshot of %s failed: %d %s\n", src_data, err, stats->error);
    }
    return err;
}
