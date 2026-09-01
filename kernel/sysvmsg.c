// SysV message queues (msgget/msgsnd/msgrcv/msgctl).
//
// In-memory, process-shared within one emulator instance -- the same scope
// as kernel/ipc.c's SysV shm. The concrete customer is fakeroot: makepkg
// (and dpkg-buildpackage) run every package() stage under fakeroot, whose
// faked daemon talks to libfakeroot over a SysV message queue; with these
// syscalls stubbed to ENOSYS no AUR/apt package can be built in the guest.
//
// mtype is the guest's `long`: 4 bytes on i386, 8 bytes on the 64-bit ABIs.

#include <stdlib.h>
#include <string.h>

#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/sysvipc.h"
#include "fs/proc.h"
#include "kernel/task.h"
#include "util/list.h"
#include "util/sync.h"

#define IPC_PRIVATE_ 0
#define IPC_CREAT_   01000
#define IPC_EXCL_    02000
#define IPC_NOWAIT_  04000

#define IPC_RMID_    0
#define IPC_SET_     1
#define IPC_STAT_    2
#define IPC_INFO_    3
#define MSG_STAT_    11
#define MSG_STAT_ANY_ 13
#define IPC_64_      0x100

#define MSG_NOERROR_ 010000
#define MSG_EXCEPT_  020000
#define MSG_COPY_    040000

// Linux defaults (msgmax/msgmnb); large enough for fakeroot's small requests
// and anything else reasonable.
#define MSGMAX_ 8192
#define MSGMNB_ 16384

struct msg_msg {
    struct list mlist;
    sqword_t type;
    size_t size;
    uint8_t data[];
};

struct msg_queue {
    struct list qlist;
    int id;
    dword_t key;
    uid_t_ uid, gid, cuid, cgid;
    mode_t_ mode;
    size_t cbytes;   // bytes currently queued
    size_t qnum;     // messages currently queued
    size_t qbytes;   // capacity (msg_qbytes)
    pid_t_ lspid, lrpid;
    time_t_ stime, rtime, ctime;
    bool removed;
    unsigned waiters;      // tasks blocked in snd/rcv referencing this queue
    struct list messages;
    cond_t snd_cond;       // senders waiting for space
    cond_t rcv_cond;       // receivers waiting for a message
};

static struct list msg_queues = LIST_INITIALIZER(msg_queues);

static lock_t msg_lock = LOCK_INITIALIZER;
static int msg_next_id = 1;

static time_t_ msg_now(void) {
    return (time_t_) time(NULL);
}

static struct msg_queue *msg_find_by_key(dword_t key) {
    struct msg_queue *q;
    list_for_each_entry(&msg_queues, q, qlist) {
        if (!q->removed && q->key == key)
            return q;
    }
    return NULL;
}

static struct msg_queue *msg_find_by_id(int id) {
    struct msg_queue *q;
    list_for_each_entry(&msg_queues, q, qlist) {
        if (!q->removed && q->id == id)
            return q;
    }
    return NULL;
}

// Called with msg_lock held, after marking removed or dropping a waiter.
// The queue node stays allocated until the last blocked task has woken up
// and seen `removed`; only then is it freed.
static void msg_queue_maybe_free(struct msg_queue *queue) {
    if (!queue->removed || queue->waiters != 0)
        return;
    struct msg_msg *msg, *tmp;
    list_for_each_entry_safe(&queue->messages, msg, tmp, mlist) {
        list_remove(&msg->mlist);
        free(msg);
    }
    list_remove(&queue->qlist);
    cond_destroy(&queue->snd_cond);
    cond_destroy(&queue->rcv_cond);
    free(queue);
}

int_t sys_msgget_guest(dword_t key, int_t msgflg) {
    lock(&msg_lock, 0);
    struct msg_queue *queue = key == IPC_PRIVATE_ ? NULL : msg_find_by_key(key);
    if (queue != NULL) {
        if ((msgflg & IPC_CREAT_) && (msgflg & IPC_EXCL_)) {
            unlock(&msg_lock);
            return _EEXIST;
        }
        int id = queue->id;
        unlock(&msg_lock);
        return id;
    }
    if (!(msgflg & IPC_CREAT_) && key != IPC_PRIVATE_) {
        unlock(&msg_lock);
        return _ENOENT;
    }

    queue = calloc(1, sizeof(*queue));
    if (queue == NULL) {
        unlock(&msg_lock);
        return _ENOMEM;
    }
    queue->id = msg_next_id++;
    queue->key = key;
    queue->uid = queue->cuid = current->euid;
    queue->gid = queue->cgid = current->egid;
    queue->mode = msgflg & 0777;
    queue->qbytes = MSGMNB_;
    queue->ctime = msg_now();
    list_init(&queue->messages);
    cond_init(&queue->snd_cond);
    cond_init(&queue->rcv_cond);
    list_add_tail(&msg_queues, &queue->qlist);
    int id = queue->id;
    unlock(&msg_lock);
    return id;
}

