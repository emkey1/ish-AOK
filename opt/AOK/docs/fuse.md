# FUSE: filesystems written as ordinary guest programs

iSH-AOK carries a real FUSE implementation — the `/dev/fuse` character device
and a `fuse` filesystem type, speaking FUSE protocol 7.31. A FUSE daemon you
compile inside your root, against the distro's own `libfuse`, mounts and serves
a filesystem here the same way it would on Linux.

That means sshfs-style overlays, archive mounts, in-memory scratch filesystems,
and anything else you would normally reach for FUSE to build, without patching
the daemon.

## Using it

Install the distro's FUSE packages and build against them as usual. On Alpine:

```sh
apk add fuse3 fuse3-dev            # libfuse3 plus headers
cc hello.c -o hello $(pkg-config --cflags --libs fuse3)
mkdir -p /mnt/hello
./hello /mnt/hello                 # daemonizes, mount appears immediately
ls -l /mnt/hello
fusermount3 -u /mnt/hello          # or: umount /mnt/hello
```

libfuse2 daemons work too — the protocol version covers both.

**An unprivileged user can serve a filesystem** — that is what FUSE is for, and
it matters here because a session with *Open Everything as Default User* turned
on is not root (see [proc-ish.md](proc-ish.md)). It works the same way it does
on any Linux box:

- **As root**, libfuse mounts directly: it opens `/dev/fuse` and calls
  `mount(2)` with `fd=<n>` itself, with no helper involved.
- **As an ordinary user**, libfuse goes through the setuid `fusermount3`
  helper, which is what supplies the privilege `mount(2)` wants. Install the
  distro's FUSE package (`apk add fuse3`, `apt install fuse3`) so that helper
  is present — `/dev/fuse` being world-accessible is not on its own enough.

**A daemon must report ownership its caller can actually use.** This is the
usual reason an unprivileged mount appears broken: a daemon that reports every
file as owned by root, over a root-owned `0755` directory, will refuse a
non-root caller's `create` with `EACCES` — correctly, and identically on real
Linux. Report the mounting user in the `user_id=`/`group_id=` mount options and
in the `uid`/`gid` you fill into each attribute reply, and an unprivileged mount
behaves exactly like a privileged one.

## Things worth knowing

- **Programs stored on a FUSE mount run.** `mmap` works — private mappings,
  shared read-only mappings, and shared writable ones, which is what the ELF
  loader needs. A store through a writable shared mapping reaches the daemon
  as a `FUSE_WRITE`, on `msync()`, on `fsync()`, and at the last close of the
  file. Only the pages that actually changed are sent.
- **Sparse files work if your daemon implements them.** `SEEK_DATA` and
  `SEEK_HOLE` are forwarded as `FUSE_LSEEK`; a daemon that answers `ENOSYS`
  (most do) gets the standard fallback instead — the whole file is data, the
  only hole is at EOF — and is not asked again on that connection.
- **A mapping and a `read()` agree at those sync points, not continuously.**
  Reads and writes go straight to the daemon rather than through the mapping's
  backing, so a program that stores through a mapping and expects a concurrent
  `read()` on another descriptor to see it should `msync()` first. On Linux the
  page cache makes the two coherent at all times. Anything that maps a file,
  works on it, and syncs before handing it on — which is the ordinary
  pattern — sees no difference.
- **Serving a filesystem from a thread of the process that uses it works.**
  That is what libfuse's multi-threaded loop gives you, and what a program
  that mounts a filesystem for its own use looks like. It used to deadlock —
  `close()` on the mount held a lock the daemon thread needed to receive the
  request — and does not any more. A daemon in a separate process was never
  affected.
- **The daemon is the filesystem.** If it exits or crashes, in-flight requests
  fail with `ENOTCONN` and every later operation on that mount answers
  `ENOTCONN` too, until you unmount it — the same behaviour Linux gives you.
  Unmounting makes the daemon's next read return `ENODEV`, which is how
  libfuse's event loop knows to exit.
- **Paths are walked, not cached — but references are returned.** AOK's VFS is
  path-based, so each operation performs a `FUSE_LOOKUP` walk from the root
  nodeid rather than reusing a cached one, and sends `FUSE_FORGET` for each
  node as it finishes with it. Because there is no node cache, forgets arrive
  promptly and in far greater number than a real kernel's — a session that
  sends Linux two may send several dozen here. That is legal, and a daemon that
  keeps an honest lookup count per node will not notice; one that assumes
  `FORGET` arrives only in bulk at unmount will.
- **Interrupts reach the daemon.** `FUSE_INTERRUPT` is sent for a request the
  daemon has already read, naming it, so a daemon that implements interrupts
  can abandon expensive work when the caller is signalled. A request still
  queued is dropped instead, since the daemon has never seen it.
- **A signalled caller stops waiting.** This is the one place AOK chooses not
  to match Linux, where a process blocked on a daemon that never answers sits
  in state `D` and cannot be killed even with `SIGKILL`. There is no root shell
  here to `umount -f` its way out, so an unkillable task would be permanent;
  the caller gets `EINTR` instead. The daemon may still answer afterwards, and
  the answer is discarded.
- **`readdirplus`, splice, and the newer `fsopen()`-based mount API are not
  wired up.** libfuse falls back cleanly on all three; no daemon needs
  changing. `FUSE_INIT` negotiates none of them, so a daemon is never told a
  capability is present when it is not. `readdirplus` is the one with real
  performance on the table, and it waits on an attribute cache: without
  somewhere to keep the attributes it returns, they would be fetched and
  discarded while each entry still had to be forgotten.

## When something is wrong

Confirm the mount actually exists before concluding the filesystem misbehaved —
a daemon that failed to start leaves the mountpoint as an ordinary empty
directory, and reads of it succeed against what is underneath:

```sh
grep fuse /proc/mounts             # nothing here means nothing is mounted
```

If the mount is present but every operation returns `ENOTCONN`, the daemon has
died; unmount and restart it. Running the daemon in the foreground (`-f` for
most libfuse programs) puts its errors on your terminal instead of losing them
to daemonization.

## See also

- [roots.md](roots.md) — the root filesystems a daemon runs inside, and
  chrooting between them.
- [persist.md](persist.md) — writable storage that survives root switches, for
  data a FUSE daemon should keep.
- [00-overview.md](00-overview.md) — how `/AOK` itself is put together.
