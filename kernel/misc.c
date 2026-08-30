#include <string.h>
#include "kernel/calls.h"

#define PRCTL_SET_PDEATHSIG_ 1
#define PRCTL_GET_PDEATHSIG_ 2
#define PRCTL_GET_DUMPABLE_ 3
#define PRCTL_SET_DUMPABLE_ 4
#define PRCTL_GET_KEEPCAPS_ 7
#define PRCTL_SET_KEEPCAPS_ 8
#define PRCTL_SET_NAME_ 15
#define PRCTL_GET_NAME_ 16
#define PRCTL_GET_SECCOMP_ 21
#define PRCTL_SET_SECCOMP_ 22
#define PRCTL_CAPBSET_READ_ 23
#define PRCTL_CAPBSET_DROP_ 24
#define PRCTL_GET_SECUREBITS_ 27
#define PRCTL_SET_SECUREBITS_ 28
#define PRCTL_SET_TIMERSLACK_ 29
#define PRCTL_GET_TIMERSLACK_ 30
#define PRCTL_SET_MM_ 35
#define PRCTL_SET_CHILD_SUBREAPER_ 36
#define PRCTL_GET_CHILD_SUBREAPER_ 37
#define PRCTL_SET_NO_NEW_PRIVS_ 38
#define PRCTL_GET_NO_NEW_PRIVS_ 39
#define PRCTL_CAP_AMBIENT_ 47

#define PRCTL_CAP_AMBIENT_IS_SET_ 1
#define PRCTL_CAP_AMBIENT_RAISE_ 2
#define PRCTL_CAP_AMBIENT_LOWER_ 3
#define PRCTL_CAP_AMBIENT_CLEAR_ALL_ 4

#define PRCTL_SET_MM_ARG_START_ 8
#define PRCTL_SET_MM_ARG_END_ 9
#define PRCTL_SET_MM_ENV_START_ 10
#define PRCTL_SET_MM_ENV_END_ 11

#define PRCTL_CAP_LAST_CAP_ 63
#define PRCTL_DEFAULT_TIMERSLACK_NS_ 50000

#define KEYCTL_GET_KEYRING_ID_ 0
#define KEYCTL_JOIN_SESSION_KEYRING_ 1
#define KEYCTL_SETPERM_ 5
#define KEYCTL_LINK_ 8
#define KEYCTL_SESSION_TO_PARENT_ 18

#define ARCH_SET_GS_ 0x1001
#define ARCH_SET_FS_ 0x1002
#define ARCH_GET_FS_ 0x1003
#define ARCH_GET_GS_ 0x1004

static bool prctl_cap_valid(uint_t cap) {
    return cap <= PRCTL_CAP_LAST_CAP_;
}

static bool prctl_cap_test(const dword_t caps[2], uint_t cap) {
    if (!prctl_cap_valid(cap))
        return false;
    return (caps[cap / 32] & (1u << (cap % 32))) != 0;
}

