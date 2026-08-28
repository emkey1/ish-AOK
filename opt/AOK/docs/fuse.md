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

**You do not need to be root.** That is the point of FUSE, and it holds here:
`/dev/fuse` is mode `0666`, and mounting is not privilege-gated, so an ordinary
user can mount and use a FUSE filesystem. This matters because a session with
*Open Everything as Default User* turned on is not root — see
[proc-ish.md](proc-ish.md).

Two things follow, and the second is the one that catches people out:

- **If your session is root**, libfuse mounts directly — it opens `/dev/fuse`
  and calls `mount(2)` with `fd=<n>` itself. No setuid helper is involved, and
  a root without `fusermount` installed still mounts fine.
- **If it is not**, libfuse uses the `fusermount3` helper, exactly as it would
  on any Linux box. Install the distro's FUSE package (`apk add fuse3`,
  `apt install fuse3`) so that helper exists.

**A daemon must report ownership its caller can actually use.** This is the
usual reason an unprivileged mount appears broken: a daemon that reports every
file as owned by root, over a root-owned `0755` directory, will refuse a
non-root caller's `create` with `EACCES` — correctly, and identically on real
Linux. Report the mounting user in the `user_id=`/`group_id=` mount options and
in the `uid`/`gid` you fill into each attribute reply, and an unprivileged mount
behaves exactly like a privileged one.

## Things worth knowing

- **You cannot execute a program stored on a FUSE mount.** `mmap` is not
  modeled, and loading an executable needs it, so binaries and AppImages must
  be copied off the mount before they will run. Reading, writing, and every
  other ordinary file operation are unaffected.
- **The daemon is the filesystem.** If it exits or crashes, in-flight requests
  fail with `ENOTCONN` and every later operation on that mount answers
  `ENOTCONN` too, until you unmount it — the same behaviour Linux gives you.
  Unmounting makes the daemon's next read return `ENODEV`, which is how
  libfuse's event loop knows to exit.
- **Paths are walked, not cached.** AOK's VFS is path-based, so each operation
  performs a `FUSE_LOOKUP` walk from the root nodeid rather than reusing a
  cached one. `FUSE_FORGET` is therefore never sent, and a daemon's nodeid
  table grows with the size of the tree it has served rather than shrinking.
  For the tree sizes a device holds this is fine; a daemon that assumes
  `FORGET` will arrive should not depend on it for correctness.
- **Interrupts do not reach the daemon.** `FUSE_INTERRUPT` is not sent. A guest
  process blocked on a FUSE call can still be interrupted by a signal, but the
  daemon is not told, so a long operation continues on its side.
- **`readdirplus`, splice, and the newer `fsopen()`-based mount API are not
  wired up.** libfuse falls back cleanly on all three; no daemon needs
  changing.

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
