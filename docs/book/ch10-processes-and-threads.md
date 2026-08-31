# 10. Processes, threads, and the shape of the task table

Part II was about making guest instructions run. Part III is about the other
half of the emulator's job, which is larger and much less glamorous: being the
operating system those instructions call into. A guest program spends its life
asking questions — who am I, what time is it, is there data on this descriptor,
give me more memory, run this program, tell me when my child dies — and every
one of those answers has to be manufactured by code in this application.

This chapter is about the structure the rest of Part III hangs from. Get the
process model wrong and nothing above it can be right.

## 10.1 One struct

In Linux, a process is `struct task_struct` plus a mm, plus a files_struct, plus
a signal_struct, and a good deal of the kernel's design is about which of those
are shared and when. AOK has the same shape and one crucial difference: there is
no kernel/user boundary, so `struct task` simply contains everything.

It holds the guest CPU (`struct cpu_state cpu`, not a pointer — Chapter 5), the
memory map, the host thread that runs it, the file descriptor table, the
filesystem context, the UTS namespace, signal state and dispositions, ptrace
state, four sets of credentials, five sets of capabilities, scheduling values,
I/O counters, the family tree, and — unique to this fork — a block of fields
describing a native program that may be running on this task's thread
(Part V).

The declaration that makes all of it work is one line:

```c
extern __thread struct task *current;
```

`current` is thread-local. That is the entire "a guest process is a host thread"
design, stated once. Every function in `kernel/` that says `current->files` or
`current->pid` is reading the thread-local of whichever host thread is
executing, and the reason a native program compiled into the app can call into
the kernel layer at all (Chapter 22) is that it runs on a thread where `current`
is set.

The struct is also, deliberately, a written record of things that went wrong.
Reading `kernel/task.h` top to bottom is one of the better ways to learn this
codebase, because a large fraction of its fields carry a comment explaining the
bug that required them. Several of those comments are quoted in this chapter.

## 10.2 Fork is a struct copy and a `pthread_create`

`task_create_` allocates a `struct task` and does this:

```c
*task = *parent;   // uts_ns is only aliased here; copy_task retains or copies it
```

A shallow copy of the whole struct, followed by a long stretch of fixing up the
fields for which "the same value as the parent" is wrong: list heads are
re-initialized, pending signals are cleared, `zombie` and `exiting` and
`exit_finished` are reset, the vfork handshake is dropped.

Then `copy_task` applies the `CLONE_*` flags, and each one is the same question
asked about a different resource — share it, or copy it?

```c
if (flags & CLONE_VM_)    mm_retain(mm);
else                      task_set_mm(task, mm_copy(mm));
if (flags & CLONE_FILES_) task->files->refcount++;
else                      task->files = fdtable_copy(task->files);
if (flags & CLONE_FS_)    task->fs->refcount++;
else                      task->fs = fs_info_copy(task->fs);
```

and so on for the UTS namespace and the signal handler table. Finally
`task_start` calls `pthread_create`, and there are now two threads where there
was one.

> **The bug that taught us this**
>
> A shallow struct copy has a third possibility that nobody chooses on purpose:
> *alias*. Two tasks, one allocation, and whichever dies first frees it under
> the other.
>
> Native bash assigns `environ = export_env`, so the task's environment vector
> **is** bash's exported-variable array. Every command bash ran spawned a child
> task, which inherited the pointer through the struct copy and freed the array
> on its way out. The shell then read freed memory in
> `add_or_supercede_exported_var`, and the app died on a null entry — while
> running bash's own test suite, several commands after the one that caused it.
>
> The fix distinguishes three answers rather than two. `native_env` and
> `native_exec` are set to **NULL**, because a task created here is on its way
> to an `execve` and the environment it ends up with is that call's `envp`.
> Supplementary groups *are* inherited across fork, so `groups` is
> **duplicated** — but duplicated it must be, for exactly the same reason.
>
> In a system where fork is a struct copy, every owned pointer in that struct is
> a decision, and the default is always wrong.

One small detail in `copy_task` is worth noticing because it is the only place
in this chapter where the four guests appear:

```c
if (stack != 0) {
    task->cpu.esp = (addr_t) stack;
    if (task->abi == GUEST_ABI_AMD64)   task->cpu.amd64_regs[amd64_rsp] = stack;
    if (task->abi == GUEST_ABI_ARM64)   task->cpu.arm64_sp = stack;
    if (task->abi == GUEST_ABI_RISCV64) task->cpu.riscv64_regs[riscv64_sp] = stack;
}
```

