# build 556 musts

Work carried out of 555 that must be done, or explicitly decided, before 556
is tagged. The maintainer put it on this list on 2026-09-24. Each entry says
what is **established**, what the **next step** is, and how to **prove** it
afterwards.

Started 2026-09-24, after `builds/iSH-AOK_555` (tagged 2026-09-18). Supersedes
[docs/historical/build_555_musts.md](historical/build_555_musts.md). §1 there
(the ptrace detach) was fixed during 555. §2–§7 are carried below, renumbered.

**Every entry was re-checked before being carried, not copied forward**,
because 555's list opened by finding 40% of its predecessor stale. This time,
one of the six had gone stale in the meantime: 555 §4, POLLHUP without POLLIN,
no longer diverges (§3 below). It stays only for the regression assertion it
asked for.

The release's **theme** is in [docs/roadmap.md](roadmap.md). This document is
the work that has to happen around it.

**Status, 2026-09-24 (end of day):**

| § | item | state |
|---|---|---|
| 1 | i386 `lock not` / `lock neg` | **FIXED** in `e6940313` |
| 2 | iosfs new-API mount persistence | **FIXED** in `1ea88a79`, proven on the M4 iPad |
| 3 | POLLHUP without POLLIN | **FIXED** in `601404a6`: it had NOT gone stale, see §3 |
| 4 | `tty_hangup_signal` device flake | **DONE**: PASS in the 556 device suite run (M4 iPad) |
| 5 | Launcher applets in `top` | **DECIDED**: not processes, listed in `/proc/ish/applets` (`3c4e085e`) |
| 6 | RLIMIT_STACK push-down | **DECIDED** "implement", **FIXED** in `05d6ae19` |

---

## 1. `lock not` and `lock neg` are SIGILL on an i386 guest

**FIXED in `e6940313`.** A group-3 entry in the LOCK table, and `not`/`neg`
in `do_op_size_atomic` on BOTH hosts: the aarch64 gadgets that ship, and the
x86_64 ones Linux CI links, which would otherwise fail to link. The
contended test runs both forms on i386 now. Its `negl` counter used to start
at 0, which neg maps to itself, so it could never fail anywhere. It starts at
1, and a copy with the lock prefix removed fails it on camd. New
`atomic_neg_not` checks value and flags at every width, including a `not`
after a `cmp` with the flags still lazy. Real hardware passes it. A `not`
gadget that clobbers the lazy result fails it 95 times. Passing: the whole x86
atomics set on alpine-i386, alpine-amd64 and devuan-amd64, and the full i386
leg on the final tree, 269 pass and 0 fail, as is arm64 at 264 and 0. An
earlier run under a load average of ~140 failed five clock, timer, rusage
and watchdog tests. All five pass alone, on the new binary and on the
baseline, in interleaved runs. **Found on the way, FIXED since in `f81591fb`:** on
an x86_64 HOST (Linux CI, never the app), `lock adc`/`lock sbb` ran with
carry-in 0, 80000 short in the contended test. `setf_a`'s `orl` between the
`btw` that loaded CF and the `sbb` was the cause; CF is now snapshotted outside
the CAS loop, as on aarch64. The same commit fixes that host's `cmpxchg` AF
(`seta` shifted into bit 4, 976 failures in `atomic_cmpxchg32`), and the
contended test now also runs `stc; lock adcl`, which nothing covered before.

*Carried from 555 §2.* **Re-verified 2026-09-24.** The i386 `LOCK` table in
`emu/decode.h` (the `case 0xf0:` block, ending at its `default: ... UNDEFINED`)
has the ALU pairs, the `80/81/83` immediate group, the `0F` atomics, `86/87`
xchg and `FE/FF` inc/dec -- and still **no group-3 entry**, no `case 0xf6` or
`0xf7`. So `lock notl (mem)` and `lock negl (mem)` fall through to `UNDEFINED`
and kill the guest with SIGILL; real Linux runs both.
`tests/manual/x86/atomic_lock_contended.c` still skips those two forms on i386
and says so.

