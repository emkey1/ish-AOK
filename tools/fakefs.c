#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <sys/stat.h>
#include <archive.h>
#include <archive_entry.h>

#define ISH_INTERNAL
#include "fs/fake-db.h"
#include "fs/fake-path.h"
#include "fs/sqlutil.h"
#include "tools/fakefs.h"
#include "misc.h"

#ifndef MAX_PATH
#define MAX_PATH 4096
#endif

// iSH's fake dev_t encoding (fs/dev.h's dev_make/dev_major/dev_minor,
// mmmMMMmm hex layout). Duplicated here rather than including fs/dev.h
// because that header pulls in fs/fd.h -> emu/memory.h, which this host
// tool doesn't link against. Keep in sync with fs/dev.h.
//
// archive_entry_rdev()/archive_entry_set_rdev() round-trip through the
// *host's* dev_t encoding (e.g. Darwin's major<<24|minor), which is not
// the on-disk fakefs format -- use archive_entry_rdevmajor/rdevminor
// instead, which libarchive decodes portably from the tar header.
static uint32_t fakefs_dev_make(int major, int minor) {
    return ((minor & 0xfff00) << 12) | (major << 8) | (minor & 0xff);
}
static int fakefs_dev_major(uint32_t dev) {
    return (dev & 0xfff00) >> 8;
}
static int fakefs_dev_minor(uint32_t dev) {
    return ((dev & 0xfff00000) >> 12) | (dev & 0xff);
}

// I have a weird way of error handling
#define FILL_ERR(_type, _code, _message) do { \
    err_out->line = __LINE__; \
    err_out->type = _type; \
    err_out->code = _code; \
    err_out->message = strdup(_message); \
    return false; \
} while (0)
#define ARCHIVE_ERR(archive) FILL_ERR(ERR_ARCHIVE, archive_errno(archive), archive_error_string(archive))
#define POSIX_ERR() FILL_ERR(ERR_POSIX, errno, strerror(errno))
#undef HANDLE_ERR // for sqlite
#define HANDLE_ERR(db) FILL_ERR(ERR_SQLITE, sqlite3_extended_errcode(db), sqlite3_errmsg(db))
#define CANCEL() FILL_ERR(ERR_CANCELLED, 0, "");

static bool progress_update(struct progress *p, double progress, const char *message) {
    bool cancelled = false;
    if (p && p->callback)
        p->callback(p->cookie, progress, message, &cancelled);
    return !cancelled;
}

// This isn't linked with ish which is why there's so much copy/pasted code

// I hate this code
static bool path_normalize(const char *path, char *out) {
#define ends_path(c) (c == '\0' || c == '/')
    // normalized format:
    // ( '/' path-component ) *
    while (path[0] != '\0') {
        while (path[0] == '/')
            path++;
        if (path[0] == '\0')
            break; // if the path ends with a slash
        // path points to the start of a path component
        if (path[0] == '.' && path[1] == '.' && ends_path(path[2]))
            return false; // no dotdot allowed!
        if (path[0] == '.' && ends_path(path[1])) {
            path++;
        } else {
            *out++ = '/';
            while (path[0] != '/' && path[0] != '\0')
                *out++ = *path++;
        }
    }
    *out = '\0';
    return true;
}

static const char *schema = Q(
    create table meta (id integer unique default 0, db_inode integer);
    insert into meta (db_inode) values (0);
    create table stats (inode integer primary key, stat blob);
    create table paths (path blob primary key, inode integer references stats(inode));
    create index inode_to_path on paths (inode, path);
    // no index is needed on stats, because the rows are ordered by the primary key
    // version 5: host names are stored in escaped form (fs/fake-path.h),
    // including non-ASCII bytes; fakefs_import writes them that way, so no
    // migration is needed
    pragma user_version=5;
);

