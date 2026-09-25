// IPC namespaces: see kernel/ipc_ns.h.

#include <stdlib.h>
#include "kernel/ipc_ns.h"
#include "kernel/task.h"

struct ipc_namespace init_ipc_ns = {
    .refcount = 1,
    .holds = 1,
    // Linux's PROC_IPC_INIT_INO, what /proc/<pid>/ns/ipc has always said.
    .inode = 4026531839ul,
    .mq_lock = LOCK_INITIALIZER,
    .mq_queues = LIST_INITIALIZER(init_ipc_ns.mq_queues),
    .mq_queues_max = 256,
    .mq_msg_max = 10,
    .mq_msgsize_max = 8192,
    .mq_msg_default = 10,
    .mq_msgsize_default = 8192,
    .shm_lock = LOCK_INITIALIZER,
    .shm_segments = LIST_INITIALIZER(init_ipc_ns.shm_segments),
    .shm_next_id = 1,
    .sem_lock = LOCK_INITIALIZER,
    .sem_sets = LIST_INITIALIZER(init_ipc_ns.sem_sets),
    .sem_next_id = 1,
    .msg_lock = LOCK_INITIALIZER,
    .msg_queues = LIST_INITIALIZER(init_ipc_ns.msg_queues),
    .msg_next_id = 1,
};

// Past the initial namespaces' fixed numbers (PROC_DYNAMIC_FIRST and the
// first handful of its range), where Linux's nsfs numbers new ones.
static _Atomic unsigned long next_ns_inum = 4026532200ul;

unsigned long ns_alloc_inum(void) {
    return atomic_fetch_add(&next_ns_inum, 1);
}

struct ipc_namespace *ipc_ns_current(void) {
    if (current != NULL && current->ipc_ns != NULL)
        return current->ipc_ns;
    return &init_ipc_ns;
}

struct ipc_namespace *ipc_ns_retain(struct ipc_namespace *ns) {
    atomic_fetch_add(&ns->refcount, 1);
    return ns;
}

void ipc_ns_hold(struct ipc_namespace *ns) {
    atomic_fetch_add(&ns->holds, 1);
}

void ipc_ns_unhold(struct ipc_namespace *ns) {
    if (atomic_fetch_sub(&ns->holds, 1) == 1 && ns != &init_ipc_ns)
        free(ns);
}

void ipc_ns_release(struct ipc_namespace *ns) {
    if (ns == NULL)
        return;
    if (atomic_fetch_sub(&ns->refcount, 1) != 1)
        return;
    // The initial namespace holds a reference nothing drops.
    if (ns == &init_ipc_ns)
        return;
    // Nothing can reach this namespace by name any more: its objects go, as
    // Linux's free_ipc_ns takes them. What is still open keeps what it needs.
    mqueue_ns_teardown(ns);
    shm_ns_teardown(ns);
    sem_ns_teardown(ns);
    msg_ns_teardown(ns);
    ipc_ns_unhold(ns);
}

struct ipc_namespace *ipc_ns_new(void) {
    struct ipc_namespace *ns = malloc(sizeof(*ns));
    if (ns == NULL)
        return NULL;
    *ns = (struct ipc_namespace) {
        .inode = ns_alloc_inum(),
        .mq_queues_max = 256,
        .mq_msg_max = 10,
        .mq_msgsize_max = 8192,
        .mq_msg_default = 10,
        .mq_msgsize_default = 8192,
        .shm_next_id = 1,
        .sem_next_id = 1,
        .msg_next_id = 1,
    };
    atomic_init(&ns->refcount, 1);
    atomic_init(&ns->holds, 1);
    lock_init(&ns->mq_lock, "ipc_ns.mq\0");
    lock_init(&ns->shm_lock, "ipc_ns.shm\0");
    lock_init(&ns->sem_lock, "ipc_ns.sem\0");
    lock_init(&ns->msg_lock, "ipc_ns.msg\0");
    list_init(&ns->mq_queues);
    list_init(&ns->shm_segments);
    list_init(&ns->sem_sets);
    list_init(&ns->msg_queues);
    return ns;
}