**Next step.** Add `not` and `neg` to the `.irp` list in `do_op_size_atomic`
(`jit/gadgets-aarch64/math.S`). `not` is `mvn` with **no** flag changes; `neg`
is `0 - operand` with the full sub flag rule. Then add `case 0xf6`/`0xf7` with a
group-3 switch to the LOCK table.

**Prove it.** Un-skip the two forms in `atomic_lock_contended`, and require the
i386 leg to pass. Run the whole i386 atomics set too: `do_op_size_atomic` is
shared by every i386 atomic, so a mistake there breaks all of them.

---

## 2. An iosfs mount made through the new mount API does not persist

**FIXED in `1ea88a79`, and PROVEN on the M4 iPad (2026-09-24)** with the
exact steps below. On Devuan, util-linux 2.41.5 `mount -t ios` went fsopen,
fsmount, move_mount (`LIBMOUNT_DEBUG=hook,cxt`), and the user picked iCloud
Drive, which landed at `/mnt/icloud556`. The app was relaunched, the saved
session deleted, and the guest booted fresh (uptime 10 s). The mount was back
at `/mnt/icloud556` with its contents, and `/.ish-fsmount` appeared 0 times in
`/proc/mounts` and mountinfo, before and after. It was unmounted afterwards,
which also drops its bookmark.
`fs_ops.relocated(mount, old_point, new_point)` is called from BOTH move
paths, `mount_relocate` and classic `MS_MOVE`, after `mounts_lock` is
dropped, holding a reference. iosfs keeps a staged bookmark in a
never-persisted side table and files it under the real path at the move.
Simulator, driven end to end: fsopen("ios") -> fsmount -> move_mount to
`/mnt/newapi556`, persisted under that key only. After a relaunch it was
mounted there again, with `/.ish-fsmount` 0 times in `/proc/mounts` and
mountinfo. A classic `mount --move` re-keyed as well, which it never did
before, and umount dropped its key. `fsopen_move_mount` and `mount_flags`
now umount without MNT_DETACH, so a reference the move leaked would show as
EBUSY. **Noticed, not changed:** both `mount -t ios` paths hold
`mounts_lock` while the folder picker waits for the user, so other path
lookups stall until the pick. That is as old as iosfs.

*Carried from 555 §3, and from 553 and 554 before it.* **Re-verified
2026-09-24.** There is still no `relocated` hook in `struct fs_ops`
(`kernel/fs.h`), and nothing calls one. The only mention is a comment in
`app/iOSFS.m` describing the staging path.

iosfs keys its security-scoped bookmark on `mount->point` at mount time. A mount
made through `fsopen`/`fsconfig`/`fsmount`/`move_mount` is created at a private
staging path (`/.ish-fsmount/<n>`) and moved later, so that key is wrong. Since
552, staging-path keys are not persisted, because persisting them brought back a
permanent phantom mount on every launch. The cost is that such a mount no longer
survives a relaunch.

**Next step.** Re-key at relocation. `mount_relocate` (`fs/mount.c`) has the
mount and both paths. Add an optional `relocated(mount, old_point)` to `struct
fs_ops` so iosfs can move the bookmark to the real path, and call it after
unlocking `mounts_lock`. The bookmark is not merely *mis-keyed* at mount time,
it is **not stored at all**. So iosfs also needs a non-persisted side table
keyed by the staging path to move it from, or it must re-derive the bookmark,
which needs the security-scoped URL it holds only during `iosfs_mount`.

**Prove it.** On a device:
- mount an iCloud directory with a util-linux `mount(8)` new enough to use the
  new API;
- relaunch the app;
- require the mount back at the path the user asked for, with nothing under
  `/.ish-fsmount/` in `/proc/mounts` either before or after.

