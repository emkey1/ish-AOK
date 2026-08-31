# 17. fakefs: a filesystem in a database

Chapter 1 opened with a guest reporting that `/bin/busybox` is owned by
`root:root` with mode 755, and a host file underneath it owned by a macOS user
with mode 644. This chapter is about the machinery in between.

The problem is stated quickly. iOS gives an application a sandboxed container
directory and nothing else. There is no way to create a file owned by uid 0,
because there is no uid 0 and no way to become it. There is no `mknod`, so there
are no device nodes. There is no setuid bit that means anything. The filesystem
underneath is usually case- and normalization-insensitive, which Linux's is not.
And a Linux userland assumes all of it works: `apk` sets ownership, distributions
ship `/dev/null` as a character device, `/etc/shadow` is mode 600 for a reason.

So AOK stores the bytes as host files and the *metadata* somewhere it can
control completely: a SQLite database sitting beside them.

## 17.1 The split

A fakefs mount is a directory containing two things:

```
build/alpine-arm64-test/
  data/           the guest's files, as ordinary host files
  meta.db         79 MB of SQLite describing them
  meta.db-wal
  meta.db-shm
```

`data/` holds contents and directory structure. `meta.db` holds, for every path,
an inode number, and for every inode, a `struct ish_stat`:

```c
struct ish_stat {
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t rdev;
};
```

Mode, owner, group, and device numbers. That is the whole of what the host
cannot represent, and separating it out means a `stat()` is a database query and
a `chown` is an `UPDATE`.

The indirection through an inode number rather than storing the metadata against
the path is what makes hard links work — several paths, one inode row — and it
is why the schema was redesigned in January 2018, six months into upstream iSH's
life, once hard links stopped being optional.

Everything else in this chapter follows from the split. Device nodes are inodes
whose mode says `S_IFCHR` and whose `rdev` says 1:3 — no host file is involved at
all. A setuid binary is a mode with `S_ISUID` in it, and it means exactly as much
as AOK decides it means (Chapter 15). Ownership is a number in a column.

## 17.2 Names that the host will not keep

The second problem is names. APFS is case-insensitive and
normalization-insensitive by default; Linux filesystems are neither. Two guest
files whose names differ only in case are two files, and on the host they are
one.

That is not hypothetical, and the header of `fs/fake-path.h` names the incident:

> installing `ncurses-terminfo` in an Alpine guest silently lost
> `/usr/share/terminfo/{a,e,l,...}` because their uppercase twins `{A,E,L,...}`
> already existed as host directories.

Unicode makes it worse in two more ways: case folding means `Ф` and `ф` collide,
and normalization means the composed and decomposed spellings of `é` — `C3 A9`
versus `65 CC 81` — are canonically equivalent and collide too.

So guest names are escaped on the way to the host:

```
'A'..'Z'     ->  '%' followed by the lowercase letter   ("Foo" -> "%foo")
'%'          ->  "%%"
byte >= 0x80 ->  '%' plus two characters encoding the low 7 bits
```

Which is why a `ls` of a fakefs backing store looks like this:

```
$ ls build/alpine-arm64-test/data/
%a%o%k  bin  cgj-test  ...
```

`%a%o%k` is `AOK`. The encoding is unambiguous on decode, and the property that
matters is that two guest names which differ at all produce two host names which
differ in a way APFS respects.

## 17.3 Transactions across host syscalls

The interesting design decision in `fs/fake.c` is that a metadata transaction is
held open across the *real host operation*, not merely across the database work.

Consider `unlink`. The guest expects the file to disappear and its metadata with
it, atomically. If AOK deleted the database row first and then crashed, the file
would exist with no metadata; the other order leaves metadata for a file that is
gone. So `fakefs_unlink` opens a write transaction, performs the host `unlink`,
and commits — and the transaction's duration includes a syscall that has nothing
to do with SQLite.

