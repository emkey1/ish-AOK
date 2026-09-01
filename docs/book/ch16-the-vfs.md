# 16. The VFS

A guest program opens a file. Between `open("/etc/passwd", O_RDONLY)` and any
bytes coming back, iSH-AOK has to decide which filesystem that path belongs to,
whether the caller may traverse every directory on the way, whether any
component is a symlink and where it points, whether the final component exists
and may be read, and then hand the request to a backing store that might be a
SQLite database, a directory on the host, a table of synthetic files compiled
into the binary, or a daemon running inside the guest itself.

That layer is the VFS, and it is where this book's recurring failure mode — the
system answering a slightly different question from the one asked — has produced
the highest concentration of bugs anywhere in the tree. Not because the code is
careless, but because POSIX path semantics have a hundred edge cases and every
one of them is load-bearing for something.

## 16.1 Three objects

**`struct fd`** is an open file description, and it is a tagged union of
everything a descriptor can be: a tty (with the hangup generation of Chapter
12), an epoll set, an eventfd counter, a fifo, a timerfd, a socket with its Unix
domain name, an `O_PATH` handle on a symlink, or an ordinary file with an offset
and a `struct fd_ops` vtable.

One field sits outside the union and it is instructive:

```c
// fcntl(F_SETOWN)/fcntl(F_GETOWN): the pid (positive) or process group
// (negative, -pgid) that would receive SIGIO/SIGURG for this fd. 0 means
// no owner. Stored generically here (not under the socket union) because
// F_SETOWN/F_GETOWN are valid on any fd type on Linux, not just sockets.
pid_t_ owner;
```

The temptation was to put it with the socket fields, because sockets are what
people use it for. Linux allows it on any descriptor, so it lives at the top
level. That decision costs four bytes on every open file and buys the absence of
a bug report from whoever uses `F_SETOWN` on a pipe.

**`struct inode_data`** is the shared per-file state that outlives any one
descriptor: the fifo buffer for a named pipe, the lock list, the reference
counts that keep a Unix socket's name alive while a listener exists.

**`struct mount`** binds a path prefix to a `struct fs_ops` — the vtable each
filesystem implements — and to a source. The mount table is flat and global,
because there are no mount namespaces (Chapter 10): one tree, visible identically
from every process and every chroot.

## 16.2 One funnel: `path_normalize`

Every path that enters the kernel goes through `path_normalize`, which converts
(dirfd, path) into a normalized, mount-relative string. Having exactly one such
function is the single most valuable structural decision in this layer, and the
rest of the chapter is essentially a list of things that were fixed *once* there
and thereby fixed for every `*at()` syscall at the same time.

The core is unsurprising: an absolute path starts at the process's root, a
relative path starts at the dirfd or the working directory, components are
resolved left to right, and symlinks are followed.

The symlink limit is worth one line, because it was wrong in an interesting way.
It was capped at 5 followed links against Linux's 40 — and it counted symlinked
*directory components* too, so ordinary `/etc/alternatives`-style chains hit
`ELOOP` on a system that was working correctly. `MAX_SYMLINKS` is 40 now, as it
should always have been.

## 16.3 The dirfd rules, and a crash three frames away

The `*at()` family — `openat`, `fstatat`, `unlinkat`, and a dozen more — takes a
directory descriptor plus a path. The rules around that descriptor are small,
precise, and were each wrong at some point.

**Rule one: if the path is absolute, `dirfd` is ignored.** Not merely unused —
*ignored*. It is never examined, so it may be `-1`, or a closed descriptor, or
something that was never a directory at all. AOK validated it anyway and
returned `EBADF`.

