# 22. A native program is a function call

A guest process runs this:

```sh
$ /AOK/native/smallclue ls -l /etc
```

No file is loaded. No ELF header is parsed, no segments are mapped, no
interpreter is invoked, and not one guest instruction is translated. `execve`
looks at the resolved path, finds an entry in a table, and calls a C function
that was compiled into the application months earlier.

The calling process cannot tell. It has the same pid it had before, its parent
still sees it as a child, `waitpid` returns the function's return value as an
exit status, and `ps` shows what it expects to show.

This is the fork's central idea, and Part V is about everything that follows
from it — the good (interpretation at native speed, one implementation across
four guest architectures) and the difficult (a program that cannot fork, a libc
that answers questions about the wrong machine, and a set of failure modes that
do not exist for ordinary programs because ordinary programs get a fresh address
space every time they start).

## 22.1 What it buys

The obvious guess is I/O, and the obvious guess is wrong. Chapter 8 already
covered data movement: high-level emulation recognizes `memcpy` and `strlen` and
runs them natively, and gets 7x on a copy loop.

What native programs remove is different. It is the program's **own instruction
stream** — the interpreter loop that reads a script, parses it, walks a tree,
and evaluates. That work is not `memcpy`-shaped and no fingerprint will ever
match it. Under emulation it is millions of guest instructions, each one a
dispatch (Chapter 6).

Measured: an arithmetic loop under the native bash runs roughly **16x faster**
than under the emulated shell. Subshells and command substitutions land near
parity, for reasons Chapter 24 is entirely about.

There is a second benefit that gets less attention and is nearly free. From
`kernel/native.h`:

> Because no guest code runs, a native program is guest-ABI-independent: one
> implementation serves i386, amd64, arm64 and riscv64 guests alike, which
> matters most for the slowest of them.

An i386 guest gets exactly the same `ssh` as an arm64 guest, at exactly the same
speed, with no per-guest work at all. For the architectures whose translation is
most expensive, that is the largest single speedup available anywhere in the
system.

## 22.2 The registry

The whole mechanism is a table:

```c
struct native_program {
    const char *name;      // basename under /AOK/native/
    int (*main)(int argc, char *const argv[], char *const envp[]);
};
```

and its contents in this build:

```c
{ "smallclue", smallclue_real_main },
{ "motepad",   native_motepad_main },
{ "bmm",       native_bmm_main },
{ "bmt",       native_bmt_main },
{ "rust-probe", rust_native_probe_main },
{ "hx",        helix_native_main },
{ "bash",      native_bash_main },
{ "zsh",       native_zsh_main },
{ "zsh-multio", native_zsh_multio_main },
```

Adding a program is one entry. The dispatcher does not change, and — the detail
worth copying — neither does the filesystem, because `/AOK/native` is served
*from the registry* rather than from a second list beside it:

> Adding a native program is then one table entry, not an entry plus five places
> in the filesystem that have to agree with it.

The enumeration is over programs that are actually present, so a program whose
implementation is not in this build (an unpopulated submodule, a disabled build
option) is skipped entirely: no node, and `execve` finds nothing — "rather than
a node that dispatches into a hole". Chapter 21 described what that looks like
from the guest's side; this is where it is enforced.

## 22.3 One binary, many names

Matching happens on the **resolved** path, not on `argv[0]`. That single choice
is what makes the multicall pattern work:

```sh
ln -s /AOK/native/smallclue /usr/local/bin/df
```

`execve("/usr/local/bin/df")` resolves through the symlink to
`/AOK/native/smallclue`, dispatches to SmallCLUE's entry point, and `argv[0]` is
still `df` — so the applet dispatcher inside SmallCLUE selects `df` exactly as
busybox does on Linux. `/AOK/tools/native-links.sh` builds the whole symlink
farm and can undo it.

There is a trap in that convenience, and it has bitten more than once: those
symlinks live in the guest root and persist. A `grep` symlinked into
`/usr/local/bin` for a quick test stays there, and `/usr/local/bin` precedes
`/bin` on the default `PATH`. A regression run that decides verdicts by grepping
its own output then reports every test as both passing and failing, and nothing
in the output says "your PATH is poisoned". Put throwaway links somewhere off
`PATH`, or remove them.

