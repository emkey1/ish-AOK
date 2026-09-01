# 20. FUSE

Every filesystem so far in this part has been AOK's: fakefs, procfs, tmpfs,
devfs, the read-only tree compiled into the binary. The guest gets what the
emulator implements.

FUSE inverts that. A FUSE filesystem is an ordinary guest program — compiled
inside the guest, against the distribution's own `libfuse`, by the guest's own
compiler — that the kernel then *asks* whenever anything touches its mount
point. `sshfs`, `rclone`, archive mounts, overlays, in-memory scratch
filesystems: none of them need porting, because none of them know they are not
on Linux.

It is the first place in this book where the guest extends the kernel rather
than the other way round.

## 20.1 The protocol, and the two file descriptors that matter

FUSE is a wire protocol over a character device. `/dev/fuse` is major 10, minor
229, and each `open` of it is one connection. A daemon opens it, then calls
`mount(2)` with `fd=<n>` in the options string, and from that moment every path
operation under the mount point becomes a request the daemon reads from that
descriptor and answers with a write.

AOK implements protocol version **7.31**, which covers both libfuse2 and
libfuse3 daemons. The structures are `linux/fuse.h`'s, with fixed layouts on
every guest architecture — which is worth a sentence given Chapter 12's four
different signal frames. FUSE's ABI is explicitly sized, so the same wire format
serves i386, amd64, arm64 and riscv64 without per-guest translation. Somebody
thought about that when they designed it.

The operation set is the familiar one: `LOOKUP`, `GETATTR`, `SETATTR`,
`READLINK`, `SYMLINK`, `MKNOD`, `MKDIR`, `UNLINK`, `RMDIR`, `RENAME`, `LINK`,
`OPEN`, `READ`, `WRITE`, and the rest.

## 20.2 The privilege that was already spent

On Linux, mounting is privileged, and FUSE's whole reason for existing is to let
unprivileged users provide filesystems. The reconciliation is `fusermount`, a
setuid-root helper that performs the mount on the daemon's behalf.

Here, the guest is fake-root (Chapter 17). `mount(2)` is AOK's own code and it
answers to whoever the guest says it is, so when a daemon runs as root, libfuse
opens `/dev/fuse`, calls `mount(2)` with `fd=<n>` itself, and no helper is
involved at all. The hardest part of deploying FUSE on a real system simply is
not a problem on this one.

The honest version has a second half, though, and the in-app documentation
states it because it is the usual reason a mount appears broken. AOK does not
remove `fusermount` from the picture; it removes it from the *root* case. A
session running as an ordinary user — which is what "Open Everything as Default
User" produces — takes libfuse's normal unprivileged path, and that path calls
`fusermount3`. The helper has to be installed. `/dev/fuse` being
world-accessible is not on its own enough.

There is a second trap in the same area, and it is not AOK's at all:

> A daemon that reports every file as owned by root, over a root-owned `0755`
> directory, will refuse a non-root caller's `create` with `EACCES` — correctly,
> and identically on real Linux.

The fix is for the daemon to report the mounting user in the
`user_id=`/`group_id=` mount options and in the `uid`/`gid` of each attribute
reply. It is worth including here because it is exactly the kind of failure that
gets reported as an emulator bug, and the emulator is doing precisely what Linux
does.

## 20.3 A path-based VFS meets a nodeid protocol

There is one real impedance mismatch. AOK's VFS is **path-based**: an operation
carries a string, and `path_normalize` (Chapter 16) resolves it. The FUSE
protocol is **nodeid-based**: the daemon hands out opaque 64-bit node
identifiers, and subsequent operations name a node rather than a path.

AOK bridges that in the simplest way available. Every operation resolves its
path with a `FUSE_LOOKUP` walk from the root node, component by component.
Nodeids are not cached between operations.

That is a real cost — an operation on a path five components deep is six
requests to the daemon rather than one — but it keeps node lifetimes trivial,
which matters because the other side of this boundary is arbitrary guest
software that may crash.

What a lookup creates, though, is a *reference*. Every nodeid the daemon hands
back carries a lookup count it is entitled to see returned, and AOK returns it:
`FUSE_FORGET` is sent for every node the resolver passes through, under one
rule stated at the top of `fuse_resolve`.

