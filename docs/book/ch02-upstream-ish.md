# 2. Upstream iSH, 2017–2023

Chapter 1 described a machine that should not exist. This chapter is about the
six years in which somebody built it, before this fork had written a line.

The Foreword names the debt and lists the mechanisms. This chapter is the
narrative — what was built, in what order, and what each step made possible —
because the *order* turns out to be the interesting part. Almost nothing in
iSH's history was built because it was next on a plan. It was built because the
previous thing had made it reachable.

## 2.1 Three weeks in May

`git log --reverse` begins on **4 May 2017**, with an initial commit, a meson
build file, and a `.gitignore`. Four days later, "Everything to get Hello World
working".

That phrase understates it. Getting a statically linked Linux `hello` to print
requires an x86 decoder, enough instruction semantics to reach the first
syscall, a guest address space, a stack laid out the way a Linux process expects
with its argument vector and auxiliary vector in place, an ELF loader, and
`write` reaching a real file descriptor. The first week of this project produced
a miniature of everything Parts II and III of this book describe.

Then, on **25 May 2017** — three weeks in — comes the commit that shaped
everything after it:

> Ptrace-O-Matic, dozens of opcodes, stack init, VDSO

**Ptrace-O-Matic** (Chapter 9) runs the same program under a real x86 kernel via
`ptrace` and under the emulator simultaneously, single-stepping both and
comparing register state after every instruction.

Consider what it means to build that in week three. The emulator could barely
run anything; there was no shortage of obvious next features; and instead the
effort went into a harness whose only output is the answer to "are these two the
same". That is a statement about what the project was going to be: **correctness
established by comparison against reality, not by reading the manual harder.**

Every oracle in Chapter 9 descends from that decision, and so does the habit —
visible throughout this book — of treating "I believe this is right" as a
hypothesis rather than a conclusion.

## 2.2 Becoming a kernel

Through late 2017 the commits stop being about instructions and start being
about *systems*, and each one unlocks a class of software.

**October 2017 — `fs/fake.c`.** Contents as host files, metadata in SQLite.
Chapter 17 is about what that means; what it meant *then* is that a guest could
have a root filesystem with owners and modes, which is the difference between
running a binary and running a distribution.

**December 2017 — thread-safe reference counting.** "Use thread-safe reference
counting on fds", "Make the data refcount thread-safe", "Duplicate fds on fork
and close them on exit". Unglamorous, and the precondition for everything after.

**January–February 2018 — threads.** "First pass at implementing thread groups",
then `CLONE_THREAD`, then "Enable creating threads", with the flag-validation
rules arriving alongside — `CLONE_VM` required for `CLONE_SIGHAND`,
`CLONE_SIGHAND` for `CLONE_THREAD`. Chapter 10's process model dates from here.

**January 2018 — the fakefs schema redesign**, "to support hardlinks", plus
"Implement fakefs rebuilding". Six months after the original, the design changed
to survive what real package managers actually do.

And in the same period, the terminal: `app/Terminal.m` in October 2017,
`app/TerminalView.m` in November — the decision to run hterm in a web view
(Chapter 29), made early and never revisited.

## 2.3 The TestFlight years

`docs/CHANGELOG.md` preserves the user-facing release notes from builds 22
through 48, and they are the best available record of what actually broke for
people. Read in order, they are a compressed history of what "being Linux"
demands:

- Build 42: **zsh**. Background jobs — Ctrl-Z, `bg`, `fg`. An option to change
  the launch command. `top` runs, "though it's useless since CPU usage is always
  displayed as 0%".
- Build 45: **pseudoterminals** — "in other words, you can now ssh into your
  phone".
- Build 46: file locking. "Forking and exiting is now 10x faster". `RLIM_INFINITY`
  on `RLIMIT_NOFILE` being interpreted as −1. "Very serious problems caused by
  renaming directories."
- Build 48: files opened from the Files app having random numbers as names,
  "preventing them from being syntax highlighted in some apps and opening at all
  in other apps".

Two things stand out. The bugs are overwhelmingly *fidelity* bugs — a flag
misinterpreted, a rename mishandled, a name generated wrongly — which is the
claim Chapter 1 opened this book with, visible in the first two years of the
project's life. And the honesty is already there: "though it's useless since CPU
usage is always displayed as 0%" is the same voice that later writes "a stale
table costs speed, never correctness".