## 22.4 Why the handoff is a two-step

The natural implementation is to run the native program where `execve` would
otherwise have loaded the image. That is wrong, for a boring and expensive
reason:

> that path never returns, so every buffer the execve syscall had allocated --
> including the argv/envp blocks it frees on the way out -- would leak on each
> invocation.

`execve` allocates. It copies argv and envp out of guest memory into kernel
buffers, and it frees them on the way out. But the ELF path does not *have* a
way out — it replaces the process image and jumps. For a real exec that is fine,
because the whole address space is being discarded anyway. For a native program,
nothing is discarded: the process continues, and every one of those buffers is
leaked, once per command the user types.

So the dispatch is split in two. `execve` calls
`native_exec_set_pending(prog, argc, argv, envp)`, which records the program on
the task with **private copies** of the arguments, and then returns success.
The syscall unwinds normally, frees its own blocks, and only then does
`native_exec_run_pending()` run the program and terminate the task with its
return value.

Two edge cases hang off that design, and both are handled where you would not
think to look. `task_run_current` also calls `native_exec_run_pending`, because a
task whose *very first* image is native is reached without any `execve` syscall
returning at all. And `native_exec_discard_pending` exists for a task torn down
between the record and the run, so the copies are not leaked by the failure path
either.

## 22.5 The rule

Everything difficult about native programs comes from one sentence:

> **A native program is not a process. It is a C function, in the app's process,
> called again every time the user types the command.**

An ordinary program is written against an implicit guarantee nobody states
because nobody has to: *every global starts at its initializer*. That guarantee
comes from the operating system giving you a fresh address space. A native
program has no fresh address space. It has the app's, which has been running for
hours and may run for days.

So the rule, in the form the project states it:

> **Any global a native program sets must be cleaned up when it exits — or keyed
> per invocation.**

Here is what its absence looks like, in the smallest possible case. Nextvi keeps
its stop flag in a global called `xquit`. Typing `:q` sets it to 1 and the editor
exits. The main loop is `while (!xquit)`.

The second `vi` of the session drew the file and returned immediately.
Instantly, silently, successfully — exit status 0, no error, no output beyond a
single frame. The program was correct. The assumption it was written under had
stopped being true.

There are two correct shapes, and both are in the tree:

**Reset on entry, tear down on exit.** Nextvi's wrapper does this; helix calls
`logging::shutdown(rt_id)` from its native entry point. Good when the state is
yours to reset.

**Key the state by the invocation token.** `nlibc_invocation_token()` returns a
value that is different for every run of every native program, inherited by every
thread the run creates, and async-signal-safe to read — it is one `__thread`
load. Zero means "this thread is not running a native program". Good when the
state belongs to somebody else's library.

That second mechanism has an unusual property worth noting: **the symbol name is
ABI**. Foreign code imports it. The project's forks of `tokio` and
`signal-hook-registry` key their per-process globals on it (Chapter 25), which
means a Rust crate somewhere is calling a C function in AOK's kernel by name.

## 22.6 A global holding a descriptor is a landmine

One category deserves separating out, because the obvious fix makes it worse.

File descriptor *numbers* are per-task. A native program that stashes an fd in a
global leaves a number behind. The next run — a different task, with a different
descriptor table — finds that global populated, and if it politely cleans up by
closing the stale object, it closes **that number in its own table**, which
names an unrelated open file belonging to the current process.

So the rule inverts: replaced cross-run state that holds descriptors must be
**leaked, not dropped**. Only the run that owns a descriptor may close it.

> **The bug that taught us this**
>
> The second `hx` in one app session simply failed to start.
>
> Two independent once-per-process initializations were the cause: tokio's
> signal handling creates a socketpair once and caches the descriptors, and
> helix's logger refuses to initialize twice. The first run of the editor left
> both in place. The second found tokio holding a pair of descriptors belonging
> to a task that no longer existed, and a logger that considered itself already
> configured.
>
> Neither library is wrong. Both are written against the guarantee in Section
> 22.5, and that guarantee is what a native program does not get.

## 22.7 Three more things that are not true any more

