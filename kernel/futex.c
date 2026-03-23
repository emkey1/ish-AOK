#include "kernel/calls.h"
#include <pthread.h>
#include "futex.h"
#include "kernel/time.h"
#include "util/timer.h"
#include "util/sync.h"
// Apple doesn't implement futex, so we have to fake it
#define FUTEX_WAIT_ 0
#define FUTEX_WAKE_ 1
#define FUTEX_FD_        2 // Deprecated in Linux
#define FUTEX_REQUEUE_ 3
#define FUTEX_CMP_REQUEUE_    4
#define FUTEX_WAKE_OP_        5
#define FUTEX_LOCK_PI_        6
#define FUTEX_UNLOCK_PI_        7
#define FUTEX_TRYLOCK_PI_    8
#define FUTEX_WAIT_BITSET_    9
#define FUTEX_WAKE_BITSET_    10
#define FUTEX_WAIT_REQUEUE_PI_    11
#define FUTEX_CMP_REQUEUE_PI_    12
#define FUTEX_PRIVATE_FLAG_ 128
#define FUTEX_CLOCK_REALTIME_    256

#define FUTEX_CMD_MASK_        ~(FUTEX_PRIVATE_FLAG_ | FUTEX_CLOCK_REALTIME_)

#define FUTEX_WAIT_PRIVATE_    (FUTEX_WAIT_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_WAKE_PRIVATE_    (FUTEX_WAKE_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_REQUEUE_PRIVATE_    (FUTEX_REQUEUE_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_CMP_REQUEUE_PRIVATE_ (FUTEX_CMP_REQUEUE_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_WAKE_OP_PRIVATE_    (FUTEX_WAKE_OP_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_LOCK_PI_PRIVATE_    (FUTEX_LOCK_PI_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_UNLOCK_PI_PRIVATE_    (FUTEX_UNLOCK_PI_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_TRYLOCK_PI_PRIVATE_ (FUTEX_TRYLOCK_PI_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_WAIT_BITSET_PRIVATE_    (FUTEX_WAIT_BITSET_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_WAKE_BITSET_PRIVATE_    (FUTEX_WAKE_BITSET_ | FUTEX_PRIVATE_FLAG_)
#define FUTEX_WAIT_REQUEUE_PI_PRIVATE_    (FUTEX_WAIT_REQUEUE_PI_ | \
                     FUTEX_PRIVATE_FLAG_)
#define FUTEX_CMP_REQUEUE_PI_PRIVATE_    (FUTEX_CMP_REQUEUE_PI_ | \
                     FUTEX_PRIVATE_FLAG_)
//#define FUTEX_CMD_MASK_ ~(FUTEX_PRIVATE_FLAG_)

extern bool doEnableMulticore;

struct futex {
    atomic_uint refcount;
    struct mem *mem;
    addr_t addr;
    struct list queue;
    struct list chain; // locked by futex_hash_lock
};

struct futex_wait {
    cond_t cond;
    struct futex *futex; // The futex on which the thread is waiting
    pthread_t thread;    // The thread that is waiting
    dword_t bitset;      // Match mask for FUTEX_WAIT_BITSET / WAKE_BITSET
    struct list queue;   // For linking in the futex's queue
};

#define FUTEX_HASH_BITS 12
#define FUTEX_HASH_SIZE (1 << FUTEX_HASH_BITS)
static lock_t futex_lock = LOCK_INITIALIZER;
static struct list futex_hash[FUTEX_HASH_SIZE];

static void __attribute__((constructor)) init_futex_hash(void) {
    for (int i = 0; i < FUTEX_HASH_SIZE; i++)
        list_init(&futex_hash[i]);
}

static struct futex *futex_get_unlocked(addr_t addr) {
    int hash = (addr ^ (unsigned long) current->mem) % FUTEX_HASH_SIZE;
    struct list *bucket = &futex_hash[hash];
    struct futex *futex;
    list_for_each_entry(bucket, futex, chain) {
        if (futex->addr == addr && futex->mem == current->mem) {
            futex->refcount++;
            return futex;
        }
    }