// Host file names are the escaped on-disk form of the guest names (see
// fs/fake-path.h), so guest names that a case- and normalization-insensitive
// host filesystem considers equal stay distinct. Escaping at most triples
// the length.
typedef char host_path_buf[MAX_PATH * 3 + 2];

void fakefs_ensure_utf8_locale(void) {
    // Guest names are opaque byte strings, so archive path/linkpath strings
    // must pass through byte-exact. Under a UTF-8 LC_CTYPE, libarchive
    // *converts* pax UTF-8 names to the locale charset, and its Apple build
    // normalizes that conversion through "UTF-8-MAC" (NFD, for HFS+ compat)
    // -- collapsing names that differ only in Unicode normalization before
    // we ever see them, exactly the collision fs/fake-path.h escaping exists
    // to prevent (observed: NFC "café" and its NFD twin imported as one
    // file). Under the "C" locale the conversion instead fails cleanly with
    // ARCHIVE_WARN (which the read loop tolerates) and the narrow accessors
    // return the archive's raw bytes untouched; on write, the pax writer
    // falls back to an hdrcharset=BINARY record and stores raw bytes. Only
    // LC_CTYPE is pinned, to avoid disturbing numeric/time formatting
    // categories used elsewhere in the process. (The name is historical:
    // this used to pin a UTF-8 locale, which predates the escaping scheme.)
    setlocale(LC_CTYPE, "C");
}

// Prefer the narrow accessors: under the "C" locale pinned above they hand
// back the archive's raw name bytes untouched, which is what fakefs stores
// (guest names are opaque byte strings). The *_utf8 accessors are only a
// fallback -- their conversion path can pass through Apple's "UTF-8-MAC"
// (NFD) and normalize the name.
static const char *entry_pathname_u8(struct archive_entry *e) {
    const char *p = archive_entry_pathname(e);
    return p != NULL ? p : archive_entry_pathname_utf8(e);
}
static const char *entry_hardlink_u8(struct archive_entry *e) {
    const char *p = archive_entry_hardlink(e);
    return p != NULL ? p : archive_entry_hardlink_utf8(e);
}
static const char *entry_symlink_u8(struct archive_entry *e) {
    const char *p = archive_entry_symlink(e);
    return p != NULL ? p : archive_entry_symlink_utf8(e);
}

