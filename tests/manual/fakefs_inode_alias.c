// Regression test for fakefs inode aliasing, and for the unremovable entry it
// used to leave behind.
//
// fakefs numbers its own inodes from a per-mount counter (fs/fake-db.c's
// fs->next_inode) that is seeded once, from max(stats.inode), and then kept in
// memory. Any second allocator on the same meta.db -- another ish process on
// the same root, or that root mounted twice -- seeds from the same watermark
// and proceeds to hand out the very numbers the first one is still working
// through. path_create's `insert into stats` then failed with a primary-key
// constraint violation, which fs/fake-db.c's db_check_error only printk()s, so
// the failure went unnoticed and the `insert or replace into paths` right below
// it ran anyway. The new path was left pointing at a pre-existing inode --
// sharing another file's stat blob.
//
// Two paths sharing a blob is invisible until they disagree about being a
// directory, and then the entry cannot be removed by anything at all:
//
//     stat  /tmp/ccKNBeAg.o  ->  directory, mode 0755
//     rmdir /tmp/ccKNBeAg.o  ->  ENOTDIR   (the host entry is a regular file)
//     rm -f /tmp/ccKNBeAg.o  ->  EISDIR    (the metadata says directory)
//     echo hi > /tmp/ccKNBeAg.o -> EISDIR
//
// rmdir is refused because the host side is not a directory, unlink and create
// because the metadata says it is; the name is poisoned for the life of the
// root. That is not a curiosity: gcc picks temporary names deterministically
// enough to repeat them, so a real i386 root reached
//
//     Cannot create temporary file in /tmp/: Is a directory
//
// and could not build anything until TMPDIR was pointed elsewhere. It was found
// on /tmp/ccKNBeAg.o aliased onto the directory /rbind, both holding inode
// 153414, with contiguous runs of unrelated temporaries (/tmp/fsconf_dGcknk vs
// /tmp/scmpd.2012.sock, ...) sharing inodes in lockstep further down the table
// -- two allocators marching from one watermark.
//
// What this checks, in the order the bug happens:
//
//   1. rmdir/unlink still answer exactly as Linux does for every entry type.
//      fs/fake.c's fakefs_rmdir now clears a metadata-says-directory entry
//      whose host side is not one, so this pins down that it fires ONLY there.
//      fakefs stores symlinks, sockets and device nodes as ordinary host files
//      with the real type in the metadata alone, so a recovery rule that keyed
//      on the host type in general would delete every symlink it was pointed
//      at.
//   2. Freshly created files and directories get distinct inodes.
//   3. No entry anywhere on the root is in the wedged state. This is the
//      detector that would have caught the original damage, and it is what
//      confirms the v7 repair pass in fs/fake-migrate.c actually cleaned a
//      root rather than just stopping the bleeding. Two signals, because the
//      obvious one does not work: opening a wedged entry with O_DIRECTORY
//      SUCCEEDS (fs/real.c stopped asserting on the type mismatch in 15ff5d8e
//      and just yields an empty listing), so "can it be opened as a directory"
//      answers yes for both.
//
//        a. st_nlink < 2 on a directory. fakefs overrides only mode/uid/gid/
//           rdev/inode and passes the host's link count straight through, so a
//           real directory carries the host's >= 2 ("." plus its entry in the
//           parent) while an entry that is only a directory in the metadata
//           carries the backing file's 1. That leak of the host type is the
//           whole trick. Skipped on hosts that do not count subdirectories
//           (btrfs reports 1 for every directory), detected by checking "/".
//        b. Two paths sharing one inode where either is a directory.
//           Directories cannot be hard-linked, so that pair cannot arise
//           honestly -- it is the aliasing itself, seen from the guest. Plain
//           hard links (the binutils multi-call tools) share an inode and a
//           type and are left alone.
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_common.h"

static unsigned failures;

static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    failures++;
}

// ---------------------------------------------------------------- semantics