That is correct, and it has two consequences that shape everything else in this
chapter: the transaction is long, and it holds a file lock the entire time.

## 17.4 One mutex, and 78% of wall clock

The first consequence showed up as a performance mystery: concurrent guest tasks
doing metadata work were *slower* than sequential ones.

`ISH_FAKEFS_LOCKSTATS` explained it. A single thread was already holding the
fakefs mutex about **78% of wall clock** on a metadata-heavy workload. There was
nothing left for a second thread to take.

The diagnosis in `fs/fake-conn.c` is precise about what that mutex actually
protects, and it is not the database:

> The mutex is not protecting the database: the db is WAL with
> `synchronous=NORMAL`, and WAL readers do not block each other. It is
> protecting the ONE sqlite3 handle and the ONE set of cached `sqlite3_stmt`
> that every thread shares.

Once stated that way the fix is obvious: give each thread its own handle and its
own prepared statements, and let readers run concurrently, which WAL already
permits. Writers keep the original mutex, because WAL allows only one writer
anyway — and because that mutex is exactly what makes the across-the-host-syscall
atomicity of Section 17.3 work.

Two details of the pool are worth borrowing:

**Connections are recycled rather than closed.** Guest task churn is high, and
an SQLite open per short-lived thread would cost more than the contention it
saves.

**The cap is a performance bound, never a correctness one.** A thread that
cannot get a connection falls back to the primary and the original locking. That
is the right shape for any resource pool in a system that must not fail: running
out means running slower.

Chapter 16's profile puts this in context. After the pool, `inodes_lock` — the
VFS's own global mutex, one layer up — became the dominant cost at 880 ms of
aggregate wait against fakefs's 0.6 ms. Which is also why the tempting
follow-up, shortening `fakefs_open`'s critical section, was measured and refused:
the inner lock was never the problem.

## 17.5 The suspension that kills you

The second consequence of a long transaction is the one that gets the app
terminated.

iOS kills a process that is suspended while holding a file or SQLite lock. The
termination code is `0xdead10cc` — "dead lock", spelled in hexadecimal, which is
either charming or infuriating depending on how long you have been looking at
crash reports.

A fakefs transaction holds such a lock for its whole duration, and by Section
17.3 that duration includes host syscalls. The header is blunt about what the
field reports looked like:

> threads caught in `db_exec` under `fakefs_inode_orphaned` (fd-table teardown
> at guest task exit), in `fakefs_migrate` during a mount, or simply in
> `realfs_unlink` with a transaction open and no sqlite frame in sight.

The fix is a quiesce gate. Before the app suspends, it blocks *new* transactions
and waits for in-flight ones to drain, so the process reaches a state where no
fakefs lock is held.

Four design choices in that gate are worth pulling out.

**It does not close the databases.** Between transactions, a WAL connection
holds no file lock — an idle connection is already safe — and closing would mean
re-preparing every cached statement on resume for nothing.

**It is process-global and taken before `fs->lock`.** One app has many mounts,
and a task waiting on the gate must hold nothing, or it deadlocks against the
task it is waiting for.

**It has a deadline and reports stragglers.**
`fakefs_quiesce_begin(timeout_ms, &still_in_flight)` returns true if the drain
reached zero and false on timeout — *having still set the gate*. The caller is on
a deadline of its own (iOS gives an app a few seconds to suspend) and must not
hang, so it takes the partial result and carries on.

**It is bounded from the other side too.** A `dispatch_after` lifts the gate
after five seconds, because a backgrounded-but-alive app that stays frozen is a
different bug — the symptom was `sshd` accepting connections while sessions hung
before ever reaching a shell.

And the honest ending: this is not fully gone, by design. The gate waits two
seconds; a transaction that takes longer is still reachable, and the residual
reports are the escape hatch rather than a defect. Three commits in build 549
took it from a recurring crash — 41 reports across builds 516–545 — to a rare
one, and the remaining cases are a bounded, understood risk rather than a
mystery.

