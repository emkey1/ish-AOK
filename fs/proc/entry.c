#include <sys/stat.h>
#include <string.h>
#include <stdint.h>
#include "fs/stat.h"
#include "fs/proc.h"
#include "kernel/task.h"

mode_t_ proc_entry_mode(struct proc_entry *entry) {
    mode_t_ mode = entry->meta->mode;
    if ((mode & S_IFMT) == 0)
        mode |= S_IFREG;
    if ((mode & 0777) == 0) {
        if (S_ISREG(mode))
            mode |= 0444;
        else if (S_ISDIR(mode))
            mode |= 0555;
        else if (S_ISLNK(mode))
            mode |= 0777;
    }
    return mode;
}

int proc_entry_stat(struct proc_entry *entry, struct statbuf *stat) {
    memset(stat, 0, sizeof(*stat));
    stat->mode = proc_entry_mode(entry);
    // Every procfs inode reported nlink 0, which is what a DELETED inode looks
    // like: `find` and `du` read it, and tools checking "is this still there"
    // read it. A file or symlink has one link; a directory has at least two
    // (itself and its parent) plus one per subdirectory, which is the number
    // find's leaf optimisation counts down. Subdirectories are counted from
    // the static child table where there is one; a directory listed by a
    // readdir callback (/proc itself, /proc/<pid>) reports the floor rather
    // than walking the pid table on every stat.
    stat->nlink = 1;
    if (S_ISDIR(stat->mode)) {
        stat->nlink = 2;
        const struct proc_children *children = entry->meta != NULL ? entry->meta->children : NULL;
        if (children != NULL)
            for (size_t i = 0; i < children->count; i++)
                if (S_ISDIR(children->entries[i].mode))
                    stat->nlink++;
    }

    struct task *task = pid_get_task_ref(entry->pid);
    if (task != NULL) {
        // generic_openat() holds inodes_lock across stat/open/fstat. Never
        // block procfs stat on a task lock while that global lock is held, or
        // a task exiting with general_lock held can stall unrelated file opens.
        if (trylock(&task->general_lock) == 0) {
            stat->uid = task->uid;
            stat->gid = task->gid;
            unlock(&task->general_lock);
        } else {
            stat->uid = task->uid;
            stat->gid = task->gid;
        }
        task_ref_cnt_mod(task, -1);
    } // else the memset above will have initialized memory to zero, which is the root uid/gid

    stat->inode = proc_entry_inode(entry);
    return 0;
}

qword_t proc_entry_inode(struct proc_entry *entry) {
    // Keep procfs inode numbers unique enough to avoid bogus cycles, but also
    // small enough to fit the legacy 32-bit stat ABI that userspace still hits.
    uint32_t hash = 2166136261U;
#define FNV_MIX_BYTE(v) do { hash ^= (uint8_t) (v); hash *= 16777619U; } while (0)
#define FNV_MIX_VALUE(v) do { \
    uintptr_t value__ = (uintptr_t) (v); \
    for (size_t i__ = 0; i__ < sizeof(value__); i__++) \
        FNV_MIX_BYTE(value__ >> (i__ * 8)); \
} while (0)

    FNV_MIX_VALUE(entry->parent);
    FNV_MIX_VALUE(entry->meta);
    if (entry->meta->getname || entry->meta->name != NULL) {
        char name[MAX_NAME + 1];
        proc_entry_getname(entry, name);
        for (size_t i = 0; name[i] != '\0'; i++)
            FNV_MIX_BYTE(name[i]);
    }
    FNV_MIX_VALUE(entry->pid);
    FNV_MIX_VALUE(entry->fd);

    // Avoid returning 0, which some callers treat as invalid or "not present".
    if (hash == 0)
        hash = 1;
    return hash;
#undef FNV_MIX_VALUE
#undef FNV_MIX_BYTE
}

void proc_entry_getname(struct proc_entry *entry, char *buf) {
    if (entry->meta->getname)
        entry->meta->getname(entry, buf);
    else if (entry->meta->name)
        strcpy(buf, entry->meta->name);
    else
        assert(!"missing name in proc entry");
}

static void proc_dot_getname(struct proc_entry *UNUSED(entry), char *buf) {
    strcpy(buf, ".");
}
static void proc_dotdot_getname(struct proc_entry *UNUSED(entry), char *buf) {
    strcpy(buf, "..");
}
static struct proc_dir_entry proc_dot_entry = {NULL, S_IFDIR, .getname = proc_dot_getname};
static struct proc_dir_entry proc_dotdot_entry = {NULL, S_IFDIR, .getname = proc_dotdot_getname};

bool proc_dir_read(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry) {
    // Every real directory reports "." and ".." as its first two dirents.
    // Some directory-walking code (e.g. glibc's nftw(), which procps' sysctl
    // -a uses) trusts that convention and unconditionally discards the first
    // two entries by position rather than by name -- without these, that
    // code silently ate the first two REAL sysctl files in every directory.
    // Shield every existing per-directory readdir/.children implementation
    // from this by translating the index here, once, rather than teaching
    // each of them about it.
    if (*index == 0) {
        (*index)++;
        *next_entry = (struct proc_entry) {&proc_dot_entry, .pid = entry->pid};
        return true;
    }
    if (*index == 1) {
        (*index)++;
        *next_entry = (struct proc_entry) {&proc_dotdot_entry, .pid = entry->pid};
        return true;
    }

    unsigned long real_index = *index - 2;
    bool any_left = false;
    if (entry->meta->readdir) {
        any_left = entry->meta->readdir(entry, &real_index, next_entry);
    } else if (entry->meta->children) {
        if (real_index >= entry->meta->children->count) {
            any_left = false;
        } else {
            next_entry->meta = &entry->meta->children->entries[real_index];
            next_entry->pid = entry->pid;
            real_index++;
            any_left = true;
        }
    } else {
        assert(!"read from invalid proc directory");
    }
    *index = real_index + 2;
    return any_left;
}

void free_string_array(char **array) {
    for (int i = 0; array[i] != NULL; i++)
        free(array[i]);
    free(array);
}

void proc_entry_cleanup(struct proc_entry *entry) {
    if (entry->name != NULL)
        free(entry->name);
    if (entry->child_names != NULL)
        free_string_array(entry->child_names);
    *entry = (struct proc_entry) {0};
}

void proc_set_entries_parent(struct proc_dir_entry *entries, size_t count, struct proc_dir_entry *parent) {
    for (size_t i = 0; i < count; i++)
        entries[i].parent = parent;
}

void proc_set_children_parent(struct proc_children *children, struct proc_dir_entry *parent) {
    proc_set_entries_parent(children->entries, children->count, parent);
}

struct proc_dir_entry *proc_find_entry(struct proc_dir_entry *entries, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (entries[i].name != NULL && strcmp(entries[i].name, name) == 0)
            return &entries[i];
    }
    return NULL;
}

struct proc_dir_entry *proc_children_find(struct proc_children *children, const char *name) {
    return proc_find_entry(children->entries, children->count, name);
}
