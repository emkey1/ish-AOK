# Linux native AIO for iSH-AOK

Plan, not a change. Prompted by MariaDB 11.8, which crash-loops on install:
`io_setup` is a stub returning ENOSYS, its thread pool does not check the
nullptr that comes back, and it dereferences it. docs/TODO.md carries the
diagnosis; this is what implementing the family would look like.

## The decision that halves the work

**Do the I/O synchronously inside `io_submit`.**

`io_submit` is specified as asynchronous, but nothing requires the completion
to arrive later -- an event that is already in the queue when `io_getevents` is
called is a valid outcome, and Linux itself completes buffered file I/O inline
in many cases. Doing the read or write in the submitting task, then queueing
the finished event, removes the entire worker pool and every cross-thread
completion race with it. What is left is one lock per context.

The cost is real and belongs in the open, because it is the main risk here:
`io_submit` blocks for the duration of the I/O where a Linux caller expects it
to return at once. MariaDB submits from a pool of I/O threads and reaps from
another, so blocking serialises its I/O rather than breaking it. A caller that
submits from an event loop it cannot afford to block would notice. If that
turns up, phase 2 below is the answer, and it is additive.

Everything below is phase 1 unless marked otherwise.

## The linchpin: what `io_setup` hands back

`aio_context_t` is not opaque in practice. libaio -- which mariadbd links
(`libaio.so.1t64`) -- treats it as a pointer to `struct aio_ring` and reads it
in userspace before deciding whether to make a syscall:

```c
static int aio_ring_is_empty(io_context_t ctx, struct timespec *to) {
    struct aio_ring *ring = (struct aio_ring *) ctx;
    if (!ring || ring->magic != AIO_RING_MAGIC) return 0;  /* -> use the syscall */
    if (ring->head == ring->tail) return 1;                /* -> return 0 events */
    return 0;
}
```

So the context has to be a **readable guest address**, or libaio faults where
the kernel would have returned an error. It does *not* have to be a working
ring: `AIO_RING_MAGIC` is `0xa10a10a1`, and a zeroed page fails that test, so
every libaio caller falls through to the syscall by construction.

`io_setup` therefore allocates one anonymous guest page through
`mmap_common_guest()` (kernel/mmap.c), leaves it zeroed, and returns its
address as the context id. That single choice is what makes a syscall-only
implementation sufficient instead of a shared-ring one, and it is the piece
worth getting right first.

Consequence to keep in mind: the page is in the guest's address space, so the
guest can scribble on it. Nothing may be trusted from it -- it is a handle and
a decoy, and the real state lives kernel-side keyed by that address.

## Per-ABI coverage, which is currently uneven

| ABI | numbers | today |
|---|---|---|
| arm64, riscv64 (asm-generic) | 0 setup, 1 destroy, 2 submit, 3 cancel, 4 getevents | all five stubbed to ENOSYS |
| i386 | 245-249 | **only 245 (`io_setup`)**; 246-249 absent |
| amd64 | 206-210 | **none** |

Absent is worse than stubbed: a missing entry is a "missing syscall" SIGSYS
kill, not an errno. So an amd64 guest running anything that touches AIO dies
without even the log line that made MariaDB diagnosable on arm64. Filling all
three tables is part of this work regardless of how far the implementation
goes, and is worth doing on its own.

## Structures

```
struct aio_ctx {
    guest_addr_t id;          // the decoy page; also the lookup key
    unsigned max_events;
    lock_t lock;
    cond_t cond;              // a reaper waits here for min_nr
    struct io_event_ *events; // ring of completed events, max_events long
    unsigned head, tail, count;
    struct aio_ctx *next;     // per-MM list, see lifetime
};
```

Contexts hang off the memory space (`current->mem`), not the task: `io_setup`
in one thread and `io_getevents` in another is the normal pattern, and threads
share an mm. That also gives the lifetime rule for free -- when the mm goes, so
do its contexts.

## Per call