// rmdir() and unlink() must keep answering the way Linux does for every entry
// type fakefs can store. fakefs_rmdir's recovery path keys on the metadata's
// directory bit precisely so none of these change.
static void check_removal_semantics(const char *dir) {
    char path[PATH_MAX];
    struct stat st;

#define PATH_IN(name) (snprintf(path, sizeof(path), "%s/%s", dir, (name)), path)

    // A regular file: rmdir must refuse with ENOTDIR and leave it alone.
    int fd = open(PATH_IN("plain"), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail("create %s: %s", path, strerror(errno));
        return;
    }
    close(fd);
    if (rmdir(PATH_IN("plain")) == 0 || errno != ENOTDIR)
        fail("rmdir(regular file) = %s, want ENOTDIR", strerror(errno));
    if (lstat(PATH_IN("plain"), &st) != 0)
        fail("rmdir(regular file) destroyed it: %s", strerror(errno));

    // A directory: unlink must refuse with EISDIR and leave it alone.
    if (mkdir(PATH_IN("realdir"), 0755) != 0)
        fail("mkdir %s: %s", path, strerror(errno));
    if (unlink(PATH_IN("realdir")) == 0 || errno != EISDIR)
        fail("unlink(directory) = %s, want EISDIR", strerror(errno));
    if (lstat(PATH_IN("realdir"), &st) != 0 || !S_ISDIR(st.st_mode))
        fail("unlink(directory) destroyed it");

    // A symlink to a directory. fakefs keeps this as a host *regular file*
    // holding the target, so the host side is "not a directory" exactly like a
    // wedged entry -- the metadata is the only thing telling them apart, and
    // the recovery path must not touch this one.
    if (symlink("realdir", PATH_IN("dirlink")) != 0)
        fail("symlink %s: %s", path, strerror(errno));
    if (rmdir(PATH_IN("dirlink")) == 0 || errno != ENOTDIR)
        fail("rmdir(symlink to directory) = %s, want ENOTDIR", strerror(errno));
    if (lstat(PATH_IN("dirlink"), &st) != 0 || !S_ISLNK(st.st_mode))
        fail("rmdir(symlink to directory) destroyed the link");
    if (lstat(PATH_IN("realdir"), &st) != 0 || !S_ISDIR(st.st_mode))
        fail("rmdir(symlink to directory) destroyed the target");

    // A symlink to a file, and a dangling one: same story.
    if (symlink("plain", PATH_IN("filelink")) != 0)
        fail("symlink %s: %s", path, strerror(errno));
    if (rmdir(PATH_IN("filelink")) == 0 || errno != ENOTDIR)
        fail("rmdir(symlink to file) = %s, want ENOTDIR", strerror(errno));
    if (lstat(PATH_IN("filelink"), &st) != 0 || !S_ISLNK(st.st_mode))
        fail("rmdir(symlink to file) destroyed the link");
    if (symlink("nowhere-at-all", PATH_IN("deadlink")) != 0)
        fail("symlink %s: %s", path, strerror(errno));
    if (rmdir(PATH_IN("deadlink")) == 0 || errno != ENOTDIR)
        fail("rmdir(dangling symlink) = %s, want ENOTDIR", strerror(errno));
    if (lstat(PATH_IN("deadlink"), &st) != 0)
        fail("rmdir(dangling symlink) destroyed the link");

    // A fifo: also a metadata-only type over a host file.
    if (mkfifo(PATH_IN("pipe"), 0644) == 0) {
        if (rmdir(PATH_IN("pipe")) == 0 || errno != ENOTDIR)
            fail("rmdir(fifo) = %s, want ENOTDIR", strerror(errno));
        if (lstat(PATH_IN("pipe"), &st) != 0)
            fail("rmdir(fifo) destroyed it");
        unlink(PATH_IN("pipe"));
    }

    // A non-empty directory, and a name that is not there at all.
    if (mkdir(PATH_IN("full"), 0755) == 0) {
        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/full/child", dir);
        int cfd = open(child, O_WRONLY | O_CREAT, 0644);
        if (cfd >= 0)
            close(cfd);
        if (rmdir(PATH_IN("full")) == 0 || errno != ENOTEMPTY)
            fail("rmdir(non-empty directory) = %s, want ENOTEMPTY", strerror(errno));
        unlink(child);
        if (rmdir(PATH_IN("full")) != 0)
            fail("rmdir(emptied directory): %s", strerror(errno));
    }
    if (rmdir(PATH_IN("no-such-name")) == 0 || errno != ENOENT)
        fail("rmdir(nonexistent) = %s, want ENOENT", strerror(errno));

    // And the ordinary paths still work.
    if (unlink(PATH_IN("dirlink")) != 0)
        fail("unlink(symlink): %s", strerror(errno));
    if (unlink(PATH_IN("filelink")) != 0)
        fail("unlink(symlink): %s", strerror(errno));
    if (unlink(PATH_IN("deadlink")) != 0)
        fail("unlink(dangling symlink): %s", strerror(errno));
    if (unlink(PATH_IN("plain")) != 0)
        fail("unlink(regular file): %s", strerror(errno));
    if (rmdir(PATH_IN("realdir")) != 0)
        fail("rmdir(empty directory): %s", strerror(errno));

#undef PATH_IN
}

