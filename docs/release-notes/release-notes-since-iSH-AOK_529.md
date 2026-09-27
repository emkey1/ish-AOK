# Release Notes Since `builds/iSH-AOK_529`

These notes summarize changes from `builds/iSH-AOK_529` intended for `builds/iSH-AOK_530`.

## Highlights

- Reworked task/session/process-group locking to remove `pids_lock` from the hottest TTY and procfs paths, fixing the multi-terminal stalls that showed up under Go builds, `top`, `watch`, and terminal churn.
- Fixed OpenSSH privilege-separation UID/GID drop behavior so `sshd` can permanently drop privileges correctly again.
- Fixed DNS refresh so guest `/etc/resolv.conf` updates when the iOS network configuration changes, including transitions between cellular, Wi-Fi, and VPN-managed resolver sets.
- Fixed socket ancillary-data handling for `ping`, including IPv4 TTL and IPv6 hop-limit metadata, and fixed blocking socket receive interruption so `^C` can break stuck `ping -4` receives again.
- Fixed three signal-delivery bugs that caused `poll`/`select`/`pselect`/`ppoll` to either hang indefinitely or return spurious EINTR — critical for shells, Python, Ruby, and any program using `select`-based I/O multiplexing.
- Fixed a crash when listing `/proc/<pid>/fd` on an exiting task, preventing `ls /proc/*/fd` and similar procfs traversals from terminating the session.
- Fixed `go run` hanging forever in multicore mode: the Go net poller's infinite `epoll_wait` now has a 2-second cap so the Go scheduler cannot starve.
- Added deeper Go release-smoke and matrix tooling to validate cold builds and real-world Go projects on release candidates.

## User-Facing Changes

### Terminal, TTY, and Job Control Stability

- Removed `pids_lock` from the interactive TTY read path and narrowed job-control locking around process-group/session metadata.
- Reduced lock hold times across procfs, signal fanout, workspace process snapshots, and related task lookups.
- Fixed an `exit_group` signal-delivery lifetime bug that could crash or wedge heavy process-churn workloads.
- Improved session startup and shared-mm coordination to reduce startup and lifecycle instability under load.

### SSH and Privilege Handling

- Fixed Linux capability handling when a root-originating task permanently drops to a non-root UID/GID.
- This restores OpenSSH privsep behavior that previously caused `sshd` to abort or mis-handle permanent privilege drops.
- Added a focused regression test for the UID/GID drop invariant used by OpenSSH.

### DNS and Networking

- Switched DNS refresh to use live Apple DNS configuration data instead of relying only on stale resolver state.
- DNS updates are now triggered from DNS-configuration notifications and path changes, then written back into guest `/etc/resolv.conf`.
- Aggregated usable resolver entries more defensively so stale Wi-Fi resolvers are less likely to persist after path changes.
- Fixed translation of host ancillary socket metadata so guest IPv4 `ping` sees TTL correctly and guest IPv6 `ping` sees hop-limit metadata correctly.
- Hardened host control-message parsing during network churn and made blocking `recvmsg()` / `recvfrom()` paths interruptible via the existing signal-unwind path.

### Signal Delivery and poll/select Correctness

- Fixed three bugs in `poll`/`select`/`pselect`/`ppoll` signal interruption (`fs/poll.c`, `util/sync.h`):
  1. The `sigunwind_start` signal-unwind path contained a `sigsetjmp` dead-frame bug (was a function, must be a macro) that could silently fail to unwind.
  2. Host SIGUSR1 pokes from TLB invalidation (`task_poke_shared_mem`) were leaking to the guest as EINTR even when no guest signal was pending.
  3. `pselect`/`ppoll` updated the guest signal mask but not the host pthread signal mask, so SIGUSR1 could be silently blocked, causing indefinite hangs.
- Fixed a crash in `fs/proc/pid.c` when `ls /proc/<pid>/fd` races with a task exiting: error returns from `bool`-typed readdir callbacks were incorrectly treated as success (non-zero int → `true`), causing a null-pointer dereference in `proc_entry_getname`.

### Runtime Compatibility and Correctness

- Fixed `go run` hanging forever: `epoll_wait(-1)` in the Go net poller was given an unbounded wait in multicore mode; now capped at 2 seconds so the Go scheduler can always make progress.
- Preserved NaNs correctly in SSE float widening conversions.
- Improved shared-mm quiescing and host runtime stability.

### Test and Release Tooling

- Added a deeper synthetic Go release-smoke harness.
- Added a repeatable Go release matrix driver for synthetic smoke plus real-world external Go repos.
- Added new net-regression tests for ancillary metadata delivery and `recvmsg()` interruption.

## Known Issue

- IPv6 still has a path-transition weakness: in current testing, IPv4 survived some Wi-Fi/cellular primary-path flips while IPv6 could still die across the same transition. This is noted but not fixed in `builds/iSH-AOK_530`.

## Maintainer Notes

- `builds/iSH-AOK_529` points to commit `efd1f90d`.
- `builds/iSH-AOK_530` should point to the current release candidate commit after this note is committed and tagged.
- Release validation status at tag time:
  - synthetic Go smoke passed on the current baseline
  - external Go matrix automation was corrected to use build-focused verification rather than `-h`/`--help` semantics

## Commit Range

- `262c9cc5` proc: fix ls /proc/<pid>/fd crash on exiting task; epoll: cap infinite wait
- `1726601d` poll: fix poll/pselect/ppoll not returning EINTR on signal delivery
- `e0a34001` Drop multicast entitlement
- `a61d93b9` futex: don't leak host-side pokes to the guest as EINTR
- `02abcdc8` signal: fix delivery deadlock, blocked-fault livelock, SIGURG default
- `797ff44b` locking: record wrlock acquisition sites only in debug builds
- `f308f7f4` jit: mix block addresses before hashing
- `7ce73ac7` memory: walk leaf arrays directly in pt_find_hole
- `a0fed82a` refcounts: make task/mem reference counts lock-free and unconditional
- `09a725f4` mmap: use quiesce/poke protocol for fork's address-space copy
- `14b4fb3e` memory: revalidate growsdown page after rwlock upgrade
- `8a2e0893` task: remove dead group_count_in_int bookkeeping
- `d8582c7d` locking: fix wrlock unlock-ordering race and nested mem-lock deadlock
- `81fa8a82` fastlane: retarget release config to iSH-AOK
- `c06f962c` docs: correct iSH-AOK 529 release notes
- `0c86eace` runtime: improve shared-mm quiescing and host stability
- `074372c7` emu: preserve NaNs in SSE float widening conversions
- `5d03bd43` stabilize session startup and shared-mm coordination
- `ca2eb694` split tty session metadata from pid topology
- `a59c1613` fix exit-group signal delivery lifetime
- `7864511a` tighten task snapshot and group-state locking
- `969c8c7a` reduce pid-lock hold times in signal and task lookups
- `7707f084` fix uid capability drop for sshd privsep
- `e4a5d424` fix network transition handling and add release matrix