## 2.4 The JIT

On **3 May 2018**, almost exactly a year after the first commit:

> Foundations of jit, no actual compiling yet

`jit/gen.c` and `jit/jit.c` follow on 14 June; the aarch64 gadget files and
`jit_enter` on 17 August. Chapter 6 is about what that architecture is and why
it works on a platform that forbids JITs — and about `gen()`, the twelve-line
function written on 26 May 2018 that has not needed to change since.

What is worth adding here is the *timing*. The JIT arrives a year in, after the
kernel is real enough to run a distribution. That is the right order and not the
obvious one: an emulator project that optimizes first ends up with a fast
interpreter for programs nobody can run.

## 2.5 The middle years

2018 was the project's largest year by volume — 838 commits from its author
alone — and 2019 not far behind at 515. Then 381 in 2020, 125 in 2021, 85 in
2022, and 18 in 2023, ending on 17 November.

The work in that tail is not decline so much as consolidation, and a lot of it
is the unglamorous half of shipping an app: keyboard notification handling, bar
sizing, launch screen colours, `O_EXCL` in realfs, "Hide items not found in
database instead of crashing", CI upgrades, fastlane. Interleaved with real
kernel work — `SIGEV_THREAD_ID`, `rt_sigtimedwait` lock paths, file timestamps
across import and export, "Enable asynchronously interrupting the emulator",
"Port emulator poke to arm".

Other people were building the pieces this book describes as engine components.
Xiangyan Sun's return cache landed in September 2019 (Chapter 6). Jason Conway's
memory-ordering fix — still on the hot path of every guest instruction — came in
2022–2023. Ryan Hileman's futex timeouts and random devices, Viktor Oreshkin's
device-number table, Matthew Merrill's SSE2, nimelehin's NEON vector work and
`/proc/loadavg`, Zhuowei Zhang's x87 additions for ffmpeg, Saagar Jha's work
across app and kernel. The Foreword credits them individually; the point here is
that the middle years were when iSH stopped being one person's program.

## 2.6 The relicensing

Between June 2020 and October 2021, upstream ran an effort that is unusual
enough to record as history rather than as legal housekeeping: relicensing so
that contributions after a named commit are additionally under GPLv2, allowing
the project to link with GPLv2 work such as Linux and QEMU.

That required asking every past contributor individually, and
[LICENSE.md](../../LICENSE.md) carries the roll — twenty-five people who each
wrote a commit saying yes, over sixteen months.

Alongside it sits `LICENSE.IOS`, the commitment not to enforce the GPL/App Store
conflict against **derived apps** — which is, as the Foreword says, why
iSH-AOK is distributable at all.

## 2.7 What upstream had built by the end

By late 2023 the project had: an i386 emulator with a threaded-code JIT; a
kernel with processes, threads, signals, job control, ptrace, futexes, memory
management and a filesystem layer; fakefs; procfs, tmpfs and devfs; sockets with
the iOS suspension workaround; a terminal built on hterm; a File Provider
extension; a rootfs importer; and a differential test harness older than most of
the code it checks.

It ran Alpine. People used it.

What it did not have is the list Chapter 3 is about — and the useful thing to
notice, before that chapter starts, is how much of what follows was *reachable*
from this foundation rather than requiring it to be replaced. Four guest
architectures needed the ABI split, but not a new memory model. Native programs
needed an `execve` branch, not a new process model. The accelerators needed a
private syscall number.

The fork's work is large. It is almost all *addition*, and that is a property of
what it was added to.

---

*Anchors:* `git log --reverse`, [docs/CHANGELOG.md](../../docs/CHANGELOG.md),
[tools/ptraceomatic.c](../../tools/ptraceomatic.c), [fs/fake.c](../../fs/fake.c),
[jit/gen.c](../../jit/gen.c), [app/Terminal.m](../../app/Terminal.m),
[LICENSE.md](../../LICENSE.md), [LICENSE.IOS](../../LICENSE.IOS),
[ch00-foreword.md](ch00-foreword.md).
