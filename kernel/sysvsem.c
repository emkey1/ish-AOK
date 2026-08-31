// SysV semaphores (semget/semop/semtimedop/semctl).
//
// Companion to kernel/sysvmsg.c and kernel/ipc.c's shm: same in-emulator
// scope. The concrete customer is again fakeroot -- faked creates two
// message queues AND a semaphore set at startup and dies without all
// three, which blocks makepkg/dpkg-buildpackage package() stages.
//
// SEM_UNDO is tracked per thread group (pthreads pass CLONE_SYSVSEM, so a
// process's threads share one undo list; fork starts a fresh one) and
// applied when the group's last thread exits via sysv_sem_exit().

#include <stdlib.h>
#include <string.h>

#include "kernel/calls.h"
#include "util/timer.h"
#include "kernel/errno.h"
#include "kernel/sysvipc.h"
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
#define SEM_STAT_    18
#define SEM_STAT_ANY_ 20
#define IPC_64_      0x100

#define GETPID_  11
#define GETVAL_  12
#define GETALL_  13
#define GETNCNT_ 14
#define GETZCNT_ 15
#define SETVAL_  16
#define SETALL_  17

#define SEM_UNDO_ 0x1000

#define SEMMSL_ 32000   // max semaphores per set (Linux default)
#define SEMVMX_ 32767   // max semaphore value

struct sem_entry {
    word_t val;
    pid_t_ last_pid;   // GETPID: pid of last semop
    unsigned ncnt;     // tasks waiting for the value to increase
    unsigned zcnt;     // tasks waiting for the value to hit zero
};

struct sem_undo {
    struct list ulist;         // node in sem_set.undos
    struct tgroup *group;      // owning thread group (CLONE_SYSVSEM scope)
    int16_t *adj;              // per-semaphore adjustment, nsems entries
};

struct sem_set {
    struct list slist;
    int id;
    dword_t key;
    uid_t_ uid, gid, cuid, cgid;
    mode_t_ mode;
    unsigned nsems;
    time_t_ otime, ctime;
    bool removed;
    unsigned waiters;
    struct sem_entry *sems;
    struct list undos;
    cond_t cond;   // any semaphore in the set changed
};

static struct list sem_sets = LIST_INITIALIZER(sem_sets);
static lock_t sem_lock = LOCK_INITIALIZER;
static int sem_next_id = 1;

static time_t_ sem_now(void) {
    return (time_t_) time(NULL);
}

static struct sem_set *sem_find_by_key(dword_t key) {
    struct sem_set *set;
    list_for_each_entry(&sem_sets, set, slist) {
        if (!set->removed && set->key == key)
            return set;
    }
    return NULL;
}

static struct sem_set *sem_find_by_id(int id) {
    struct sem_set *set;
    list_for_each_entry(&sem_sets, set, slist) {
        if (!set->removed && set->id == id)
            return set;
    }
    return NULL;
}

// Called with sem_lock held once removed is set or a waiter drops out; the
// set stays allocated until the last blocked task has seen `removed`.
static void sem_set_maybe_free(struct sem_set *set) {
    if (!set->removed || set->waiters != 0)
        return;
    struct sem_undo *undo, *tmp;
    list_for_each_entry_safe(&set->undos, undo, tmp, ulist) {
        list_remove(&undo->ulist);
        free(undo->adj);
        free(undo);
    }
    list_remove(&set->slist);
    cond_destroy(&set->cond);
    free(set->sems);
    free(set);
}

