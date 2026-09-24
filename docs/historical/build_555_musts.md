# build 555 musts (archived, not maintained)

**This file is history.** It is the deferred-work list written during the 554
release run for build 555. It is kept because 556's list records what happened
to each entry, and the reasoning is worth being able to check. Nothing adds to
it.

The live list is `docs/build_556_musts.md`. §1 here was fixed during 555. §2–§7
were re-checked on 2026-09-24 and carried there, and §4 no longer diverges.

---


Work carried out of 554, with the diagnosis already done so nobody has to
re-derive it. Each entry says what is **established**, what the **next step**
is, and how to **prove** it afterwards.

Started 2026-09-08, immediately after `builds/iSH-AOK_554` was tagged.
Supersedes [docs/historical/build_554_musts.md](historical/build_554_musts.md).

**Three of that document's ten open entries were already fixed when it was read
at the top of this cycle**, two of them by work that never mentioned them — see
*Closed during 554 while the doc still listed them* at the bottom. A fourth had
a diagnosis that does not reproduce at all. That is a 40% staleness rate on a
six-day-old document, and it is the reason every entry below was re-measured
before being carried rather than copied forward.

That re-measurement paid for itself the same day: §1 was carried for two
releases as an unbounded "the debugging tools do not work", and once measured
it was one line in `PTRACE_INTERRUPT` and is now fixed.

The release's **theme** is in [docs/roadmap.md](roadmap.md) and it is
persistence. This document is the other half: the work that has to happen
around it.

---

## Order for the cycle

The roadmap says of the debugging tools that this is "the item most likely to be
worth more than its place in the list". 555's headline is a checkpoint/restore
feature whose phase 0 is *an inventory of what a live guest is holding* — which
is precisely the work a functioning `strace` and `gdb` make cheap and an absent
one makes archaeology. So:

1. **The debugging tools** (§1 below). It is a prerequisite for the headline,
   not a parallel track. **Done 2026-09-08** -- the re-measurement found the
   carried diagnosis wrong, and the real cause turned out to be one line in
   `PTRACE_INTERRUPT`. `strace -p` no longer kills what it attaches to. What is
   left of it is listed there and is smaller than what closed.
2. **OS snapshot** — roadmap. The cheaper and more certain of the two
   persistence items, and it lands #575 on the way past. **CLI prototype done
   2026-09-08** (`fs/fake-snapshot.c`, `/proc/ish/snapshot`): clone a quiesced
   root, boot the clone, diverge both ways, all verified including under write
   load. It also corrected the roadmap's cost model -- a snapshot is O(files),
   not O(bytes), at ~25 us per directory entry -- so the app side needs progress
   and a cancel rather than a spinner. What is left is the app side and restore.
3. **Suspend to disk** — roadmap. **Done, and past phase 0.** The inventory
   said the common session is mostly the easy kind, so it went on: the machine
   can be stopped, a multi-process guest saved and restored, and the app saves
   on backgrounding and resumes on launch behind a Settings switch. A native
   zsh describes itself and comes back with its session; anything that cannot
   be described is refused by name rather than half-saved.
4. **Desktop chrome** — #580, #579, and #483/#482 (confirm they are one bug
   before scheduling two).
5. **The rest of this document**, as fill.

§§2–4 below are cheap, bounded, and each has a written next step; they are the
right things to pick up when a headline item is blocked on a device run.

---

## 1. A clean ptrace detach killed the tracee — FIXED 2026-09-08

**Closed the day this document was written**, and kept here in full rather than
moved out, because what it took to find is the reusable part: two of the three
diagnoses on the way to it were wrong, and each was wrong in a way that looked
like an answer.

**The carried entry was wrong.** `docs/historical/build_554_musts.md` said
`strace` and `gdb` "kill the thread they attach to", because threads here are
children of their creator so a wait after attaching to a non-leader resolves to
the wrong task. A probe doing `PTRACE_ATTACH(tid)` then `waitpid(-1, __WALL)`
returns the thread's own tid with status `0x137f`, byte-identical to the oracle.
That mechanism was fixed by the `__WALL` work in `do_wait` that shipped for
`strace -f`, and nobody noticed it had closed this entry's stated cause.

**The symptom was real.** Measured on `build/alpine-arm64-test` against Devuan 6
/ Linux 6.12, running `strace` against a two-thread process:

| case | AOK before | Linux |
|---|---|---|
| `strace -p <non-leader tid>`, SIGINT to detach | target **DEAD** | alive |
| `strace -p <leader>`, SIGINT to detach | target **DEAD** | alive |
| `strace -f -p <leader>`, SIGINT to detach | target **DEAD** | alive |
| `strace -p <non-leader tid>`, tracer SIGKILLed | target alive | alive |