## 17.6 Building one, rebuilding one, moving one

`tools/fakefsify` converts a rootfs tarball into a fakefs: it extracts through
libarchive, writes each entry's contents to the escaped host path, and writes
each entry's mode, owner and device numbers into the database. It is the only
supported way to create one — there is no facility for copying a host file into
an existing root, which is why the standard trick for injecting a binary is to
append it to the tarball first and re-run the conversion.

`fs/fake-rebuild.c` reconstructs a database from the host tree when the metadata
is lost or corrupted, with the obvious limitation: it can recover structure and
contents, and it cannot recover ownership or device nodes that only ever existed
in the database.

`fs/fake-migrate.c` upgrades an existing root when the schema changes, which has
happened several times over the project's life. The operational lesson recorded
alongside it is that "flattened" roots — ones whose metadata was lost to an
older bug or an unlucky export — need reinstalling rather than migrating.
Migration assumes the metadata was once right.

## 17.7 The copy engine, and 64 KB of silence

One more story belongs here, because it lives at the boundary between the
filesystem layer and everything that uses it.

`fd_copy_range()` in `kernel/fs.c` is the shared engine behind both `sendfile`
and `copy_file_range`. It reads into a 64 KB bounce buffer and writes it out.

> **The bug that taught us this**
>
> On a short write — which a pipe produces routinely — it dropped the
> read-but-unwritten tail of the buffer, and advanced the input offset past bytes
> that had never been delivered. The caller then saw a clean EOF.
>
> busybox's `cat` and `tar` both use `sendfile`, so **any guest pipe copy of a
> file larger than 64 KB silently truncated.** That broke the end-to-end suite's
> tar setup and teardown, and cascaded into failures that looked like guest
> `gcc` internal compiler errors and Python segfaults on CI — because a
> truncated archive produces corrupted source files, and corrupted source files
> crash compilers.
>
> The fix is a write loop that drains each chunk, and a rewind of the input by
> any leftover. The pinning test is the pipe-copy case in
> `tests/manual/sendfile_vhangup.c`.

The reason to tell it in this chapter is the failure's shape: silent truncation
at a buffer boundary, presenting as crashes in unrelated, well-tested software
several layers away. It is the same shape as Chapter 16's MariaDB crash, and it
is the characteristic failure of a filesystem layer — the bug is data-shaped, and
the symptom is somebody else's.

## 17.8 What fakefs is, in one sentence

It is a filesystem whose semantics live in a relational database and whose bytes
live in a sandbox, joined by an escaping scheme so that a case-insensitive host
can store case-sensitive names, and locked in a way that has to survive an
operating system which kills processes for holding locks at the wrong moment.

Nothing about that is how one would design a filesystem given a free choice. All
of it is a consequence of the constraints in Chapter 1, and it is the single
largest piece of evidence for that chapter's claim that the interesting part of
this project is what the platform forbids.

---

*Anchors:* [fs/fake.c](../../fs/fake.c), [fs/fake-db.c](../../fs/fake-db.c),
[fs/fake-db.h](../../fs/fake-db.h), [fs/fake-conn.c](../../fs/fake-conn.c),
[fs/fake-path.h](../../fs/fake-path.h), [fs/fake-rebuild.c](../../fs/fake-rebuild.c),
[fs/fake-migrate.c](../../fs/fake-migrate.c),
[fs/fake-lockstats.h](../../fs/fake-lockstats.h),
[tools/fakefsify.c](../../tools/fakefsify.c), [kernel/fs.c](../../kernel/fs.c)
(`fd_copy_range`), `app/AppDelegate.m` (the suspension gate),
`tests/manual/sendfile_vhangup.c`.

*Story:* `0xdead10cc` — 41 crash reports across builds 516–545, all of them iOS
terminating the app for being suspended while a fakefs transaction held a lock
it could not have released in time.
