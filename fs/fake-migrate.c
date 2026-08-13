#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kernel/fs.h"
#include "debug.h"
#include "kernel/errno.h"
#include "fs/fake-db.h"
#include "fs/fake-path.h"
#include "fs/sqlutil.h"

// The value of the user_version pragma is used to decide what needs migrating.

// versions 4 and 5: rename host files to the escaped on-disk form
// (fs/fake-path.h), so guest names that a case- and normalization-
// insensitive host filesystem (APFS) considers equal stop colliding.
//
// Both versions share one walk (migrate_host_names); they differ only in
// what the *source* name on the host currently looks like:
//   v4: raw guest names (pre-escaping roots)
//   v5: the v4 escape format, which covered only ASCII case ('A' -> "%a",
//       '%' -> "%%") and left non-ASCII bytes raw
// The destination is always the current full format from fs/fake-path.h. A
// pre-v4 root therefore jumps straight to the full format in its v4 pass,
// and its v5 pass finds nothing left to do.
//
// Walked in path order, so a directory is renamed before anything inside it;
// each entry's current host location is therefore escape(dirname) plus its
// own source-format name. Best-effort by design: a root that already
// suffered collisions has host state that can't be fully recovered (two DB
// paths sharing one host file), and a half-finished previous run leaves
// entries already in escaped form -- both are detected and skipped, so the
// migration is safe to re-run.

// The frozen v4 escape format: ASCII case only, non-ASCII bytes raw. Used as
// the v5 pass's source encoding; must never change again.
static char *fake_path_to_host_v4(const char *path, char *buf, unsigned long bufsize) {
    unsigned long i = 0;
    for (; *path != '\0'; path++) {
        char c = *path;
        if (c >= 'A' && c <= 'Z') {
            if (i + 2 >= bufsize)
                return NULL;
            buf[i++] = FAKE_PATH_ESCAPE_CHAR;
            buf[i++] = c + ('a' - 'A');
        } else if (c == FAKE_PATH_ESCAPE_CHAR) {
            if (i + 2 >= bufsize)
                return NULL;
            buf[i++] = FAKE_PATH_ESCAPE_CHAR;
            buf[i++] = FAKE_PATH_ESCAPE_CHAR;
        } else {
            if (i + 1 >= bufsize)
                return NULL;
            buf[i++] = c;
        }
    }
    buf[i] = '\0';
    return buf;
}

// The v4 source encoding is the identity: raw guest names on the host.
static char *fake_path_identity(const char *path, char *buf, unsigned long bufsize) {
    unsigned long len = strlen(path);
    if (len >= bufsize)
        return NULL;
    memcpy(buf, path, len + 1);
    return buf;
}

// One directory's entry names, held across consecutive rows of the migration
// query.
//
// Each escaped path used to cost a full readdir of its parent, so the pass was
// O(escaped paths x entries in their directory): a single directory with a few
// thousand escape-needing names (any uppercase letter or byte >= 0x80 qualifies,
// so this is ordinary content, not exotic filenames) took millions of dirent
// comparisons and thousands of getdirentries syscalls. Boot runs this inside
// mount_root on the main thread, so on a large filesystem RunningBoard killed
// the app before it finished (0xdead10cc, thread 0 in readdir under
// find_ondisk_name).
//
// The query is "order by path", so every path in a directory arrives in one
// consecutive run: caching just the most recent directory turns that into one
// readdir per directory.
struct migrate_dir_cache {
    char dir[MAX_PATH + 1];
    bool loaded;
    // Set only when the listing is known to hold every entry. A partial listing
    // must never be consulted: a miss on a name that is really there sends the
    // caller down the twin-fallback path and it hardlinks a file it should have
    // renamed. On a short read we fall back to re-reading the directory.
    bool complete;
    char **names;
    size_t count;
    size_t cap;
    // v6 only, built on demand: the entry names sorted so "is this name here?"
    // is a binary search rather than a scan. v6 asks that of *every* path, not
    // just the escape-needing ones, and the largest directory in an ordinary
    // root is big (11137 entries in /usr/share/man/man3 on an Arch ARM root),
    // so a linear probe per path would reinstate the O(paths x entries) shape
    // 7ef8f91b removed. Pointers into names, so building it allocates one
    // array and copies no strings.
    const char **sorted;
    size_t sorted_count;
    bool sorted_valid;
};

static void dir_cache_drop_index(struct migrate_dir_cache *cache) {
    free(cache->sorted);
    cache->sorted = NULL;
    cache->sorted_count = 0;
    cache->sorted_valid = false;
}

static void dir_cache_reset(struct migrate_dir_cache *cache) {
    dir_cache_drop_index(cache);
    for (size_t i = 0; i < cache->count; i++)
        free(cache->names[i]);
    free(cache->names);
    cache->names = NULL;
    cache->count = cache->cap = 0;
    cache->loaded = false;
    cache->complete = false;
}

static int migrate_name_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *) a, *(const char *const *) b);
}

static bool dir_cache_index(struct migrate_dir_cache *cache) {
    if (cache->sorted_valid)
        return true;
    dir_cache_drop_index(cache);
    if (cache->count != 0) {
        cache->sorted = calloc(cache->count, sizeof(*cache->sorted));
        if (cache->sorted == NULL)
            return false;
        for (size_t i = 0; i < cache->count; i++)
            cache->sorted[i] = cache->names[i];
        cache->sorted_count = cache->count;
        qsort(cache->sorted, cache->sorted_count, sizeof(*cache->sorted), migrate_name_cmp);
    }
    cache->sorted_valid = true;
    return true;
}