- **`io_setup(nr_events, ctx_idp)`** -- validate `nr_events`, allocate the
  decoy page, allocate the context and its event ring, link it, `user_put` the
  address. `EAGAIN` if a per-mm limit is exceeded (Linux uses `aio-max-nr`),
  `EINVAL` if `*ctx_idp` is not already zero, which Linux checks and callers
  rely on.
- **`io_submit(ctx, nr, iocbpp)`** -- read `nr` guest pointers, then each
  `struct iocb`. For each: resolve the fd, do the operation, push an
  `io_event`. Returns the number submitted; a failure on the *first* iocb is
  an error return, a failure on a later one is a short count. That asymmetry
  is in the man page and callers depend on it.
- **`io_getevents(ctx, min_nr, nr, events, timeout)`** -- drain up to `nr`,
  waiting on `cond` until `count >= min_nr` or the timeout expires. Must be
  interruptible: `wait_for` returning `_EINTR` becomes `EINTR` with whatever
  was already copied.
- **`io_destroy(ctx)`** -- unlink, free the ring, unmap the decoy page.
- **`io_cancel`** -- `EINVAL`. Linux almost never manages to cancel a
  submitted iocb either, and every caller has to handle the failure.

Operations to support: `IOCB_CMD_PREAD`, `PWRITE`, `PREADV`, `PWRITEV`,
`FSYNC`, `FDSYNC`. Anything else gets `EINVAL` in the event's `res`, not a
submit failure -- that is where Linux puts it.

The I/O itself reuses what kernel/fs.c already does for pread/pwrite:
`f_get`, `task_may_block_start()`, `fd->ops->pread` / `->pwrite`. No new I/O
path, which is most of why this is small.

## Completion notification

`IOCB_FLAG_RESFD` names an eventfd in `iocb->aio_resfd`. On completion, add 1
to it. kernel/eventfd.c already does exactly this internally --
`fd->eventfd.val += n; notify(&fd->cond); poll_wakeup(fd, POLL_READ);` -- so
this is a few lines against an existing fd, not new machinery. It is also the
half MariaDB's thread pool actually waits on, so it is not optional.

## Marshalling

The only genuinely fiddly part, and where the bugs will be.

`struct iocb` is 64 bytes and `struct io_event` 32 on 64-bit; both differ on
i386, where the pointer-shaped fields narrow. Follow the split this codebase
already uses 30 times over -- `guest_abi_is_64bit(current->abi)` with a struct
per shape, as `siginfo_to_user` does -- rather than a packed struct with
conditional field widths.

`aio_data` is an opaque u64 the caller gets back in the event; it must survive
the round trip byte for byte on every ABI. That is the one field where a
marshalling bug is invisible in testing and fatal in use, because callers key
their own state off it.

## Test

`tests/manual/aio_basic.c`, in the tier0 set:

1. `io_setup`, then `io_submit` a PWRITE, reap it, check `res` is the byte
   count and `data` came back unchanged.
2. PREAD it back, compare bytes.
3. FSYNC.
4. An iocb with a bad fd: submit succeeds, the *event* carries the error.
5. `IOCB_FLAG_RESFD`: the eventfd reads 1 after a completion.
6. `min_nr` blocking: reap 2 when only 1 is queued, with a timeout, and check
   it waits and then returns 1.
7. `io_destroy`, then `io_getevents` on the dead context returns `EINVAL`.

Run it on i386 and amd64 as well as arm64 -- the marshalling is the risk, and
only a 32-bit run exercises the narrow shape.

## Phasing

**Phase 0** -- fill the amd64 and i386 tables so every ABI gets an errno
instead of SIGSYS. Small, independent, worth landing on its own.

**Phase 1** -- everything above. Synchronous submit. ~450-550 lines in a new
kernel/aio.c plus ~200 of test.

**Phase 2, only if something needs it** -- real asynchrony: a worker per
context, submit returns immediately, completions arrive from the worker. The
structures above do not change; `io_submit` stops doing the work and queues it
instead. Deferred deliberately, because it buys nothing measurable for MariaDB
and brings the concurrency risk this design currently does not have.

## Risks, in order