> A caller that asked for the nodeid owns the reference and must forget it when
> done — after the operation that names it, never before.

The ordering half of that rule is the part worth stating twice. `FORGET` goes
on the same queue as the request that used the node, behind it, so a daemon
never sees a node forgotten while an operation on it is still outstanding. In
`fusefs_close` that means the `RELEASE` is queued first and the `FORGET` after
it, on a node the daemon is still holding open.

Without this, a long-lived mount leaks daemon memory in exact proportion to how
much of the tree the guest has walked — which for something like `sshfs` over a
large remote tree is unbounded. It was one of the two things this chapter used
to list as absent.

Because AOK has no node cache, it forgets *promptly* rather than in bulk: the
regression test measures 67 `FORGET`s for a session where Linux sends 2. Both
are protocol-legal — a daemon must cope with whatever counts arrive — and the
test asserts the property that actually matters, which is that no node is ever
forgotten more times than it was handed out. Over-forgetting is a daemon
use-after-free; under-forgetting is only a leak.

## 20.4 `may_block`, and the classic FUSE deadlock

This is the load-bearing piece, and it connects directly to Chapter 16's
profiling.

`generic.c` and `stat.c` hold the global `inodes_lock` across filesystem calls.
That lock exists for fakefs: it is what keeps metadata and host-file state from
tearing apart under concurrent access, and Chapter 16 measured it at 880 ms of
aggregate wait on an open-heavy workload.

Now consider a FUSE call made while holding it. The call blocks on a userspace
daemon. That daemon is a guest process, and to answer the request it will
probably do filesystem work of its own — read a real file, stat something,
write a cache. Which needs `inodes_lock`. Which is held by the thread waiting
for the daemon's answer.

That is the classic FUSE deadlock, and it has taken down real Linux kernels in
its time.

The solution is a flag on the filesystem: `fs_ops.may_block`. A filesystem
marked that way has its operations run *without* `inodes_lock` held, guarded at
each site with `if (!fs_blocks)`. `fusefs` sets it.

The maintenance rule that comes with it is stated in the design notes and is
worth elevating, because it is a general hazard:

> Any NEW `inodes_lock`-wrapped fs call must repeat the guard.

A correctness property enforced by a condition that must be re-applied at every
future call site is a property that will eventually be forgotten. It is right
for now, it is documented, and it is the kind of invariant that deserves a test
or a structural change before the tenth call site rather than after it.

## 20.5 When the daemon goes away

A filesystem whose implementation is a user process can crash, be killed, or
simply exit. The lifetime rules are therefore more of this subsystem than the
operations are.

**Every request wait is interruptible.** A guest process blocked on a FUSE
request can be signalled, which follows Chapter 12's rule that everything
blocking must be.

**An interrupted or fire-and-forget request is marked abandoned and freed by
whichever side touches it last.** That is a clean ownership protocol for a
structure two threads may be finished with in either order — no "who frees it"
question, because the answer is always "the second one".

**A request the daemon has already read gets a `FUSE_INTERRUPT`.** One still
sitting on the pending queue does not, and the distinction is the whole content
of the feature: a request the daemon has never seen is simply dequeued, and
telling it to interrupt a `unique` it cannot match is noise. Linux draws the
line in exactly the same place, on an `FR_SENT` flag; AOK's is `req->sent`, set
in `fuse_dev_read` at the moment the request moves from pending to processing.

`INTERRUPT` is advisory by protocol. The daemon may act on it and answer the
original with `EINTR`, may ignore it and answer normally, or may not implement
it at all — and all three already worked here, because the abandoned request is
freed by whichever side touches it last either way. What changed is that a
daemon doing something expensive now gets the chance to stop. `sshfs` on a dead
link is the case that motivates it.

There is a deliberate divergence in the same area, and it is worth naming
because it is the one place this chapter chooses *not* to match Linux.

Measured on Devuan, not inferred: once the daemon has read a request, Linux
queues the interrupt and then *waits the request out* uninterruptibly. A child
blocked on a read the daemon never answers sits in state `D` and survives
`SIGKILL` indefinitely — it was still there three seconds later, and would have
been there at reboot.