// Is an entry spelled exactly `host_name` in this directory? That, and only
// that, is what makes a guest path resolve -- "E" and "%e" are one guest name
// in two spellings and only the escaped one is reachable.
static bool dir_cache_has(const struct migrate_dir_cache *cache, const char *host_name) {
    long lo = 0, hi = (long) cache->sorted_count - 1;
    while (lo <= hi) {
        long mid = lo + (hi - lo) / 2;
        int r = strcmp(cache->sorted[mid], host_name);
        if (r == 0)
            return true;
        if (r < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}

// Load host_dir's entry names, reusing the cache when it is already the
// directory we want. Returns false only if the directory can't be read.
static bool dir_cache_load(struct migrate_dir_cache *cache, int root_fd, const char *host_dir) {
    if (cache->loaded && strcmp(cache->dir, host_dir) == 0)
        return true;
    dir_cache_reset(cache);
    if (strlen(host_dir) >= sizeof(cache->dir))
        return false;

    int dirfd = openat(root_fd, host_dir[0] == '\0' ? "." : host_dir, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0)
        return false;
    DIR *dir = fdopendir(dirfd);
    if (dir == NULL) {
        close(dirfd);
        return false;
    }
    bool complete = true;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (cache->count == cache->cap) {
            size_t cap = cache->cap == 0 ? 64 : cache->cap * 2;
            char **names = realloc(cache->names, cap * sizeof(*names));
            if (names == NULL) {
                complete = false;
                break;
            }
            cache->names = names;
            cache->cap = cap;
        }
        char *name = strdup(ent->d_name);
        if (name == NULL) {
            complete = false;
            break;
        }
        cache->names[cache->count++] = name;
    }
    closedir(dir);
    strcpy(cache->dir, host_dir);
    cache->loaded = true;
    cache->complete = complete;
    return true;
}

// Find the on-disk name (into name_out) of the cached entry matching name
// case-insensitively (ASCII). Returns true if found. Scans in readdir order and
// returns the first match, exactly as the per-call readdir it replaces did --
// which case-twin wins must not change.
static bool dir_cache_find(const struct migrate_dir_cache *cache, const char *name, char *name_out, size_t name_out_size) {
    for (size_t i = 0; i < cache->count; i++) {
        if (strcasecmp(cache->names[i], name) == 0) {
            size_t len = strlen(cache->names[i]);
            if (len >= name_out_size)
                return false;
            memcpy(name_out, cache->names[i], len + 1);
            return true;
        }
    }
    return false;
}

// The original per-call directory scan, kept for the case where the listing
// couldn't be held in memory.
static bool find_ondisk_name(int root_fd, const char *host_dir, const char *name, char *name_out, size_t name_out_size) {
    int dirfd = openat(root_fd, host_dir[0] == '\0' ? "." : host_dir, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0)
        return false;
    DIR *dir = fdopendir(dirfd);
    if (dir == NULL) {
        close(dirfd);
        return false;
    }
    bool found = false;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcasecmp(ent->d_name, name) == 0) {
            size_t len = strlen(ent->d_name);
            if (len < name_out_size) {
                memcpy(name_out, ent->d_name, len + 1);
                found = true;
            }
            break;
        }
    }
    closedir(dir);
    return found;
}

// Resolve through the cached listing when it is known complete, and otherwise
// by re-reading the directory, so a short listing costs speed and never
// correctness.
static bool lookup_ondisk_name(struct migrate_dir_cache *cache, int root_fd, const char *host_dir, const char *name, char *name_out, size_t name_out_size) {
    if (dir_cache_load(cache, root_fd, host_dir) && cache->complete)
        return dir_cache_find(cache, name, name_out, name_out_size);
    return find_ondisk_name(root_fd, host_dir, name, name_out, name_out_size);
}

// Keep the listing honest after the pass renames an entry in it. Without this a
// later case-twin in the same directory would match the name we just moved away
// and be misclassified as sharing a host file with it.
static void dir_cache_note_rename(struct migrate_dir_cache *cache, const char *from, const char *to) {
    for (size_t i = 0; i < cache->count; i++) {
        if (strcmp(cache->names[i], from) != 0)
            continue;
        char *name = strdup(to);
        if (name == NULL) {
            // Can't record it; drop the whole listing rather than leave a stale
            // name behind, and the next lookup re-reads the directory.
            dir_cache_reset(cache);
            return;
        }
        free(cache->names[i]);
        cache->names[i] = name;
        cache->sorted_valid = false; // the index points into names
        return;
    }
}

// Give a collision twin its own host entry under host_dst: the equivalent-
// named file stays with the twin that owns it; this DB path gets a hardlink
// (files) or a fresh directory (dirs) so it at least still resolves. The
// shared content was already merged when the collision happened and can't be
// un-merged.
static void migrate_twin_fallback(int root_fd, const char *host_src, const char *host_dst, const struct stat *st) {
    if (S_ISDIR(st->st_mode)) {
        if (mkdirat(root_fd, host_dst, 0777) < 0 && errno != EEXIST)
            printk("fakefs migrate: mkdir %s failed: %d\n", host_dst, errno);
        printk("fakefs migrate: %s collided with an equivalent host name; contents stay with the twin\n",
                host_dst);
    } else {
        if (linkat(root_fd, host_src, root_fd, host_dst, 0) < 0 && errno != EEXIST)
            printk("fakefs migrate: link %s -> %s failed: %d\n", host_src, host_dst, errno);
    }
}