int_t sys_semget_guest(dword_t key, int_t nsems, int_t semflg) {
    if (nsems < 0 || nsems > SEMMSL_)
        return _EINVAL;
    lock(&sem_lock, 0);
    struct sem_set *set = key == IPC_PRIVATE_ ? NULL : sem_find_by_key(key);
    if (set != NULL) {
        if ((semflg & IPC_CREAT_) && (semflg & IPC_EXCL_)) {
            unlock(&sem_lock);
            return _EEXIST;
        }
        if (nsems != 0 && (unsigned) nsems > set->nsems) {
            unlock(&sem_lock);
            return _EINVAL;
        }
        int id = set->id;
        unlock(&sem_lock);
        return id;
    }
    if (!(semflg & IPC_CREAT_) && key != IPC_PRIVATE_) {
        unlock(&sem_lock);
        return _ENOENT;
    }
    if (nsems == 0) {
        unlock(&sem_lock);
        return _EINVAL;
    }

    set = calloc(1, sizeof(*set));
    struct sem_entry *sems = set == NULL ? NULL : calloc(nsems, sizeof(*sems));
    if (sems == NULL) {
        free(set);
        unlock(&sem_lock);
        return _ENOMEM;
    }
    set->id = sem_next_id++;
    set->key = key;
    set->uid = set->cuid = current->euid;
    set->gid = set->cgid = current->egid;
    set->mode = semflg & 0777;
    set->nsems = (unsigned) nsems;
    set->ctime = sem_now();
    set->sems = sems;
    list_init(&set->undos);
    cond_init(&set->cond);
    list_add_tail(&sem_sets, &set->slist);
    int id = set->id;
    unlock(&sem_lock);
    return id;
}

// Finds (or creates) this thread group's undo record for the set. Called
// with sem_lock held.
static struct sem_undo *sem_undo_get(struct sem_set *set) {
    struct sem_undo *undo;
    list_for_each_entry(&set->undos, undo, ulist) {
        if (undo->group == current->group)
            return undo;
    }
    undo = malloc(sizeof(*undo));
    int16_t *adj = undo == NULL ? NULL : calloc(set->nsems, sizeof(*adj));
    if (adj == NULL) {
        free(undo);
        return NULL;
    }
    undo->group = current->group;
    undo->adj = adj;
    list_add_tail(&set->undos, &undo->ulist);
    return undo;
}

struct sembuf_ {
    word_t sem_num;
    int16_t sem_op;
    int16_t sem_flg;
};
static_assert(sizeof(struct sembuf_) == 6, "sembuf size");

#define SEMOPM_ 500

// semop_common takes an optional deadline: NULL waits forever, otherwise the
// wait ends at that CLOCK_MONOTONIC instant with EAGAIN. The deadline is
// absolute rather than a duration because the loop below can wake several
// times before the operation succeeds -- handing wait_for the original
// duration each time would restart the clock on every wakeup and make the
// timeout unbounded in exactly the contended case it exists for.
static int_t semop_common(int_t semid, guest_addr_t sops_addr, uint_t nsops,
                          const struct timespec *deadline) {
    if (nsops == 0)
        return _EINVAL;
    if (nsops > SEMOPM_)
        return _E2BIG;
    struct sembuf_ sops[SEMOPM_];
    if (user_read(sops_addr, sops, nsops * sizeof(*sops)))
        return _EFAULT;

    lock(&sem_lock, 0);
    struct sem_set *set = sem_find_by_id(semid);
    if (set == NULL) {
        unlock(&sem_lock);
        return _EINVAL;
    }

    // semop alters the set, so it needs write permission. Without this any uid
    // could operate on another user's private semaphores.
    if (!ipc_access_ok(set->uid, set->gid, set->cuid, set->cgid, set->mode, 2)) {
        unlock(&sem_lock);
        return _EACCES;
    }
    for (unsigned i = 0; i < nsops; i++) {
        if (sops[i].sem_num >= set->nsems) {
            unlock(&sem_lock);
            return _EFBIG;
        }
    }

    for (;;) {
        // Try to apply the whole array atomically; on the first op that
        // must wait, roll back what was applied and sleep (or EAGAIN).
        int blocked = -1;
        for (unsigned i = 0; i < nsops; i++) {
            struct sem_entry *sem = &set->sems[sops[i].sem_num];
            int16_t op = sops[i].sem_op;
            if (op > 0) {
                if (sem->val + op > SEMVMX_) {
                    blocked = (int) i; // ERANGE, but treat as hard error
                    for (unsigned j = 0; j < i; j++)
                        set->sems[sops[j].sem_num].val -= sops[j].sem_op;
                    unlock(&sem_lock);
                    return _ERANGE;
                }
                sem->val += op;
            } else if (op == 0) {
                if (sem->val != 0) {
                    blocked = (int) i;
                    break;
                }
            } else {
                if (sem->val < -op) {
                    blocked = (int) i;
                    break;
                }
                sem->val += op;
            }
        }
        if (blocked < 0) {
            // Success: record pids, undo adjustments, wake anyone waiting.
            for (unsigned i = 0; i < nsops; i++) {
                struct sem_entry *sem = &set->sems[sops[i].sem_num];
                sem->last_pid = current->tgid;
                if ((sops[i].sem_flg & SEM_UNDO_) && sops[i].sem_op != 0) {
                    struct sem_undo *undo = sem_undo_get(set);
                    if (undo != NULL)
                        undo->adj[sops[i].sem_num] -= sops[i].sem_op;
                }
            }
            set->otime = sem_now();
            notify(&set->cond);
            unlock(&sem_lock);
            return 0;
        }

        // Roll back the ops applied before the blocking one.
        for (int j = 0; j < blocked; j++)
            set->sems[sops[j].sem_num].val -= sops[j].sem_op;
        if (sops[blocked].sem_flg & IPC_NOWAIT_) {
            unlock(&sem_lock);
            return _EAGAIN;
        }
        struct sem_entry *bsem = &set->sems[sops[blocked].sem_num];
        bool for_zero = sops[blocked].sem_op == 0;
        if (for_zero)
            bsem->zcnt++;
        else
            bsem->ncnt++;
        set->waiters++;
        int err;
        if (deadline != NULL) {
            struct timespec remaining = timespec_subtract(*deadline, timespec_now(CLOCK_MONOTONIC));
            if (!timespec_positive(remaining)) {
                err = _ETIMEDOUT;
            } else {
                err = wait_for(&set->cond, &sem_lock, &remaining);
            }
        } else {
            err = wait_for(&set->cond, &sem_lock, NULL);
        }
        set->waiters--;
        if (for_zero)
            bsem->zcnt--;
        else
            bsem->ncnt--;
        if (set->removed) {
            sem_set_maybe_free(set);
            unlock(&sem_lock);
            return _EIDRM;
        }
        if (err == _ETIMEDOUT) {
            // The operation never became possible in the time allowed. Linux
            // answers EAGAIN, the same as IPC_NOWAIT would have.
            sem_set_maybe_free(set);
            unlock(&sem_lock);
            return _EAGAIN;
        }
        if (err < 0) {
            unlock(&sem_lock);
            return _EINTR;
        }
    }
}

