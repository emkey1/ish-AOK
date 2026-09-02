# Release Notes Since `builds/iSH-AOK_552`

22 commits. A short cycle with two fixes in it that between them
account for a lot of wasted CPU: the locked instructions an x86 guest uses to
build every lock it has, and a blocking `poll()` that burned a whole core doing
nothing.

## Highlights

**An idle `poll()` on a socket burned a whole core.** Any process sitting in a
blocking `poll()` or `select()` on a quiet socket spun a host CPU at ~97% for
the entire duration of the wait. On a device that meant an idle `chronyd`, an
idle `rsyslogd` and each `sshd-session` pinning a core apiece — reported as
*sleeping*, issuing no syscalls at all, because the spin was inside the emulator
rather than in guest code.

The cause: Darwin's kqueue write filter is registered for hangup detection even
when the guest only wants to read, it is level-triggered, and a quiet socket is
always writable — so the wait woke immediately, found nothing the guest had
asked for, and slept again, forever. There was already a guard meant to prevent
exactly this; Darwin does not honour it on these objects. Measured before and
after on the same binary: 95–97% CPU down to 0%, with readiness and hangup still
waking the poll in 0.20s.

This one is worth a battery-life note. Nothing about it was visible to any
existing test, because the answer `poll()` returned was always correct — only
what it cost to produce that answer was wrong.

**Locked instructions on an amd64 guest were not atomic.** Not "atomic against
the kernel but not the host", which is what the 552 cycle left written down as
a known gap — genuinely not atomic, against other guest threads, in the ordinary
`lock addl %reg, (mem)` case. Four guest threads incrementing a shared counter
that way lost 3876 of 200000 increments. The same audit found `xchg reg, [mem]`
— which is atomic on x86 whether or not you write the prefix, and is the store
half of every spinlock — **livelocking**: two guest threads on a one-word
spinlock managed 78 acquisitions and then one spun two hundred million times
without ever seeing the lock released.

Every locked form the amd64 guest implements now runs as a real host atomic when
the operand is naturally aligned, which is every case that matters. Misaligned
operands — legal on x86, and able to straddle a page, so no single host atomic
covers them — keep the old global-lock path. The i386 guest already compiled its
atomics into host-atomic gadgets; what it got this cycle is a repair to two of
them and one instruction it could not decode at all.

What this changes in practice: multithreaded amd64 programs stop corrupting
their own data structures, and stop hanging on their own spinlocks. Because the
global software lock is gone from the amd64 path, they also stop serialising the
whole emulator on one mutex every time any thread takes any lock.

## Fixes worth naming

**`lock <alu> [mem], reg` lost updates (amd64).** Of the many eligibility
predicates in the amd64 JIT front end, exactly one accepted a `LOCK` prefix —
necessarily, since the helper it emits is the only implementation of that form
the JIT has. It then dropped the prefix on the floor and did a plain
read/compute/write.

**`xchg reg, [mem]` livelocked (amd64).** It was a read/write pair under the
global lock, which is not merely weaker than a host atomic — under contention it
starved. A spinlock built on it could stop making progress entirely.

**`lock inc`, `lock dec` and `lock <alu> [mem], imm` lost updates (amd64).**
Each of these has two implementations — one reached through the JIT bridge, one
in the main interpreter's own switch — and only the bridge copies ever took the
lock at all. `lock neg`/`lock not` and `lock bts`/`btr`/`btc` took it in
neither copy.

**`lock adc` and `lock sbb` used the wrong carry (i386).** The gadget read the
incoming carry flag *inside* its compare-exchange retry loop, but the arithmetic
overwrites that same flag with the carry it produces — so a contended retry
re-ran the operation using its own result as its input. Roughly 841 of 80000
`stc; lock sbbl` iterations took the wrong carry with four threads on one word.
Only ever wrong under contention, which is why every existing atomics test
passed over it.

**`lock xchg` killed the guest with SIGILL (i386).** It was simply missing from
the i386 `LOCK` opcode table, so an explicit prefix — redundant but legal, and
something hand-written assembly does emit — decoded to *undefined*.

**`poll()` reported a closed socket as hung up but not readable.** Unchanged in
this build and noted for the next one — Linux reports `POLLIN|POLLHUP`, AOK
reports `POLLHUP` alone, so a program that waits for `POLLIN` before reading
will not read the EOF it is being told about.

**`ru_maxrss` reported 2.7 TB, and every child inherited it.** Found running the
regression suite on a device. `getrusage` folds a page count into a high-water
mark — a latch — and takes that count from a walk that is deliberately lock-free,
which is fine for the `/proc` reader it was written for and not fine for
something that remembers. One torn read became the permanent answer. Then
`fork` copied it onward: a child started life owning its parent's all-time peak,
where Linux gives it a fresh one. So a single bad sample poisoned every process
in a shell's subtree for as long as that shell lived, while a new login looked
healthy — which is why it presented as device-only and would not reproduce.

The latch now refuses a sample larger than the address space, and a child no
longer inherits its parent's peak. The unsafe walk itself is unchanged and is
written up for the next build.