static void migrate_host_names(struct fakefs_db *fs, int root_fd,
        char *(*src_encode)(const char *, char *, unsigned long)) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(fs->db, "select path from paths order by path", -1, &stmt, NULL) != SQLITE_OK) {
        printk("ERROR: fakefs migrate: %s\n", sqlite3_errmsg(fs->db));
        return;
    }
    struct migrate_dir_cache cache = {0};
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const void *path_blob = sqlite3_column_blob(stmt, 0);
        int path_len = sqlite3_column_bytes(stmt, 0);
        if (path_blob == NULL || path_len <= 0 || path_len > MAX_PATH)
            continue;
        char path[MAX_PATH + 1];
        memcpy(path, path_blob, path_len);
        path[path_len] = '\0';
        // If nothing in the path needs escaping, every encoding of it
        // (raw, v4, current) is identical and there is nothing to move.
        if (!fake_path_needs_escape(path))
            continue;

        // DB paths look like "/usr/share/Foo" ("" for the root). Split into
        // dirname (already migrated, so fully escaped on the host) and
        // basename (still in the pass's source format on the host).
        char *slash = strrchr(path, '/');
        if (slash == NULL)
            continue; // not a real path ("" root can't need escaping anyway)
        *slash = '\0';
        const char *base = slash + 1;
        char host_dir[MAX_PATH + 1];
        if (fake_path_to_host(path[0] == '\0' ? "" : path + 1, host_dir, sizeof(host_dir)) == NULL)
            continue;

        char src_base[NAME_MAX * 3 + 2], host_base[NAME_MAX * 3 + 2];
        if (src_encode(base, src_base, sizeof(src_base)) == NULL)
            continue;
        if (fake_path_to_host(base, host_base, sizeof(host_base)) == NULL)
            continue;
        if (strcmp(src_base, host_base) == 0)
            continue; // this pass changes nothing about this name

        char host_src[MAX_PATH + 1], host_dst[MAX_PATH + 1];
        if (snprintf(host_src, sizeof(host_src), "%s%s%s", host_dir, host_dir[0] != '\0' ? "/" : "", src_base) >= (int) sizeof(host_src))
            continue;
        if (snprintf(host_dst, sizeof(host_dst), "%s%s%s", host_dir, host_dir[0] != '\0' ? "/" : "", host_base) >= (int) sizeof(host_dst))
            continue;

        // An insensitive host resolves host_src to *some* entry -- maybe
        // this path's own file, maybe a case-twin's. Find the true on-disk
        // name to tell those apart (and to detect an already-migrated entry
        // after an interrupted run).
        char ondisk[NAME_MAX + 1];
        if (!lookup_ondisk_name(&cache, root_fd, host_dir, src_base, ondisk, sizeof(ondisk))) {
            struct stat st;
            if (fstatat(root_fd, host_dst, &st, AT_SYMLINK_NOFOLLOW) == 0)
                continue; // already migrated (resumed run)
            // The directory scan compares bytes (plus ASCII case), but the
            // host may still resolve host_src through Unicode case folding
            // or normalization equivalence -- that's a twin whose on-disk
            // bytes differ from ours.
            if (fstatat(root_fd, host_src, &st, AT_SYMLINK_NOFOLLOW) == 0) {
                migrate_twin_fallback(root_fd, host_src, host_dst, &st);
                continue;
            }
            printk("fakefs migrate: %s: missing on host, skipped\n", host_src);
            continue;
        }
        if (strcmp(ondisk, host_base) == 0)
            continue; // already migrated (resumed run)

        if (strcmp(ondisk, src_base) == 0) {
            // The normal case: the file is really ours; move it.
            if (renameat(root_fd, host_src, root_fd, host_dst) < 0)
                printk("fakefs migrate: rename %s -> %s failed: %d\n", host_src, host_dst, errno);
            else
                dir_cache_note_rename(&cache, src_base, host_base);
            continue;
        }

        // The on-disk name differs only in ASCII case: a case-twin owns the
        // shared host file.
        struct stat st;
        if (fstatat(root_fd, host_src, &st, AT_SYMLINK_NOFOLLOW) < 0) {
            printk("fakefs migrate: %s: missing on host, skipped\n", host_src);
            continue;
        }
        migrate_twin_fallback(root_fd, host_src, host_dst, &st);
    }
    dir_cache_reset(&cache);
    sqlite3_finalize(stmt);
}

// version 4: raw guest names -> escaped (originally just ASCII case; now
// escapes straight to the full current format, letting v5 no-op)
static void migrate_escape_host_names(struct fakefs_db *fs, int root_fd) {
    migrate_host_names(fs, root_fd, fake_path_identity);
}

// version 5: v4's ASCII-case-only escape format -> the full format that also
// escapes non-ASCII bytes (Unicode case folding/normalization collisions)
static void migrate_reescape_unicode(struct fakefs_db *fs, int root_fd) {
    migrate_host_names(fs, root_fd, fake_path_to_host_v4);
}

// version 6: put back what the v4/v5 rename pass orphaned.
//
// Those passes move a host entry when its *name* changes, and skip any guest
// path that needs no escaping. Right for the name, wrong for the location: when
// a case-twin ancestor is renamed (host "A" -> "%a"), entries that a folding
// host had been reaching *through* that directory are still physically inside
// it, while their own escaped path ("a/...") now resolves to nothing. On a
// pre-v4 root whose twins had merged, that is the entire lowercase half of
// every twin pair -- and it goes missing at the moment of the upgrade, having
// been perfectly readable before it. Measured on an ArchLinuxARM aarch64 root:
// 1069 of 2899 terminfo files unreachable afterwards, xterm-256color among
// them, so ncurses "installs" and every curses program fails to find a
// terminal.
//
// This pass restores the invariant the escaping exists to provide -- for every
// DB path there is a host entry at its escaped form -- by hunting anything
// missing in the twin that swallowed it. An entry whose own guest path is also
// a DB path is genuinely shared (both guest paths named one host file, and did
// before the escaping too), so it gets hardlinked rather than stolen from its
// owner; that is the same concession migrate_twin_fallback makes.
//
// It has to be its own version rather than a repair inside v4/v5: every root
// that has already booted under a build carrying those passes sits at
// user_version 5 and would never run them again, and those are precisely the
// roots that are broken.
//
// Every action is gated on the entry being missing, so the pass is idempotent
// and a healthy root pays one readdir per directory and performs no writes.

// A directory this pass had to recreate because the host entry holding its
// contents belongs to a twin. Its children are still inside that twin, so a
// child that comes up missing later in the pass knows where to look. Only
// populated for real damage, so it stays small.
struct migrate_salvage {
    char *guest_dir;
    char *host_dir;
};

