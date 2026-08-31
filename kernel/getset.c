#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/personality.h"
#include "util/sync.h"

#define _LINUX_CAPABILITY_VERSION_1_ 0x19980330
#define _LINUX_CAPABILITY_VERSION_2_ 0x20071026
#define _LINUX_CAPABILITY_VERSION_3_ 0x20080522

struct cap_user_header_ {
    dword_t version;
    int_t pid;
};

struct cap_user_data_ {
    dword_t effective;
    dword_t permitted;
    dword_t inheritable;
};

static int cap_data_count(dword_t version) {
    switch (version) {
        case _LINUX_CAPABILITY_VERSION_1_:
            return 1;
        case _LINUX_CAPABILITY_VERSION_2_:
        case _LINUX_CAPABILITY_VERSION_3_:
            return 2;
        default:
            return -1;
    }
}

static bool cap_words_subset(const dword_t *subset, const dword_t *superset, int count) {
    for (int i = 0; i < count; i++) {
        if ((subset[i] & ~superset[i]) != 0)
            return false;
    }
    return true;
}

static bool current_has_cap(uint_t cap) {
    if (current == NULL || cap >= 64)
        return false;
    return (current->cap_effective[cap / 32] & (1u << (cap % 32))) != 0;
}

// Shared with the privileged syscalls elsewhere in the tree; see task.h.
bool current_capable(unsigned cap) {
    return superuser() || current_has_cap(cap);
}

bool current_may_access_task_mem(struct task *target) {
    if (current == NULL || target == NULL)
        return false;
    if (target == current)
        return true;
    if (current_capable(CAP_SYS_PTRACE_))
        return true;
    // Linux requires the caller's euid to equal ALL THREE of the target's
    // uids, and the same for gids. Comparing only the effective ids would let
    // a process that has temporarily dropped privilege be read by one that
    // never held any -- it could regain that privilege later, so its memory is
    // still privileged memory.
    return current->euid == target->uid &&
           current->euid == target->euid &&
           current->euid == target->suid &&
           current->egid == target->gid &&
           current->egid == target->egid &&
           current->egid == target->sgid;
}

static bool current_can_setuids(void) {
    return superuser() || current_has_cap(CAP_SETUID_);
}

static bool current_can_setgids(void) {
    return superuser() || current_has_cap(CAP_SETGID_);
}

static void cap_emulate_setxuid(uid_t_ old_ruid, uid_t_ old_euid, uid_t_ old_suid) {
    bool old_any_root = old_ruid == 0 || old_euid == 0 || old_suid == 0;
    bool new_any_root = current->uid == 0 || current->euid == 0 || current->suid == 0;

    // Linux drops all capabilities once a root-originating task has fully
    // transitioned to non-root credentials unless PR_SET_KEEPCAPS or
    // securebits say otherwise -- EXCEPT the ambient set, whose bits stay in
    // permitted and effective across the transition (that is the entire
    // point of ambient capabilities; systemd's AmbientCapabilities= relies
    // on it, and then re-asserts the bits with capset which our
    // permitted-subset check would reject if they had been cleared).
    if (old_any_root && !new_any_root) {
        current->cap_effective[0] = current->cap_ambient[0];
        current->cap_effective[1] = current->cap_ambient[1];
        if (!current->keepcaps) {
            current->cap_permitted[0] = current->cap_ambient[0];
            current->cap_permitted[1] = current->cap_ambient[1];
        } else {
            current->cap_permitted[0] |= current->cap_ambient[0];
            current->cap_permitted[1] |= current->cap_ambient[1];
        }
        return;
    }

    // Dropping only the effective uid from 0 disables effective capabilities
    // (minus ambient ones) until/unless the task returns to euid 0.
    if (old_euid == 0 && current->euid != 0) {
        current->cap_effective[0] = current->cap_ambient[0];
        current->cap_effective[1] = current->cap_ambient[1];
        return;
    }

    // Regaining euid 0 restores effective capabilities from the permitted set.
    if (old_euid != 0 && current->euid == 0) {
        current->cap_effective[0] = current->cap_permitted[0];
        current->cap_effective[1] = current->cap_permitted[1];
    }
}

