#ifndef KERNEL_FUTEX_H
#define KERNEL_FUTEX_H

int futex_wake(guest_addr_t uaddr, dword_t val);

// Drop any futex pinned by the current task across an SA_RESTART restart (see
// the futex_restart_* fields in struct task). Called when a FUTEX_WAIT returns
// EINTR to the guest without restarting, and on task exit. No-op if nothing is
// parked. Safe to call with futex_lock NOT held.
void futex_release_restart_park(void);

#endif