static const char *salvage_lookup(struct migrate_salvage *salvage, size_t count, const char *guest_dir) {
    for (size_t i = 0; i < count; i++)
        if (strcmp(salvage[i].guest_dir, guest_dir) == 0)
            return salvage[i].host_dir;
    return NULL;
}

// Scan host_dir for the entry that is guest name `name` (*exact_out) and for
// one that a folding host would have conflated with it (*fold_out). Used for
// the salvage directory, which is never the one the pass has cached, and only
// reached for an entry already known to be missing.
static void scan_host_dir(int root_fd, const char *host_dir, const char *name,
        char *exact_out, char *fold_out, size_t out_size) {
    exact_out[0] = fold_out[0] = '\0';
    int dirfd = openat(root_fd, host_dir[0] == '\0' ? "." : host_dir, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0)
        return;
    DIR *dir = fdopendir(dirfd);
    if (dir == NULL) {
        close(dirfd);
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char guest[NAME_MAX * 3 + 2];
        if (strlen(ent->d_name) >= sizeof(guest) || strlen(ent->d_name) >= out_size)
            continue;
        strcpy(guest, ent->d_name);
        fake_path_from_host(guest);
        if (strcmp(guest, name) == 0) {
            strcpy(exact_out, ent->d_name);
            break; // an exact match is the answer; nothing beats it
        }
        if (fold_out[0] == '\0' && strcasecmp(guest, name) == 0)
            strcpy(fold_out, ent->d_name);
    }
    closedir(dir);
}

