#include "debug.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/tty.h"
#include "kernel/calls.h"
#include "kernel/mm.h"
#include "kernel/ptrace.h"
#include "util/sync.h"
#include <string.h>

#define CSIGNAL_ 0x000000ff
#define CLONE_VM_ 0x00000100
#define CLONE_FS_ 0x00000200
#define CLONE_FILES_ 0x00000400
#define CLONE_SIGHAND_ 0x00000800
#define CLONE_PIDFD_ 0x00001000
#define CLONE_PTRACE_ 0x00002000
#define CLONE_VFORK_ 0x00004000
#define CLONE_PARENT_ 0x00008000
#define CLONE_THREAD_ 0x00010000
#define CLONE_NEWNS_ 0x00020000
#define CLONE_SYSVSEM_ 0x00040000
#define CLONE_SETTLS_ 0x00080000
#define CLONE_PARENT_SETTID_ 0x00100000
#define CLONE_CHILD_CLEARTID_ 0x00200000
#define CLONE_DETACHED_ 0x00400000
#define CLONE_UNTRACED_ 0x00800000
#define CLONE_CHILD_SETTID_ 0x01000000
#define CLONE_NEWCGROUP_ 0x02000000
#define CLONE_NEWUTS_ 0x04000000
#define CLONE_NEWIPC_ 0x08000000
#define CLONE_NEWUSER_ 0x10000000
#define CLONE_NEWPID_ 0x20000000
#define CLONE_NEWNET_ 0x40000000
#define CLONE_IO_ 0x80000000
// CLONE_IO is purely a hint that parent and child should share an io_context
// for I/O scheduler accounting (CFQ/BFQ); iSH has no io_context/io scheduler
// to share, so it's a harmless no-op to accept, same as real Linux treats it
// when CONFIG_BLOCK is off. Rejecting it (as "unimplemented") broke
// stress-ng's clone stressor outright, not just noisy logging.
#define IMPLEMENTED_FLAGS (CLONE_VM_|CLONE_FILES_|CLONE_FS_|CLONE_SIGHAND_|CLONE_SYSVSEM_|CLONE_VFORK_|CLONE_THREAD_|\
        CLONE_SETTLS_|CLONE_CHILD_SETTID_|CLONE_PARENT_SETTID_|CLONE_CHILD_CLEARTID_|CLONE_DETACHED_|CLONE_PARENT_|\
        CLONE_PIDFD_|CLONE_IO_|CLONE_NEWUTS_)
// The remaining namespace flags are recognized but never implemented (no
// mount/user/pid/ipc/cgroup/net namespaces; UTS is real, see kernel/uts.h).
// Handled separately from IMPLEMENTED_FLAGS below so they get the errno real
// Linux gives an unprivileged caller (EPERM: creating a new namespace needs
// CAP_SYS_ADMIN/CAP_SYS_USER_NS) instead of the generic "unrecognized flag"
// EINVAL -- callers that already fall back when they lack namespace privilege
// need to see that, not a malformed-argument error.
#define CLONE_NEW_FLAGS_ (CLONE_NEWNS_|CLONE_NEWCGROUP_|CLONE_NEWIPC_|CLONE_NEWUSER_|CLONE_NEWPID_|CLONE_NEWNET_)

static struct tgroup *tgroup_copy(struct tgroup *old_group) {
    struct tgroup *group = malloc(sizeof(struct tgroup));
    if (group == NULL)
        return NULL;
    *group = *old_group;
    list_init(&group->threads);
    // The shallow copy above aliased the parent's heap-allocated cgroup
    // path; give the child its own copy so neither free poisons the other
    // (fork keeps cgroup membership on Linux, so copying the value is
    // right).
    if (old_group->cgroup_path != NULL)
        group->cgroup_path = strdup(old_group->cgroup_path);
    if (group->tty) {
        lock(&group->tty->lock, 0);
        group->tty->refcount++;
        unlock(&group->tty->lock);
    }
    group->itimer = NULL;
    group->itimer_vprof_sampler = NULL;
    group->itimer_virtual = (typeof(group->itimer_virtual)) {};
    group->itimer_prof = (typeof(group->itimer_prof)) {};
    // POSIX timers (timer_create) are NOT inherited across fork() on Linux --
    // the child starts with none. The *group = *old_group shallow copy above
    // duplicated the parent's posix_timers[] (including the struct timer *
    // pointers), so a forked child that called timer_delete (e.g. stress-ng
    // --cpu-sched, whose children delete the inherited timer at startup) freed
    // a timer the parent still owned -> double free / use-after-free of the
    // running timer thread. Clear the array so the child inherits no timers.
    memset(group->posix_timers, 0, sizeof(group->posix_timers));
    group->doing_group_exit = false;
    group->continued = false;
    group->children_rusage = (struct rusage_) {};
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    lock_init(&group->lock, "tgroup_copy\0");
    return group;
}