**The second wrong diagnosis was "the explicit detach path".** It is the obvious
read of that table — the surviving case is the one that never detaches — and
`PTRACE_DETACH` does have a real defect beside it (it never unlinks
`ptrace_siblings`; only kernel/exit.c does). But a test covering all eight
attach/detach shapes passed every one. **Not reproducing is data**: it meant the
difference was something `strace` does that a minimal detach does not.

**What it actually was.** The shell can say how a process died, and it said
**133 — that is 128 + 5, SIGTRAP.** `PTRACE_INTERRUPT` was implemented by
sending a real SIGTRAP to the tracee (kernel/ptrace.c). While the task is traced
that signal never reaches the program: `signal_delivery_stop` intercepts it and
reports the stop, which is why it worked at all. The moment the tracer detaches
it is an ordinary SIGTRAP again, and SIGTRAP's default action is to terminate.

`strace`'s detach interrupts a *running* tracee and then waits — and a program
making a syscall every few milliseconds reaches a syscall-stop of its own first.
So strace saw the stop it wanted, detached, and left the interrupt's SIGTRAP
sitting in the queue with nothing left to intercept it. Linux has no such
window: its interrupt is `JOBCTL_TRAP_STOP`, a flag rather than a signal, and
`__ptrace_unlink` clears it on detach.

**The fix.** Three parts, in `kernel/ptrace.c`, `kernel/signal.c` and
`kernel/exit.c`:

- `PTRACE_INTERRUPT` stamps its SIGTRAP with `SI_PTRACE_INTERRUPT_`, a si_code
  outside anything a guest can produce.
- `ptrace_discard_interrupt_traps` drops exactly those traps, and runs on both
  detach paths — the explicit one and the tracer-death sweep — under
  `ptrace.lock` and *before* the notify, because the tracee is parked holding
  that same lock and is free to take the trap the instant it is released.
- Identified by tag rather than by a count kept alongside the send. A count was
  written first and was wrong: `send_signal` drops the signal instead of
  queueing it when SIGTRAP is `SIG_IGN` or the task is exiting, so the count
  would over-run and eat the next SIGTRAP the guest raised for itself.
- The tag is normalised away in `ptrace_stop_common` before the stop is
  published, so no tracer can read it back through `PTRACE_GETSIGINFO`. That
  also *corrects* the value: an interrupt-stop's si_code was 0, because the
  interrupt was sent with `SIGINFO_NIL`, where Linux reports
  `(event << 8) | SIGTRAP`.

Two things were repaired in passing: `ptrace.seized` was never cleared on
detach, so a later `PTRACE_ATTACH` of the same task inherited it and had its
group-stops reported as seize-style event-stops to a tracer that never seized;
and `do_wait`'s `P_PID_` branch resolved a traced non-leader to
`task->group->leader` before testing for a stop, so `waitpid(<tid>, __WALL)`
**hung** where `waitpid(-1, __WALL)` returned correctly. That second one is the
gdb half: `linux_nat_post_attach_wait` waits on the pid it just attached to.

**Proved.** `tests/manual/ptrace_detach_survives.c`, registered in all three
places, covers eight attach/detach shapes, the interrupt-unconsumed shape that
actually reproduced it, and the tracer-death shape that always worked and must
keep working. 10/10 on the Linux oracle first — where the first draft's expected
stop status was refused for all four SEIZE cases, because a seized tracee's
interrupt-stop is `PTRACE_EVENT_STOP`, not a SIGSTOP delivery-stop — then 10/10
in the guest. The four real `strace` scenarios all leave the target alive, and
`ptrace_attach`, `ptrace_group_stop`, `ptrace_thread_follow`, `ptrace_exit_kill`,
`ptrace_trap_siginfo`, `native_ptrace_group_stop`, `ptrace_singlestep`,
`signal_core`, `signal_stop_cont`, `signal_forced_trap`, `process_lifecycle` and
`orphan_pgrp_wait` all still pass. Beyond the ptrace set: the **full arm64 guest
suite, 201 PASS / 0 FAIL**, and the ptrace group again on `devuan-amd64-test`
(x86_64 glibc, 9/9) because the change is in shared kernel code and only the
emulator underneath it differs.

**What is left, and it is smaller than what closed.**