1. **`io_submit` blocking.** Discussed above. Known, bounded, phase 2 exists.
2. **32-bit marshalling.** Mitigated by testing on i386, not by care.
   *(This one landed backwards -- see "What implementation changed" below.
   i386 was fine; amd64 was the fatal one.)*
3. **The decoy page being written by the guest.** Nothing may be read back
   from it; treat it as a handle only.
4. **A context outliving its mm.** Tie the list to the mm and free with it;
   do not key contexts on the task.

## What it is worth

MariaDB is the reporter, not the constituency. nginx's `aio`, PostgreSQL's
`io_method=aio`, RocksDB and anything else that assumes native AIO exists on
Linux hit the same stub. MariaDB's failure -- a null dereference that reads as
a hang -- is the worst of the class, but the others are only better by
accident.

## What implementation changed

Three things the plan had wrong, all found by running the test rather than by
reading the code.

**The marshalling risk was on the 64-bit ABIs, not i386.** An `aio_context_t`
is the address of the ring page, so every call after `io_setup` carries a real
64-bit pointer in argument 0. amd64's legacy marshaller validates that an
argument fits a dword and SIGSYS-kills the task when it does not -- which it
did, to `io_submit`, at `0x7ffffdfff000`. That refusal was correct: silently
truncating an opaque handle is worse. The fix is native full-width dispatch
(`handle_amd64_native_memory_syscall` for 206-210,
`handle_asm_generic_native_syscall` for 0-4), with the dword forms kept only
for i386, where the whole address space fits in a dword and widening is
lossless. Narrowing the declared arity was the first attempt and did nothing.

**arm64's table entries were dead on arrival.** Syscalls 0-4 were already in
the asm-generic handler's "clean ENOSYS" list, which runs before the table --
so wiring `arm64_syscall_table` changed nothing at all, silently. Worth
remembering: on the asm-generic ABIs a table entry is not evidence that a
syscall is reachable.

**A dword errno widened into a 4GB success.** Every `sys_*` the submit path
calls returns `dword_t`, so `-EBADF` arrives as `0xfffffff7`; assigning that
to a 64-bit `io_event.res` without sign-extending turns an error into a
~4-billion-byte completed transfer. This is the failure mode with real
consequences -- InnoDB reads `res` as the byte count and would conclude a
write it never made had succeeded. Hence `aio_widen`.

**And a fourth, found by writing the concurrency test rather than by running
it.** `io_destroy` unlinked the context and freed it on the spot, reasoning
that synchronous submission means nothing is in flight. That is true and it is
not the question: a thread parked in `io_getevents` is holding the context's
lock and cond, and another inside `io_submit` is holding the context itself.
Freeing it under either is a use-after-free. Contexts are reference counted
now -- the list holds one, every caller holds one for the length of its call,
and a destroy marks the context dead and wakes its reapers so they return
EINVAL instead of waiting on a ring nothing will ever post to.

Worth being straight about the evidence: tests/manual/aio_threads.c pins down
that contract, but run it against the old code and it still passes, because
`cond_destroy` happens to wake the waiters and freed memory happens to still be
readable. The obvious detector is not available either -- an AddressSanitizer
build of the emulator dies inside ASan's own poisoning code before any guest
starts. So this one rests on reading the code, and the test is a regression
test for the behaviour rather than a reproduction of the bug.

One deliberate change of semantics came out of the same pass: Linux validates
an iocb in `io_submit` (`aio_prep_rw`'s `fget`, the FMODE checks, the opcode
switch) and returns the error from the submit, queueing no event. The first
implementation accepted everything and reported failures through the event
instead. That is not a cosmetic difference -- a caller told "1 submitted" waits
for exactly one event -- so submit-time validation now matches the kernel, and
only errors the transfer itself hits reach `io_event.res`.

Verified on all four guest ABIs (i386, amd64, arm64, riscv64) with the raw
syscalls, and end-to-end on amd64 and arm64 against real libaio 0.3.113 --
including `io_getevents` with a zero timeout, which is the path that reads the
ring in userspace and is where a plausible-but-wrong context id would hang.