static int copy_task(struct task *task, dword_t flags, guest_addr_t stack, guest_addr_t ptid_addr,
        guest_addr_t tls_addr, guest_addr_t ctid_addr) {
    task->vfork = NULL;
    if (stack != 0) {
        task->cpu.esp = (addr_t) stack;
        if (task->abi == GUEST_ABI_AMD64)
            task->cpu.amd64_regs[amd64_rsp] = stack;
        if (task->abi == GUEST_ABI_ARM64)
            task->cpu.arm64_sp = stack;
        if (task->abi == GUEST_ABI_RISCV64)
            task->cpu.riscv64_regs[riscv64_sp] = stack;
    }

    int err;
    struct mm *mm = task->mm;
    if (flags & CLONE_VM_) {
        mm_retain(mm);
    } else {
        struct mm *new_mm = mm_copy(mm);
        if (new_mm == NULL)
            // task->mm still aliases the parent's (unretained) mm from the
            // task_create_ struct copy; task_destroy doesn't touch it.
            return _ENOMEM;
        task_set_mm(task, new_mm);
    }

    if (flags & CLONE_FILES_) {
        task->files->refcount++;
    } else {
        task->files = fdtable_copy(task->files);
        if (IS_ERR(task->files)) {
            err = (int)PTR_ERR(task->files);
            goto fail_free_mem;
        }
    }

    err = _ENOMEM;
    if (flags & CLONE_FS_) {
        task->fs->refcount++;
    } else {
        task->fs = fs_info_copy(task->fs);
        if (task->fs == NULL)
            goto fail_free_files;
    }

    if (flags & CLONE_NEWUTS_) {
        task->uts_ns = uts_ns_copy(task->uts_ns);
        if (task->uts_ns == NULL)
            goto fail_free_fs;
    } else {
        uts_ns_retain(task->uts_ns);
    }

    if (flags & CLONE_SIGHAND_) {
        task->sighand->refcount++;
    } else {
        task->sighand = sighand_copy(task->sighand);
        if (task->sighand == NULL)
            goto fail_free_uts;
    }

    struct tgroup *old_group = task->group;
    struct tgroup *new_group = NULL;
    if (!(flags & CLONE_THREAD_)) {
        lock(&old_group->lock, 0);
        new_group = tgroup_copy(old_group);
        unlock(&old_group->lock);
        if (new_group == NULL) {
            err = _ENOMEM;
            goto fail_free_sighand;
        }
    } else {
        // New threads do not inherit the parent's alternate signal stack
        task->altstack = 0;
        task->altstack_size = 0;
    }

    complex_lockt(&pids_lock, 0);
    lock(&old_group->lock, 0);
    if (new_group != NULL) {
        list_add(&old_group->pgroup, &new_group->pgroup);
        list_add(&old_group->session, &new_group->session);
        task->group = new_group;
        task->group->leader = task;
        task->tgid = task->pid;
    }
    list_add(&task->group->threads, &task->group_links);
    unlock(&old_group->lock);
    unlock(&pids_lock);

    if (flags & CLONE_SETTLS_) {
        if (task->abi == GUEST_ABI_AMD64) {
            // On amd64, CLONE_SETTLS passes the new thread's FS base directly.
            task->cpu.tls_ptr = tls_addr;
        } else if (task->abi == GUEST_ABI_ARM64) {
            // On arm64, CLONE_SETTLS passes the new thread's TPIDR_EL0
            // directly, same shape as amd64's FS base — NOT an i386
            // user_desc pointer, which is what task_set_thread_area
            // would try to read tls_addr as.
            task->cpu.arm64_tpidr = tls_addr;
        } else if (task->abi == GUEST_ABI_RISCV64) {
            // On riscv64, CLONE_SETTLS passes the new thread's tp value
            // directly; tp is an ordinary GPR (x4), not an out-of-band
            // register like arm64's TPIDR_EL0.
            task->cpu.riscv64_regs[riscv64_tp] = tls_addr;
        } else {
            err = task_set_thread_area(task, (addr_t) tls_addr);
            if (err < 0)
                goto fail_unlink_group;
        }
    }

    err = _EFAULT;
    if (flags & CLONE_CHILD_SETTID_)
        if (user_put_task(task, ctid_addr, task->pid))
            goto fail_unlink_group;
    if (flags & CLONE_PARENT_SETTID_) {
        if (user_put(ptid_addr, task->pid))
            goto fail_unlink_group;
    } else if (flags & CLONE_PIDFD_) {
        // Reuses the parent_tid slot to instead receive a pidfd for the new
        // task, installed in the *caller's* fd table -- copy_task runs in
        // the calling (parent) thread's context before the child starts, so
        // `current` here is the parent and f_install (which operates on
        // current->files) lands the fd in the right table. Mutual exclusion
        // with CLONE_PARENT_SETTID/CLONE_THREAD already checked in
        // sys_clone_common.
        struct fd *pidfd = pidfd_create(task);
        if (IS_ERR(pidfd)) {
            err = PTR_ERR(pidfd);
            goto fail_unlink_group;
        }
        // Linux unconditionally sets close-on-exec on a CLONE_PIDFD fd (same
        // as pidfd_open); see kernel/pidfd.c's sys_pidfd_open for why an
        // uncloseexeced self-pidfd deadlocks do_exit.
        fd_t pidfd_num = f_install(pidfd, O_CLOEXEC_);
        if (pidfd_num < 0) {
            err = pidfd_num;
            goto fail_unlink_group;
        }
        if (user_put(ptid_addr, pidfd_num))
            goto fail_unlink_group;
    }
    if (flags & CLONE_CHILD_CLEARTID_)
        task->clear_tid = ctid_addr;
    task->exit_signal = flags & CSIGNAL_;

    // remember to do CLONE_SYSVSEM
    return 0;

fail_unlink_group:
    // Undo the session/pgroup/threads linkage performed above before the task is
    // destroyed. The normal exit path unlinks these (kernel/exit.c:73,552-554)
    // before the group object is freed; skipping it here lets task_free_final()
    // free new_group while its pgroup/session nodes are still linked into the
    // live pid-rooted lists -- a use-after-free. Reached only from failures that
    // occur after the linkage (CLONE_SETTLS / CLONE_*_SETTID above).
    complex_lockt(&pids_lock, 0);
    lock(&old_group->lock, 0);
    list_remove(&task->group_links);
    if (new_group != NULL) {
        list_remove(&new_group->pgroup);
        list_remove(&new_group->session);
    }
    unlock(&old_group->lock);
    unlock(&pids_lock);
fail_free_sighand:
    while(task_ref_cnt_get(task, 0)) { // Wait for now, task is in one or more critical sections
        nanosleep(&lock_pause, NULL);
    }
    // The half-created task stays visible in the pid table until
    // sys_clone_common's task_destroy, and the ref-drain wait above doesn't
    // stop a NEW reader from grabbing a task reference afterward. Cross-task
    // readers reach these resources through the task: procfs and
    // user_read_task/user_write_task retain task->mm / task->files / task->fs
    // under general_lock, and signal senders read task->sighand under
    // pids_lock. Detach each pointer under the lock its readers use BEFORE
    // releasing it -- releasing while still attached let a concurrent
    // /proc/<pid>/stat read retain the mm as it was freed, walk freed page
    // tables, and double-free it, corrupting the malloc freelist (the
    // app-wide mm_copy heap-corruption crash, issue #463). Same invariant
    // exec.c (elf_exec) and exit.c (do_exit) already follow.
    complex_lockt(&pids_lock, 0);
    struct sighand *dead_sighand = task->sighand;
    task->sighand = NULL;
    unlock(&pids_lock);
    sighand_release(dead_sighand);
fail_free_uts:
    lock(&task->general_lock, 0);
    struct uts_namespace *dead_uts = task->uts_ns;
    task->uts_ns = NULL;
    unlock(&task->general_lock);
    uts_ns_release(dead_uts);
fail_free_fs:
    lock(&task->general_lock, 0);
    struct fs_info *dead_fs = task->fs;
    task->fs = NULL;
    unlock(&task->general_lock);
    fs_info_release(dead_fs);
fail_free_files:
    lock(&task->general_lock, 0);
    struct fdtable *dead_files = task->files;
    task->files = NULL;
    unlock(&task->general_lock);
    fdtable_release(dead_files);
fail_free_mem:
    lock(&task->general_lock, 0);
    struct mm *dead_mm = task->mm;
    task->mm = NULL;
    task->mem = NULL;
    task->cpu.mmu = NULL;
    unlock(&task->general_lock);
    mm_release(dead_mm);
    return err;
}