pid_t_ sys_getpid(void) {
    STRACE("getpid()");
    return current->tgid;
}
pid_t_ sys_gettid(void) {
    STRACE("gettid()");
    return current->pid;
}
pid_t_ sys_getppid(void) {
    STRACE("getppid()");
    pid_t_ ppid;
    complex_lockt(&pids_lock, 0);
    // The parent PROCESS, i.e. the parent's tgid -- Linux returns
    // task_tgid_vnr(real_parent). `parent` points at the specific task that
    // forked us, which for a fork from a non-leader thread is NOT the group
    // leader, and `->pid` there is that thread's tid (the value sys_gettid
    // returns), not a process id at all.
    //
    // Getting this wrong is invisible while the forking task is its group's
    // leader, which is every shell and most single-threaded programs. It broke
    // Go outright: syscall.forkAndExecInChild, with SysProcAttr.Pdeathsig set,
    // captures getpid() in the parent and then has the child check
    // `getppid() != ppid` as "my parent already died" and kill itself with
    // Pdeathsig. Go runs the fork on whatever thread the runtime scheduled, so
    // on a non-leader the check misfired and the child SIGTERMed itself before
    // reaching execve -- surfacing only as os/exec's "signal: terminated", with
    // no error from the command, because the command never ran (GH #523, yay).
    if (current->parent != NULL)
        ppid = current->parent->tgid;
    else
        ppid = 0;
    unlock(&pids_lock);
    return ppid;
}

dword_t sys_getuid32(void) {
    STRACE("getuid32()");
    return current->uid;
}
dword_t sys_getuid(void) {
    STRACE("getuid()");
    return current->uid & 0xffff;
}

dword_t sys_geteuid32(void) {
    STRACE("geteuid32()");
    return current->euid;
}
dword_t sys_geteuid(void) {
    STRACE("geteuid()");
    return current->euid & 0xffff;
}

int_t sys_setuid(uid_t_ uid) {
    STRACE("setuid(%d)", uid);
    uid_t_ old_ruid = current->uid;
    uid_t_ old_euid = current->euid;
    uid_t_ old_suid = current->suid;
    if (current_can_setuids()) {
        current->uid = current->suid = uid;
    } else {
        if (uid != current->uid && uid != current->suid)
            return _EPERM;
    }
    current->euid = uid;
    current->fsuid = uid;
    cap_emulate_setxuid(old_ruid, old_euid, old_suid);
    return 0;
}

dword_t sys_setresuid(uid_t_ ruid, uid_t_ euid, uid_t_ suid) {
    STRACE("setresuid(%d, %d, %d)", ruid, euid, suid);
    uid_t_ old_ruid = current->uid;
    uid_t_ old_euid = current->euid;
    uid_t_ old_suid = current->suid;
    if (!current_can_setuids()) {
        if (ruid != (uid_t) -1 && ruid != current->uid && ruid != current->euid && ruid != current->suid)
            return _EPERM;
        if (euid != (uid_t) -1 && euid != current->uid && euid != current->euid && euid != current->suid)
            return _EPERM;
        if (suid != (uid_t) -1 && suid != current->uid && suid != current->euid && suid != current->suid)
            return _EPERM;
    }

    if (ruid != (uid_t) -1)
        current->uid = ruid;
    if (euid != (uid_t) -1)
        current->euid = euid;
    if (suid != (uid_t) -1)
        current->suid = suid;
    if (euid != (uid_t) -1)
        current->fsuid = euid;
    cap_emulate_setxuid(old_ruid, old_euid, old_suid);
    return 0;
}

int_t sys_getresuid(addr_t ruid_addr, addr_t euid_addr, addr_t suid_addr) {
    STRACE("getresuid(%#x, %#x, %#x)", ruid_addr, euid_addr, suid_addr);
    if (user_put(ruid_addr, current->uid))
        return _EFAULT;
    if (user_put(euid_addr, current->euid))
        return _EFAULT;
    if (user_put(suid_addr, current->suid))
        return _EFAULT;
    return 0;
}