Schedule this with the 556 device regression run. That is the reason it slipped
three releases.

---

## 3. POLLHUP without POLLIN on a closed socket -- no longer diverges

**FIXED in `601404a6`, and the heading was wrong: it still diverged.** The
0x11 measured below is for a poll made AFTER the close. A poll(POLLIN) that is
already BLOCKED when the peer closes is answered from the kqueue event, and
that path still gave 0x10 on every root. `poll_idle_cpu`'s own wake-on-hangup
line printed it and passed. On a socket, an EOF from either filter is now
answered by `sock_poll`. `poll_rdhup_bounds` asserts the exact value for
stream and seqpacket pairs, through poll and epoll, after the close and during
a blocked wait, and also asserts that the wait is woken promptly. The
`conn_dead` arm is untouched.

**SOCK_DGRAM, measured against Linux 6.12 (camd), as asked.** It IS a
divergence, but not the one guessed. After the peer of a dgram socketpair
closes, with nothing queued:
- `epoll(EPOLLIN)` reports EPOLLIN (Linux: nothing);
- `poll(events=0)` reports POLLERR (Linux: 0);
- `recv(MSG_DONTWAIT)` fails ECONNRESET (Linux: EAGAIN).

`poll(POLLIN)` agrees (0, or 0x1 with data queued). Also found: after our
OWN `shutdown(SHUT_WR)` on a stream pair, poll says OUT|HUP|RDHUP (0x2014)
where Linux says OUT (0x4): the zero-length-send discriminator in `sock_poll`
cannot tell our half-close from the peer's close. Neither is a regression, and
neither was fixed here.

*Carried from 555 §4.* **Re-measured 2026-09-24 on devuan-amd64-test:** on a
`SOCK_STREAM` unix socketpair whose peer has closed, `poll(POLLIN)` returns
`revents=0x11` (POLLIN|POLLHUP). That is Linux 6.12's value. The `0x10` that
555 recorded is gone.

