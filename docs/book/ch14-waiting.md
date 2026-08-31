# 14. Waiting

Every interesting thing a program does eventually involves waiting. A mutex is
contended; a pipe is empty; a child has not exited; a socket has no data; a
timer has not fired. Linux offers a dozen ways to express that, and a guest
running real software uses all of them.

Chapter 12 established what a blocking call owes: it must be interruptible, it
must know whether to restart, and it must be reachable from another thread by a
wake that cannot be lost. This chapter is what those obligations look like once
they are implemented a dozen times over, and it has an unusually high density of
bugs per page — because in every one of these subsystems, the hard half is never
the waiting. It is the waking.

## 14.1 Futexes

A futex is a design that is mostly *not* in the kernel. The fast path — an
uncontended lock — is an atomic compare-and-swap in userspace, and the kernel is
only involved when a thread has to sleep or when a sleeper has to be woken. That
is why every threaded program on Linux is, underneath, a futex program: pthread
mutexes, condition variables, semaphores, Go's scheduler, Rust's `std::sync`,
Java's monitors.

AOK implements the operation set on a hash table of wait queues keyed by guest
address. The interesting parts are all in the corners.

> **The bug that taught us this**
>
> `FUTEX_CMP_REQUEUE` — the operation `pthread_cond_broadcast` uses to move
> waiters from a condition variable's queue to the mutex's queue without
> thundering — was wrong in three ways at once.
>
> It compared `*uaddr` against `val`, the number of waiters to *wake*, instead
> of against `val3`, the expected value. So an ordinary caller got a spurious
> `EAGAIN`, while a caller whose word happened to equal the wake count sailed
> past the check that exists to stop it.
>
> It woke nobody and only requeued, so a broadcast lost every wakeup it was
> meant to deliver.
>
> And it returned only the requeued count, where Linux returns woken plus
> requeued.
>
> The part worth sitting with: **current musl is unaffected**, because it uses
> plain `FUTEX_REQUEUE`. No guest program on the platform exercised the broken
> path, so nothing failed, no report was filed, and the only way this was ever
> going to be found was somebody deliberately writing a probe for an operation
> nothing currently calls. Some bugs cannot be found by using the system.

Robust futexes tell an even better story, because there the bug was hidden
behind another bug.

A robust mutex is one whose owner might die while holding it. The owning thread
registers a linked list of held locks with `set_robust_list`, and the kernel
promises that if the thread dies, it will walk that list, mark each lock as
having a dead owner, and wake a waiter. That promise is the entire feature.

AOK never walked the list. A thread that died holding a robust mutex left every
waiter blocked forever — precisely the outcome a robust mutex exists to prevent.

And nobody noticed, because of a second, smaller defect in front of it:
`get_robust_list(pid = 0)` returned `EPERM`. Zero means "the calling thread"
everywhere else in the API, but it was looked up like any other pid, and pid 0
is never allocated, so the lookup came back as "not you". musl gates *all*
robust-mutex support on exactly that probe succeeding once — so no musl program
on AOK could create a robust mutex at all, which made the missing death handling
unreachable, and therefore invisible.

The fix does what Linux does: `futex_exit_robust_list` walks the list on the
dying thread, before its address space goes away, writes `FUTEX_OWNER_DIED` into
each lock word it still owns, clears the TID, preserves `FUTEX_WAITERS`, and
wakes one waiter if there was one — bounded by Linux's `ROBUST_LIST_LIMIT`, with
the `list_op_pending` entry handled last, as Linux does.

The test drives the raw robust-list protocol as well as the pthread layer,
deliberately: the raw half tests the kernel, and the pthread half tests only
whatever the C library decided to support.

One more futex detail belongs here because it is where Chapter 12 meets this
chapter. A `FUTEX_WAIT` interrupted by a signal whose handler restarts the
syscall has a window: the waiter is off the queue during the handler and the
restart. If a `FUTEX_WAKE` arrives in that window, it wakes nobody and is lost,
and the restarted wait blocks forever on a wakeup that already happened. So the
waiter dequeues but **pins** the futex — holding a reference that keeps the
object and its `wake_seq` counter alive — and snapshots the counter. If
`wake_seq` moved while it was away, the restarted wait treats that as its wakeup
rather than sleeping.

## 14.2 Readiness, and the day `sudo` segfaulted

`poll`, `select` and `epoll` all answer the same question: which of these
descriptors is ready? Underneath, AOK has a poll object per waiter, a
registration per descriptor, and — for descriptors backed by host file
descriptors — a `kqueue`.

That last part is where the trouble comes from, because Darwin's readiness model
and Linux's are not the same model.

