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

## Queued for a future release

The one queue for follow-up work. Sessions add a bullet here instead of
raising task chips (the maintainer's rule, 2026-09-26): what was seen, the
evidence, and the proposed fix. Check here, and `git log origin/working`,
before starting anything. When a session, chip or subagent takes an item,
put `**[in progress: <who>, <date>]**` at the start of its bullet and commit
that before the work; when the work lands, delete the bullet, since the
commit is the record. The first ten were chips not started before the 556
freeze (2026-09-25); their full text is in
[future-release-queue.md](future-release-queue.md).

- **Implement ENTER and 16-bit branch EIP truncation on i386 JIT.** While fixing 16-bit PUSH/POP on the i386 JIT, I found ENTER (C8) is SIGILL there, and 0x66 near JMP rel/Jcc/LOOP/JCXZ keep the full target where x86 truncates EIP to 16 bits. This session would measure both on camd and fix them with a test.
- **Fix x86_fp_env failures on the x86_64-host gadgets.** Running the x86 guest tests under a GCC/x86_64-host build on camd, x86_fp_env fails two SSE checks (ucomisd qnan, divsd FTZ), with or without today's changes; aarch64 builds pass. This session would find and fix the x86_64-backend cause.
- **Make meminfo Shmem/AnonPages/Mapped Linux-shaped.** While making /proc/meminfo cheap, a side-by-side run against Linux 6.12 showed AOK's Shmem, AnonPages and Mapped mean something different: mapped entries per process from mmap, not resident pages once each. A new session would change the accounting to Linux's meaning and update the test.
- **Raise #GP for privileged and misaligned x86 instructions.** While fixing how iSH-AOK reports a #GP, the camd oracle showed several instructions Linux faults with #GP that iSH-AOK either answers with SIGILL or runs silently. This session would decode them and raise #GP(0) on both x86 engines, with tests measured against camd.
- **amd64: int n, sigreturn CS/SS, non-canonical jumps.** The #GP oracle work found three amd64 gaps: every `int n` (CD) is SIGILL, rt_sigreturn ignores a broken CS or SS, and a jump to a non-canonical address is reported at the target instead of the jump. This session would fix all three against measured camd behaviour.
- **Match Linux's x86 page-fault REG_ERR and CR2.** The #GP oracle runs also showed the page-fault frame's error-code bits differ from Linux (a plain read of a PROT_NONE page claims to be an instruction fetch), and CR2 is not kept across frames. This session would make REG_ERR and REG_CR2 match camd on both x86 ABIs.
- **Add PTRACE_POKEUSER and base checks to ptrace.** While adding the amd64 GS base, I found iSH-AOK has no PTRACE_POKEUSER at all (any ABI), and SETREGS accepts any fs_base/gs_base where Linux says EIO. The new session implements POKEUSER for amd64 and i386 and the base validation, oracle-checked on camd.
- **O_PATH opens of /proc magic links to a pathless file.** While giving memfds and unlinked files descriptions of their own, I found `open("/proc/self/fd/N" or "/proc/self/exe", O_PATH)` still walks the link's text (procfd_openat leaves O_PATH to path resolution): ENOENT for a memfd, the stale name's file for an unlinked one. This session would give O_PATH the same inode Linux does, measured on camd.
- **Keep /proc/<pid>/exe of a memfd image across a checkpoint.** An image started from a memfd saves its exe as the path "/memfd:name (deleted)", which restore cannot open, so the link is empty afterwards. This session would carry it by the memfd's checkpoint identity when that memfd is in the image.
- **Make checkpoint_anonfd.sh wait for the guest.** It checkpoints 2 s after start; on build/alpine-arm64-test that is before the probe runs ("refused: there is no guest running"), before and after today's changes. This session would have the probe signal readiness and the harness wait for it.
- **Give realfs files a guest owner a non-root user can work with.** Found by the 556 device leg (uid 1000): realfs reports the host's uid (501) as the owner of every file (fs/real.c copy_stat, unchanged since 2017), so a non-root guest user is "other" even on a directory it just made in `/AOK/persist`, and cannot create anything inside it (`mktemp -d /AOK/persist/x.XXXXXX; touch $d/f` is EACCES). Root never sees it, so no Mac run did. fs_ctime_updates now skips its realfs leg when it cannot write there and runs as root on a device (needs_root_tests). The fix is a design choice -- a vfat-style `uid=`/`gid=` owner per mount, the default user as owner of host-owned files, or ownership kept in a side table -- to be made and measured against Linux's vfat/exfat behaviour.
- **Give the guest a CLOCK_REALTIME a file stamp is never ahead of.** Found by the 556 device leg: vdso_clock's "a file's mtime is never in the future" check failed on the M4 iPad (17 of 300 files, by up to 164 ns). Not a vDSO bug -- the system call reads the same host clock (clock_gettime_host_backed). Darwin's userspace wall clock has whole-microsecond resolution only (clock_gettime, clock_gettime_nsec_np and mach_get_times alike; getres says 1000 ns), while APFS stamps files in nanoseconds, so a file written in the same microsecond as a later read is dated after it. The Mac host shows it natively in 60% of tries (up to 749 ns); the emulated guest on the Mac is too slow to land in one microsecond, the device's JIT is not. Linux reads REALTIME in ns and stamps files from a coarse clock, so it never happens there. Candidate fixes: report each REALTIME read at the end of its microsecond (Linux's ordering; mind vdso_clock's gettimeofday bracket check), or synthesize a ns wall clock from mach time recalibrated against the µs one. Deferred from 556 by the maintainer (2026-09-26).
- **[in progress: "Build 557" session (chip), 2026-09-26]** **Boot a chosen root for one launch, so a device leg can boot each root.** Asked by the maintainer during the 556 device leg (2026-09-26): the four Alpine roots run their suite inside a `mount-root.sh` chroot of the booted Devuan one, so a device leg tests chroot behaviour, not each root booted, and three of its failures were chroot-only. Add a launch environment variable (e.g. `ISH_BOOT_ROOT=<name>`, read like `ISH_SESSION_RESUME`) that boots the named root for that launch without changing the saved default, so `xcrun devicectl device process launch -e '{"ISH_BOOT_ROOT":"Alpine3.23.3"}' --terminate-existing app.ish.iSH-AOK` boots it; then run the device suite once per booted root.
- **Walk through a /proc link to a target outside the caller's chroot.** Linux jumps straight to the file (nd_jump_link), so a chrooted process reaches a descriptor opened outside through `/proc/self/fd/N/...` or `/dev/fd/N/...`. AOK re-walks the link's text, and outside the root that text is "(unreachable)/..." (fs_rebase_readlink_path) because the global path, walked inside the chroot, would reach a different file. So exec_link_by_fd's detached case (`/bin/sh` opening `/dev/fd/3/script`) fails inside a mount-root.sh chroot (556 device leg, Alpine i386). Fix: let the walk resolve such a link from the real root (N_REALROOT) or from the descriptor itself, then drop the marker.
- **Settle the device-only failures carried since 555.** Every device leg of 555 and 556 has failed these, and only in the Alpine roots, which run in a `mount-root.sh` chroot of the booted Devuan root: `mount_bind_rbind` (`rbind.self_mounted`, all four), `futex_timeout_duration` (i386: the FUTEX_WAIT_BITSET +500 ms deadlines), and `kmsg_stream`/`kmsg_records` (arm64 and riscv64 in 555; x86_64 too in 556). The kmsg pair is diagnosed: the booted Devuan runs `rsyslogd`, which reads `/proc/kmsg` destructively and takes the record `kmsg_records` waits for (the same trap as on camd), and `atop`/`atopacctd` keep adding kernel-log records, so `kmsg_stream`'s "drained" log never stays drained. Those tests need to detect another `/proc/kmsg` reader and bracket their drain, not a kernel change. The other two: re-run on a booted root once the launch-time root selector exists, and fix whatever still fails.
- **Show a zombie's architecture in ktop.** Seen on the device during the 556 suite (reparent_zombie_*): ktop's ARCH column read `?` for a moment. `/proc/ish/arch` lists live processes only, and ktop's fallback reads `/proc/<pid>/exe`, which a zombie no longer has. This session would decide whether `/proc/ish/arch` keeps a zombie's last architecture until it is reaped, and make ktop show it.

## Diagnosed, not fixed

### A native bash ignored SIGKILL (unconfirmed, seen once)

While reproducing the above, a `/AOK/native/bash` at pid 16 survived
`kill -9 16` and was still in `ps` a second later; it went away only on a cold
boot. Not chased and not confirmed -- the kill's own exit status was not
checked -- and it may be the known shape of a native program blocked in a host
read rather than a regression of the fix in [[native-spawn-unkillable-task]].

**Retry it before believing it.** That observation predates the blocking-path
work later the same day: a task blocked reading a pipe or a socket was deaf to
the checkpoint freeze (fs/sock.c and fs/real.c asked only about guest signals,
bfbefefc8), and a long guest timeout left the host wait unbounded (251465dba).
A native bash sitting in a host read was in exactly that state, so "ignored
SIGKILL" may simply have been the same deafness seen through a different lens.
Reproduce on a current build first; if it still happens, the freezer's host
backtrace (ISH_CHECKPOINT_DEBUG, [[stuck-task-host-backtrace]]) will say where
it actually is rather than leaving it a mystery.

### EPOLLET and SOCK_SEQPACKET: what the 2026-09-25 fixes left

The edge-triggered epoll fix (fs/poll.c `poll_drain_host_locked`) and the
SEQPACKET framing (fs/sock.c `struct unix_seqpacket_hdr`) left three measured
divergences, each judged not worth its cost yet.

**Established**:
- A read that leaves data behind in a host pipe or FIFO re-arms the host's read
  event (Darwin's `pipe_read` and its fifofs both do it; measured with a bare
  kqueue, sockets do not), so an `EPOLLET` reader that does a partial read is
  told `EPOLLIN` once more with no new data. Linux says nothing. Harmless to a
  reader that reads until `EAGAIN`, which an `EPOLLET` reader must;
  `epoll_edge_triggered` checks partial reads on sockets and ptys only.
  Filtering it means telling the re-arm from a real write, which needs a byte
  count kept across every guest read of the pipe -- and getting that count wrong
  loses a real edge, which hangs, where the re-arm only costs a wake.
- dup'd fds registered in one epoll, one `EPOLLET` and one not, share a host
  watch that has to be level-triggered for the level one, so the edge-triggered
  one behaves level-triggered (`poll_fd.host_edges` stays false for both).
- A SEQPACKET `MSG_PEEK` does not deliver the message's `SCM_RIGHTS`; the real
  read does. Linux clones the descriptors for a peek. AOK's unix datagrams
  behave the same way, on purpose (see the peek comment in
  `sys_recvmsg_guest_abi`).

**Next step**: none until a program is seen to need one of them.

### The app's UI thread impersonates a guest process

`current` is per-thread, and on the app's main thread it is whatever the last
`TerminalViewController.startSession` left there -- the session's own first
process -- or init after a boot. The thread is not that process, and kernel
code it calls acts as one.

**Established** (2026-09-11): clearing it is a one-line change and it does not
work. Suspend to disk needed the UI thread not to be mistaken for a task
(`ckpt_freeze_all` skips `current`, so the backgrounding save skipped the
session leader). Clearing it in `startSession` fixed that and crashed
Settings -> Appearance the same day: `pty_slave_init_inode` reads
`current->euid`, and the appearance preview creates its pty from the UI thread.
The audit that followed found the surface is wide -- `CurrentRoot`,
`AudioLibrary` and `MotePadDocumentStore` all call `generic_open` /
`generic_statat` / `generic_renameat` with `AT_PWD` from that thread, resolving
paths through `current->fs` and checking permission as `current->fsuid`, none of
them borrowing a task first.

So the clearing was reverted and the checkpoint instead clears `current` for
the duration of its own call (`checkpoint_save_external`), which is correct and
scoped.

**Next step**: give every app-side kernel caller on the UI thread
`AppDelegate`'s `pushUsableInitTaskAsCurrent` / `popCurrentTask`, which two
callers already use (`UpgradeRootViewController`, and `AboutAppearance` since
this crash). Then clearing `current` in `startSession` becomes safe and the
invariant is "the UI thread is not a process; borrow one if you need to be".
Worth doing as its own change with the whole surface audited, not as a rider on
something else.

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
- **A name that does not exist costs nine requests, not one.** Measured
  2026-09-17 with the daemon in `tests/manual/blocked_wait_state.c`: one
  `stat` of a missing name in the mount root sent READLINK and GETATTR of the
  root, the LOOKUP, then READLINK, GETATTR, READLINK and GETATTR of the root
  again and the LOOKUP twice more. Linux 6.12 (camd, as root) sent the one
  LOOKUP. Two things are separable from the cache: READLINK of a node whose
  attributes already say it is a directory, and the walk being repeated after
  a failed lookup. The repeat is also what kept a poke's spurious EINTR on a
  FUSE request from reaching the guest before `wait_for_blocked` (the daemon
  still got the FUSE_INTERRUPT), so find out what it is for before removing
  it.
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

### Darwin compresses our memory already, and it buys us nothing

Measured 2026-09-09 on this Mac, because "should AOK compress guest memory?"
turns on whether the host is already doing it for us. It is -- and the result is
the opposite of the obvious one.

**Darwin compresses idle anonymous memory with no memory pressure at all.** A
process that dirties 1 GiB and then does nothing has essentially all of it in
the compressor within ~25 seconds:

```
COMPRESSIBLE (repeating byte)      INCOMPRESSIBLE (random)
after dirtying  fp=1025.4 c=   0.0    after dirtying  fp=1025.4 c=   0.0
after 25s idle  fp=1025.4 c=1024.2    after 25s idle  fp=1025.4 c=1023.9
```

(`fp` = `task_vm_info.phys_footprint`, `c` = `.compressed`, MB. Interleaved A/B,
two rounds.)

**And `phys_footprint` does not move.** 1025.4 MB in every arm -- whether the
gigabyte compresses ~infinitely or not at all, whether the compressor has taken
it or not. The ledger charges for the pages regardless of how well they
compressed.

**Why that matters more than it looks.** jetsam kills on `phys_footprint`
(platform/darwin.c says so, and the swap budget is measured against it, not
RSS). So the host's compression -- which is already happening, for free, to
every cold guest page -- **buys AOK no headroom whatsoever**. It cannot be
relied on to keep the app alive, and no amount of making guest memory more
compressible will help by itself.

**Which inverts the design question.** AOK-level compression is not redundant
with the host's; it is the only kind that can help, because AOK would compress
into its *own* smaller buffer and then actually release the originals -- and
released pages do move the footprint. That is zram's model: hold N pages'
worth of data in a pool of roughly N/ratio, and the footprint falls by the
difference. It also composes with the pager: compress in RAM first, and only
spend flash when the compressed pool is full, which cuts writes by the same
ratio (see the swap write budget).

**Not yet established, and needed before building anything:** the compression
ratio and CPU cost on *real guest pages* rather than synthetic ones, the added
latency on the fault path, and confirmation on a device that iOS's
`phys_footprint` behaves as macOS's does here. The measurement above is macOS
and uses `malloc`, not guest memory through AOK's page tables.

### Guest-memory compression: BUILT (phases 1-2), and what is left

**Phases 1 and 2 are in** as of 2026-09-09. `kernel/zpool.c` is a size-classed
pool for compressed frames (host unit test, `meson test -C build zpool`), and
`kernel/zswap.c` puts it in front of the swap area by intercepting
`swap_slot_write`/`read`/`free`. Nothing about eviction eligibility, the fault
path, fork/COW or the address-space barrier changed -- that seam was already a
backing-store interface, which is why the integration is three call sites.

Verified end to end by `tests/manual/zswap_roundtrip.c`: 24 MB survives a
compressed round trip byte-for-byte, and the test FAILS rather than skips if no
frame went through the tier. Run at a 1 MB cap it also covers the mixed case --
640 frames held in RAM, 896 overflowed to flash, all correct.

**UNDER A REAL DATABASE, WITH DECOMPRESSION ON THE CRITICAL PATH.** The runs
above evicted memory and left it alone; nothing faulted back in any volume, so
the decompress path was barely exercised (46 loads). This one puts it under
sustained load. MariaDB 11.8.6 on the same iPad, 512 MB InnoDB buffer pool,
sysbench 1.0.20 `oltp_read_write` over 2 tables x 394k rows (188 MB), two
threads.

Memory was pushed down until kswapd evicted part of the buffer pool -- reclaim
fired at headroom 478 MB against the 482 MB watermark, **the second independent
confirmation of that threshold** (nothing at 494, fired at 478) -- then sysbench
was run against a buffer pool that was partly compressed:

```
                      TPS    avg ms   95th ms   errors   zswap loads
cold baseline        3.48    571.89    787.74        0     64 ->  64
partly compressed    4.42    452.25    707.07        0    162 -> 674
```

**512 frames were decompressed on the fault path during a 90-second run, with
zero errors.** That is the correctness result: sysbench's point-selects and
updates run against InnoDB pages that went out through the compressor and came
back, and a decompression fault would surface as a query error or a wrong row,
not silently. It did not.

`declined` stayed 0 and `bytes_written` stayed 0 across the whole exercise, so
real InnoDB pages compress as well as the synthetic ones did and none reached
flash.

**Do NOT read the TPS column as "compression makes it faster."** The two runs
are not a controlled A/B: the baseline ran immediately after the data load with
a cold buffer pool doing real disk I/O, and the second had a warmer one. The
defensible claim is the weaker one -- **serving 512 faults from the compressed
pool cost no measurable throughput or latency** -- and that is the question a
user actually has.

**Ratio on a mix including real InnoDB pages: 2.42x.** The reclaim added 7,278
frames (113 MB of guest memory) for 46.8 MB of pool growth. That is close to the
2.38x measured for cc1 on the Mac and above the 2.11x of the synthetic run, so
database pages compress at least as well as the text records did.

**THE CLEAN DEVICE RUN, 2026-09-09.** iPad 5th gen (A9, 1.45 GB), app in the
foreground, no debugger attached, swap 1 GB, pool cap 128 MB, enabled from
Settings. A single non-forking probe (`zprobe`) allocated 64 MB at a time and
sampled after each step:

```
+768MB   headroom=490MB  stores=0     poolKB=0      bytes_written=0
+832MB   headroom=453MB  stores=3754  poolKB=28960  bytes_written=0   <- engaged
settle+10s  headroom=484MB stores=7874 poolKB=61744 bytes_written=0
settle+180s headroom=483MB stores=8132 poolKB=63776 bytes_written=0
```

**Four things, and all four are what the feature promised:**

1. **Reclaim engaged exactly where predicted.** The watermark is
   `available < 2 x host_mem_headroom_floor` = 482 MB. Nothing happened at
   490 MB; it fired at 453 MB. The threshold is not approximately right, it is
   right.
2. **Zero flash writes.** `bytes_written` stayed at 0 for the entire run, and
   `declined` stayed at 0 -- every single evicted frame went to RAM. On a
   feature whose headline objection is flash wear, that is the number.
3. **THE FOOTPRINT ACTUALLY MOVED.** Headroom recovered 453 -> 484 MB and held
   there for three minutes. This is the measurement the debugger would have
   destroyed: `MADV_FREE_REUSABLE` returns success while moving no ledger under
   an attached debugger, so a recovering headroom is the only proof the frames
   were released rather than merely accounted for.
4. **It reached equilibrium and stopped.** 484 MB is just above the 482 MB
   watermark, and reclaim ceased there rather than continuing to evict. The
   pool used 60 MB of its 128 MB cap. That is a pager doing the right amount of
   work, not the most.

**The effective ratio is 2.11x, with fragmentation counted.** 8132 frames x
16 KiB = 127 MB of guest memory held in 61,744 KB of pool. That is the honest
figure -- `poolKB` is what the slabs occupy, so size-class waste is already in
it. It sits just below the raw 2.38x measured for cc1, which is what ~10%
fragmentation predicts. The probe's data is structured text records, so it is
somewhat more compressible than a binary heap; treat 2.11x as a good case and
not a ceiling.

**One number is not fully explained and should not be smoothed over.** 127 MB of
frames released into 60 MB of pool should free about 67 MB, and headroom
recovered 31 MB (memfree agrees: +32 MB). The gap may be `MADV_FREE_REUSABLE`
pages counting as reusable-but-not-yet-free, or accounting differing between the
two figures. Not chased, and flagged rather than averaged away.

**IT WORKS ON A DEVICE, UNDER REAL PRESSURE.** Measured on the iPad 5th gen
(A9, 1.45 GB) on 2026-09-09, with the tier enabled from Settings at a 128 MB cap
and swap at 256 MB. Memory was consumed until the machine crossed kswapd's
watermark -- which is `available < 2 x host_mem_headroom_floor`, so 482 MB
against the 241 MB floor:

```
headroom 483 MB   pressure WARN (throttle engaged, growth still allowed)
t+10s   stores=13419   bytes_written=0   headroom=482
```

kswapd engaged at the predicted threshold to the megabyte, evicted **13,419
frames** -- 13419 x 16 KiB, about 210 MB of guest memory -- and
**`bytes_written` stayed at 0**. Not one byte reached flash. That is the whole
claim of the feature, on real hardware, under pressure that arrived on its own
rather than being forced through a development control.

**AND THEN THE DEVICE STOPPED ANSWERING -- BUT NOT, IT TURNS OUT, BECAUSE OF A
JETSAM KILL.** An earlier version of this entry said it was one. That was not
established and is probably wrong: the maintainer found the app **backgrounded,
not dead**, and iOS destroys a backgrounded app's listening socket, which is
already documented in this file. "Connection reset", then "timed out during
banner exchange", is exactly what that looks like from the other end -- and it
is a far better fit than the memory-pressure story I reached for, which was that
fork was failing under pressure. Both readings explain the symptom; only one of
them was checked, and it was not mine.

**So no jetsam kill is confirmed at any point in this work.** What is confirmed
is that the app stopped being reachable over ssh while under heavy memory
pressure, twice. The practical consequence for anyone repeating this: keep the
app in the FOREGROUND for the duration, or the guest is suspended out from under
the measurement.

There is still a design point worth keeping, and it does not depend on how the
app stopped:

**THE POOL IS RESIDENT MEMORY AND COMPETES WITH WHAT IT SAVES.** Compressing
210 MB into a pool of at most 128 MB saves at most 82 MB; it does not save 210.
The pool's own bytes are charged to `phys_footprint` exactly like the frames it
replaced. So a cap that is generous relative to the device's RAM can make
pressure worse rather than better, and the ceiling this shipped with -- 4 GB,
chosen against swap's 16 GB -- is far too generous for a 1.5 GB device to be
offered without guidance.

**AND THE BINDING CONSTRAINT WAS THE SWAP AREA, NOT THE POOL.** ktop's last
frame before the kill settles it:

```
Mem[  1.18G/1.42G ]      Swp[  208.2M/256.0M ]
  827  /tmp/cold 400   VIRT 411728  RES 305232
  882  /tmp/cold 450   VIRT 462928  RES 356432
```

208 of 256 MB of slots were consumed -- 81% -- with `bytes_written` still 0, so
every one of those slots held its data in RAM. About 190 MB had genuinely left
the two cold processes (VIRT minus RES). But **slots are allocated per evicted
frame whether or not the bytes go to flash**, so the 256 MB area was about to
run out, and when it does eviction stops completely however much pool is left.

That is the sizing rule, and it is not the obvious one:

- **The swap area size caps how much memory can be evicted at all.** It is the
  address space of the pager.
- **The pool size decides how much of that costs RAM instead of flash.**

So the two are not alternatives and the area cannot be made small (which an
earlier version of this entry wrongly suggested). Against 850 MB of demand, a
256 MB area could never have kept up no matter what the pool did.

**Open, and it is the next thing to settle**: whether the pool cap should be
clamped against device RAM, and whether the UI should relate the two sizes at
all rather than offering them as independent numbers. Unresolved here because
the evidence does not distinguish "the pool made it worse" from "the area was
too small and I allocated too much too fast" -- and picking one without the data
is exactly the kind of story this file exists to prevent.

**ZRAM VERIFIED ON A DEVICE, 2026-09-09.** Everything about the file-less mode
had been proven on the CLI, where /proc/ish/swap_evict can force an eviction --
a control that is EPERM on an installed app by design. So on the iPad 5th gen,
with swap OFF and compressed memory ON from Settings, memory was consumed until
kswapd engaged on its own and then every allocated region was read back:

```
loads      201 -> 11389     11,188 frames faulted back OUT OF THE POOL
bad bytes  0                every one byte-for-byte correct
stores   14038 -> 25373
bytes_written  0            nothing reached storage, because there is no file
```

That is roughly 175 MB of guest memory compressed, released, and restored
exactly, on the oldest hardware AOK supports, with no swap file in existence.

**THE FIRST ATTEMPT REPORTED PASS AND PROVED NOTHING**, which is worth recording
because it is the third instance of the same mistake in this feature's history.
It verified only the region it had filled first, and reported `loads 70 -> 70` --
the counter never moved, so the bytes it checked had never left RAM. 14,038
frames had been evicted; none of them were the ones being checked. The pass
condition was `bad == 0 && stores > 0 && bytes_written == 0`, which neglected the
one thing that mattered.

Fixed by verifying EVERY allocated region rather than one, and by requiring
`loads` to move for a PASS -- it reports INCONCLUSIVE otherwise.

**The rule, stated because it caught three separate green results here:** for a
feature that only acts under a condition, the test must assert THE CONDITION WAS
REACHED, not merely that nothing broke. `swap_roundtrip` passed with 4096 frames
declined and 0 stored; a sysbench run showed 3.75 TPS with the tier never
engaging; and this reported clean bytes that never left memory. In all three the
counters, not the assertion, were what exposed it.

**Also observed: reclaim lags a fast allocator.** `stores` stayed at 0 through an
entire 896 MB allocation and only climbed once it stopped. That is the
second-chance clock working as designed -- a frame must survive two sweeps
untouched before the third may take it -- but it means the tier protects against
sustained pressure rather than a burst that outruns kswapd. Worth saying in
release notes, so a user who hits a limit during a fast allocation does not
conclude the feature is broken.

**RAM-ONLY IS BUILT** (2026-09-09), so there are two modes and both ship:

| Settings | mode | storage cost |
|---|---|---|
| swap on + compressed memory on | **zswap** -- pool in front of the file | area preallocated, writes ~0 |
| swap **off** + compressed memory on | **zram** -- pool is the only storage | **none, ever** |

No new switch: "Enable Compressed Memory" works either way. The zram mode exists
because requiring swap was an awkward ask -- enabling swap costs flash
immediately, since the area is `F_PREALLOCATE`d and `ftruncate`d to full size
before a page is written, so a user whose worry is wear or free space had to
hand over a gigabyte of storage to turn on the feature whose point is not
writing to storage.

It was small because the eviction path was already right: a refused
`swap_slot_write` frees the slot and leaves the frame resident
(emu/memory.c:3669), so "pool full" and "does not compress" simply mean that
frame stops being evictable. Nothing lost, nothing written. What made it
invasive was `swap_fd >= 0` doing double duty as "does an area exist" -- six
sites meant that and now ask `swap_area_live_locked()`, which tests the bitmap,
allocated and freed with the area in both modes.

**In zram mode an incompressible frame is simply never evicted.** There is
nowhere for it to go, and that is correct rather than a limitation: it stays
resident, exactly as it would with the feature off. Demonstrated by
`swap_roundtrip` SKIPPING in that mode -- its pattern is a per-byte hash, so all
4096 frames were declined and none moved.

**All four configurations verified separately**, because they exercise different
paths: default (201/201 guest suite, zpool unit test); swap only (round trip
PASS with 67 MB genuinely written, so the file path still does real I/O); zswap
(all three tests PASS); zram (round trip and fork invariant PASS, zero bytes
written).

**The Settings design follows from that**: the two sizes are separate knobs, and
the swap-file one should be allowed to be small rather than implying a large
area. Not yet wired -- `ISH_GUEST_ZSWAP_MB` is a launch variable, so the tier is
reachable from the CLI and Xcode and not from an installed app.

### 555: zram was lazy, because it inherited a swap file's caution

**"It reached equilibrium and stopped" was recorded above as the fourth thing
the feature got right. For a swap FILE it is. For RAM-only it was the bug.**

Measured on the ip5 device on 2026-09-10, with compressed memory on, swap off,
and the pool cap raised to its maximum from Settings:

```
kswapd   running, 864 passes, 0 bytes reclaimed
pool     0 KB of a 494 MB cap        headroom 931 MB    ceiling 1450 MB
write_window  0 of 4294967296 bytes used in the last 24h
```

**864 background passes, a half-gigabyte pool the user had deliberately asked
for, and not one frame ever compressed.** The watermark was
`available < floor * 2` = 482 MB, and the app sat at 931 MB.

Every gate around eviction was designed for a file: reclaim writes to the user's
flash, that is metered against a 24-hour budget, and flash wears out. Waiting
until the app is nearly dead is right when each eviction costs a write. **In
RAM-only mode none of that is true.** `swap_write_frame` takes the `ram_only`
branch and returns `_ENOSPC` before it reaches a file descriptor -- which is
what `write_window 0 of 4 GiB` after 864 passes actually says.

**A watermark was the wrong SHAPE, not merely the wrong number.** Two attempts
at one failed the same way, and both are worth recording because the second
looked convincing:

1. *Half the budget.* On the device that moves the trigger from 482 MB to
   725 MB of headroom -- and the device was at 931 MB, so it still did nothing.
2. *Half the budget with hysteresis*, a low/high pair so a run continues once
   started. That fixed a real defect on the way past -- a bare threshold does
   not shed memory, it hovers: measured with a 400 MB hog against an 800 MB
   budget, headroom sat at **401 MB against a 400 MB mark** for a minute with
   kswapd taking 24 passes and reclaiming 0 bytes. But it still begins with
   "wait until enough is gone".

Any threshold against remaining headroom encodes waiting, and there is nothing
to wait for. **So RAM-only reclaim has no watermark at all: it runs while the
pool has room and stops when it is full.** The cap is a size the user chose;
filling it with cold frames is what choosing it asked for.

What keeps that honest is not a headroom test but three things that already
existed, plus one that did not:

- the **aging clock**, which offers only frames that have read cold across
  several sweeps, so hot memory is never a candidate;
- the **thrash guard**, which pauses reclaim outright when evicted pages come
  straight back, whatever the headroom says;
- the **pool-full check**, because at capacity every further eviction
  compresses, is declined for want of room, and leaves the frame resident --
  CPU spent, nothing moved;
- a **housekeeping cadence**, which is the new one and was not optional.

**Why the cadence was needed.** A sweep is not free even when it reclaims
nothing: it takes a task snapshot and an address-space barrier per mm, and that
barrier is paid by the guest's own threads -- the same mechanism that makes
mallocng's mmap/munmap churn expensive. An empty-sweep backoff was tried first
and does not help, because the common case is not "nothing cold" but "a trickle
of newly-cold memory": measured, **60 passes in 30 s for 12 frames**, with every
productive pass resetting the backoff. So unpressured reclaim now sweeps on one
pass in four, and under real pressure on every pass. Measured after:

```
over 30 s unpressured: passes=60 sweeps=6
```

**The cost of being wrong is small, and was measured** on that device against
MariaDB's live 684 MB (175,209 pages):

```
lz4    compress   2.46 us/page   decompress   9.70 us/page
```

A 16 KiB frame costs ~10 us to compress, so the whole 494 MB pool is well under
a second of CPU, once -- roughly 0.4 J against a ~7 Wh battery. A frame faulted
back costs ~39 us: 0.4% of one core at 100 frames/s, 3.9% at 1,000. Compressing
is close to free; picking hot frames is what would cost, and the clock and the
thrash guard are what prevent that.

`/proc/ish/swap` now reports **sweeps as well as passes**, because with a
cadence those are different numbers and the gap between them is the policy.

**`zswap_fork_cow` had to change, and the reason generalises.** It asserted that
a forced sweep with a fork outstanding did not move the `stores` counter. That
became unfalsifiable the moment reclaim stopped waiting for pressure: kswapd now
runs continuously, so `stores` moves for reasons unrelated to the region under
test, and it failed on a kernel whose COW handling was correct (stores
545 -> 549, invariant intact). **A global counter cannot attribute a store to a
particular frame.** It now does what its first version did and what the counter
was only ever a proxy for: the child writes through the sharing and the parent's
view must be untouched. Immune to background reclaim, and it fails for exactly
one reason.

`tests/manual/zram_idle_reclaim.c` locks the new behaviour in, and asserts it
**where there is no pressure** -- it refuses to run unless headroom is well
above the old file-backed mark, and fails if nothing is stored while it stays
there. Testing under pressure would have passed before the fix and after it.
Measured: `stores 288 -> 545 after 11 s, with headroom never below 982 MB
against a 400 MB file-backed mark`.

**Still open:** the pool cap default is a flat 128 MB, and the maximum is
physical RAM / 4 -- which is why a device asking for 512 MB gets 494. The
maximum scales; the default does not, and the clamp is silent.

### Phase 0, the measurements the above rests on

Follows the entry above -- the host's own compression buys AOK nothing, so only
compression AOK does itself can help. `kernel/memcomp.c` and
`/proc/ish/mem_compress` measure what it would buy, on real guest pages.
Measured 2026-09-09 on an M4 Mac, every round trip decompressed and compared
(`verify_failures 0` throughout, and that guard earned itself twice -- see
below).

**cc1 compiling 28 MB of C, 90 MB resident anonymous -- the representative one:**

| algo  | ratio | compress | decompress | >=8x | >=4x | >=2x | >=1.33x | worse |
|-------|------:|---------:|-----------:|-----:|-----:|-----:|--------:|------:|
| lz4   | 2.23x |  7.06 us |    1.89 us | 3573 | 1666 | 5191 |   12732 |    22 |
| lzfse | 3.23x | 55.56 us |    9.47 us | 4580 | 3078 |14885 |     641 |     0 |
| zlib  | 3.60x | 70.90 us |   13.55 us | 5105 | 3355 |14712 |      12 |     0 |

Two other workloads for shape, neither as representative: a synthetic
malloc mix (64 MB) gave lz4 2.45x, and `sort` over repetitive text (41 MB) gave
lz4 6.50x -- that last one is inflated by the input being `yes`-generated lines
and should not be quoted as a result.

**The decision.** `lz4` for an in-RAM pool. 2.23x on a real workload for
**1.89 us to decompress on the fault path**, against the hundreds of
microseconds a read from flash costs -- so holding a page compressed in RAM is
roughly two orders of magnitude cheaper than having paged it out, which is the
entire zram argument and it survives being quantified here.

lzfse and zlib buy 45-60% more ratio for 5-7x the decompress cost. That is the
wrong trade for a hot pool and possibly the right one for what is actually
written to flash, where the cost is paid once and the ratio directly reduces
the 24-hour write budget. A two-tier design (lz4 in RAM, something denser on the
way out) is the shape the numbers point at.

**A real server, measured on the A9: mariadbd with a 131k-row InnoDB table,
293 MB resident anonymous** (out of 1.35 GB of *mapped* RSS -- the gap is
AOK's RSS counting address space, and it is worth knowing that four fifths of
what a database appears to hold is not resident at all). lz4 5.42x at 3.05 us
compress and 8.45 us decompress; zlib 9.82x.

**Read that ratio with care.** 55,760 of 75,106 pages land in the >=8x bucket,
because InnoDB allocates a large buffer pool that is mostly untouched. That is
not a measurement error -- a real server genuinely does hold that memory, and
compressing it really is nearly free -- but it means 5.42x is an *expected
benefit* number and not a *worst case* one. For CPU planning use the dense-data
figures (python on the same device: 2.83x, 3.07 us), because a page that
compresses to nothing costs almost nothing to compress. Note also that lz4's
decompress there (8.45 us) exceeds its compress (3.05 us), which is backwards
for lz4 and is the same near-empty-page effect: decompressing a trivial input
still has to write 4 KB, and on an A9 that write is what is being timed.

**Incompressible pages are a rounding error, not a design burden**: 0 pages
failed to fit, and 22 of 23,184 got no smaller. A raw-storage fallback is still
required for correctness, but it will not be a common path.

**On-device, and the earlier inversion is explained -- it was not hardware.**
Measured on an iPad 5th gen (iPad6,12, A9 at 1.07 GHz, the oldest part AOK
supports) against a real Python heap, pid verified:

| host | workload | resident | lz4 ratio | lz4 compress | lz4 decompress |
|------|----------|---------:|----------:|-------------:|---------------:|
| M4 Mac | cc1     |   138 MB |     2.38x |      6.30 us |        1.94 us |
| A9 iPad| python  |    33 MB |     2.83x |      7.61 us |        3.07 us |

**The A9 decompresses only 1.6x slower than an M4**, not the 2-3x guessed, and
on real data the algorithm ordering is the same on both: lz4 fastest, zlib
slowest. That closes the CPU gap, and it closes it favourably -- 3 us on the
slowest supported device is still two orders of magnitude under a flash read.

The inversion that prompted all this (lz4 11.41 us against zlib 5.64 on the
same device) came from **degenerate input, not from the SoC**. Those earlier
device samples were near-empty pages -- 4431 of 4500 in the >=8x bucket, ratios
of 25x and 136x -- and when a page compresses to almost nothing the measurement
is fixed API overhead rather than throughput, which does not rank the codecs the
way real data does. The first device numbers should not have been quoted, and
the lesson is the ordinary one: **a ratio of 136x is not a good result, it is a
warning that the input is not representative.**

The order-rotation added to the instrument is kept anyway. It is cheap, it makes
position and algorithm independent, and it is the thing that would have
distinguished these two explanations without needing a second workload.

**Hardware acceleration remains a real consideration even though it was not the
cause here** -- Apple's codecs are tuned per architecture and their relative
speeds need not be constant -- so the runtime pick below is still the right
design. It is now a cheap insurance policy rather than a necessity.

**What is NOT established, and none of it should be skipped:**
- **The pool allocator.** Compressed pages are variable-sized, so they need one;
  Linux uses zsmalloc and it is not small. Fragmentation overhead is unmeasured
  and eats directly into the ratio above.
- ~~Device confirmation that iOS's `phys_footprint` ignores compression.~~
  **CONFIRMED on an iPad 5th gen, 2026-09-09**, and it is the finding the whole
  case rests on. `/proc/ish/mem_guard` headroom, while a guest process held
  250 MB of a single repeating byte -- maximally compressible, the easiest
  possible case for the compressor:

  ```
  baseline  890 MB
  t+25s     722 MB   <- the 250 MB is charged
  t+70s     723 MB
  t+130s    723 MB
  t+190s    726 MB   <- never comes back
  ```

  macOS compressed the equivalent buffer within 25 seconds. iOS charges for it
  regardless, for at least three minutes of idle. **The host's compression buys
  AOK no jetsam headroom on a device**, which is exactly what it does on the
  Mac, and it means only compression AOK performs itself -- into its own
  smaller buffer, releasing the originals -- can move the number that kills the
  app.
- Only two genuinely representative workloads. A JVM, a Python, and a Node
  process would each have a different shape.

**Two instrument bugs worth remembering**, both caught by the measurement's own
guards rather than by review:
- The first run reported exactly one verify failure per algorithm, all three
  identical -- not how three independent compressors fail. The pages belong to a
  RUNNING process and were written between the compress and the compare. Each
  page is copied to a private buffer now, which also makes the three algorithms
  measure the same bytes.
- The second run **killed the emulator**: SIGBUS/KERN_MEMORY_ERROR reading a
  file-backed page whose host bytes were not there. `mem_walk_resident_pages`
  now yields only anonymous, host-readable pages. See the commit; that filter is
  a safety requirement, not a preference.

### Signals left over from moving kill() to the process queue

Fixed 2026-09-23 ("signal: a thread takes its own signals first, and kill()
queues on the process", tests/manual/signal_dequeue_order.c): a process's
signal waits on the process's queue, and a thread takes its own queue before
the process's, synchronous signals first. Found alongside, by reading, not
measured:
- A native program's handlers run in the order their signals are taken:
  `nlibc_deliver_signals_count` calls each as it takes it. A translated
  guest, like Linux, stacks a frame per signal, so the LAST one taken runs
  first.
- A stop or continue signal the kernel sends to one thread while holding
  pids_lock (ptrace's attach SIGSTOP, a resume's signal) cancels the other
  kind on that thread's queue and the process's, not on the other threads'
  own queues: `send_signal` has no thread list. tkill, tgkill and
  rt_tgsigqueueinfo do reach every thread (`signal_prepare_stop_cont_threads`).
- kill(-1) skips only the calling thread (`kill_everything`); Linux skips
  the caller's whole thread group, so a non-leader thread's kill(-1) also
  signals its own process here.

### Stop and continue notices: what is still open

Fixed 2026-09-23 (tests/manual/notify_parent_cldstop.c): a stop, a continue
and a ptrace stop are announced with SIGCHLD, not the exit signal, once, to
the leader's parent or the tracer, as Linux's do_notify_parent_cldstop does;
and a tracer's resume lifts a group-stop before the tracee wakes.

Also fixed 2026-09-23 (tests/manual/reparent_zombie_disposition.c, measured
on Linux 6.12 64-bit and -m32 first): zombies handed to a new parent at an
exit were announced with one SIGCHLD whatever its disposition, and left for
a wait a parent that disclaimed SIGCHLD never makes. do_exit's reparent loop
now goes through exit_notify_process_locked, as Linux's reparent_leader goes
through do_notify_parent: SIG_IGN or SA_NOCLDWAIT releases the zombie at
once, SIG_IGN sends nothing even to a new parent that blocks SIGCHLD, and
each zombie gets its own SIGCHLD, with its CPU times.

Also fixed 2026-09-23 (tests/manual/wait_child_order.c, measured on Linux
6.12 64-bit and -m32 first): AOK's children lists were newest-first, Linux's
oldest-first. fork, CLONE_PARENT, exec's de-thread and the reparent loop all
linked at the head, so wait(-1) reaped the youngest zombie first (Linux 1 2
3, AOK 3 2 1), a subreaper reaped its orphans before its own children, and of
three zombies reparented together the SIGCHLD a blocking new parent found
named the youngest. Now a child goes at the end, as copy_process's
list_add_tail puts it; an exit hands its children on after the new parent's
own, in their order, as list_splice_tail_init does; and a thread's exec
takes the old leader's place, as de_thread's list_replace_init does.
reparent_zombie_disposition.c now requires the oldest. A checkpoint had its
own copy of the bug: the restore builds each parent's children in image
order, and the save wrote siblings in whatever order its placement left
them (four zombies came back reaped 7 4 6 5). The save now walks the tree
for its order (checkpoint_threads.sh, mode `order`). AOK has no
/proc/<pid>/task/<tid>/children; the test checks it where it exists.

Found alongside, by reading, not measured:
- A group-stop is announced as soon as one thread takes the stop signal:
  receive_signal stops the whole process at once, and the others stop at
  their next pass through handle_interrupt -- one blocked in a syscall only
  when that call returns. Linux stops every thread first and announces the
  stop from the last (task_participate_group_stop), so a parent told
  CLD_STOPPED can find threads here still in R or S where Linux shows T.
- The notices carry si_utime and si_stime 0; Linux fills in the child's CPU
  times.

### The orphaned-group test has two copies, and neither skips what Linux does

Found 2026-09-23 by reading, while adding the orphaned-group rule's
per-child case; not measured. Linux has one test, `will_become_orphaned_pgrp`,
for exit and for the terminal (`is_current_pgrp_orphaned`, which makes a
background read or write EIO rather than a stop). AOK has two --
`pgrp_is_orphaned_locked` in kernel/exit.c and `pgroup_is_orphaned` in
kernel/group.c, which fs/tty.c's `tty_check_change_locked` calls -- and
they differ from Linux and from each other:
- The terminal's copy counts a zombie member, and one init has adopted, as a
  way back into the session. Linux skips both (`exit_state &&
  thread_group_empty`, `is_global_init`); only the exit copy skips init.
- The exit copy skips a member whose leader thread is `exiting` even while
  its other threads run. Linux counts that process until its whole thread
  group is gone, so a group whose only way back is a process whose main
  thread called `pthread_exit` is orphaned here and not there.

**Next step:** one helper in kernel/group.c, walking the group's pgroup list
(complete now that setpgid files a joiner there), with Linux's two skips,
for both callers. Tests against camd: a background read from a group whose
only way back is a zombie member (EIO on Linux), and the `pthread_exit`
way back above.

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

FUTEX_CMP_REQUEUE_PI is not ENOSYS but is no more right (seen 2026-09-25,
while keying shared futexes by memory): it compares `*uaddr1` against `val`,
the wake count, where Linux compares `val3`, and it wakes no one where Linux
takes the PI lock for one waiter. Its only real callers wait with
WAIT_REQUEUE_PI, which is ENOSYS, so it is unreachable in practice; rewrite it
with the locking half. Its waiters do now keep their futex's reference when
moved (`futex_requeue_waiters`).

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

### Address-space walks: what still costs per page or per region

**Established (2026-09-25).** The hole finder, and every walk built on "next
mapped page" / "next unmapped page", used to read the 56-byte entry of every
page it passed -- and leaves are immortal, so also every entry of every leaf a
process had ever used. One mmap(NULL, 4096) cost 11.8 ms beside a 2 GiB
MAP_SHARED memfd mapping and 10.4 ms after it was unmapped, against 0.02 ms
alone, and `dotnet --info` spent most of an hour there. Per-leaf occupancy
bitmaps with per-chunk used/full summaries (emu/memory.c, "occupancy bitmaps")
made both 0.012-0.014 ms; `tests/manual/mmap_hole_scaling` guards it, and
`ISH_PT_OCCUPANCY_CHECK=1` verifies every bit and `vm_entries` after each
structural change. Fault backpressure reads the resident-set counter instead of
walking, and RLIMIT_AS reads VmSize's counter.

**Still open, none of them measured as a problem yet:**
- `pt_find_hole` is O(occupied runs between mmap_floor and mmap_ceiling), each
  a few words. Linux is O(log n) with a gap-augmented VMA tree. An address
  space fragmented into thousands of separate runs would still pay per run.
- RLIMIT_DATA, when finite and the mapping is data, walks every mapped page's
  flags per mmap/brk/mremap (`vm_may_expand`). A data-page counter would need
  maintaining at every flags change, not just every entry change.
- `/proc/<pid>/maps` and `smaps` compare flags page by page, and
  `mem_resident_page_count` (VmSwap, `/proc/ish/swap_evict`) asks each mapped
  entry's frame. Both skip unmapped pages and empty leaves now, but are still
  linear in mapped pages.
- fork copies entries one page at a time; that is the page-table design, not a
  walk.

**Next step:** only if a workload shows one of these in a `sample`.

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

**System V shared memory segments are not listed**: /proc/sysvipc/shm is its
header alone and shmctl has no SHM_STAT/SHM_INFO, so `ipcs -m` shows nothing
even while segments exist (kernel/ipc.c implements them). Semaphores and
message queues are listed for real.

**`/proc/meminfo`'s Shmem, AnonPages and Mapped are not Linux's figures.**
Since 2026-09-25 they are counters (`class_entries` in struct mem), so a read no
longer walks every page of every address space -- 0.5 ms alone and 54 ms beside
a 2 GiB memfd mapped twice before, 0.05-0.06 ms both after, 0.011 ms on Linux.
The counters kept the old walk's values exactly, and those differ from what
Linux means. Measured with the same program on amd64 and on Linux 6.12 (camd),
the change in kB as each step happens:

| step (16 MiB unless said)          | Linux 6.12                     | AOK                         |
|------------------------------------|--------------------------------|-----------------------------|
| private anon mapped, untouched     | 0                              | AnonPages +16384            |
| ...then touched                    | AnonPages +16384               | 0                           |
| shared anon mapped, untouched      | 0                              | Shmem +16384                |
| ...then touched                    | Shmem +16384, Mapped +16384    | 0                           |
| memfd mapped RW and RX, untouched  | 0                              | Mapped +32768               |
| ...touched through RW              | Shmem +16384, Mapped +16384    | 0                           |
| ...read through RX too             | 0                              | 0                           |
| ...unmapped, fd still open         | Mapped -16384 (Shmem stays)    | Mapped -32768               |
| ...fd closed                       | Shmem -16384                   | 0                           |
| file mapped private, then read     | Mapped + the pages not already mapped elsewhere | Mapped + all, at mmap |
| 2 GiB memfd mapped twice, untouched| 0                              | Mapped +4194304             |
| fork, child breaks half of 32 MiB  | AnonPages +16384               | AnonPages +32768            |

So AOK counts page-table entries per address space at mmap time; Linux counts
resident pages once each, from first touch: AnonPages is anonymous pages
mapped anywhere, Mapped is file pages (shmem included) mapped anywhere, and
Shmem is every shmem page -- memfd, shared anonymous, tmpfs, SysV -- mapped or
not. Linux-shaped figures need per-page state rather than per-entry: a
touched-and-first-mapping count on the frame (struct data already tracks its
owners), a memfd/shared-anon page counted as Shmem, and tmpfs file pages
counted whether mapped or not. `tests/manual/meminfo_scaling.c` asserts only
what both agree on; its table would change with this.

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

**A load or store past EOF does not SIGBUS if its host page holds any of the
file.** Linux faults at guest-page granularity; the host pages a file in at its
own, 16 KiB on Apple silicon. So a guest page past EOF that shares a host page
with the file's last bytes reads zeroes and takes stores, which become file
content if the file later grows: guest pages 2 and 3 of a 5000-byte file read
as zeroes on alpine-arm64-test, and `write(2)` from one returns 16, where Linux
6.12 gives SIGBUS and EFAULT (measured 2026-09-25). A host page WHOLLY past
EOF does fault -- a guest SIGBUS from the JIT, and a failed syscall from the
kernel (next section) -- and a host whose page is the guest's has no gap. The
fault handler would have to know the backing file's current size at fault
time, which means carrying the file identity into the page fault path rather
than just the host memory.

**`remap_file_pages` is ENOSYS.** Linux has emulated it over mmap since 3.16
and a linear remap returns 0. Linux's emulation is a `MAP_FIXED` shared mapping
of the same backing over the subrange at the new offset. Lazy reservations are
no obstacle: every page-table entry already carries its own `data` and
`offset`, and a large shared anonymous mapping that is still reserved can be
materialised first, as `mprotect` does. What is missing is the syscall itself.

### File pages past EOF: what the guarded copies leave

Fixed 2026-09-25: kernel C code that touched a host page of a file mapping
holding no byte of the file -- `write()` from it, `read()` into it,
`/proc/<pid>/mem`, `process_vm_readv`, `ptrace(PEEK/POKE)`, a futex word, the
copy-on-write break after a fork -- took a host SIGBUS that ended the whole app
(exit 138). Every such access to a page that is not anonymous now runs under a
fault guard (emu/host_fault.h) that both host fault handlers resume at, so the
syscall fails as Linux 6.12's does: EFAULT, or EIO from `/proc/<pid>/mem` and
`ptrace`, and a store after a fork is a guest SIGBUS. (The forced accesses and
the copy-on-write break were closed first, by "mem: a debugger's write leaves
the page as protected as it was", through `mem_host_copy`, which now makes the
same guarded copy.) `tests/manual/syscall_page_past_eof.c` takes every route,
each beside a control inside the file. What is left:

**A `read(2)` that faults has already consumed its data.** AOK reads into a
kernel buffer and then copies it out, so when the copy faults the bytes are
already gone -- from a pipe, or past a file's offset. Linux copies first and
consumes only what it copied: after `read(pipe, bad, 16)` fails EFAULT, the next
read on Linux returns the 16 bytes, and on AOK it returns EAGAIN (measured on
camd and alpine-arm64-test). Any bad buffer does it, not only a page past EOF.

**On device, a store into the host page that holds EOF.** kernel/exec.c's
split_tail comment records APFS failing the copy-on-write page-in of a host page
that straddles EOF, on iOS. A syscall's copy into such a page is guarded like
any other (EFAULT) and the guest's own store is a guest SIGBUS, where Linux lets
a store into the guest page holding EOF succeed. Not reproduced on macOS 26,
where that store succeeds (host probe, 2026-09-25); unmeasured on a device.

**Debug-only readers are unguarded.** emu/amd64_interp.c's trace functions and
kernel/user.c's htop trace `memcpy` from `mem_ptr` directly; each runs only
behind its own `ISH_*` trace knob.

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

### FIXED: a checkpoint save before the guest booted crashed on a NULL list head

**Root-caused and fixed 2026-09-12.** Four device crashes, all
`task_snapshot_collect+172`, `ldur x28, [x24, #-0x8]`,
`KERN_INVALID_ADDRESS at 0xfffffffffffffff8`, always from the app's background
save.

**The cause.** `alive_pids_list` was a bare global (`kernel/task.c`), so it
started **NULL/NULL** and only became a valid empty list when
`become_first_process` called `list_init` (`kernel/init.c:304`). But
`list_for_each_entry` has no NULL check, so any walk before boot followed
`next` into 0 and faulted on the FIRST iteration. `ckpt_check_scope` does have
the right guard -- it refuses with "there is no guest running" when the
snapshot is empty -- but it has to call `task_snapshot_collect` to learn that,
and that call was the crash. The guard sat downstream of the fault.

**What made it reachable, and it was a regression of mine.** The app's
background save (`AppDelegate.m`, `ISHSuspendGuardEnterBackground`) is gated on
`shouldSuspendToDisk`, **not** on the guest being booted. The session-resume
picker added earlier the same day defers `ensureBooted` while it waits for an
answer (`TerminalViewController.m:464-467`). So once any session slot existed,
every launch deferred the boot, and backgrounding fired a save against a kernel
that had never come up. The first device run had no slot, booted normally, and
saved 20 tasks and 24 sockets without incident -- which is why it looked
intermittent.

**The fix** is one line: `alive_pids_list` (and
`tasks_pending_deletion_queue`, the only other bare global head) are now
`LIST_INITIALIZER`-initialised, so an empty list READS as empty from load.
`init.c`'s `list_init` stays -- re-initialising an empty list is a no-op.

**Measured, same tree and build dir, only the declaration differing:**

| binary | `ISH_CHECKPOINT_AFTER=0.001` x8 | result |
|--------|--------------------------------|--------|
| before | 8/8 **SIGSEGV** (rc 139)       | crash in the walk |
| after  | 0/8                            | `refused (-3) there is no guest running` |

The reproducer to keep: `ISH_CHECKPOINT_AFTER=<delay>:<path>` with a delay
short enough to beat the boot. Driving `/proc/ish/checkpoint` instead exercises
`checkpoint_save` from a guest task and cannot reach this at all -- which is
why ~20 earlier attempts found nothing.

**Still worth doing:** gate the background save on the guest actually being
booted, so it refuses cleanly rather than relying on the scope check; and the
`list_remove`/`list_for_each_entry` NULL class below.

### list_remove leaves a NULL node and the walk macro has no NULL check

**The class behind the alive_pids_list crash above, and it is wider than that
one list.** `list_remove` (util/list.h:69) sets a node's `next` and `prev` to
**NULL**; `list_init` leaves a node pointing at **itself**; and
`list_for_each_entry` terminates only on `&item->member != (list)`, with no
NULL check. So any node that is reachable from a list while holding a NULL
`next` faults the walk at the `container_of` subtraction --
`ldur x28, [x24, #-0x8]`, address `0xfffffffffffffff8`.

Measured spread in kernel/ and fs/:

- **94** bare `list_remove(` calls vs **14** `list_remove_safe(`.
- Unguarded `list_for_each_entry` walks per list head: `mounts` 19,
  `group->threads` 9, `pid->pgroup` 7, `alive_pids_list` 6, `sighand->queue` 4,
  `poll->poll_fds` 4, and a long tail.
- `list_for_each_entry_safe` caches `next` one step ahead but has the same
  termination test, so it is not immune either.

**Fixed so far: only the two `alive_pids_list` sites** (`task_unlink_locked`,
`kernel/exec.c`'s exec unlink), which now `list_init` after removing. That is
the list with an actual device crash report behind it.

**The class fix would be one line** -- have `list_remove` re-init instead of
NULLing -- but it changes the meaning of `list_null()` for 94 call sites, and
`list_empty()` treats NULL and self-pointing as the same thing while
`list_null()` does not. That needs its own audit of every `list_null` reader
before it is safe, which is why it was not bundled into a checkpoint fix.

**Next step.** Audit `list_null()` callers, then decide between the one-line
`list_remove` change and converting the remaining bare removes to a re-initing
form.

### The Desktops applet's default height under-counts its action buttons

Noticed 2026-09-12 while adding the Session button, and **pre-existing** -- left
alone rather than changed under an unrelated commit.

`ISHWorkspaceWorkspacesContentSize` (app/WorkspaceViewController.m) derives the
applet's preferred size from its contents, but carries a single
`actionsHeight` term (36pt phone / 44pt otherwise) while `_contentStack`
receives more than one action row in the classic style:

- **classic**: `_newWorkspaceButton`, `_closeHiddenButton`, `_sessionButton`,
  `listCard` -- three buttons, one term (plus the `sessionHeight` term added
  with the Session button, so the shortfall is the *second* of the two older
  buttons).
- **modern**: `layoutRow`, `_sessionButton`, `listCard` -- covered correctly by
  `actionsHeight` + `sessionHeight`.

The consequence is cosmetic: this is the preferred/fallback size for a window
the user can resize, so the applet opens a little shorter than its contents in
the classic style and the bottom button sits tight against the edge.

**Next step.** Replace the fixed `actionsHeight` with a count of the action
rows actually added, so the two styles cannot drift apart again -- the same
bug will recur the next time a button is added to one branch and not the
other.

### An effective uid of 0 counts as every capability, whatever the effective set says

`current_capable()` (kernel/getset.c) is `superuser() || <the bit in
cap_effective>`, and `superuser()` is an effective uid of 0. Linux asks the
effective set alone. The one place a test shows it is
`tests/manual/exec_setid_unsafe.sh`, row R7. Root drops `CAP_SYS_PTRACE` and
`CAP_SETUID` from its effective set, calls `PTRACE_TRACEME`, and execs a
binary that is set-user-ID to uid 1000:

- **Linux** refuses the new uid, since the tracer could not have attached and
  the caller cannot set ids itself. The image runs as 0/0/0/0.
- **AOK** runs it as 0/1000/1000/1000.

That is the only one of the test's 51 rows that differs.

Every privileged syscall asks the same function, so the fix is tree-wide. Each
`current_capable()` and `superuser()` caller needs checking against what Linux
asks there, and the capability tests need re-running as root with a reduced
effective set, which none of them do today.

Two more gaps in the same exec rules, neither with a test:
- **Shared filesystem context:** Linux's third unsafe-exec condition, a
  `CLONE_FS` shared with a process outside this one, is not modelled.
- **`#!` interpreters:** Linux honours an interpreter's own set-id bits, and
  AOK does not.

### Tracing a native program: what a tracer still cannot see

Found 2026-09-25 while fixing gdb's `startup-with-shell` under
`SHELL=/AOK/native/zsh` (tests/manual/ptrace_startup_with_shell.c). A traced
native program now reports its exec, and its own exec replaces it in place,
keeping its pid (native_exec_in_place_wanted, kernel/native.h) -- which is all
gdb's start-up needs. Three things a tracer sees on Linux it still does not:

- **No syscall stops.** A native program's calls go through
  syscall_dispatch_native, which has no ptrace hooks, and there is no guest
  register file to report them from. `strace` shows the exec and the exit and
  nothing between; a PTRACE_SYSCALL tracer sees one execve entry, the native
  program's exec events, and one exit.
- **Children are not followed.** A native shell starts a command with
  native_spawn, not clone, so `strace -f` and gdb's `follow-fork-mode` never
  attach to it.
- **An untraced exec still stands in.** Only a traced program's exec goes in
  place, since abandoning the program leaks its heap. Anything that attaches
  AFTER a native program has exec'd finds the stand-in's wait, not the program
  (its pid is the child's).

## Timers across a checkpoint

### FIXED: timers, and the signals they queue, were not in the image

**Fixed 2026-09-22.** POSIX timers (`timer_create`), the interval timers
(`setitimer`) and `alarm()` were not in a checkpoint image at all, so a
restored process that had armed one never got its signal: a program using
SIGALRM as a timeout waited for ever. Now each process's timers travel with its
first running task (struct group_timers_ckpt, kernel/timer_ckpt.h), are
rebuilt in their own slots, and are armed only once every restored task has
started -- `task_never_ran_destroy` does not free a group's timers, so arming
them any earlier would leave them firing into a failed restore.

- **Each deadline travels on the clock Linux counts it on**, not as "time
  left". MONOTONIC does not count the stop, and a relative arming on
  CLOCK_REALTIME and ITIMER_REAL live there (hrtimer_init moves them).
  BOOTTIME, the alarm clocks and a TIMER_ABSTIME arming on the wall clock do
  count it, and come due that much sooner. A CPU-time timer keeps the CPU time
  it had left. `posix_timer.abstime` and `fd->timerfd.abstime` remember the
  arming, and a timerfd now uses the same rule, where it was time left.
- **The signal queues travel too.** Only the task's `pending` bitmask was
  carried, and delivery takes from the queue lists while the waits ask the
  bitmask, so a signal pending at the save came back as a bit with no signal
  behind it: never delivered, and once unblocked, every wait returned EINTR at
  once (measured: a 0.2 s select took 0.000 s). Both queues now come back with
  their siginfo, overrun counts included, and the pending sets are rebuilt from
  them. Timers are read before the queues, so a timer firing mid-save costs at
  most an overrun, never an expiry.
- `signal_wake_task` no longer pokes a task whose host thread has not started
  (its `thread` is the parent's, or nothing, for the app's pid 1 before
  `task_start`), which a due timer made ordinary on the resume path.

**Three defects in the first version, found by an adversarial review and each
proved on the committed binary (e28c9476) before its fix:**
- *ITIMER_VIRTUAL/PROF were measured on the wrong thread.* `cpu_time_now_of`
  asks the CALLING thread for whichever member is `current`, and the writer
  sets `current` to the task it describes. With 2.5 s of CPU on the carrying
  thread, ITIMER_PROF came back 2.6 s of CPU late. The group's CPU is now read
  with `current` cleared (`group_cpu_now`), on both sides.
- *An expiry being delivered during the save was lost.* The timer thread
  decides to fire, drops its lock and only then queues the signal; a timer read
  in that gap was a fired one-shot, and its signal was not in the queue yet.
  `timer_read` now waits the delivery out, and ITIMER_VIRTUAL/PROF are re-read
  if the sampler ticked meanwhile (`timer_settle`). The window is microseconds,
  so `ISH_TEST_TIMER_FIRE_DELAY_MS` holds every delivery open: with it, the
  committed code lost both a POSIX timer's and ITIMER_REAL's signal 3/3, and
  the fix delivered both 3/3 (checkpoint_timers.sh's race leg).
- *"Never" overflowed.* A timer or timerfd armed at TIME_T_MAX -- systemd's
  clock-change watch -- and a nanosleep of TIME_T_MAX, which is what
  `sleep infinity` asks for, came back due at once: the nanosecond arithmetic
  wrapped. Saturated now (TIMER_CKPT_NEVER), and "never" comes back never.

**Test:** `tests/manual/checkpoint_timers.sh [root]` (+ .c), both save paths, a
3 s stop: alarm() in a child, POSIX timers on MONOTONIC, REALTIME relative
and absolute, BOOTTIME, periodic, SIGEV_NONE, process and thread CPU clocks,
ITIMER_REAL and ITIMER_PROF, queued signals with their siginfo, and the sleeps
below. Before the fix every check failed on both legs (24 on x86_64); after, each meets its
deadline within ~10 ms, on devuan amd64 and arm64 (glibc) and alpine amd64
(musl).

### FIXED: a sleep the freeze interrupted started over -- even with no restore

**Fixed 2026-09-22.** The earlier entry here said that within one process the
freeze's restart carried the deadline. It did not: a freeze reached a sleep, poll or
select as a bare EINTR, which `syscall_result_should_restart` restarted, but
nothing had recorded the deadline, so the re-executed call waited its whole
timeout again. Measured on the old binary with a plain save, no restore:
nanosleep, clock_nanosleep, select, pselect6, poll, ppoll, epoll_wait and
epoll_pwait all took 8.07 s for a 6 s timeout. Across a restore `sleep 5`,
frozen 3 s in, slept 5 s more.

Now the sleeps (`sleep_restart_or_eintr`) and `poll_wait` report a freeze as
the `_ERESTART_NOHAND` it is and keep their deadline, as a job-control stop
already did. The image carries it on the guest clock (MONOTONIC; BOOTTIME for a
relative BOOTTIME sleep, which counts the stop), with the pending-rewind flags,
so a handler that runs before the call re-executes still cancels it. epoll
keeps a freeze's restart and drops the deadline with every other one: nothing
consumed it, so after a SIGSTOP the NEXT poll or select to run waited out the
stale deadline, or epoll's own 2 s cap. `tests/manual/checkpoint_freeze_restart.c`
now also fails a sleep or poll-family case that returns late.

### Timers and timed waits: what is still open

- **Every other timed wait still starts its timeout over after a freeze:** a
  relative futex FUTEX_WAIT (and so every glibc timed lock and condvar wait
  that is relative), `rt_sigtimedwait`, `semtimedop`, a socket's
  SO_RCVTIMEO/SO_SNDTIMEO wait, and clock_nanosleep on a CPU clock. Each needs
  a deadline carried the way `sleep_restart_deadline` carries one (Linux's
  restart_block). kernel/calls.c's `syscall_result_should_restart` names them.
  A relative FUTEX_WAIT does now keep its deadline across a restart nothing
  ran in front of -- a stop, an ignored signal (`futex_restart_deadline`,
  parked with the wait) -- but a freeze answers "no restart" from the signal
  side, which drops the park, so it still starts over there.
- **The CPU-time clocks start again from zero after a restore.** A restored
  thread is a new host thread, so CLOCK_PROCESS_CPUTIME_ID,
  CLOCK_THREAD_CPUTIME_ID, getrusage, times() and /proc/<pid>/stat's
  utime/stime all go backward across one. A CPU-time timer is right -- it
  carries the CPU time it had left -- but a reading taken before the save, or
  an absolute CPU-clock arming made from one, is not.
- **The clocks resume when the restore STARTS** (`guest_clock_resume`), so
  MONOTONIC counts the restore's own duration, which Linux's does not -- it
  continues from the thaw. Every carried deadline agrees with it, so a relative
  and an absolute wait still agree; moving the resume to just before the thaw
  would fix all of them at once.
- **A native program's pending signals come back as bits with no queue entry**,
  as before. It is re-launched, and nothing it had pending is delivered.
- **Found alongside, not a checkpoint bug:** `ppoll` with an INT64_MAX
  timeout returns 0 at once (measured on the pre-change binary, no checkpoint
  involved), where nanosleep and clock_nanosleep with the same value sleep --
  poll_wait's deadline arithmetic overflows. A NULL timeout is the usual way to
  say "for ever", so nothing common hits it.
- **Found alongside, not a checkpoint bug -- FIXED 2026-09-23:** a signal
  whose delivery runs no handler (SIGCHLD with SIG_DFL) ended a restartable
  wait with EINTR, where Linux restarts the call; and
  `deliver_signal_to_group_locked` queued such a signal whenever ANY member
  blocked it, where Linux asks only the target. musl's fork() and pthread_exit
  block every signal, so a child dying while a sibling thread exited EINTR'd
  the other threads' sleeps. Now only the target's mask decides, and an ignored
  signal restarts what it interrupts, even when a sibling took it first
  (kernel/signal.c; `tests/manual/signal_ignored_restart.c`). A HANDLED
  process signal woke every thread too, so its handler ran in a sibling and
  other siblings' calls failed with EINTR; since the same day ONE thread is
  told, as Linux's complete_signal does
  (`tests/manual/signal_process_wake_one.c`).
  `checkpoint_timers.c` still parks its threads and its asker, so it depends
  on neither.

## Suspend and resume across the three modes

The goal: a suspend or checkpoint comes back exactly as it was, whether it was
taken in shell, Workspace or Wayland mode, with every applet in it (the Wayland
display included). Terminal and applet restoration are done. These three are
open. All were reported 2026-09-15.

### Saving takes a long time when a Wayland applet is open

**Established.** Nothing yet. The user reported that a suspend with a Wayland
applet open is slow on the SAVE side. It has not been measured and the cause is
not known. Candidates, none checked:
- the freezer waiting for the compositor's or RFB client's tasks to park, up to
  the freeze timeout in kernel/checkpoint.c;
- a much larger image, because a Wayland session keeps framebuffers in guest
  memory;
- the app side of the save (ISHWorkspaceCaptureLayoutForSuspend, and
  DisplayViewController tearing down its RFB connection).

**Next step.** Time the phases. Save the same session with and without the
Wayland applet open, and compare the image size, the `session.*` breadcrumb
timestamps in Diagnostics, and an `ISH_CHECKPOINT_DEBUG` trace of which task
the freezer waits on.

### Restore should come back in the mode it was saved in

**Established.**
- The launch mode comes only from the Settings "Initial Window" preference.
  SceneDelegate.m reads it through `ISHShouldLaunchWaylandDisplayAtStartup()`
  and `ISHShouldLaunchWorkspaceAtStartup()`.
- A suspend records the Workspace arrangement next to its image
  (`ISHWorkspaceCaptureLayoutForSuspend`). It does not record which mode was on
  screen.
- So a session saved in Wayland mode and resumed with the preference set to
  Workspace comes back in Workspace, and the reverse.

**Next step.** Record the on-screen mode (shell, Workspace, or standalone
Wayland) beside the image, the same way the layout is filed. On a resume,
choose the window from that record instead of the preference, and use the
preference only for a fresh boot.

### Wayland mode has no quick way to suspend or checkpoint

**Established.** Shell mode has a Save Session button and a Cmd+S key command
(TerminalViewController.m, TerminalView.m). Workspace has "Save Session" in its
root menu (WorkspaceViewController.m). The standalone Wayland display
(DisplayViewController) has neither. Its only suspend path is backgrounding the
app with Suspend to Disk on.

**Next step.** Give the standalone display the same two actions the others
have, "Save Session Now" and "Suspend and Exit" (`ISHSuspendSessionSaveNow`,
`ISHSuspendSessionSuspendAndExit`). Offer them as an on-screen control that
does not cover the desktop, plus a key command. Cmd+S matches shell mode and
looks free there: DisplayRFBView forwards only Cmd+= + - 0 to the guest, and
deliberately not Cmd+letter. Confirm it does not also reach the Wayland
session before claiming it.

### The signal waits can still be poked into EINTR

**Established (2026-09-17).** The address-space barrier
(`task_poke_shared_mem`) skips a task that is `io_block`, but it can read the
flag a moment before a task entering a blocking call sets it. The poke then
lands inside the wait, and `wait_for` reports it as `_EINTR` with no signal
pending. Seen unforced once in several hundred tries: an `inotify` read on the
Devuan arm64 root came back EINTR at 0ms with no handler run, while a sibling
thread mapped memory. `ISH_TEST_POKE_BLOCKED_TASKS=<comm prefix>` forces the
race (kernel/task.c).

Forced, it failed every wait that trusted `io_block` alone, and all of those
outside kernel/signal.c are now on `wait_for_blocked` (util/sync.c), which
treats a bare poke as a spurious wakeup: `eventfd`, `inotify` and `timerfd`
reads, pty reads and writes, FIFO opens, reads and writes (tmpfs FIFOs in
fs/fifo.c; a host FIFO's open retries in fs/real.c), `F_SETLKW`, `flock`, the
kmsg wait and the `/dev/fuse` read. `tests/manual/blocked_wait_state.c` checks
thirteen of them, and run with the knob it failed all thirteen before the
change and passes after.

**Still open: pause, rt_sigsuspend, rt_sigtimedwait and the signalfd read**, all
in kernel/signal.c, which was being rewritten for the restart record when the
rest landed. Forced, `rt_sigtimedwait` returned EINTR within milliseconds.
Only signalfd can take `wait_for_blocked` as it is, because its loop re-reads
the signals first. The other three cannot:
- `rt_sigtimedwait` waits for signals it has BLOCKED. Their arrival shows up
  only as the interruption mark a poke also leaves, which
  `task_wake_signal_pending` does not count. Its loop is
  `do wait_for(...) while (err == 0)`, with no look at the set, so a spurious
  wakeup would wait on past the signal. It needs to check the set on every
  pass first.
- pause and rt_sigsuspend loop until `wait_for` says `_EINTR`, so they need the
  same predicate inside the loop rather than a different wait.

**Next step.** Once the restart-record work in kernel/signal.c has landed,
restructure those loops as above. Then add the four calls to
`blocked_wait_state`'s knob-driven checks.

### A task blocked OPENING a FIFO still cannot be frozen if its wake is lost

**Established (2026-09-15).** The freezer's wakes (a `pthread_kill` and a
cond notify) can be lost on a device. `ISH_CHECKPOINT_LOSE_WAKES=1` drops them
on the CLI, so that failure can be reproduced on a Mac. Every cond-based wait
in the kernel now checks for a freeze once a second (`wait_for` in
util/sync.c). With the wakes dropped, a sweep of blocking shapes shows:
- **Now freeze:** dash/busybox `wait` (rt_sigsuspend), bash `wait` and
  `waitpid` (wait4), `flock`, a pipe read, and perl `pause`/`sigsuspend`.
- **Still refuses:** `cat` opening a FIFO nobody has opened for writing:
  "did not reach a syscall boundary (blocked in arm64 syscall 56)".

A fakefs FIFO is a real host FIFO, and `realfs_open` calls the host `openat`
without O_NONBLOCK. The task therefore sits in a HOST syscall that only the
`pthread_kill` can interrupt. No wait slice helps, because the task is not in a
cond wait.

**Why it was not fixed with the rest.**
- The writer half is simple: open O_WRONLY|O_NONBLOCK, and retry on ENXIO in
  short slices that ask about signals and the freeze.
- The reader half has no faithful emulation. A non-blocking O_RDONLY open
  succeeds at once, and Darwin offers no way to ask whether a writer exists.
  Inferring it from Darwin's spurious POLLHUP misses a writer that opens and
  closes without writing. Linux wakes the blocked reader for that writer, so
  the inference would change guest-visible behaviour.
- Cancelling a blocked reader by briefly opening the write end is visible to
  any other reader of the same FIFO, which would see a writer come and go.

**Next step.** Do the writer half as above. For the reader, look for a Darwin
query that reports the writer count, or accept the POLLHUP inference only
where a lost write-and-close cannot happen. Until then this is a
rarely-hit shape: a checkpoint has to land while a process sits in an unpaired
FIFO open.

## Deferred on purpose

### Suspend and Exit terminates the app, which the HIG discourages

`ISHSuspendSessionSuspendAndExit` ends in `exit(0)`, because iOS has no public
"quit my app" API and the feature is, precisely, to put the machine down and
leave. It is behind an explicit, confirmed, user-initiated action in the Session
menu -- never automatic -- and the image is fsynced and renamed into place
before the process goes, so the session is durable rather than merely written.

**The decision to make before an App Store build**: keep it, or replace the exit
with a "session suspended" screen that leaves closing the app to the person.
Reviewers object to apps that appear to crash; an app that quits on a button the
user just confirmed is a weaker case against, but it is not no case. Recorded
here so the choice is made deliberately at submission rather than discovered.
`/AOK/tools/suspend.sh` has always ended the same way and is unaffected either
way, since a guest-initiated halt is not the app terminating itself.



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

### YubiKey for ssh -- possible add, tested by the requester

Asked for on Discord (2026-09-21) by a user who authenticates ssh with a
YubiKey over NFC or USB. They had tried to build it themselves and stopped at
the NFC entitlement, which a free Apple account cannot hold. That wall is ours,
not theirs: the TestFlight build is signed on a paid team, so an entitlement we
add reaches TestFlight users. It does not reach free-account sideloaders of the
GitHub IPA, whose re-signer drops what their team cannot hold
([[unsigned-ipa-drops-entitlements]]).

**Established.**

- **Shape: an ssh-agent in the app, not key passthrough.** The guest has no
  USB, HID or PC/SC, and building any of them is far more work than the agent
  protocol, which is small (list identities, sign) and stable. The app talks to
  the key; the guest gets a unix socket and `SSH_AUTH_SOCK`; guest `ssh`, `git`,
  `scp` and `rsync` work unchanged on every arch, musl and glibc.
- **Transports.** NFC is iPhone only -- iPads and Macs have no reader CoreNFC
  can use. USB-C (iPad, iPhone 15+, iOS 16+) goes through CryptoTokenKit and
  reaches only the key's smart-card applets: PIV, not FIDO2, because apps get no
  HID. Lightning (5Ci) is MFi ExternalAccessory, and App Store use needs Yubico
  to register the app -- skip it.
- **Key types.** PIV (slot 9a) works over NFC and USB-C. FIDO2 `ed25519-sk` /
  `ecdsa-sk` works over NFC only, and only by speaking CTAP2 to the key
  directly with its `ssh:` application id. Apple's AuthenticationServices
  security-key API cannot do it: it signs only for an associated web domain, so
  it never matches a key `ssh-keygen -t ed25519-sk` made. Existing `id_*_sk`
  files would load through `ssh-add`; creating sk keys on the device is out of
  scope.
- **Socket plumbing is small.** `unix_socket_get` (fs/sock.c) already maps a
  guest socket node to a host socket by `socket_id`, so the kernel makes the
  node and the app owns the host listener behind it. The listener must strip
  the 8-byte peer cookie AOK's connect sends first (the unix peer-token
  registry, same file). AF_UNIX listeners survive suspend
  ([[listening-sockets-die-on-suspend]]), so no resume hook should be needed --
  confirm it on the device rather than assume.
- **UX limits.** Signing needs AOK in the foreground (NFC sheets and PIN
  prompts both do). Over NFC it is one tap per ssh connection, so `git` work
  that opens many wants `ControlMaster`; USB-C has no such cost. Listing
  identities would cost a tap of its own unless the public keys are cached by a
  one-time "enroll key" step in Settings.

**Sizing** (estimated 2026-09-22, nothing built): about 2,500 lines. Agent
protocol and signature encoding ~700 lines of C; PIV and both transports ~600,
or far fewer on YubiKit (ObjC, Apache-2 -- compatible with GPLv3 -- vendored
as an emkey1 fork per [[vendor-as-fork-submodule]]), which also covers FIDO2;
app UI ~500 (enroll, key list, PIN prompt with an optional per-session cache).
FIDO2 sk keys over NFC are a second phase of ~500 more. Entitlements:
`com.apple.developer.nfc.readersession.formats` (TAG), the PIV and FIDO AIDs in
`com.apple.developer.nfc.readersession.iso7816.select-identifiers`,
`NFCReaderUsageDescription`, and `com.apple.security.smartcard` for USB-C.
NFC Tag Reading must be enabled on the App ID in the developer portal and the
profiles regenerated; whether the smart-card entitlement needs the same is not
known.

**Testing without hardware.** The maintainer has no YubiKey and does not plan
to buy one, and the simulator has neither NFC nor smart cards, so the
transports can only be proven by the requester on TestFlight. What can be
proven here first: the agent core and socket on the CLI build, behind a
software key backend, with guest `ssh-add -L` and `ssh` against a guest sshd.
Ship the hardware path behind a Settings toggle marked experimental, and log
each APDU exchange's status word with `ish_printk` so the tester can paste
`dmesg` back -- ssh itself only ever sees `SSH_AGENT_FAILURE`.

**Next step** is a question to the requester, not code: PIV or FIDO2? Yubico's
own guide calls FIDO2 the simplest ssh setup, and if that is what they use, a
PIV-first phase does nothing for them and the order flips.

### x86 guests have no crypto acceleration, and it costs 18-46x

**Established, measured 2026-09-18** ([docs/guest_pc_sampling_2026_09.md](guest_pc_sampling_2026_09.md)).
`jit/guest-arm64/crypto.S` maps the arm64 guest's AESE/AESMC/PMULL/SHA256H onto
the host's own crypto instructions and `kernel/exec.c` advertises them in
`AT_HWCAP`. The x86 guests get none of it, and `openssl speed` at 16 KB blocks
shows what that is worth:

| | arm64 guest | amd64 guest | ratio |
|---|---:|---:|---:|
| AES-128-GCM | 97,352 kB/s | 5,308 kB/s | **18.3x** |
| SHA256 | 109,685 kB/s | 2,383 kB/s | **46.0x** |

Not a benchmark artefact: on `apt install` this is 20.6% of on-CPU time on the
amd64 guest (`libmd` 10.6% + `libcrypto` 10.0%, hashing and verifying packages;
24.6% of wall) against ~1% on arm64, and 35.8% of on-CPU on amd64 `apk add` against 6.9%.

**The scoping point that makes this tractable.** In `emu/cpuid.h` the AES-NI and
PCLMULQDQ bits sit inside `#if CPUID_ADVERTISE_VECTOR_STATE`, next to
XSAVE/OSXSAVE/AVX. That switch is 0 for a real reason -- the signal frame cannot
carry `ymm_hi`, so advertising AVX would corrupt registers across a signal.
**AES-NI does not share that debt**: its legacy SSE encodings use `xmm0-15`,
which the existing 512-byte FXSAVE signal frame already saves in full. The bits
are bundled there because making OpenSSL emit those encodings is, in that
comment's words, a separate claim needing its own evidence -- not because XSAVE
is required.

**Next step**: implement the legacy SSE `AESENC`/`AESENCLAST`/`AESDEC`/
`AESDECLAST`/`AESIMC`/`AESKEYGENASSIST` and `PCLMULQDQ` in the i386 and amd64
engines, mapped onto host AESE/AESMC/PMULL as `jit/guest-arm64/crypto.S`
already does, then advertise **only** bits 1 and 25 and leave
`CPUID_ADVERTISE_VECTOR_STATE` at 0. Do SHA256 in the same pass -- it is the
larger half of the loss, and the host instructions for it are already in use by
the arm64 guest. Note `emu/avx.c`'s `avx_aes_round` is a software S-box today,
so the VEX forms need the same treatment or they become a trap for anything
that probes AES-NI and then uses the VEX encoding.

Related and already shipped: `ISH_SYS_AEAD` (`kernel/ish_accel_aes.c` plus the
OpenSSL provider in `opt/AOK/tools/crypto`) is ABI-neutral, so it already works
for x86 guests -- but it is off by default, needs a provider installed in the
root, and covers AEAD ciphers only, not the bare SHA256 that `libmd` and `apt`
spend their time in.

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

### gzip is already native, and nothing routes to it

**Established, measured 2026-09-18** ([docs/guest_pc_sampling_2026_09.md](guest_pc_sampling_2026_09.md)).
`tar xzf` spends 50-83% of its on-CPU time in a decompressor **binary**, never
in libz: `/usr/bin/gzip` on the Devuan roots, `/bin/busybox` on Alpine. Read off
the ELF, `DT_NEEDED` for `/usr/bin/gzip` is `libc.so.6` alone -- GNU gzip and
busybox each carry their own inflate, so no libz accelerator of any kind can
touch this workload.

smallclue already has `gzip`/`gunzip`/`zcat` applets. Shadowing the guest's
gzip with the native one on PATH, interleaved A/B, median of 4, same 14 MB
tarball:

| guest | distro gzip | native gzip | speedup |
|---|---:|---:|---:|
| amd64 / glibc | 13.130 s | 4.759 s | **2.76x** |
| arm64 / glibc | 5.583 s | 4.797 s | **1.16x** |

So the capability exists and is unreached, because `/AOK/tools/native-links.sh`
is not applied by default. **Next step** is a decision, not code: whether
shadowing a distro binary by default is acceptable (it changes what `tar -z`
runs, and the applets' flag coverage would need to be checked against GNU
gzip's first). The arm64 win is smaller because what is left there is tar's own
file creation through fakefs, not the codec.

### smallclue's `git`, `rsync` and `dvtm` are compiled as stubs

**Wanted, and deferred on 2026-09-25: not before 556.** smallclue carries all
three, and the applets are in AOK's binary, but each one only refuses:

| applet | what it says today | why |
|---|---|---|
| `git` | "libgit2 support is not enabled in this build" | built without `PSCAL_HAS_LIBGIT2` |
| `dvtm` | "applet is disabled in this build" | built without `SMALLCLUE_WITH_DVTM` |
| `rsync` | "openrsync: not built into this iSH-AOK" | AOK's own stub, `kernel/smallclue_glue.c` |

meson.build takes only openssh and nextvi from deps/smallclue/third-party. So
AOK has never checked out the dvtm, libgit2 and openrsync submodules, not even
in the main checkout. Each one is a port, not a flag:

- **libgit2** (`git`). It has to be built for iOS inside meson, with every
  source file going through kernel/native_libc.h like the rest of smallclue.
  - It needs a TLS and HTTPS transport, and AOK links no OpenSSL. The
    NSURLSession-backed curl shim in deps/smallclue-shim is the likely route,
    through a custom smart-HTTP transport.
  - ssh remotes need an ssh transport; running the native `ssh` is one option.
  - Licence: GPLv2 with the linking exception. Check it against the
    Licensing summary in meson.build before shipping. Nothing else in the
    binary is GPL unless `-Dnative_bash` is on.
- **dvtm**. It starts a shell on each pty, and a native program cannot
  `fork()`. That needs AOK's native spawn with a pty.
  deps/smallclue/src/dvtm_runtime_hooks.h is smallclue's iOS hook point.
- **openrsync** (`rsync`). Its remote side runs over an ssh child. That needs
  native spawn plus pipes, and deps/smallclue/src/openrsync_ios_shim.h and
  openrsync_hooks.h are the starting points.
- **Submodules.** Initialise them in the main checkout only, never from a
  worktree, because worktrees share the submodule git dir. A worktree made
  now gets only the two submodules the build uses, and git then marks
  deps/smallclue as modified for the three missing folders. Creating the
  folders silences it.

Until then, the guest's packages do the job: `apk add git rsync dvtm`, or
`apt install` on Devuan. They are slower, being translated rather than native.

### Library-level native interposition: measured, and the answer is no

**Established 2026-09-18.** The idea of running a host libz/libcrypto/libc in
place of the guest's was measured with a sampling profiler
(`kernel/guestprof.c`, `ISH_GUEST_PROFILE`) across four workloads and four
guests. It does not pay:

- `tar xzf` reaches no shared library at all (above).
- In `git clone`, libz is 8-11% of on-CPU against `[kernel]` at 22-38%.
- The only library clearing a high bar is `liblzma` (27-40% of on-CPU in
  `apt install`, because `.deb` data is xz) -- and on musl that slot is libz
  instead, because `.apk` is gzip. Even "accelerate the package manager's
  codec" is a different library per distro, for a win confined to one of them.

No further work is planned on it. The instrument stays; the full argument,
including what would have had to be true for a go, is in the doc.

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

Checked against GitHub on 2026-09-08: **17 open**, down one because
[#503](https://github.com/emkey1/ish-AOK/issues/503) closed with the breakpoint
`si_code` fix. **Closing the issue is part of fixing the bug** -- a fix recorded
here and not there is a fix the reporter never learns about, and #541 is still
proving that point.

### Bugs

| # | Title | Notes |
|---|---|---|
| [#482](https://github.com/emkey1/ish-AOK/issues/482) | Wayland applet does not resize properly | body is a screenshot only. Very likely the same root cause as #483's second half -- confirm before treating them as two jobs |
| [#485](https://github.com/emkey1/ish-AOK/issues/485) | Qt apps (Falkon) cannot connect to session bus | 6 comments |
| [#521](https://github.com/emkey1/ish-AOK/issues/521) | Buildroot `make` crashes on "checking for working sigaltstack" | body is a screenshot only |
| [#523](https://github.com/emkey1/ish-AOK/issues/523) | yay (AUR helper) fails on Arch ARM64 | **reported symptom does not reproduce** -- see *Diagnosed* above. What does reproduce is a TLS handshake tail of 15.3 s against a sub-second median, which is a wait not being woken rather than slow work |
| [#527](https://github.com/emkey1/ish-AOK/issues/527) | pikaur fails on Arch ARM64 | blocked on `systemd-run` |
| [#541](https://github.com/emkey1/ish-AOK/issues/541) | ptraceomatic does not run: tracee reaped during setup | **fixed 2026-08-20**, and still open on GitHub as of 2026-09-08. Close it -- see `docs/historical/build_555_musts.md` §8, and re-run ptraceomatic alongside the ptrace work there rather than closing it blind |
| [#568](https://github.com/emkey1/ish-AOK/issues/568) | Network throughput is very slow for downloads and browsing | split out of #559. Two readings with different causes -- guest-side throughput vs device-wide degradation -- and which one it is has not been settled. Same neighbourhood as #523's handshake tail |
| [#572](https://github.com/emkey1/ish-AOK/issues/572) | Cannot determine a usable wildcard IP (Gradle) | Devuan aarch64 on an iPhone 7 Plus, iOS 15. Gradle wants a bindable local address; worth checking what AOK reports for the interface list before assuming it is a name-resolution problem |
| [#575](https://github.com/emkey1/ish-AOK/issues/575) | Unable to delete machines | **re-read 2026-09-09: the delete button already exists** (`deleteFilesystem`, RootDetailViewController) with confirmation, correct booted/default disabling and an explanatory footer. The gap is reachability -- the machines LIST returns `canEditRowAtIndexPath` YES only for cached archives, so swiping an installed machine does nothing whatsoever. Also: when it is the booted/default root the footer gives the rule and no remedy. See the roadmap |
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