static void migrate_repair_orphans(struct fakefs_db *fs, int root_fd) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(fs->db, "select path from paths order by path", -1, &stmt, NULL) != SQLITE_OK) {
        printk("ERROR: fakefs migrate: %s\n", sqlite3_errmsg(fs->db));
        return;
    }
    sqlite3_stmt *owner;
    if (sqlite3_prepare_v2(fs->db, "select 1 from paths where path = ?", -1, &owner, NULL) != SQLITE_OK) {
        printk("ERROR: fakefs migrate: %s\n", sqlite3_errmsg(fs->db));
        sqlite3_finalize(stmt);
        return;
    }

    struct migrate_dir_cache cache = {0};
    struct migrate_salvage *salvage = NULL;
    size_t salvage_count = 0, salvage_cap = 0;
    unsigned moved = 0, linked = 0, recreated = 0, unfound = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const void *path_blob = sqlite3_column_blob(stmt, 0);
        int path_len = sqlite3_column_bytes(stmt, 0);
        if (path_blob == NULL || path_len <= 0 || path_len > MAX_PATH)
            continue;
        char path[MAX_PATH + 1];
        memcpy(path, path_blob, path_len);
        path[path_len] = '\0';

        char *slash = strrchr(path, '/');
        if (slash == NULL)
            continue;
        *slash = '\0';
        const char *guest_dir = path; // "" at the root, else "/usr/share"
        const char *base = slash + 1;
        if (base[0] == '\0')
            continue;

        char host_dir[MAX_PATH + 1], host_base[NAME_MAX * 3 + 2], host_dst[MAX_PATH + 1];
        if (fake_path_to_host(guest_dir[0] == '\0' ? "" : guest_dir + 1, host_dir, sizeof(host_dir)) == NULL)
            continue;
        if (fake_path_to_host(base, host_base, sizeof(host_base)) == NULL)
            continue;
        if (snprintf(host_dst, sizeof(host_dst), "%s%s%s",
                    host_dir, host_dir[0] != '\0' ? "/" : "", host_base) >= (int) sizeof(host_dst))
            continue;

        // Is it where it belongs? The cached listing answers this without a
        // syscall; a listing that couldn't be built or completed falls back to
        // asking the host, so a short read costs speed and never correctness.
        if (dir_cache_load(&cache, root_fd, host_dir) && cache.complete && dir_cache_index(&cache)) {
            if (dir_cache_has(&cache, host_base))
                continue;
        } else {
            struct stat st;
            if (fstatat(root_fd, host_dst, &st, AT_SYMLINK_NOFOLLOW) == 0)
                continue;
        }

        // Missing. Everything from here is the repair path, taken only for an
        // entry that is really gone, so it can afford to read directories
        // again and decode names.
        char src_dir[MAX_PATH + 1], src_name[NAME_MAX + 1];
        bool found_here = false;
        {
            // Still in this directory under another spelling? Either its own,
            // predating the escaping (a subtree that v6 moved wholesale carries
            // its children in whatever spelling they had, and v4/v5 could not
            // reach them to escape them while they were stranded), or a twin's.
            char alias[NAME_MAX + 1], fold[NAME_MAX + 1];
            scan_host_dir(root_fd, host_dir, base, alias, fold, sizeof(alias));
            const char *here = alias[0] != '\0' ? alias : (fold[0] != '\0' ? fold : NULL);
            if (here != NULL) {
                strcpy(src_name, here);
                found_here = true;
            }
        }

        // Missing. It is either still in this same directory under another
        // spelling (its own, pre-escaping, or a twin's), or inside the twin of
        // this directory itself -- which, for a directory this pass already
        // recreated, it recorded.
        if (found_here) {
            strcpy(src_dir, host_dir);
        } else {
            const char *from = salvage_lookup(salvage, salvage_count, guest_dir);
            if (from != NULL) {
                if (strlen(from) >= sizeof(src_dir))
                    continue;
                strcpy(src_dir, from);
            } else {
                // No record, so look for a twin of this path's own directory.
                char parent[MAX_PATH + 1];
                strcpy(parent, guest_dir);
                char *pslash = strrchr(parent, '/');
                if (pslash == NULL) {
                    unfound++;
                    continue;
                }
                *pslash = '\0';
                char host_parent[MAX_PATH + 1], twin[NAME_MAX + 1], ignored[NAME_MAX + 1];
                if (fake_path_to_host(parent[0] == '\0' ? "" : parent + 1, host_parent, sizeof(host_parent)) == NULL)
                    continue;
                scan_host_dir(root_fd, host_parent, pslash + 1, ignored, twin, sizeof(twin));
                if (twin[0] == '\0') {
                    unfound++;
                    continue;
                }
                if (snprintf(src_dir, sizeof(src_dir), "%s%s%s",
                            host_parent, host_parent[0] != '\0' ? "/" : "", twin) >= (int) sizeof(src_dir))
                    continue;
            }
            char exact[NAME_MAX + 1], fold[NAME_MAX + 1];
            scan_host_dir(root_fd, src_dir, base, exact, fold, sizeof(exact));
            const char *pick = exact[0] != '\0' ? exact : (fold[0] != '\0' ? fold : NULL);
            if (pick == NULL) {
                unfound++;
                continue;
            }
            strcpy(src_name, pick);
        }

        char src_path[MAX_PATH + 1];
        if (snprintf(src_path, sizeof(src_path), "%s%s%s",
                    src_dir, src_dir[0] != '\0' ? "/" : "", src_name) >= (int) sizeof(src_path))
            continue;
        struct stat st;
        if (fstatat(root_fd, src_path, &st, AT_SYMLINK_NOFOLLOW) < 0)
            continue;

        // Does another DB path claim the entry we found? Escaping passes '/'
        // through, so decoding the whole host path gives the guest path it
        // would be reached by.
        char claim[MAX_PATH + 2];
        claim[0] = '/';
        strcpy(claim + 1, src_path);
        fake_path_from_host(claim + 1);
        char full[MAX_PATH + 2];
        snprintf(full, sizeof(full), "%s/%s", guest_dir, base);
        // If what we found decodes to this very path it is our own entry in a
        // spelling that predates the escaping, and simply needs renaming into
        // the canonical one. Otherwise ask whether another DB path claims it.
        bool owned = false;
        if (strcmp(claim, full) != 0) {
            // paths is a blob column, and SQLite never compares a text value
            // equal to a blob one -- bound as text this asks "is anything
            // unowned?", always answers yes, and renames a twin's whole
            // directory away.
            sqlite3_bind_blob(owner, 1, claim, (int) strlen(claim), SQLITE_TRANSIENT);
            owned = sqlite3_step(owner) == SQLITE_ROW;
            sqlite3_reset(owner);
        }

        if (S_ISDIR(st.st_mode)) {
            if (!owned) {
                // Nothing else answers to it, so the whole subtree moves.
                if (renameat(root_fd, src_path, root_fd, host_dst) < 0)
                    printk("fakefs migrate: rename %s -> %s failed: %d\n", src_path, host_dst, errno);
                else
                    moved++;
            } else if (mkdirat(root_fd, host_dst, 0777) < 0 && errno != EEXIST) {
                printk("fakefs migrate: mkdir %s failed: %d\n", host_dst, errno);
            } else {
                // The twin keeps the directory; remember where this one's
                // children are so they can be pulled across as they come up.
                recreated++;
                if (salvage_count == salvage_cap) {
                    size_t cap = salvage_cap == 0 ? 8 : salvage_cap * 2;
                    struct migrate_salvage *grown = realloc(salvage, cap * sizeof(*grown));
                    if (grown != NULL) {
                        salvage = grown;
                        salvage_cap = cap;
                    }
                }
                if (salvage_count < salvage_cap) {
                    // Children arrive with this directory as their own
                    // guest_dir, which is exactly `full`.
                    char *g = strdup(full);
                    char *h = strdup(src_path);
                    if (g != NULL && h != NULL) {
                        salvage[salvage_count].guest_dir = g;
                        salvage[salvage_count].host_dir = h;
                        salvage_count++;
                    } else {
                        free(g);
                        free(h);
                    }
                }
            }
        } else if (owned) {
            if (linkat(root_fd, src_path, root_fd, host_dst, 0) < 0 && errno != EEXIST)
                printk("fakefs migrate: link %s -> %s failed: %d\n", src_path, host_dst, errno);
            else
                linked++;
        } else {
            if (renameat(root_fd, src_path, root_fd, host_dst) < 0)
                printk("fakefs migrate: rename %s -> %s failed: %d\n", src_path, host_dst, errno);
            else
                moved++;
        }
        // The cached listing is left alone deliberately. The only entry added
        // to it is the one being processed, which no later path asks about,
        // and an entry is only renamed *away* when no DB path claims it, so
        // nothing later asks about that either.
    }

    if (moved != 0 || linked != 0 || recreated != 0 || unfound != 0)
        printk("fakefs migrate: repaired %u moved, %u shared, %u directories; %u not found\n",
                moved, linked, recreated, unfound);

    for (size_t i = 0; i < salvage_count; i++) {
        free(salvage[i].guest_dir);
        free(salvage[i].host_dir);
    }
    free(salvage);
    dir_cache_reset(&cache);
    sqlite3_finalize(owner);
    sqlite3_finalize(stmt);
}

// version 7: give back their own metadata to paths that an inode collision
// aliased onto another file's.
//
// fs/fake-db.c's path_create used to write the paths row even when its
// `insert into stats` had just lost a primary-key race, binding the new path to
// a pre-existing inode belonging to an unrelated file. Both paths then read one
// stat blob. Harmless-looking until the two disagree about being a directory,
// at which point the entry is beyond removal: rmdir(2) gets ENOTDIR from the
// host, unlink(2) gets EISDIR from the metadata. A guest gcc, which reuses
// temporary names, then aborts with "Cannot create temporary file in /tmp/: Is
// a directory" and cannot build anything until TMPDIR moves.
//
// path_create is fixed and fakefs_rmdir can now clear one of these on demand,
// but neither helps a root that already carries the damage -- the aliased path
// keeps reporting a type its host entry does not have. So walk the table once
// and hand every mismatched path a stats row of its own, carrying the type the
// host entry really has and the permissions the blob already claimed.
//
// ONLY the directory bit is compared. fakefs deliberately stores symlinks,
// sockets and device nodes as ordinary host files, with the real type held in
// the metadata alone, so comparing full S_IFMT would report every symlink in
// the root as damaged and "repair" them into regular files.
struct migrate_alias {
    char *path;
    uint64_t inode;
    struct ish_stat stat;
    bool host_is_dir;
};