That is defensible on a workstation, where a root shell can `umount -f` the
mount or abort the connection through `fusectl`. Inside an application it is
not: an unkillable task is a worse failure than an early return, and Chapter
12's rule that every blocking wait is interruptible is one this subsystem
keeps. AOK sends the interrupt and then returns `EINTR` to the caller. The
daemon may still answer afterwards, and the abandoned request is freed by
whichever side touches it last, exactly as before.

**`RELEASE` is fire-and-forget, and `umount` never waits on the daemon.** It
cannot: unmounting runs under `mounts_lock`, and waiting for a daemon while
holding it reintroduces the deadlock of the previous section. After unmount, the
daemon's reads get `ENODEV` — which is exactly how libfuse's own event loop
learns to exit, so the daemon shuts down cleanly without anybody having to
signal it.

**A daemon that dies with requests in flight fails them all with `ENOTCONN`,**
and the mount answers `ENOTCONN` to everything afterwards until it is
unmounted. That is "Transport endpoint is not connected" — the message anyone
who has used sshfs over a flaky link knows by heart, and precisely what real
Linux produces.

## 20.6 What mmap needed, and what is still not there

The gap that mattered most was `mmap`, because the ELF loader uses it: without
`mmap` a FUSE mount could hold programs but never run one. It returned `ENODEV`,
and this section used to say so.

Implementing it meant answering a question the design had so far avoided. Linux
backs a FUSE mapping with the page cache, so every process mapping a node maps
the *same* pages, and dirty pages go back to the daemon as `FUSE_WRITE`s. AOK
has no page cache. What it has instead is a stand-in: an unlinked host temp file
per `(connection, nodeid)`, filled from the daemon on the first `mmap` of that
node and shared by every mapping of it afterwards. Mapping that host file gives
the guest the sharing a page cache would provide, for nothing — two processes
mapping one node land on the same host pages.

Writeback is the harder half, because there is no dirty bit to consult. So a
second, identical temp file records what the daemon last saw, and writeback
diffs the two a page at a time, sending only the pages that differ. A read-only
mapping — by far the common case, and the one that makes execution work — costs
no `FUSE_WRITE`s at all, and a one-page store in a large file sends one page,
which is the granularity Linux writes back at too. The sync points are
`msync()`, `fsync()`, and the last close of the file.

### The lock that made this a design problem

There is one consequence of AOK mapping eagerly where Linux faults lazily, and
it is worth stating because it is not obvious from either side.

`fd_ops.mmap` runs under the address-space **write** lock, with the calling
process's other threads quiesced — that is what `mem_write_lock_with_pokes`
does, and it is right for every filesystem whose bytes are already in hand.
Filling a FUSE cache there is a round trip to a *guest process*. If that
process is this one — a program that mounts a FUSE filesystem and then maps a
file on it, which is an entirely ordinary thing for an archive tool to do — the
daemon thread is among those quiesced, and the mapping thread waits for a
thread that cannot run. Linux never meets this because `mmap` there does no
I/O at all; the fault does, and a fault quiesces nothing.

So the fetch is hoisted out of the locked region into a new optional hook,
`fd_ops.mmap_prepare`, called before the address space is touched, and
`fusefs_fd_mmap` now takes no lock at all — not even `fd->lock`, which the
read, `pread` and `readdir` paths hold across a daemon wait.

Three more paths had the same shape, and the fix for two of them is the same
move — note what needs doing under the lock, do it once the lock is gone:

- **`msync`** walked the page tables under the memory read lock and would have
  written back from inside the walk.
- **Unmapping**, which is subtler and turned out not to be about `munmap` at
  all. Unmapping drops the mapping's reference to the descriptor, and when the
  guest has already closed its own — the completely ordinary `open`, `mmap`,
  `close`, store, `munmap` sequence — that is the *last* reference, so the
  file's `->close` runs from inside `pt_unmap`, under the write lock. Fixing
  it in `munmap` fixed only `munmap`: `MAP_FIXED` overwriting an existing
  mapping, `mremap` shrinking one, and `brk` giving memory back all reach the
  same teardown by other routes. So the deferral moved down to where the
  reference is actually dropped — `pt_unmap` parks such descriptors on the
  `struct mem`, and the unlock drains them. One place, every path, including
  the ones nobody has written yet.
