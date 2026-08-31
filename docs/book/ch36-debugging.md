# 36. Debugging a system with no debugger

Attach `lldb` to iSH-AOK and you get the *host* process: Objective-C frames,
Mach threads, and — somewhere in the middle — a thread sitting in `jit_enter`
executing an array of gadget addresses on behalf of a guest process whose state
lives in a struct the debugger has no idea how to read.

Everything the guest thinks is happening is invisible. There is no guest stack to
walk, no guest symbols, no guest breakpoints. And the interesting failures are
mostly races that stop reproducing the moment anything slows down.

So this chapter is about what replaces a debugger, and — as much — about the
rules for not being fooled by what it tells you.

## 36.1 The log, and the file descriptor nobody opened

The primary diagnostic is `strace`-style syscall logging, compiled in when the
build asks for it:

```sh
meson setup build-strace -Dlog=strace
```

Every syscall handler calls `STRACE(...)` with its parameters and its result, so
the log is a transcript of the conversation between guest and kernel. It answers
the question Chapter 11 said the syscall tables cannot: *which code actually
ran*, as opposed to which code you would expect from reading.

And it has a trap that has cost hours more than once.

> **The bug that taught us this**
>
> `printk` is `ish_printk` → `log_line` → `writev(555, ...)`. **File descriptor
> 555 is open only if you opened it.**
>
> That is the convention the strace build uses, and it applies to *every*
> `printk` in the tree — so a diagnostic `printk` added to chase a bug prints
> nothing at all, and reads exactly like "the condition never happened".
>
> Two separate negative results in one investigation were this, and one of them
> would otherwise have closed the case an hour earlier.
>
> The invocation is `bash -c 'exec 555>trace.log; ./build-strace/ish ...'` — and
> note that **zsh cannot parse `555>file`**, so the obvious way to attach it does
> not work from the project's own default shell.
>
> For a one-off debugging run, use `fprintf(stderr, ...)` instead — and verify it
> with a probe line before trusting any "nothing fired".

That last sentence is the durable rule: **before believing a negative result,
prove the instrument works.** It recurs in Section 36.4 with watchpoints, and it
is the single most common way an investigation goes backwards.

The log is also lossy by design — a 16 KB per-thread buffer that flushes on
newline — which is a reasonable trade for something in the syscall path, and a
thing to remember when the last few lines before a hang are missing.

## 36.2 Reading a hang

`sample <pid>` first, always. It gives the whole stack and the live thread list
for free — no attach, no permission prompt, no perturbation of what is running.
Chapter 27's stdio-lock investigation got most of its answer from that alone:
two threads left, neither in stdio, therefore the lock owner is gone.

`lldb -p <pid>` and `thread backtrace all` come next, and Chapter 12's
`PTRACE_SEIZE` hang is the model for what to do with the output: it named both
sides in one shot — the tracer in `do_wait`, the tracee in the *untraced*
job-control wait — and that second frame was the entire bug.

Then the rule that Chapter 16 paid for:

> **Blocked is not contended. The question that separates them is: who holds it?**

N threads blocked on a lock look identical whether the lock is held or free, and
only one of those is contention. A confident, specific, written-down diagnosis
naming a lock cycle turned out to describe a lock nobody held — the waiter was
asleep on a *free* lock, a Darwin lost-wakeup, triggered by the very signal
pokes sent to evict readers.

## 36.3 Measuring locks

`ISH_LOCKSTATS` and `ISH_FAKEFS_LOCKSTATS` instrument every `lock_t` and
`wrlock_t`, reporting duty cycle, aggregate wait, and per-call-site holds. They
produced the map in Chapter 16: `inodes_lock` at 74–77% duty with 880 ms of
aggregate wait, `mem->lock` not a problem at all.

Two literacy notes, because the numbers are easy to misread:

**A duty cycle above 100% on an exclusive mutex means the tool is wrong.** On a
*shared* lock it means concurrency, which is the point of a shared lock —
`mem->lock`'s read side at 240–305% under `make -j4` is what healthy looks like.

**A lock that is saturated by one thread cannot be fixed by parallelism.**
Chapter 17's fakefs mutex was at 78% duty with a single thread, which is why
concurrent metadata work measured *slower*.

## 36.4 Watching memory without a debugger

Some questions are only answerable by catching the write: *who stored this
value?* If the bug does not reproduce under `lldb`, that is normally
unanswerable.

It is not, on arm64. `thread_set_state(port, ARM_DEBUG_STATE64, ...)` sets
hardware watchpoints **from inside the process**, with no entitlement and no
debugger — and it works on *other* threads while they are running. iSH-AOK ships
it as `ISH_PTHREAD_WATCH`.

The mechanics are small: `__wvr[i]` is the address, `__wcr[i]` encodes a
store-only EL0 watch, masked ranges work (a 32 MB window traps fine), and the
exception arrives as `SIGTRAP` with the exact store address in `__far` and the
storing instruction in `__pc`. The PC is not advanced, so returning re-executes
and re-traps: report and `_exit`, or disarm first.

And two ways it lies, both hit in practice:

**A watchpoint that was never applied looks exactly like one that was never
hit.** The remedy is a positive control that stores a watched word's own value
back over itself and shouts if no trap follows — run it before believing a quiet
run. This is Section 36.1's rule again, and it keeps arriving because negative
results are the ones nobody checks.

