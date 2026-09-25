#ifndef KERNEL_IPC_NS_H
#define KERNEL_IPC_NS_H

#include <stdatomic.h>
#include "misc.h"
#include "util/list.h"
#include "util/sync.h"

// An IPC namespace (CLONE_NEWIPC): its own POSIX message queues
// (kernel/mqueue.c) and System V objects (kernel/ipc.c, sysvsem.c, sysvmsg.c),
// and the limits /proc/sys/fs/mqueue sets for them. A task created with
// CLONE_NEWIPC, or one that unshares it, starts with an empty one; everything
// else shares its parent's.
//
// Two counts. `refcount` is what keeps the namespace ALIVE -- the tasks in it
// and the mqueue filesystems mounted from it -- and when it reaches zero the
// namespace's objects are destroyed: its
// queues lose their names and its System V objects are removed. `holds` only
// keeps the struct itself allocated for what still points at it afterwards: a
// queue that is open outlives its name, and the namespace it was counted in.
struct ipc_namespace {
    atomic_uint refcount;
    atomic_uint holds;
    // Its identity in /proc/<pid>/ns/ipc, which is how a program tells two
    // namespaces apart.
    unsigned long inode;

    // POSIX message queues by name, and how many; the rest of mqueue's state
    // lives on the queues. The limits are the namespace's own sysctls.
    lock_t mq_lock;
    struct list mq_queues;
    unsigned mq_queues_count;
    unsigned mq_queues_max;
    unsigned mq_msg_max;
    unsigned mq_msgsize_max;
    unsigned mq_msg_default;
    unsigned mq_msgsize_default;

    // System V IPC: each kind's objects and the next id to hand out, under
    // its own lock (kernel/ipc.c, kernel/sysvsem.c, kernel/sysvmsg.c).
    lock_t shm_lock;
    struct list shm_segments;
    int shm_next_id;
    lock_t sem_lock;
    struct list sem_sets;
    int sem_next_id;
    lock_t msg_lock;
    struct list msg_queues;
    int msg_next_id;
};

// The namespace every task starts in. Static, and never destroyed.
extern struct ipc_namespace init_ipc_ns;

// The calling task's namespace, or the initial one off a task thread.
struct ipc_namespace *ipc_ns_current(void);
struct ipc_namespace *ipc_ns_retain(struct ipc_namespace *ns);
void ipc_ns_release(struct ipc_namespace *ns);
// A new, empty namespace with Linux's default limits, one reference held.
struct ipc_namespace *ipc_ns_new(void);
// Keep the struct (not the namespace) allocated; see `holds` above.
void ipc_ns_hold(struct ipc_namespace *ns);
void ipc_ns_unhold(struct ipc_namespace *ns);

// Destroying a namespace's objects, one kind each (kernel/mqueue.c and the
// System V files), when its last reference goes.
void mqueue_ns_teardown(struct ipc_namespace *ns);
void shm_ns_teardown(struct ipc_namespace *ns);
void sem_ns_teardown(struct ipc_namespace *ns);
void msg_ns_teardown(struct ipc_namespace *ns);

// A namespace inode number, for a namespace just created: one no other
// namespace has had, from the range Linux's nsfs hands out after the initial
// namespaces' (kernel/ipc_ns.c).
unsigned long ns_alloc_inum(void);

#endif