int_t sys_getresuid_guest(guest_addr_t ruid_addr, guest_addr_t euid_addr, guest_addr_t suid_addr) {
    STRACE("getresuid(%#llx, %#llx, %#llx)",
            (unsigned long long) ruid_addr,
            (unsigned long long) euid_addr,
            (unsigned long long) suid_addr);
    if (user_put(ruid_addr, current->uid))
        return _EFAULT;
    if (user_put(euid_addr, current->euid))
        return _EFAULT;
    if (user_put(suid_addr, current->suid))
        return _EFAULT;
    return 0;
}

int_t sys_setreuid(uid_t_ ruid, uid_t_ euid) {
    // setreuid(2): "If the real user ID is set, or the effective user ID is
    // set to a value not equal to the previous real user ID, the saved
    // set-user-ID will be set to the new effective user ID."
    //
    // Passing -1 left the saved id at its OLD value, so a process that dropped
    // privilege with setreuid(1000, 1000) kept suid 0 and could call setuid(0)
    // to become root again. Making the drop permanent is the entire reason to
    // call setreuid, so this was worth more than its severity suggests.
    //
    // The computed saved value is always the NEW euid, which sys_setresuid's
    // own euid check has already validated, so passing it explicitly cannot
    // turn a permitted call into EPERM.
    uid_t_ new_euid = (euid == (uid_t_) -1) ? current->euid : euid;
    uid_t_ suid = (uid_t_) -1;
    if (ruid != (uid_t_) -1 || new_euid != current->uid)
        suid = new_euid;
    return sys_setresuid(ruid, euid, suid);
}

uid_t_ sys_setfsuid(uid_t_ uid) {
    uid_t_ old = current->fsuid;
    STRACE("setfsuid(%d)", uid);
    if (uid == (uid_t_) -1)
        return old;
    if (current_can_setuids() || uid == current->uid || uid == current->euid || uid == current->suid)
        current->fsuid = uid;
    return old;
}

dword_t sys_getgid32(void) {
    STRACE("getgid32()");
    return current->gid;
}
dword_t sys_getgid(void) {
    STRACE("getgid()");
    return current->gid & 0xffff;
}

dword_t sys_getegid32(void) {
    STRACE("getegid32()");
    return current->egid;
}
dword_t sys_getegid(void) {
    STRACE("getegid()");
    return current->egid & 0xffff;
}

int_t sys_setgid(uid_t_ gid) {
    STRACE("setgid(%d)", gid);
    if (current_can_setgids()) {
        current->gid = current->sgid = gid;
    } else {
        if (gid != current->gid && gid != current->sgid)
            return _EPERM;
    }
    current->egid = gid;
    current->fsgid = gid;
    return 0;
}

dword_t sys_setresgid(uid_t_ rgid, uid_t_ egid, uid_t_ sgid) {
    STRACE("setresgid(%d, %d, %d)", rgid, egid, sgid);
    if (!current_can_setgids()) {
        if (rgid != (uid_t) -1 && rgid != current->gid && rgid != current->egid && rgid != current->sgid)
            return _EPERM;
        if (egid != (uid_t) -1 && egid != current->gid && egid != current->egid && egid != current->sgid)
            return _EPERM;
        if (sgid != (uid_t) -1 && sgid != current->gid && sgid != current->egid && sgid != current->sgid)
            return _EPERM;
    }

    if (rgid != (uid_t) -1)
        current->gid = rgid;
    if (egid != (uid_t) -1)
        current->egid = egid;
    if (sgid != (uid_t) -1)
        current->sgid = sgid;
    if (egid != (uid_t) -1)
        current->fsgid = egid;
    return 0;
}

int_t sys_getresgid(addr_t rgid_addr, addr_t egid_addr, addr_t sgid_addr) {
    STRACE("getresgid(%#x, %#x, %#x)", rgid_addr, egid_addr, sgid_addr);
    if (user_put(rgid_addr, current->gid))
        return _EFAULT;
    if (user_put(egid_addr, current->egid))
        return _EFAULT;
    if (user_put(sgid_addr, current->sgid))
        return _EFAULT;
    return 0;
}