bool fakefs_import(const char *archive_path, const char *fs, struct fakefsify_error *err_out, struct progress p) {
    fakefs_ensure_utf8_locale();
    int err = mkdir(fs, 0777);
    if (err < 0)
        POSIX_ERR();

    // make the data root dir
    char path_tmp[PATH_MAX];
    snprintf(path_tmp, sizeof(path_tmp), "%s/data", fs);
    err = mkdir(path_tmp, 0777);
    int root_fd = open(path_tmp, O_RDONLY);
    if (root_fd < 0)
        POSIX_ERR();

    // open the database
    snprintf(path_tmp, sizeof(path_tmp), "%s/meta.db", fs);
    sqlite3 *db;
    err = sqlite3_open_v2(path_tmp, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    CHECK_ERR();
    EXEC("pragma journal_mode=wal")
    EXEC("begin");
    EXEC(schema);

    // open the archive
    struct archive *archive = archive_read_new();
    if (archive == NULL)
        ARCHIVE_ERR(archive);
    archive_read_support_filter_all(archive); // gzip, bzip2, xz, zstd, ... (whatever libarchive was built with)
    archive_read_support_format_tar(archive);
    if (archive_read_open_filename(archive, archive_path, 65536) != ARCHIVE_OK)
        ARCHIVE_ERR(archive);

    struct stat real_stat;
    if (stat(archive_path, &real_stat) < 0)
        POSIX_ERR();
    size_t archive_bytes = real_stat.st_size;

    sqlite3_stmt *insert_stat = PREPARE("insert into stats (stat) values (?)");
    sqlite3_stmt *insert_path = PREPARE("insert or replace into paths values (?, ?)");
    sqlite3_stmt *insert_hardlink = PREPARE("insert or replace into paths values (?, (select inode from paths where path = ? limit 1))");

    bool archive_has_root = false;

    // do actual shit
    struct archive_entry *entry;
    while (true) {
        err = archive_read_next_header(archive, &entry);
        if (err == ARCHIVE_EOF)
            break;
        // ARCHIVE_WARN is non-fatal: libarchive returns it when e.g. a pax/UTF-8
        // link path can't also be rendered in the process locale. The UTF-8 form
        // is still available via the *_utf8 accessors, so keep importing.
        if (err != ARCHIVE_OK && err != ARCHIVE_WARN)
            ARCHIVE_ERR(archive);
        char entry_path[MAX_PATH];
        if (!path_normalize(entry_pathname_u8(entry), entry_path)) {
            // Avoid pwnage
            fprintf(stderr, "warning: skipped possible path traversal %s\n", entry_pathname_u8(entry));
            continue;
        }
        if (!progress_update(&p, (double) archive_filter_bytes(archive, -1) / archive_bytes, entry_path))
            CANCEL();
        if (entry_path[0] == '\0')
            archive_has_root = true;

        host_path_buf host_entry_path;
        if (fake_path_to_host(entry_path, host_entry_path, sizeof(host_entry_path)) == NULL)
            FILL_ERR(ERR_POSIX, ENAMETOOLONG, "escaped path too long");

        const char *hardlink = entry_hardlink_u8(entry);
        if (hardlink) {
            char hardlink_path[MAX_PATH];
            if (!path_normalize(hardlink, hardlink_path)) {
                fprintf(stderr, "warning: almost pwned by hardlink %s\n", hardlink);
                continue;
            }
            host_path_buf host_hardlink_path;
            if (fake_path_to_host(hardlink_path, host_hardlink_path, sizeof(host_hardlink_path)) == NULL)
                FILL_ERR(ERR_POSIX, ENAMETOOLONG, "escaped path too long");
            if (linkat(root_fd, fix_path(host_hardlink_path), root_fd, fix_path(host_entry_path), 0) < 0)
                POSIX_ERR();
            sqlite3_bind_blob64(insert_hardlink, 1, entry_path, strlen(entry_path), SQLITE_TRANSIENT);
            sqlite3_bind_blob64(insert_hardlink, 2, hardlink_path, strlen(hardlink_path), SQLITE_TRANSIENT);
            STEP_RESET(insert_hardlink);
            continue;
        }

        // mkdir -p (on the escaped host path; '/' is never escaped, so the
        // component boundaries are the same as in the guest path)
        char *host_path_copy = strdup(host_entry_path);
        char *slash = host_path_copy;
        while ((slash = strchr(*slash ? slash + 1 : slash, '/')) != NULL) {
            *slash = '\0';
            int err = mkdirat(root_fd, fix_path(host_path_copy), 0777);
            *slash = '/';
            if (err < 0) {
                if (errno == EEXIST) continue;
                fprintf(stderr, "fakefs_import: mkdirat failed for '%s': %s\n",
                        fix_path(host_path_copy), strerror(errno));
                POSIX_ERR();
            }
        }
        free(host_path_copy);

        int fd = -1;
        if (archive_entry_filetype(entry) != AE_IFDIR) {
            fd = openat(root_fd, fix_path(host_entry_path), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd < 0) {
                if (errno == EISDIR) continue; // assuming it's case insensitivity
                // Debug: check parent directory
                char dbg_parent[MAX_PATH];
                strlcpy(dbg_parent, fix_path(host_entry_path), sizeof(dbg_parent));
                char *dbg_slash = strrchr(dbg_parent, '/');
                if (dbg_slash) {
                    *dbg_slash = '\0';
                    struct stat dbg_st;
                    int dbg_res = fstatat(root_fd, dbg_parent, &dbg_st, AT_SYMLINK_NOFOLLOW);
                    fprintf(stderr, "fakefs_import: openat failed for '%s': %s; parent='%s' fstatat=%d mode=%06o\n",
                            fix_path(host_entry_path), strerror(errno),
                            dbg_parent, dbg_res, dbg_res == 0 ? dbg_st.st_mode : 0);
                } else {
                    fprintf(stderr, "fakefs_import: openat failed for '%s': %s (no parent)\n",
                            fix_path(host_entry_path), strerror(errno));
                }
                POSIX_ERR();
            }
        }

        switch (archive_entry_filetype(entry)) {
            case AE_IFDIR:
                err = mkdirat(root_fd, fix_path(host_entry_path), 0777);
                if (err < 0 && errno != EEXIST)
                    POSIX_ERR();
                break;
            case AE_IFREG:
                if (archive_read_data_into_fd(archive, fd) != ARCHIVE_OK)
                    ARCHIVE_ERR(archive);
                break;
            case AE_IFLNK:
                err = (int) write(fd, entry_symlink_u8(entry), strlen(entry_symlink_u8(entry)));
                if (err < 0)
                    POSIX_ERR();
        }
        if (fd != -1)
            close(fd);

        struct timespec times[2] = {
            // for utimes, atime is first, mtime is second
            {.tv_sec = archive_entry_atime(entry), .tv_nsec = archive_entry_atime_nsec(entry)},
            {.tv_sec = archive_entry_mtime(entry), .tv_nsec = archive_entry_mtime_nsec(entry)},
            // utimes cannot set ctime
        };
        if (!archive_entry_atime_is_set(entry))
            times[0].tv_nsec = UTIME_OMIT;
        if (!archive_entry_mtime_is_set(entry))
            times[1].tv_nsec = UTIME_OMIT;
        err = utimensat(root_fd, fix_path(host_entry_path), times, 0);
        if (err < 0 && errno != ENOENT && errno != EPERM)
            POSIX_ERR();

        struct ish_stat stat = {
            .mode = (uint32_t) archive_entry_mode(entry),
            .uid = (uint32_t) archive_entry_uid(entry),
            .gid = (uint32_t) archive_entry_gid(entry),
            .rdev = fakefs_dev_make((int) archive_entry_rdevmajor(entry), (int) archive_entry_rdevminor(entry)),
        };
        sqlite3_bind_blob64(insert_stat, 1, &stat, sizeof(stat), SQLITE_TRANSIENT);
        STEP_RESET(insert_stat);
        sqlite3_bind_blob64(insert_path, 1, entry_path, strlen(entry_path), SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert_path, 2, sqlite3_last_insert_rowid(db));
        STEP_RESET(insert_path);
    }
    // (EOF reached normally above; ARCHIVE_WARN entries were imported and hard
    // errors already returned, so no trailing status check is needed.)

    // Add a path entry for the root if it's missing. Every other row's mode
    // comes from archive_entry_mode(), which libarchive populates with the
    // S_IFMT type bits from the tar header; this synthetic fallback must OR
    // S_IFDIR in explicitly or the root inode ends up untyped (bare 0755,
    // S_ISDIR() false), which among other things makes dpkg see "/" as a
    // "weird file" and misfire "trying to overwrite '/.'" on the first
    // package whose data.tar includes a "./" entry (nearly all of them).
    if (!archive_has_root) {
        struct ish_stat stat = {.mode = S_IFDIR | 0755};
        sqlite3_bind_blob64(insert_stat, 1, &stat, sizeof(stat), SQLITE_TRANSIENT);
        STEP_RESET(insert_stat);
        sqlite3_bind_blob64(insert_path, 1, "", 0, SQLITE_TRANSIENT);
        sqlite3_bind_int64(insert_path, 2, sqlite3_last_insert_rowid(db));
        STEP_RESET(insert_path);
    }

    FINALIZE(insert_stat);
    FINALIZE(insert_path);
    FINALIZE(insert_hardlink);
    EXEC("commit");
    sqlite3_close(db);
    close(root_fd);

    if (archive_read_free(archive) != ARCHIVE_OK)
        ARCHIVE_ERR(archive);
    return true;
}

// Mirrors fakefs_import's setup (data dir + meta.db + schema + synthetic
// root inode), minus the archive walk. The root inode is 01777 rather than
// import's 0755 fallback: an empty shared filesystem's whole purpose is for
// arbitrary guest uids to create things in it.
bool fakefs_init_empty(const char *fs, struct fakefsify_error *err_out) {
    int err = mkdir(fs, 0777);
    if (err < 0)
        POSIX_ERR();

    char path_tmp[PATH_MAX];
    snprintf(path_tmp, sizeof(path_tmp), "%s/data", fs);
    err = mkdir(path_tmp, 0777);
    if (err < 0)
        POSIX_ERR();

    snprintf(path_tmp, sizeof(path_tmp), "%s/meta.db", fs);
    sqlite3 *db;
    err = sqlite3_open_v2(path_tmp, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    CHECK_ERR();
    EXEC("pragma journal_mode=wal")
    EXEC("begin");
    EXEC(schema);

    sqlite3_stmt *insert_stat = PREPARE("insert into stats (stat) values (?)");
    sqlite3_stmt *insert_path = PREPARE("insert or replace into paths values (?, ?)");
    struct ish_stat stat = {.mode = S_IFDIR | 01777};
    sqlite3_bind_blob64(insert_stat, 1, &stat, sizeof(stat), SQLITE_TRANSIENT);
    STEP_RESET(insert_stat);
    sqlite3_bind_blob64(insert_path, 1, "", 0, SQLITE_TRANSIENT);
    sqlite3_bind_int64(insert_path, 2, sqlite3_last_insert_rowid(db));
    STEP_RESET(insert_path);
    FINALIZE(insert_stat);
    FINALIZE(insert_path);
    EXEC("commit");
    sqlite3_close(db);
    return true;
}

bool fakefs_export(const char *fs, const char *archive_path, struct fakefsify_error *err_out, struct progress p) {
    fakefs_ensure_utf8_locale();
    // open the archive
    struct archive *archive = archive_write_new();
    if (archive == NULL)
        ARCHIVE_ERR(archive);
    archive_write_add_filter_gzip(archive);
    archive_write_set_format_pax(archive);
    // Mirror the import side: store guest names as raw bytes rather than
    // letting the Apple build normalize them through "UTF-8-MAC" (NFD).
    archive_write_set_options(archive, "hdrcharset=BINARY");
    if (archive_write_open_filename(archive, archive_path) != ARCHIVE_OK)
        ARCHIVE_ERR(archive);

    // open the data root dir
    char path_tmp[PATH_MAX];
    snprintf(path_tmp, sizeof(path_tmp), "%s/data", fs);
    int root_fd = open(path_tmp, O_RDONLY);
    if (root_fd < 0)
        POSIX_ERR();

    // open the database
    snprintf(path_tmp, sizeof(path_tmp), "%s/meta.db", fs);
    sqlite3 *db;
    int err = sqlite3_open_v2(path_tmp, &db, SQLITE_OPEN_READONLY, NULL);
    CHECK_ERR();
    EXEC("begin");

    sqlite3_stmt *count_stmt = PREPARE("select count(path) from paths");
    STEP(count_stmt);
    int64_t paths_total = sqlite3_column_int64(count_stmt, 0);
    FINALIZE(count_stmt);
    int64_t paths_done = 0;

    struct archive_entry_linkresolver *linkresolver = archive_entry_linkresolver_new();
    archive_entry_linkresolver_set_strategy(linkresolver, ARCHIVE_FORMAT_TAR_PAX_INTERCHANGE);

    sqlite3_stmt *query = PREPARE("select path, inode, stat from paths, stats using (inode)");
    while (STEP(query)) {
        struct archive_entry *entry = archive_entry_new();

        const char *path_in_db = sqlite3_column_blob(query, 0);
        size_t path_len = sqlite3_column_bytes(query, 0);
        char *path = malloc(path_len + 2);
        path[0] = '.';
        memcpy(path + 1, path_in_db, path_len);
        path[path_len + 1] = '\0';
        // the archive gets the guest path; the host filesystem is accessed
        // via the escaped on-disk form (fs/fake-path.h)
        host_path_buf host_path;
        if (fake_path_to_host(path, host_path, sizeof(host_path)) == NULL)
            FILL_ERR(ERR_POSIX, ENAMETOOLONG, "escaped path too long");
        archive_entry_set_pathname(entry, path);

        if (!progress_update(&p, (double) paths_done / paths_total, path))
            CANCEL();

        archive_entry_set_ino64(entry, sqlite3_column_int64(query, 1));
        struct ish_stat stat = *(struct ish_stat *) sqlite3_column_blob(query, 2);
        archive_entry_set_mode(entry, stat.mode);
        archive_entry_set_uid(entry, stat.uid);
        archive_entry_set_gid(entry, stat.gid);
        archive_entry_set_rdevmajor(entry, fakefs_dev_major(stat.rdev));
        archive_entry_set_rdevminor(entry, fakefs_dev_minor(stat.rdev));

        struct stat real_stat;
        if (fstatat(root_fd, host_path, &real_stat, 0) < 0) {
            if (errno == ENOENT) {
                printf("skipping %s\n", path);
                goto skip;
            }
            POSIX_ERR();
        }
        archive_entry_set_size(entry, real_stat.st_size);
#if __APPLE__
#define TIMESPEC(x) st_##x##timespec
#elif __linux__
#define TIMESPEC(x) st_##x##tim
#endif
        archive_entry_set_atime(entry, real_stat.st_atime, real_stat.TIMESPEC(a).tv_nsec);
        archive_entry_set_mtime(entry, real_stat.st_mtime, real_stat.TIMESPEC(m).tv_nsec);
        archive_entry_set_ctime(entry, real_stat.st_ctime, real_stat.TIMESPEC(c).tv_nsec);

        int fd = -1;
        S_IFMT;
        if (S_ISREG(stat.mode) || S_ISLNK(stat.mode))
            fd = openat(root_fd, host_path, O_RDONLY);
        if (S_ISLNK(stat.mode)) {
            char buf[MAX_PATH+1];
            ssize_t len = read(fd, buf, sizeof(buf)-1);
            if (len < 0)
                POSIX_ERR();
            buf[len] = '\0';
            archive_entry_set_symlink(entry, buf);
        }

        struct archive_entry *sparse;
        archive_entry_linkify(linkresolver, &entry, &sparse);
        if (entry != NULL)
            archive_write_header(archive, entry);
        if (sparse != NULL)
            archive_write_header(archive, sparse);

        if (S_ISREG(stat.mode) && archive_entry_size(entry) != 0) {
            char buf[8192];
            ssize_t len;
            while ((len = read(fd, buf, sizeof(buf))) > 0) {
                ssize_t written = archive_write_data(archive, buf, len);
                if (written < 0)
                    ARCHIVE_ERR(archive);
                if (written != len)
                    printf("uh oh\n");
            }
            if (len < 0)
                POSIX_ERR();
        }
        if (fd != -1)
            close(fd);

    skip:
        paths_done++;
        free(path);
        archive_entry_free(entry);
    }

    FINALIZE(query);
    archive_entry_linkresolver_free(linkresolver);
    sqlite3_close(db);
    close(root_fd);
    if (archive_write_close(archive) != ARCHIVE_OK)
        ARCHIVE_ERR(archive);
    archive_write_free(archive);
    return true;
}