// Tear down a task that was fully created (copy_task or fork+exec succeeded)
// but whose host thread could not be started (task_start failure). The task
// has never executed a single instruction, so no exit notification is owed
// to anyone: unlink it from the pid table and from every group list exactly
// as copy_task's fail_unlink_group would, then release the resources do_exit
// would have released (task_free_final only frees the task/group structs).
// The pid leaves the table before the resource release so no /proc walk or
// signal sender can observe a half-torn-down task.
void task_never_ran_destroy(struct task *task) {
    struct tgroup *parent_group = task->parent != NULL ? task->parent->group : task->group;
    complex_lockt(&pids_lock, 0);
    lock(&parent_group->lock, 0);
    list_remove(&task->group_links);
    if (task->group != parent_group) {
        // non-CLONE_THREAD: the fresh tgroup's session/pgroup nodes are
        // linked into the live pid-rooted lists (see copy_task)
        list_remove(&task->group->pgroup);
        list_remove(&task->group->session);
    }
    unlock(&parent_group->lock);
    task_unlink_locked(task);
    // Detach resources before releasing them, under the locks their readers
    // use (see the matching comment in copy_task's error path): a procfs
    // reader that took a task reference before task_unlink_locked can still
    // retain task->mm/files/fs via general_lock after the unlink, and the
    // old release-then-NULL order let it retain a freed object.
    struct sighand *dead_sighand = task->sighand;
    task->sighand = NULL;
    unlock(&pids_lock);
    lock(&task->general_lock, 0);
    struct fs_info *dead_fs = task->fs;
    task->fs = NULL;
    struct uts_namespace *dead_uts = task->uts_ns;
    task->uts_ns = NULL;
    struct fdtable *dead_files = task->files;
    task->files = NULL;
    struct mm *dead_mm = task->mm;
    task->mm = NULL;
    task->mem = NULL;
    task->cpu.mmu = NULL;
    unlock(&task->general_lock);
    sighand_release(dead_sighand);
    fs_info_release(dead_fs);
    uts_ns_release(dead_uts);
    fdtable_release(dead_files);
    mm_release(dead_mm);
    task_destroy_unlinked(task, 3);
}