- ~~**`PTRACE_INTERRUPT` should not use a signal at all.**~~ **Done
  2026-09-17.** It is now `ptrace.trap_stop`, a per-task flag like Linux's
  `JOBCTL_TRAP_STOP`, and the SIGTRAP, its `SI_PTRACE_INTERRUPT_` tag and the
  detach-time discard are gone. A tracee with SIGTRAP ignored or blocked now
  stops, an interrupt sent while it is stopped is taken once it resumes, and a
  `read` the interrupt broke into restarts instead of returning `EINTR`. The
  same flag gives a seized tracer's new children their Linux first stop,
  `PTRACE_EVENT_STOP` rather than a SIGSTOP, which `strace -f` had been
  printing and injecting into every child. Covered by
  `tests/manual/ptrace_seize_trap_stop.c`.
- ~~**`PTRACE_DETACH` still does not unlink `ptrace_siblings`.**~~ **Fixed
  2026-09-17**, with the change that made a tracee's exit reach its tracer
  (`tests/manual/ptrace_tracee_exit.c`). By then it mattered: the tracer's wait
  counts its tracee list to decide between blocking and ECHILD, so a detached
  task left on it kept a tracer from ever hearing "no children", and a second
  attach by anyone else linked the same node into two lists.
- ~~`gdb -p` end to end has not been re-run.~~ **Done, and it works.** `gdb -q
  -p <non-leader tid> -batch -ex bt -ex 'info threads' -ex detach` on a live
  guest process prints a full symbolic backtrace -- through musl's
  `__syscall_cp_asm`, `__clock_nanosleep` and `usleep` into the program's own
  function -- detaches with rc 0, and leaves the process running. Attaching to
  the leader instead reports `[New LWP <tid>]`, so it finds the second thread
  too. No `linux_nat_post_attach_wait` assertion. (`gdb` is not in the stock
  test root; `apk add gdb` first.)

**So the tools work.** `strace -p`, `strace -f -p` and `gdb -p` all attach,
report, detach, and leave the target running -- which was the whole point of
putting this first, and the roadmap's claim that it was worth more than its
place in the list held up.

**Two lessons worth keeping.** *A test that does not reproduce the bug is
telling you which shapes are innocent* — eight passing cases is what turned
"the detach path" into "something strace does that this does not". And *ask the
corpse how it died*: one `wait $!` reporting 133 replaced a whole afternoon of
reading ptrace code, and it was available from the first hour.


## 2. `lock not` and `lock neg` are SIGILL on an i386 guest

**Established, and re-verified 2026-09-08.** The i386 `LOCK` table in
`emu/decode.h` (the `case 0xf0:` block, ending at the `default: UNDEFINED` near
line 1689) has the ALU pairs, the `80/81/83` immediate group, the `0F` atomics,
`86/87` xchg and `FE/FF` inc/dec — **and still no group-3 entry**. There is no
`case 0xf6` or `0xf7` in it. So `lock notl (mem)` and `lock negl (mem)` fall
through to `UNDEFINED` and kill the guest with SIGILL. Real Linux runs both.
Found by `tests/manual/x86/atomic_lock_contended.c`, which skips those two forms
on i386 for exactly this reason and says so.

**Next step.** Unchanged from the carried entry, which was correct: add `not`
and `neg` to the `.irp` list in `do_op_size_atomic`
(`jit/gadgets-aarch64/math.S:2217`) — `not` is `mvn` with **no** flag changes,
`neg` is `0 - operand` with the full sub flag rule — then add `case 0xf6`/`0xf7`
with a group-3 switch to the LOCK table.

**Prove it.** Un-skip the two forms in `atomic_lock_contended` and require the
i386 leg to pass, plus the whole i386 atomics set: `do_op_size_atomic` is shared
by every i386 atomic, so a mistake there breaks all of them.

---

## 3. An iosfs mount made through the new mount API does not persist

**Established, and re-verified 2026-09-08**: there is still no `relocated` hook
in `struct fs_ops` and no caller of one — `grep relocated kernel/fs.h fs/mount.c
fs/iosfs.m` is empty. Carried unchanged through 553 and 554.

iosfs keys its security-scoped bookmark on `mount->point` at mount time, and a
mount made through `fsopen`/`fsconfig`/`fsmount`/`move_mount` is created at a
private staging path (`/.ish-fsmount/<n>`) and relocated later, so the key is
wrong. 552 stopped persisting staging-path keys because persisting them
resurrected a permanent phantom mount on every launch; the cost is that such a
mount no longer survives a relaunch.

**Next step.** Re-key at relocation. `mount_relocate` (`fs/mount.c`) has the
mount and both paths; an optional `relocated(mount, old_point)` in
`struct fs_ops` (`kernel/fs.h`) lets iosfs move the bookmark to the real path.
Call it after unlocking `mounts_lock`. Note the bookmark is not merely
*mis-keyed* at mount time, it is **not stored at all**, so iosfs also needs a
non-persisted side table keyed by the staging path to move from — or it must
re-derive the bookmark, which needs the security-scoped URL it only holds during
`iosfs_mount`.

