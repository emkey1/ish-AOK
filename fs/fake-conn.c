// Per-thread sqlite connections for the fakefs metadata database.
//
// Every metadata operation on a mount used to serialize on one sqlite mutex.
// Measurement (ISH_FAKEFS_LOCKSTATS) showed a single thread already holding it
// ~78% of wall clock on a metadata-heavy workload, which makes concurrent guest
// tasks slower than sequential ones. The mutex is not protecting the database:
// the db is WAL with synchronous=NORMAL, and WAL readers do not block each
// other. It is protecting the ONE sqlite3 handle and the ONE set of cached
// sqlite3_stmt that every thread shares.
//
// So: give each thread its own handle and its own statements, and let readers
// run concurrently. Writers keep the original mutex -- WAL permits only one
// writer, and fake.c deliberately holds write transactions across the real host
// syscall, which that mutex is what makes atomic.
//
// Connections are recycled rather than closed when a thread exits: guest task
// churn is high, and an sqlite open per short-lived thread would cost more than
// the contention it saves. The pool is capped; a thread that cannot get one
// falls back to the primary connection and the original locking, so the cap is
// a performance bound, never a correctness one.

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "debug.h"
#include "fs/fake-db.h"

struct fakefs_pool {
    char *db_path;
    pthread_mutex_t lock;
    pthread_key_t key;
    struct fakefs_db *idle;  // recycled, ready to hand out
    struct fakefs_db *all;   // every connection ever opened, for teardown
    unsigned opened;
    unsigned cap;
    bool dead;               // unmounted; connections are closed, do not touch
};

// One connection per host thread that touches the fs, bounded. Connections are
// recycled on thread exit, so the high-water mark is the number of threads
// concurrently touching fakefs, not the number the guest ever had. A guest
// process costs about three of them (shell, the command, its pipe peer): at a
// cap of 16, eight parallel finds already spilled to the serialized fallback
// and gave back 3.1 s of the 4.0 s the pool had won. Past the cap the primary
// connection still serves everyone, so this is a performance bound only.
#define DEFAULT_CAP 32

// pthread_key destructor: the thread is gone, so its connection goes back on
// the free list for the next one rather than being closed. Guest task churn is
// high enough that closing here would cost more than the contention this saves.
static void conn_released(void *arg) {
    struct fakefs_db *conn = arg;
    struct fakefs_pool *pool = conn->owner_pool;
    if (pool == NULL)
        return;
    pthread_mutex_lock(&pool->lock);
    if (pool->dead) {
        // Unmounted underneath us. fakefs_pool_deinit closed the handle and
        // left the struct allocated precisely so this path stays valid.
        pthread_mutex_unlock(&pool->lock);
        return;
    }
    conn->idle_next = pool->idle;
    pool->idle = conn;
    pthread_mutex_unlock(&pool->lock);
}

int fakefs_pool_init(struct fakefs_db *fs, const char *db_path) {
    fs->shared = NULL;
    fs->pool = NULL;
    fs->pool_next = NULL;
    fs->idle_next = NULL;
    fs->owner_pool = NULL;

    const char *knob = getenv("ISH_FAKEFS_PARALLEL_READS");
    if (knob == NULL || strcmp(knob, "0") == 0)
        return 0;

    struct fakefs_pool *pool = calloc(1, sizeof(*pool));
    if (pool == NULL)
        return 0;  // no pool is a valid state; carry on serialized
    pool->db_path = strdup(db_path);
    if (pool->db_path == NULL) {
        free(pool);
        return 0;
    }
    pthread_mutex_init(&pool->lock, NULL);
    if (pthread_key_create(&pool->key, conn_released) != 0) {
        free(pool->db_path);
        free(pool);
        return 0;
    }
    pool->cap = DEFAULT_CAP;
    const char *cap = getenv("ISH_FAKEFS_CONN_CAP");
    if (cap != NULL) {
        int n = atoi(cap);
        if (n > 0)
            pool->cap = (unsigned) n;
    }
    fs->pool = pool;
    return 0;
}

struct fakefs_db *fakefs_db_thread(struct fakefs_db *fs) {
    struct fakefs_pool *pool = fs->pool;
    if (pool == NULL)
        return fs;  // pooling off, or fs is already a pooled connection
    struct fakefs_db *conn = pthread_getspecific(pool->key);
    if (conn != NULL)
        return conn;

    pthread_mutex_lock(&pool->lock);
    conn = pool->idle;
    if (conn != NULL) {
        pool->idle = conn->idle_next;
    } else if (pool->opened < pool->cap) {
        conn = calloc(1, sizeof(*conn));
        if (conn != NULL) {
            if (fake_db_open_conn(conn, pool->db_path) < 0) {
                free(conn);
                conn = NULL;
            } else {
                conn->shared = fs;
                conn->owner_pool = pool;
                conn->lock = fs->lock;  // writers still serialize on it
                conn->pool = NULL;      // a connection never re-enters the pool
                pool->opened++;
                conn->pool_next = pool->all;
                pool->all = conn;
            }
        }
    }
    pthread_mutex_unlock(&pool->lock);

    if (conn == NULL)
        return fs;  // at the cap: serialized, exactly as before
    pthread_setspecific(pool->key, conn);
    return conn;
}

void fakefs_pool_deinit(struct fakefs_db *fs) {
    struct fakefs_pool *pool = fs->pool;
    if (pool == NULL)
        return;
    fs->pool = NULL;
    pthread_mutex_lock(&pool->lock);
    pool->dead = true;
    // Close every connection, idle or still checked out to a live thread, but
    // do NOT free the structs and do NOT delete the key: a thread holding one
    // in its TLS will run conn_released when it exits, possibly long after this
    // returns, and that destructor has to find valid memory. Deliberately
    // leaking at most `cap` connection structs plus the pool is the price of
    // that; unmount is rare and the amount is bounded.
    for (struct fakefs_db *conn = pool->all; conn != NULL; conn = conn->pool_next)
        fake_db_close_conn(conn);
    pool->idle = NULL;
    pthread_mutex_unlock(&pool->lock);
    free(pool->db_path);
    pool->db_path = NULL;
}