> **The bug that taught us this**
>
> glibc, canonicalizing a path while creating an Aria temporary table, calls
> `openat(-1, "/tmp", O_PATH|O_CLOEXEC|O_NOFOLLOW)`. The `-1` is deliberate and
> correct: the path is absolute, so no descriptor is needed.
>
> AOK's `EBADF` made Aria carry a NULL, which `ha_maria::drop_table` then
> dereferenced — so the failure presented as `mariadbd` crashing with `SIGSEGV`,
> and the fault address was written into `docs/TODO.md` as a suspected AIO null
> pointer. It also meant **no MariaDB installation on iSH-AOK had ever
> completed**: `mysql_install_db` died leaving 3 of roughly 30 system tables.
>
> Two lessons, both general. A crash inside a well-tested third-party program is
> usually this emulator returning the wrong errno somewhere earlier — chase the
> syscall, not the backtrace. And `fs/stat.c` keeps its **own copy** of the
> dirfd helper: the first fix touched only `kernel/fs.c`, everything passed
> except `fstatat`, and the regression test caught what review did not. `statx`
> lives in that same file, and `statx` is what modern glibc actually calls.

**Rule two: a non-directory dirfd is `ENOTDIR`, decided before any lookup.** AOK
went straight to the search-permission check on the descriptor, and a 0644 file
has no execute bit — so the answer was `EACCES`. The commit describes the damage
precisely: "a plausible errno for entirely the wrong reason, which sends the
caller to look at permissions rather than at the descriptor it passed." Fixed
centrally in `path_normalize`, so every `*at()` call gets it.

**Rule three: a descriptor with no path is `ENOTDIR` too.** A socket, a pipe, an
anonymous inode — `stress-ng --sockabuse` passes a socket fd to `utimensat`, and
AOK asserted, which aborted the entire application. A genuinely NULL dirfd
(`stress-ng --dir`) did the same. Both now return the errno Linux returns.

It is worth noticing who found two of those: `stress-ng`, which passes garbage
to syscalls on purpose. A fuzzer for this layer is not a hypothetical piece of
future tooling; it is a package in the guest's own repository.

**And one caller can ask for more than the rules give it.** `openat2` takes a
`struct open_how` carrying a `resolve` mask, which is the whole reason it exists
over `openat`: it constrains *how* the path is allowed to resolve. AOK used to
reject every bit in that mask, which left `openat2` doing nothing `openat` did
not already do. Two of the bits are honoured now, and the other four are refused
rather than pretended:

- **`RESOLVE_NO_SYMLINKS`** is answered inside path resolution itself. A symlink
  anywhere in the path — a directory component included — is the answer rather
  than something to follow, and the call fails with `ELOOP`.
- **`RESOLVE_CACHED`** returns `EAGAIN`, which is exactly what it asks for:
  "only if this is already cached." Declining is honest, because on Linux the
  answer depends on what happens to be in the dcache, so every caller of it has
  a slow path already.
- **`RESOLVE_BENEATH`, `RESOLVE_IN_ROOT`, `RESOLVE_NO_XDEV` and
  `RESOLVE_NO_MAGICLINKS`** are refused with `EINVAL`, and the refusal is the
  interesting part. Those four are *sandboxes*, and what they promise is that no
  intermediate step of the resolution escaped — a statement about the resolution
  as it happens. AOK resolves the path and then opens it in a second pass, so
  anything checked in between is checked against a path that could have changed
  underneath, which is exactly the time-of-check-to-time-of-use hole
  `RESOLVE_BENEATH` exists to close. `EINVAL` is what a kernel without them
  says, and `openat2` is Linux 5.6+, so nothing may assume it is there.

A sandbox that reports success without holding is worse than one that says it is
not available — Chapter 40's capability-honesty rule applied to a security
primitive.

## 16.4 A permission check, and the measurement that scoped it

Here is a subtle hole that most implementations have at some point had.

The dirfd's *own* search bit is never a component of the path being resolved, so
the loop that walks components never checks it — it only checks what follows.
Without an explicit check, `openat(dirfd, "file")` reads files inside a
directory the caller has no search permission on, and the same applies to a
working directory whose search bit was removed after the `chdir`.

`O_PATH` on a directory correctly performs no permission check of its own, so
the descriptor is legitimately obtainable. The check therefore has to happen at
*use* time, and the comment in `path_normalize` states the principle exactly:
"an fd opened while permissions allowed it must not keep working after they
change."

Then comes the part worth studying, which is how far the check extends:

> Only for a RELATIVE path. An absolute one starts at the process's own root,
> and Linux does not require search permission on that — it checks the
> components it descends into, which `__path_normalize` already does. Checking
> it here cost an fstat on every absolute-path resolution and measured ~9% on
> open/stat-heavy work, for a check Linux does not perform.