**Prove it.** Mount an iCloud directory with a util-linux `mount(8)` new enough
to use the new API, relaunch the app, and require the mount back at the path the
user asked for — with nothing under `/.ish-fsmount/` in `/proc/mounts` either
before or after. Needs a device and an app relaunch, which is why it has now
been deferred out of two releases; if it is not scheduled against a device run
in 555 it should be moved to TODO.md and stop being called a must.

---

## 4. POLLHUP without POLLIN on a closed socket

**Established, and *not* re-measured — measure before fixing.** On a unix
socketpair whose peer has closed, `poll(POLLIN)` returned `revents=0x10`
(POLLHUP alone) under AOK against `0x11` (POLLIN|POLLHUP) on Linux 6.12,
measured by `tests/manual/poll_idle_cpu.c`, which accepts either because it is
testing something else. Linux sets POLLIN as well because a closed socket **is**
readable: a read returns 0 for EOF. A program that waits for POLLIN before
reading, and treats POLLHUP as informational, never reads the EOF it is being
told about.

**The measurement is a cycle old and `sock_poll` changed underneath it.**
554 added the `conn_dead` arm (`fs/sock.c:8709`), which returns
`POLL_ERR | POLL_HUP` and **deliberately excludes POLL_READ**, with a comment
explaining that a poll loop told "readable" and then handed an error by every
`recv` is a 100%-CPU spin. That is a different case — an iOS-killed connection,
where there is no EOF to read — but it is close enough that a fix here must not
be pattern-matched onto it.

**Next step.** Re-measure the socketpair case against the oracle first. If it
still diverges, find where the peer-closed result is composed and add POLL_READ
alongside POLL_HUP *for that case only*. Check the half-close case separately:
`fs/sock.c:8724` already has careful reasoning about EPOLLRDHUP vs EPOLLHUP,
paid for with a zero-length send, and it must not be disturbed.

**Prove it.** A test asserting `revents == POLLIN|POLLHUP` after the peer
closes, checked against the oracle first, plus `poll_idle_cpu` still passing —
including its CPU-cost assertion, which is what the `conn_dead` arm exists to
protect.

---

## 5. `tty_hangup_signal` failed once on device, under suite load

Carried unchanged, and deliberately not dismissed. It failed in the 553 device
suite run — "still alive 6s after the hangup" — then passed 3 of 3 standalone on
the same device minutes later, and passes on all five local legs. The test gives
the hangup a 6-second budget and the device was running the rest of a 188-test
suite at the time.

**"Passes alone, fails in the suite" is NOT by itself proof of a load flake** —
that exact shape was a real bug once (GH #542, `ptrace_group_stop`). If it
recurs, A/B the suspected cause in one binary before re-running anything.

**Next step.** Nothing, unless it recurs. This entry is the date it was not yet
a regression.

---

## 6. The Launcher applets do not appear in `top`

Carried unchanged, and still a **design decision rather than a bug**. Programs
under `/AOK/native` do appear in `ps`, `top` and `ktop` with correct state,
`%CPU` and — since 2026-09-02 — their own `ARGUMENTS` rather than the parent's.
What does not appear is the Launcher applets: File Manager, MotePad, LLM Chat,
Markdown, Clock, Music, Settings, Wayland. Those are not guest processes at all.
They are iOS UI running in the app, with no pid, no `/proc` entry and no guest
address space.

Showing them means synthesising `/proc/<pid>` entries for app-side work —
inventing pids that no guest syscall can act on, so `kill` on one has to mean
something or be refused. Worth doing only if the goal is "the user can see what
the app is doing", in which case a distinct presentation (a separate section, or
an `ARCH` value reading `applet`) is more honest than pretending they are
processes.

This is also a **capability-lie risk**: a synthesised process that accepts a
signal and does nothing reports a state a real system never produces.

**Prove it.** Whatever is decided, `kill -9` on such an entry must do something
defensible and must not corrupt the process table.

---

## 7. RLIMIT_STACK is not pushed down for a third party

Carried unchanged from 553 and 554, and still deliberate. `prlimit64` against
another process updates that process's limits without updating its address
space, so a lowered `RLIMIT_STACK` takes effect at its next `exec` rather than
immediately. Reading another task's `->mm` needs `general_lock`, and the stack
stays bounded by the guard gap meanwhile, so the failure mode is "bounded less
tightly than asked", never unbounded.

**Next step.** None, unless something real depends on it. Three cycles carried
is enough to say so: if 555 ends without a consumer, this belongs in TODO.md
rather than in a musts document.

