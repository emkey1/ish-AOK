# 40. What this project believes

Thirty-nine chapters have been describing a system. This one describes the
beliefs the system was built out of — nine rules, each stated with the failure
that produced it, because a rule without its incident is just an opinion.

They are here at the end rather than the beginning because none of them was
adopted in advance. Every one was extracted from something going wrong, and they
are worth having in one place precisely because they are *transferable*: none
of them is about emulators.

## 40.1 Capability lies are load-bearing

**The rule:** before reporting a feature bit absent, ask whether any real Linux
reports it that way. If none does, implementing the feature is usually cheaper
than the consequences of denying it.

**The incident:** AOK reported `DCZID_EL0.DZP = 1` — "DC ZVA prohibited" — so
that the arm64 JIT would never have to emulate the instruction, and made the
instruction itself `SIGILL`.

Linux sets `SCTLR_EL1.DZE`, so `DZP` reads 0 at EL0 on **every** aarch64 Linux
system. The value AOK reported is one no real guest has ever seen.

HotSpot reads it, leaves its zero-block length at 0, and generates the zeroing
stub anyway — because `UseBlockZeroing` is a separate flag that defaults on. The
stub emits `and Xd, Xn, #(0-1)`, a mask of all ones, which the AArch64
logical-immediate encoding cannot represent. **The JVM died inside its own
assembler before running any Java code.**

The gadget that fixed it was about fifteen lines of assembly.

**Why "advertise it off" feels safe and is not:** the off state is only safe if
software has been tested against it. For a feature every real kernel enables,
nobody has tested the off path — so the failure surfaces somewhere
unrecognizable. Not "DC ZVA unsupported", but an assembler assertion.

And the symmetric warning, because the fix has its own blast radius: turning
such a bit **on** switches libc onto code paths that were dead in every guest
until then.

## 40.2 Check the oracle before claiming a defect

**The rule:** empty output is not death. Before calling anything a defect,
establish what real Linux does with the same input.

**The incident** is one the project's own notes record against their author,
which is what makes it useful. A recursion stack guard was reported as failing
in re-launched children, written into two commit messages as a known gap, and
reported to the user as a still-reachable app-killer.

It was fixed. Both mistakes came from reading empty output as failure:

- `zsh -f -c 'r(){ r; }; print $(r)'` printed nothing — which is exactly what a
  *correctly refusing* child produces, because the diagnostic goes to **stderr**
  and `$( )` captures **stdout**.
- The second test also printed nothing — and so does `/bin/zsh`, byte for byte.
  Real zsh abandons the rest of a `-c` script after a nested-function error too.
  **The oracle had the same behaviour and nobody checked.**

Chapter 9 is the machinery for this. The rule is the discipline that makes the
machinery worth having.

## 40.3 A table entry is not a reachable syscall

**The rule:** the code you can find is not necessarily the code that runs.

A syscall table entry may exist only to pass a NULL check, while the real
dispatch happens natively before the table is consulted (Chapter 11). A JIT
gadget may never be emitted because a fusion bit changed. A filesystem operation
may be intercepted by `may_block` handling (Chapter 20).

**The remedy is instrumental, not analytical:** the strace log names the path
actually taken. Reading the table tells you what somebody intended.

## 40.4 Not faulting is not executing

**The rule:** an emulator probe needs an observable witness.

A test that runs an instruction group and checks only that nothing crashed
passes when the instruction is implemented as a no-op. It also passes when the
instruction is skipped, when the block was never compiled, and when the guest
took a different branch.

Demand a register value, a memory effect, a signal — something the correct
implementation produces and the absent one cannot.

## 40.5 Read what the consumer reads

**The rule:** when a tool misbehaves, find out which files it actually opens
before improving the ones you assume it reads.

**The incident:** `btop`'s empty panels (Chapter 18). The recorded diagnosis —
that `/proc/diskstats` and `/proc/mounts` disagreed about device names — was
correct, was fixed properly, and was verified across all four guest
architectures. The panels stayed empty.

```sh
strings /usr/bin/btop | grep -E "^/(proc|sys|etc)/"
```

`/proc/net/dev` appears nowhere in the binary. Neither does `/proc/diskstats`.
The invariant that had been verified was never what btop reads.

**The generalized form:** the invariant was verified and the *outcome* was not.
Chapter 29's three terminal bugs are the same shape — correct in the source and
absent from the artifact, correct in the model and unverified on the screen,
correct in each pipeline and wrong in their union.

## 40.6 Verify what the user runs

**The rule:** test the artifact they will install, in the shape they will
install it.

**The incident:** three features declared working on a `ninja` build and handed
over in an Xcode build that did not have them (Chapter 34) — a missing build
knob, a link flag that reached only meson's own link line, and then the knob
added and defaulted wrongly.

