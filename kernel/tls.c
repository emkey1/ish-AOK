#include "kernel/calls.h"
#include "emu/i386_sreg.h"

// struct user_desc, its bit-fields as the one word they share (USER_DESC_*).
struct user_desc_ {
    dword_t entry_number;
    dword_t base_addr;
    dword_t limit;
    dword_t flags;
};

// do_set_thread_area: a TLS descriptor for `task`, read from the caller's
// memory. idx is the entry, or -1 to take the one in the descriptor; when
// that is -1 too and can_allocate is set, the first free entry is used and
// written back. The descriptor itself is the task's (emu/i386_sreg.c), and
// so are FS and GS, which follow it when they select that entry.
int task_set_thread_area(struct task *task, int idx, addr_t u_info, bool can_allocate) {
    struct user_desc_ info;
    if (user_get(u_info, info))
        return _EFAULT;
    if (!i386_tls_desc_okay(info.base_addr, info.limit, info.flags))
        return _EINVAL;
    if (idx == -1)
        idx = (int) info.entry_number;
    if (idx == -1 && can_allocate) {
        idx = i386_tls_free_entry(&task->cpu);
        if (idx < 0)
            return _ESRCH;
        info.entry_number = (dword_t) idx;
        if (user_put(u_info, info.entry_number))
            return _EFAULT;
    }
    if (idx < I386_TLS_ENTRY_MIN || idx >= I386_TLS_ENTRY_MIN + I386_TLS_ENTRIES)
        return _EINVAL;
    i386_tls_set(&task->cpu, (unsigned) idx, info.base_addr, info.limit, info.flags);
    return 0;
}

// do_get_thread_area: entry idx, as fill_user_desc reads it back.
int task_get_thread_area(struct task *task, int idx, addr_t u_info) {
    if (idx < I386_TLS_ENTRY_MIN || idx >= I386_TLS_ENTRY_MIN + I386_TLS_ENTRIES)
        return _EINVAL;
    struct user_desc_ info = {.entry_number = (dword_t) idx};
    i386_tls_get(&task->cpu, (unsigned) idx, &info.base_addr, &info.limit, &info.flags);
    if (user_put(u_info, info))
        return _EFAULT;
    return 0;
}

int sys_set_thread_area(addr_t u_info) {
    STRACE("set_thread_area(0x%x)", u_info);
    return task_set_thread_area(current, -1, u_info, true);
}

int sys_get_thread_area(addr_t u_info) {
    STRACE("get_thread_area(0x%x)", u_info);
    dword_t idx;
    if (user_get(u_info, idx))
        return _EFAULT;
    return task_get_thread_area(current, (int) idx, u_info);
}

int sys_set_tid_address(addr_t tid) {
    return sys_set_tid_address_guest(tid);
}

int sys_set_tid_address_guest(guest_addr_t tid) {
    current->clear_tid = tid;
    return sys_gettid();
}
