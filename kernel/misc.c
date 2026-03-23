#include <string.h>
#include "kernel/calls.h"

#define PRCTL_SET_KEEPCAPS_ 8
#define PRCTL_SET_NAME_ 15

#define KEYCTL_GET_KEYRING_ID_ 0
#define KEYCTL_JOIN_SESSION_KEYRING_ 1
#define KEYCTL_SETPERM_ 5
#define KEYCTL_SESSION_TO_PARENT_ 18

int_t sys_prctl(dword_t option, uint_t arg2, uint_t UNUSED(arg3), uint_t UNUSED(arg4), uint_t UNUSED(arg5)) {
    switch (option) {
        case PRCTL_SET_KEEPCAPS_:
            // stub
            return 0;
        case PRCTL_SET_NAME_: {
            char name[16];
            if (user_read_string(arg2, name, sizeof(name) - 1))
                return _EFAULT;
            name[sizeof(name) - 1] = '\0';
            STRACE("prctl(PRCTL_SET_NAME, \"%s\")", name);
            lock(&current->general_lock, 0);
            strncpy(current->comm, name, sizeof(current->comm));
            unlock(&current->general_lock);
            return 0;
        }
        default:
            STRACE("prctl(%#x)", option);
            return _EINVAL;
    }
}

int_t sys_arch_prctl(int_t code, addr_t addr) {
    STRACE("arch_prctl(%#x, %#x)", code, addr);
    return _EINVAL;
}

int_t sys_rseq(addr_t rseq_addr, dword_t rseq_len, dword_t flags, dword_t sig) {
    STRACE("rseq(%#x, %u, %#x, %#x)", rseq_addr, rseq_len, flags, sig);
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
