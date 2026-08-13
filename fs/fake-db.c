#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include "kernel/errno.h"
#include "debug.h"
#include "misc.h"
#include "fs/fake-db.h"
#include "fs/sqlutil.h"

static void db_check_error(struct fakefs_db *fs) {
    int errcode = sqlite3_errcode(fs->db);
    switch (errcode) {
        case SQLITE_OK:
        case SQLITE_ROW:
        case SQLITE_DONE:
            break;

        default:
            printk("ERROR: sqlite error: %s.  Probable filesystem corruption.  Backup and reinstall.", sqlite3_errmsg(fs->db));
                         //die("sqlite error: %s", sqlite3_errmsg(fs->db));
    }
}

static sqlite3_stmt *db_prepare(struct fakefs_db *fs, const char *stmt) {
    sqlite3_stmt *statement;
    sqlite3_prepare_v2(fs->db, stmt, strlen(stmt) + 1, &statement, NULL);
    db_check_error(fs);
    return statement;
}

bool db_exec(struct fakefs_db *fs, sqlite3_stmt *stmt) {
    int err = sqlite3_step(stmt);
    db_check_error(fs);
    return err == SQLITE_ROW;
}
void db_reset(struct fakefs_db *fs, sqlite3_stmt *stmt) {
    sqlite3_reset(stmt);
    db_check_error(fs);
}
void db_exec_reset(struct fakefs_db *fs, sqlite3_stmt *stmt) {
    db_exec(fs, stmt);
    db_reset(fs, stmt);
}

// Suspension quiesce gate; see the contract in fake-db.h. Deliberately raw
// pthread primitives rather than the emulator's lock_t/cond_t: the quiescing
// side runs on the host UI thread, where `current` is NULL and the
// task-aware interruptible waits do not apply.
static pthread_mutex_t quiesce_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t quiesce_cond = PTHREAD_COND_INITIALIZER;
static bool quiesce_requested = false;
static unsigned transactions_in_flight = 0;

// Must be called before taking fs->lock, so a task parked here owns nothing.
static void transaction_enter(void) {
    pthread_mutex_lock(&quiesce_mutex);
    while (quiesce_requested)
        pthread_cond_wait(&quiesce_cond, &quiesce_mutex);
    transactions_in_flight++;
    pthread_mutex_unlock(&quiesce_mutex);
}

static void transaction_leave(void) {
    pthread_mutex_lock(&quiesce_mutex);
    if (transactions_in_flight > 0)
        transactions_in_flight--;
    if (transactions_in_flight == 0)
        pthread_cond_broadcast(&quiesce_cond);
    pthread_mutex_unlock(&quiesce_mutex);
}

bool fakefs_quiesce_begin(unsigned timeout_ms, unsigned *still_in_flight) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&quiesce_mutex);
    quiesce_requested = true;
    int err = 0;
    while (transactions_in_flight > 0 && err == 0)
        err = pthread_cond_timedwait(&quiesce_cond, &quiesce_mutex, &deadline);
    unsigned remaining = transactions_in_flight;
    pthread_mutex_unlock(&quiesce_mutex);

    if (still_in_flight != NULL)
        *still_in_flight = remaining;
    // A straggler is possible and must not hang the caller: a transaction can
    // stay open across a blocking host operation (opening a FIFO with no peer,
    // say), and the caller is running out its own suspension deadline.
    return remaining == 0;
}

void fakefs_quiesce_end(void) {
    pthread_mutex_lock(&quiesce_mutex);
    quiesce_requested = false;
    pthread_cond_broadcast(&quiesce_cond);
    pthread_mutex_unlock(&quiesce_mutex);
}

void db_begin_read(struct fakefs_db *fs) {
    transaction_enter();
    sqlite3_mutex_enter(fs->lock);
    db_exec_reset(fs, fs->stmt.begin_deferred);
}

void db_begin_write(struct fakefs_db *fs) {
    transaction_enter();
    sqlite3_mutex_enter(fs->lock);
    db_exec_reset(fs, fs->stmt.begin_immediate);
}

void db_commit(struct fakefs_db *fs) {
    db_exec_reset(fs, fs->stmt.commit);
    sqlite3_mutex_leave(fs->lock);
    transaction_leave();
}
void db_rollback(struct fakefs_db *fs) {
    db_exec_reset(fs, fs->stmt.rollback);
    sqlite3_mutex_leave(fs->lock);
    transaction_leave();
}

static void bind_path(sqlite3_stmt *stmt, int i, const char *path) {
    sqlite3_bind_blob(stmt, i, path, strlen(path), SQLITE_TRANSIENT);
}