> **The bug that taught us this**
>
> Darwin's `kqueue` implements no filter at all for character devices other than
> ttys, or for directories. Its `poll(2)` answers `POLLNVAL` for them rather
> than a readiness. AOK passed both of those straight through to the guest.
>
> The consequences split. Registering such an fd failed the *entire*
> `poll`/`select`/`epoll_ctl` with `EINVAL`. Where it did not fail, the fd was
> reported permanently not-ready, so a blocking wait sat there until its
> timeout.
>
> Linux has no such category. A file with no `->poll` method is polled through
> `DEFAULT_POLLMASK` and is **always readable and always writable, never
> POLLPRI**, whatever its type and whatever access mode it was opened with —
> measured identical on Linux 6.12 for regular files, `/dev/null`, `/dev/zero`,
> `/dev/full` and directories.
>
> And the `EINVAL` was not a quiet wrong answer. musl's secure-execution startup
> polls fds 0, 1 and 2, and calls `a_crash()` — a deliberate store to address
> zero — if that poll fails. So **every setuid or setgid binary died with
> `SIGSEGV` before `main()`** whenever any of its standard descriptors was a
> host device node.
>
> `sudo something >/dev/null` was a segfault. And the fault address in the crash
> report pointed at `ld-musl` rather than at anything of ours, so the report
> named the victim rather than the cause.

Regular files were wrong in the other direction at the same time: AOK asked the
host, forwarded Darwin's spurious `POLLPRI`, and gated the request on the open
access mode — so an `O_RDONLY` file came back readable but not writable, where
Linux says both.

The rule that came out of it is the one this book keeps arriving at from
different directions: **the host's answer is not automatically the right
answer.** The guest's contract is Linux's contract, and where the host has no
opinion, or a different one, the emulator has to supply Linux's.

## 14.3 A deadlock across three subsystems

The single hardest bug in this area was reported as "`pidfd_epoll_deadlock`
intermittently hangs and exits 137". It was two unrelated bugs wearing one
report, which is a shape worth recognizing: 40 runs produced 8 exits with status
137 and no hang; a later 24-run batch produced 6 kills and 1 hang.

**Exit 137 was not the guest.** `cli_halt` maps a signalled init to `128 + signo`
(Chapter 1), so 137 reads as "the guest was SIGKILLed". The *host* process was.
Every run left a crash report, and all fourteen had byte-for-byte the same
stack:

```
_os_unfair_lock_recursive_abort <- malloc <- _tlv_get_addr
  <- sigusr2_handler <- _sigtramp <- malloc <- _tlv_get_addr <- task_thread
```

A wake poke landed on a brand-new task thread while it was inside
`_tlv_get_addr`, instantiating `current` — the first `__thread` access on that
thread, which on Darwin calls `malloc`. The signal handler's own `__thread` read
re-entered `malloc` while it held its lock, and libplatform kills the process
for that.

`task_start` blocks both wake signals across `pthread_create` precisely to
prevent this, and the mask really is inherited — instrumented, the creating
thread read the right mask in 1,000 of 1,000 creations. But 18 of 1,000 task
threads entered `task_thread` with `SIGUSR2` already missing from theirs. That
is the unexplained anomaly of Chapter 12, and the fix is the important part:

**it does not rely on the mask.** `signal_thread_locals_init` sets a pthread TSD
flag once the thread-local storage exists, and both handlers return immediately
unless the flag is set — reading a TSD slot cannot allocate, and a thread that
has not run the init has no task and nothing to interrupt anyway. Every thread
that runs guest work now calls it, including native-program threads, which had
the same latent hole.

There is a diagnostic trap in that story too, and it is a recurring one: because
stdout to a pipe is block-buffered, the test's already-completed "PASS" died in
the buffer along with the process. The run looked like a failure of the thing
being tested rather than a crash after it had passed.

**The hang was an AB-BA cycle across three locks**, and the `pidfd` path — the
thing the test was named after — was innocent. Sampled live:

| | holds | wants |
|---|---|---|
| A `do_exit` | `pids_lock` | → `signalfd_wakeup_task` → `fdtable_release` → `files->lock` |
| B `close(2)` | `files->lock` | → `epoll_close` → `poll_destroy` → `poll->lock` |
| C `epoll_wait`/`ctl` | `poll->lock` | → `pidfd_poll` → `pids_lock` |

`signalfd_wakeup_task` is written *not to block*: it trylocks the file
descriptor table and gives up when it is busy. But its give-up path called
`fdtable_release()`, and that took `table->lock` unconditionally — so the
function that had just declined to wait for that lock immediately waited for it,
with `pids_lock` still in hand.

The fix is a small correction with a general shape: `refcount` is already
atomic, so the lock was never what made the decrement safe. It now guards only
the final teardown, which by definition runs when no other reference remains.

> **How a race fix gets proved**
>
> "It stopped reproducing" is not evidence, because it never reproduced
> reliably. What was done instead: build a version with a 1.5 ms delay inserted
> in `poll_destroy` to widen the window, and A/B against it.
>
> **2 hangs in 60 runs without the fix, 0 in 100 with it.** Then, unwidened,
> 252 runs of the real binary with no kill and no hang.
>
> Widening the window turns a rare race into a testable one. It is the single
> most useful technique in this chapter, and it costs one `usleep`.

