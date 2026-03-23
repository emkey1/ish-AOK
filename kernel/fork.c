#include "debug.h"
#include "kernel/task.h"
#include "fs/fd.h"
#include "fs/tty.h"
#include "kernel/calls.h"
#include "kernel/mm.h"
#include "kernel/ptrace.h"
#include "util/sync.h"

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
#define IMPLEMENTED_FLAGS (CLONE_VM_|CLONE_FILES_|CLONE_FS_|CLONE_SIGHAND_|CLONE_SYSVSEM_|CLONE_VFORK_|CLONE_THREAD_|\
        CLONE_SETTLS_|CLONE_CHILD_SETTID_|CLONE_PARENT_SETTID_|CLONE_CHILD_CLEARTID_|CLONE_DETACHED_)

static struct tgroup *tgroup_copy(struct tgroup *old_group) {
    struct tgroup *group = malloc(sizeof(struct tgroup));
    *group = *old_group;
    list_init(&group->threads);
    list_add(&old_group->pgroup, &group->pgroup);
    list_add(&old_group->session, &group->session);
    if (group->tty) {
        lock(&group->tty->lock, 0);
        group->tty->refcount++;
        unlock(&group->tty->lock);
    }
    group->itimer = NULL;
    group->doing_group_exit = false;
    group->children_rusage = (struct rusage_) {};
    cond_init(&group->child_exit);
    cond_init(&group->stopped_cond);
    lock_init(&group->lock, "tgroup_copy\0");
    return group;
}