// Can the host directory this entry lives in still be opened? Used as the
// safety gate before deleting a row whose own host entry is missing: a whole
// subtree that suddenly cannot be found means the name mapping is wrong, not
// that every file in it was deleted.
static bool host_parent_exists(int root_fd, const char *host) {
    char parent[MAX_PATH + 1];
    const char *slash = strrchr(host, '/');
    size_t len = slash == NULL ? 0 : (size_t) (slash - host);
    if (len == 0 || len >= sizeof(parent)) {
        strcpy(parent, ".");
    } else {
        memcpy(parent, host, len);
        parent[len] = '\0';
    }
    int fd = openat(root_fd, parent, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

static void migrate_repair_type_alias(struct fakefs_db *fs, int root_fd) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(fs->db,
                "select paths.path, paths.inode, stats.stat from paths "
                "join stats on stats.inode = paths.inode", -1, &stmt, NULL) != SQLITE_OK) {
        printk("ERROR: fakefs migrate: %s\n", sqlite3_errmsg(fs->db));
        return;
    }

    // Collected first and applied afterwards: rewriting paths while stepping a
    // select over it is not something SQLite promises to iterate sanely. Real
    // damage is a handful of rows at most, so this stays small.
    struct migrate_alias *alias = NULL;
    size_t count = 0, cap = 0;
    char **stale = NULL;
    size_t stale_count = 0, stale_cap = 0;
    unsigned long scanned = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const void *path_blob = sqlite3_column_blob(stmt, 0);
        int path_len = sqlite3_column_bytes(stmt, 0);
        // The root ("") has no name to check and no parent to check it from.
        if (path_blob == NULL || path_len <= 1 || path_len > MAX_PATH)
            continue;
        const void *stat_blob = sqlite3_column_blob(stmt, 2);
        if (stat_blob == NULL || sqlite3_column_bytes(stmt, 2) != sizeof(struct ish_stat))
            continue;
        char path[MAX_PATH + 1];
        memcpy(path, path_blob, path_len);
        path[path_len] = '\0';
        if (path[0] != '/')
            continue;
        scanned++;

        char host[MAX_PATH + 1];
        if (fake_path_to_host(path + 1, host, sizeof(host)) == NULL)
            continue;
        struct stat st;
        if (fstatat(root_fd, host, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            // A row with nothing behind it is NOT harmless. The guest cannot
            // stat the name, but it cannot create it either: generic_openat
            // reads the metadata, sees a directory and answers EISDIR long
            // before the host is consulted, so `: > name` fails with "Is a
            // directory" on a name that appears not to exist. That is the same
            // dead name reached from the other side, and it is what a collision
            // leaves behind once the host file is eventually removed.
            //
            // ENOENT only, and only when the parent directory really is there.
            // If the escaped-name mapping were ever wrong every lookup in a
            // subtree would miss, and this would delete the root's whole table;
            // an openable parent says the mapping works and the entry is simply
            // gone. The stats row left behind is collected by the orphan sweep
            // fake_db_init runs just after the migration.
            if (errno != ENOENT || !host_parent_exists(root_fd, host))
                continue;
            if (stale_count == stale_cap) {
                size_t new_cap = stale_cap == 0 ? 8 : stale_cap * 2;
                char **grown = realloc(stale, new_cap * sizeof(*grown));
                if (grown == NULL)
                    continue;
                stale = grown;
                stale_cap = new_cap;
            }
            char *copy = strdup(path);
            if (copy != NULL)
                stale[stale_count++] = copy;
            continue;
        }

        struct ish_stat ishstat;
        memcpy(&ishstat, stat_blob, sizeof(ishstat));
        bool host_is_dir = S_ISDIR(st.st_mode);
        if (host_is_dir == (S_ISDIR(ishstat.mode) != 0))
            continue;

        if (count == cap) {
            size_t new_cap = cap == 0 ? 8 : cap * 2;
            struct migrate_alias *grown = realloc(alias, new_cap * sizeof(*grown));
            if (grown == NULL)
                break;
            alias = grown;
            cap = new_cap;
        }
        alias[count].path = strdup(path);
        if (alias[count].path == NULL)
            break;
        alias[count].inode = (uint64_t) sqlite3_column_int64(stmt, 1);
        alias[count].stat = ishstat;
        alias[count].host_is_dir = host_is_dir;
        count++;
    }
    sqlite3_finalize(stmt);

    if (stale_count != 0) {
        sqlite3_stmt *drop;
        if (sqlite3_prepare_v2(fs->db, "delete from paths where path = ?", -1, &drop, NULL) == SQLITE_OK) {
            for (size_t i = 0; i < stale_count; i++) {
                sqlite3_bind_blob(drop, 1, stale[i], strlen(stale[i]), SQLITE_TRANSIENT);
                sqlite3_step(drop);
                sqlite3_reset(drop);
                printk("fakefs migrate: dropped %s, which had metadata but no host entry\n", stale[i]);
            }
            sqlite3_finalize(drop);
            printk("fakefs migrate: freed %lu dead names\n", (unsigned long) stale_count);
        } else {
            printk("ERROR: fakefs migrate: %s\n", sqlite3_errmsg(fs->db));
        }
    }
    for (size_t i = 0; i < stale_count; i++)
        free(stale[i]);
    free(stale);

    if (count == 0) {
        free(alias);
        return;
    }

    sqlite3_stmt *shared = NULL, *add_stat = NULL, *repoint = NULL, *fix_stat = NULL;
    unsigned split = 0, retyped = 0;
    if (sqlite3_prepare_v2(fs->db, "select count(*) from paths where inode = ?", -1, &shared, NULL) != SQLITE_OK ||
            sqlite3_prepare_v2(fs->db, "insert into stats (stat) values (?)", -1, &add_stat, NULL) != SQLITE_OK ||
            sqlite3_prepare_v2(fs->db, "update paths set inode = ? where path = ?", -1, &repoint, NULL) != SQLITE_OK ||
            sqlite3_prepare_v2(fs->db, "update stats set stat = ? where inode = ?", -1, &fix_stat, NULL) != SQLITE_OK) {
        printk("ERROR: fakefs migrate: %s\n", sqlite3_errmsg(fs->db));
        goto out;
    }

    for (size_t i = 0; i < count; i++) {
        struct ish_stat fixed = alias[i].stat;
        fixed.mode = (fixed.mode & ~(uint32_t) S_IFMT) |
            (uint32_t) (alias[i].host_is_dir ? S_IFDIR : S_IFREG);

        sqlite3_bind_int64(shared, 1, (int64_t) alias[i].inode);
        int64_t sharers = sqlite3_step(shared) == SQLITE_ROW ? sqlite3_column_int64(shared, 0) : 1;
        sqlite3_reset(shared);

        if (sharers > 1) {
            // Someone else is using this blob correctly; leave it alone and
            // give this path a row of its own. stats.inode is the rowid, so
            // SQLite picks a free number here without consulting the in-memory
            // counter -- which fake_db_init has not even seeded yet.
            sqlite3_bind_blob(add_stat, 1, &fixed, sizeof(fixed), SQLITE_TRANSIENT);
            if (sqlite3_step(add_stat) != SQLITE_DONE) {
                printk("ERROR: fakefs migrate: %s\n", sqlite3_errmsg(fs->db));
                sqlite3_reset(add_stat);
                continue;
            }
            int64_t fresh = sqlite3_last_insert_rowid(fs->db);
            sqlite3_reset(add_stat);
            sqlite3_bind_int64(repoint, 1, fresh);
            sqlite3_bind_blob(repoint, 2, alias[i].path, strlen(alias[i].path), SQLITE_TRANSIENT);
            sqlite3_step(repoint);
            sqlite3_reset(repoint);
            split++;
        } else {
            // Sole owner, so the blob is simply wrong about the type.
            sqlite3_bind_blob(fix_stat, 1, &fixed, sizeof(fixed), SQLITE_TRANSIENT);
            sqlite3_bind_int64(fix_stat, 2, (int64_t) alias[i].inode);
            sqlite3_step(fix_stat);
            sqlite3_reset(fix_stat);
            retyped++;
        }
        printk("fakefs migrate: %s claimed to be %s; the host entry is %s\n", alias[i].path,
                alias[i].host_is_dir ? "a file" : "a directory",
                alias[i].host_is_dir ? "a directory" : "a file");
    }
    printk("fakefs migrate: repaired %u aliased and %u mistyped entries of %lu\n",
            split, retyped, scanned);

out:
    sqlite3_finalize(shared);
    sqlite3_finalize(add_stat);
    sqlite3_finalize(repoint);
    sqlite3_finalize(fix_stat);
    for (size_t i = 0; i < count; i++)
        free(alias[i].path);
    free(alias);
}

