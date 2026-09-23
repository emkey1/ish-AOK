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
    struct pid *group_pid = pid;
    if (id != pgid) {
        // there has to be a process in pgrp that's in the same session as id
        err = _EPERM;
        group_pid = pid_get(pgid);
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

    // Filed under the GROUP's pid: its pgroup list is what a group signal
    // walks (send_group_signal, kill(-pgid)), and what keeps the group -- and
    // its number -- alive once the leader is gone. This used the caller's own
    // pid, the same thing when a process makes a group of its own and wrong
    // whenever it joins another one: the joiner had the right pgid and nothing
    // sent to the group reached it, so the second process of every
    // job-controlled pipeline missed its ^C, ^Z and hangup, and once the
    // leader was reaped the group looked gone -- kill(-pgid) said ESRCH and
    // joining it said EPERM.
    if (tgroup->pgid != pgid) {
        list_remove(&tgroup->pgroup);
        tgroup->pgid = pgid;
        list_add(&group_pid->pgroup, &tgroup->pgroup);
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

// POSIX: a process group is orphaned when no member has a parent that is in a
// DIFFERENT process group but the SAME session -- that is, when there is
// nobody outside the group yet still inside the session who could ever
// continue it after a stop.
//
// It matters because stopping such a group would wedge it forever. Linux
// therefore refuses the terminal access that would have stopped it (EIO)
// instead of sending SIGTTIN/SIGTTOU, and AOK had no notion of this at all.
//
// Caller holds pids_lock, which is what keeps the membership lists still.
bool pgroup_is_orphaned(pid_t_ pgid, pid_t_ sid) {
    struct pid *pid = pid_get(pgid);
    if (pid == NULL)
        return true;
    struct tgroup *tgroup;
    list_for_each_entry(&pid->pgroup, tgroup, pgroup) {
        struct task *leader = tgroup->leader;
        struct task *parent = leader != NULL ? leader->parent : NULL;
        if (parent == NULL || parent->group == NULL || parent->group == tgroup)
            continue;
        lock(&parent->group->lock, 0);
        bool rescuer = parent->group->pgid != pgid && parent->group->sid == sid;
        unlock(&parent->group->lock);
        if (rescuer)
            return false;
    }
    return true;
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
    // The leader's pid, matching new_sid, NOT pid_get(task->pid).
    //
    // group->sid and group->pgid are both set to new_sid, which comes from
    // group->leader. Membership has to hang off the same pid, because that is
    // what a group signal is delivered through: send_group_signal(pgid) does
    // pid_get(pgid) and walks that pid's pgroup list. Linked under a different
    // pid, the lookup SUCCEEDS and the list simply does not contain this group,
    // so the signal reaches nobody and reports no error -- see the comment on
    // tgroup_restore_ids below, which documents the same hazard.
    //
    // For the usual caller the two are identical: a process calling setsid() is
    // its own group leader, so task->pid == group->leader->pid. They diverge
    // only when a NON-LEADER thread calls setsid(), where this used to name the
    // leader in group->sid while filing the membership under the thread's pid.
    // Linux uses task_pid(group_leader) for both (ksys_setsid ->
    // set_special_pids).
    struct pid *pid = pid_get(new_sid);
    list_add(&pid->session, &group->session);
    group->sid = new_sid;

    list_remove_safe(&group->pgroup);
    list_add(&pid->pgroup, &group->pgroup);
    group->pgid = new_sid;

    unlock(&group->lock);
    unlock(&pids_lock);
    return new_sid;
}

// Put a restored process back into the session and process group the image
// says it was in.
//
// setsid/setpgid cannot express this. They are the rules a LIVE process must
// obey when it ASKS -- a session leader may not create a process group, a
// child that has exec'd may not be moved, a session may not be joined at all
// -- and a restore is not a request. It is the state being put back, and the
// state was legal when it was taken.
//
// Assigning group->sid and group->pgid alone is not enough either: membership
// lives in the per-pid session and pgroup LISTS, which is what tcsetpgrp's
// "does this group belong to my session" check walks, and what a group signal
// is delivered through. A restored shell whose lists said one thing and whose
// fields said another got ENOTTY from tcsetpgrp and came up with job control
// disabled.
void tgroup_restore_ids(struct task *task, pid_t_ sid, pid_t_ pgid) {
    complex_lockt(&pids_lock, 0);
    struct tgroup *group = task->group;
    lock(&group->lock, 0);
    // A session or group whose leader is not in the image (it exited before
    // the checkpoint and the members were left behind) has no pid struct to
    // hang off. Keeping the task's own is what a live orphan would look like
    // and is better than a dangling id.
    struct pid *spid = pid_get((dword_t) sid);
    struct pid *gpid = pid_get((dword_t) pgid);
    if (spid == NULL) { spid = pid_get((dword_t) task->pid); sid = task->pid; }
    if (gpid == NULL) { gpid = pid_get((dword_t) task->pid); pgid = task->pid; }
    if (spid != NULL) {
        list_remove_safe(&group->session);
        list_add(&spid->session, &group->session);
        group->sid = sid;
    }
    if (gpid != NULL) {
        list_remove_safe(&group->pgroup);
        list_add(&gpid->pgroup, &group->pgroup);
        group->pgid = pgid;
    }
    unlock(&group->lock);
    unlock(&pids_lock);
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