- **`close(2)` itself**, which is the one that had already been broken the
  longest and had nothing to do with `mmap`. `f_close` held the fd table's
  lock across the filesystem's `->close`. For a FUSE file that `->close` sends
  `FUSE_FLUSH` and waits for the daemon — and the daemon's next read of
  `/dev/fuse` has to look its own descriptor up in that same table. The daemon
  was seen answering the `READ` and then simply never receiving the `FLUSH`.
  Both threads stopped there permanently. `f_close`, `dup2`'s replacement of
  an occupied slot, and `close_range` now take the descriptor out of the table
  under the lock and close it outside, which is the rule the split expresses:
  **`table->lock` covers the slot, never the filesystem work behind it.**

Process exit needed something different again. `kernel/exit.c` releases the
address space *before* the fd table, so a mapping's descriptor is closed while
the connection is still open but every thread that could serve it has already
gone — there is no "after the lock" that helps, because the answer is never
coming. A dying task therefore sends its `FLUSH` and does not wait for it,
which is where Linux ends up from the other direction: its waits are killable
and a dying task's return at once.

None of those is a FUSE bug in isolation — the `close(2)` one is a rule about
every filesystem whose `->close` can block, and FUSE is merely the first one
here that can. What they share is that a daemon in a *separate process* shares
neither an fd table nor an address space with its callers, so not one of them
is visible to a test built that way, and `fuse_basic.c` forks its daemon and
passed throughout. `tests/manual/fuse_threaded_daemon.c` exists for that
reason: it runs the daemon on a **thread** of the process using the mount,
which is what libfuse's multi-threaded loop produces and what any program that
mounts a filesystem for its own use looks like. On the parent commit it hangs
before it can print its second line.

It also closes the descriptor *before* unmapping, in one deliberate case, for
a reason worth stating: every other test in the tree unmaps first, and with
the descriptor still open the unmap never drops the last reference — so the
whole deferred-close path was there, doing something load-bearing, and no test
went anywhere near it.

The exec loader was the one the ordinary suite *did* catch. It maps segments through its
own call to `fd_ops.mmap` rather than through `mmap(2)`, so it never ran the
new hook, and the moment `fusefs_fd_mmap` stopped fetching on its own behalf,
running a program off a FUSE mount broke. It calls `mmap_prepare` now. A test
that asserted "mmap returns something" would not have noticed; the one that
actually executes a binary served by the daemon did.

Every one of those behaviours was measured against Devuan before it was written,
and the regression test asserts the same things on both kernels:

| | Devuan 6.12 | AOK |
|---|---|---|
| `MAP_PRIVATE`, `MAP_SHARED` read-only | works | works |
| `MAP_SHARED` + `PROT_WRITE` | works, store reaches the daemon | same |
| the same on an `O_RDONLY` file | `EACCES` | `EACCES` |
| `mmap` of a directory | `ENODEV` | `ENODEV` |
| two processes mapping one file | see each other's stores | same |
| execute a program off the mount | runs | runs |

Two smaller things were found by measuring rather than by reading the list of
known gaps, and both were silent failures rather than honest refusals:

- **`poll` reported nothing.** `fusefs_fd_ops` had no `->poll`, so
  `poll_scan_ready_locked` read back zero and a FUSE file was *never* ready.
  Anything that polls before reading — which is most event loops — hung. Linux
  has no `->poll` for a regular file either; it goes through `DEFAULT_POLLMASK`
  and is always both readable and writable, which measures on a FUSE file as
  `POLLIN|POLLOUT`.
- **`SEEK_DATA`/`SEEK_HOLE` returned `EINVAL`.** Two things were wrong. The
  daemon was never asked: Linux sends `FUSE_LSEEK` (opcode 46, confirmed by
  watching what a Devuan kernel actually puts on the wire), so a filesystem
  over an archive or a sparse remote file can say where its holes really are.
  And when the daemon answers `ENOSYS` — most do — Linux falls back to "the
  whole file is data": `SEEK_DATA` at 0 is 0, `SEEK_HOLE` at 0 is the size,
  and either past the end is `ENXIO`. AOK now does both, with the same
  once-per-connection latch Linux keeps in `fc->no_lseek`, since a refusal is
  a property of the daemon rather than of the call. `EINVAL` — the old answer
  — tells a caller the interface does not exist at all, and the sparse-copy
  paths in `cp`, `tar` and `rsync` act on that difference.