`clone` with a stack argument means "the new thread runs on this stack", and the
register that means "stack pointer" has four different names. Chapter 11's rule
about second copies starts here.

## 10.3 What `clone` actually supports

The implemented flag set is explicit in `kernel/fork.c`:

```
CLONE_VM CLONE_FILES CLONE_FS CLONE_SIGHAND CLONE_SYSVSEM CLONE_VFORK
CLONE_THREAD CLONE_SETTLS CLONE_CHILD_SETTID CLONE_PARENT_SETTID
CLONE_CHILD_CLEARTID CLONE_DETACHED CLONE_PARENT CLONE_PIDFD CLONE_IO
CLONE_NEWUTS
```

That covers everything a threading library or a shell needs. What it does not
cover is the namespace family — `CLONE_NEWNS`, `CLONE_NEWPID`, `CLONE_NEWNET`,
`CLONE_NEWIPC`, `CLONE_NEWUSER`, `CLONE_NEWCGROUP` — with the single exception
of `CLONE_NEWUTS`, which is cheap because a UTS namespace is a hostname and a
domain name in a refcounted struct.

The absent ones are why nothing container-shaped runs here, and they are absent
architecturally rather than accidentally: there is one filesystem tree, one pid
space, and one network. Chapter 21 shows the one place that turns out to be an
advantage, and Chapter 41 is honest about the rest. `CLONE_IO` is accepted
because on Linux it is only a hint about I/O scheduling, and AOK has no I/O
scheduler for it to hint at.

## 10.4 Threads are children of their creator

Here is a genuine divergence from Linux, and it is the kind that hides for
years because almost nothing looks at it.

In Linux, every thread in a group shares the leader's parent. A thread created
by thread 4 of a process is not thread 4's child; it is a child of whatever
process created the group.

In AOK, **a thread is a child of whichever task created it**. `kernel/fork.c`
re-links a new task into `current->parent->children` only for `CLONE_PARENT`,
never for `CLONE_THREAD`.

For most of the system's life this was invisible, because almost nothing walks
the family tree of a thread. Then somebody implemented `de_thread` — the part of
`execve` that must leave exactly one thread standing when a non-leader thread
execs — and it surfaced immediately:

When the old leader exits, `find_new_parent` hands its children to the first
live thread in the group. After the other threads are zapped, the first live
thread *is the exec'ing thread itself*. So the exec'ing task became its own
parent, and the real parent's `wait()` returned `ECHILD`.

The symptom pointed nowhere near the cause. An earlier probe in the same
investigation had printed `ppid=8` for pid 8 — a process that is its own parent,
stated plainly in the output — and it was read past, because nobody was looking
for that.

The rule that came out of it: anything that retires or replaces a thread group
leader must fix the family tree explicitly, and must capture `leader->parent`
*before* the teardown, because afterwards there is nothing to capture.

## 10.5 Exit is not one moment

`do_exit` is not atomic, and the interesting bugs in this area come from
treating it as if it were.

A task partway through `do_exit` has already left `group->threads`, and it
continues to use its own `struct task` afterwards. So "the group is down to one
thread" does **not** mean "that struct is free" — the two questions are separate
and both get asked. That is why `exit_finished` exists:

```c
// Set by do_exit as its very last act, so another thread can tell "this
// task has left the group list" (which happens partway through do_exit)
// from "this task is finished with its own struct".
_Atomic bool exit_finished;
```

`de_thread` needs the second question, and there was no way to ask it.

The rest of the exit path is conventional: the task becomes a zombie holding its
`exit_code`, its parent is notified, `wait4` collects and reaps it, orphans are
re-parented — to init, or to the nearest ancestor that set
`PR_SET_CHILD_SUBREAPER`, which systemd relies on.

> **The bug that taught us this**
>
> Reference counting is not one thing, and this is the sharpest illustration in
> the tree.
>
> A pidfd must keep a `struct task` allocated after the task exits, or a poller
> holding one would dereference freed memory. So a pidfd takes a reference. But
> the exit path also waits for references to drain before proceeding — and those
> two facts together are a deadlock, because the holder of a pidfd typically
> learns about the exit *by polling the pidfd*, which only becomes readable
> after `do_exit` has run.
>
> Every systemd service whose main process PID 1 tracks by pidfd hung for the
> full job timeout. The task could not exit while the pidfd was open, and PID 1
> would not close it until the exit.
>
> The fix is a second counter: `pidfd_ref_count` records how many of the
> references are held by pidfds, and `exit_wait_needed()` subtracts it. A pidfd
> reference keeps the memory alive without gating progress — which is what it
> always meant, and what a single undifferentiated count could not express.

