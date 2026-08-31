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
Nodeids are not cached, and `FUSE_FORGET` is never sent.

That is a real cost — an operation on a path five components deep is six
requests to the daemon rather than one — and the design note explains what it
buys:

> repeated lookups of the same name bump the daemon's per-node lookup count (a
> u64) on the same node, so daemon-side memory is bounded by the tree the guest
> has touched, not by the number of operations.

Caching nodeids would mean owning their lifetimes, which means sending
`FORGET`, which means getting reference counting right across a boundary where
the other side is arbitrary guest software that may crash. Not caching trades
throughput for an entire category of bug that cannot then occur. Given that the
alternative to a slow FUSE mount is no FUSE mount, that is the right trade to
have made first.

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

## 20.6 What is not there

Stated plainly, from the header:

- **`mmap` is not modeled**, and loading an executable requires it. So you
  cannot run a program stored on a FUSE mount — binaries and AppImages have to
  be copied off first.
- **`FUSE_INTERRUPT`** is not sent, so a daemon cannot be told that a request
  has been abandoned; AOK abandons it locally instead.
- **`FUSE_FORGET`** is not sent, per Section 20.3.
- **`readdirplus`**, **splice**, and the fd-passing new-mount API are absent.

None of those are hidden behind a plausible-looking success. This is the
capability-lie discipline of Chapter 40 applied to an unfinished feature: the
missing pieces are missing visibly.

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
