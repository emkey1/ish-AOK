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

No PID namespaces, no mount namespaces, no network, IPC, user or cgroup
namespaces. `CLONE_NEWUTS` is the single exception, because a UTS namespace is a
hostname in a refcounted struct.

So nothing container-shaped runs. No Docker, no `unshare`, no rootless podman,
no per-service filesystem views. There is one process table, one filesystem
tree, and one network, visible identically from every root and every chroot.

This is not a missing feature with a ticket. The absence runs through the design
— Chapter 10's task model, Chapter 16's global mount table, Chapter 21's
`/AOK`. And Chapter 21 is also where it reads as an *advantage*: one true
`/proc` from anywhere is what makes `ktop`'s cross-architecture process list
possible, and on a single-user device the isolation being traded away was
protecting nobody.

## 41.2 Diagnosed: `PROT_EXEC` is never enforced

Every guest `.data` and `.bss` page is executable, and any guest JIT's own W^X
discipline is decorative. Chapter 13 has it in full, and it is the model entry
for how a gap should be recorded: measured against Linux 6.12 in a two-row
table, **graded** as a mitigation gap rather than a hole (exploiting it needs a
separate memory-corruption bug in guest software), and carrying both candidate
designs with the specific reason each was or was not taken.

The verdict — "a contained project rather than a patch, and it touches the one
path where a mistake stops every guest from running" — is a decision, not a
deferral. The difference is that somebody can act on it.

## 41.3 Diagnosed: `fcntl(F_GETFL)` lies about a pipe

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
`realfs_read` permanently forces that host descriptor non-blocking the first
time the guest does a *blocking* read on it, and deliberately never restores it.

And that second decision is correct. Restoring it races a sibling task into an
uninterruptible, `SIGKILL`-proof host `read` — a real pipe-herd hang that was
fixed by exactly this non-restoration.

So the host flag is an implementation detail that must not be visible, and it
is. Before the first read `F_GETFL` says 0; after it says `O_NONBLOCK`, with the
guest having done nothing. The kernel's own `fd->flags` — which is what actually
governs guest blocking semantics — still says blocking, and the two disagree.

The recorded next step is precise, including its own scope warning:

> `realfs_getflags` should report the guest-visible flags from `fd->flags` for
> the bits the guest owns (`O_APPEND`, `O_NONBLOCK`) and take only the access
> mode and the rest from the host. Small, but it needs its own test: the
> get-modify-set idiom is everywhere, and silently turning a pipe non-blocking
> under a program that never asked is the kind of thing that surfaces far from
> here. Worth checking whether sockets and ttys answer `F_GETFL` the same way
> before fixing just the one path.

Two correct decisions, one wrong seam. That is the characteristic shape of a
bug in a system this size, and it is why Chapter 40's rules are about *checking*
rather than about care.

## 41.4 The interpreters are legacy, not dead

The `engine` build option offers exactly one value. New work targets the JIT.
And yet:

- `emu/amd64_interp.c` is still the **largest single file in the tree** at
  16,675 lines.
- It is still what runs on non-aarch64 hosts, because the amd64 JIT is validated
  only on the iOS target (Chapter 7).
- It is still what GNU `as` executes on, behind a containment workaround for
  crashes that were never root-caused — with a probe harness in the tree waiting
  for somebody to re-run it.
- It is still where AVX semantics execute for amd64 (Chapter 5).

`emu/arm64_interp.c` survives for a different reason: as a bisection escape
hatch behind `ISH_ARM64_FORCE_INTERP=1`, with a comment that is candid about
expecting it to crash on anything nontrivial.

"Legacy" here means "not where new work goes", not "vestigial". A reader who
assumes otherwise will misread both the amd64 story and the AVX one.

## 41.5 Native programs: the open list

Part V's mechanism is finished; its coverage is not.

**The `argv` ownership class** (Chapter 25) is fixed where it was found and not
swept. `find` was fixed; `du`, `stat`, `rm` and `wget` were audited and are
mostly unreachable for incidental reasons — a plain `du` in the guest hits the
distribution's coreutils, since `/AOK/native` holds only the multicall entries,
and `wget`'s fetch path is compiled out of every build so only its argument
handling is reachable at all. "Unreachable today" is a weaker guarantee than
"fixed", and the audit tool exists because the hand-written exclusion list was
not good enough: `env` was missed, and since one test harness runs
`env ... bash ...`, installing the symlinks took its suite from 217 passing to
zero.

**Unrouted host symbols** remain, and are enumerated on demand — Chapter 23's
gate has a `--report` mode whose third list is exactly the outstanding work.

**Process substitution** — `<(...)` and `>(...)` — fails on Alpine, and that is
a property of the *rootfs* rather than of the shell: it needs `/dev/fd`, which
Alpine does not ship, so it fails identically under the emulated `/bin/bash`
there and works under both shells on Devuan.