int_t sys_semop_guest(int_t semid, guest_addr_t sops_addr, uint_t nsops) {
    return semop_common(semid, sops_addr, nsops, NULL);
}

// The timeout used to be dropped on the floor, so semtimedop was semop: a
// caller that asked to wait 200ms for a semaphore waited for as long as it
// took, or forever. That is the one thing the call exists to avoid -- every
// user of it is a program that has decided it would rather give up than hang,
// and it was given the hang anyway, with no way to tell.
int_t sys_semtimedop_guest(int_t semid, guest_addr_t sops_addr, uint_t nsops,
                           guest_addr_t timeout_addr) {
    if (timeout_addr == 0)
        return semop_common(semid, sops_addr, nsops, NULL);

    struct timespec timeout;
    if (read_guest_timespec_abi(current->abi, timeout_addr, &timeout))
        return _EFAULT;
    if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000)
        return _EINVAL;
    struct timespec deadline = timespec_add(timespec_now(CLOCK_MONOTONIC), timeout);
    return semop_common(semid, sops_addr, nsops, &deadline);
}

struct semid64_ds_i386_ {
    struct ipc_perm_i386_ sem_perm;
    dword_t sem_otime;
    dword_t sem_otime_high;
    dword_t sem_ctime;
    dword_t sem_ctime_high;
    dword_t sem_nsems;
    dword_t __unused3;
    dword_t __unused4;
};
static_assert(sizeof(struct semid64_ds_i386_) == 64, "i386 semid64_ds size");

// asm-generic 64-bit layout (arm64/riscv64).
struct semid64_ds_generic_ {
    struct ipc_perm_amd64_ sem_perm;
    sqword_t sem_otime;
    sqword_t sem_ctime;
    qword_t sem_nsems;
    qword_t __unused3;
    qword_t __unused4;
};
static_assert(sizeof(struct semid64_ds_generic_) == 88, "generic semid64_ds size");