// Reads the guest msgbuf: mtype (guest long) followed by the payload.
static int msg_read_from_guest(guest_addr_t msgp, size_t msgsz,
                               sqword_t *type_out, struct msg_msg **msg_out) {
    sqword_t type;
    size_t long_size;
    if (task_is_64bit(current)) {
        sqword_t t;
        if (user_get(msgp, t))
            return _EFAULT;
        type = t;
        long_size = 8;
    } else {
        sdword_t t;
        if (user_get(msgp, t))
            return _EFAULT;
        type = t;
        long_size = 4;
    }
    struct msg_msg *msg = malloc(sizeof(*msg) + msgsz);
    if (msg == NULL)
        return _ENOMEM;
    msg->type = type;
    msg->size = msgsz;
    if (msgsz > 0 && user_read(msgp + long_size, msg->data, msgsz)) {
        free(msg);
        return _EFAULT;
    }
    *type_out = type;
    *msg_out = msg;
    return 0;
}

int_t sys_msgsnd_guest(int_t msqid, guest_addr_t msgp, qword_t msgsz, int_t msgflg) {
    if (msgsz > MSGMAX_)
        return _EINVAL;

    sqword_t type;
    struct msg_msg *msg;
    int err = msg_read_from_guest(msgp, (size_t) msgsz, &type, &msg);
    if (err < 0)
        return err;
    if (type < 1) {
        free(msg);
        return _EINVAL;
    }

    lock(&msg_lock, 0);
    struct msg_queue *queue = msg_find_by_id(msqid);
    if (queue == NULL) {
        unlock(&msg_lock);
        free(msg);
        return _EINVAL;
    }
    // Sending needs write permission on the queue.
    if (!ipc_access_ok(queue->uid, queue->gid, queue->cuid, queue->cgid, queue->mode, 2)) {
        unlock(&msg_lock);
        free(msg);
        return _EACCES;
    }
    while (queue->cbytes + msgsz > queue->qbytes) {
        if (msgflg & IPC_NOWAIT_) {
            unlock(&msg_lock);
            free(msg);
            return _EAGAIN;
        }
        queue->waiters++;
        err = wait_for(&queue->snd_cond, &msg_lock, NULL);
        queue->waiters--;
        if (queue->removed) {
            msg_queue_maybe_free(queue);
            unlock(&msg_lock);
            free(msg);
            return _EIDRM;
        }
        if (err < 0) {
            unlock(&msg_lock);
            free(msg);
            return _EINTR;
        }
    }
    list_add_tail(&queue->messages, &msg->mlist);
    queue->cbytes += msgsz;
    queue->qnum++;
    queue->lspid = current->pid;
    queue->stime = msg_now();
    notify(&queue->rcv_cond);
    unlock(&msg_lock);
    return 0;
}

// msgtyp selection per msgrcv(2): 0 = first message; >0 = first of that type
// (or first NOT of that type with MSG_EXCEPT); <0 = lowest type <= |msgtyp|.
static struct msg_msg *msg_pick(struct msg_queue *queue, sqword_t msgtyp, int_t msgflg) {
    struct msg_msg *msg, *best = NULL;
    list_for_each_entry(&queue->messages, msg, mlist) {
        if (msgtyp == 0)
            return msg;
        if (msgtyp > 0) {
            if (msgflg & MSG_EXCEPT_) {
                if (msg->type != msgtyp)
                    return msg;
            } else if (msg->type == msgtyp) {
                return msg;
            }
        } else {
            if (msg->type <= -msgtyp && (best == NULL || msg->type < best->type))
                best = msg;
        }
    }
    return best;
}