// Decode unimplemented clone flags into a readable '|'-joined list. The runtime
// FIXME below is the primary signal for prioritizing clone-flag support from
// real-world logs, so it should report *which* feature a program wanted (e.g.
// CLONE_NEWUSER|CLONE_NEWPID = unprivileged container setup) instead of a bare
// hex residual. Any bit we have no name for is appended as hex.
static void clone_flag_names(dword_t flags, char *buf, size_t bufsize) {
    static const struct { dword_t bit; const char *name; } table[] = {
        {CLONE_PIDFD_, "CLONE_PIDFD"},
        {CLONE_PTRACE_, "CLONE_PTRACE"},
        {CLONE_PARENT_, "CLONE_PARENT"},
        {CLONE_NEWNS_, "CLONE_NEWNS"},
        {CLONE_UNTRACED_, "CLONE_UNTRACED"},
        {CLONE_NEWCGROUP_, "CLONE_NEWCGROUP"},
        {CLONE_NEWUTS_, "CLONE_NEWUTS"},
        {CLONE_NEWIPC_, "CLONE_NEWIPC"},
        {CLONE_NEWUSER_, "CLONE_NEWUSER"},
        {CLONE_NEWPID_, "CLONE_NEWPID"},
        {CLONE_NEWNET_, "CLONE_NEWNET"},
        {CLONE_IO_, "CLONE_IO"},
    };
    size_t len = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
        if (!(flags & table[i].bit))
            continue;
        int n = snprintf(buf + len, bufsize - len, "%s%s", len ? "|" : "", table[i].name);
        if (n < 0 || (size_t) n >= bufsize - len)
            return;
        len += (size_t) n;
        flags &= ~table[i].bit;
    }
    if (flags != 0)
        snprintf(buf + len, bufsize - len, "%s%#x", len ? "|" : "", flags);
}

// Drop one reference to a vfork_info, destroying it on the last one.
//
// acq_rel rather than release: a release-only decrement gives the thread that
// happens to perform the final decrement no happens-before edge with the other
// side's decrement, so it could destroy the struct without observing that
// side's writes to it. That matters more than usual here because the object
// being freed contains the mutex both sides were using.
static void vfork_info_release(struct vfork_info *vfork) {
    if (atomic_fetch_sub_explicit(&vfork->refcount, 1, memory_order_acq_rel) == 1) {
        cond_destroy(&vfork->cond);
        free(vfork);
    }
}