// x86_64 pads each time with an unused long (32-bit-era layout kept).
struct semid64_ds_amd64_ {
    struct ipc_perm_amd64_ sem_perm;
    sqword_t sem_otime;
    qword_t __unused1;
    sqword_t sem_ctime;
    qword_t __unused2;
    qword_t sem_nsems;
    qword_t __unused3;
    qword_t __unused4;
};
static_assert(sizeof(struct semid64_ds_amd64_) == 104, "amd64 semid64_ds size");

int_t sys_semctl_guest(int_t semid, int_t semnum, int_t cmd, guest_addr_t arg) {
    int cmd_base = cmd & ~IPC_64_;

    if (cmd_base == IPC_INFO_)
        return _EINVAL;

    lock(&sem_lock, 0);
    struct sem_set *set = sem_find_by_id(semid);
    if (set == NULL) {
        unlock(&sem_lock);
        return _EINVAL;
    }

    switch (cmd_base) {
    case IPC_RMID_:
        // Owner or creator only, like Linux.
        if (!ipc_owner_ok(set->uid, set->cuid)) {
            unlock(&sem_lock);
            return _EPERM;
        }
        set->removed = true;
        notify(&set->cond);
        sem_set_maybe_free(set);
        unlock(&sem_lock);
        return 0;

    case GETVAL_: case GETPID_: case GETNCNT_: case GETZCNT_: {
        if (semnum < 0 || (unsigned) semnum >= set->nsems) {
            unlock(&sem_lock);
            return _EINVAL;
        }
        struct sem_entry *sem = &set->sems[semnum];
        int_t result = cmd_base == GETVAL_ ? sem->val :
                       cmd_base == GETPID_ ? sem->last_pid :
                       cmd_base == GETNCNT_ ? (int_t) sem->ncnt : (int_t) sem->zcnt;
        unlock(&sem_lock);
        return result;
    }

    case SETVAL_: {
        if (semnum < 0 || (unsigned) semnum >= set->nsems) {
            unlock(&sem_lock);
            return _EINVAL;
        }
        // arg is union semun's int val: the register/low word itself.
        sdword_t val = (sdword_t) arg;
        if (val < 0 || val > SEMVMX_) {
            unlock(&sem_lock);
            return _ERANGE;
        }
        set->sems[semnum].val = (word_t) val;
        // Linux clears this semaphore's undo adjustments on SETVAL.
        struct sem_undo *undo;
        list_for_each_entry(&set->undos, undo, ulist)
            undo->adj[semnum] = 0;
        set->ctime = sem_now();
        notify(&set->cond);
        unlock(&sem_lock);
        return 0;
    }

    case GETALL_: case SETALL_: {
        unsigned nsems = set->nsems;
        word_t vals[SEMMSL_ < 4096 ? SEMMSL_ : 4096];
        if (nsems > sizeof(vals) / sizeof(vals[0])) {
            unlock(&sem_lock);
            return _EINVAL;
        }
        if (cmd_base == GETALL_) {
            for (unsigned i = 0; i < nsems; i++)
                vals[i] = set->sems[i].val;
            unlock(&sem_lock);
            if (user_write(arg, vals, nsems * sizeof(word_t)))
                return _EFAULT;
            return 0;
        }
        if (user_read(arg, vals, nsems * sizeof(word_t))) {
            unlock(&sem_lock);
            return _EFAULT;
        }
        for (unsigned i = 0; i < nsems; i++) {
            if (vals[i] > SEMVMX_) {
                unlock(&sem_lock);
                return _ERANGE;
            }
        }
        for (unsigned i = 0; i < nsems; i++)
            set->sems[i].val = vals[i];
        struct sem_undo *undo;
        list_for_each_entry(&set->undos, undo, ulist)
            memset(undo->adj, 0, nsems * sizeof(*undo->adj));
        set->ctime = sem_now();
        notify(&set->cond);
        unlock(&sem_lock);
        return 0;
    }

    case IPC_SET_: {
        // Only the permission fields matter; all three ABI layouts start
        // with the perm struct, so read just that.
        uid_t_ uid; uid_t_ gid; mode_t_ mode;
        if (task_is_64bit(current)) {
            struct ipc_perm_amd64_ perm;
            if (user_read(arg, &perm, sizeof(perm))) {
                unlock(&sem_lock);
                return _EFAULT;
            }
            uid = perm.uid; gid = perm.gid; mode = (mode_t_) perm.mode;
        } else {
            struct ipc_perm_i386_ perm;
            if (user_read(arg, &perm, sizeof(perm))) {
                unlock(&sem_lock);
                return _EFAULT;
            }
            uid = perm.uid; gid = perm.gid; mode = (mode_t_) perm.mode;
        }
        set->uid = uid;
        set->gid = gid;
        set->mode = mode & 0777;
        set->ctime = sem_now();
        unlock(&sem_lock);
        return 0;
    }

    case IPC_STAT_: case SEM_STAT_: case SEM_STAT_ANY_: {
        int err = 0;
        if (current->abi == GUEST_ABI_AMD64) {
            struct semid64_ds_amd64_ info = {
                .sem_perm = {
                    .key = set->key,
                    .uid = set->uid, .gid = set->gid,
                    .cuid = set->cuid, .cgid = set->cgid,
                    .mode = (word_t) set->mode,
                },
                .sem_otime = set->otime,
                .sem_ctime = set->ctime,
                .sem_nsems = set->nsems,
            };
            if (user_write(arg, &info, sizeof(info)))
                err = _EFAULT;
        } else if (task_is_64bit(current)) {
            struct semid64_ds_generic_ info = {
                .sem_perm = {
                    .key = set->key,
                    .uid = set->uid, .gid = set->gid,
                    .cuid = set->cuid, .cgid = set->cgid,
                    .mode = (word_t) set->mode,
                },
                .sem_otime = set->otime,
                .sem_ctime = set->ctime,
                .sem_nsems = set->nsems,
            };
            if (user_write(arg, &info, sizeof(info)))
                err = _EFAULT;
        } else {
            struct semid64_ds_i386_ info = {
                .sem_perm = {
                    .key = set->key,
                    .uid = set->uid, .gid = set->gid,
                    .cuid = set->cuid, .cgid = set->cgid,
                    .mode = set->mode,
                },
                .sem_otime = (dword_t) set->otime,
                .sem_ctime = (dword_t) set->ctime,
                .sem_nsems = set->nsems,
            };
            if (user_write(arg, &info, sizeof(info)))
                err = _EFAULT;
        }
        unlock(&sem_lock);
        return err;
    }
    }

    unlock(&sem_lock);
    return _EINVAL;
}