inode_t path_get_inode(struct fakefs_db *fs, const char *path) {
    // select inode from paths where path = ?
    bind_path(fs->stmt.path_get_inode, 1, path);
    inode_t inode = 0;
    if (db_exec(fs, fs->stmt.path_get_inode))
        inode = sqlite3_column_int64(fs->stmt.path_get_inode, 0);
    db_reset(fs, fs->stmt.path_get_inode);
    return inode;
}
bool path_read_stat(struct fakefs_db *fs, const char *path, struct ish_stat *stat, inode_t *inode) {
    // select inode, stat from stats natural join paths where path = ?
    bind_path(fs->stmt.path_read_stat, 1, path);
    bool exists = db_exec(fs, fs->stmt.path_read_stat);
    if (exists) {
        if (inode)
            *inode = sqlite3_column_int64(fs->stmt.path_read_stat, 0);
        if (stat)
            *stat = *(struct ish_stat *) sqlite3_column_blob(fs->stmt.path_read_stat, 1);
    }
    db_reset(fs, fs->stmt.path_read_stat);
    return exists;
}
static inode_t fakefs_next_inode_init(struct fakefs_db *fs);

// Returns 0 if no inode could be claimed, in which case nothing was written and
// the caller must fail the operation rather than commit a half-made file.
//
// The stats insert is a plain `insert`, so it fails with SQLITE_CONSTRAINT when
// the inode is already taken -- and db_check_error only printk()s, so that used
// to pass unnoticed: the `insert or replace into paths` below it ran anyway and
// bound the new path to a *pre-existing* inode belonging to some other file.
// The new path then reads that file's stat blob. When the blob says S_IFDIR and
// the new host entry is an ordinary file, the result cannot be removed by
// anything: rmdir(2) gets the host's ENOTDIR, unlink(2) the metadata's EISDIR.
// Seen in the field as a gcc temporary, /tmp/ccKNBeAg.o, wearing the mode of
// the directory /rbind, which aborted every in-guest build that reused the name.
//
// next_inode is a per-fakefs_db counter seeded once from max(stats.inode) and
// then kept in memory, so a second allocator on the same meta.db -- another ish
// process on the same root, or that root mounted twice -- hands out the very
// numbers this one is working through. Take the constraint failure as the
// signal it is: re-seed from the database and try again.
inode_t path_create(struct fakefs_db *fs, const char *path, struct ish_stat *stat) {
    inode_t inode = 0;
    bool created = false;
    // Each retry re-seeds past every inode in the table, so it can only lose
    // again to a writer that committed in between; a couple of rounds is
    // already generous.
    for (int attempt = 0; attempt < 8; attempt++) {
        inode = fs->next_inode++;
        // insert into stats (inode, stat) values (?, ?)
        sqlite3_bind_int64(fs->stmt.path_create_stat, 1, inode);
        sqlite3_bind_blob(fs->stmt.path_create_stat, 2, stat, sizeof(*stat), SQLITE_TRANSIENT);
        int err = sqlite3_step(fs->stmt.path_create_stat);
        sqlite3_reset(fs->stmt.path_create_stat);
        if (err == SQLITE_DONE) {
            created = true;
            break;
        }
        // Extended result codes are off by default, but mask anyway so a build
        // that turns them on still recognizes SQLITE_CONSTRAINT_PRIMARYKEY.
        if ((err & 0xff) != SQLITE_CONSTRAINT) {
            db_check_error(fs);
            return 0;
        }
        fs->next_inode = fakefs_next_inode_init(fs);
    }
    if (!created) {
        printk("ERROR: fakefs path_create(%s): could not claim a free inode\n", path);
        return 0;
    }
    // insert or replace into paths values (?, ?)
    bind_path(fs->stmt.path_create_path, 1, path);
    sqlite3_bind_int64(fs->stmt.path_create_path, 2, inode);
    db_exec_reset(fs, fs->stmt.path_create_path);
    return inode;
}

bool inode_exists(struct fakefs_db *fs, inode_t inode) {
    sqlite3_mutex_enter(fs->lock);
    sqlite3_bind_int64(fs->stmt.inode_read_stat, 1, inode);
    bool exists = db_exec(fs, fs->stmt.inode_read_stat);
    db_reset(fs, fs->stmt.inode_read_stat);
    sqlite3_mutex_leave(fs->lock);
    return exists;
}

