# Apt Runtime Syscall Audit

Scope: modern glibc / apt / event-loop / process-management syscalls that are
likely to matter for guest userspace startup, helper process launch, and HTTP
method execution.

Last updated: 2026-03-23

## Recently Implemented

- `413 pselect6_time64`
- `414 ppoll_time64`
- `417 recvmmsg_time64`
- `421 rt_sigtimedwait_time64`
- `422 futex_time64`
- `261 timer_gettime`
- `262 timer_getoverrun`
- `408 timer_gettime64`
- `409 timer_settime64`
- `404 clock_settime64`
- `326 timerfd_gettime`
- `410 timerfd_gettime64`
- `411 timerfd_settime64`
- `441 epoll_pwait2`
- `364 accept4`
- `321 signalfd`
- `327 signalfd4`
- `341 signalfd4`
- `291 inotify_init`
- `292 inotify_add_watch`
- `293 inotify_rm_watch`
- `332 inotify_init1`
- `356 memfd_create`
- `435 clone3`
- `310 unshare`

## Already Safe Enough

- `354 seccomp`
  - Currently returns `-ENOSYS` via `syscall_stub`, not success.
- `439 faccessat2`
  - Mapped to `sys_faccessat`, but the in-tree handler already processes flags
    (`AT_EACCESS`, `AT_SYMLINK_NOFOLLOW`, `AT_EMPTY_PATH`).
- `148 fdatasync`
  - Mapped to `sys_fsync`; functionally acceptable, performance-only concern.

## Remaining Likely Gaps

- `424 pidfd_send_signal`
  - Modern process-management gap.
- `405 clock_adjtime64`
  - Time-management surface still incomplete, though much less likely to affect apt/helper startup.

## Current Limitations

- `291/292/293/332 inotify*`
  - The fd, watch bookkeeping, `read(2)`, `poll/epoll`, and internal
    guest-visible mutation delivery now exist for open/write/truncate/create/
    delete/rename/mkdir/rmdir/symlink/mknod paths handled by the emulator.
    Host-backed out-of-band file changes are still not delivered.
- `356 memfd_create`
  - Implemented as an anonymous RAM-backed regular file with ordinary
    read/write/seek/truncate behavior. Sealing is not implemented.
- `435 clone3`
  - Implemented as a translation shim onto legacy `clone(2)` for the existing
  supported flag set. Clone3-only features such as pidfds, set_tid arrays,
  and cgroup targeting still return `-ENOSYS`.
- `310 unshare`
  - `CLONE_FILES`, `CLONE_FS`, and `CLONE_SYSVSEM` now work as a safe subset.
    Namespace-oriented flags still return `-ENOSYS`.

## Notes

- The current debugging evidence does not support the theory that generic stubs
  are falsely returning success. `syscall_stub()` returns `-ENOSYS`.
- The remaining work should prioritize:
  1. Audit of sandbox / namespace probes (`seccomp`, `unshare`) against actual
     guest logs
  2. `pidfd_send_signal` and the rest of the pidfd surface
  3. Broader namespace coverage beyond the current `unshare` subset