    futex = malloc(sizeof(struct futex));
    if (futex == NULL) {
        unlock(&futex_lock);
        return NULL;
    }
    futex->refcount = 1;
    futex->mem = current->mem;
    futex->addr = addr;
    list_init(&futex->queue);
    list_add(bucket, &futex->chain);
    return futex;
}

// Returns the futex for the current process at the given addr, and locks it
// Unlocked variant is available for times when you need to get two futexes at once
static struct futex *futex_get(addr_t addr) {
    lock(&futex_lock, 0);
    struct futex *futex = futex_get_unlocked(addr);
    if (futex == NULL)
        unlock(&futex_lock);
    return futex;
}

static void futex_put_unlocked(struct futex *futex) {
    if (--futex->refcount == 0) {
        assert(list_empty(&futex->queue));
        list_remove(&futex->chain);
        free(futex);
    }
}

// Must be called on the result of futex_get when you're done with it
// Also has an unlocked version, for releasing the result of futex_get_unlocked
static void futex_put(struct futex *futex) {
    futex_put_unlocked(futex);
    unlock(&futex_lock);
}

static int futex_load(struct futex *futex, dword_t *out) {
    assert(futex->mem == current->mem);
    read_lock(&current->mem->lock);
    dword_t *ptr = mem_ptr(current->mem, futex->addr, MEM_READ);
    read_unlock(&current->mem->lock);
    if (ptr == NULL)
        return 1;
    *out = *ptr;
    return 0;
}

static int futex_wait_masked(addr_t uaddr, dword_t val, struct timespec *timeout, dword_t bitset) {
    struct futex *futex = futex_get(uaddr);
    int err = 0;
    dword_t tmp;
    if (futex_load(futex, &tmp))
        err = _EFAULT;
    else if (tmp != val)
        err = _EAGAIN;
    else {
        struct futex_wait wait;
        wait.cond = COND_INITIALIZER;
        wait.futex = futex;
        wait.thread = pthread_self();
        wait.bitset = bitset;
        list_add_tail(&futex->queue, &wait.queue);
        TASK_MAY_BLOCK {
            err = wait_for(&wait.cond, &futex_lock, timeout);
        }
        futex = wait.futex;
        list_remove_safe(&wait.queue);
    }
    futex_put(futex);
    STRACE("%d end futex(FUTEX_WAIT)", current->pid);
    return err;
}

static int futex_wait(addr_t uaddr, dword_t val, struct timespec *timeout) {
    return futex_wait_masked(uaddr, val, timeout, ~0u);
}

static int futex_read_timeout(addr_t timeout_addr, bool time64, struct timespec *timeout) {
    if (!time64) {
        struct timespec_ timeout_guest;
        if (user_get(timeout_addr, timeout_guest))
            return _EFAULT;
        timeout->tv_sec = timeout_guest.sec;
        timeout->tv_nsec = timeout_guest.nsec;
    } else {
        struct timespec64_ timeout_guest;
        if (user_get(timeout_addr, timeout_guest))
            return _EFAULT;
        timeout->tv_sec = timeout_guest.sec;
        timeout->tv_nsec = timeout_guest.nsec;
    }
    if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 || timeout->tv_nsec >= 1000000000)
        return _EINVAL;
    return 0;
}

static int futex_wakelike(int op, addr_t uaddr, dword_t wake_max, dword_t requeue_max, addr_t requeue_addr, dword_t wake_mask) {
    struct futex *futex = futex_get(uaddr);

    struct futex_wait *wait, *tmp;
    unsigned woken = 0;
    list_for_each_entry_safe(&futex->queue, wait, tmp, queue) {
        if (woken >= wake_max)
            break;
        if ((wait->bitset & wake_mask) == 0)
            continue;
        notify(&wait->cond);
        list_remove(&wait->queue);
        woken++;
    }

    if (op == FUTEX_REQUEUE_) {
        struct futex *futex2 = futex_get_unlocked(requeue_addr);
        unsigned requeued = 0;
        list_for_each_entry_safe(&futex->queue, wait, tmp, queue) {
            if (requeued >= requeue_max)
                break;
            // sketchy as hell
            list_remove(&wait->queue);
            list_add_tail(&futex2->queue, &wait->queue);
            assert(futex->refcount > 1); // should be true because this function keeps a reference
            futex->refcount--;
            futex2->refcount++;
            wait->futex = futex2;
            requeued++;
        }
        futex_put_unlocked(futex2);
        woken += requeued;
    }

    futex_put(futex);
    return woken;
}