**A dynamically chosen watch address goes stale.** Watching "wherever the
interesting object is right now" traps on whoever legitimately owns those bytes
later; ten traps named the wrong culprit until the trap handler re-validated
that the address was still the thing it meant.

New threads also start with a clean debug state, so arming has to happen at
thread creation as well as periodically.

## 36.5 Forensics on code you do not have

Crashes arrive as reports, from devices you do not own, in binaries that are
stripped. Chapter 27 showed one half of that — reading Darwin's `flockfile`
prologue to recover a `FILE` pointer from a register. The other half is a JVM
crash diagnosed from a log file and `llvm-objdump`, with no symbols, no debug
info and no debugger:

**The register dump names the bad value.** The failing assertion was
`val < (1ULL << nbits)`, and `R1=0x00000000ffffffff` in the report *is* that
`val`. A −1 in an unsigned argument means some lookup or encode helper returned
its failure sentinel — which is a far narrower question than "why did the JVM
crash".

**The caller identifies the computation.** Frames appear as `libjvm.so+0xNNNNN`
even when the library is stripped, so disassembling that range showed the whole
argument setup: the instruction word being patched, the field it went into, and
the subtraction that fed it.

**A global's identity comes from the other code that touches it.** A value
loaded through a GOT slot has no name — but the slot's target address does, and
everything else that reads or writes it can be found and read.

Three moves, no symbols, and an answer specific enough to fix. The general form:
**a crash report is a partial memory dump plus an address space, and both are
readable.**

## 36.6 Driving what has no interface

Some bugs are only visible in a full-screen program, which is awkward when the
harness has no terminal.

Three things are needed for a guest TUI — `btop`, `ktop`, `top` — and each fails
differently:

1. **A pty with a size.** `script -q /dev/null` gives a pty and no window size,
   and `btop` exits with "Failed to get size of terminal!". A `pty.fork()`
   wrapper with an explicit `TIOCSWINSZ` is the fix.
2. **`--force-utf`**, because a guest with no locale files makes `btop` refuse to
   start.
3. **A screen reconstruction.** The raw capture is cursor-addressed, so grepping
   it finds labels in the wrong places — a memory total read as a disk total, and
   panels that look blank because their content was written by an earlier
   absolute-position escape. About forty lines handling `CUP`/`ED`/`EL` plus
   printable text is enough to dump the real screen and settle it.

That third one is the general hazard: **a terminal capture is not text.** Any
tool that greps one is asserting that the program wrote its output in reading
order, which full-screen programs never do.

And one harness note with a broader moral: automation whose stdin is a socket
cannot allocate a pty at all — macOS `script` fails with
"tcgetattr/ioctl: Operation not supported on socket" and writes a **zero-byte**
capture file, which reads as "the program printed nothing" rather than as "the
harness failed".

## 36.7 Rules for not fooling yourself

Each of these was paid for once, in this project, and each is short enough to
remember.

**Check the oracle before claiming a defect.** Empty output is not death. Real
Linux shares more behaviour than expected, and a difference has to be
demonstrated rather than assumed.

**Not faulting is not executing.** A probe that runs an instruction group and
checks only that nothing crashed passes when the instruction is a no-op. Demand
an observable witness — a register, a memory effect, a signal.

**A knob that "fixes" a crash may only be short-circuiting the read.** When
bisecting by feature flag, check that the flag is not simply preventing the code
from reaching the thing that was wrong.

**Root hides permission bugs.** The CLI runs as uid 0, so ordering bugs between
permission and existence checks are invisible there (Chapter 15).

**A refuted finding can still contain a real bug.** A verifier that correctly
rejects your *classification* may be reporting a genuine defect underneath it.
Read the refutations.

**Verify what the user runs.** Not a build with the same sources — the artifact
they will install, in the shape they will install it (Chapter 34).

**Re-derive a recorded diagnosis before acting on it.** However confident,
however specific, however much it looks like somebody already did the work
(Chapter 16).

**And prove the instrument before believing a negative.** The `printk` that went
to a closed descriptor, the watchpoint that was never armed, the `script`
capture that was zero bytes — all three read as "the thing did not happen", and
all three were the tool failing silently.

## 36.8 What debugging is here

In an ordinary program, the debugger is the ground truth and the program is the
thing under suspicion.

Here there is no ground truth in the room. The host debugger sees a different
system than the one that is failing; the guest's own view is produced by the
code being doubted; and the most interesting bugs are races that disappear under
observation.

So every technique in this chapter is really the same technique: **construct an
observation the failure cannot avoid, and then check the observation itself
before trusting it.** A byte on a pipe rather than a signal that can coalesce. A
hardware watchpoint with a self-test. A screen reconstruction rather than a
grep. A widened race window with a measured before and after.

The debugger is not missing so much as it is one instrument among several, none
of which can be believed on its own.

---

*Anchors:* [kernel/log.c](../../kernel/log.c), [debug.h](../../debug.h),
[util/lockstats.c](../../util/lockstats.c), [util/lockstats.h](../../util/lockstats.h),
[kernel/task.c](../../kernel/task.c) (`ISH_PTHREAD_WATCH`),
[ish-lldb.lldbinit](../../ish-lldb.lldbinit), [ish-gdb.gdb](../../ish-gdb.gdb),
[fs/fake-lockstats.h](../../fs/fake-lockstats.h),
[docs/TODO.md](../../docs/TODO.md).

*Story:* two negative results in one investigation that were both the
instrument — a diagnostic `printk` writing to file descriptor 555, which is open
only if you opened it, and reads exactly like the condition never happening.