int_t sys_prctl_guest(dword_t option, qword_t arg2, qword_t arg3, qword_t UNUSED(arg4), qword_t UNUSED(arg5)) {
    switch (option) {
        case PRCTL_SET_PDEATHSIG_:
            current->pdeath_signal = (dword_t) arg2;
            return 0;
        case PRCTL_GET_PDEATHSIG_:
            if (user_put((guest_addr_t) arg2, current->pdeath_signal))
                return _EFAULT;
            return 0;
        case PRCTL_GET_DUMPABLE_:
            return 1;
        case PRCTL_SET_DUMPABLE_:
            if (arg2 > 1)
                return _EINVAL;
            return 0;
        case PRCTL_GET_KEEPCAPS_:
            return current->keepcaps ? 1 : 0;
        case PRCTL_SET_KEEPCAPS_:
            if (arg2 > 1)
                return _EINVAL;
            current->keepcaps = arg2 != 0;
            return 0;
        case PRCTL_GET_NAME_: {
            char name[16] = {};
            lock(&current->general_lock, 0);
            strncpy(name, current->comm, sizeof(name) - 1);
            unlock(&current->general_lock);
            if (user_write((guest_addr_t) arg2, name, sizeof(name)))
                return _EFAULT;
            return 0;
        }
        case PRCTL_GET_SECCOMP_:
            // Report "disabled" rather than erroring out during helper setup.
            return 0;
        case PRCTL_SET_SECCOMP_:
            // Compatibility stub: userland may try to sandbox helpers.
            // Pretend success instead of returning EINVAL, which newer apt
            // treats as a startup failure.
            STRACE("prctl(PR_SET_SECCOMP, %#x)", arg2);
            return 0;
        case PRCTL_SET_NAME_: {
            char name[16];
            if (user_read_string((guest_addr_t) arg2, name, sizeof(name) - 1)) {
                // user_read_string fails both on a fault AND on a string that
                // does not fit. Linux TRUNCATES here rather than failing -- the
                // comm field is 16 bytes and prctl(2) says so -- and AOK was
                // dropping the update with EFAULT for any name of 15+ chars,
                // which is an ordinary length for a thread name. Retry as a
                // fixed-size read: if those 15 bytes are readable the name was
                // merely long; if they are not, this is a real fault.
                if (user_read((guest_addr_t) arg2, name, sizeof(name) - 1))
                    return _EFAULT;
            }
            name[sizeof(name) - 1] = '\0';
            STRACE("prctl(PRCTL_SET_NAME, \"%s\")", name);
            lock(&current->general_lock, 0);
            strncpy(current->comm, name, sizeof(current->comm));
            unlock(&current->general_lock);
            return 0;
        }
        case PRCTL_CAPBSET_READ_:
            if (!prctl_cap_valid(arg2))
                return _EINVAL;
            // We do not model a separate bounding set. Use the permitted set so
            // capability probes see a coherent answer instead of EINVAL.
            return prctl_cap_test(current->cap_permitted, arg2) ? 1 : 0;
        case PRCTL_CAPBSET_DROP_:
            // As above, we don't model a separate bounding set to shrink --
            // just validate the capability number and accept. Every
            // process's exec_context capability-dropping sequence
            // (CapabilityBoundingSet=, systemd's exec_context_apply, PAM's
            // pam_cap, etc.) calls this once per capability it wants gone
            // before ever calling capset(); rejecting it outright with
            // EINVAL aborted the whole spawn ("Failed to drop capabilities")
            // before the exec'd program ever ran, which blocked every
            // service using CapabilityBoundingSet= (systemd-logind among
            // them) during Arch aarch64 boot.
            if (!prctl_cap_valid(arg2))
                return _EINVAL;
            return 0;
        // Model exactly the SECBIT_KEEP_CAPS bit (1 << 4), mapping it onto
        // the same task->keepcaps flag PR_SET_KEEPCAPS uses -- on Linux the
        // two are literally the same bit. systemd's enforce_user() (every
        // service with User= plus AmbientCapabilities=/SecureBits=) arms
        // keep-caps ONLY via PR_SET_SECUREBITS before its setresuid;
        // accepting the call while dropping the bit meant the uid change
        // wiped the permitted set and the follow-up capset ("Failed to keep
        // CAP_SYS_ADMIN") EPERM'd, killing systemd-resolved at step USER.
        // Other securebits describe policy we don't model (no privilege
        // boundary here); accept and ignore them.
        case PRCTL_GET_SECUREBITS_:
            return current->keepcaps ? (1 << 4) : 0;
        case PRCTL_SET_SECUREBITS_:
            current->keepcaps = (arg2 & (1 << 4)) != 0;
            return 0;
        case PRCTL_SET_TIMERSLACK_:
            return 0;
        case PRCTL_GET_TIMERSLACK_:
            return PRCTL_DEFAULT_TIMERSLACK_NS_;
        case PRCTL_SET_MM_:
            if (!superuser())
                return _EPERM;
            lock(&current->general_lock, 0);
            if (current->mm == NULL) {
                unlock(&current->general_lock);
                return _EINVAL;
            }
            switch (arg2) {
                case PRCTL_SET_MM_ARG_START_:
                    current->mm->argv_start = (guest_addr_t) arg3;
                    break;
                case PRCTL_SET_MM_ARG_END_:
                    current->mm->argv_end = (guest_addr_t) arg3;
                    break;
                case PRCTL_SET_MM_ENV_START_:
                    current->mm->env_start = (guest_addr_t) arg3;
                    break;
                case PRCTL_SET_MM_ENV_END_:
                    current->mm->env_end = (guest_addr_t) arg3;
                    break;
                default:
                    unlock(&current->general_lock);
                    return _EINVAL;
            }
            unlock(&current->general_lock);
            return 0;
        case PRCTL_SET_CHILD_SUBREAPER_:
            if (arg2 > 1)
                return _EINVAL;
            // Was accepted and discarded, which is the worst of both: a
            // service manager set it, believed it, and then lost every
            // orphaned grandchild to init.
            lock(&current->group->lock, 0);
            current->group->child_subreaper = arg2 != 0;
            unlock(&current->group->lock);
            return 0;
        case PRCTL_GET_CHILD_SUBREAPER_: {
            lock(&current->group->lock, 0);
            dword_t value = current->group->child_subreaper ? 1 : 0;
            unlock(&current->group->lock);
            if (user_write((guest_addr_t) arg2, &value, sizeof(value)))
                return _EFAULT;
            return 0;
        }
        case PRCTL_SET_NO_NEW_PRIVS_:
            if (arg2 > 1)
                return _EINVAL;
            STRACE("prctl(PR_SET_NO_NEW_PRIVS, %#x)", arg2);
            return 0;
        case PRCTL_GET_NO_NEW_PRIVS_:
            return 0;
        // A real ambient set (kernel/task.h cap_ambient), not a no-op:
        // ambient bits survive the root-to-nonroot uid transition into
        // permitted+effective (kernel/getset.c cap_emulate_setxuid), which
        // is what lets systemd's AmbientCapabilities= services (e.g.
        // systemd-resolved with CAP_NET_RAW/CAP_NET_BIND_SERVICE, plus the
        // CAP_SYS_ADMIN systemd itself raises for post-setuid seccomp
        // setup) re-assert those caps with capset after enforce_user's
        // setuid -- capset's permitted-subset check EPERM'd otherwise
        // ("Failed to keep CAP_SYS_ADMIN", exit 217/USER).
        case PRCTL_CAP_AMBIENT_:
            switch (arg2) {
                case PRCTL_CAP_AMBIENT_IS_SET_:
                    if (!prctl_cap_valid((uint_t) arg3))
                        return _EINVAL;
                    return prctl_cap_test(current->cap_ambient, (uint_t) arg3) ? 1 : 0;
                case PRCTL_CAP_AMBIENT_RAISE_:
                    if (!prctl_cap_valid((uint_t) arg3))
                        return _EINVAL;
                    // Linux requires the cap in both permitted and
                    // inheritable to raise it into the ambient set.
                    if (!prctl_cap_test(current->cap_permitted, (uint_t) arg3) ||
                            !prctl_cap_test(current->cap_inheritable, (uint_t) arg3))
                        return _EPERM;
                    current->cap_ambient[arg3 / 32] |= 1u << (arg3 % 32);
                    return 0;
                case PRCTL_CAP_AMBIENT_LOWER_:
                    if (!prctl_cap_valid((uint_t) arg3))
                        return _EINVAL;
                    current->cap_ambient[arg3 / 32] &= ~(1u << (arg3 % 32));
                    return 0;
                case PRCTL_CAP_AMBIENT_CLEAR_ALL_:
                    current->cap_ambient[0] = current->cap_ambient[1] = 0;
                    return 0;
                default:
                    return _EINVAL;
            }
        default:
            STRACE("prctl(%#x)", option);
            return _EINVAL;
    }
}