// ------------------------------------------------------------- fresh inodes

// Every newly created name must get an inode of its own. A second allocator
// racing this one used to hand out numbers already in use, which is how a path
// ends up wearing another file's stat blob.
#define FRESH_COUNT 64

static void check_fresh_inodes(const char *dir) {
    ino_t seen[FRESH_COUNT * 2];
    unsigned count = 0;
    char path[PATH_MAX];

    for (int i = 0; i < FRESH_COUNT; i++) {
        struct stat st;
        snprintf(path, sizeof(path), "%s/f%d", dir, i);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            fail("create %s: %s", path, strerror(errno));
            continue;
        }
        close(fd);
        if (lstat(path, &st) != 0) {
            fail("lstat %s: %s", path, strerror(errno));
            continue;
        }
        if (!S_ISREG(st.st_mode))
            fail("%s was created as a regular file but reports mode %#o", path, (unsigned) st.st_mode);
        seen[count++] = st.st_ino;

        snprintf(path, sizeof(path), "%s/d%d", dir, i);
        if (mkdir(path, 0755) != 0) {
            fail("mkdir %s: %s", path, strerror(errno));
            continue;
        }
        if (lstat(path, &st) != 0) {
            fail("lstat %s: %s", path, strerror(errno));
            continue;
        }
        if (!S_ISDIR(st.st_mode))
            fail("%s was created as a directory but reports mode %#o", path, (unsigned) st.st_mode);
        seen[count++] = st.st_ino;
    }

    for (unsigned i = 0; i < count; i++)
        for (unsigned j = i + 1; j < count; j++)
            if (seen[i] == seen[j]) {
                fail("two freshly created entries share inode %llu", (unsigned long long) seen[i]);
                i = count; // one report is enough
                break;
            }

    for (int i = 0; i < FRESH_COUNT; i++) {
        snprintf(path, sizeof(path), "%s/f%d", dir, i);
        unlink(path);
        snprintf(path, sizeof(path), "%s/d%d", dir, i);
        rmdir(path);
    }
}

// ---------------------------------------------------------------- root scan

// Open-addressed (dev, ino) -> type map, used to spot two paths sharing an
// inode when one of them is a directory, or when the two disagree about being
// one. Genuine hard links share an inode and a type and are never directories,
// so they pass.
#define MAP_BITS 18
#define MAP_SLOTS (1u << MAP_BITS)

struct map_entry {
    dev_t dev;
    ino_t ino;
    char *path;
    bool is_dir;
    bool used;
};

static struct map_entry *inode_map;
static unsigned map_used;

static bool map_note(dev_t dev, ino_t ino, bool is_dir, const char *path, struct map_entry **clash) {
    if (inode_map == NULL || map_used * 4 >= MAP_SLOTS * 3)
        return false; // full enough; stop tracking rather than probe forever
    unsigned h = (unsigned) ((ino * 2654435761u) ^ (unsigned) dev) & (MAP_SLOTS - 1);
    for (unsigned probe = 0; probe < MAP_SLOTS; probe++) {
        struct map_entry *e = &inode_map[(h + probe) & (MAP_SLOTS - 1)];
        if (!e->used) {
            e->used = true;
            e->dev = dev;
            e->ino = ino;
            e->is_dir = is_dir;
            e->path = strdup(path);
            map_used++;
            return false;
        }
        if (e->dev == dev && e->ino == ino) {
            // Disagreeing types, or agreeing on "directory" -- which is just as
            // impossible, since link(2) will not hard-link a directory.
            if (e->is_dir != is_dir || is_dir) {
                *clash = e;
                return true;
            }
            return false;
        }
    }
    return false;
}

// A queue of directories still to visit, so a deep tree costs heap rather than
// C stack.
static char **queue;
static size_t queue_len, queue_cap;

static void queue_push(const char *path) {
    if (queue_len == queue_cap) {
        size_t cap = queue_cap == 0 ? 64 : queue_cap * 2;
        char **grown = realloc(queue, cap * sizeof(*grown));
        if (grown == NULL)
            return;
        queue = grown;
        queue_cap = cap;
    }
    char *copy = strdup(path);
    if (copy != NULL)
        queue[queue_len++] = copy;
}

#define SCAN_LIMIT 400000u