**`FUTEX_WAKE_OP` no longer takes the global atomic lock.** 552 made it do so,
correctly, because the host compare-exchange it already performed had nothing on
the amd64 side to interlock with. Now that it does, the lock is gone and the
compare-exchange stands on its own.

## Testing

A new regression test, `atomic_lock_contended`, runs nineteen locked forms from
four threads at once and requires the arithmetic to come out exact. It is a
counter test on purpose: a lost update is the only symptom a broken atomic has,
and no single-threaded test can produce one — every existing x86 atomics test
passed throughout all of the above. It is checked against a real Devuan 6 /
Linux 6.12 box compiled both `-m32` and `-m64`, which is where the expected
values come from.

`poll_idle_cpu` is the same idea applied to cost rather than correctness: it
measures the CPU a blocking `poll()` spends on a quiet socket, because that is
the only thing the spin above was ever visible in.

The release gate itself had a bug worth naming: `if meson test … | tail -6`
tests the exit status of `tail`, which is always zero, so a failed end-to-end
leg never counted and the script could sign off **GATE CLEAN** with a red leg
printed six lines above it. Fixed. A gate that reports clean over a failure is
the one thing it must not do.

## Also in this build

- **The book ships on the device.** `/AOK/docs/book` is the whole thing — 43
  chapters and 8 appendices on how iSH-AOK works, from the emulator and its four
  guests through the VFS, native programs, the app, testing and releasing, to an
  honest account of what is still wrong. It existed only in the git repository
  before; a reader with a terminal and no browser is exactly who it was written
  for.
- **`/AOK/docs` has an index.** Twenty-one documents ship there and the overview
  linked six of them; six more were linked from nowhere at all. On a device,
  with no search, that is close to not shipping them.
- **`ktop`'s `%CPU` column was multiplied by the refresh delay.** It divided
  jiffies used by ticks-per-second rather than by ticks-per-interval, so with
  the default 3-second refresh every reading was three times too large — a
  process using one whole core showed as 301%. Changing `-d` changed every
  number on screen. Now measured from the clock across the two samples, so a
  late redraw stays honest too.
- `/AOK` is now a shortcut in the File Manager's left pane, alongside Home and
  Persist. It is the one path that looks the same on every root.
- **Compiled tests are cached.** Building ~190 tests under emulation was most of
  a release gate — 30–40 minutes a leg, five legs, and again on device — and
  almost none of it was new work. Binaries are now keyed on the source, the
  shared headers, the compiler and the machine, and reused when that key
  matches, in `/AOK/fakefs` where it is writable.
- `ISH_TRACE_POLL_WAIT_COMM` points the existing poll tracer at any process.
  It was hardcoded to a package-manager comm list, so it could not be aimed at
  the daemons that were spinning without editing the source and rebuilding.

## Verified

The full guest regression suite on all four architectures and on a glibc root,
plus the end-to-end suite, plus a real device:

| leg | libc | pass | fail | skip |
| --- | --- | --- | --- | --- |
| arm64 | musl | 185 | 0 | 6 |
| i386 | musl | 190 | 0 | 6 |
| amd64 | musl | 191 | 0 | 8 |
| riscv64 | musl | 179 | 0 | 6 |
| devuan-arm64 | **glibc** | 185 | 0 | 6 |
| e2e (i686) | musl | 7/7 | 0 | — |
| **device** (iPad, Devuan arm64) | glibc | 186 | 0 | 0 |

Two device failures were chased rather than waved through. `resource_limits_sched`
was the `ru_maxrss` bug above and passes 12/12 under fork load once fixed;
`tty_hangup_signal` failed once under the load of the rest of the suite and
passes 3/3 standalone and on every local leg — recorded in the next build's list
rather than dismissed, because "passes alone, fails in the suite" has been a real
bug here before.

Every fix in this build was checked against a real Devuan 6 / Linux 6.12 machine
before being called a fix, and in two cases the oracle is what defined the
expected answer.

## Known gaps

- `lock not` and `lock neg` on an **i386** guest still decode to *undefined* and
  raise SIGILL: the i386 `LOCK` opcode table has no group-3 entry at all. The
  new test skips those two forms on i386 rather than crashing, and says why.
  They work on amd64.
- A **misaligned** locked access still uses the global software lock, so it does
  not interlock with a host atomic on the same bytes. Nothing real does this,
  and on hardware it is atomic because the CPU makes it so — it is a gap in the
  emulation rather than in any one caller.
- The amd64 JIT still **bridges** locked instructions to a C helper rather than
  compiling them into host-atomic gadgets the way the i386 JIT does. That costs
  throughput, not correctness; the correctness half is what this build fixes.
- The page-table walk behind `ru_maxrss` and `/proc/<pid>/statm` is still
  **lock-free**, so it can read a leaf array another thread is freeing. 553
  fixed what made that observable, not the race itself.
- `strace` and `gdb` still **kill a thread they attach to** — attaching to a
  non-leader thread makes the following wait report the wrong pid, which trips
  gdb's own internal assertion and loses the process. Worked around during this
  cycle by reading `/proc/<pid>/io` instead; tracked for the next build.