int_t sys_prctl(dword_t option, uint_t arg2, uint_t arg3, uint_t arg4, uint_t arg5) {
    return sys_prctl_guest(option, arg2, arg3, arg4, arg5);
}

int_t sys_arch_prctl_guest(int_t code, guest_addr_t addr) {
    STRACE("arch_prctl(%#x, %#llx)", code, (unsigned long long) addr);
    if (!task_is_64bit(current))
        return _EINVAL;

    switch (code) {
        case ARCH_SET_FS_:
            current->cpu.tls_ptr = addr;
            return 0;
        case ARCH_GET_FS_: {
            qword_t fs_base = current->cpu.tls_ptr;
            if (user_put(addr, fs_base))
                return _EFAULT;
            return 0;
        }
        case ARCH_SET_GS_:
        case ARCH_GET_GS_:
            // The current long-mode bring-up only has one TLS base, used for
            // amd64 FS-relative accesses.
            return _EINVAL;
        default:
            return _EINVAL;
    }
}

int_t sys_arch_prctl(int_t code, addr_t addr) {
    return sys_arch_prctl_guest(code, addr);
}

int_t sys_rseq(addr_t rseq_addr, dword_t rseq_len, dword_t flags, dword_t sig) {
    return sys_rseq_guest(rseq_addr, rseq_len, flags, sig);
}