static int copy_task(struct task *task, dword_t flags, addr_t stack, addr_t ptid_addr, addr_t tls_addr, addr_t ctid_addr) {
    task->vfork = NULL;
    if (stack != 0)
        task->cpu.esp = stack;

    int err;
    struct mm *mm = task->mm;
    if (flags & CLONE_VM_) {
        mm_retain(mm);
    } else {
        task_set_mm(task, mm_copy(mm));
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

    if (flags & CLONE_SIGHAND_) {
        task->sighand->refcount++;
    } else {
        task->sighand = sighand_copy(task->sighand);
        if (task->sighand == NULL)
            goto fail_free_fs;
    }

    struct tgroup *old_group = task->group;
    complex_lockt(&pids_lock, 0);
    lock(&old_group->lock, 0);
    if (!(flags & CLONE_THREAD_)) {
        task->group = tgroup_copy(old_group);
        task->group->leader = task;
        task->tgid = task->pid;
    }
    list_add(&task->group->threads, &task->group_links);
    unlock(&old_group->lock);
    unlock(&pids_lock);

    if (flags & CLONE_SETTLS_) {
        err = task_set_thread_area(task, tls_addr);
        if (err < 0)
            goto fail_free_sighand;
    }

    err = _EFAULT;
    if (flags & CLONE_CHILD_SETTID_)
        if (user_put_task(task, ctid_addr, task->pid))
            goto fail_free_sighand;
    if (flags & CLONE_PARENT_SETTID_)
        if (user_put(ptid_addr, task->pid))
            goto fail_free_sighand;
    if (flags & CLONE_CHILD_CLEARTID_)
        task->clear_tid = ctid_addr;
    task->exit_signal = flags & CSIGNAL_;

    // remember to do CLONE_SYSVSEM
    return 0;

fail_free_sighand:
    while(task_ref_cnt_get(task, 0)) { // Wait for now, task is in one or more critical sections
        nanosleep(&lock_pause, NULL);
    }
    sighand_release(task->sighand);
fail_free_fs:
    fs_info_release(task->fs);
fail_free_files:
    fdtable_release(task->files);
fail_free_mem:
    mm_release(task->mm);
    return err;
}

dword_t sys_clone(dword_t flags, addr_t stack, addr_t ptid, addr_t tls, addr_t ctid) {
    STRACE("clone(0x%x, 0x%x, 0x%x, 0x%x, 0x%x)", flags, stack, ptid, tls, ctid);
    if (strcmp(current->comm, "apt") == 0 || strcmp(current->comm, "sudo") == 0) {
        printk("APTTRACE clone parent_pid=%d comm=%s flags=%#x stack=%#x ptid=%#x tls=%#x ctid=%#x\n",
            current->pid, current->comm, flags, stack, ptid, tls, ctid);
    }
    if (flags & ~CSIGNAL_ & ~IMPLEMENTED_FLAGS) {
        FIXME("unimplemented clone flags 0x%x", flags & ~CSIGNAL_ & ~IMPLEMENTED_FLAGS);
        return _EINVAL;
    }
    if (flags & CLONE_SIGHAND_ && !(flags & CLONE_VM_))
        return _EINVAL;
    if (flags & CLONE_THREAD_ && !(flags & CLONE_SIGHAND_))
        return _EINVAL;

    struct task *task = task_create_(current);
    if (task == NULL)
        return _ENOMEM;
    int err = copy_task(task, flags, stack, ptid, tls, ctid);
    if (err < 0) {
        // FIXME: there is a window between task_create_ and task_destroy where
        // some other thread could get a pointer to the task.
        // FIXME: task_destroy doesn't free all aspects of the task, which
        // could cause leaks
        complex_lockt(&pids_lock, 0);
        task_destroy(task, 3);
        unlock(&pids_lock);
        
        return err;
    }
    task->cpu.eax = 0;

    struct vfork_info vfork;
    if (flags & CLONE_VFORK_) {
        lock_init(&vfork.lock, "sys_clone\0");
        cond_init(&vfork.cond);
        vfork.done = false;
        task->vfork = &vfork;
    }

    // task might be destroyed by the time we finish, so save the pid
    pid_t pid = task->pid;
    if ((strcmp(current->comm, "apt") == 0 || strcmp(current->comm, "sudo") == 0) &&
            (flags & CLONE_VFORK_)) {
        printk("APTTRACE vfork child pid=%d parent_pid=%d shared_vm=%d\n",
            pid, current->pid, !!(flags & CLONE_VM_));
    }
    if ((strcmp(current->comm, "apt") == 0 || strcmp(current->comm, "sudo") == 0) &&
            !(flags & CLONE_THREAD_)) {
        task->apt_syscall_trace_remaining = 24;
        printk("APTTRACE child_trace_arm pid=%d parent_pid=%d flags=%#x remaining=%u\n",
            pid, current->pid, flags, task->apt_syscall_trace_remaining);
    }

    if (current->ptrace.traced) {
        current->ptrace.trap_event = PTRACE_EVENT_FORK_;
        send_signal(current, SIGTRAP_, SIGINFO_NIL);
    }

    task_start(task);

    if (flags & CLONE_VFORK_) {
        lock(&vfork.lock, 0);
        while (!vfork.done)
            // FIXME this should stop waiting if a fatal signal is received
            wait_for_ignore_signals(&vfork.cond, &vfork.lock, NULL);
        unlock(&vfork.lock);
        if (strcmp(current->comm, "apt") == 0 || strcmp(current->comm, "sudo") == 0)
            printk("APTTRACE vfork parent_resume parent_pid=%d child_pid=%d\n", current->pid, pid);
        lock(&task->general_lock, 0);
        task->vfork = NULL;
        unlock(&task->general_lock);
        cond_destroy(&vfork.cond);
    }

    return pid;
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

dword_t sys_clone3(addr_t uargs_addr, dword_t size) {
    STRACE("clone3(%#x, %u)", uargs_addr, size);

    struct clone_args_ args = {};
    if (size < offsetof(struct clone_args_, tls) + sizeof(args.tls))
        return _EINVAL;
    if (user_read(uargs_addr, &args, size < sizeof(args) ? size : sizeof(args)))
        return _EFAULT;

    if ((args.flags >> 32) != 0)
        return _ENOSYS;
    if (args.pidfd != 0 || args.set_tid != 0 || args.set_tid_size != 0 || args.cgroup != 0)
        return _ENOSYS;

    dword_t flags = (dword_t) args.flags;
    dword_t exit_signal = (dword_t) args.exit_signal;
    if ((args.exit_signal >> 32) != 0)
        return _EINVAL;
    if ((flags & CSIGNAL_) != 0 && (flags & CSIGNAL_) != exit_signal)
        return _EINVAL;
    flags = (flags & ~CSIGNAL_) | exit_signal;

    if (flags & CLONE_PIDFD_)
        return _ENOSYS;

    if ((args.child_tid >> 32) != 0 || (args.parent_tid >> 32) != 0 || (args.tls >> 32) != 0)
        return _EINVAL;

    qword_t child_stack = args.stack;
    if (child_stack != 0 && args.stack_size != 0)
        child_stack += args.stack_size;
    if ((child_stack >> 32) != 0)
        return _EINVAL;

    return sys_clone(flags, (addr_t) child_stack, (addr_t) args.parent_tid,
        (addr_t) args.tls, (addr_t) args.child_tid);
}

dword_t sys_unshare(dword_t flags) {
    STRACE("unshare(%#x)", flags);

    const dword_t supported = CLONE_FILES_ | CLONE_FS_ | CLONE_SYSVSEM_;
    const dword_t known_unsupported = CLONE_VM_ | CLONE_SIGHAND_ | CLONE_THREAD_ |
        CLONE_NEWNS_ | CLONE_NEWCGROUP_ | CLONE_NEWUTS_ | CLONE_NEWIPC_ |
        CLONE_NEWUSER_ | CLONE_NEWPID_ | CLONE_NEWNET_ | CLONE_IO_;
    const dword_t known = supported | known_unsupported;

    if (flags & ~known)
        return _EINVAL;
    if (flags & known_unsupported)
        return _ENOSYS;

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

dword_t sys_fork(void) {
    return sys_clone(SIGCHLD_, 0, 0, 0, 0);
}

dword_t sys_vfork(void) {
    return sys_clone(CLONE_VFORK_ | CLONE_VM_ | SIGCHLD_, 0, 0, 0, 0);
}

void vfork_notify(struct task *task) {
    lock(&task->general_lock, 0);
    if ((task->vfork) && task->pid <= MAX_PID) { // If task->pid is large, badness.  -mke
        lock(&task->vfork->lock, 0);
        task->vfork->done = true;
        notify(&task->vfork->cond);
        unlock(&task->vfork->lock);
    }
    unlock(&task->general_lock);
}
