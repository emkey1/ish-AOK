# 41. The honest gaps

A book that only describes what a system does is a brochure. This chapter is the
other list — and it is longer than a marketing document would like, which is the
point.

The gaps sort into four kinds, and the distinction matters more than the
inventory:

- **Architectural** — will not be fixed, because fixing it would be a different
  system.
- **Diagnosed, not fixed** — understood, measured, costed, and not done.
- **Deferred on purpose** — built, judged, and rejected.
- **Structural ceilings** — bounded by physics or arithmetic rather than by
  effort.

## 41.1 Architectural: there are no namespaces

No PID namespaces, no mount namespaces, no network, user or cgroup namespaces.
`CLONE_NEWUTS` and `CLONE_NEWIPC` are the two exceptions: a UTS namespace is a
hostname in a refcounted struct, and an IPC namespace is System V shared
memory, semaphores and message queues kept per namespace instead of global.
`setns(2)` can join either one — or a namespace nothing is in any more — through
its `/proc/<pid>/ns` file. Every other kind is `ENOSYS` to `unshare`, and
`setns` into one is the no-op of joining the only one there is — a mount
namespace's join moves the caller's root and working directory to the real
root, as Linux's does, and changes nothing else.

Network is the one kind with no `/proc/<pid>/ns` file at all, as on a Linux
kernel built without network namespaces. systemd decides whether a kind
exists by checking for that file, and while `ns/net` was listed every unit
with `PrivateNetwork=yes` went on to call `unshare(CLONE_NEWNET)`, got
`ENOSYS`, and failed: on Arch that took `systemd-hostnamed`,
`systemd-localed` and `shadow.service` with it, so `hostnamectl` and
`localectl` could not start their daemons. With the file absent systemd says
"proceeding without" and runs them.

`clone` refuses the same flags (`CLONE_NEWNS`, `CLONE_NEWPID`, `CLONE_NEWNET`,
`CLONE_NEWUSER`, `CLONE_NEWCGROUP`), but with `EINVAL`, the answer of a Linux
kernel built without that namespace type. The two calls give different errnos
on purpose: each is the one its callers read as "no sandbox here, carry on".
systemd forks its generator sandbox with `clone(CLONE_NEWNS)` and runs the
generators unsandboxed on `EPERM` or `EINVAL`, while its `unshare(CLONE_NEWNS)`
for a service's mount namespace accepts `ENOSYS` and treats `EINVAL` as a
failure. Build 557 briefly made `clone` say `ENOSYS` too, and systemd as PID 1
stopped at "Failed to start up manager." (Before that `clone` answered `EPERM`,
which reads as "you may not" to a caller who is root and may.)
`tests/manual/namespace_errno.c` holds both rules.

So nothing container-shaped runs. No Docker, no `unshare -m` or `-n`, no
rootless podman, no per-service filesystem views. There is one process table,
one filesystem tree, and one network, visible identically from every root and
every chroot.

This is not a missing feature with a ticket. The absence runs through the design
— Chapter 10's task model, Chapter 16's global mount table, Chapter 21's
`/AOK`. And Chapter 21 is also where it reads as an *advantage*: one true
`/proc` from anywhere is what makes `ktop`'s cross-architecture process list
possible, and on a single-user device the isolation being traded away was
protecting nobody.

**The other half of containment is capabilities, and there the gap is
diagnosed rather than architectural.** Extended attributes and file
capabilities landed in 556 — `setcap cap_net_bind_service+ep` lets uid 1000
bind port 80, taking effect at exec as it does on Linux. But
`current_capable()` is `superuser() || <the bit in cap_effective>`, so an
effective uid of 0 still counts as *every* capability whatever its effective
set says. A root process that drops capabilities to confine itself — which is
what `capsh --drop` and systemd's `CapabilityBoundingSet=` are for — is not
confined. `tests/manual/exec_setid_unsafe.sh` sees it in exactly one of its 51
rows: root drops `CAP_SYS_PTRACE` and `CAP_SETUID`, calls `PTRACE_TRACEME`, and
execs a program set-user-ID to 1000; Linux refuses the new uid, AOK grants it.
Every privileged syscall asks the same function, so the fix is tree-wide, and
none of the capability tests yet run as root with a reduced effective set.