int_t sys_rseq_guest(guest_addr_t rseq_addr, dword_t rseq_len, dword_t flags, dword_t sig) {
    STRACE("rseq(%#llx, %u, %#x, %#x)", (unsigned long long) rseq_addr, rseq_len, flags, sig);
    // Deliberately report rseq as unsupported. Modern glibc falls back cleanly
    // on ENOSYS, but a fake success here would expose an ABI we do not emulate.
    return _ENOSYS;
}

int_t sys_keyctl(dword_t cmd, dword_t arg2, dword_t arg3, dword_t arg4, dword_t arg5) {
    STRACE("keyctl(%u, %#x, %#x, %#x, %#x)", cmd, arg2, arg3, arg4, arg5);
    switch (cmd) {
        case KEYCTL_GET_KEYRING_ID_:
            return 1;
        case KEYCTL_JOIN_SESSION_KEYRING_:
            return 1;
        case KEYCTL_SETPERM_:
        case KEYCTL_SESSION_TO_PARENT_:
        // Links one keyring's key into another (e.g. the user keyring into
        // the session keyring, systemd-executor's setup_keyring() for every
        // service with KeyringMode=shared, the default). We have no real
        // kernel keyring backing store -- every keyring ID this stub hands
        // out is already the same placeholder (1) -- so there's nothing to
        // actually link; accept as a no-op, matching KEYCTL_SETPERM above.
        case KEYCTL_LINK_:
            return 0;
        default:
            return _ENOSYS;
    }
}

#define REBOOT_MAGIC1 0xfee1dead
#define REBOOT_MAGIC2 672274793
#define REBOOT_MAGIC2A 85072278
#define REBOOT_MAGIC2B 369367448
#define REBOOT_MAGIC2C 537993216

#define REBOOT_CMD_CAD_OFF 0
#define REBOOT_CMD_CAD_ON 0x89abcdef

int_t sys_reboot(int_t magic, int_t magic2, int_t cmd) {
    STRACE("reboot(%#x, %d, %d)", magic, magic2, cmd);
    if (!superuser())
        return _EPERM;
    if (magic != (int) REBOOT_MAGIC1 ||
            (magic2 != REBOOT_MAGIC2 &&
             magic2 != REBOOT_MAGIC2A &&
             magic2 != REBOOT_MAGIC2B &&
             magic2 != REBOOT_MAGIC2C))
        return _EINVAL;

    switch (cmd) {
        case REBOOT_CMD_CAD_ON:
        case REBOOT_CMD_CAD_OFF:
            return 0;
        default:
            return _EPERM;
    }
}
