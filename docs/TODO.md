# iSH-AOK TODO

Open work: bugs that are diagnosed but not fixed, reported issues, features
deferred on purpose, and host capabilities worth exposing. Each entry says what
is already **established**, so nobody re-derives it, and what the **next step**
actually is.

This is a lab notebook, not a task list. It records what is *known* about work
that is not done; it does not say what will be done next or in what order.
That is [docs/roadmap.md](roadmap.md), and the near-term commitments for the
release being prepared are in `docs/build_<N>_musts.md`.

Started 2026-08-19, after the 549 release run. Closed entries from the 549 and
550 cycles moved to
[docs/historical/todo-closed-549-550.md](historical/todo-closed-549-550.md) on
2026-09-07.

---

## Diagnosed, not fixed

### Under `set -T`, some bash re-launches announce a command twice

The last trap divergence between a re-launched subshell and a forked one, and
the only one left after the 2026-09-07 work on `deps/bash/aok_fork.c` (the
special traps emitted last, emitted DEBUG-last among themselves, and `$?` moved
out of the state script into `AOK_BASH_STATUS`).

A re-launch from `execute_simple_command` -- a pipeline element, an async simple
command -- fires the DEBUG trap in the parent (that site runs the trap and *then*
calls `make_child`) and again in the child, which re-parses the text it was
handed. `set -T; trap 'echo T' DEBUG; : | cat` fires 5 times against a fork's 3.
`( )` and `$( )` are exact, because the parent does not announce those, and
`tests/manual/native_bash_fork_state.sh` asserts that agreement.

Only reachable under `-T`, which is what a DEBUG-trap debugger sets. The obvious
mechanism -- the child skipping a counted number of fires on a signal from the
parent -- can *swallow* a real fire if the count is ever wrong, which is worse
for a debugger than an extra one, so it has not been built. Anything that closes
it has to work the other way round: the child would have to be told that the
command it is about to parse has already been announced, which is a property of
that one re-launch site rather than a count.

### atop's accounting daemon wedges boot, and nothing after it starts

Reported from a device 2026-09-03. Symptom looks like broken networking --
`ssh` to the guest is refused on every route -- and the console shows
`receive NETLINK family, errno -2` on boot. Neither is the actual fault.

The thread dump names it exactly. `atopacc` sits in `ppoll` with a NULL
timeout, so it waits forever with nothing able to wake it; `S01atop` and `rc`
are both parked in `wait4` behind it. Every init script ordered after
`S01atop` therefore never runs, and sshd is one of them. The connection is
refused because sshd was never started, not because networking is broken.

The netlink line is the same event seen from the other end. AOK's generic
netlink controller (fs/sock.c, around the `GENL_FAMILY_TASKSTATS_` block)
implements exactly one family, `TASKSTATS`, because iotop is the consumer it
was written for and iotop hard-exits without it. atop's accounting daemon
asks for its own `netatop` family instead, correctly gets ENOENT, and then
blocks rather than giving up.

**The interesting half is why it blocks.** A real Linux without the netatop
module returns the same ENOENT and atop copes there, so the divergence is not
the missing family. The likely cause is that AOK's netlink socket never
becomes readable and never reports an error after a failed family lookup, so
a poll on it has nothing to return -- worth confirming before fixing, since
the fix differs depending on whether the socket should report POLLERR or the
lookup should fail the socket outright. **Unverified.**

Not a regression: atop is newly added to that root's boot sequence, so this
is the first time the path has been exercised. Removing `S01atop` restores
boot and sshd. Implementing the `netatop` family is NOT the fix -- a guest
that blocks forever on a family a real kernel also refuses is the bug.


### SEEK_DATA/SEEK_HOLE is still EINVAL on tmpfs

Noticed while giving fusefs a real implementation, and **half fixed since**:
`fs/real.c:819` now answers both for a realfs file, and `fs/fuse.c:1236` answers
them for FUSE. **tmpfs does not.** `fs/tmp.c:1593` routes straight into
`generic_seek` (fs/generic.c:1345), which answers `_EINVAL` for any whence that
is not SET/CUR/END.

Measured on Devuan for a 4096-byte file with no holes: `SEEK_DATA(1000)` is
1000, `SEEK_HOLE(1000)` is 4096, a negative offset is `ENXIO`, and **both
reposition the file offset** like any other whence. `EINVAL` tells a caller the
interface does not exist at all, which is what the sparse-copy paths in `cp`,
`tar` and `rsync` act on -- the same reasoning that made it worth fixing for the
other two backends. The fix is the same shape: answer from the file's size, and
set `fd->offset`. It is now the odd one out rather than one of a pair, which
makes it a smaller job than the original entry described.

### FUSE has no attribute cache, and three absences follow from it

Measured against Devuan (Linux 6.12) on 2026-09-01, when mmap, FUSE_FORGET and
FUSE_INTERRUPT were added (`fs/fuse.c`, `tests/manual/fuse_basic.c`).

AOK's VFS is path-based, so every FUSE operation walks from the root nodeid
with one `LOOKUP` per component and forgets each node behind it. There is no
dentry cache and no attribute cache. Three things follow, and all three are
currently absences rather than half-implementations:

