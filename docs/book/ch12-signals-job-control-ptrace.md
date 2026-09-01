# 12. Signals, job control, and ptrace

A signal is a message to a program that is not listening. That is what makes it
hard: the recipient is doing something else — computing, or blocked in a `read`,
or stopped, or already handling a different signal — and the operating system
has to interrupt it, run code it did not call, and then put everything back.

In a real kernel there is one natural place to do that, on the return path from
kernel to user mode, and every signal delivery in the system funnels through it.
iSH-AOK has no such boundary, so it has to invent one. This chapter is about
what it invented, the second one it accidentally invented alongside, and the
long tail of bugs that came from the two drifting apart.

## 12.1 Where "return to userspace" happens

There are exactly two places where a guest task can be made to notice something.

The first is `handle_interrupt` (Chapter 11). Translated code leaves the JIT for
a syscall, a fault, or a timer poke, and the interrupt-return path checks
whether there is a pending signal, whether the thread group is stopped, and
whether a tracer wants a stop reported. That is the analogue of a kernel's
return-to-user path, and it covers every guest process running translated code.

The second is `native_checkpoint` (Chapter 22). A native program is host C
running on the guest task's thread, so nothing dispatches instructions for it
and `handle_interrupt` never runs. Instead the libc shim calls a checkpoint
function from every syscall it makes, and that function does the same job.

Two delivery points, written years apart, doing the same thing. Section 12.8 is
about the day that stopped being merely inelegant.

## 12.2 Where signals live

Signal state is split between the task and the thread group, exactly as Linux
splits it, because the two kinds of signal have different rules.

Per task: `blocked` (the mask), `pending` (a bitmask), `queue` (the queued
`siginfo`s for real-time signals), `waiting` (what a `sigtimedwait` is parked
on), and the alternate signal stack, which is per-thread and not shared even
with `CLONE_SIGHAND` siblings.

Per `struct sighand`, shared across the group: the `action` table — 64
`struct sigaction_` — and a second, *process-directed* queue:

> Process-directed signal queue, shared by every thread in the `CLONE_SIGHAND`
> group (Linux's `signal_struct->shared_pending`). A signal landing here (as
> opposed to one specific task's own `pending`/`queue`) can be observed and
> dequeued by ANY sibling thread with it unblocked — not just whichever task
> object the sender happened to address.

That distinction is the whole of "a signal sent to a process is delivered to
some thread of that process", and getting it wrong is not academic: `sigqueue`
was once delivered thread-directed, straight into the resolved task's private
queue. Linux routes it through the process, so any thread that can take the
signal is a legitimate destination — and a sibling parked in `sigwait` is the
entire reason a program uses `sigqueue`. That thread sat out its full timeout
while the signal waited, undeliverable, beside it. `kill(pid, SIGTERM)` does not target a thread; it
targets the group, and whichever thread has `SIGTERM` unblocked and gets there
first handles it. Modelling that as "deliver to the leader" works until the
leader has the signal blocked, at which point a program that carefully dedicates
one thread to signal handling stops receiving signals.

## 12.3 Sending, and a lock that had to be invented

Delivery looks simple — set the bit, queue the info, wake the target — and the
waking is where it becomes difficult, because waking a task can require taking
locks that the sender must not hold.

`signal_wake_task` therefore releases `sighand->lock` before calling
`wake_waiting_task`, and reacquires it afterwards. That avoids an ABBA deadlock
against the code paths that take `pids_lock` before `sighand->lock` — the
established order, never the reverse.

It also opens a hole, and the comment on the field invented to plug it is one of
the most precise pieces of writing in the tree:

> That leaves a real window where `lock` is genuinely, fully unlocked despite
> the "caller holds sighand->lock" contract — a second, concurrent
> `deliver_signal_*` call for the same sighand (e.g. two children exiting at
> once, each delivering SIGCHLD to the same parent) can legitimately acquire
> `lock` during that window and reach its own `signal_wake_task` call, which
> then races the first call's manual unlock/relock of the exact same mutex —
> undefined behavior for a plain, non-recursive `pthread_mutex_t`, and able to
> corrupt `lock` for good.

Two children exiting at the same time is not an exotic scenario; it is what a
shell pipeline does. The fix is `wake_lock`, acquired *only while `lock` is not
held*, so it can never nest with `lock` in a way that reintroduces the ABBA
hazard. It ensures at most one thread is mid-wake for a given sighand at a time.

The general lesson is worth extracting, because it applies well beyond signals:
**a lock you temporarily release is a lock somebody else can take.** Code
written under a "caller holds X" contract is not entitled to assume X was held
continuously, and any function that drops and reacquires needs to say so at the
top.

## 12.4 Reaching a thread that is not running

Chapter 10 introduced the wake problem; here is the code:

```c
// Wake a sibling blocked in poll_wait through its notify pipe, in addition to
// the SIGUSR1 poke. SIGUSR1 is shared with the TLB/quiesce shootdown poke and
// does not queue, so under load the guest-signal SIGUSR1 can be coalesced with
// a poke or land in a window where it has no effect, leaving real_poll_wait to
// run to its timeout and return 0 instead of EINTR.
static void poll_notify_poke(int fd) { ... write(fd, "", 1); ... }
```

The failure this prevents is not "the signal was lost" but something subtler:
the signal was *merged*. POSIX signals are a set, not a queue, so two `SIGUSR1`s
delivered close together may arrive as one — and if the one that arrives is
consumed as a TLB shootdown, the guest signal has no effect at all. The host
`kevent` then runs to its timeout and returns 0, and a guest `poll` that should
have returned `EINTR` returns "nothing happened".

A byte written to a pipe is not coalesced with anything. When a wake must not be
lost, it goes on a channel that counts.

> **Measured, and still unexplained**
>
> There is an open anomaly in this area, and it is recorded here rather than
> smoothed over. `task_start` creates every task thread with `SIGUSR1` and
> `SIGUSR2` blocked, and the thread unblocks them itself once its thread-local
> storage exists. Measurement says some task threads nonetheless end up with
> `SIGUSR2` missing from their mask — proved with a `pthread_sigmask(..., NULL,
> &m)` poll loop at thread entry and a non-TLS ring buffer of handler entries
> keyed by `pthread_self()`.
>
> A standalone reproduction with the same `pthread_attr` (detached, 4 MB stack,
> the same QoS class) and 1,600 threads doing the same immediate `pthread_kill`
> pokes produced zero anomalies across three runs. So it is something about
> AOK rather than about Darwin thread creation, and it has not been explained.
>
> The operational rule it produced is the useful part: **never write a fix whose
> safety argument is "the wake signals are blocked here".** Guard on state the
> thread itself sets, not on the mask.

## 12.5 Delivering: four ABIs, four signal frames

Running a handler means building a stack frame the guest's libc will recognize,
because `sigreturn` is implemented *by the guest* — the handler returns into a
trampoline that makes a `sigreturn` syscall, and that syscall reads back the
frame the kernel wrote. Every field has to be where the guest expects it.

Four guests means four layouts, and `kernel/signal.c` pins them at compile time:

```c
static_assert(sizeof(struct amd64_siginfo_) == 128, "amd64 siginfo layout mismatch");
static_assert(sizeof(struct amd64_fpstate_) == 512, "amd64 fpstate layout mismatch");
static_assert(sizeof(struct arm64_fpsimd_context_) == 528, "arm64 fpsimd_context layout mismatch");
static_assert(sizeof(struct riscv64_mcontext_) == 784, "riscv64 sigcontext size");
static_assert(sizeof(struct riscv64_ucontext_) == 960, "riscv64 ucontext size");
static_assert(offsetof(struct rt_sigframe_riscv64, uc) == 128, "riscv64 frame uc offset");
```

These are the cheapest tests in the project. A structure that drifts by four
bytes produces a guest that resumes with a corrupted register file after every
signal — an intermittent, unattributable disaster — and a `static_assert`
catches it at build time, in the right file, with a message naming the
structure.

The conversion functions around them (`siginfo_to_i386_user`,
`siginfo_to_amd64_user`, and their riscv64 and arm64 equivalents) are the
per-ABI second copies Chapter 11 warned about, and they are exactly the case
where a shared body is impossible: the structures genuinely differ.

## 12.6 Restarting what the signal interrupted

A signal that arrives while a task is blocked in a syscall leaves a question:
when the handler returns, does the syscall resume, or does it fail with `EINTR`?

Linux answers with a small taxonomy, and AOK implements it:

- `ERESTART` (Linux calls it `ERESTARTSYS`; AOK spells it `_ERESTART` in
  `kernel/errno.h`) — restart if the handler was installed with `SA_RESTART`,
  otherwise `EINTR`.
- `ERESTART_NOHAND` — restart across a job-control stop, but **never** across a
  handler. `poll`, `select` and `epoll_wait` use this: a stopped-and-continued
  process resumes its wait, but a process that ran a handler gets `EINTR`.

The second one needs machinery, because "cancel the restart because a handler is
about to run" is a decision made in a different place from the one that set it
up:

```c
if (current->restart_nohand_pending) {
    current->restart_nohand_pending = false;
    current->poll_restart_valid = false;
    current->sleep_restart_valid = false;
    cancel_syscall_restart();
}
```

There is a second subtlety underneath. A restarted *timed* wait must not restart
its timeout. A `poll` with a 5-second timeout, stopped after 4 seconds and
continued, has 1 second left — not 5. Linux keeps a `restart_block` for this;
AOK keeps `poll_restart_deadline` and `sleep_restart_deadline` on the task, set
only when such a wait returns `ERESTART_NOHAND` and consumed by the re-executed
syscall, which is necessarily the very next one the task makes.

## 12.7 Job control

Job control is the part of the terminal that decides who is allowed to speak.

A `struct tty` carries a session and a foreground process group. The rules, as
implemented in `tty_check_change`, follow Linux's exactly:

> A process in the terminal's foreground group always may. A BACKGROUND process
> is stopped instead — `SIGTTIN` for reading, `SIGTTOU` for writing or changing
> settings.

with three exceptions taken in Linux's order, of which the interesting one is
asymmetric: a process that has `SIGTTOU` blocked or ignored simply proceeds,
because there is nothing to stop it with, but the same cannot apply to `SIGTTIN`
— there is no way to read from a terminal you are not the foreground of. A
background write and a background read are not mirror images.

The write side has a second asymmetry, and a bug behind it:

> `SIGTTOU`, but only when the terminal asks for it with `TOSTOP` — unlike the
> read side, where `SIGTTIN` is unconditional. There was no check here at all,
> so a background job scribbled over the foreground one's screen.

> **The bug that taught us this**
>
> Job control has a permission exception, and AOK had the rule without it.
>
> Linux's `check_kill_permission` normally requires matching credentials, but
> lets `SIGCONT` reach any process in the same **session** whatever its
> credentials — and job control is built on that exception. A shell that starts
> a privileged job keeps the stopped process in its session but not under its
> uid, so without the exception `fg` could not resume anything that had gone
> through `sudo`, and `kill_group` inherited the same refusal for the whole
> group.
>
> The check existed in two copies, which is Chapter 11's rule arriving again;
> they are one function now.

Input drives the rest: `^C` sends `SIGINT` to the foreground group, `^Z` sends
`SIGTSTP`, `^\` sends `SIGQUIT`, and the group's `stopped` flag — the
`_Atomic bool` of Chapter 10, read locklessly on every interrupt-return — is
what makes every thread of the group park until `SIGCONT`.

Two details in `struct tty` are worth quoting because they are the kind of thing
that is only right if somebody measured it.

**Flow control has two flags, not one.** `stopped` gates writes; `tco_stopped`
records that `tcflow(TCOOFF)` was what stopped them, "because `TCOON` restarts
only output it stopped itself — a `^S` is cleared by `^Q`, never by `tcflow`.
Both measured against Linux 6.12."

**Hangup is a generation, not a flag.**

> Bumped by every hangup. A descriptor records this at open and is hung up only
> if the tty has been hung up SINCE — which is what a hangup means on Linux: it
> belongs to the descriptors that were open at the time, and a fresh open of the
> same terminal gets a working one. Modelling it as a single sticky flag made a
> hung-up console permanently dead.

That is a small correction with a large symptom: a system console that stopped
producing a login prompt and never recovered.

## 12.8 ptrace, and the same bug twice

`ptrace` is where the process model, the signal model, and the scheduler all
have to be right at once, which is why it finds bugs nothing else does. A tracer
here is just another guest task; a tracee carries a small sub-struct with its
own lock and condition variable, recording whether it is traced, whether it is
stopped, whether it was attached via `PTRACE_SEIZE` or classically, which
options are set, and what signal the tracer has injected.

Several of those fields exist for a specific compatibility reason. `seized`
determines how a job-control group-stop is reported: a seized tracee gets a
`PTRACE_EVENT_STOP` event-stop, which `strace -f` recognizes and resumes with
`PTRACE_CONT(0)`, while a classic tracee gets a plain signal-stop carrying the
stop signal. `deliver_sig` records a signal the tracer injected, which must be
*delivered* on the next receive rather than re-trapped through
`signal_delivery_stop` — otherwise an injected signal loops forever.

`PTRACE_ATTACH` — the request `gdb -p` and `strace -p` use — was missing until
this cycle, and fell through to the default arm's `EPERM`, so a debugger could
not be pointed at a running guest process at all. It shares its implementation
with `SEIZE` and differs in two ways the rest of the sub-struct keys off. It
takes no options, so `seized` stays false and the tracee reports group-stops
classically; and it *stops* the tracee, by sending a `SIGSTOP` — after
`ptrace.traced` is set, so the stop is reported to the tracer rather than being
an ordinary job-control stop — which the tracer collects from its first `wait`.
`PTRACE_INTERRUPT` is now gated on `seized` the way Linux gates it: an attached
tracee gets `EIO`. `gdb -p <pid>` against a running guest process attaches,
walks the stack, evaluates and detaches as a result.

> **The bug that taught us this**
>
> `ptrace_group_stop` began failing in the arm64 suite the moment an unrelated
> gadget landed: `FAIL timeout (no group-stop report / never resumed)`. It
> passed standalone and failed in the suite — the exact signature of a load
> flake, and the project has a documented one, so the temptation to file it as
> such was real.
>
> What separated them was not more re-runs. It was one A/B: flipping the single
> constant the unrelated change touched made it pass, flipping it back made it
> fail, in one build, both directions. Then `lldb -p` on the hung process and
> `thread backtrace all`, which named both sides in one shot — the tracer parked
> in `do_wait`, and the tracee parked in the **untraced** job-control wait.
>
> That second frame is the whole answer. `handle_interrupt` tested
> `current->ptrace.traced` once, on entry, and only then fell into the plain
> job-control wait. `PTRACE_SEIZE` sets `traced` from the *tracer's* thread and
> does not wake the tracee. So a tracee that reached `raise(SIGSTOP)` before its
> tracer reached `ptrace()` parked in a wait that nothing would ever notify,
> never noticed it had become traced, and never reported the group-stop. The
> tracer's `wait4` then waited for a report that could not arrive.
>
> Nothing synchronizes those two events, so the losing order was always
> reachable — the parent had simply always won. A gadget change shifted musl's
> `memset` timing enough to flip it, every time.
>
> Linux handles the same race from the other side: `ptrace_attach` explicitly
> wakes a stopped tracee so it can re-enter the trap and report. AOK now does
> both — `PTRACE_SEIZE` notifies the group's `stopped_cond` when the tracee it
> just seized is already stopped, and the group-stop wait re-checks
> `ptrace.traced` on every pass instead of once on entry.
>
> The test that guards it **forces** the losing order with
> `waitpid(WUNTRACED)` rather than racing for it. A test that only fails when
> the scheduler cooperates would have reported this fixed while it was still
> broken.

And then the same bug again, in the other delivery point.

> **The bug that taught us this**
>
> Found by reading the entry above rather than by a failure report, and
> confirmed by running it: `strace` attached to a program AOK runs as **host
> code** hung the moment that program was `^Z`'d. No stop report ever reached
> the tracer, so its `wait4` waited forever.
>
> `native_checkpoint` carried its own group-stop wait, whose comment said it
> mirrored `handle_interrupt` and which did not:
>
> ```c
> struct tgroup *group = current->group;
> if (group->stopped) {
>     lock(&group->lock, 0);
>     while (group->stopped)
>         wait_for_ignore_signals(&group->stopped_cond, &group->lock, NULL);
>     unlock(&group->lock);
> }
> ```
>
> No `ptrace_group_stop()` on the way in, so the stop was invisible to the
> tracer; and no re-check of `ptrace.traced` while parked, so the
> attach-after-stop order was unreachable too. Both attach orders hung.
>
> It was fixed by **deleting the second copy rather than repairing it**. The
> group-stop wait now lives once, as `group_stop_wait()` in `kernel/signal.c`,
> and both `handle_interrupt` and `native_checkpoint` call it. Copy-and-drift is
> what produced the bug — the native copy was written from the translated one
> and then never tracked its ptrace fixes — so one function is the fix for the
> class rather than for the instance. The native path also gained the
> `continued` notification it had never had, which wakes a parent blocked in
> `wait4(WCONTINUED)`.

Two details of the test written for that second bug deserve to outlive it,
because they are about how to prove a resumption happened at all:

- The victim writes into a pipe forever. The first read proves the program is
  really running, so the stop lands inside it rather than somewhere in `exec`;
  and draining the pipe *while the child is stopped* makes a later successful
  read positive proof that it was **resumed**. "It died when I killed it" proves
  nothing — `SIGKILL` reaps a stopped task too.
- The watchdog names the case it timed out in. Under the suite, stdout is
  block-buffered and a signal handler cannot safely flush it, so without that a
  hang reports only "timeout" and every log line is lost — including the one
  saying whether it was the control or the subject that hung. The first A/B run
  was misread for exactly that reason.

## 12.9 What this costs the rest of Part III

Everything that blocks has to satisfy three obligations that come from this
chapter.

It must be **interruptible** — parked on something a signal can break, which in
practice means a condition variable that the wake path knows about, or a host
call paired with a notify pipe.

It must **record what to do afterwards** — restart or `EINTR`, and for a timed
wait, the deadline it already had.

And it must be **reachable from another thread**, through a wake that cannot be
coalesced away.

Chapter 14's readiness primitives look more complicated than `poll` has any
right to be. Those three obligations are why.

---

*Anchors:* [kernel/signal.c](../../kernel/signal.c),
[kernel/signal.h](../../kernel/signal.h), [kernel/ptrace.c](../../kernel/ptrace.c),
[kernel/calls.c](../../kernel/calls.c) (`handle_interrupt`'s group-stop path),
[kernel/native.c](../../kernel/native.c) (`native_checkpoint`),
[fs/tty.c](../../fs/tty.c), [fs/tty.h](../../fs/tty.h),
[util/sync.c](../../util/sync.c) (`signal_thread_unwedge_wake_sigs`),
`tests/manual/ptrace_group_stop.c`, `tests/manual/native_ptrace_group_stop.c`,
`tests/manual/tty_hangup_reopen.c`, [docs/TODO.md](../../docs/TODO.md).

*Story:* `PTRACE_SEIZE` of an already-stopped tracee hanging forever — a race
that was always reachable and never lost until an unrelated gadget shifted
musl's `memset` timing, and the same bug living a second time in the native
path's copy of the group-stop wait.
