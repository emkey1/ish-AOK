# 27. What native programs cost

Part V has been mostly about what the technique buys. This chapter is the
invoice, and it opens with the sentence that sets every price on it:

**A native program's crash is the application's crash.**

An emulated guest program that dereferences a null pointer produces a guest
`SIGSEGV`, a core dump if anyone asked for one, and a shell prompt. Everything
else keeps running, because the guest process was a `struct task` and the
emulator is still there to clean it up.

A native program that dereferences a null pointer takes down the process it is
running inside — which is the terminal, and the shell, and the editor in the
other tab, and the `ssh` session, and everything anybody had not saved. There is
no isolation to fall back on, because the absence of isolation is the whole
mechanism (Chapter 22).

So every failure mode in this chapter is an app-killer, and the countermeasures
are correspondingly aggressive.

## 27.1 Running off the end of a stack

A guest process that recurses forever hits a guard page and gets `SIGSEGV`. A
native program shares the application's thread stacks, so a runaway recursion
walks off the end of one and the app is gone.

bash makes this easy to reach:

> bash's `FUNCNEST` is **unset** by default, so nothing at all stood between an
> ordinary runaway recursive function and the end of the stack.

Not a crafted input. A recursive shell function with a missing base case — the
kind of thing people write at a prompt while working something out.

The fix is worth reading as a piece of design, because all four of its decisions
are transferable:

**It lives in the shim, not in the shell.** `nlibc_stack_exhausted` is in
`kernel/native_libc.c` "so one implementation serves every native program
including ones not written yet". zsh's own guard had landed first; putting the
second one in the same place would have meant a third for the next program.

**Bounds come from the platform, per thread.** `pthread_get_stackaddr_np` and
`pthread_get_stacksize_np`, so a guest task thread, the main thread and any
future host thread each answer for themselves. Only "where am I now" comes from
`__builtin_frame_address`.

**The state is `__thread`.** A re-launched subshell (Chapter 24) is a fresh task
on a fresh thread and must work out its own bounds — "which is what makes the
guard hold inside `$( )` and inside a pipeline, not just at the top level".

**When the platform cannot answer, nothing is refused.**

> a guess here would break working scripts to prevent a crash that might not be
> coming.

That last one is the principle worth keeping. A safety check that fires when it
should not is not a conservative choice — it converts a *possible* crash into a
*certain* failure, and the failure lands on someone whose script was fine.

## 27.2 Dying inside somebody else's lock

Chapter 1 told half of this story: two `ish` processes found at 0% CPU, five and
twenty-three hours after their guests had exited, blocked in
`_fwalk → sflush_locked → flockfile`. A guest task had been killed inside stdio,
leaving a host stream lock held by a thread that no longer existed, and Darwin
does not release a mutex when its owner dies.

The other half is the fix: **a fatal signal must not fire while host stdio holds
a `FILE` lock.** Delivery is deferred until the lock is released, because the
alternative is a process that cannot exit and cannot be diagnosed.

What makes this worth a section rather than a footnote is the forensics, because
"some stream's lock is held" is not a diagnosis — it is a restatement of the
symptom. Naming *which* stream and *who* held it took a specific sequence, and
it is one worth having written down:

> **`sample <pid>` first.** It gives the whole stack and the live thread list
> for free, no attach, no permission prompt. Two threads left and neither in
> stdio already implies the lock owner is gone.

Then recover the `FILE` from the blocked frame — by *reading Darwin's
`flockfile` prologue* rather than guessing at it:

```
lldb -b -p PID -o "disassemble -s <flockfile addr> -c 20" -o "register read x19 x22"
```

`flockfile` does `mov x19, x0` (so `x19` is the `FILE`), then
`ldr x8, [x19, #0x68]` (`_extra`, at `FILE` offset 104), then
`add x0, x8, #0x8` (`&_extra->fl_mutex`). And
`_pthread_mutex_firstfit_lock_slow` keeps that mutex pointer in `x19` — so frame
0's `x19` is `_extra + 8`, and the `FILE` follows by arithmetic.

That is Chapter 36's stripped-binary discipline applied to the host's own libc:
the disassembly of a function you did not write is a specification of where its
arguments went, and it is more reliable than any assumption about calling
conventions.

## 27.3 The task that could not be killed

Some costs are not the native program's fault at all, and this one is the
clearest example of a report naming the wrong subsystem.

The bug was filed as "a task spawned by a native program cannot be killed", and
it was neither native-specific nor about spawning. The trigger was fork churn.

What it actually was:

> AOK wakes a blocked task with `pthread_kill(task->thread, SIGUSR1)`. On Darwin
> that poke is intermittently swallowed in a way that leaves `SIGUSR1` **blocked
> and pending in the target thread's own host mask with the handler never
> running**. The thread is then permanently deaf: it finishes its host syscall on
> its own schedule and never reaches the syscall-return checkpoint where
> `receive_signals()` would act.

Every guest-side fact was correct. `SigPnd` was set. `SigBlk`, `SigIgn` and
`SigCgt` were all zero. `sys_kill` returned 0. Reading `/proc` would have told
you the signal was pending and deliverable and that the process was simply
ignoring it, which is not a thing a process can do to `SIGKILL`.

The reason it surfaced where it did: `clock_nanosleep_common` was the one
blocking site with no notify pipe and no waiting condition variable, so the poke
was its *only* way out. Chapter 14's rule — when a wake must not be lost, put it
on a channel that counts — had one site it had not reached.