## 41.2 Closed in 556: `PROT_EXEC` was diagnosed, then fixed

Through build 555, every guest `.data` and `.bss` page was executable, and any
guest JIT's own W^X discipline was decorative. Chapter 13 has the full history,
and it is the model entry for how a gap should be recorded: measured against
Linux 6.12 in a two-row table, **graded** as a mitigation gap rather than a hole
(exploiting it needed a separate memory-corruption bug in guest software), and
carrying both candidate designs with the specific reason each was or was not
taken.

It is also the model entry for what a diagnosed gap is *for*: 556's fix
(`bd055d53`) took the second design off the page and built it, unchanged in
shape, because the record had already done the hard part of deciding what the
right shape was. The difference between a diagnosed gap and a deferral is that
somebody can act on it — and eventually somebody did.

## 41.3 Diagnosed, then fixed: `fcntl(F_GETFL)` lied about a pipe

This one is small, current, and unusually instructive, because it is a bug
sitting exactly between two correct decisions.

The idiom that trips over it is in every codebase:

```c
int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);
... read until EAGAIN ...
fcntl(fd, F_SETFL, flags);          // "restore"
```

Under AOK the restore leaves the pipe **non-blocking**, and every later `read`
returns `EAGAIN`.

It is not an `F_SETFL` bug: `flags` was *already* `O_NONBLOCK` when it was read
back, so the restore faithfully wrote what it was given.

**Where the lie comes from.** A guest pipe is a host pipe, and
`realfs_getflags` answers `F_GETFL` by asking the **host** descriptor. Meanwhile
`realfs_read` and `realfs_write` permanently force that host descriptor
non-blocking the first time the guest does a *blocking* read or write on it,
and deliberately never restore it.

And that second decision is correct. Restoring it races a sibling task into an
uninterruptible, `SIGKILL`-proof host `read` — a real pipe-herd hang that was
fixed by exactly this non-restoration.

So the host flag is an implementation detail that must not be visible, and it
is. Before the first read `F_GETFL` says 0; after it says `O_NONBLOCK`, with the
guest having done nothing. The kernel's own `fd->flags` — which is what actually
governs guest blocking semantics — still says blocking, and the two disagree
until the idiom's "restore" writes the lie into `fd->flags` as well, and then
they agree on the wrong answer.

It was still true in 557, measured for this chapter on aarch64 and x86_64
guests in the Mac CLI: a fresh pipe reported 0; its write end reported
`O_NONBLOCK` after one blocking write, its read end after one blocking read, and
after the get-set-restore the read end *was* non-blocking. The recorded next
step was precise, including its own scope warning:

> `realfs_getflags` should report the guest-visible flags from `fd->flags` for
> the bits the guest owns (`O_APPEND`, `O_NONBLOCK`) and take only the access
> mode and the rest from the host. Small, but it needs its own test: the
> get-modify-set idiom is everywhere, and silently turning a pipe non-blocking
> under a program that never asked is the kind of thing that surfaces far from
> here. Worth checking whether sockets and ttys answer `F_GETFL` the same way
> before fixing just the one path.

The check came first -- a `socketpair` and a pty reported no `O_NONBLOCK`
before or after, so the pipe path really was the one path -- and then the fix,
in October 2026, exactly as recorded. `realfs_getflags` now answers the bits the
guest owns from `fd->flags`, and with them the open-time flags the host's
`F_GETFL` never keeps and Linux does report: `O_DIRECTORY`, `O_NOFOLLOW`, and
the `O_LARGEFILE` a 64-bit Linux adds to every open. `fcntl_getfl_flags` covers
the idiom and checks each flag against Linux, 64- and 32-bit.

Two correct decisions, one wrong seam. That is the characteristic shape of a
bug in a system this size, and it is why Chapter 40's rules are about *checking*
rather than about care.

## 41.4 The interpreters are legacy, not dead

The `engine` build option offers exactly one value. New work targets the JIT.
And yet:

- `emu/amd64_interp.c` is still the **largest single file in the tree** at
  about 18,000 lines.
- It is still what runs on non-aarch64 hosts, because the amd64 JIT's gadgets
  exist only for aarch64 (Chapter 7).