// The same collision in its other shape: two paths that are BOTH real host
// directories sharing one inode. Nothing is wedged here -- each agrees with its
// own host entry, so the pass above leaves them alone -- but (st_dev, st_ino)
// stops identifying a file, which is exactly the uniqueness cp, mv, rsync and
// install rely on to tell two files apart (see tests/manual/mount_cross_dev.c
// for the same invariant broken a different way). link(2) refuses to hard-link
// a directory, so two of them on one inode cannot have happened honestly; give
// every one after the first a stats row of its own. Regular files sharing an
// inode are left alone -- those are indistinguishable from ordinary hard links,
// which the roots are full of (/usr/bin/ar and friends).
#define MIGRATE_MAX_SHARERS 256

static void migrate_split_shared_dirs(struct fakefs_db *fs, int root_fd) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(fs->db,
                "select inode from paths group by inode having count(*) > 1", -1, &stmt, NULL) != SQLITE_OK) {
        printk("ERROR: fakefs migrate: %s\n", sqlite3_errmsg(fs->db));
        return;
    }
    // Collected up front: the repair rewrites paths, which this reads from.
    uint64_t *dup = NULL;
    size_t dup_count = 0, dup_cap = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (dup_count == dup_cap) {
            size_t cap = dup_cap == 0 ? 16 : dup_cap * 2;
            uint64_t *grown = realloc(dup, cap * sizeof(*grown));
            if (grown == NULL)
                break;
            dup = grown;
            dup_cap = cap;
        }
        dup[dup_count++] = (uint64_t) sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (dup_count == 0) {
        free(dup);
        return;
    }

    sqlite3_stmt *list = NULL, *get_stat = NULL, *add_stat = NULL, *repoint = NULL;
    unsigned split = 0;
    if (sqlite3_prepare_v2(fs->db, "select path from paths where inode = ? order by path", -1, &list, NULL) != SQLITE_OK ||
            sqlite3_prepare_v2(fs->db, "select stat from stats where inode = ?", -1, &get_stat, NULL) != SQLITE_OK ||
            sqlite3_prepare_v2(fs->db, "insert into stats (stat) values (?)", -1, &add_stat, NULL) != SQLITE_OK ||
            sqlite3_prepare_v2(fs->db, "update paths set inode = ? where path = ?", -1, &repoint, NULL) != SQLITE_OK) {
        printk("ERROR: fakefs migrate: %s\n", sqlite3_errmsg(fs->db));
        goto out;
    }

    for (size_t i = 0; i < dup_count; i++) {
        struct ish_stat ishstat;
        bool have_stat = false;
        sqlite3_bind_int64(get_stat, 1, (int64_t) dup[i]);
        if (sqlite3_step(get_stat) == SQLITE_ROW &&
                sqlite3_column_bytes(get_stat, 0) == sizeof(ishstat)) {
            memcpy(&ishstat, sqlite3_column_blob(get_stat, 0), sizeof(ishstat));
            have_stat = true;
        }
        sqlite3_reset(get_stat);
        if (!have_stat || !S_ISDIR(ishstat.mode))
            continue;

        char *shared[MIGRATE_MAX_SHARERS];
        size_t shared_count = 0;
        sqlite3_bind_int64(list, 1, (int64_t) dup[i]);
        while (sqlite3_step(list) == SQLITE_ROW && shared_count < MIGRATE_MAX_SHARERS) {
            const void *blob = sqlite3_column_blob(list, 0);
            int len = sqlite3_column_bytes(list, 0);
            if (blob == NULL || len <= 1 || len > MAX_PATH)
                continue;
            char path[MAX_PATH + 1];
            memcpy(path, blob, len);
            path[len] = '\0';
            if (path[0] != '/')
                continue;
            // Only entries the host really has as directories: an entry whose
            // host side is a file was the pass above's business, and by now it
            // is off this inode anyway.
            char host[MAX_PATH + 1];
            struct stat st;
            if (fake_path_to_host(path + 1, host, sizeof(host)) == NULL)
                continue;
            if (fstatat(root_fd, host, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(st.st_mode))
                continue;
            char *copy = strdup(path);
            if (copy == NULL)
                break;
            shared[shared_count++] = copy;
        }
        sqlite3_reset(list);

        // The first keeps the original row; the rest each get their own.
        for (size_t j = 1; j < shared_count; j++) {
            sqlite3_bind_blob(add_stat, 1, &ishstat, sizeof(ishstat), SQLITE_TRANSIENT);
            if (sqlite3_step(add_stat) != SQLITE_DONE) {
                printk("ERROR: fakefs migrate: %s\n", sqlite3_errmsg(fs->db));
                sqlite3_reset(add_stat);
                continue;
            }
            int64_t fresh = sqlite3_last_insert_rowid(fs->db);
            sqlite3_reset(add_stat);
            sqlite3_bind_int64(repoint, 1, fresh);
            sqlite3_bind_blob(repoint, 2, shared[j], strlen(shared[j]), SQLITE_TRANSIENT);
            sqlite3_step(repoint);
            sqlite3_reset(repoint);
            printk("fakefs migrate: %s shared inode %llu with %s; moved to %lld\n",
                    shared[j], (unsigned long long) dup[i], shared[0], (long long) fresh);
            split++;
        }
        for (size_t j = 0; j < shared_count; j++)
            free(shared[j]);
    }
    if (split != 0)
        printk("fakefs migrate: gave %u directories an inode of their own\n", split);

out:
    sqlite3_finalize(list);
    sqlite3_finalize(get_stat);
    sqlite3_finalize(add_stat);
    sqlite3_finalize(repoint);
    free(dup);
}