// Applies and drops a thread group's SEM_UNDO adjustments. Called from
// exit_tgroup() when the last thread of the group exits.
void sysv_sem_exit(struct tgroup *group) {
    lock(&sem_lock, 0);
    struct sem_set *set, *stmp;
    list_for_each_entry_safe(&sem_sets, set, stmp, slist) {
        struct sem_undo *undo, *utmp;
        bool changed = false;
        list_for_each_entry_safe(&set->undos, undo, utmp, ulist) {
            if (undo->group != group)
                continue;
            for (unsigned i = 0; i < set->nsems; i++) {
                if (undo->adj[i] == 0)
                    continue;
                sdword_t val = (sdword_t) set->sems[i].val + undo->adj[i];
                if (val < 0)
                    val = 0; // Linux clamps
                if (val > SEMVMX_)
                    val = SEMVMX_;
                set->sems[i].val = (word_t) val;
                changed = true;
            }
            list_remove(&undo->ulist);
            free(undo->adj);
            free(undo);
        }
        if (changed)
            notify(&set->cond);
    }
    unlock(&sem_lock);
}

// i386 direct-syscall entry points (semget=393, semtimedop=394; semop has
// no direct i386 number). The ipc(2) multiplexer path is in kernel/ipc.c.
int_t sys_semget(dword_t key, int_t nsems, int_t semflg) {
    return sys_semget_guest(key, nsems, semflg);
}
int_t sys_semtimedop(int_t semid, addr_t sops, dword_t nsops, addr_t timeout) {
    return sys_semtimedop_guest(semid, sops, nsops, timeout);
}
int_t sys_semctl(int_t semid, int_t semnum, int_t cmd, addr_t arg) {
    return sys_semctl_guest(semid, semnum, cmd, arg);
}