- And it is still where **most `lock`-prefixed instructions** execute. Nearly
  every eligibility predicate in `jit/gen.c`'s amd64 front-end requires the
  lock prefix to be absent, so a locked `xadd`, `cmpxchg`, `inc` or `neg`
  leaves the JIT for a C helper or the interpreter. Two families no longer do:
  since 556, `lock add/or/and/sub/xor [mem], imm` and `xchg [mem], reg` are
  `ldaxr`/`stlxr` gadgets, as the i386 JIT's locked instructions have long
  been.

One bullet has come off this list. It used to say the interpreter was what GNU
`as` executed on, behind a containment workaround for crashes nobody had
root-caused, with a probe harness waiting for somebody to re-run it. Somebody
did, on 2026-09-07: the full guest suite with its cache disabled, so gas ran 200
times under the JIT with no errors and no block fallbacks, and one nontrivial
translation unit assembled byte-identically under both engines. The bypass was
deleted (`jit/jit.c` keeps the evidence in a comment), and with it a blind
spot — `as` had never counted toward any "zero fallbacks" measurement.

A second came off on 2026-10-08. It said the interpreter was still where some
AVX-512 executed for amd64: an EVEX instruction the JIT's table lacked went to
`amd64_jit_vex`, which decoded it in the interpreter's file before `emu/avx.c`
did the arithmetic. Now the JIT runs every VEX and EVEX instruction as gadgets,
and one it does not know is #UD — checked against an Intel CPU's answer for
every VEX encoding of maps 1-3 and every EVEX encoding of maps 1-3, 5 and 6.
The bridge is gone; only an instruction whose bytes cannot be read still lands
in the interpreter, as any undecodable instruction does (Chapter 5).

The locked instructions used to cost twice, and the correctness half is now
paid. Until 553 the interpreter serialised locked instructions on the global
`atomic_l_lock`, which does not interlock with a host atomic — so a kernel-side
read-modify-write on guest memory raced with an amd64 guest's own atomics.
`FUTEX_WAKE_OP` lost 1107 of 40,000 increments that way, and the fix for 552 was
`kernel/futex.c` taking `atomic_l_lock` itself: agreement with the weaker
mechanism rather than a repair of it.

Writing this section down is what got it repaired, and the repair found that the
description above had been too kind. One predicate did **not** reject the
prefix — the block that emits `amd64_jit_mem_op`, which is the only
implementation of `<alu> [mem], reg` the JIT has. It compiled the instruction
and discarded the prefix, so that whole family was not atomic against anything,
including other guest threads: four of them lost 3876 of 200,000 increments.
`xchg reg, [mem]` was worse still — under the global lock it did not merely
weaken, it **livelocked**. Both had been true for the whole life of the amd64
JIT and no test could see them, because every x86 atomics test in the tree was
single-threaded and a lost update is the only symptom a broken atomic has.

553 routes every locked amd64 site through real host atomics
(`x86_atomic_rmw` and friends in `emu/tlb.c`), `atomic_l_lock` is gone from that
path, `kernel/futex.c` is back to a plain compare-exchange, and
`tests/manual/x86/atomic_lock_contended.c` runs nineteen locked forms from four
threads at once. What remains is the throughput half: apart from the two
families above, a locked instruction still leaves the JIT for a C helper
instead of becoming a gadget. What that costs a real workload has not been
measured.

The *misaligned* half closed in 557 (`4d7a6981`). A locked access that is not
naturally aligned — which x86 allows, and which the i386 ABI makes ordinary for
a `uint64_t` in a struct — still took the global lock around a plain read and
write, so it was atomic against other locked instructions and nothing else: a plain
store from another thread could land in between, and
`tests/manual/x86_unaligned_lock` lost up to 60% of them on amd64. It now uses a
16-byte host compare-exchange when the operand sits inside one 16-byte block,
and the address space's writer lock when it straddles two.