## 10.6 `SIGKILL` cannot mean "just this thread"

`receive_signal`'s `SIGNAL_KILL` case calls `do_exit_group`. That is correct:
`SIGKILL` on Linux terminates the whole thread group, and a signal sent to any
thread kills all of them.

It is also inconvenient exactly once, and the once is `de_thread`. Killing the
other threads of a group without killing the exec'ing thread cannot be expressed
as a signal, because the signal's disposition is "kill the group" and the
exec'ing thread is in the group.

The solution reuses everything except the disposition:

```c
// So the signal is still what wakes and reaches the thread -- all of that
// machinery is reused -- and this flag only changes the disposition, from
// "kill the group" to "exit just me".
bool exit_requested;
```

The reason to reuse rather than reimplement is stated in the same note, and it
is the theme of the next section: *reaching* a thread is the hard part. Deciding
what it should do on arrival is easy.

## 10.7 Getting a thread's attention

A guest task that is blocked is usually blocked inside a host call — `kevent`,
`epoll_wait`, `read`. The only way to tear a thread out of a host wait is a host
signal, and AOK uses `SIGUSR1` for that.

`SIGUSR1` is also used for TLB shootdowns and for memory-quiesce pokes, and that
sharing produced a subtle failure:

> A thread blocked in `real_poll_wait` (kevent/epoll_wait) can only be torn out
> of its host wait by a host signal, and SIGUSR1 is shared with TLB/quiesce
> pokes, so a guest-signal SIGUSR1 can be coalesced away or consumed in a window
> where it has no effect — letting the host wait run to its timeout and return 0
> instead of EINTR.

POSIX signals are not a queue; two `SIGUSR1`s delivered close together may
arrive as one. A wake that is merged into another wake is a wake that did not
happen.

The fix is to stop relying on a lossy channel for a message that matters:
`poll_notify_fd` holds the write end of the notify pipe of whatever poll the
task is currently blocked in, and guest-signal delivery writes a byte to it *in
addition to* sending `SIGUSR1`. A pipe write is not coalesced.

The same family of problem shows up at thread creation, and the answer there is
ordering rather than a second channel. `task_start` creates every thread with
`SIGUSR1` and `SIGUSR2` blocked:

> Otherwise a sibling's TLB-shootdown poke could be delivered while the new
> thread is mid-`malloc` instantiating that storage, making the handler re-enter
> `malloc` and abort on the malloc lock.

The new thread inherits the blocked mask and unblocks the signals itself once
its thread-local storage exists. A signal handler that runs before the thread is
ready is not an edge case in a system that creates threads under load; it is a
Tuesday.

## 10.8 The ghost task

> **The bug that taught us this**
>
> `pthread_create` returns a **positive** errno on failure. It does not use the
> `-1`-and-`errno` convention, and the check in `task_start` was `< 0`.
>
> So the check could never fire, and a failed thread creation was silently
> ignored. The result was a fully-linked `struct task` with no host thread
> behind it: a ghost that held a pid forever, appeared in `/proc`, and wedged
> its thread group's exit because the group was waiting for a thread that did
> not exist.
>
> It was found by the 10,000-thread storm benchmark, which is the only workload
> that reaches the host's thread limit reliably. `task_start` now returns
> `_EAGAIN`, matching what Linux's `clone` returns at the thread or rlimit
> ceiling.
>
> The part worth copying is what shipped alongside the fix:
> `ISH_TEST_FAIL_TASK_START_AFTER=N` makes every thread creation after the Nth
> fail as though the host were at its limit, so the unwind path can be
> regression-tested without a 16,000-thread storm. A rare path that cannot be
> triggered on demand is a rare path that will not stay fixed.

## 10.9 The thread group

`struct tgroup` holds what threads share: the thread list and the leader, the
process's accumulated rusage, session and process-group membership, the
controlling terminal, interval timers, POSIX timers, and resource limits.

Two of its fields are worth pulling out.