static dword_t sys_clone_common(dword_t flags, guest_addr_t stack, guest_addr_t ptid,
        guest_addr_t tls, guest_addr_t ctid) {
    STRACE("clone(0x%x, 0x%x, 0x%x, 0x%x, 0x%x)", flags, stack, ptid, tls, ctid);
    if (flags & CLONE_NEW_FLAGS_)
        return _EPERM;
    // Creating any namespace needs CAP_SYS_ADMIN in real Linux.
    if ((flags & CLONE_NEWUTS_) && !superuser())
        return _EPERM;
    // The low byte of flags (or clone3's separate exit_signal field, folded in
    // by sys_clone3_guest) becomes task->exit_signal and is later handed to
    // send_signal() uninspected when the child exits. Out-of-range values
    // (e.g. stress-ng's clone stressor passing garbage here) would otherwise
    // reach sig_mask()'s assert and abort the whole process.
    if ((flags & CSIGNAL_) >= NUM_SIGS)
        return _EINVAL;
    dword_t unimpl_flags = flags & ~CSIGNAL_ & ~IMPLEMENTED_FLAGS;
    if (unimpl_flags) {
        char names[256];
        clone_flag_names(unimpl_flags, names, sizeof(names));
        FIXME("unimplemented clone flags %s (requested %#x) from %s[%d]",
              names, flags, current->comm, current->pid);
        return _EINVAL;
    }
    if (flags & CLONE_SIGHAND_ && !(flags & CLONE_VM_))
        return _EINVAL;
    if (flags & CLONE_THREAD_ && !(flags & CLONE_SIGHAND_))
        return _EINVAL;
    // CLONE_PARENT with no parent to attach to (only possible for the
    // initial/init task) -- matches real Linux's EINVAL here.
    if (flags & CLONE_PARENT_ && current->parent == NULL)
        return _EINVAL;
    // CLONE_PIDFD reuses the parent_tid argument slot (to instead receive a
    // pidfd) and needs a stable, separately-waitable pid -- both restrictions
    // match real Linux's clone(2).
    if (flags & CLONE_PIDFD_ && flags & CLONE_PARENT_SETTID_)
        return _EINVAL;
    if (flags & CLONE_PIDFD_ && flags & CLONE_THREAD_)
        return _EINVAL;

    // RLIMIT_NPROC caps how many processes ONE USER may have, so a runaway
    // fork loop cannot take the whole system down with it. It was stored and
    // reported and never consulted, so `ulimit -u` was decorative. Threads do
    // not count against it -- Linux charges the limit in copy_process, which
    // CLONE_THREAD reaches too, but the count it compares against is of
    // processes; keeping threads out of both sides is the same answer with
    // less bookkeeping. root is exempt, as there.
    if (!(flags & CLONE_THREAD_)) {
        rlim_t_ nproc = rlimit(RLIMIT_NPROC_);
        if (nproc != RLIM_INFINITY_ && !current_capable(CAP_SYS_RESOURCE_) &&
                !current_capable(CAP_SYS_ADMIN_) &&
                task_count_for_uid(current->uid) >= nproc)
            return _EAGAIN;
    }

    struct task *task = task_create_(current);
    if (task == NULL)
        return _ENOMEM;
    if (flags & CLONE_PARENT_) {
        // Reparent to the caller's own parent, so the new task becomes
        // current's sibling instead of its child. task_create_ always links
        // to its `parent` argument's children list *and* uses that argument
        // as the template for the new task's inherited state (uid/gid/caps/
        // etc, via `*task = *parent`) -- re-link the family-tree bookkeeping
        // here instead of passing current->parent to task_create_, so the
        // new task still inherits current's (possibly since-diverged) state,
        // not the grandparent's.
        complex_lockt(&pids_lock, 0);
        list_remove(&task->siblings);
        list_add(&current->parent->children, &task->siblings);
        task->parent = current->parent;
        unlock(&pids_lock);
    }
    int err = copy_task(task, flags, stack, ptid, tls, ctid);
    if (err < 0) {
        // FIXME: there is a window between task_create_ and task_destroy where
        // some other thread could get a pointer to the task.
        // FIXME: task_destroy doesn't free all aspects of the task, which
        // could cause leaks
        // Debug hook (issue #463): widen the copy_task-failure window to make
        // the procfs-reader-vs-error-path race reproducible on demand.
        const char *fail_delay = getenv("ISH_DEBUG_CLONE_FAIL_DELAY_US");
        if (fail_delay != NULL)
            usleep((unsigned) atoi(fail_delay));
        complex_lockt(&pids_lock, 0);
        task_destroy(task, 3);
        unlock(&pids_lock);
        
        return err;
    }
    task->cpu.eax = 0;
    if (task->abi == GUEST_ABI_AMD64)
        task->cpu.amd64_regs[amd64_rax] = 0;
    if (task->abi == GUEST_ABI_ARM64)
        // The child returns 0 from clone in X0. Without this, the child
        // resumes with the parent's copied X0 (the clone flags argument)
        // as its "pid", and both sides run the parent path — the actual
        // first-fork failure mode observed bringing up busybox sh.
        task->cpu.arm64_regs[arm64_x0] = 0;
    if (task->abi == GUEST_ABI_RISCV64)
        // Same rule: the child returns 0 from clone in a0.
        task->cpu.riscv64_regs[riscv64_a0] = 0;

    struct vfork_info *vfork = NULL;
    if (flags & CLONE_VFORK_) {
        vfork = malloc(sizeof(struct vfork_info));
        if (vfork == NULL) {
            task_never_ran_destroy(task);
            return _ENOMEM;
        }
        lock_init(&vfork->lock, "sys_clone\0");
        cond_init(&vfork->cond);
        vfork->done = false;
        // One reference for us, one for the child; the child's is released by
        // vfork_notify() at its exec or exit. See struct vfork_info in task.h
        // for why this cannot be a stack allocation.
        atomic_init(&vfork->refcount, 2);
        task->vfork = vfork;
    }

    // task might be destroyed by the time we finish, so save the pid
    pid_t pid = task->pid;
    if (amd64_trace_is_lineage_tgid(current->tgid)) {
        printk("tracked kernel child: parent=%d tgid=%d abi=%d child=%d child_tgid=%d flags=%#x\n",
               current->pid, current->tgid, current->abi, pid, task->tgid, flags);
    }
    bool trace_child = false;
    int ptrace_event = 0;
    if (current->ptrace.traced && !(flags & CLONE_UNTRACED_)) {
        dword_t trace_option = 0;
        if (flags & CLONE_VFORK_)
            trace_option = PTRACE_O_TRACEVFORK_;
        else if (flags & CLONE_THREAD_)
            trace_option = PTRACE_O_TRACECLONE_;
        else
            trace_option = PTRACE_O_TRACEFORK_;

        if (flags & CLONE_VFORK_)
            ptrace_event = PTRACE_EVENT_VFORK_;
        else if (flags & CLONE_THREAD_)
            ptrace_event = PTRACE_EVENT_CLONE_;
        else
            ptrace_event = PTRACE_EVENT_FORK_;

        if (current->ptrace.options & trace_option) {
            ptrace_attach_fork_child(task, current);
            trace_child = true;
        }
    }

    if (task_start(task) < 0) {
        // Host thread limit or memory exhaustion: the child never ran.
        // Unwind completely and give the guest a clean EAGAIN, matching
        // Linux clone() at the thread/rlimit ceiling. (Previously this
        // failure was silently swallowed — see task_start — leaving a
        // ghost task linked in the pid table and thread-group lists.)
        task->vfork = NULL;
        task_never_ran_destroy(task);
        if (flags & CLONE_VFORK_) {
            // Sole owner: the child never ran, so it will never reach
            // vfork_notify() to release the reference taken on its behalf.
            // Going through vfork_info_release() here would leave the count at
            // 1 and leak the allocation.
            cond_destroy(&vfork->cond);
            free(vfork);
        }
        return _EAGAIN;
    }
    if (trace_child)
        send_signal(task, SIGSTOP_, SIGINFO_NIL);
    if (trace_child) {
        struct siginfo_ info = {
            .sig = SIGTRAP_,
            .code = SI_KERNEL_,
            .kill.pid = current->pid,
            .kill.uid = current->uid,
        };
        ptrace_event_stop(SIGTRAP_, &info, ptrace_event, pid);
    }

    if (flags & CLONE_VFORK_) {
        // Linux makes this wait killable but not interruptible, and the
        // distinction is load-bearing: until the child execs or _exits it is
        // running on the parent's stack, so a parent that stopped waiting and
        // resumed guest code would execute over frames the child is still
        // using. Only a signal that will terminate us qualifies, and after one
        // of those we run no guest instructions at all -- handle_interrupt()
        // calls receive_signals() before the JIT is re-entered.
        bool fatal = false;
        lock(&vfork->lock, 0);
        while (!vfork->done) {
            lock(&current->sighand->lock, 0);
            sigset_t_ pending = current->pending | current->sighand->pending;
            pending &= ~current->blocked;
            // sigset_has() asserts 1 <= sig < NUM_SIGS (64), so this covers
            // every signal that can be represented in the mask.
            for (int sig = 1; sig <= 63 && !fatal; sig++) {
                if (sigset_has(pending, sig) &&
                        signal_action(current->sighand, sig) == SIGNAL_KILL)
                    fatal = true;
            }
            unlock(&current->sighand->lock);
            if (fatal)
                break;
            // send_signal() wakes even an ignore-signals wait, through
            // task->waiting_cond, so a signal that arrives mid-wait gets the
            // scan above re-run rather than sitting unnoticed until the child
            // finishes.
            wait_for_ignore_signals(&vfork->cond, &vfork->lock, NULL);
        }
        unlock(&vfork->lock);

        // Deliberately NOT clearing task->vfork: that is the only handle the
        // child has on this struct, and on the fatal path above the child is
        // still running and has not released its reference yet. Clearing it
        // here would strand that reference and leak the allocation in exactly
        // the case this wait exists to handle. The stack-allocated version had
        // to unhook it because the struct died with this frame; the heap
        // allocation is what lifts that requirement.
        //
        // We also leave the child alone. Linux does not kill a vfork child
        // whose parent takes a fatal signal -- the child runs on to its exec or
        // _exit with the shared mm kept alive by its own reference, which is
        // equally true here (mem is refcounted). Killing it would additionally
        // be racy: the child may have already completed its execve and be
        // blocked on vfork->lock on its way to setting done, in which case it
        // no longer shares our stack and is a legitimately running new program.
        vfork_info_release(vfork);
    }

    return pid;
}