**Its i386 twin closed later in the same cycle** (`ce74598e`). The i386 JIT's
locked gadgets for 16- and 32-bit operands checked alignment and then ignored
the answer: on a misaligned operand they called a tracing helper that did
nothing and ran `ldaxr`/`stlxr` on the misaligned host address anyway, so a
`lock addl` on a word straddling a 16-byte boundary killed the whole app with a
bus error. Each such gadget now sends a misaligned operand to the amd64 JIT's C
slow path. One edge is still open, found by the release's device leg: across a
*page* the slow path takes the address space's writer lock, which prefers
writers, so a thread doing locked increments there can starve a thread doing
plain stores to the same word. The fix that removed the starvation cost that
case about 100x in throughput and was not committed; `docs/TODO.md` has it.

`emu/arm64_interp.c` survives for a different reason: as a bisection escape
hatch behind `ISH_ARM64_FORCE_INTERP=1`, with a comment that is candid about
expecting it to crash on anything nontrivial.

"Legacy" here means "not where new work goes", not "vestigial". A reader who
assumes otherwise will misread both the amd64 story and the AVX one.

## 41.5 Native programs: the open list

Part V's mechanism is finished; its coverage is not.

**The `argv` ownership class** (Chapter 25) is fixed where it was found and not
swept. `find` was fixed; `du`, `stat`, `rm` and `wget` were audited and are
mostly unreachable for incidental reasons — a plain `du` in the guest hit the
distribution's coreutils, since `/AOK/native` holds only the multicall entries,
and `wget`'s fetch path is compiled out of every build so only its argument
handling is reachable at all. "Unreachable today" is a weaker guarantee than
"fixed", and 557 proved it: `native-links.sh` now links every applet into
`/usr/local/bin`, ahead of `/bin`, and the provisioning scripts run it, so the
plain `du` is SmallCLUE's wherever that has been done. The audit tool
exists because the hand-written exclusion list was
not good enough: `env` was missed, and since one test harness runs
`env ... bash ...`, installing the symlinks took its suite from 217 passing to
zero.

**Unrouted host symbols** do not ship: Chapter 23's gate fails the build on one.
Run against a 557 build, it finds 283 host symbols referenced across every
native archive, all of them on the pure list, and none needing work. What
remains is the porting of programs not yet native, and the gate's `--report`
mode enumerates that for any candidate — its third list is exactly the
outstanding work. Three are already in the binary and only refuse: smallclue's
`git` (built without libgit2), `dvtm` and `rsync`. Each is a port, not a flag —
libgit2 needs an HTTPS transport in a binary that links no OpenSSL, and `dvtm`
and `rsync` both start children, which a native program does through native
spawn or not at all. Until then the distribution's packages do the job,
translated.

**Two divergences are recorded as the shell's**: a pattern compiled at first
use is cached in the parse tree with nothing recording the options in force at
the time, so a re-launched child can compile it under different options than its
parent did; and `pipestatus` under a MULTIOS redirection was seen to report
`1 0` where zsh reports `0 0`. Both are written down in the 549 release notes,
and — contrary to what this chapter said until 557 — **neither has a test**.
The native zsh's fork-state test covers MULTIOS and `pipestatus` separately,
never the two together, and the simple MULTIOS pipelines tried for this revision
agree with the host's zsh. So the second one needs its reproducer found again
before it is either fixed or struck off.

**A tracer cannot see inside a native program.** gdb and strace can start one
since 556 — it reports its exec, and exec replaces it in place with its pid
kept — but its calls go through a dispatcher with no ptrace hooks and no guest
register file to report from. So `strace` shows the exec, the exit, and nothing
between; `strace -f` and gdb's `follow-fork-mode` never see a native shell's
children, because it starts them with native spawn rather than `clone`; and a
tracer that attaches after an untraced native exec finds the stand-in's wait,
not the program.

**And there is no native `sshd`** (Chapter 25), blocked on privilege-separation
forking — mitigated rather than fixed, because the crypto accelerator takes the
cipher out of the emulator and the cipher is what an ssh session is bound by.

## 41.6 FUSE, stated as absences — and one that stopped being one

`fs/fuse.c`'s header comment names three things not modeled: `readdirplus`,
which needs an attribute cache to be worth having; splice; and the
`fsopen()`-based mount API. `FUSE_INIT` offers the daemon no optional features
at all, so a daemon is never told AOK supports something it does not. Missing
*visibly*, which Chapter 40 explains is the whole difference between an
unfinished feature and a capability lie.