**Two divergences are genuinely the shell's**: a pattern compiled at first use
is cached in the parse tree with nothing recording the options in force at the
time, so a re-launched child can compile it under different options than its
parent did; and `pipestatus` under a MULTIOS redirection reports `1 0` where zsh
reports `0 0`. Both have tests pinning them.

**And there is no native `sshd`** (Chapter 25), blocked on privilege-separation
forking — mitigated rather than fixed, because the crypto accelerator takes the
cipher out of the emulator and the cipher is what an ssh session is bound by.

## 41.6 FUSE, stated as absences

No `mmap`, so no executing a program stored on a FUSE mount — binaries and
AppImages have to be copied off first. No `FUSE_INTERRUPT`, so a daemon is never
told a request was abandoned. No `FORGET`, which is the deliberate trade behind
not caching nodeids (Chapter 20). No `readdirplus`, no splice, no fd-passing
mount API.

All of them are missing *visibly*, which Chapter 40 explains is the whole
difference between an unfinished feature and a capability lie.

## 41.7 Deferred on purpose: external display

This entry is the rarest kind, and worth holding up.

Work exists for mirroring the Wayland display to an external display — one
commit, on a branch. It is **not merged**, and the reason is recorded verbatim:

> Deferred to a future release by the maintainer: *"the external display work is
> flawed"*. The commit is NOT merged and must not be swept into a release by
> accident. Left on its branch deliberately.

Most projects do one of two things with an implementation they have judged
inadequate: merge it because it mostly works, or delete it because it does not.
Keeping it, naming the judgement, and fencing it against accidental inclusion is
better than either — the work is recoverable, the verdict is legible, and
nothing is going to ship it by mistake.

## 41.8 The open reports, and one that is a speed problem

The tracked issues are worth a glance because of what they are *made of*: the
Wayland applet not resizing, Qt applications unable to reach the session bus,
`gdb`'s `next`/`step` crashing with `SIGILL` on amd64 after a breakpoint,
Buildroot's `make` dying at "checking for working sigaltstack", `pikaur` blocked
on `systemd-run`.

And one of a category that deserves naming:

> `yay -S pandoc-bin` dying with `context: signal: terminated` … is not a crash:
> yay's Go runtime sends itself `SIGTERM` when its context is cancelled, most
> likely its own timeout firing because emulated syscalls are slower than its
> budget assumes. **Not a re-test; a timeout question.**

That is a bug report with no bug in it. The software is working; it has a
deadline calibrated for native hardware, and the emulator misses it. There is no
fix short of being faster, and there is no honest way to close it either.

Any emulator accumulates these, and they are worth distinguishing from
correctness failures early — because the investigation is completely different,
and because "make it faster" is not a triage outcome.

## 41.9 Structural ceilings

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

**And there is no instruction-level oracle for the arm64 and riscv64 guests**
(Chapter 9). Ptraceomatic needs real x86 silicon; unicornomatic needs Unicorn's
x86 support; the conductor's oracle cells are Rosetta and an x86 Linux VM. The
newest and fastest guests are the least differentially verified, and the
lockstep harness that could fix that on an Apple silicon host has not been
built.

## 41.10 Why the list exists

Every entry here shares one property: **it is written down somewhere a person
would find it**, usually in `docs/TODO.md`, usually with a measurement, often
with the designs that were rejected and why.

That turns a gap into a decision. `PROT_EXEC` is not "we never got to NX" — it
is a two-row table against Linux 6.12, a severity grade, two candidate designs
and a reason. The external display is not an abandoned branch — it is a
maintainer's judgement with a fence around it. The `F_GETFL` lie is not a
mystery — it is two correct decisions and a named seam with a scoped next step.

The alternative is not a shorter list. It is the same list, undiscovered, found
one user report at a time by people who have no way to know whether they are the
first.

---

*Anchors:* [docs/TODO.md](../../docs/TODO.md) ("Diagnosed, not fixed",
"Deferred on purpose", "Native program candidates", "Reported issues"),
[emu/memory.h](../../emu/memory.h) (`P_EXEC`), [fs/real.c](../../fs/real.c)
(`realfs_getflags`, `realfs_read`), [emu/amd64_interp.c](../../emu/amd64_interp.c),
[jit/jit.c](../../jit/jit.c) (the `as` bypass), [fs/fuse.c](../../fs/fuse.c),
[tools/native-applet-audit.py](../../tools/native-applet-audit.py),
[tools/check-native-libc.py](../../tools/check-native-libc.py).

*Story:* a pipe that reports `O_NONBLOCK` the guest never set — because
`F_GETFL` asks the host descriptor, and a blocking read permanently makes that
descriptor non-blocking on purpose, to prevent a `SIGKILL`-proof hang that was
real.