Separately, the checkpoint-restored case was fixed in `05d60ac4`
(`fd->socket.ckpt_hungup` in `fs/sock.c`, which cites 555's entry). The
`conn_dead` arm still deliberately answers POLL_ERR|POLL_HUP without POLL_READ,
for a connection iOS killed. That is a different case, and it must stay as it
is.

**Next step.** Only the regression assertion 555 asked for: a test asserting
`revents == (POLLIN|POLLHUP)` after the peer of a stream socketpair closes. Not
covered, and unmeasured against the oracle: the `SOCK_DGRAM` pair, where AOK
answers `0x1`. Measure Linux first before calling that a divergence.

**Prove it.** The new assertion passes, and so does `poll_idle_cpu`, including
the CPU-cost check that the `conn_dead` arm exists to protect.

---

## 4. `tty_hangup_signal` failed once on device, under suite load

**DONE 2026-09-24: PASS in the 556 device suite run** on the M4 iPad (booted
Devuan aarch64 root, uid 1000; kernel built 2026-09-24 11:29Z, carrying all
five fixes above): 249 pass, 0 fail, suite
exit 0. The first attempt was started under `nohup`, the launcher mistake 555
had already made. With SIGHUP ignored, `tty_hangup_signal` SKIPped and
`orphan_pgrp_wait` failed 7 checks. That was the harness, not the kernel:
restarted with `setsid` alone, both pass. `orphan_pgrp_wait` now resets SIGHUP
to its default itself (`ac869ac4`), so it can no longer be fooled that way.

*Carried from 555 §5.* It failed in the 553 device suite run ("still alive 6s
after the hangup"), then passed 3 of 3 standalone on the same device minutes
later. It passes on every local leg.

**"Passes alone, fails in the suite" is NOT by itself proof of a load flake.**
That exact shape was a real bug once (GH #542, `ptrace_group_stop`).

**Done when** the 556 device suite run passes it. If it fails again, A/B the
suspected cause in one binary before re-running anything.

---

## 5. The Launcher applets do not appear in `top`

**DECIDED 2026-09-24 by the maintainer: they are not processes, and nothing
pretends they are. `/proc/ish/applets` lists them (`3c4e085e`).** One line per
open tool window: serial, tool, Desktop, `front`/`shown`/`hidden`, age and
title. Terminal windows are left out, since their programs are processes.
Read on a guest thread, answered by the main thread within 500 ms or from the
last list it built. `/AOK/docs/workspace.md` says why applets are not in
`ps`/`top`, and `proc-ish.md` and `ktop.md` point there. Verified in the
simulator, including `hidden` on a second Desktop.

*Carried from 555 §6.* This is a **decision to make, not a bug to fix.**
Programs under `/AOK/native` appear in `ps`, `top` and `ktop`, with correct
state, `%CPU` and their own arguments. The Launcher applets do not: File
Manager, MotePad, LLM Chat, Markdown, Clock, Music, Settings, Wayland. They are
iOS UI running in the app, with no pid, no `/proc` entry and no guest address
space.

Showing them means synthesising `/proc/<pid>` entries for app-side work, which
invents pids that no guest syscall can act on. It is also a **capability-lie
risk**: a synthesised process that accepts a signal and does nothing reports a
state no real system produces.

**Done when** one of these is decided and recorded:
- show them with a distinct presentation (their own section, or an `ARCH` of
  `applet`), with `kill -9` doing something defensible and never corrupting the
  process table;
- or state in `/AOK/docs` that they are app UI, not processes, and close it.

---

## 6. RLIMIT_STACK is not pushed down for a third party

**DECIDED 2026-09-24 by the maintainer: implement. FIXED in `05d6ae19`.** The
reason for deferring had gone: the cached bound is one atomic store, so
another task's space takes it under that task's `general_lock`. The same gap
covered prlimit on one's own tgid from a non-leader thread, which resolves to
the leader. exec re-reads the limit after installing the new space, so a
change racing an exec is not lost. `stack_rlimit_pushdown` covers both kinds
of third party with a control: Linux passes, the old binary fails, and the
new one passes on all six roots.

*Carried from 555 §7, and from 553 and 554.* `prlimit64` against another
process updates its limits but not its address space. So a lowered
`RLIMIT_STACK` takes effect at its next `exec`, not immediately. Reading another
task's `->mm` needs `general_lock`, and the guard gap still bounds the stack
meanwhile, so the failure is "bounded less tightly than asked", never unbounded.

**Done when** it is either implemented, or moved to `docs/TODO.md` with this
reasoning. 555 set that bar: with no consumer after four cycles, TODO.md is
where it belongs. This entry exists so the decision is made, not carried again.

---

## For the 556 release notes

**Swap to an external drive (#605) is untested on a real drive.** Contributed
by @KrisButIAmAnOSDev, finished here on 2026-09-24. What was tested:
- a code review;
- the kernel path, through `ISH_GUEST_SWAP_FILE` on the CLI, with
  `swap_roundtrip` passing on APFS, exFAT and FAT32 disk images;
- a clean Xcode device build.

The folder picker, security-scoped bookmarks across launches, and a real USB
drive on iPadOS have NOT been exercised: nobody had a drive to test with. The
notes must say so, and call the feature experimental. It is off by default.
Watch for launch-time stalls: the whole area is reserved on the main thread at
launch, which is instant on APFS and unmeasured on USB.

---

## Deferred, by decision

**External display ([#540](https://github.com/emkey1/ish-AOK/issues/540)):
deferred again for 556** (maintainer's call, 2026-09-24). It stays parked where
it was:
- `6156597e` is unmerged on `worktree-external-display-540`, and is a child of
  the reverted pair.
- The resume plan is in `docs/external_display_plan.md`.
- Raise it early in the 557 pass, not last.