`stopped` is `_Atomic bool` rather than a plain bool guarded by the group lock,
and the comment says why: it is read locklessly on every interrupt-return fast
path in `handle_interrupt`. Job control (Chapter 12) has to be checked
constantly and taken almost never, so the check must be a relaxed load rather
than a lock acquisition. All writes still happen under `group->lock`, which also
orders the wait and notify on `stopped_cond`.

`itimer_vprof_sampler` is an honest workaround with its reasoning attached.
`ITIMER_VIRTUAL` and `ITIMER_PROF` measure CPU time, and AOK's timer subsystem
only supports `CLOCK_MONOTONIC` and `CLOCK_REALTIME`. Rather than reporting the
feature absent — which Chapter 40 explains is the worst available option — a
single periodic monotonic sampler drives both, armed lazily on first use.

And one field is a small case study in why a guest's *reported* state matters as
much as its real one:

> `cgroup_path` … systemd `--user` derives its own delegated subtree from
> `/proc/self/cgroup` — the hardcoded `"0::/"` made it try to create
> `init.scope` at the hierarchy root (EACCES for uid != 0), killing every
> `user@` start with "Failed to allocate manager object".

There is no cgroup implementation behind that string. What there is, is a
correct answer to the question systemd asks.

## 10.10 Reporting faithfully what you do not implement

That last point generalizes, and it is the thesis of this chapter.

AOK does not schedule. There is no run queue; guest threads are host threads and
the host's scheduler decides everything. But `struct task` carries `nice` and
`sched_policy` anyway, with this comment:

> Scheduling, which AOK does not act on but must report back faithfully: a
> process that sets `SCHED_IDLE` or a nice level and then reads back something
> else concludes the call failed.

The same reasoning produced several other fields:

- **`minflt` and `nvcsw`.** Every `getrusage` field except user and system time
  used to be a hard zero — "a value Linux never produces for a process that has
  run at all" — so `time -v`, Python's `resource` module, and every `wait4`
  supervisor reported a process that had touched no memory and never blocked.
- **`maxrss_kb`.** Latched on the task as well as on the mm, because `do_exit`
  releases the address space *before* it snapshots the task's final usage. Read
  only from the mm, peak RSS is zero by then — which is exactly the value
  `wait4` and `getrusage(RUSAGE_CHILDREN)` report, and exactly the consumer
  (`time -v prog`) that cares most.
- **`oom_score_adj`.** There is no OOM killer to adjust. The value is stored
  because systemd's executor writes it, reads it back, and calls
  `exit(EXIT_OOM_ADJUST)` if the file is missing or rejects a valid value.
- **Kernel threads.** `pid_is_kthread` and `pid_kthread_at` synthesize entries
  with no task and no thread behind them — "just enough of `/proc` for `ps` to
  render them bracketed, which is what programs testing 'am I in a container'
  actually look for."

None of those are features. They are the process model's *description of itself*,
and userspace reads that description constantly and makes decisions from it. A
system that behaves correctly but describes itself as a machine that has never
run any code will be misdiagnosed by every tool that looks.

Chapter 40 states the principle in its harshest form — reporting a capability
absent to avoid implementing it produces a state real systems never produce, and
callers fall into paths nobody tests. The corollary is this chapter's: reporting
a plausible value for something you do not implement is not a lie, it is an
implementation of the observable contract, and it is often the only part of the
feature anyone needed.

## 10.11 What the next chapter needs from this one

Three facts carry forward.

`current` is thread-local, so every syscall handler in Chapter 11 runs on the
thread of the task it acts on, and "the current process" needs no argument.

A task holds its own `struct cpu_state`, so a syscall's arguments are read from,
and its result written to, registers in that struct — through the per-ABI
accessors of Chapter 7.

And a task is reachable, but only just: waking one that is parked in a host call
takes a signal plus, where the signal can be coalesced, a pipe. Every blocking
syscall in the rest of Part III is built on top of that.

---

*Anchors:* [kernel/task.h](../../kernel/task.h), [kernel/task.c](../../kernel/task.c),
[kernel/fork.c](../../kernel/fork.c), [kernel/exit.c](../../kernel/exit.c),
[kernel/group.c](../../kernel/group.c), [kernel/pidfd.c](../../kernel/pidfd.c),
[kernel/signal.c](../../kernel/signal.c), [kernel/resource.c](../../kernel/resource.c).

*Story:* the exec'ing thread that became its own parent — because AOK links a
thread as a child of its creator, and `find_new_parent` handed the dying
leader's children to the first live thread in the group, which was itself.