int futex_wake(addr_t uaddr, dword_t wake_max) {
    return futex_wakelike(FUTEX_WAKE_, uaddr, wake_max, 0, 0, ~0u);
}

static int futex_cmp_requeue(addr_t uaddr1, dword_t val, addr_t uaddr2, dword_t val2, dword_t val3) {
    struct futex *futex1 = futex_get(uaddr1);
    struct futex *futex2 = futex_get_unlocked(uaddr2);
    int err = 0;
    dword_t tmp;

    if (futex_load(futex1, &tmp)) {
        err = _EFAULT;
    } else if (tmp != val) {
        err = _EAGAIN;
    } else {
        struct futex_wait *wait, *tmp_wait;
        dword_t requeued = 0;
        list_for_each_entry_safe(&futex1->queue, wait, tmp_wait, queue) {
            if (requeued >= val2) {
                break;
            }
            list_remove(&wait->queue);
            list_add_tail(&futex2->queue, &wait->queue);
            wait->futex = futex2;
            requeued++;
        }
        err = requeued;
    }

    futex_put(futex1);
    futex_put_unlocked(futex2);
    return err;
}

// Get the priority of a thread
int get_thread_priority(pthread_t thread) {
    struct sched_param param;
    int policy;
    pthread_getschedparam(thread, &policy, &param);
    return param.sched_priority;
}

// Set the priority of a thread
void set_thread_priority(pthread_t thread, int priority) {
    struct sched_param param;
    int policy;
    pthread_getschedparam(thread, &policy, &param);
    param.sched_priority = priority;
    pthread_setschedparam(thread, policy, &param);
}

static int futex_cmp_requeue_pi(addr_t uaddr1, dword_t val, addr_t uaddr2, dword_t val2, dword_t val3) {
    struct futex *futex1 = futex_get(uaddr1);
    struct futex *futex2 = futex_get_unlocked(uaddr2);
    int err = 0;
    dword_t tmp;

    if (futex_load(futex1, &tmp)) {
        err = _EFAULT;
    } else if (tmp != val) {
        err = _EAGAIN;
    } else {
        struct futex_wait *wait, *tmp_wait;
        int requeued = 0;
        int current_priority = get_thread_priority(pthread_self());
        int highest_waiting_priority = current_priority;

        // Find the highest priority among waiting threads
        list_for_each_entry_safe(&futex1->queue, wait, tmp_wait, queue) {
            int wait_priority = get_thread_priority(wait->thread);
            if (wait_priority > highest_waiting_priority) {
                highest_waiting_priority = wait_priority;
            }
        }

        // Inherit the highest priority if necessary
        if (highest_waiting_priority > current_priority) {
            set_thread_priority(pthread_self(), highest_waiting_priority);
        }

        list_for_each_entry_safe(&futex1->queue, wait, tmp_wait, queue) {
            if (requeued >= val2) {
                break;
            }

            list_remove(&wait->queue);
            list_add_tail(&futex2->queue, &wait->queue);
            wait->futex = futex2;
            requeued++;
        }

        // Restore original priority
        set_thread_priority(pthread_self(), current_priority);
        err = requeued;
    }

    futex_put(futex1);
    futex_put_unlocked(futex2);
    return err;
}

