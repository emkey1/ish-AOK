#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "kernel/fs.h" // for MAX_PATH
#include "fs/sqlutil.h"
#include "fs/fake-db.h"
#include "fs/fake-path.h"
#include "kernel/errno.h"
#include "util/list.h"
#include "debug.h"

// rebuild process in pseudocode:
//
// table = {}
// for each path, inode:
//     real_inode = stat(path).st_ino
//     if inode in table:
//         unlink(path)
//         link(table[inode], path)
//     else:
//         table[inode] = path
//     stat = db['stat ' + inode]
//     new_db['inode ' + path] = real_inode
//     new_db['stat ' + real_inode] = stat

// ad hoc hashtable
struct entry {
    ino_t inode;
    ino_t compact_inode;
    char *path;
    struct list chain;
};

int fakefs_rebuild(struct fakefs_db *fs, int root_fd) {
    sqlite3 *db = fs->db;
    int err;

    EXEC_RET("begin");
    EXEC_RET("create table paths_old (path blob primary key, inode integer)");
    EXEC_RET("create table stats_old (inode integer primary key, stat blob)");
    EXEC_RET("insert into paths_old select * from paths");
    EXEC_RET("insert into stats_old select * from stats");
    // Attributes are keyed by the inode numbers this renumbers, so they move
    // with them: each old inode's rows go to its new number below.
    EXEC_RET("create table xattrs_old (inode integer, name blob, value blob)");
    EXEC_RET("insert into xattrs_old select inode, name, value from xattrs");
    EXEC_RET("delete from xattrs");
    EXEC_RET("delete from paths");
    EXEC_RET("delete from stats");
    sqlite3_stmt *get_paths = PREPARE_RET("select path, inode from paths_old");
    sqlite3_stmt *read_stat = PREPARE_RET("select stat from stats_old where inode = ?");
    sqlite3_stmt *write_path = PREPARE_RET("insert into paths (path, inode) values (?, ?)");
    sqlite3_stmt *write_stat = PREPARE_RET("replace into stats (inode, stat) values (?, ?)");
    sqlite3_stmt *move_xattrs = PREPARE_RET("insert or replace into xattrs (inode, name, value) "
            "select ?, name, value from xattrs_old where inode = ?");
    inode_t next_inode = 1;

    struct list hashtable[2000];
#define HASH_SIZE (sizeof(hashtable)/sizeof(hashtable[0]))
    for (unsigned i = 0; i < HASH_SIZE; i++)
        list_init(&hashtable[i]);

    while (STEP_RET(get_paths)) {
        const char *path = (const char *) sqlite3_column_text(get_paths, 0);
        ino_t inode = sqlite3_column_int64(get_paths, 1);

        // the DB speaks guest paths; the host filesystem speaks the escaped
        // on-disk form (fs/fake-path.h)
        char host_path[MAX_PATH + 1];
        if (fake_path_to_host(path, host_path, sizeof(host_path)) == NULL)
            continue;

        // grab real inode
        struct stat stat;
        int err = fstatat(root_fd, fix_path(host_path), &stat, 0);
        if (err < 0)
            continue;
        // restore hardlinks
        struct list *bucket = &hashtable[inode % HASH_SIZE];
        struct entry *entry;
        bool found = false;
        ino_t compact_inode = 0;
        list_for_each_entry(bucket, entry, chain) {
            if (entry->inode == inode) {
                char host_link[MAX_PATH + 1];
                if (fake_path_to_host(entry->path, host_link, sizeof(host_link)) == NULL)
                    continue;
                unlinkat(root_fd, fix_path(host_path), 0);
                linkat(root_fd, fix_path(host_link), root_fd, fix_path(host_path), 0);
                found = true;
                compact_inode = entry->compact_inode;
                break;
            }
        }
        if (!found) {
            entry = malloc(sizeof(struct entry));
            entry->inode = inode;
            entry->compact_inode = next_inode++;
            entry->path = strdup(path);
            list_add(bucket, &entry->chain);
            compact_inode = entry->compact_inode;
            err = sqlite3_bind_int64(move_xattrs, 1, compact_inode); CHECK_ERR_RET();
            err = sqlite3_bind_int64(move_xattrs, 2, inode); CHECK_ERR_RET();
            STEP_RET(move_xattrs);
            RESET_RET(move_xattrs);
        }

        // extract the stat so we can copy it
        err = sqlite3_bind_int64(read_stat, 1, inode); CHECK_ERR_RET();
        if (STEP_RET(read_stat) == false) {
            RESET_RET(read_stat);
            continue;
        }
        const void *stat_data = sqlite3_column_blob(read_stat, 0);
        size_t stat_data_size = sqlite3_column_bytes(read_stat, 0);

        // store all the information in the new database
        err = sqlite3_bind_int64(write_stat, 1, compact_inode); CHECK_ERR_RET();
        err = sqlite3_bind_blob(write_stat, 2, stat_data, stat_data_size, SQLITE_TRANSIENT); CHECK_ERR_RET();
        STEP_RET(write_stat);
        RESET_RET(write_stat);
        err = sqlite3_bind_blob(write_path, 1, path, strlen(path), SQLITE_TRANSIENT); CHECK_ERR_RET();
        err = sqlite3_bind_int64(write_path, 2, compact_inode); CHECK_ERR_RET();
        STEP_RET(write_path);
        RESET_RET(write_path);

        RESET_RET(read_stat);
    }

    for (unsigned i = 0; i < HASH_SIZE; i++) {
        struct entry *entry, *tmp;
        list_for_each_entry_safe(&hashtable[i], entry, tmp, chain) {
            list_remove(&entry->chain);
            free(entry->path);
            free(entry);
        }
    }

    EXEC_RET("drop table paths_old");
    EXEC_RET("drop table stats_old");
    EXEC_RET("drop table xattrs_old");
    EXEC_RET("commit");
    FINALIZE_RET(get_paths);
    FINALIZE_RET(read_stat);
    FINALIZE_RET(write_path);
    FINALIZE_RET(write_stat);
    FINALIZE_RET(move_xattrs);
    return 0;
}
