#ifndef KERNEL_SYSVIPC_H
#define KERNEL_SYSVIPC_H

#include "misc.h"

// Linux's ipcperms(): shm, sem and msg all store uid/gid/cuid/cgid/mode and
// all three ignored them entirely, so any uid could read, write and destroy
// another user's private objects. `want` is 4 (read) or 2 (write), matching
// the mode bits. Implemented in kernel/ipc.c.
bool ipc_access_ok(uid_t_ uid, uid_t_ gid, uid_t_ cuid, uid_t_ cgid,
                   mode_t_ mode, int want);
// IPC_RMID / IPC_SET: only the owner, the creator, or CAP_SYS_ADMIN.
bool ipc_owner_ok(uid_t_ uid, uid_t_ cuid);

// Guest-ABI SysV ipc64_perm layouts, shared by shm (kernel/ipc.c) and
// message queues (kernel/sysvmsg.c). The "amd64" layout is the common
// 64-bit shape (amd64/arm64/riscv64 all match).

struct ipc_perm_i386_ {
    dword_t key;
    uid_t_ uid;
    uid_t_ gid;
    uid_t_ cuid;
    uid_t_ cgid;
    dword_t mode;
    word_t seq;
    word_t __pad2;
    dword_t __unused1;
    dword_t __unused2;
};
static_assert(sizeof(struct ipc_perm_i386_) == 36, "i386 ipc_perm size");

struct ipc_perm_amd64_ {
    dword_t key;
    uid_t_ uid;
    uid_t_ gid;
    uid_t_ cuid;
    uid_t_ cgid;
    word_t mode;
    word_t __pad1;
    word_t seq;
    word_t __pad2;
    qword_t __unused1;
    qword_t __unused2;
};
static_assert(sizeof(struct ipc_perm_amd64_) == 48, "amd64 ipc_perm size");

#endif

// /proc/sysvipc/{sem,msg}: written by the modules that own the lists, so the
// content and the lock that guards it stay together. See kernel/sysvsem.c.
struct proc_data;
void proc_sysvipc_show_sem(struct proc_data *buf);
void proc_sysvipc_show_msg(struct proc_data *buf);