int_t sys_msgrcv_guest(int_t msqid, guest_addr_t msgp, qword_t msgsz,
                       sqword_t msgtyp, int_t msgflg) {
    if (msgflg & MSG_COPY_)
        return _EINVAL; // requires CONFIG_CHECKPOINT_RESTORE; nobody sane uses it

    lock(&msg_lock, 0);
    struct msg_queue *queue = msg_find_by_id(msqid);
    if (queue == NULL) {
        unlock(&msg_lock);
        return _EINVAL;
    }
    // Receiving needs read permission on the queue.
    if (!ipc_access_ok(queue->uid, queue->gid, queue->cuid, queue->cgid, queue->mode, 4)) {
        unlock(&msg_lock);
        return _EACCES;
    }
    struct msg_msg *msg;
    for (;;) {
        msg = msg_pick(queue, msgtyp, msgflg);
        if (msg != NULL)
            break;
        if (msgflg & IPC_NOWAIT_) {
            unlock(&msg_lock);
            return _ENOMSG;
        }
        queue->waiters++;
        int err = wait_for(&queue->rcv_cond, &msg_lock, NULL);
        queue->waiters--;
        if (queue->removed) {
            msg_queue_maybe_free(queue);
            unlock(&msg_lock);
            return _EIDRM;
        }
        if (err < 0) {
            unlock(&msg_lock);
            return _EINTR;
        }
    }

    if (msg->size > msgsz && !(msgflg & MSG_NOERROR_)) {
        unlock(&msg_lock);
        return _E2BIG;
    }
    list_remove(&msg->mlist);
    queue->cbytes -= msg->size;
    queue->qnum--;
    queue->lrpid = current->pid;
    queue->rtime = msg_now();
    notify(&queue->snd_cond);
    unlock(&msg_lock);

    size_t copy_size = msg->size > msgsz ? (size_t) msgsz : msg->size;
    int err = 0;
    if (task_is_64bit(current)) {
        sqword_t type = msg->type;
        if (user_put(msgp, type))
            err = _EFAULT;
        else if (copy_size > 0 && user_write(msgp + 8, msg->data, copy_size))
            err = _EFAULT;
    } else {
        sdword_t type = (sdword_t) msg->type;
        if (user_put(msgp, type))
            err = _EFAULT;
        else if (copy_size > 0 && user_write(msgp + 4, msg->data, copy_size))
            err = _EFAULT;
    }
    free(msg);
    if (err < 0)
        return err;
    return (int_t) copy_size;
}

struct msqid64_ds_i386_ {
    struct ipc_perm_i386_ msg_perm;
    dword_t msg_stime;
    dword_t msg_stime_high;
    dword_t msg_rtime;
    dword_t msg_rtime_high;
    dword_t msg_ctime;
    dword_t msg_ctime_high;
    dword_t msg_cbytes;
    dword_t msg_qnum;
    dword_t msg_qbytes;
    pid_t_ msg_lspid;
    pid_t_ msg_lrpid;
    dword_t __unused4;
    dword_t __unused5;
};
static_assert(sizeof(struct msqid64_ds_i386_) == 88, "i386 msqid64_ds size");

struct msqid64_ds_64_ {
    struct ipc_perm_amd64_ msg_perm;
    sqword_t msg_stime;
    sqword_t msg_rtime;
    sqword_t msg_ctime;
    qword_t msg_cbytes;
    qword_t msg_qnum;
    qword_t msg_qbytes;
    pid_t_ msg_lspid;
    pid_t_ msg_lrpid;
    qword_t __unused4;
    qword_t __unused5;
};
static_assert(sizeof(struct msqid64_ds_64_) == 120, "64-bit msqid64_ds size");