This is Section 40.5 applied one level up, and it is why the project's own rule
is stated as *Xcode is the only build*.

## 40.7 Root hides an entire class of bug

**The rule:** permission ordering is not testable as root.

**The incident:** `mkdir("/tmp")` returned `EACCES` where Linux returns
`EEXIST`, because AOK asked for write permission on the parent before looking up
the final component and Linux does it the other way round. Since `mkdir -p`
calls `mkdir` on every component and treats `EEXIST` as success, **`mkdir -p
/tmp/anything` failed outright for every unprivileged user**.

It was invisible in every CLI session and every test, because AOK's
command-line build runs as uid 0 and a root process skips the check entirely. It
surfaced only because somebody on a real device was running as uid 1000.

Chapter 15's `execve` audit found five more of the same class in one sweep.

## 40.8 A syscall usually has a second copy

**The rule:** grep for the name, not the number, in `kernel/` and `fs/`
together, and read every hit before editing any of them.

**The incidents**, plural, because this one recurs:

- The `*at()` dirfd helper exists in both `kernel/fs.c` and `fs/stat.c`. The
  first fix reached one of them; `fstatat` alone stayed broken, and only the
  regression test noticed. `statx` — what modern glibc actually calls — lives in
  the file that was missed. They are still not unified.
- The group-stop wait existed in `handle_interrupt` and again in
  `native_checkpoint`, and the second copy never tracked the first's ptrace
  fixes. That one was fixed by *deleting* the copy (Chapter 12), which is the
  only permanent form of the fix.
- `errno_map()` means a handler can return `_EINTR` without the token appearing
  anywhere near it, so searching for the constant does not find every path that
  produces it.

## 40.9 Blocked is not contended

**The rule:** N threads waiting on a lock look identical whether the lock is
held or free. Ask *who holds it*.

**The incident:** a confident, specific, written-down diagnosis naming a lock
cycle between the memory read lock and the JIT jetsam lock, with two candidate
fixes costed against it. Both were wrong, because there was no cycle — nobody
held the jetsam write lock at all. The blocked reader was asleep on a *free*
lock, a Darwin lost-wakeup, triggered by the very signal pokes sent to evict
readers.

**And the meta-rule it produced**, which may be the most valuable line in this
chapter: **re-derive a recorded diagnosis from fresh evidence before acting on
it** — however confident, however specific, however much it looks like somebody
already did the work.

## 40.10 Comments make checkable claims, and some of them are false

**The rule:** verify a cited guard exists before trusting it.

**The incident:** `emu/cpuid.h` cited **two non-existent files** in a single
header — a test that had never been in the tree or in git history, and a header
that did not exist. The comment claimed a guard against advertising
unimplemented CPUID bits. The guard had never been written, and behind it sat a
real shipping bug.

This one is uncomfortable to include, because this book has quoted this
codebase's comments on almost every page and holds them up as a model. Both
things are true: the comments are unusually detailed and unusually confident,
*and* that confidence is itself a claim requiring verification. A comment that
says "covered by `tests/manual/foo.c`" is a testable assertion, and `ls` tests
it.

## 40.11 Finish, and publish the failures

**The rule:** a result is not finished until the negative results are written
down where the next person will be standing.

Chapter 39 counted them: two of six optimizations produced a negative result
worth keeping, and both are recorded *in the source*, with numbers, beside the
code somebody would have to change to re-propose them. Chapter 38's biggest win
is published with its own +0.5% regression, the mechanism, and the fix that has
not been done. Chapter 13's `PROT_EXEC` gap is measured, graded, and carries
both rejected designs.

The alternative is not a tidier record. It is the same investigation being run
again in two years by somebody who has no way to know it was already done.

## 40.12 The one underneath all of them

Read the nine back and they are the same rule in different clothes.

*The oracle, not your expectation. The tool's actual file, not the one you
improved. The artifact the user installs, not the one you built. The path the
strace log names, not the table entry you found. The witness, not the absence of
a crash. The lock's holder, not the waiters. The file the comment cites, not the
comment. Fresh evidence, not the recorded diagnosis.*

**Check the thing itself.**

Which sounds too obvious to write down, until you count how many of the
incidents in this book were somebody competent, careful, and in possession of a
plausible model, checking the model instead.

---

*Anchors:* [docs/TODO.md](../../docs/TODO.md) (most of these incidents in full),
[emu/cpuid.h](../../emu/cpuid.h), [kernel/fs.c](../../kernel/fs.c),
[fs/stat.c](../../fs/stat.c), [kernel/signal.c](../../kernel/signal.c)
(`group_stop_wait`), [fs/proc/root.c](../../fs/proc/root.c),
[jit/gadgets-aarch64/gadgets.h](../../jit/gadgets-aarch64/gadgets.h),
[docs/perf_benchmarks_2026_08.md](../../docs/perf_benchmarks_2026_08.md).