dword_t sys_futex_common(addr_t uaddr, dword_t op, dword_t val, addr_t timeout_or_val2,
        addr_t uaddr2, dword_t val3, bool timeout_time64) {
    if (!(op & FUTEX_PRIVATE_FLAG_)) {
        STRACE("!FUTEX_PRIVATE ");
    }
    struct timespec timeout = {0};
    if (((op & FUTEX_CMD_MASK_) == FUTEX_WAIT_ || (op & FUTEX_CMD_MASK_) == FUTEX_WAIT_BITSET_) && timeout_or_val2) {
        int err = futex_read_timeout(timeout_or_val2, timeout_time64, &timeout);
        if (err < 0)
            return err;
        if ((op & FUTEX_CMD_MASK_) == FUTEX_WAIT_BITSET_) {
            clockid_t clock = (op & FUTEX_CLOCK_REALTIME_) ? CLOCK_REALTIME : CLOCK_MONOTONIC;
            timeout = timespec_subtract(timeout, timespec_now(clock));
            if (!timespec_positive(timeout))
                return _ETIMEDOUT;
        }
    }
    
    switch (op & FUTEX_CMD_MASK_) {
        case FUTEX_WAIT_:
            STRACE("futex(FUTEX_WAIT, %#x, %d, 0x%x {%ds %dns}) = ...\n", uaddr, val, timeout_or_val2, timeout.tv_sec, timeout.tv_nsec);
            dword_t return_val;
            return_val = futex_wait(uaddr, val, timeout_or_val2 ? &timeout : NULL);
            return return_val;
        case FUTEX_WAKE_:
            STRACE("futex(FUTEX_WAKE, %#x, %d)", uaddr, val);
            return futex_wakelike(op & FUTEX_CMD_MASK_, uaddr, val, 0, 0, ~0u);
        case FUTEX_REQUEUE_:
            STRACE("futex(FUTEX_REQUEUE, %#x, %d, %#x)", uaddr, val, uaddr2);
            return futex_wakelike(op & FUTEX_CMD_MASK_, uaddr, val, timeout_or_val2, uaddr2, ~0u);
        case FUTEX_FD_: // Deprecated, little need to support
            STRACE("Unimplemented futex(FUTEX_FD, %#x, %d, %#x)", uaddr, val, uaddr2);
            FIXME("Unsupported futex(%#x, %d, %d, timeout=%#x, %#x, %d) (FUTEX_FD) ", uaddr, op, val, timeout_or_val2, uaddr2, val3);
            return _ENOSYS;
        case FUTEX_CMP_REQUEUE_:
            STRACE("Unimplemented futex(FUTEX_CMP_REQUEUE, %#x, %d, %#x)", uaddr, val, uaddr2);
            return futex_cmp_requeue(uaddr, val, uaddr2, timeout_or_val2, val3);
        case FUTEX_WAKE_OP_:
            STRACE("Unimplemented futex(FUTEX_WAKE_OP, %#x, %d, %#x)", uaddr, val, uaddr2);
            FIXME("Unsupported futex FUTEX_WAKE_OP(%#x, %d, %d, timeout=%#x, %#x, %d) (FUTEX_WAKE_OP) ", uaddr, op, val, timeout_or_val2, uaddr2, val3);
            return _ENOSYS;
        case FUTEX_LOCK_PI_:
            STRACE("Unimplemented futex(FUTEX_LOCK_PI, %#x, %d, %#x)", uaddr, val, uaddr2);
            FIXME("Unsupported futex FUTEX_LOCK_PI(%#x, %d, %d, timeout=%#x, %#x, %d) (FUTEX_LOCK_PI) ", uaddr, op, val, timeout_or_val2, uaddr2, val3);
            return _ENOSYS;
        case FUTEX_UNLOCK_PI_:
            STRACE("Unimplemented futex(FUTEX_UNLOCK_PI, %#x, %d, %#x)", uaddr, val, uaddr2);
            FIXME("Unsupported futex FUTEX_UNLOCK_PI(%#x, %d, %d, timeout=%#x, %#x, %d) (FUTEX_UNLOCK_PI) ", uaddr, op, val, timeout_or_val2, uaddr2, val3);
            return _ENOSYS;
        case FUTEX_TRYLOCK_PI_:
            STRACE("Unimplemented futex(FUTEX_TRYLOCK_PI, %#x, %d, %#x)", uaddr, val, uaddr2);
            FIXME("Unsupported futex FUTEX_TRYLOCK_PI(%#x, %d, %d, timeout=%#x, %#x, %d) (FUTEX_TRYLOCK_PI) ", uaddr, op, val, timeout_or_val2, uaddr2, val3);
            return _ENOSYS;
        case FUTEX_WAIT_BITSET_:
            STRACE("futex(FUTEX_WAIT_BITSET, %#x, %d, timeout=%#x, bitset=%#x)", uaddr, val, timeout_or_val2, val3);
            if (val3 == 0)
                return _EINVAL;
            return futex_wait_masked(uaddr, val, timeout_or_val2 ? &timeout : NULL, val3);
        case FUTEX_WAKE_BITSET_:
            STRACE("futex(FUTEX_WAKE_BITSET, %#x, %d, bitset=%#x)", uaddr, val, val3);
            if (val3 == 0)
                return _EINVAL;
            return futex_wakelike(op & FUTEX_CMD_MASK_, uaddr, val, 0, 0, val3);
        case FUTEX_WAIT_REQUEUE_PI_:
            STRACE("Unimplemented futex(FUTEX_WAIT_REQUEUE_PI, %#x, %d, %#x)", uaddr, val, uaddr2);
            FIXME("Unsupported futex FUTEX_WAIT_REQUEUE_PI(%#x, %d, %d, timeout=%#x, %#x, %d) (FUTEX_WAIT_REQUEUE_PI) ", uaddr, op, val, timeout_or_val2, uaddr2, val3);
            return _ENOSYS;
        case FUTEX_CMP_REQUEUE_PI_:
            STRACE("Unimplemented futex(FUTEX_CMP_REQUEUE_PI, %#x, %d, %#x)", uaddr, val, uaddr2);
            return futex_cmp_requeue_pi(uaddr, val, uaddr2, timeout_or_val2, val3);
    }
    STRACE("futex(%#x, %d, %d, timeout=%#x, %#x, %d) ", uaddr, op, val, timeout_or_val2, uaddr2, val3);
    FIXME("Unsupported futex(%#x, %d, %d, timeout=%#x, %#x, %d) ", uaddr, op, val, timeout_or_val2, uaddr2, val3);
    return _ENOSYS;
}