The missing cache has costs that are not absences, and `docs/TODO.md` measures
them: every operation walks from the root with one `LOOKUP` per component, so a
path five deep is six requests; a `stat` of a name that does not exist sent
**nine** requests against Linux 6.12's one; and a mapping and `read()` agree
only at sync points, where Linux's page cache keeps them coherent all the time.
One change — a real dentry and attribute cache honouring FUSE's timeouts — is
behind all of them, and the reference accounting it needs is already asserted
by the test.

The third absence is no longer visible. The new mount API arrived for systemd
and util-linux, generically, and it reaches FUSE: `fsopen("fuse")` succeeds,
`fsconfig` accepts `fd`, `rootmode`, `user_id` and `group_id`, and
`FSCONFIG_CMD_CREATE` makes the mount. Then `fsmount()` opens the new mount's
root directory to hand back a descriptor — and opening a FUSE directory asks the
daemon. A daemon mounts first and serves afterwards, so it is still waiting for
its own mount call and never answers. Measured for this chapter:
`tests/manual/fuse_basic.c` with its `mount(2)` swapped for that sequence hangs
in `fsmount` until its watchdog kills it, where the unmodified test passes.
Linux's `fsmount` returns an `O_PATH`-style descriptor for the mount, which asks
the daemon nothing. No user has reported it — every daemon tested here mounts
through `mount(2)` — but it is a hang where the header comment promises an
absence, and it is queued in `docs/TODO.md`.

## 41.7 Deferred on purpose: external display

This entry is the rarest kind, and worth holding up.