int_t sys_getresgid_guest(guest_addr_t rgid_addr, guest_addr_t egid_addr, guest_addr_t sgid_addr) {
    STRACE("getresgid(%#llx, %#llx, %#llx)",
            (unsigned long long) rgid_addr,
            (unsigned long long) egid_addr,
            (unsigned long long) sgid_addr);
    if (user_put(rgid_addr, current->gid))
        return _EFAULT;
    if (user_put(egid_addr, current->egid))
        return _EFAULT;
    if (user_put(sgid_addr, current->sgid))
        return _EFAULT;
    return 0;
}

int_t sys_setregid(uid_t_ rgid, uid_t_ egid) {
    // The gid half of the same rule; see sys_setreuid.
    uid_t_ new_egid = (egid == (uid_t_) -1) ? current->egid : egid;
    uid_t_ sgid = (uid_t_) -1;
    if (rgid != (uid_t_) -1 || new_egid != current->gid)
        sgid = new_egid;
    return sys_setresgid(rgid, egid, sgid);
}

uid_t_ sys_setfsgid(uid_t_ gid) {
    uid_t_ old = current->fsgid;
    STRACE("setfsgid(%d)", gid);
    if (gid == (uid_t_) -1)
        return old;
    if (current_can_setgids() || gid == current->gid || gid == current->egid || gid == current->sgid)
        current->fsgid = gid;
    return old;
}

int_t sys_getgroups(dword_t size, addr_t list) {
    return sys_getgroups_guest(size, list);
}

int_t sys_getgroups_guest(dword_t size, guest_addr_t list) {
    STRACE("getgroups(%d, %#x)", size, list);
    if (size == 0)
        return current->ngroups;
    if (size < current->ngroups)
        return _EINVAL;
    for (unsigned i = 0; i < current->ngroups; i++)
        STRACE(" %d", current->groups[i]);
    if (user_write(list, current->groups, current->ngroups * sizeof(uid_t_)))
        return _EFAULT;
    return current->ngroups;
}

int_t sys_setgroups(dword_t size, addr_t list) {
    return sys_setgroups_guest(size, list);
}

int_t sys_setgroups_guest(dword_t size, guest_addr_t list) {
    STRACE("setgroups(%d, %#x)", size, list);
    if (!current_can_setgids())
        return _EPERM;
    if (size > MAX_GROUPS)
        return _EINVAL;
    // Built to the side and swapped in, so a faulting list leaves the current
    // group set intact rather than half-overwritten.
    uid_t_ *groups = NULL;
    if (size != 0) {
        groups = malloc((size_t) size * sizeof(uid_t_));
        if (groups == NULL)
            return _ENOMEM;
        if (user_read(list, groups, (size_t) size * sizeof(uid_t_))) {
            free(groups);
            return _EFAULT;
        }
    }
    for (unsigned i = 0; i < size; i++)
        STRACE(" %d", groups[i]);
    free(current->groups);
    current->groups = groups;
    current->ngroups = size;
    return 0;
}

// this does not really work
int_t sys_capget(addr_t header_addr, addr_t data_addr) {
    return sys_capget_guest(header_addr, data_addr);
}

int_t sys_capget_guest(guest_addr_t header_addr, guest_addr_t data_addr) {
    STRACE("capget(%#llx, %#llx)", (unsigned long long) header_addr, (unsigned long long) data_addr);
    struct cap_user_header_ header;
    if (user_read(header_addr, &header, sizeof(header)))
        return _EFAULT;
    int count = cap_data_count(header.version);
    if (count < 0) {
        header.version = _LINUX_CAPABILITY_VERSION_3_;
        if (user_write(header_addr, &header, sizeof(header)))
            return _EFAULT;
        return _EINVAL;
    }
    // Accept the tgid too: glibc/systemd pass getpid() (the tgid) here,
    // while current->pid is the tid.
    if (header.pid != 0 && header.pid != current->pid && header.pid != current->tgid)
        return _EPERM;

    struct cap_user_data_ data[2] = {};
    data[0].effective = current->cap_effective[0];
    data[0].permitted = current->cap_permitted[0];
    data[0].inheritable = current->cap_inheritable[0];
    if (count > 1) {
        data[1].effective = current->cap_effective[1];
        data[1].permitted = current->cap_permitted[1];
        data[1].inheritable = current->cap_inheritable[1];
    }
    if (user_write(data_addr, data, sizeof(data[0]) * count))
        return _EFAULT;
    return 0;
}
int_t sys_capset(addr_t header_addr, addr_t data_addr) {
    return sys_capset_guest(header_addr, data_addr);
}