Two independent arguments arriving at the same answer: the fidelity argument
(Linux does not do this) and the cost argument (9% of an open-heavy workload).
When those two agree, the decision is easy. The interesting cases in this book
are the ones where they do not, and the fidelity argument wins anyway.

## 16.5 Five permission defects in one commit

A single conformance sweep in August 2026 found five, and they make a good set
because each is the same shape: a check that answered a nearby question.

**`utimensat` checked nothing at all.** `generic_utime` went straight to the
filesystem, unlike `generic_setattrat` sitting beside it. Any user could restamp
any file on the system, including root-owned files they could not write — which
the commit rates, correctly, as "enough on its own to mislead make, rsync, tar
and anything else that trusts an mtime."

The fix has a structural lesson in it. Linux requires *ownership* for explicit
times (`EPERM`) and *write permission* for "now" (`EACCES`) — two different
rules for two different requests. But by the time `generic_utime` runs, "now"
has already been resolved to a concrete timestamp, and the two cases are
indistinguishable. So the check went into `sys_utime_common`, upstream of the
resolution, and the entry points now pass `UTIME_NOW` through instead of
resolving it themselves: "that is what the argument means, and it keeps the
distinction intact." **A check has to live where the information it needs still
exists.**

**`access(2)` always said yes for root.** `access` deliberately swaps the
filesystem ids to the *real* uid, so that a setuid-root program can ask "could
the user who ran me read this?" — which is the entire point of the call. But
`access_check`'s root override consulted the *effective* uid, which the swap
does not touch, so the answer was always yes. That is the opposite of what the
caller wants to know, and it is exactly the check a setuid program makes before
opening a file on a user's behalf.

**`chdir` and `chroot` demanded read.** They opened the directory `O_RDONLY`
where Linux wants only search, so `cd` into a 0711 directory — the ordinary
shape of a home directory or a drop-box — failed with "Permission denied".

**A trailing slash demanded execute on the final component**, because the
resolver could not distinguish "traverse into this" from "this must be a
directory". `stat("/root/")` was `EACCES` for a normal user where
`stat("/root")` succeeded, so `test -d /root/` and `ls -d /root/` failed.

All four are invisible when you run as root, which is Chapter 15's rule and the
reason this class of bug survived so long.

## 16.6 Validation, and two ways to be wrong about a buffer

A second sweep found seven arguments AOK accepted that Linux rejects. Two are
worth quoting because they fail in opposite directions.

**`fstatat` and `fchownat` accepted any flag word** and silently ignored the
bits they did not recognize — "telling a caller probing for a flag this kernel
lacks that it had been honoured." That is the capability lie of Chapter 40 in
its most literal form: a program that sets `AT_NO_AUTOMOUNT` or
`AT_EMPTY_PATH` and gets success has been told the flag worked.

**`readlinkat` took `bufsiz` unsigned**, so a negative size read as an enormous
one, was clamped to `MAX_PATH`, and the entire link target was written into a
buffer the caller had just said was not there. A sign error became a buffer
overflow.

`statfs` managed to be wrong in three ways at once, and the third is a nice
trap: it resolved with `NOFOLLOW`, so it reported the filesystem a symlink
*lives on* rather than the one it names; it did not require the final component
to exist, so `df /no/such/file` printed a filesystem and exited 0; and when that
was fixed, existence had to be checked against the **raw** path rather than the
normalized one — because a normalized `/` is the empty string internally, and
checking the output made `df /` return `ENOENT`.

## 16.7 What the lock profiler says

The VFS is also where this system's contention lives, and it is worth being
precise about that rather than impressionistic, because the impressions were
wrong.

`ISH_LOCKSTATS` instruments every `lock_t` and `wrlock_t` and reports duty
cycle, aggregate wait, and per-call-site hold counts. Run against an
open/write-heavy workload, it says:

**`inodes_lock` is the bottleneck.** 74–77% duty cycle, **880 ms of aggregate
wait**, 5,676 contended acquires in a 443 ms window. It is a single global mutex
that `generic_openat` holds across a stat, a host open, and an fstat — and the
source already says so, at `fs/generic.c`: *"TODO: don't do this"*.