bool inode_read_stat(struct fakefs_db *fs, inode_t inode, struct ish_stat *stat) {
    // select stat from stats where inode = ?
    sqlite3_bind_int64(fs->stmt.inode_read_stat, 1, inode);
    if (!db_exec(fs, fs->stmt.inode_read_stat)) {
        db_reset(fs, fs->stmt.inode_read_stat);
        return false;
    }
    *stat = *(struct ish_stat *) sqlite3_column_blob(fs->stmt.inode_read_stat, 0);
    db_reset(fs, fs->stmt.inode_read_stat);
    return true;
}
void inode_write_stat(struct fakefs_db *fs, inode_t inode, struct ish_stat *stat) {
    // update stats set stat = ? where inode = ?
    sqlite3_bind_blob(fs->stmt.inode_write_stat, 1, stat, sizeof(*stat), SQLITE_TRANSIENT);
    sqlite3_bind_int64(fs->stmt.inode_write_stat, 2, inode);
    db_exec_reset(fs, fs->stmt.inode_write_stat);
}

void path_link(struct fakefs_db *fs, const char *src, const char *dst) {
    inode_t inode = path_get_inode(fs, src);
    if (inode == 0)
        die("fakefs link(%s, %s): nonexistent src path", src, dst);
    // insert or replace into paths (path, inode) values (?, ?)
    bind_path(fs->stmt.path_link, 1, dst);
    sqlite3_bind_int64(fs->stmt.path_link, 2, inode);
    db_exec_reset(fs, fs->stmt.path_link);
}
inode_t path_unlink(struct fakefs_db *fs, const char *path) {
    inode_t inode = path_get_inode(fs, path);
    if (inode == 0) {
//        die("path_unlink(%s): nonexistent path", path);
        printk("ERROR: path_unlink(%s), nonexistent path\n", path);
        return(0);
    }
    // delete from paths where path = ?
    bind_path(fs->stmt.path_unlink, 1, path);
    db_exec_reset(fs, fs->stmt.path_unlink);
    return inode;
}
void path_rename(struct fakefs_db *fs, const char *src, const char *dst) {
    // update or replace paths set path = change_prefix(path, ? [len(src)], ? [dst])
    //  where (path >= ? [src plus /] and path < [src plus 0]) or path = ? [src]
    // arguments:
    // 1. length of src
    // 2. dst
    // 3. src plus /
    // 4. src plus 0
    // 5. src
    size_t src_len = strlen(src);
    sqlite3_bind_int64(fs->stmt.path_rename, 1, src_len);
    bind_path(fs->stmt.path_rename, 2, dst);
    char src_extra[src_len + 1];
    memcpy(src_extra, src, src_len);
    src_extra[src_len] = '/';
    sqlite3_bind_blob(fs->stmt.path_rename, 3, src_extra, src_len + 1, SQLITE_TRANSIENT);
    src_extra[src_len] = '0';
    sqlite3_bind_blob(fs->stmt.path_rename, 4, src_extra, src_len + 1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(fs->stmt.path_rename, 5, src_extra, src_len, SQLITE_TRANSIENT);
    db_exec_reset(fs, fs->stmt.path_rename);
}

#if DEBUG_sql
static int trace_callback(unsigned UNUSED(why), void *UNUSED(fuck), void *stmt, void *_sql) {
    char *sql = _sql;
    printk("WARNING: %d sql trace: %s %s\n", current ? current->pid : -1, sqlite3_expanded_sql(stmt), sql[0] == '-' ? sql : "");
    return 0;
}
#endif

static void sqlite_func_change_prefix(sqlite3_context *context, int argc, sqlite3_value **args) {
    assert(argc == 3);
    const void *in_blob = sqlite3_value_blob(args[0]);
    size_t in_size = sqlite3_value_bytes(args[0]);
    size_t start = sqlite3_value_int64(args[1]);
    const void *replacement = sqlite3_value_blob(args[2]);
    size_t replacement_size = sqlite3_value_bytes(args[2]);
    size_t out_size = in_size - start + replacement_size;
    char *out_blob = sqlite3_malloc(out_size);
    memcpy(out_blob, replacement, replacement_size);
    memcpy(out_blob + replacement_size, in_blob + start, in_size - start);
    sqlite3_result_blob(context, out_blob, out_size, sqlite3_free);
}

static bool fakefs_inode_namespace_needs_compaction(struct fakefs_db *fs) {
    sqlite3_stmt *statement = db_prepare(fs, "select max(inode) from stats");
    bool needs_compaction = false;
    if (sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_type(statement, 0) != SQLITE_NULL) {
        uint64_t max_inode = (uint64_t) sqlite3_column_int64(statement, 0);
        needs_compaction = max_inode > UINT32_MAX;
    }
    sqlite3_finalize(statement);
    return needs_compaction;
}

static inode_t fakefs_next_inode_init(struct fakefs_db *fs) {
    sqlite3_stmt *statement = db_prepare(fs, "select coalesce(max(inode), 0) from stats");
    inode_t next_inode = 1;
    if (sqlite3_step(statement) == SQLITE_ROW)
        next_inode = (inode_t) sqlite3_column_int64(statement, 0) + 1;
    sqlite3_finalize(statement);
    if (next_inode == 0 || next_inode > UINT32_MAX)
        next_inode = 1;
    return next_inode;
}

extern int fakefs_rebuild(struct fakefs_db *fs, int root_fd);
extern int fakefs_migrate(struct fakefs_db *fs, int root_fd);

// Same base schema tools/fakefs.c's fakefs_import/fakefs_init_empty lay down
// for a fresh root (minus the synthetic root inode, which callers seed
// themselves via path_create once fake_db_init has prepared the statements) --
// needed because fake_db_init's migration path (fakefs_migrate) assumes these
// tables already exist rather than creating them from nothing.
int fake_db_create_schema(const char *db_path) {
    sqlite3 *db;
    int err = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    if (err != SQLITE_OK)
        HANDLE_ERR_RET(db);
    EXEC_RET("pragma journal_mode=wal");
    EXEC_RET("begin");
    EXEC_RET("create table meta (id integer unique default 0, db_inode integer);"
             "insert into meta (db_inode) values (0);"
             "create table stats (inode integer primary key, stat blob);"
             "create table paths (path blob primary key, inode integer references stats(inode));"
             "create index inode_to_path on paths (inode, path);"
             // v6 repairs what the v4/v5 rename passes stranded and v7 the
             // inode aliasing that concurrent allocators left; a root created
             // here never had unescaped names and has no history to alias, so
             // there is nothing for either to find.
             "pragma user_version=7;");
    EXEC_RET("commit");
    sqlite3_close(db);
    return 0;
}

int fake_db_init(struct fakefs_db *fs, const char *db_path, int root_fd) {
    int err = sqlite3_open_v2(db_path, &fs->db, SQLITE_OPEN_READWRITE, NULL);
    if (err != SQLITE_OK) {
        printk("ERROR: sqlite3 opening database: %s\n", sqlite3_errmsg(fs->db));
        sqlite3_close(fs->db);
        return _EINVAL;
    }
    sqlite3_busy_timeout(fs->db, 1000);
    sqlite3_create_function(fs->db, "change_prefix", 3, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sqlite_func_change_prefix, NULL, NULL);
    db_check_error(fs);

    // let's do WAL mode
    sqlite3_stmt *statement = db_prepare(fs, "pragma journal_mode=wal");
    db_check_error(fs);
    sqlite3_step(statement);
    db_check_error(fs);
    sqlite3_finalize(statement);

    // synchronous=NORMAL is safe in WAL mode, and much faster
    statement = db_prepare(fs, "pragma synchronous=NORMAL");
    db_check_error(fs);
    sqlite3_step(statement);
    db_check_error(fs);
    sqlite3_finalize(statement);

    // Bound the WAL. The default journal_size_limit is -1, which means a
    // checkpoint RESETS the WAL but never shrinks it, so it grows to its
    // high-water mark and stays there. That matters because sqlite3_close()
    // checkpoints the whole WAL back into the database on the last connection,
    // and on iOS that close happens as the process is being suspended: a large
    // WAL turns it into slow I/O performed while still holding the database
    // lock, which is exactly what RunningBoard kills with 0xdead10cc (seen in
    // the File Provider extension, whose crash stacks sat in sqlite3WalClose ->
    // sqlite3WalCheckpoint -> unixWrite/unixTruncate). Truncating at each
    // checkpoint keeps that final close cheap.
    statement = db_prepare(fs, "pragma journal_size_limit=1048576");
    db_check_error(fs);
    sqlite3_step(statement);
    db_check_error(fs);
    sqlite3_finalize(statement);

    statement = db_prepare(fs, "pragma foreign_keys=true");
    db_check_error(fs);
    sqlite3_step(statement);
    db_check_error(fs);
    sqlite3_finalize(statement);

#if DEBUG_sql
    sqlite3_trace_v2(mount->db, SQLITE_TRACE_STMT, trace_callback, NULL);
#endif

    err = fakefs_migrate(fs, root_fd);
    if (err < 0)
        return err;

    // after the filesystem is compressed, transmitted, and uncompressed, the
    // inode numbers will be different. to detect this, the inode of the
    // database file is stored inside the database and compared with the actual
    // database file inode, and if they're different we rebuild the database.
    struct stat statbuf;
    if (stat(db_path, &statbuf) < 0) ERRNO_DIE("stat database");
    ino_t db_inode = statbuf.st_ino;
    statement = db_prepare(fs, "select db_inode from meta");
    if (sqlite3_step(statement) == SQLITE_ROW) {
        if ((uint64_t) sqlite3_column_int64(statement, 0) != db_inode) {
            sqlite3_finalize(statement);
            statement = NULL;
            int err = fakefs_rebuild(fs, root_fd);
            if (err < 0) {
                return err;
            }
        }
    }
    if (statement != NULL)
        sqlite3_finalize(statement);

    if (fakefs_inode_namespace_needs_compaction(fs)) {
        int err = fakefs_rebuild(fs, root_fd);
        if (err < 0)
            return err;
    }

    // save current inode
    statement = db_prepare(fs, "update meta set db_inode = ?");
    sqlite3_bind_int64(statement, 1, (int64_t) db_inode);
    db_check_error(fs);
    sqlite3_step(statement);
    db_check_error(fs);
    sqlite3_finalize(statement);

    // delete orphaned stats
    statement = db_prepare(fs, "delete from stats where not exists (select 1 from paths where inode = stats.inode)");
    db_check_error(fs);
    sqlite3_step(statement);
    db_check_error(fs);
    sqlite3_finalize(statement);

    fs->next_inode = fakefs_next_inode_init(fs);
    fs->lock = sqlite3_mutex_alloc(SQLITE_MUTEX_FAST);
    fs->stmt.begin_deferred = db_prepare(fs, "begin deferred");
    fs->stmt.begin_immediate = db_prepare(fs, "begin immediate");
    fs->stmt.commit = db_prepare(fs, "commit");
    fs->stmt.rollback = db_prepare(fs, "rollback");
    fs->stmt.path_get_inode = db_prepare(fs, "select inode from paths where path = ?");
    fs->stmt.path_read_stat = db_prepare(fs, "select inode, stat from stats natural join paths where path = ?");
    fs->stmt.path_create_stat = db_prepare(fs, "insert into stats (inode, stat) values (?, ?)");
    fs->stmt.path_create_path = db_prepare(fs, "insert or replace into paths values (?, ?)");
    fs->stmt.inode_read_stat = db_prepare(fs, "select stat from stats where inode = ?");
    fs->stmt.inode_write_stat = db_prepare(fs, "update stats set stat = ? where inode = ?");
    fs->stmt.path_link = db_prepare(fs, "insert or replace into paths (path, inode) values (?, ?)");
    fs->stmt.path_unlink = db_prepare(fs, "delete from paths where path = ?");
    fs->stmt.path_rename = db_prepare(fs, "update or replace paths set path = change_prefix(path, ?, ?) "
            "where (path >= ? and path < ?) or path = ?");
    fs->stmt.path_from_inode = db_prepare(fs, "select path from paths where inode = ?");
    fs->stmt.try_cleanup_inode = db_prepare(fs, "delete from stats where inode = ? and not exists (select 1 from paths where inode = stats.inode)");
    return 0;
}

int fake_db_deinit(struct fakefs_db *fs) {
    if (fs->db) {
        sqlite3_finalize(fs->stmt.begin_deferred);
        sqlite3_finalize(fs->stmt.begin_immediate);
        sqlite3_finalize(fs->stmt.commit);
        sqlite3_finalize(fs->stmt.rollback);
        sqlite3_finalize(fs->stmt.path_get_inode);
        sqlite3_finalize(fs->stmt.path_read_stat);
        sqlite3_finalize(fs->stmt.path_create_stat);
        sqlite3_finalize(fs->stmt.path_create_path);
        sqlite3_finalize(fs->stmt.inode_read_stat);
        sqlite3_finalize(fs->stmt.inode_write_stat);
        sqlite3_finalize(fs->stmt.path_link);
        sqlite3_finalize(fs->stmt.path_unlink);
        sqlite3_finalize(fs->stmt.path_rename);
        sqlite3_finalize(fs->stmt.path_from_inode);
        sqlite3_finalize(fs->stmt.try_cleanup_inode);
        return sqlite3_close(fs->db);
    }
    return SQLITE_OK;
}