// A child of `current` with fork semantics, for a caller that will exec into
// it immediately. Exists so natively-implemented programs (kernel/native.h)
// can start a real guest task -- there is one host process, so a native
// program's child cannot be a host process and has to be an AOK task.
//
// copy_task is static here and the error unwinding is fiddly, so this is
// exported rather than reimplemented at the call site.
//
// The mm copy is wasted work when the caller execs straight away, exactly as
// it is for a guest's own fork+exec; correctness first.
struct task *task_fork_for_exec(void) {
    struct task *task = task_create_(current);
    if (task == NULL)
        return NULL;
    // SIGCHLD_ as the exit signal, exactly as a guest fork() passes it in the
    // low byte of clone's flags. This was 0, meaning "tell nobody when this
    // task dies", and that was invisible for as long as every caller reaped
    // its child with a blocking waitpid -- which is what native_spawn's own
    // helpers and native bash both do.
    //
    // A native SHELL does not. zsh waits for a job by sleeping in sigsuspend
    // until its SIGCHLD handler reaps, so with no exit signal the child
    // finished, became a zombie, and the parent slept forever: the shell
    // printed the output of its first external command and then hung.
    int err = copy_task(task, SIGCHLD_, 0, 0, 0, 0);
    if (err < 0) {
        complex_lockt(&pids_lock, 0);
        task_destroy(task, 3);
        unlock(&pids_lock);
        return NULL;
    }
    return task;
}