Measured before and after across six shapes: **93 of 120 unkillable, then 0 of
120**, with two independent verifiers reproducing it in their own from-scratch
builds before the fix was accepted.

This is the same family as the unexplained mask anomaly of Chapter 12, and the
same operational rule applies: do not build a fix whose safety argument is "the
signal is blocked here".

## 27.4 The host libc is not one libc

A native program links against Darwin's libc, and Darwin's libc is a version
matrix.

`strchrnul` exists on newer iOS and not on older ones. Linked weakly, it
resolves to NULL where it is absent — and a call through a null function pointer
goes to address zero.

> the `strchrnul` weak import … killed every device below iOS 18.4 the moment
> the terminal started

Every device. On launch. Not a rare path, not a specific workload: the symbol
was reached during startup, so the app was unusable on a large fraction of the
installed base, and the crash reports pointed at address zero, which names
nothing.

The fix is to implement it in the shim rather than import it. The general rule
is harder and worth stating: **"the host provides it" is a claim about a
specific OS version**, and this project's entire reason for existing is old
devices. Every host symbol a native program reaches is a compatibility floor,
and the gate of Chapter 23 enumerates them precisely because that list needs to
be short and known.

## 27.5 When not to make something native

Part V has enough experience in it now to state the decision criteria, and they
are mostly reasons to say no.

**Is the win interpretation, or something else?** Native execution removes the
cost of *translating the program's own instructions*. If a program is I/O-bound,
crypto-bound, or spends its time in the kernel, that cost is not where its time
goes and the effort buys little. `sshd` is the worked example (Chapter 25): the
right mechanism for it was an accelerator, not a port.

**Does its API split into a pure half and an I/O half?** If it does, the shim's
`#define` layer is enough. If I/O runs throughout, the price is the whole
symbol-rewriting pipeline and the four silent traps that come with it.

**Does it fork for a reason re-launch can serve?** Re-launch is state transfer.
It serves "a second copy of me that then diverges". It does not serve privilege
separation, and it does not serve a daemon forking per connection to isolate
faults, because isolating faults is exactly what a native program cannot do.

**Does anything need to outlive the call?** A native program is a function call.
Something that must hold descriptors after the command returns needs a separate
program — which is why zsh's MULTIOS redirections needed `zsh-multio` rather
than a function inside zsh.

**Can its globals be tamed?** Reset-on-entry if the state is yours;
`nlibc_invocation_token()` if it belongs to somebody else's library. If the
state is deep, undocumented, and not yours, that is a reason to stop rather than
a problem to solve later.

## 27.6 The answer that was measured and declined

The obvious escape from most of the above is to give native programs a guest
address space — heap and globals in AOK's per-task memory, so `fork` becomes a
copy-on-write of that region and every fidelity gap closes at once.

Chapter 22 gave the summary; the full finding is that it was explored by a
multi-agent study in August 2026 and the verdict was **do not build it**, for
two reasons in decreasing order of decisiveness. It would not produce `fork`
anyway, because a real fork's child continues on a copy of the parent's *C
stack*, which is host memory at absolute addresses that no translation scheme
reaches — so the prize was only ever state transfer, which re-launch already
delivers. And the cost is the memory lock: 8.7–10.0 ns for a lock/unlock pair
against a 0.19 ns three-level page walk, which hides in the cache-miss shadow on
a pointer chase (1.26x) and is **33–39x** on tight access.

It is included here as well as there because "why not just do the obvious thing"
is the first question anyone asks about this design, and because the answer's
form matters: a seven-agent exploration produced a measurement, and the
measurement closed the question rather than a preference closing it.

## 27.7 The ledger

**What it buys.** Roughly 16x on shell interpretation, and 38–46x on the ceiling
for time spent inside bash. One implementation serving all four guest
architectures, which is worth most to the slowest of them. An editor, an `ssh`
client and a toolbox that respond at native speed on a ten-year-old device.

**What it costs.** An app-crash class that needs its own stack guard and its own
stdio-lock discipline. A shim that is nine thousand lines and structurally
never finished. A gate whose allowlist has to stay too small. A rule about
globals that every new program must be audited against, and which no program's
author has ever heard of. Four foreign-toolchain traps that all exit zero. And a
`fork` that is a text file.

**Why it is worth it.** The alternative was never "a safer native program". It
was no native program, and 40x on everything a shell does — which on the devices
this project exists for is the difference between a system somebody uses and a
demonstration that it can be done.

Part VI is about the other half of that sentence: the application these programs
run inside, and what it takes to make an emulator feel like a terminal.

---

*Anchors:* [kernel/native_libc.c](../../kernel/native_libc.c)
(`nlibc_stack_exhausted`, the deferred fatal signal, `strchrnul`),
[kernel/native.h](../../kernel/native.h),
[kernel/signal.c](../../kernel/signal.c) (`signal_wake_task`),
[kernel/time.c](../../kernel/time.c) (`clock_nanosleep_common`),
[main.c](../../main.c) (`cli_halt`'s comment on `fflush(NULL)`),
[docs/TODO.md](../../docs/TODO.md),
[docs/release-notes-since-iSH-AOK_549.md](../../docs/release-notes-since-iSH-AOK_549.md).

*Story:* a weak import of `strchrnul` resolving to NULL — and every device below
iOS 18.4 crashing at address zero the moment the terminal started.
