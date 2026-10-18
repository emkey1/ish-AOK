// fakefs must never hand out an inode number twice on one root, not even after
// the file that had it is gone and the root has been remounted.
//
// It used to: the allocation counter was seeded from max(stats.inode) + 1 at
// every mount, so deleting the newest file freed its number for the next file
// or directory. The Files extension names items by these numbers and remounts
// after a few idle seconds, and the Files app keeps names for days -- so a
// delete, a save, or a move of a file that was gone reached whatever held the
// number by then. A guest boot recreating /run was enough to put a directory
// there. path_create (fs/fake-db.c) now also starts at and advances
// meta.next_inode, schema version 10 (fs/fake-migrate.c).
//
// Host unit test: two "mounts" of one meta.db in a scratch directory, with the
// same sequence each time -- create, delete the newest, remount, create.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "fs/fake-db.h"

// What libfakefs expects its host to provide (the kernel does, in the app).
void ish_printk(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
}
void die(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
    abort();
}
int err_map(int err) {
    return -err;
}

static int failures = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
        failures++; \
    } \
} while (0)

static char db_path[4096];
static int root_fd = -1;

static void mount_db(struct fakefs_db *fs) {
    memset(fs, 0, sizeof(*fs));
    int err = fake_db_init(fs, db_path, root_fd);
    if (err < 0) {
        fprintf(stderr, "fake_db_init: %d\n", err);
        exit(1);
    }
}

static inode_t create(struct fakefs_db *fs, const char *path, mode_t mode) {
    struct ish_stat stat = {.mode = mode};
    db_begin_write(fs);
    inode_t inode = path_create(fs, path, &stat);
    db_commit(fs);
    return inode;
}

static void unlink_path(struct fakefs_db *fs, const char *path) {
    db_begin_write(fs);
    path_unlink(fs, path);
    db_commit(fs);
}

int main(void) {
    char dir[] = "/tmp/fakefs-inode-reuse-XXXXXX";
    if (mkdtemp(dir) == NULL) {
        perror("mkdtemp");
        return 1;
    }
    snprintf(db_path, sizeof(db_path), "%s/meta.db", dir);
    root_fd = open(dir, O_RDONLY | O_DIRECTORY);
    if (root_fd < 0 || fake_db_create_schema(db_path) < 0) {
        fprintf(stderr, "cannot set up %s\n", dir);
        return 1;
    }

    struct fakefs_db fs;
    mount_db(&fs);
    inode_t root = create(&fs, "", S_IFDIR | 0755);
    inode_t victim = create(&fs, "/victim.txt", S_IFREG | 0644);
    CHECK(root != 0 && victim > root, "setup: root %llu victim %llu",
          (unsigned long long) root, (unsigned long long) victim);
    // Within one mount the in-memory counter already moves on.
    unlink_path(&fs, "/victim.txt");
    inode_t same_mount = create(&fs, "/same-mount", S_IFREG | 0644);
    CHECK(same_mount > victim, "same mount: %llu after victim %llu",
          (unsigned long long) same_mount, (unsigned long long) victim);
    unlink_path(&fs, "/same-mount");
    fake_db_deinit(&fs);

    // The case that reused it: the newest file is gone, then a fresh mount.
    mount_db(&fs);
    inode_t heir = create(&fs, "/run", S_IFDIR | 0755);
    CHECK(heir > same_mount, "remount: /run got %llu; %llu was deleted before the remount",
          (unsigned long long) heir, (unsigned long long) same_mount);
    // And a second allocator on the same root, mounted at the same time, must
    // continue from the mark rather than from its own seed.
    struct fakefs_db other;
    mount_db(&other);
    inode_t from_other = create(&other, "/from-other", S_IFREG | 0644);
    CHECK(from_other > heir, "second mount: %llu after %llu",
          (unsigned long long) from_other, (unsigned long long) heir);
    unlink_path(&other, "/from-other");
    inode_t back = create(&fs, "/back", S_IFREG | 0644);
    CHECK(back > from_other, "first mount after the second: %llu after %llu",
          (unsigned long long) back, (unsigned long long) from_other);
    fake_db_deinit(&other);
    fake_db_deinit(&fs);

    char cmd[4200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", dir);
    if (failures == 0)
        printf("fakefs_inode_reuse: ok (victim %llu, then %llu, %llu, %llu, %llu)\n",
               (unsigned long long) victim, (unsigned long long) same_mount,
               (unsigned long long) heir, (unsigned long long) from_other,
               (unsigned long long) back);
    return failures == 0 ? 0 : 1;
}