**The program *is* the task.** It runs on the calling task's own thread, inside
the `execve` that would have replaced the image. So `pid`, `waitpid` and exit
status all work with no new machinery — and a long-running native program blocks
that guest task for its entire duration, because it is that task.

**Nothing checks for signals.** Translated code is dispatched by a loop that
tests for pending signals at block boundaries (Chapter 6). A native program is a
plain C loop, and nothing tests anything. Without help it is **uninterruptible**
— which, in the header's own example, "is how `top` became impossible to quit".

`native_checkpoint()` is the answer: poll for pending signals and group-stops,
and act on them. The libc shim calls it on every read and write, which covers
anything doing I/O; a compute loop with no I/O still needs its own call. It may
not return, because a default-fatal signal exits the task from inside it. And it
lives in `kernel/native.h` rather than in the shim deliberately, "because it is
a property of running natively, not of any one program".

**The libc is the host's.** A native program is linked against Darwin's libc, so
a bare `open()` or `write()` reaches the *host* filesystem rather than the
guest's rootfs. That is not a subtlety to be careful about; it is a wall. A
program compiled without the shim writes files onto the Mac and reads the host's
`/etc/hosts`, and does so silently and successfully. Chapter 23 is about the
layer that fills that seam, and about the gate that proves it was filled.

## 22.8 What was considered and rejected

The obvious grand unification is to give native programs a guest address space:
let them execute natively but hold their heap and globals in AOK's per-task
guest memory, so that `fork` becomes a copy-on-write of that region and every
fidelity gap in Chapter 24 closes at once.

It was explored properly in August 2026 and measured. **Do not build it**, in
the tree's own words, for two reasons in order of decisiveness.

**It would not produce `fork` anyway.** A real fork's child continues on a copy
of the parent's C stack. Native stacks are host memory at absolute host
addresses; neither address translation nor a base-plus-offset scheme reaches
frame chains or a spilled `&local`. Even perfect "guest data" yields a faithful
*state transfer* to a child that still starts at a fresh entry point — which is
re-launch, which is what Chapter 24 already does.

**The lock is the cost.** Reading guest memory requires the memory read lock
held; measured, the lock and unlock pair is a flat 8.7–10.0 ns against a 0.19 ns
three-level page walk. On a pointer chase that hides in the cache-miss shadow
(1.26x). On tight access it is **33–39x**.

Chapter 27 has the full account. It is included here because "why not just do the
obvious thing" is the first question anyone asks about this design, and the
answer is a measurement rather than a preference.

## 22.9 No boundary at all

Chapter 20 ended on a comparison worth completing.

FUSE lets a guest program act as part of the kernel, and puts a **wire protocol**
between them. The daemon can crash without taking anything with it; the cost is a
round trip per operation.

A native program is host code acting as a guest process, and there is **nothing**
between them. Same address space, same heap, same libc, same signal handlers,
same stdio locks. That is where the speed comes from, and it is also why Chapter
1's `fflush(NULL)` story is in Chapter 1: a guest task killed inside a native
program's stdio leaves a *host* stream lock held by a thread that no longer
exists, and two `ish` processes were found blocked on exactly that, five and
twenty-three hours after their guests had exited.

There is no sandbox here. What there is instead is a set of rules, a shim, and a
gate that checks the rules were followed — which is Chapter 23.

---

*Anchors:* [kernel/native.h](../../kernel/native.h), [kernel/native.c](../../kernel/native.c),
[kernel/exec.c](../../kernel/exec.c) (the dispatch branch),
[kernel/task.h](../../kernel/task.h) (`native_exec`, `native_env`, `native_argv`),
[fs/aok.c](../../fs/aok.c) (`/AOK/native` served from the registry),
[opt/AOK/docs/native-programs.md](../../opt/AOK/docs/native-programs.md),
[opt/AOK/docs/native-setup.md](../../opt/AOK/docs/native-setup.md),
[docs/bash_native_plan.md](../../docs/bash_native_plan.md).

*Story:* the second `vi` of a session drawing the file and exiting immediately
with status 0 — because `xquit` was still 1 from the first one, and a program
that has never had to think about its own globals surviving is every program.
