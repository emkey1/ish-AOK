#ifndef KERNEL_FUTEX_H
#define KERNEL_FUTEX_H

int futex_wake(guest_addr_t uaddr, dword_t val);

// Release every robust mutex this dying thread still holds: mark each lock
// word FUTEX_OWNER_DIED and wake a waiter, so a blocking lock returns
// EOWNERDEAD rather than hanging. Called from do_exit while the task's address
// space is still live.
void futex_exit_robust_list(struct task *task);

// Drop any futex pinned by the current task across an SA_RESTART restart (see
// the futex_restart_* fields in struct task). Called when a FUTEX_WAIT returns
// EINTR to the guest without restarting, and on task exit. No-op if nothing is
// parked. Safe to call with futex_lock NOT held.
void futex_release_restart_park(void);

// True if `flag` (a task's waiting_interrupt_flag) still points at a live
// queued futex waiter. See the definition in kernel/futex.c.
bool futex_wait_flag_is_live(const bool *flag);

#endif