dword_t sys_futex(addr_t uaddr, dword_t op, dword_t val, addr_t timeout_or_val2, addr_t uaddr2, dword_t val3) {
    return sys_futex_common(uaddr, op, val, timeout_or_val2, uaddr2, val3, false);
}

dword_t sys_futex_time64(addr_t uaddr, dword_t op, dword_t val, addr_t timeout_or_val2, addr_t uaddr2, dword_t val3) {
    return sys_futex_common(uaddr, op, val, timeout_or_val2, uaddr2, val3, true);
}

struct robust_list_head_ {
    addr_t list;
    dword_t offset;
    addr_t list_op_pending;
};

int_t sys_set_robust_list(addr_t robust_list, dword_t len) {
    STRACE("set_robust_list(%#x, %d)", robust_list, len);
    if (len != sizeof(struct robust_list_head_))
        return _EINVAL;
    current->robust_list = robust_list;
    return 0;
}

int_t sys_get_robust_list(pid_t_ pid, addr_t robust_list_ptr, addr_t len_ptr) {
    STRACE("get_robust_list(%d, %#x, %#x)", pid, robust_list_ptr, len_ptr);

    complex_lockt(&pids_lock,0);
    struct task *task = pid_get_task(pid);
    unlock(&pids_lock);
    if (task != current)
        return _EPERM;

    if (user_put(robust_list_ptr, current->robust_list))
        return _EFAULT;
    if (user_put(len_ptr, (int[]) {sizeof(struct robust_list_head_)}))
        return _EFAULT;
    return 0;
}