- **`readdirplus` would cost more than it saves.** It returns each entry's
  attributes with its name, which is what makes `ls -l` one request instead of
  N. With nowhere to keep them the attributes would be fetched and dropped,
  while every entry it returned would still be a reference to forget. Nothing
  to do here until there is a cache.
- **A path five components deep is six requests.** Same cause. The design note
  in `fs/fuse.c` argues the trade was right to make first, and it was, but the
  measured cost is real on a deep tree.
- **A mapping and `read()` are coherent only at sync points.** `mmap` is backed
  by a per-nodeid host stand-in for the page cache (see the chapter), while
  `read`/`write` go straight to the daemon. Linux's page cache makes the two
  coherent continuously. Routing reads through the stand-in would change
  behaviour for daemons that return different bytes on each read, so it needs
  the same cache design rather than a local patch.

The one change behind all three is a real dentry/attribute cache with FUSE's
`entry_valid`/`attr_valid` timeouts honoured -- which also means owning node
lifetimes across a boundary where the other side may crash. The reference
accounting that makes that safe now exists and is asserted by the test (no node
is ever forgotten more times than it was handed out), so the groundwork is
there; the cache is not.

Two other absences are unrelated to this and are simply not worth it: **splice**
on `/dev/fuse` (the transfers are not copy-bound) and the **`fsopen()`-based
mount API** (a whole syscall family, and libfuse falls back cleanly).

### PI futexes are ENOSYS

Measured 2026-09-01 alongside the futex argument-validation work
(`tests/manual/futex_validation.c`, which closed alignment, the expired
absolute deadline, and the WAKE_OP encoding). FUTEX_LOCK_PI, FUTEX_UNLOCK_PI,
FUTEX_TRYLOCK_PI and FUTEX_WAIT_REQUEUE_PI all return ENOSYS, so a glibc
PTHREAD_PRIO_INHERIT mutex fails at `pthread_mutex_lock`. (musl does not
implement PI mutexes at all -- `pthread_mutexattr_setprotocol` returns ENOSYS
in userspace -- so this is only reachable from a glibc guest: Debian, Arch.)

The locking half is implementable and is what programs actually depend on:
the word holds the owner's TID with FUTEX_WAITERS and FUTEX_OWNER_DIED as the
top two bits, TRYLOCK_PI is a compare-exchange from 0 to the caller's TID,
LOCK_PI sets FUTEX_WAITERS and blocks, UNLOCK_PI checks ownership and hands
off. That needs an owner field per futex and interacts with the robust-list
FUTEX_OWNER_DIED path already implemented here.

The INHERITANCE half is not implementable and would not be even if it were
written: iSH has no scheduler priority to donate -- realtime scheduling
classes are already refused with EPERM (kernel/resource.c). So the honest
shape is working mutual exclusion with the priority boost documented as a
no-op, which is strictly better than a lock that cannot be taken at all --
but it should be written knowing that, not discovered later.

### tmpfs size= is accepted and not enforced

Measured 2026-09-01. `mount -t tmpfs -o size=1M` takes the option and then
lets the filesystem grow without limit: 4 MiB written to a size=1M mount with
no ENOSPC, where Linux accepts 1044480 bytes and then fails. It matters more
here than on a desktop -- an unbounded /tmp or /dev/shm is host memory on a
device with a jetsam budget.

The obstacle is accounting, not the check. `tmpfs_file_resize(inode, size)`
takes no mount, and `struct tmp_inode` has no way back to one, so there is
nowhere to add up a mount's bytes. `tmpfs_statfs` already walks the whole tree
with `tmpfs_count_tree` to answer df, which is fine once per statfs and
hopeless per write. The fix is a per-mount used-bytes counter that
`tmpfs_file_resize` and the write path adjust, which means giving the inode a
pointer to its mount's accounting (set at creation, since every inode is
created under a known parent) and threading it through the four resize call
sites.

### A directory walk still costs more per entry the larger the directory

Measured 2026-09-01: 2000 entries at 5.28 us each, 8000 at 8.61 us -- a 1.63x
per-entry increase for 4x the entries, where Linux is flat (0.43 vs 0.41).

`fs/dir.c` used to call the host `telldir()` twice per entry and now calls it
once (the position after entry N is the position before entry N+1, so it is
carried forward). That halved the calls and moved the ratio only from 1.75x to
1.63x, so the per-call cost of telldir is NOT the dominant term and the
remaining superlinearity is somewhere else -- most likely the per-entry
metadata lookup in the fakefs backing this measurement rather than the dirent
loop itself. Worth re-measuring against a realfs directory and a tmpfs one
separately before changing anything: the audit files this as two findings (a
quadratic telldir and an unindexed tmpfs directory scan) and the evidence so
far does not clearly implicate either.

### What is still missing from procfs