// Walk everything on the root's own device -- which skips /proc, /sys, /dev and
// any other mount without needing to name them -- and check two things per
// entry: that anything claiming to be a directory can actually be opened as
// one, and that no inode is shared by entries of disagreeing type.
static void scan_root(void) {
    struct stat root_st;
    if (stat("/", &root_st) != 0) {
        fail("stat /: %s", strerror(errno));
        return;
    }

    inode_map = calloc(MAP_SLOTS, sizeof(*inode_map));
    unsigned entries = 0, wedged = 0, aliased = 0;
    bool truncated = false;

    // Only meaningful where the host counts subdirectories. btrfs and friends
    // report st_nlink == 1 for every directory, which would make this fire on
    // all of them; the root directory is the cheapest way to ask.
    bool nlink_counts = root_st.st_nlink >= 2;
    if (!nlink_counts)
        test_logf("host does not count subdirectory links (/ has %u); "
                "skipping the link-count check\n", (unsigned) root_st.st_nlink);

    queue_push("/");
    for (size_t head = 0; head < queue_len; head++) {
        char *dirpath = queue[head];
        DIR *d = opendir(dirpath);
        if (d == NULL)
            continue;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            char path[PATH_MAX];
            if (snprintf(path, sizeof(path), "%s%s%s", dirpath,
                        strcmp(dirpath, "/") == 0 ? "" : "/", ent->d_name) >= (int) sizeof(path))
                continue;

            struct stat st;
            if (lstat(path, &st) != 0)
                continue; // vanished under us, or genuinely unreadable
            if (st.st_dev != root_st.st_dev)
                continue; // another filesystem mounted here
            if (++entries > SCAN_LIMIT) {
                truncated = true;
                break;
            }

            if (S_ISDIR(st.st_mode)) {
                // The wedge. A directory backed by a real host directory has at
                // least "." and its parent's entry; one that is a directory
                // only in the metadata carries the backing file's single link.
                if (nlink_counts && st.st_nlink < 2) {
                    if (wedged++ < 10)
                        fail("%s reports as a directory with st_nlink=%u; a real one has at "
                             "least 2, so the metadata and the host entry disagree",
                                path, (unsigned) st.st_nlink);
                }
                queue_push(path);
            }

            struct map_entry *clash = NULL;
            bool is_dir = S_ISDIR(st.st_mode) != 0;
            if (map_note(st.st_dev, st.st_ino, is_dir, path, &clash)) {
                if (aliased++ < 10) {
                    // Two IDENTICAL paths is a different fault from two paths
                    // sharing an inode, and saying "X and X share inode N but
                    // directories cannot be hard-linked" sends the reader
                    // looking for a second path that does not exist. It means
                    // readdir handed back the same name twice, which no real
                    // filesystem does: on a case-insensitive host, an escaped
                    // and an unescaped host entry can both decode to one guest
                    // name (see fs/fake-path.h), and then lstat resolves both
                    // sightings to whichever one wins. Measured on device:
                    // /usr/share/terminfo listed "A" twice, and the winner was
                    // the EMPTY twin, so all 335 entries under "a" were still
                    // there while Apple_Terminal was unreachable by name.
                    if (strcmp(clash->path, path) == 0)
                        fail("%s was returned twice by readdir (inode %llu); a directory "
                             "cannot hold two entries of the same name, so an escaped and "
                             "an unescaped host entry are both decoding to it",
                                path, (unsigned long long) st.st_ino);
                    else
                        fail("%s and %s share inode %llu but %s", clash->path, path,
                                (unsigned long long) st.st_ino,
                                clash->is_dir != is_dir ? "disagree about being a directory"
                                                        : "directories cannot be hard-linked");
                }
            }
        }
        closedir(d);
        if (truncated)
            break;
    }

    test_logf("scanned %u entries, %u tracked inodes%s\n", entries, map_used,
            truncated ? " (stopped at the scan limit)" : "");
    if (wedged > 10 || aliased > 10)
        printf("  (%u wedged, %u aliased entries in total)\n", wedged, aliased);

    for (size_t i = 0; i < queue_len; i++)
        free(queue[i]);
    free(queue);
    if (inode_map != NULL) {
        for (unsigned i = 0; i < MAP_SLOTS; i++)
            free(inode_map[i].path);
        free(inode_map);
    }
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(300));

    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "/tmp/fakefs_inode_alias.%d", (int) getpid());
    if (mkdir(dir, 0755) != 0) {
        printf("fakefs_inode_alias: FAIL setup mkdir %s: %s\n", dir, strerror(errno));
        return 1;
    }

    check_removal_semantics(dir);
    check_fresh_inodes(dir);
    rmdir(dir);

    scan_root();

    if (failures != 0) {
        printf("fakefs_inode_alias: FAIL failures=%u\n", failures);
        return 1;
    }
    printf("fakefs_inode_alias: PASS\n");
    return 0;
}