int_t sys_msgctl_guest(int_t msqid, int_t cmd, guest_addr_t buf) {
    int cmd_base = cmd & ~IPC_64_;

    if (cmd_base == IPC_INFO_) // no tool we care about consumes the limits
        return _EINVAL;

    lock(&msg_lock, 0);
    struct msg_queue *queue = msg_find_by_id(msqid);
    if (queue == NULL) {
        unlock(&msg_lock);
        return _EINVAL;
    }

    if (cmd_base == IPC_RMID_) {
        if (!ipc_owner_ok(queue->uid, queue->cuid)) {
            unlock(&msg_lock);
            return _EPERM;
        }
        queue->removed = true;
        notify(&queue->snd_cond);
        notify(&queue->rcv_cond);
        msg_queue_maybe_free(queue);
        unlock(&msg_lock);
        return 0;
    }

    if (cmd_base == IPC_SET_) {
        if (!ipc_owner_ok(queue->uid, queue->cuid)) {
            unlock(&msg_lock);
            return _EPERM;
        }
        // Only perms and qbytes are settable; read the ABI-appropriate
        // layout and apply the fields we track.
        uid_t_ uid; uid_t_ gid; mode_t_ mode; qword_t qbytes;
        if (task_is_64bit(current)) {
            struct msqid64_ds_64_ info;
            if (user_read(buf, &info, sizeof(info))) {
                unlock(&msg_lock);
                return _EFAULT;
            }
            uid = info.msg_perm.uid;
            gid = info.msg_perm.gid;
            mode = (mode_t_) info.msg_perm.mode;
            qbytes = info.msg_qbytes;
        } else {
            struct msqid64_ds_i386_ info;
            if (user_read(buf, &info, sizeof(info))) {
                unlock(&msg_lock);
                return _EFAULT;
            }
            uid = info.msg_perm.uid;
            gid = info.msg_perm.gid;
            mode = (mode_t_) info.msg_perm.mode;
            qbytes = info.msg_qbytes;
        }
        queue->uid = uid;
        queue->gid = gid;
        queue->mode = mode & 0777;
        queue->qbytes = (size_t) qbytes;
        queue->ctime = msg_now();
        notify(&queue->snd_cond); // capacity may have grown
        unlock(&msg_lock);
        return 0;
    }

    if (cmd_base == IPC_STAT_ || cmd_base == MSG_STAT_ || cmd_base == MSG_STAT_ANY_) {
        int err = 0;
        if (task_is_64bit(current)) {
            struct msqid64_ds_64_ info = {
                .msg_perm = {
                    .key = queue->key,
                    .uid = queue->uid,
                    .gid = queue->gid,
                    .cuid = queue->cuid,
                    .cgid = queue->cgid,
                    .mode = (word_t) queue->mode,
                },
                .msg_stime = queue->stime,
                .msg_rtime = queue->rtime,
                .msg_ctime = queue->ctime,
                .msg_cbytes = queue->cbytes,
                .msg_qnum = queue->qnum,
                .msg_qbytes = queue->qbytes,
                .msg_lspid = queue->lspid,
                .msg_lrpid = queue->lrpid,
            };
            if (user_write(buf, &info, sizeof(info)))
                err = _EFAULT;
        } else {
            struct msqid64_ds_i386_ info = {
                .msg_perm = {
                    .key = queue->key,
                    .uid = queue->uid,
                    .gid = queue->gid,
                    .cuid = queue->cuid,
                    .cgid = queue->cgid,
                    .mode = queue->mode,
                },
                .msg_stime = (dword_t) queue->stime,
                .msg_rtime = (dword_t) queue->rtime,
                .msg_ctime = (dword_t) queue->ctime,
                .msg_cbytes = (dword_t) queue->cbytes,
                .msg_qnum = (dword_t) queue->qnum,
                .msg_qbytes = (dword_t) queue->qbytes,
                .msg_lspid = queue->lspid,
                .msg_lrpid = queue->lrpid,
            };
            if (user_write(buf, &info, sizeof(info)))
                err = _EFAULT;
        }
        unlock(&msg_lock);
        return err;
    }

    unlock(&msg_lock);
    return _EINVAL;
}

// i386 direct-syscall entry points (numbers 399-402, kernel >= 5.1 ABI);
// the ipc(2) multiplexer path lives in kernel/ipc.c.
int_t sys_msgget(dword_t key, int_t msgflg) {
    return sys_msgget_guest(key, msgflg);
}
int_t sys_msgsnd(int_t msqid, addr_t msgp, dword_t msgsz, int_t msgflg) {
    return sys_msgsnd_guest(msqid, msgp, msgsz, msgflg);
}
int_t sys_msgrcv(int_t msqid, addr_t msgp, dword_t msgsz, int_t msgtyp, int_t msgflg) {
    return sys_msgrcv_guest(msqid, msgp, msgsz, (sqword_t) msgtyp, msgflg);
}
int_t sys_msgctl(int_t msqid, int_t cmd, addr_t buf) {
    return sys_msgctl_guest(msqid, cmd, buf);
}

// /proc/sysvipc/msg -- see the note on proc_sysvipc_show_sem.
void proc_sysvipc_show_msg(struct proc_data *buf) {
    proc_printf(buf, "%10s %10s %-10s %10s %10s %5s %5s %5s %5s %5s %5s %10s %10s %10s\n",
                "key", "msqid", "perms", "cbytes", "qnum", "lspid", "lrpid",
                "uid", "gid", "cuid", "cgid", "stime", "rtime", "ctime");
    lock(&msg_lock, 0);
    struct msg_queue *q;
    list_for_each_entry(&msg_queues, q, qlist) {
        if (q->removed)
            continue;
        proc_printf(buf, "%10d %10d %-10o %10zu %10zu %5d %5d %5u %5u %5u %5u %10lld %10lld %10lld\n",
                    (int) q->key, q->id, q->mode & 0777, q->cbytes, q->qnum,
                    (int) q->lspid, (int) q->lrpid,
                    q->uid, q->gid, q->cuid, q->cgid,
                    (long long) q->stime, (long long) q->rtime, (long long) q->ctime);
    }
    unlock(&msg_lock);
}
