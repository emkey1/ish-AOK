#include <string.h>
#include "util/list.h"
#include "util/sync.h"
#include "kernel/calls.h"
#include "kernel/task.h"
#include "fs/tty.h"

dword_t sys_setpgid(pid_t_ id, pid_t_ pgid) {
    STRACE("setpgid(%d, %d)", id, pgid);
    int err;
    if (id == 0)
        id = current->pid;
    if (pgid == 0)
        pgid = id;
    // a negative process-group id is invalid (checked before any task lookup,
    // matching the kernel's early `if (pgid < 0) return -EINVAL`)
    if (pgid < 0)
        return _EINVAL;
    complex_lockt(&pids_lock, 0);
    bool group_locked = false;
    struct pid *pid = pid_get(id);
    err = _ESRCH;
    if (pid == NULL)
        goto out;
    struct task *task = pid->task;
    if (task == NULL)
        goto out;
    struct tgroup *tgroup = task->group;
    lock(&tgroup->lock, 0);
    group_locked = true;

    // you can only join a process group in the same session
    if (id != pgid) {
        // there has to be a process in pgrp that's in the same session as id
        err = _EPERM;
        struct pid *group_pid = pid_get(pgid);
        if (group_pid == NULL || list_empty(&group_pid->pgroup))
            goto out;
        struct tgroup *group_first_tgroup = list_first_entry(&group_pid->pgroup, struct tgroup, pgroup);
        pid_t_ group_sid;
        if (group_first_tgroup == tgroup) {
            group_sid = tgroup->sid;
        } else {
            lock(&group_first_tgroup->lock, 0);
            group_sid = group_first_tgroup->sid;
            unlock(&group_first_tgroup->lock);
        }
        if (tgroup->sid != group_sid)
            goto out;
    }

    // you can only change the process group of yourself or a child
    err = _ESRCH;
    if (task != current && task->parent != current)
        goto out;
    // you cannot change the process group of a child that has already exec'd
    // (man setpgid: EACCES). Changing your *own* pgid post-exec is always
    // allowed, so this only applies to a child. Checked before the
    // session-leader EPERM to match the kernel's error precedence.
    err = _EACCES;
    if (task != current && task->did_exec)
        goto out;
    // a session leader cannot create a process group
    err = _EPERM;
    if (tgroup_is_session_leader(tgroup))
        goto out;

    if (tgroup->pgid != pgid) {
        list_remove(&tgroup->pgroup);
        tgroup->pgid = pgid;
        list_add(&pid->pgroup, &tgroup->pgroup);
    }

    err = 0;
out:
    if (group_locked)
        unlock(&tgroup->lock);
    unlock(&pids_lock);
    return err;
}

dword_t sys_setpgrp(void) {
    return sys_setpgid(0, 0);
}

pid_t_ sys_getpgid(pid_t_ pid) {
    STRACE("getpgid(%d)", pid);
    struct task *task = current;
    bool release_task = false;
    if (pid != 0) {
        task = pid_get_task_ref(pid);
        release_task = true;
    }
    if (!task)
        return _ESRCH;
    lock(&task->group->lock, 0);
    pid_t_ pgid = task->group->pgid;
    unlock(&task->group->lock);
    if (release_task)
        task_ref_cnt_mod(task, -1);
    return pgid;
}
pid_t_ sys_getpgrp(void) {
    return sys_getpgid(0);
}

// Must lock pids_lock and task->group->lock.
void task_leave_session(struct task *task) {
    struct tgroup *group = task->group;
    struct pid *sid_pid = pid_get(group->sid);
    bool last_session_group = sid_pid == NULL || list_size(&sid_pid->session) <= 1;
    list_remove_safe(&group->session);
    if (group->tty) {
        lock(&ttys_lock, 0);
        if (last_session_group) {
            lock(&group->tty->lock, 0);
            group->tty->session = 0;
            unlock(&group->tty->lock);
        }
        tty_release(group->tty);
        group->tty = NULL;
        unlock(&ttys_lock);
    }
}

pid_t_ task_setsid(struct task *task) {
    complex_lockt(&pids_lock, 0);
    struct tgroup *group = task->group;
    lock(&group->lock, 0);
    pid_t_ new_sid = group->leader->pid;
    if (group->pgid == new_sid || group->sid == new_sid) {
        unlock(&group->lock);
        unlock(&pids_lock);
        return _EPERM;
    }

    task_leave_session(task);
    struct pid *pid = pid_get(task->pid);
    list_add(&pid->session, &group->session);
    group->sid = new_sid;

    list_remove_safe(&group->pgroup);
    list_add(&pid->pgroup, &group->pgroup);
    group->pgid = new_sid;

    unlock(&group->lock);
    unlock(&pids_lock);
    return new_sid;
}

dword_t sys_setsid(void) {
    STRACE("setsid()");
    return task_setsid(current);
}

dword_t sys_getsid(pid_t_ pid) {
    STRACE("getsid(%d)", pid);
    struct task *task = current;
    bool release_task = false;
    if (pid != 0) {
        task = pid_get_task_ref(pid);
        release_task = true;
    }
    if (task == NULL)
        return _ESRCH;
    lock(&task->group->lock, 0);
    pid_t_ sid = task->group->sid;
    unlock(&task->group->lock);
    if (release_task)
        task_ref_cnt_mod(task, -1);
    return sid;
}