dword_t sys_clone(dword_t flags, addr_t stack, addr_t ptid, addr_t tls, addr_t ctid) {
    return sys_clone_common(flags, stack, ptid, tls, ctid);
}

dword_t sys_clone_guest(qword_t flags, guest_addr_t stack, guest_addr_t ptid,
        guest_addr_t tls, guest_addr_t ctid) {
    if ((flags >> 32) != 0)
        return _ENOSYS;
    return sys_clone_common((dword_t) flags, stack, ptid, tls, ctid);
}

struct clone_args_ {
    qword_t flags;
    qword_t pidfd;
    qword_t child_tid;
    qword_t parent_tid;
    qword_t exit_signal;
    qword_t stack;
    qword_t stack_size;
    qword_t tls;
    qword_t set_tid;
    qword_t set_tid_size;
    qword_t cgroup;
};

dword_t sys_clone3_guest(guest_addr_t uargs_addr, dword_t size) {
    STRACE("clone3(%#x, %u)", uargs_addr, size);

    struct clone_args_ args = {};
    if (size < offsetof(struct clone_args_, tls) + sizeof(args.tls))
        return _EINVAL;
    if (user_read(uargs_addr, &args, size < sizeof(args) ? size : sizeof(args)))
        return _EFAULT;

    // CLONE_CLEAR_SIGHAND (bit 32, Linux 5.5+) asks the kernel to reset the
    // child's signal handlers to SIG_DFL as part of the clone, instead of
    // glibc iterating and resetting each one itself after the fact. We
    // already give every new task a fresh (non-shared, unless CLONE_SIGHAND)
    // sighand via copy_task, so honoring this is a no-op: nothing forces the
    // handlers to non-default that this would need to undo. glibc's
    // posix_spawn sets it unconditionally on every clone3 call (used by
    // systemd's posix_spawn_wrapper for every service start), so rejecting
    // it as ENOSYS made every service spawn via clone3 fail outright.
    if ((args.flags >> 32) != 0 && (args.flags >> 32) != 1)
        return _ENOSYS;
    if (args.set_tid != 0 || args.set_tid_size != 0 || args.cgroup != 0)
        return _ENOSYS;

    dword_t flags = (dword_t) args.flags;
    dword_t exit_signal = (dword_t) args.exit_signal;
    if ((args.exit_signal >> 32) != 0)
        return _EINVAL;
    if ((flags & CSIGNAL_) != 0 && (flags & CSIGNAL_) != exit_signal)
        return _EINVAL;
    flags = (flags & ~CSIGNAL_) | exit_signal;

    qword_t child_stack = args.stack;
    if (child_stack != 0 && args.stack_size != 0)
        child_stack += args.stack_size;
    // clone3's `pidfd` field is CLONE_PIDFD's output slot -- sys_clone_common
    // already implements CLONE_PIDFD by writing the new pidfd's fd number to
    // whatever `ptid` address it's given (the same argument plain clone(2)
    // repurposes for CLONE_PARENT_SETTID vs CLONE_PIDFD; see its comment).
    // Reuse that here instead of rejecting clone3(CLONE_PIDFD) outright --
    // glibc's pidfd_spawn (systemd's posix_spawn_wrapper for every service
    // start) probes clone3+CLONE_PIDFD support before ever calling it for
    // real, so an unconditional ENOSYS here made every service spawn fail.
    guest_addr_t ptid = (flags & CLONE_PIDFD_) ? (guest_addr_t) args.pidfd : (guest_addr_t) args.parent_tid;
    return sys_clone_common(flags, child_stack, ptid, args.tls, args.child_tid);
}

dword_t sys_clone3(addr_t uargs_addr, dword_t size) {
    return sys_clone3_guest(uargs_addr, size);
}

dword_t sys_unshare(dword_t flags) {
    STRACE("unshare(%#x)", flags);

    const dword_t supported = CLONE_FILES_ | CLONE_FS_ | CLONE_SYSVSEM_ | CLONE_NEWUTS_;
    const dword_t known_unsupported = CLONE_VM_ | CLONE_SIGHAND_ | CLONE_THREAD_ |
        CLONE_NEWNS_ | CLONE_NEWCGROUP_ | CLONE_NEWIPC_ |
        CLONE_NEWUSER_ | CLONE_NEWPID_ | CLONE_NEWNET_ | CLONE_IO_;
    const dword_t known = supported | known_unsupported;

    if (flags & ~known)
        return _EINVAL;
    if (flags & known_unsupported)
        return _ENOSYS;
    if ((flags & CLONE_NEWUTS_) && !superuser())
        return _EPERM;

    if (flags & CLONE_NEWUTS_) {
        struct uts_namespace *new_uts = uts_ns_copy(current->uts_ns);
        if (new_uts == NULL)
            return _ENOMEM;
        struct uts_namespace *old_uts = current->uts_ns;
        current->uts_ns = new_uts;
        uts_ns_release(old_uts);
    }

    if (flags & CLONE_FILES_) {
        int err = fdtable_unshare_current();
        if (err < 0)
            return err;
    }

    if ((flags & CLONE_FS_) && current->fs->refcount != 1) {
        struct fs_info *old_fs = current->fs;
        struct fs_info *new_fs = fs_info_copy(old_fs);
        if (new_fs == NULL)
            return _ENOMEM;
        current->fs = new_fs;
        fs_info_release(old_fs);
    }

    // SysV semaphore undo lists are not modeled separately, so treat this as a no-op.
    return 0;
}