**`mem->lock` is not a problem**, despite looking alarming. Its read side shows
240–305% duty under `make -j4`, which is what a *working* shared lock looks like
— several readers holding it simultaneously — with 1.1 ms of wait across a
one-second build. The write side is 13–16% duty and 0.1 ms.

And one negative result that saved somebody a day:

> **Do NOT shorten `fakefs_open`'s critical section.** Its write transaction is
> nested inside `inodes_lock` for its entire life. Measured side by side, fakefs
> waits 0.6 ms and inodes waits 880 ms. Shortening the inner lock cannot help.
> This was asked for explicitly and the measurement said no.

There is also a tool-literacy rule buried in those numbers: a duty cycle above
100% on an *exclusive* mutex means the measurement is wrong, not that the lock
is superhumanly busy. On a shared lock it means concurrency, which is the point
of a shared lock.

## 16.8 Blocked is not contended

The most expensive lock lesson in the project was not about a lock at all.

> **The bug that taught us this**
>
> `docs/TODO.md` carried a confident, specific diagnosis of a hang: "Established
> by sampling a hung one — which produced a lock cycle rather than a hunch",
> naming the memory read lock held across the JIT jetsam lock, with two candidate
> fixes costed against that cycle.
>
> Both were wrong, because **there was no cycle.** Sampling a fresh hang showed
> the same threads *and* the thing the original analysis had never checked:
> nobody held the jetsam write lock. The blocked reader was asleep on a **free**
> lock — a Darwin psynch lost wakeup, already documented elsewhere in the tree,
> and triggered by the very `SIGUSR1` pokes that `mem_write_lock_with_pokes`
> sends to evict readers.
>
> The question that separates the two cases is one line long: *who holds it?* N
> threads blocked on a lock look identical whether the lock is held or free, and
> only one of those is contention.
>
> The rule: before acting on a recorded diagnosis — however confident, however
> specific, however much it sounds like somebody already did the work — re-derive
> it from fresh evidence.

## 16.9 File locking

`fs/lock.c` implements `flock` and POSIX advisory record locks, and the recent
work on it is a compact illustration of the chapter's themes: validate the byte
range rather than trusting it, name the process holding a conflicting lock in
the diagnostic rather than reporting a bare `EWOULDBLOCK`, and break the
deadlock that two processes waiting on each other's ranges would otherwise
produce.

Advisory locking also matters more here than it might elsewhere, because
`fcntl` locks are what SQLite uses — and SQLite is what fakefs is built on
(Chapter 17). The guest's locks and the emulator's own storage engine are using
the same primitive one layer apart.

## 16.10 Why one funnel was worth it

Count the fixes in this chapter that landed in `path_normalize`: the non-directory
dirfd, the non-path dirfd, the NULL dirfd, the search-permission check, the
symlink limit, the trailing-slash rule. Each was written once and every `*at()`
syscall in the system inherited it.

Then count the counterexample: the dirfd helper that exists in two copies, in
`kernel/fs.c` and `fs/stat.c`, where the first fix reached one of them, `fstatat`
alone stayed broken, and only the regression test noticed. They are still not
unified.

That contrast is the argument for a VFS in miniature. The value of the layer is
not abstraction for its own sake — it is that a rule about paths has exactly one
place to be right.

---

*Anchors:* [fs/path.c](../../fs/path.c), [fs/path.h](../../fs/path.h),
[fs/fd.c](../../fs/fd.c), [fs/fd.h](../../fs/fd.h), [fs/inode.c](../../fs/inode.c),
[fs/mount.c](../../fs/mount.c), [fs/generic.c](../../fs/generic.c),
[fs/lock.c](../../fs/lock.c), [fs/stat.c](../../fs/stat.c),
[kernel/fs.c](../../kernel/fs.c), [util/lockstats.c](../../util/lockstats.c),
[docs/TODO.md](../../docs/TODO.md).

*Story:* `mariadbd` crashing in `ha_maria::drop_table` — three frames and one
library away from an `openat(-1, "/tmp", ...)` that returned `EBADF` because AOK
validated a descriptor the standard says is ignored.