Added 2026-09-01 alongside the procfs work in `tests/manual/proc_files.c`,
which closed /proc/{devices,partitions,swaps,modules,cgroups,interrupts,
thread-self,sysvipc/*}, /proc/sys/fs/inotify/*, procfs link counts, and
status's Umask/SigIgn/SigCgt plus stat's starttime. What is left, measured
against Linux 6.12:

**`/proc/locks` does not exist.** Linux lists every POSIX, OFD and FLOCK lock
with type, holder pid, major:minor:inode and byte range; `lslocks(8)` and
`lsof` read it and see nothing without it. AOK has all of this in `fs/lock.c`
-- the work is enumerating the per-inode lock lists safely from procfs, which
means a lock-ordering question (procfs read -> inode locks) rather than
missing data.

**`/proc/sys` is 29 keys against Linux's ~1355.** The audit called this out
and it stays a deliberate non-implementation: the keys that exist are the ones
something reads, and inventing the rest would mean 1300 files whose values are
made up. Adding a key is cheap when a real consumer turns up.

**A directory listed by a readdir callback reports nlink 2, not 2 + its
subdirectories.** /proc itself and /proc/<pid> are built by callback rather
than a static child table, and counting their subdirectories means walking the
pid table on every stat. Linux reports 219 for /proc and 9 for /proc/<pid>;
2 is the honest floor, and no longer 0, which is what a deleted inode looks
like.

**System V shared memory is unimplemented**, so /proc/sysvipc/shm is its
header alone. Semaphores and message queues are listed for real.

### PIPE_BUF atomicity cannot be imposed on a HOST pipe

Measured 2026-09-01. A write of at most PIPE_BUF is atomic on Linux: with
insufficient room it writes NOTHING and blocks or returns EAGAIN, rather than
putting in what fits. That is why several processes may share one pipe for log
lines -- a partial write splits a record and the next writer's bytes land in
the middle of it.

AOK's own FIFO buffer now honours it (`fs/fifo.c`, covered by
`tests/manual/fd_conventions.c`), which is every FIFO on a tmpfs. A `pipe(2)`
pair is different: `fs/pipe.c` hands the guest a HOST pipe and writes go
straight through `realfs_fdops`, so the guarantee is Darwin's, and Darwin's
PIPE_BUF is **512**, not 4096. A guest write between 513 and 4096 bytes can
come back short where Linux would have refused it whole -- measured directly
on the host: a 1024-byte write with ~600 bytes free returns 600.

Enforcing it from here needs the free space BEFORE the write, and Darwin does
not offer it. What it does offer, measured:

- `PIPE_BUF` is 512; capacity starts at 16K and grows to 64K on demand
- `fstat(fd).st_size` on EITHER end reports the bytes currently buffered
- `ioctl(FIONREAD)` works on the read end only; the write end answers 0
- there is no `FIONSPACE`, and no `F_GETPIPE_SZ`/`F_SETPIPE_SZ`

So the buffered count is available but the capacity is not, and free space is
capacity minus buffered. A running estimate of capacity would be a lower
bound, which makes the check refuse writes that had room -- wrong in the other
direction. The real fix is to stop delegating: give `pipe(2)` AOK's own
buffer, the way tmpfs FIFOs already have one. That is a large change (pipes
are handed to native code and to the host across `exec`), so it is recorded
rather than attempted here.

### Three file-mapping behaviours that need what the memory model does not keep

Measured against Linux 6.12 on 2026-09-01, alongside the mmap conformance work
that closed seven of the group (`tests/manual/mmap_conventions.c`). These three
were left because each needs state `emu/memory.c` does not carry, not because
the behaviour is in doubt.

**`MADV_DONTNEED` on a MAP_PRIVATE FILE mapping keeps the COW copy.** Linux
drops the private page so the next read comes back from the file: write 'Z'
over a file byte 'A' through a private mapping, `madvise(MADV_DONTNEED)`, read
again, and Linux gives 'A'. AOK gives 'Z'. `kernel/mmap.c` already handles the
anonymous case (jemalloc depends on it) and says so in a comment; the file case
needs the page's ORIGIN to still be reachable after the copy-on-write, and a
`struct pt_entry` that has been written keeps only the private copy. Doing it
properly means remembering the file-backed `struct data` per COW page.

**A load or store wholly past EOF in a file mapping does not SIGBUS.** Linux
faults both; AOK reads zeroes and lets stores land, and they become file
content if the file later grows. The fault handler would have to know the
backing file's current size at fault time, which means carrying the file
identity into the page fault path rather than just the host memory.

**`remap_file_pages` is ENOSYS.** Linux has emulated it over mmap since 3.16
and a linear remap returns 0. Implementing the emulation means splitting a
mapping into per-page mappings with independent offsets, which the reservation
model (never split -- see `struct mem_lazy_map`) is built to avoid.

### PROT_EXEC is never enforced -- no NX for guest pages

`emu/memory.h` says "P_READ and P_EXEC are ignored for now", and P_EXEC really
is: it is stored, printed in `/proc/<pid>/maps`, reconstructed by mremap, and
never once consulted. Measured against Linux 6.12 (arm64 `mov w0,#42; ret` in a
PROT_READ|PROT_WRITE page):

| | Linux | AOK |
|---|---|---|
| call into a never-PROT_EXEC page | SIGSEGV | returns 42 |
| `mprotect(PROT_READ)` over a PROT_EXEC page, then call | SIGSEGV | returns 42 |

So every guest `.data`/`.bss` page is executable and any guest JIT's W^X
discipline is decorative. Graded a mitigation gap rather than a hole: it takes
a separate memory-corruption bug in guest software to matter.

**Why it is not fixed here, and what it would take.** The instruction-fetch
path has no access type of its own -- `emu/tlb.h` fills the TLB for a fetch
with `MEM_READ` -- so there is nothing for a check to hang off. Two designs
were considered:

- *A TLB bit.* `struct tlb_entry` is `page`, `page_if_writable`,
  `data_minus_addr` -- 16 bytes, 1024 entries. Adding `page_if_executable`
  pushes it to 24 and grows the emulator's hottest structure by half. Rejected
  on cost.

- *Check once per compiled block, invalidate on revoke.* This is the right
  shape and is nearly free: `jit_block_compile_common` runs once per block, so
  the check lands exactly where Linux's fault-on-fetch would, and
  `jit_invalidate_page` (already used for self-modifying code) handles the
  revoke half when `pt_set_flags` clears P_EXEC.

  The obstacle is fault delivery. `jit_block_compile*` returning NULL already
  means OOM, and every dispatch loop responds by flushing the entire JIT,
  retrying, and then killing the task with a "JIT OOM" printk. A non-executable
  page needs a *distinct* signal threaded out to raise INT_PF with the faulting
  address instead -- and there are four dispatch loops (i386, amd64, arm64,
  riscv64), each with its own OOM and crash-unwind structure. The interpreter
  build needs its own check as well.

That is a contained project rather than a patch, and it touches the one path
where a mistake stops every guest from running. Worth doing deliberately, with
its own before/after benchmark run, rather than folded into a conformance
sweep.

### `fcntl(F_GETFL)` on a pipe reports an O_NONBLOCK the guest never set

Found 2026-08-23 while writing `native_ptrace_group_stop.c`, whose drain step
did the textbook thing and got bitten:

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    ... read until EAGAIN ...
    fcntl(fd, F_SETFL, flags);          // "restore"

The restore left the pipe NON-blocking, and every later `read` returned
EAGAIN. Not an `F_SETFL` bug -- `flags` was already `O_NONBLOCK` when it was
read back, so the restore faithfully wrote it again.

**Where the lie comes from.** A guest pipe is a host pipe (`fs/pipe.c` ->
`realfs_fdops`), and `realfs_getflags` (`fs/real.c:1160`) answers `F_GETFL` by
asking the HOST descriptor:

    int flags = fcntl(fd->real_fd, F_GETFL);

Meanwhile `realfs_read` (`fs/real.c:585`) permanently forces that host
descriptor non-blocking the first time the guest does a *blocking* read on it,
and deliberately never restores it -- the comment there is right about why
(restoring it races a sibling task into an uninterruptible, SIGKILL-proof host
`read`, which is a real pipeherd hang that was fixed by exactly this). So the
host flag is an implementation detail that must not be visible, and it is:
before the first read `F_GETFL` says 0, after it says `O_NONBLOCK`, with the
guest having done nothing.

The kernel's own `fd->flags` -- which is what actually governs guest blocking
semantics, and what `realfs_setflags` maintains -- still says blocking. The
two disagree and `F_GETFL` returns the wrong one.

**Next step.** `realfs_getflags` should report the guest-visible flags from
`fd->flags` for the bits the guest owns (`O_APPEND`, `O_NONBLOCK`) and take
only the access mode and the rest from the host. Small, but it needs its own
test: the get-modify-set idiom is everywhere, and silently turning a pipe
non-blocking under a program that never asked is the kind of thing that
surfaces far from here. Worth checking whether sockets and ttys answer
`F_GETFL` the same way before fixing just the one path.

### #523: yay's reported failure does not reproduce; an http2 flake does

Reproduced the environment on 2026-08-20 -- Arch Linux ARM aarch64, yay v13.0.1
built from AUR under emulation -- and ran `yay -S pandoc-bin` four times. **The
reported `context: signal: terminated` never appeared.** All four got through
the AUR fetch and downloaded sources.

Getting there needed five things fixed first, only one of them AOK's:

1. Landlock, 2. a dangling /etc/resolv.conf, 3. an empty keyring -- all three
   now shipped as `/AOK/fixes/arch`.
4. **`/dev/fd` was missing**, so bash process substitution was ENOENT and
   makepkg died at "Retrieving sources". Ours, and fixed (`59827f5ce`).
5. The minirootfs strips headers from 137 packages, so anything that compiles
   needs `pacman -S glibc linux-api-headers` first.

**What DOES reproduce, about one run in four:**

    request failed: Get "https://aur.archlinux.org/rpc?...":
        http2: client conn could not be established

yay recovers -- it falls back to git and carries on -- so it is not fatal, and
it is not what was reported. But it is a real intermittent failure of Go's
HTTP/2 client against a host that curl reaches every time, on both HTTP/2 and
HTTP/1.1. Ruled out already: not git (3 of 3 clones standalone), not TLS
generally (pacman syncs fine), not concurrency (9 simultaneous TLS operations
all succeeded).

**Measured 2026-08-20, and it is a latency tail, not a protocol bug.** TLS
handshake time to the same host, 15 samples each:

    host  (macOS)   min 0.09  median 0.21  max  1.15
    guest (AOK)     min 0.21  median 0.39  max 15.32

The median is about twice the host's -- unremarkable for emulation. The TAIL is
13x worse, and 15.3s is past Go's default TLSHandshakeTimeout of 10s, which is
exactly how "http2: client conn could not be established" arises. The same
stall explains the OpenSSL "unexpected eof while reading" seen from git.

**The CPU-count lie is not the cause**, though it was the obvious suspect:
AOK reports 4 of the host's 10 logical CPUs, and Go sizes GOMAXPROCS from it.
A Go HTTP/2 probe run at GOMAXPROCS 1, 4 and 8 (20 requests each) failed once
in 60, at GOMAXPROCS=8, with a handshake timeout -- the same tail, not a
scheduling effect.

**Next step.** Find what stalls a socket for seconds when the median is
sub-second. Nothing in the handshake is compute-heavy, so this is a wait that
is not being woken promptly rather than work that is slow -- which puts it in
the same neighbourhood as the poll/quiesce machinery. `curl` on its own shows
the tail too, so it reproduces without Go, without yay and without the AUR:
any repeated HTTPS handshake will do.

---

## Deferred on purpose

### External display / AirPlay -- GH #540

Work exists on the branch `worktree-external-display-540`:

    6156597ee app: mirror the Wayland display to an external display (GH #540)

**Deferred to a future release by the maintainer (2026-08-18): "the external
display work is flawed".** The commit is NOT merged and must not be swept into a
release by accident. Left on its branch deliberately.

---

## Host capabilities worth exposing

What the iPhone and iPad hardware actually lets an app reach, and which parts
are worth surfacing to the guest. Surveyed 2026-09-07.

### Bluetooth LE -- the one radio that is genuinely open

**Established.** CoreBluetooth's central role is available to every app with no
MFi programme, no Apple-granted entitlement and no vendor agreement: scan,
connect, discover services and characteristics, read/write/notify against any
BLE peripheral. The peripheral role is open too, and `CBL2CAPChannel` (iOS 11+)
gives a real bidirectional stream over LE credit-based flow control rather than
characteristic ping-pong. Entry cost is one Info.plist key
(`NSBluetoothAlwaysUsageDescription`) and a user prompt.

Classic BR/EDR is closed in the other direction -- no RFCOMM, no SPP, no SDP,
no HCI, no programmatic pairing. Bluetooth serial needs MFi through
ExternalAccessory, the same gate as the port. Keyboards (HID) and audio (A2DP)
are handled by the system and already work in AOK for free. Multipeer
Connectivity and `Network.framework` peer-to-peer do use Bluetooth, but only
between Apple devices. AccessorySetupKit (iOS 18+) narrows the permission
prompt; it does not add capability.

One hard limit: there are no Bluetooth addresses. CoreBluetooth hands out an
opaque per-app UUID that differs between apps and rotates, so anything
`hcitool`-shaped is impossible by construction. Throughput is kilobytes per
second -- fine for sensors and control, useless for bulk transfer.

**Next step** is a `/dev/bluetooth` character device on the `/dev/url` recipe
([[dev-url-scheme-device]]), central role only to start: a line protocol for
scan / connect / read / write / subscribe, plus a guest-side helper tool.
app/LocationDevice.m is 190 lines for a read-only device; this one is read/write
and stateful, so budget several hundred, plus the same five registration points.

Two things to get right:

- **Do not fake BlueZ.** `AF_BLUETOOTH`, HCI sockets and `bluetoothctl` would
  mean synthesising controller-level events out of a GATT-level API, and the
  result reports states no real controller produces -- exactly the failure in
  [[capability-lies-are-load-bearing]]. Ship an AOK-native interface and
  document it as one. A `bleak` backend on top is a reasonable follow-on.
- **App Review will ask why a terminal wants Bluetooth.** The precedent is
  already in the tree: AOK ships `/dev/location` and asks for location
  permission on the same argument -- an opt-in device capability surfaced to a
  scripting environment.

### The Lightning / USB-C port -- mostly nothing to do

**Established.** There is no raw USB access at any tier: no enumeration, no bulk
transfers, no device nodes. What exists, in order of reachability:

- **Free, no code.** USB Ethernet adapters (just a network interface -- the
  wired device-testing link already rides this), USB and Bluetooth keyboards,
  USB audio, external displays.
- **Files plus security-scoped bookmarks.** iOS 13+ mounts USB storage into
  Files, and a document picker can select a folder on it. **AOK already has
  this** -- `iosfs` in app/iOSFS.m is exactly that mechanism. Limits: no block
  device, no `mount(2)`, FAT/exFAT/APFS/HFS+ only, and bookmarks go stale on
  unplug.
- **ExternalAccessory (MFi).** Per-accessory protocol strings declared in
  Info.plist, and the accessory needs Apple's auth chip. Not generic USB.
- **DriverKit (iPadOS 16+, M-series only).** The
  `com.apple.developer.driverkit.*` entitlements are granted by Apple per app
  against a specific hardware justification, and they vanish in unsigned builds
  ([[unsigned-ipa-drops-entitlements]]).

Thunderbolt is not a separate thing to expose: the M-series iPad port is USB4,
but there is no PCIe API on iPadOS. The device side is fully closed -- AOK
cannot present itself to a connected Mac as USB serial or mass storage.

**Verdict: no DriverKit work.** M-series iPad only, an entitlement Apple is
unlikely to grant for this use case, and dead in sideloaded builds -- it would
split the user base for a feature most users cannot run.

**Next step**, if anything: polish `iosfs` for external volumes -- a sane story
when the drive is unplugged mid-session and the bookmark goes stale, and a line
in the docs saying a USB-C SSD can be mounted. Users do not know AOK already
does this. A Redpark-backed `/dev/ttyUSB0` over ExternalAccessory would slot
into the same dyndev recipe and is the one genuinely differentiating port
feature -- iPad plus console cable -- but it needs specific hardware, Redpark's
licensing terms, and it is dead code for everyone without the cable. Only worth
it if the maintainer wants it personally.

---

## Native program candidates

Programs worth compiling in as native code (kernel/native.h), and the one
question that decides most of them.

**The dividing line is the shim, not the program.** kernel/native_libc.h works
by `#define`-ing libc names ahead of the system headers, so it redirects calls
in translation units AOK COMPILES. It does nothing to calls made from a
prebuilt dylib or from another toolchain's objects -- that is exactly what made
zlib's gz* family unusable until deps/smallclue-shim/zlib.h reimplemented it.
So candidates fall into two groups, and the second is a different project from
the first:

- **C sources AOK can compile itself.** bash, zsh, OpenSSH and nextvi are
  already here. Cost is the porting work the gate enumerates
  (`tools/check-native-libc.py --report <objects>`), which is finite and
  visible up front.
- **Anything built by a foreign toolchain** -- Rust, Go, or a vendored build
  system we do not drive. Their `open`/`read`/`write` resolve to the host's at
  link time and no `#define` reaches them.

  **Prototyped 2026-08-20, and it works with link flags alone.** Darwin's
  linker will alias an undefined import onto a symbol we define:

      clang -o prog shim.o foreign.o \
          -Wl,-alias,_nlibc_open,_open \
          -Wl,-alias,_nlibc_read,_read

  An object compiled with no knowledge of AOK then calls `nlibc_*` instead --
  verified against a control build of the same object, which read the host's
  real /etc/hosts while the aliased one read the stand-in guest VFS.
  `llvm-objcopy --redefine-sym` also works, including on static archives (what
  a Rust staticlib ships as), but it rewrites the objects and the alias needs
  nothing but the link line. Caveats and the one open decision are below.

### helix

Requested 2026-08-20. Modal editor, Rust, MPL-2.0 (confirm before any work --
the licence matters here the way GPLv3 does for bash, which is why
`-Dnative_bash` exists at all).

Second group, so it is behind the interposition question above. Beyond that:

- Rust std does its own syscalls, and helix does file I/O throughout -- there
  is no pure/impure split to exploit the way zlib's deflate/inflate allowed.
- LSP servers and formatters are spawned processes. `fork()` is ENOSYS for a
  native program, but exec/spawn works (see [[native-exec-standin]]), so this
  is probably not the blocker it first looks like.
- Tree-sitter grammars are built as loadable objects by default; a static
  grammar build would be needed.
- Size is tens of MB with grammars, against a binary that ships in an app.

AOK already has nextvi and micro native, so this is the "modern editor" slot
rather than a gap. **Next step** is the interposition prototype, not helix
itself -- pick the smallest Rust program that does one `open` and see whether
its objects can be made to call `nlibc_open`.

---

## Large work in flight, tracked in its own document

Recorded here because this file was silent about the biggest thing in the tree
for a whole cycle. These are not TODO entries -- each has a plan document that
is the live record -- but a reader who does not know they exist will misread
everything above.

**Simulated swap.** [docs/simulated_swap_plan.md](simulated_swap_plan.md).
Phases 0, 1 and 2 built and on `working`; phase 3 device validation under way
and already the most productive part of the project -- a 3 GB iPhone SE reached
states the 16 GB iPad never does, and found that AOK's growth guard did not
cover the page-fault path at all, so a guest could commit 704 MiB past the point
where the same total in separate `mmap`s was refused. Fixed by sensing in
`tlb_handle_miss` and acting in `handle_timer_interrupt`. Ships **off by
default**, enabled in Settings with a user-specified size. The plan's own
"still open" lists are themselves stale: `/proc/swaps`, `swapon`/`swapoff` and
`mincore` have all landed since they were written.

**Memory truth.** Landed with the above and worth naming separately, because
the swap plan's section 11 deferred it and that deferral is what the plan is
still written around. `mem_resident_page_count` (emu/memory.c:879) is real, and
`/proc/meminfo` now describes the guest rather than the phone
(fs/proc/root.c:479). Before that a guest on a 3 GB phone read `MemTotal` 2957
MB against a real ceiling of 2098 MB, so `free`, `top` and ktop all showed the
machine as ~100% full at idle and no guest allocator could act on any of it.

**The amd64 JIT.** A full amd64 regression suite now runs with **zero**
interpreter fallbacks, closed opcode group by opcode group with
`/proc/ish/amd64_jit` reporting what could not be compiled. The interpreter
entry points the JIT stopped using are deleted. What remains is the dispatch
loop itself, which is a separate question from the fallbacks.

---

## Reported issues

Checked against GitHub on 2026-09-07: 18 open. The previous version of this
table listed #541 and #542 as fixed while #541 was still open upstream, and was
missing eight issues filed since. **Closing the issue is part of fixing the
bug** -- a fix recorded here and not there is a fix the reporter never learns
about.

### Bugs

| # | Title | Notes |
|---|---|---|
| [#482](https://github.com/emkey1/ish-AOK/issues/482) | Wayland applet does not resize properly | body is a screenshot only. Very likely the same root cause as #483's second half -- confirm before treating them as two jobs |
| [#485](https://github.com/emkey1/ish-AOK/issues/485) | Qt apps (Falkon) cannot connect to session bus | 6 comments |
| [#503](https://github.com/emkey1/ish-AOK/issues/503) | amd64: gdb next/step after a breakpoint crashes with SIGILL | ours. Related to the strace/gdb entry in `build_554_musts.md`: both tools are unreliable against this kernel, and that costs every later diagnosis |
| [#521](https://github.com/emkey1/ish-AOK/issues/521) | Buildroot `make` crashes on "checking for working sigaltstack" | body is a screenshot only |
| [#523](https://github.com/emkey1/ish-AOK/issues/523) | yay (AUR helper) fails on Arch ARM64 | **reported symptom does not reproduce** -- see *Diagnosed* above. What does reproduce is a TLS handshake tail of 15.3 s against a sub-second median, which is a wait not being woken rather than slow work |
| [#527](https://github.com/emkey1/ish-AOK/issues/527) | pikaur fails on Arch ARM64 | blocked on `systemd-run` |
| [#541](https://github.com/emkey1/ish-AOK/issues/541) | ptraceomatic does not run: tracee reaped during setup | **fixed 2026-08-20**, and still open on GitHub. Close it |
| [#568](https://github.com/emkey1/ish-AOK/issues/568) | Network throughput is very slow for downloads and browsing | split out of #559. Two readings with different causes -- guest-side throughput vs device-wide degradation -- and which one it is has not been settled. Same neighbourhood as #523's handshake tail |
| [#572](https://github.com/emkey1/ish-AOK/issues/572) | Cannot determine a usable wildcard IP (Gradle) | Devuan aarch64 on an iPhone 7 Plus, iOS 15. Gradle wants a bindable local address; worth checking what AOK reports for the interface list before assuming it is a name-resolution problem |
| [#575](https://github.com/emkey1/ish-AOK/issues/575) | Unable to delete machines | `Roots` already implements `destroyRootNamed:` (app/Roots.h), so this is a UI gap, not a missing capability. It is also the natural home for snapshot and restore -- see the roadmap |
| [#579](https://github.com/emkey1/ish-AOK/issues/579) | Terminal loses keyboard focus after selecting with a Magic Keyboard trackpad | iPad Air M3, iPadOS 26.6.1. Hurts the desktop use case disproportionately: the pointer and the keyboard are the two things a windowed session depends on |
| [#580](https://github.com/emkey1/ish-AOK/issues/580) | Window controls incorrectly positioned in windowed mode on iPadOS | correct in fullscreen, wrong in a window, so it is a layout-guide bug rather than anything deep |
| [#581](https://github.com/emkey1/ish-AOK/issues/581) | makepkg hangs at "Generating .PKGINFO file..." | reporter's own repro steps are incomplete and say so. Unconfirmed. The step it hangs at is `bsdtar`/`fakeroot` work, which is a different neighbourhood from #523 despite both being AUR |

### Feature requests

| # | Title | Notes |
|---|---|---|
| [#483](https://github.com/emkey1/ish-AOK/issues/483) | Wayland applet: derive desktop resolution from window size | **half shipped.** Standalone fullscreen is in `DisplayViewController`; deriving resolution from the window is not -- the client still asks for a fixed pair. The mechanism already exists: `d60437caf` drives per-orientation resize through RFB `SetDesktopSize` |
| [#484](https://github.com/emkey1/ish-AOK/issues/484) | 3D acceleration via virglrenderer | the largest request on the list. Needs a host GL/GLES implementation that iOS does not have, so it is ANGLE-over-Metal or nothing. Wants a feasibility gate of its own before any estimate |
| [#540](https://github.com/emkey1/ish-AOK/issues/540) | External display support (AirPlay) | see *Deferred on purpose* above -- fenced deliberately, and the branch must not be swept into a release |
| [#556](https://github.com/emkey1/ish-AOK/issues/556) | Updated preset appearances | |
| [#559](https://github.com/emkey1/ish-AOK/issues/559) | Feedback: own icons rather than iSH's, more OS images, QEMU | #568 was split out of this thread; the rest is still one issue carrying several asks |
| [#574](https://github.com/emkey1/ish-AOK/issues/574) | Allow for desktop environments | the stack this asks for already installs -- `opt/AOK/tools/setup-wayland.sh` -- so the gap is packaging and documentation rather than capability |
| [#577](https://github.com/emkey1/ish-AOK/issues/577) | Home Screen start-up options | asks for long-press Home Screen quick actions selecting a startup mode. The Shortcuts/AppIntents work shipped in 550 is the machinery this would build on |

---

## Build and test infrastructure

### Linux CI

**Green again as of 2026-08-19**, both arms of the `[clang, gcc]` matrix.

It had been red since 2026-08-10, which is BEFORE the 548 release -- `e4fe5116`,
the commit tagged 548, was itself red. Never a regression of the 549 cycle, and
it affected no shipped code: `build-mac` and `Build Dev IPA` were green
throughout.

Nearly all of it was one root cause: bash's, zsh's and OpenSSH's `config.h` are
each generated by running configure **on a Mac**, and the tree is compiled for
both platforms, so all three asserted Darwin facts that are false on glibc.
iconv lives inside libc on Linux; `<sys/sysctl.h>` and `<sys/filio.h>` are not
glibc headers; `st_atimespec`, `d_namlen` and `fpurge` are Darwin spellings;
`strtonum`, `timingsafe_bcmp`, `memset_s`, `<util.h>`, the `pw_class` family and
`sin_len` are BSD's. Every such macro is now behind `!__linux__`, so the shipping
build is bit-for-bit unmoved. The rest:

- `__thread` must FOLLOW the storage class for gcc -- 40 declarations, mostly in
  the vendored OpenSSH;
- `-D_GNU_SOURCE` project-wide, for `off64_t`, the `cookie_io` typedefs and
  `RUN_LVL`;
- the xattr port, which was a real port and not a config guard: Darwin's calls
  carry a position and an options word and Linux's do not, so the shim now
  declares the shape each platform actually has;
- `<rpc/types.h>`, which OpenSSH asks for and Debian hides in libtirpc -- one
  missing header accounted for 181 of the original 186 failures;
- the fused i386 ALU gadgets, which exist only in aarch64 assembly, so merely
  naming them was a link error on any x86_64 host;
- a duplicate `smallclueRunRsync`, which ld64 quietly tolerates and GNU ld does
  not.

One of the fixes was not a build fix at all. GCC rejected an assignment clang
waves through and turned up a live crash on iOS: `bash --rcfile FILE` and
`--init-file FILE` wrote through a NULL pointer, and native bash runs in-process,
so that is the app going down rather than a shell. See `deps/bash` `a097512`.

Verified by cloning the pushed branch fresh on Debian 13 and building it exactly
the way CI does, with each compiler: 0 failed targets, `float80` and
`riscv64_decode` pass, `e2e` passes.

### `time_conformance` fails only in a full-suite run

Seen 2026-08-20 in a full tier0 sweep: x86_64 reported `time_conformance: FAIL
failures=3`, and the same test passed three times out of three when run alone
immediately afterwards. It is a timing test, the full sweep loads the machine,
and nothing in that run touched clocks -- the only kernel change was
pidfd_open. Recorded rather than diagnosed: if it starts failing alone, it is a
real regression and this note is the date it was not one.

The conductor keeps no per-test log, so the three failing assertions were not
recoverable after the fact. Worth fixing if this recurs -- a failing test that
cannot say what it checked costs a re-run every time.

### Regression-suite observations

From the 4-arch on-device run, 2026-08-19 (aarch64 booted 118/118 clean; i386
110/6, x86_64 112/5, riscv64 103/5):

- **Five failures are identical on every chroot arm and absent from the booted
  arm**: `devtmpfs_mount`, `proc_pid_io`, `taskstats_genl`, `mount_stdev`,
  `fifo_open_creat_deadlock`.
- `mount_stdev` and `devtmpfs_mount` are proven chroot artifacts: they fail in
  an **aarch64** chroot, the same arch that passes them when booted. AOK has no
  mount namespaces, so `/proc` inside a chroot describes the booted root while
  `stat()` sees the chroot's.
- The other three pass when run **by hand** inside the same chroot, and failed
  when x86_64 ran **alone**, so it is the chroot plus the suite runner -- not
  contention and not architecture. Worth understanding before anyone reads them
  as product bugs.
- `mount-root.sh` bind-mounts `/AOK/tools` but not `/AOK/tests`, so
  `setup-regressions.sh` cannot find its sources inside a chroot without a
  manual bind. Small gap worth closing if this becomes routine.
