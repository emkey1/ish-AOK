#ifndef FS_PROC_H
#define FS_PROC_H

#include "fs/stat.h"
#include "misc.h"

struct proc_dir_entry;

struct proc_entry {
    struct proc_dir_entry *meta;
    unsigned long index;
    char **child_names;
    char *name;
    pid_t_ pid;
    sdword_t fd; // typedef might not have been read yet
    struct proc_dir_entry *parent;
};

struct proc_data {
    char *data;
    size_t size;
    size_t capacity;
};

struct proc_dir_entry {
    const char *name;
    mode_t_ mode;
    
    // file with dynamic name
    void (*getname)(struct proc_entry *entry, char *buf);

    // file with custom show data function
    int (*show)(struct proc_entry *entry, struct proc_data *data);
    
    // file with a custom write function
    int (*update)(struct proc_entry *entry, struct proc_data *data);
    
    // file with custom pread functionality. flags are the opening fd's, so a
    // streaming entry can honour O_NONBLOCK -- /proc/kmsg blocks until there
    // is a new message otherwise, and a caller that asked not to block gets
    // exactly the hang it opened O_NONBLOCK to avoid.
    ssize_t (*pread)(struct proc_entry *entry, struct proc_data *data, off_t off, int flags);
    
    // file with custom pwrite functionality
    ssize_t (*pwrite)(struct proc_entry *entry, struct proc_data *data, off_t off);

    // symlink
    int (*readlink)(struct proc_entry *entry, char *buf);
    
    // file whose readiness is not simply "always". Without this every procfs
    // file reports POLL_READ, which is right for the ones that answer from a
    // snapshot and wrong for a stream that can be caught up.
    int (*poll)(struct proc_entry *entry, off_t off);

    // remove
    int (*unlink)(struct proc_entry *entry);

    // directory with static list
    struct proc_children *children;

    // directory with dynamic contents
    bool (*readdir)(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry);

    struct proc_dir_entry *parent;
    int inode;
};

struct proc_children {
    size_t count;
    struct proc_dir_entry entries[];
};

#define PROC_CHILDREN(...) { .count = sizeof((struct proc_dir_entry[])__VA_ARGS__) / sizeof(struct proc_dir_entry), .entries = __VA_ARGS__ }

// open(2) on a /proc/pid/ns/* magic link: returns a namespace fd (NULL if
// name is not a namespace entry -- caller falls back to path resolution).
struct fd *proc_ns_open(int pid, const char *name);

extern struct proc_dir_entry proc_root;
extern struct proc_dir_entry proc_pid;
extern struct proc_children proc_ish_children;
extern struct proc_children proc_net_children;
extern struct proc_children proc_sys_children;
extern struct proc_dir_entry proc_root_entries[];

int proc_show_mountinfo(struct proc_entry *entry, struct proc_data *buf);
// Wakes pollers of every open mountinfo fd (fs/proc.c). Call after any
// guest-visible mount-table change (mount/umount/move_mount/...), with
// mounts_lock NOT held. libmount's kernel mount monitor (systemd) depends
// on an edge per change; see the comment at the definition.
void proc_mountinfo_notify_changed(void);
int proc_show_mounts(struct proc_entry *entry, struct proc_data *buf);

mode_t_ proc_entry_mode(struct proc_entry *entry);
void proc_entry_getname(struct proc_entry *entry, char *buf);
qword_t proc_entry_inode(struct proc_entry *entry);
int proc_entry_stat(struct proc_entry *entry, struct statbuf *stat);
void proc_entry_cleanup(struct proc_entry *entry);

void free_string_array(char **array);

bool proc_dir_read(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry);

void proc_buf_append(struct proc_data *buf, const void *data, size_t size);
// Format-checked: an argument/conversion mismatch here silently shifts every
// column of a procfs file (see proc_show_dev, which shipped 18 arguments
// against 16 conversions).
void proc_printf(struct proc_data *buf, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

void proc_set_entries_parent(struct proc_dir_entry *entries, size_t count, struct proc_dir_entry *parent);
void proc_set_children_parent(struct proc_children *children, struct proc_dir_entry *parent);
struct proc_dir_entry *proc_find_entry(struct proc_dir_entry *entries, size_t count, const char *name);
struct proc_dir_entry *proc_children_find(struct proc_children *children, const char *name);

void proc_root_init(void);
void proc_pid_init(void);
void proc_ish_init(struct proc_dir_entry *root_entry);
void proc_net_init(struct proc_dir_entry *root_entry);
void proc_sys_init(struct proc_dir_entry *root_entry);

#endif