static void migrate_repair_inode_collisions(struct fakefs_db *fs, int root_fd) {
    // Order matters: retyping a path whose host entry is not a directory takes
    // it off the shared inode, so the sharer sweep below sees only the pairs
    // that are genuinely two directories.
    migrate_repair_type_alias(fs, root_fd);
    migrate_split_shared_dirs(fs, root_fd);
}

#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static struct migration {
    const char *sql;
    void (*migrate)(struct fakefs_db *fs, int root_fd);
} migrations[] = {
    // version 1: add another index
    {
        "create index inode_to_path on paths (inode, path);"
    },
    // version 2: add foreign key constraint on paths, create trigger to automatically cleanup stats
    {
        "create table paths_new (path blob primary key, inode integer references stats(inode));"
        "insert into paths_new select * from paths where exists (select 1 from stats where inode = paths.inode);"
        "drop table paths; alter table paths_new rename to paths;"
        "create index inode_to_path on paths (inode, path);"
        "delete from stats where not exists (select 1 from paths where inode = stats.inode);"
        "create trigger delete_path after delete on paths "
        "when not exists (select 1 from paths where inode = old.inode) "
        "begin "
            "delete from stats where not exists (select 1 from paths where inode = old.inode) and inode = old.inode; "
        "end;"
    },
    // version 3: the trigger was a mistake
    {
        "drop trigger delete_path"
    },
    // version 4: escape host file names (see the comment block at the top)
    {
        NULL, migrate_escape_host_names
    },
    // version 5: extend the escaping to non-ASCII bytes
    {
        NULL, migrate_reescape_unicode
    },
    // version 6: recover what v4/v5 left stranded inside a renamed twin
    {
        NULL, migrate_repair_orphans
    },
    // version 7: unpick the inode aliasing left by concurrent allocators
    {
        NULL, migrate_repair_inode_collisions
    },
};

int fakefs_migrate(struct fakefs_db *fs, int root_fd) {
    sqlite3 *db = fs->db;
    int err;
    sqlite3_stmt *user_version = PREPARE_RET("pragma user_version");
    STEP_RET(user_version);
    int version = sqlite3_column_int(user_version, 0);
    FINALIZE_RET(user_version);

    EXEC_RET("begin");
    int versions = sizeof(migrations)/sizeof(migrations[0]);
    while (version < versions) {
        struct migration m = migrations[version];
        if (m.sql != NULL)
            EXEC_RET(m.sql);
        if (m.migrate != NULL)
            m.migrate(fs, root_fd);
        version++;
    }
    // for some reason placeholders aren't allowed in pragmas
    char *pragma_user_version = sqlite3_mprintf("pragma user_version = %d", version);
    EXEC_RET(pragma_user_version);
    sqlite3_free(pragma_user_version);
    EXEC_RET("commit");

    return 0;
}