---

## 8. Issue hygiene, which is part of fixing bugs

[#541](https://github.com/emkey1/ish-AOK/issues/541) — *ptraceomatic does not
run: tracee reaped during setup* — has been **fixed since 2026-08-20** and is
still open on GitHub, confirmed 2026-09-08. Close it, with the commit named. A
fix the reporter never hears about did not fully happen.

It is also in §1's neighbourhood, so re-run ptraceomatic as part of the ptrace
work rather than closing it blind.

**Two issues are one investigation.**
[#568](https://github.com/emkey1/ish-AOK/issues/568) (slow throughput) and
[#523](https://github.com/emkey1/ish-AOK/issues/523)'s reproducible half (a
15.3 s TLS handshake tail against a sub-second median) are a wait not being
woken rather than work being slow, which puts them in the poll/quiesce
neighbourhood. It reproduces with plain `curl`, so it needs neither Go nor the
AUR to chase. Schedule them together or not at all.

---

## Closed during 554 while the doc still listed them

Recorded because two of the three were closed by work that never mentioned the
entry, which is how a musts document goes stale without anyone noticing.

**A NULL dereference in the ptrace memory-write path.** Fixed.
`__user_write_task_mem` (kernel/user.c:135) now checks `mem_ptr`'s result in
*both* places it mints a pointer — before the trace hook at line 146, and again
after it at line 152, because the hook may drop the read lock and free the
mapping underneath. The second check is the interesting one and was not part of
the carried diagnosis.

**`mem_mapped_page_count` walks a page table without the lock.** Fixed, and
with a better answer than the entry proposed. `task_maxrss_kb`
(kernel/resource.c:245) now holds `general_lock` **across** the walk — by
`trylock`, returning the latched value on failure, and skipping the lock
entirely when this thread already holds it, which is `do_exit`'s own call. The
comment there records why a reference-count approach was rejected: retaining
the mm would make the timer thread the last referrer and run `mem_destroy` on a
thread with no `current`.

**The amd64 JIT still bridges locked instructions.** Closed. The amd64 JIT now
has its own `ldaxr`/`stlxr` fast path with an alignment check and a bridge
fallback — `jit/gadgets-aarch64/math.S:4598` documents it, and notes explicitly
that "the 'lock (atomic path bridges)' comments described a gap, not a design".
`LOCK <alu> [mem],imm`, `XCHG [mem]` and `CMPXCHG [mem],reg` all compile
natively. This landed as part of the zero-fallback amd64 work rather than
against this entry.

**And one whose diagnosis was wrong rather than stale:** the strace/gdb entry.
See §1. The mechanism it named was fixed by the `__WALL` work in `do_wait` that
shipped for `strace -f`, and the symptom it described has a different cause.

---

## Also open, and tracked elsewhere

Not repeated here, but a reader of this document should know where they are.

**554's own known gaps** (`docs/release-notes-since-iSH-AOK_554.md`) — two of
them are directly the 555 theme and should be read before the suspend-to-disk
design, not after: **swap does not survive a guest reboot** (the area is
recreated empty, and `swapoff` tears it down rather than parking it), and
**swap does not evict pages shared by a forked family**. A checkpoint feature
built on a pager whose store is deliberately non-persistent needs to say which
of the two changes. Also there: `signal_child_burst` on the A9 iPad,
unresolved; a single unreproduced `wayland_scm_shm` failure; and the macOS-only
FileProvider limitation, which is a protocol port and affects no iOS user.

**The conformance long tail** is in [TODO.md](TODO.md), each entry with its
measurement: `PROT_EXEC` never enforced (guest W^X is decorative — two candidate
designs, both rejected on cost, and it deserves its own before/after benchmark
rather than being folded into a sweep); PI futexes ENOSYS; `pipe(2)` cannot
honour PIPE_BUF atomicity while it delegates to a host pipe; `tmpfs size=`
accepted and not enforced; `fcntl(F_GETFL)` reporting an `O_NONBLOCK` the guest
never set; `/proc/locks` absent; FUSE with no attribute cache and three absences
following from it; and `SEEK_DATA`/`SEEK_HOLE` still `EINVAL` on tmpfs, now the
odd one out rather than one of a pair since realfs and FUSE both answer it.

**atop's accounting daemon wedges boot** (TODO.md) is the one in that list with
a device report behind it and an unverified half: AOK's netlink socket appears
never to become readable and never to report an error after a failed family
lookup, so `atopacc` blocks in `ppoll` forever and every init script after
`S01atop` — sshd included — never runs. Implementing the `netatop` family is
**not** the fix.