int_t sys_capset_guest(guest_addr_t header_addr, guest_addr_t data_addr) {
    STRACE("capset(%#llx, %#llx)", (unsigned long long) header_addr, (unsigned long long) data_addr);
    struct cap_user_header_ header;
    if (user_read(header_addr, &header, sizeof(header)))
        return _EFAULT;
    int count = cap_data_count(header.version);
    if (count < 0)
        return _EINVAL;
    if (header.pid != 0 && header.pid != current->pid && header.pid != current->tgid)
        return _EPERM;

    struct cap_user_data_ data[2] = {};
    if (user_read(data_addr, data, sizeof(data[0]) * count))
        return _EFAULT;

    dword_t new_effective[2] = {data[0].effective, 0};
    dword_t new_permitted[2] = {data[0].permitted, 0};
    dword_t new_inheritable[2] = {data[0].inheritable, 0};
    if (count > 1) {
        new_effective[1] = data[1].effective;
        new_permitted[1] = data[1].permitted;
        new_inheritable[1] = data[1].inheritable;
    }

    // Linux allows an unprivileged task to drop capabilities it already has
    // and to toggle effective bits within its permitted set. The old
    // superuser-only gate breaks helpers like ping that reduce their
    // capability set after a uid transition.
    if (!superuser()) {
        if (!cap_words_subset(new_permitted, current->cap_permitted, count))
            return _EPERM;
        // Linux permits raising inheritable bits from the PERMITTED set too
        // (pI' must be a subset of pI | pP, not of pI alone): systemd's
        // keep_capability() after enforce_user's setresuid re-asserts a cap
        // in all three of effective/permitted/inheritable, and the cap is
        // in permitted (kept across the uid switch via SECBIT_KEEP_CAPS)
        // but typically NOT in the pre-existing inheritable set.
        dword_t inheritable_limit[2] = {
            current->cap_inheritable[0] | current->cap_permitted[0],
            current->cap_inheritable[1] | current->cap_permitted[1],
        };
        if (!cap_words_subset(new_inheritable, inheritable_limit, count))
            return _EPERM;
        if (!cap_words_subset(new_effective, new_permitted, count))
            return _EPERM;
        if (!cap_words_subset(new_effective, current->cap_permitted, count))
            return _EPERM;
    }

    if (!cap_words_subset(new_effective, new_permitted, count))
        return _EPERM;

    current->cap_effective[0] = new_effective[0];
    current->cap_permitted[0] = new_permitted[0];
    current->cap_inheritable[0] = new_inheritable[0];
    current->cap_effective[1] = 0;
    current->cap_permitted[1] = 0;
    current->cap_inheritable[1] = 0;
    if (count > 1) {
        current->cap_effective[1] = new_effective[1];
        current->cap_permitted[1] = new_permitted[1];
        current->cap_inheritable[1] = new_inheritable[1];
    }
    return 0;
}

// minimal version according to Linux sys/personality.h
// personality() sets the execution domain and returns the PREVIOUS value.
// Linux accepts whatever it is given -- the low byte is the domain, the upper
// bits are flags -- and 0xffffffff is the conventional "just tell me" query
// because it sets nothing anybody uses.
//
// This used to refuse everything except ADDR_NO_RANDOMIZE with EINVAL,
// including personality(0), which is the single most common call: it is what
// `setarch --uname-2.6`, ancient-binary wrappers and every "restore the
// default domain" path do. They all failed.
int_t sys_personality(dword_t persona) {
    STRACE("personality(%#x)", persona);
    dword_t previous = current->group->personality;
    if (persona == 0xffffffff)
        return previous;

    // ADDR_NO_RANDOMIZE stays set whatever the caller asks for. AOK does not
    // randomize the address space at all, so reporting the bit clear would
    // claim a randomization that does not happen -- and a program that checks
    // it before deciding whether to re-exec itself under setarch would then
    // loop. Everything else is stored as given and reported back.
    current->group->personality = persona | ADDR_NO_RANDOMIZE_;
    return previous;
}
