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

---

## 1. `lock not` and `lock neg` are SIGILL on an i386 guest

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

*Carried from 555 §5.* It failed in the 553 device suite run ("still alive 6s
after the hangup"), then passed 3 of 3 standalone on the same device minutes
later. It passes on every local leg.

**"Passes alone, fails in the suite" is NOT by itself proof of a load flake.**
That exact shape was a real bug once (GH #542, `ptrace_group_stop`).

**Done when** the 556 device suite run passes it. If it fails again, A/B the
suspected cause in one binary before re-running anything.

---

## 5. The Launcher applets do not appear in `top`

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