// kcmp(2): tell whether two processes share a kernel resource. systemd is
// the motivating caller -- it uses KCMP_FILE to dedup its fd store and its
// serialization fds across daemon-reexec, and PID 1 logged an
// "arm64 stub syscall 272" ERROR line for every single probe (dozens per
// reload). Linux's return contract for the comparison types is an ORDERING,
// not a boolean: 0 if equal, 1 if the first sorts lower, 2 if higher (the
// kernel obfuscates the pointers before comparing; the ordering just has to
// be consistent within a type, which raw pointers give us).
#define KCMP_FILE_ 0
#define KCMP_VM_ 1
#define KCMP_FILES_ 2
#define KCMP_FS_ 3
#define KCMP_SIGHAND_ 4
#define KCMP_IO_ 5
#define KCMP_SYSVSEM_ 6
#define KCMP_EPOLL_TFD_ 7

static int kcmp_ptr(const void *a, const void *b) {
    if (a == b)
        return 0;
    return a < b ? 1 : 2;
}

dword_t sys_kcmp(pid_t_ pid1, pid_t_ pid2, dword_t type, dword_t idx1, dword_t idx2) {
    STRACE("kcmp(%d, %d, %d, %d, %d)", pid1, pid2, type, idx1, idx2);
    complex_lockt(&pids_lock, 0);
    struct task *t1 = pid_get_task(pid1);
    struct task *t2 = pid_get_task(pid2);
    if (t1 == NULL || t2 == NULL) {
        unlock(&pids_lock);
        return _ESRCH;
    }
    int res;
    switch (type) {
        case KCMP_FILE_: {
            // Same struct fd == same open file description. Sequential
            // lock/lookup/unlock per table (no nested table locks, so two
            // concurrent kcmps can't AB-BA); the pointers are only compared,
            // never dereferenced, and both tasks are pinned by pids_lock.
            lock(&t1->files->lock, 0);
            struct fd *f1 = fdtable_get(t1->files, (fd_t) idx1);
            unlock(&t1->files->lock);
            lock(&t2->files->lock, 0);
            struct fd *f2 = fdtable_get(t2->files, (fd_t) idx2);
            unlock(&t2->files->lock);
            if (f1 == NULL || f2 == NULL)
                res = _EBADF;
            else
                res = kcmp_ptr(f1, f2);
            break;
        }
        case KCMP_VM_: res = kcmp_ptr(t1->mem, t2->mem); break;
        case KCMP_FILES_: res = kcmp_ptr(t1->files, t2->files); break;
        case KCMP_FS_: res = kcmp_ptr(t1->fs, t2->fs); break;
        case KCMP_SIGHAND_: res = kcmp_ptr(t1->sighand, t2->sighand); break;
        // No io_context / sysvsem-undo / epoll-tfd introspection modeled;
        // EOPNOTSUPP matches a kernel built without the facility.
        case KCMP_IO_:
        case KCMP_SYSVSEM_:
        case KCMP_EPOLL_TFD_:
            res = _EOPNOTSUPP;
            break;
        default:
            res = _EINVAL;
            break;
    }
    unlock(&pids_lock);
    return res;
}

dword_t sys_fork(void) {
    return sys_clone(SIGCHLD_, 0, 0, 0, 0);
}

dword_t sys_vfork(void) {
    return sys_clone(CLONE_VFORK_ | CLONE_VM_ | SIGCHLD_, 0, 0, 0, 0);
}

void vfork_notify(struct task *task) {
    if (task == NULL || task->pid > MAX_PID)
        return;

    // Callers already own the task lifetime here, and do_exit() can invoke us
    // while still holding task->general_lock. Re-locking it here self-deadlocks
    // the exiting task and wedges any later pids_lock users behind it. So the
    // handle is claimed with an exchange instead of under a lock.
    //
    // Claiming it (rather than just reading it) is what makes us idempotent,
    // which is required: a vfork child that execs and later exits calls us
    // TWICE, from exec.c and then from exit.c, and must release the child's
    // single reference exactly once.
    struct vfork_info *vfork = atomic_exchange(&task->vfork, NULL);
    if (vfork == NULL)
        return;

    lock(&vfork->lock, 0);
    vfork->done = true;
    notify(&vfork->cond);
    unlock(&vfork->lock);

    vfork_info_release(vfork);
}