What is still absent, and visibly so:

- **`readdirplus`**, which returns each entry's attributes alongside its name
  and would collapse `ls -l`'s per-entry `LOOKUP`+`GETATTR` into one request.
  It is absent for a reason worth stating rather than an oversight: AOK has no
  attribute cache, so the attributes would arrive and be thrown away
  immediately, while every entry `READDIRPLUS` returns is a *reference* that
  then has to be forgotten. It would cost more than it saves until there is
  somewhere to put the answers. That is the same missing cache Section 20.3's
  per-component walk is paying for, and it is the one change that would make
  several of these numbers better at once.
- **splice**, which avoids a copy on the `/dev/fuse` transfer. The transfers
  here are not copy-bound.
- The newer **`fsopen()`-based mount API** — a whole syscall family rather than
  anything FUSE-specific. libfuse falls back cleanly.
- The mapping and ordinary `read`/`write` are coherent at the sync points
  above, not continuously: `read()` goes to the daemon rather than through the
  cache. Linux's page cache makes the two coherent at all times.

None of those is hidden behind a plausible-looking success. `FUSE_INIT` sends
`flags = 0` and discards the daemon's reply flags, so a daemon is never told
AOK supports a capability it does not — the absences are negotiated away rather
than left to be discovered. That is the capability-lie discipline of Chapter 40
applied to an unfinished feature: the missing pieces are missing visibly.

## 20.7 How it was tested, and why that is the interesting part

`tests/manual/fuse_basic.c` is a daemon and its checks in one program, speaking
the **raw protocol** with no `libfuse` dependency at all.

That choice is the whole point. A test written against libfuse tests libfuse's
interpretation of the protocol. A test written against the wire format tests the
kernel — and, crucially, it can be compiled and run **on a real Linux kernel**,
which is what happened: it passes on all three tested guest architectures *and*
on a real 6.12 kernel as root.

That is Chapter 9's oracle discipline applied to a protocol rather than to an
instruction. The same source, the same expectations, two kernels, and any
divergence is AOK's.

It paid for itself directly when `mmap` was added. The refusal of a writable
shared mapping had been reasoned out from the protocol and was simply wrong —
Linux supports it, and one run of the probe on Devuan said so before a line of
the implementation was written. The daemon in the test now also serves a whole
executable, so `exec` off the mount is checked rather than assumed, and it
records what it was asked — every reference handed out, every `FORGET`, every
`INTERRUPT` — so the accounting of Section 20.3 is asserted rather than
described.

Verification also went the other way, with real software: a libfuse3 "hello"
filesystem and a read-write in-memory filesystem, both built inside an Alpine
guest with the distribution's own packages, both working unmodified.

## 20.8 Two inversions

It is worth pausing on what FUSE is, structurally, because Part V is about its
mirror image.

**FUSE is guest code running as part of the kernel.** A program in the guest
implements a filesystem, and the emulator calls into it — across a descriptor,
over a protocol, with the guest process free to do anything it likes to answer.

**A native program is host code running as a guest process.** A function
compiled into the application implements a program, and the guest's `execve`
calls into it — no protocol at all, because both sides are the same address
space.

The two directions have opposite risk profiles, and the difference is
instructive. FUSE is safe and slow: the boundary is a wire protocol, the daemon
can crash without taking anything with it, and the cost is a round trip per
operation. Native programs are fast and dangerous: there is no boundary at all,
so a native program's mistake is the application's mistake, and Part V is
largely a catalogue of what that costs and how it is contained.

---

*Anchors:* [fs/fuse.c](../../fs/fuse.c), [fs/devices.h](../../fs/devices.h),
[kernel/fs.h](../../kernel/fs.h) (`may_block`), [fs/generic.c](../../fs/generic.c),
[fs/stat.c](../../fs/stat.c), [fs/mount.c](../../fs/mount.c),
[opt/AOK/docs/fuse.md](../../opt/AOK/docs/fuse.md),
`tests/manual/fuse_basic.c`, commit `f583d3c4c`.