External display support ([#540](https://github.com/emkey1/ish-AOK/issues/540))
did ship once. It landed in July (`57380ba6`, `cc0b5b21`) and was reverted for
546 (`ad602c7c`) after testing on an M4 iPad with a real monitor, because it
fought how iPadOS already uses an external display: the app would not open
there, dragged there it took a portrait-iPad shape, a second window on the
built-in display pulled it back, and a terminal applet opened on it disturbed
the primary. One further commit — mirroring the Wayland display — is left on
`worktree-external-display-540`. It is **not merged**, it is a child of the
reverted pair so it cannot be merged alone, and the reason is recorded
verbatim:

> Deferred to a future release by the maintainer: *"the external display work is
> flawed"*. The commit is NOT merged and must not be swept into a release by
> accident. Left on its branch deliberately.

It was deferred again, by decision, for 556, and 557's plan does not reopen it.
The resume plan — revert the revert, then preferably stop claiming a screen an
interactive scene already occupies — is in `docs/external_display_plan.md`.

Most projects do one of two things with an implementation they have judged
inadequate: merge it because it mostly works, or delete it because it does not.
Keeping it, naming the judgement, and fencing it against accidental inclusion is
better than either — the work is recoverable, the verdict is legible, and
nothing is going to ship it by mistake.

## 41.8 The open reports, and one that was not a speed problem

The tracked issues are worth a glance because of what they are *made of*, and
because they turn over. Of the five bugs this section named at 556, four are
closed: the Wayland applet now sizes the desktop to its window and Qt
applications get a session bus (both 555), `gdb`'s `next`/`step` after a
breakpoint no longer dies with `SIGILL` on amd64, and `pikaur` builds packages
since 554. Only Buildroot's `make` dying at "checking for working sigaltstack"
is still open. What was open at the start of the 557 run (2026-09-28) was
mostly the app rather than the kernel, and most of it closed with 557: a
terminal's last row hidden under the keyboard toolbar after returning to the
foreground, window controls misplaced in iPadOS windowed mode, a copy that lost
the part of a selection scrolled off screen, and network throughput slower than
it should be, which its reporter found gone. Still open (2026-10-02) is a list
of programs that misbehave in the Wayland desktop (#620): its first two,
`btop` needing a flag to start and Synaptic freezing the display, were a missing
UTF-8 locale and a `pkexec` with no polkit session to ask, both fixed in 557,
and the list stays open for the programs after them. The newest report, filed
2026-10-02, is a crash from the Filesystems screen's Browse Files (#625).

This section also used to hold up a report as the model of a category:

> `yay -S pandoc-bin` dying with `context: signal: terminated` … is not a crash:
> yay's Go runtime sends itself `SIGTERM` when its context is cancelled, most
> likely its own timeout firing because emulated syscalls are slower than its
> budget assumes. **Not a re-test; a timeout question.**

— a bug report with no bug in it, a deadline calibrated for native hardware that
the emulator misses, with no fix short of being faster.

The category was right to name and wrong for this report. The reported failure
never reproduced (the issue was closed on that basis); what did, one run in
four, was Go's HTTP/2 client giving up on the AUR. And the measurement behind
*that* was not a slow machine. Fifteen TLS handshakes to the same host had a
guest median of 0.39 s against the host's 0.21 — unremarkable for emulation —
and a guest **maximum of 15.3 s** against the host's 1.15, past Go's 10-second
handshake timeout. Nothing in a handshake is compute-heavy enough to take
fifteen seconds when it usually takes a third of one. That is a wait not being
woken, which is a correctness question dressed as a speed one.

It is not settled. Re-measured for this chapter on 557 in the Mac CLI, 45 guest
handshakes on aarch64, in two batches, had medians of 0.28 and 0.43 s and a
maximum of 1.29 s: no tail. The original run was in an Arch Linux ARM root rather than Alpine, the
device has not been re-measured, and the throughput report closed for 557 was in
the same neighbourhood.

So the lesson got sharper rather than going away. "Make it faster" is not a
triage outcome — and neither is "it is just slow" until somebody has looked at
the *tail*, because a median describes the emulator and a tail describes a
bug.

## 41.9 New in 557: the GPU's edges

557 put a GPU under the guest ([#484](https://github.com/emkey1/ish-AOK/issues/484)):
`/dev/dri/renderD128` (`fs/virtgpu.c`) speaks the virtio-gpu interface to stock
Mesa and replays its Vulkan onto Metal through Venus and MoltenVK, in-process;
the Wayland compositor renders on it by default; and `wl-present` hands its
frames to the app directly instead of through VNC. The gate it had to pass is
in `docs/roadmap.md`, with numbers, and most of its edges are written down in
the same place:

- **Diagnosed, not built.** No guest-memory blobs and no DRM sync objects —
  Mesa's Venus path needs neither, and simulates the latter. One lock
  serialises every call into the renderer, as virglrenderer's own server does.
- **Not carried by a checkpoint.** What stands behind an open render node —
  Vulkan contexts, blobs, the presenter — lives in the app and cannot be
  imaged; reopened, the node would be a blank device under a compositor that
  believes it has a GPU. So a save refuses in words ("a Wayland desktop or GPU
  program is running … close it, then suspend"), and Save Anyway, or the
  automatic save as the app goes to the background, leaves out every job
  holding the GPU or anything else unsaveable — the whole desktop, from the
  shell that started it down — and keeps the rest. To a kept parent a left-out
  child is a process killed by `SIGKILL`, which is what it is after a resume.
- **Bounded by the distributions.** Alpine builds no virgl GL driver at all, so
  GL goes through zink on top of Venus — and zink here is GL 2.1 and GLES 2.0,
  so GL programs stay on llvmpipe unless told otherwise, and only the
  compositor is pointed at the GPU by default. Alpine's i386 and riscv64
  builds have no Venus driver either, so those guests stay on software
  rendering.

## 41.10 Structural ceilings

Some limits are arithmetic.

**The engine is dispatch-bound at ~6.8 ns** (Chapter 38). No amount of work on
gadget bodies removes the dispatch; only reducing the *count* helps, which is
what fusion, HLE and native programs each do in their own way.

**A guest address space for native programs is closed**, measured: it would not
produce `fork` anyway, and the memory lock is 33–39x on tight access
(Chapter 27).

**The 10,000-thread benchmark needs about 5.4 GB** at ~564 KB of peak RSS per
guest thread, and therefore cannot pass on a 2 GB device regardless of any
emulator change (Chapter 38). Knowing that is what stops it being treated as a
regression.

**There is no instruction-level oracle for the arm64 and riscv64 guests**
(Chapter 9). Ptraceomatic needs real x86 silicon; unicornomatic needs Unicorn's
x86 support; the conductor's oracle cells are Rosetta and an x86 Linux VM. The
newest and fastest guests are the least differentially verified, and the
lockstep harness that could fix that on an Apple silicon host has not been
built.

**And the GPU stops when the app is not in front.** iOS refuses GPU command
buffers from an app in the background, and the refusal is not advisory:
MoltenVK marked the whole device lost on the first one, every fence a guest was
waiting on stayed unsignalled, and even `vulkaninfo` hung. 557 holds every
submission at a gate that closes when the app's last window goes to the
background and opens when one returns, so guest GPU work *pauses* rather than
breaking. It cannot be made to continue. A render that has to finish while the
user is in another app is not a job this GPU can take.

## 41.11 Why the list exists

Every entry here shares one property: **it is written down somewhere a person
would find it**, usually in `docs/TODO.md`, usually with a measurement, often
with the designs that were rejected and why.

The 552 release added a second such file, a per-release list of work that must
be done or explicitly decided before the next build is tagged:
`docs/build_556_musts.md` carried what was deferred out of 555, with the
diagnosis already done so nobody has to re-derive it. Each entry says what is
established, what the next step is, and how to prove it afterwards. During the
557 cycle the follow-ups gathered instead in one queue at the top of
`docs/TODO.md`, "Queued for a future release": every session that finds
something adds a bullet with its evidence, marks the bullet before starting on
it, and deletes it when the work lands, since the commit is then the record.
This chapter's own revision went through that queue.

The habit paid for itself immediately. `docs/historical/build_553_musts.md`'s entry on the
amd64 locked-instruction path is what got that path opened up at all — and the
first thing the work found was that the entry's own diagnosis was wrong in the
optimistic direction, describing as a performance gap something that was losing
guest data. A written-down gap is not just a reminder; it is a claim someone can
go and check. That file's header now says what it got wrong, and its
successor's *Closed in 553* section says how, which is the more useful half.

That turns a gap into a decision, and sometimes into a fix. `PROT_EXEC` was
never "we never got to NX" — it was a two-row table against Linux 6.12, a
severity grade, two candidate designs and a reason, and it is closed. The
external display is not an abandoned branch — it is a maintainer's judgement
with a fence around it. The `F_GETFL` lie was not a mystery — it was two correct
decisions and a named seam with a scoped next step, and the step was taken. And this chapter's own
record needed the same treatment: at 556 it still said GNU `as` ran on the
interpreter, that a zsh divergence had a test, and that a yay failure was a
timeout — and none of the three survived being checked.

The alternative is not a shorter list. It is the same list, undiscovered, found
one user report at a time by people who have no way to know whether they are the
first.

---

*Anchors:* [docs/TODO.md](../../docs/TODO.md) ("Queued for a future release",
"Diagnosed, not fixed", "Deferred on purpose", "Native program candidates",
"Reported issues"), [docs/roadmap.md](../../docs/roadmap.md) ("557 -- reach"),
[docs/build_556_musts.md](../../docs/build_556_musts.md),
[docs/external_display_plan.md](../../docs/external_display_plan.md),
[kernel/fork.c](../../kernel/fork.c) (`sys_unshare`, `sys_setns`),
[kernel/getset.c](../../kernel/getset.c) (`current_capable`),
[emu/memory.h](../../emu/memory.h) (`P_EXEC`), [fs/real.c](../../fs/real.c)
(`realfs_getflags`, `realfs_read`, `realfs_write`),
[emu/amd64_interp.c](../../emu/amd64_interp.c), [emu/tlb.c](../../emu/tlb.c)
(`x86_atomic_rmw`), [jit/jit.c](../../jit/jit.c) (where the `as` bypass was),
[jit/gadgets-aarch64/math.S](../../jit/gadgets-aarch64/math.S),
[fs/fuse.c](../../fs/fuse.c), [fs/mount.c](../../fs/mount.c)
(`sys_fsmount_guest`), [fs/virtgpu.c](../../fs/virtgpu.c),
[tools/native-applet-audit.py](../../tools/native-applet-audit.py),
[tools/check-native-libc.py](../../tools/check-native-libc.py).

*Story:* a pipe that reports `O_NONBLOCK` the guest never set — because
`F_GETFL` asks the host descriptor, and a blocking read or write permanently
makes that descriptor non-blocking on purpose, to prevent a `SIGKILL`-proof hang that was
real.
