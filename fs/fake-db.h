#ifndef FS_FAKEFS_API_H
#define FS_FAKEFS_API_H

#include <sqlite3.h>
#include "fs/fake-lockstats.h"
#include "fs/fix_path.h"
#include "misc.h"

typedef uint64_t inode_t;

struct fakefs_db {
    sqlite3 *db;
    inode_t next_inode;
    struct {
        sqlite3_stmt *begin_deferred;
        sqlite3_stmt *begin_immediate;
        sqlite3_stmt *commit;
        sqlite3_stmt *rollback;
        sqlite3_stmt *path_get_inode;
        sqlite3_stmt *path_read_stat;
        sqlite3_stmt *path_create_stat;
        sqlite3_stmt *path_create_path;
        sqlite3_stmt *inode_read_stat;
        sqlite3_stmt *inode_write_stat;
        sqlite3_stmt *path_link;
        sqlite3_stmt *path_unlink;
        sqlite3_stmt *path_rename;
        sqlite3_stmt *path_from_inode;
        sqlite3_stmt *try_cleanup_inode;
    } stmt;
    // Serializes writers. Every pooled connection carries the same mutex
    // pointer as the mount's primary, so the write path is unchanged.
    sqlite3_mutex *lock;

    // --- per-thread read connections (fs/fake-conn.c) --------------------
    // Opt-in via ISH_FAKEFS_PARALLEL_READS. The mount's primary fakefs_db owns
    // a pool; each pooled connection is itself a struct fakefs_db with its own
    // sqlite3 handle and its own prepared statements, so every existing
    // fs->db / fs->stmt.* access keeps working unchanged on the thread's own
    // handles. `shared` points back at the primary for the state that must
    // stay common (next_inode), and doubles as "this is a pooled connection,
    // reads here need no mutex".
    struct fakefs_db *shared;
    struct fakefs_pool *pool;
    struct fakefs_db *pool_next;  // registry of every connection
    struct fakefs_db *idle_next;  // free list; a connection is on both
    struct fakefs_pool *owner_pool;  // pool that owns a pooled connection
};

// The mount's primary db: where next_inode lives, and the only handle
// fake-migrate.c / fake-rebuild.c ever run on.
static inline struct fakefs_db *fakefs_db_shared(struct fakefs_db *fs) {
    return fs->shared != NULL ? fs->shared : fs;
}

// The calling thread's private connection for this mount, or `fs` itself when
// pooling is off, unavailable, or already at its connection cap. Cheap after
// the first call on a thread.
struct fakefs_db *fakefs_db_thread(struct fakefs_db *fs);
int fakefs_pool_init(struct fakefs_db *fs, const char *db_path);
void fakefs_pool_deinit(struct fakefs_db *fs);
// Opens one more connection to the same database, with the same pragmas and
// the same prepared statements as the primary. fake-db.c.
int fake_db_open_conn(struct fakefs_db *conn, const char *db_path);
void fake_db_close_conn(struct fakefs_db *conn);

// Read-side critical region. A pooled connection touches only its own
// statements and takes a WAL read snapshot, so it needs no mutex at all --
// that is the whole point. The primary still serializes, so a thread that
// could not get a pooled connection behaves exactly as before.
#define FAKEFS_LOCK_READ(fs) do {                                             \
    if ((fs)->shared != NULL) {                                               \
        if (fakefs_lockstats_on) {                                            \
            static struct fakefs_lock_site *_flr_site;                        \
            if (_flr_site == NULL)                                            \
                _flr_site = fakefs_lockstats_site(__func__);                  \
            fakefs_lockstats_held(_flr_site, fakefs_lockstats_now());         \
        }                                                                     \
        break;                                                                \
    }                                                                         \
    FAKEFS_LOCK(fs);                                                          \
} while (0)

#define FAKEFS_UNLOCK_READ(fs) do {                                           \
    if ((fs)->shared != NULL) {                                              \
        if (fakefs_lockstats_on)                                              \
            fakefs_lockstats_account(fakefs_lockstats_now());                 \
        break;                                                                \
    }                                                                         \
    FAKEFS_UNLOCK(fs);                                                        \
} while (0)