## 14.4 The rest of the family

The readiness subsystem has grown a descriptor type for nearly everything, and
each brings its own hazard.

**`eventfd`** is a counter you can poll. Simple, and the one to reach for when a
subsystem needs to wake a poller.

**`signalfd`** turns signals into readable bytes. Its constraint is the one
Section 14.3 exposed: the code that wakes a `signalfd` runs from signal delivery,
which runs under `sighand->lock` and sometimes under `pids_lock`, so it may not
block on anything.

**`timerfd`** is a clock you can poll.

**`pidfd`** is a process you can poll, and Chapter 10 covered its subtlety: the
reference it holds must keep the task's memory alive without gating the task's
exit, or every systemd service tracked by pidfd hangs. A related fix in the same
area: `pidfd_open` used to refuse a zombie, which is exactly the process a
supervisor most wants a handle on.

**`inotify`** is a directory you can poll, and it is the clearest example in the
tree of flags that were accepted and ignored:

- `IN_MASK_ADD` ORs new events into an existing watch's mask — "that is the
  entire reason the flag exists". Ignored, a second add silently discarded what
  the first was watching for, so a caller widening a watch quietly narrowed it.
- `IN_ONESHOT` means "tell me once, then forget me". Ignored, the watch fired
  forever and never sent the `IN_IGNORED` that tells a reader the watch
  descriptor is dead, so a program that installed a one-shot watch and moved on
  kept receiving events for a descriptor it believed was gone.

And the queue behind all of it had no limit at all. A watched directory under
churn plus a reader that stalls grows the heap without bound — which in a
process with a jetsam budget (Chapter 13) is not merely untidy, it is a way for
a guest to kill the application. It is now capped at
`fs.inotify.max_queued_events`, with one synthetic `IN_Q_OVERFLOW` event
appended per overflow episode, which as the commit notes "is the only honest
thing available — the missed events cannot be recovered, so the reader is told
to rescan".

That is the honest-signal principle again, and inotify is where Linux itself
applies it: when you cannot deliver the truth, deliver an accurate statement
that the truth is missing.

## 14.5 The older primitives

**SysV semaphores and message queues** are implemented rather than delegated,
because the guest is fake-root and the host's SysV namespace is shared with
every other process on the device. Two guests using the same key must find each
other's semaphore, and must not find Xcode's.

**Linux AIO** — `io_setup`, `io_submit`, `io_getevents` — is implemented on top
of a worker pool, because Darwin has no equivalent. `io_uring`, by contrast, is
deliberately absent and returns `ENOSYS` silently (Chapter 11), because liburing
and cmake both fall back to epoll cleanly when told no.

**Timers and clocks** round it out: `times()`, `nanosleep` with its validation
rules, per-process and per-thread CPU clocks, `alarm`, POSIX timers, and the
interval timers whose CPU-time variants are driven by a sampler because the
underlying timer subsystem only offers monotonic and realtime clocks
(Chapter 10).

## 14.6 The pattern

Every subsystem in this chapter is the same two-part construction: a thing to
wait on, and a thing to wake it. In every single case, the bugs were in the
second half.

`FUTEX_CMP_REQUEUE` requeued and forgot to wake. Robust lists never woke the
waiters of a dead owner. `poll` reported permanently-not-ready and waited for a
timeout that was the only thing that would ever end it. `signalfd`'s wake path
blocked on a lock it had explicitly declined to wait for. inotify's queue grew
because nothing bounded what the waker could enqueue.

Waiting is easy: park on a condition variable. Waking is where the state
machine, the lock order, the signal mask, and the host's own idea of readiness
all have to agree at once — and that is why Chapter 36's debugging playbook
spends most of its time on threads that are asleep when they should not be.

---

*Anchors:* [kernel/futex.c](../../kernel/futex.c), [kernel/futex.h](../../kernel/futex.h),
[kernel/poll.c](../../kernel/poll.c), [fs/poll.c](../../fs/poll.c),
[kernel/epoll.c](../../kernel/epoll.c), [kernel/eventfd.c](../../kernel/eventfd.c),
[kernel/pidfd.c](../../kernel/pidfd.c), [kernel/inotify.c](../../kernel/inotify.c),
[kernel/aio.c](../../kernel/aio.c), [kernel/sysvsem.c](../../kernel/sysvsem.c),
[kernel/sysvmsg.c](../../kernel/sysvmsg.c), [kernel/time.c](../../kernel/time.c),
[kernel/signal.c](../../kernel/signal.c) (`signalfd_wakeup_task`),
`tests/manual/futex_robust_requeue.c`, `tests/manual/pidfd_epoll_deadlock.c`,
[docs/TODO.md](../../docs/TODO.md).

*Story:* `sudo something >/dev/null` segfaulting before `main` — because Darwin's
`poll` answers `POLLNVAL` for a device node, Linux answers always-ready, and
musl's secure-execution startup crashes deliberately when the poll of its
standard descriptors fails.
