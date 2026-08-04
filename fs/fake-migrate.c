#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
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
};

static void dir_cache_reset(struct migrate_dir_cache *cache) {
    for (size_t i = 0; i < cache->count; i++)
        free(cache->names[i]);
    free(cache->names);
    cache->names = NULL;
    cache->count = cache->cap = 0;
    cache->loaded = false;
    cache->complete = false;
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