int fake_db_init(struct fakefs_db *fs, const char *db_path, int root_fd);
int fake_db_deinit(struct fakefs_db *fs);
void fake_db_finalize_statements(struct fakefs_db *fs);
// Lays down a brand-new, empty fakefs metadata db at db_path (base schema,
// no root inode) so fake_db_init -- whose migration path assumes the base
// tables already exist -- can open it immediately. Mirrors the schema
// tools/fakefs.c's fakefs_import/fakefs_init_empty create for a fresh root;
// callers still need to seed a root ("") path_create themselves.
int fake_db_create_schema(const char *db_path);

// A transaction holds fs->lock from begin to commit/rollback. The macros exist
// only so ISH_FAKEFS_LOCKSTATS can attribute the hold to the fakefs entry point
// that opened it (fakefs_stat, fakefs_unlink, ...) rather than to db_begin_*.
void db_begin_read_at(struct fakefs_db *fs, struct fakefs_lock_site *site);
void db_begin_write_at(struct fakefs_db *fs, struct fakefs_lock_site *site);
void db_commit(struct fakefs_db *fs);
void db_rollback(struct fakefs_db *fs);

#define FAKEFS_DB_BEGIN(fs, which) do {                                       \
    static struct fakefs_lock_site *_fls_txn;                                 \
    if (fakefs_lockstats_on && _fls_txn == NULL)                              \
        _fls_txn = fakefs_lockstats_site(__func__);                           \
    which((fs), _fls_txn);                                                    \
} while (0)
#define db_begin_read(fs) FAKEFS_DB_BEGIN(fs, db_begin_read_at)
#define db_begin_write(fs) FAKEFS_DB_BEGIN(fs, db_begin_write_at)

// Suspension quiesce gate.
//
// iOS kills a process that is suspended while holding a file or SQLite lock,
// with RUNNINGBOARD 0xdead10cc. A fakefs transaction holds one for its whole
// duration, not merely while executing inside libsqlite3 -- fakefs_unlink and
// friends keep it open across the real host operation -- so a suspension
// landing anywhere in that window is fatal. That is what these crashes look
// like in the field: threads caught in db_exec under fakefs_inode_orphaned
// (fd-table teardown at guest task exit), in fakefs_migrate during a mount, or
// simply in realfs_unlink with a transaction open and no sqlite frame in sight.
//
// Quiescing blocks NEW transactions and waits for in-flight ones to finish, so
// the process reaches a state with no fakefs lock held. It does not close the
// databases: between transactions a WAL connection holds no file lock, so an
// idle connection is already safe, and closing would mean re-preparing every
// cached statement on resume for no benefit.
//
// The gate is process-global (one app has many mounts) and is taken BEFORE
// fs->lock, so a task waiting on it holds nothing and cannot deadlock against
// a task that is mid-transaction.
//
// fakefs_quiesce_begin waits up to timeout_ms for the drain and returns true if
// it reached zero. It returns false on timeout, having still set the gate: the
// caller is on a deadline of its own and must not hang, so it reports the
// stragglers and carries on.
bool fakefs_quiesce_begin(unsigned timeout_ms, unsigned *still_in_flight);
void fakefs_quiesce_end(void);

bool db_exec(struct fakefs_db *fs, sqlite3_stmt *stmt);
void db_reset(struct fakefs_db *fs, sqlite3_stmt *stmt);
void db_exec_reset(struct fakefs_db *fs, sqlite3_stmt *stmt);

struct ish_stat {
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t rdev;
};

inode_t path_get_inode(struct fakefs_db *fs, const char *path);
bool path_read_stat(struct fakefs_db *fs, const char *path, struct ish_stat *stat, uint64_t *inode);
inode_t path_create(struct fakefs_db *fs, const char *path, struct ish_stat *stat);

bool inode_exists(struct fakefs_db *fs, inode_t inode);
bool inode_read_stat(struct fakefs_db *fs, inode_t inode, struct ish_stat *stat);
void inode_write_stat(struct fakefs_db *fs, inode_t inode, struct ish_stat *stat);

void path_link(struct fakefs_db *fs, const char *src, const char *dst);
inode_t path_unlink(struct fakefs_db *fs, const char *path);
void path_rename(struct fakefs_db *fs, const char *src, const char *dst);

#endif
