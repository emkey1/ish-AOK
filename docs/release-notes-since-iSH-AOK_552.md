# Release Notes Since `builds/iSH-AOK_551`

174 commits. The long cycle after a short one, and mostly a conformance
release: a systematic audit of what the emulated kernel promises against what
Linux actually does, closed defect by defect against a real Devuan box. Plus a
book about how the whole thing works, and the first release in which the i386
guest's regression suite has ever run to completion.

## Highlights

**The kernel now behaves like the kernel it claims to be.** Roughly sixty
commits of conformance work, each measured against Devuan 6 / Linux 6.12 rather
than argued from the manual page. Permissions and path resolution, credentials,
signal delivery and job control, ptrace, POSIX timers and the clocks behind
them, socket options, mounts, inotify, futexes and robust lists. Most of it is
invisible when it works — the point is that programs which quietly did the
wrong thing now do the right one.

**`PTRACE_ATTACH`.** You can attach a debugger to a running guest process.
`gdb -p <pid>` used to answer *ptrace: Operation not permitted*.

**`splice`, `vmsplice` and `tee`.** Implemented rather than stubbed, so the
zero-copy pipe idioms that busybox, systemd and a lot of shell plumbing reach
for stop failing.

**`openat2` resolution constraints.** `RESOLVE_BENEATH`, `RESOLVE_IN_ROOT` and
the rest are honoured again, so a program that uses them to sandbox its own
path handling gets the guarantee it asked for instead of a silent lack of one.

**FUSE, finished.** The v1 limits are gone: `mmap` on a FUSE file works, so
programs can execute and memory-map from a FUSE mount, and `FUSE_INTERRUPT`,
`FORGET` and `LSEEK` are wired up. Writable `MAP_SHARED` reaches the daemon,
which is what sqlite-on-FUSE needs.

**`/dev/url`.** A character device the guest writes a URL to and iOS opens —
including `shortcuts://` links, so a guest script can drive a Shortcut. The
write waits up to five seconds for an answer, so a success really means opened,
and it is bounded on purpose: a guest write must never become an unkillable
wait on the UI thread.

**`lsblk`, `lsns` and `lsmem` work.** They wanted `/sys/dev/block`,
`/sys/devices/system/memory`, and — the interesting one — distinct device
numbers for the anonymous filesystems. `lsns` filters namespaces by
`st_dev == nsfs_dev`, and AOK gave one device to everything anonymous, so it
printed roughly seventy warnings and then a table it had guessed at.

**A book.** `docs/book/` — 43 chapters and 8 appendices on how iSH-AOK works,
from the impossible-app framing through the JIT, the four guests, the VFS,
native programs, the iOS app, testing, releasing and the honest gaps. Two of
those appendices are generated from the tree, so they cannot drift silently.

## Fixes worth naming

**A runaway recursion could take the whole app down.** Stack growth was bounded
only by the distance to the mapping *above* it, which a recursion never trips —
so the stack descended into whatever was mapped below and wrote into it. On
i386 that meant walking through musl's thread block, destroying the thread
pointer, and killing the SIGSEGV handler that was about to report the overflow;
gnulib's *checking for working sigaltstack* probe hit it, which took out every
`configure` run in a Buildroot build (#521). On a 64-bit guest there was
nothing close enough below to hit, so the stack simply grew — to 1.42 GB in
measurement, which on an iPad is not a fault but a jetsam kill. AOK now keeps
Linux's stack guard gap *and* enforces `RLIMIT_STACK`; the same program peaks
at 45.7 MB.

**Ctrl-Z inside `poll` returned errno 512 on amd64.** `_ERESTART_NOHAND` is an
internal code the dispatcher is meant to convert; the amd64 path wrote it
straight to the result register. `poll`, `ppoll`, `select`, `pselect` and the
three `epoll_wait` variants were all affected, under job control only — so any
program stopped and continued while waiting on descriptors.

**`vmsplice` failed on amd64** with EFAULT on perfectly valid addresses: the
amd64 table routed it to the i386 entry point, which reads iovecs in the 32-bit
layout.

**`write` returned errno 85 on amd64** under a job-control stop — press ^Z on
anything writing to a pipe and the write failed with a code no manual page
lists. Same cause as the errno 512 above, in a different syscall: an internal
restart code reaching userspace. Both are now caught at a single point that a
newly added syscall cannot bypass.

**`epoll_wait` restarted where Linux returns EINTR.** Linux breaks out of
`ep_poll` on any pending signal and never reaches the restart machinery, while
`poll` and `select` resume transparently — so a plain job-control stop
separates them. AOK treated all three alike.

**`FUTEX_WAKE_OP` lost updates on amd64** — 1107 of 40000 measured. This is the
primitive glibc's condition variables are built on, so a lost update is a wakeup
that never happens.

**No robust futex was ever marked on an i386 guest.** The robust-list offset is
a signed long and in practice always negative; it was zero-extended, so every
computed address landed past 4 GiB and was refused. Silent: no error, no
diagnostic, just a lock whose owner died and which no later waiter recovers.

**The CPU-time clocks were broken for every glibc program.** The C library does
not pass `CLOCK_PROCESS_CPUTIME_ID` to the kernel — it converts to a dynamic
clock id first, and four call sites did not decode that form. `timer_create`
and `clock_nanosleep` returned EINVAL, `clock_getcpuclockid` returned ESRCH,
and `clock_getres(clk, NULL)` returned EFAULT where a NULL is legal and is
exactly how the library validates an id. So no glibc program could arm a
CPU-time timer or sleep on a CPU clock at all. Invisible to a suite run only on
musl roots, which pass the constant unchanged — the iPad, which boots Devuan,
is where every one of these surfaced.

**Boot could hang after `rsyslogd`.** A zero-length read of `/dev/kmsg` never
returned.

**The Filesystems screen says which root you are running.** It marked the root
that boots *next*, which is a different thing and often a different row, so the
disabled Delete button on the running root read as "AOK cannot delete
filesystems at all" (#575). The booted one is now tinted, bold, and labelled.

## Under the hood

- The i386 regression suite ran to completion for the first time. It used to
  abort at build time on a test that could not compile there, and the runner
  stops at the first build failure — so one unbuildable test had been hiding
  the whole architecture's results. Seven failures were waiting behind it:
  three real kernel bugs, four non-portable tests.
- `RLIMIT_FSIZE` and `RLIMIT_NPROC` are enforced, and `getrusage` is filled in.
- The `/AOK/tools` benchmarks ship as native programs (`bmm`, `bmt`), so the
  same workload can be timed with and without emulation.
- Sysctl writes are validated and honoured, and `binfmt_misc` is no longer
  faked.
- Locale is written to `/etc/default/locale` rather than `/etc/environment`,
  and `LC_ALL` is not set at all — it overrides everything and is the wrong
  tool for a default.
- Several crashes closed: a mount-table walk on the first mount, two
  dangling-pointer bugs, a use-after-free in `do_exit`, and two syscalls that
  let any guest program kill the emulator.
- The release gate is a script now, `tools/run-guest-gate.sh`, and it runs a
  fifth leg: the same kernel against **glibc**, not only the four musl roots.
  Four of the bugs above were reachable only through glibc and were green on
  every Alpine root — four architectures had been one libc wearing four hats.

## Known gaps

- The amd64 guest never JIT-compiles a locked instruction, so every atomic
  falls back to the interpreter and serialises on one global lock. It is
  correct, and it is slow. See `docs/build_553_musts.md`.
- An iosfs mount made through the new mount API (`fsopen`/`fsmount`/
  `move_mount`, which util-linux 2.39's `mount(8)` uses) no longer survives a
  relaunch. It used to "survive" as a phantom mount at a private staging path
  that could not be used and could not be removed; not persisting it is the
  better half of a bad trade until the bookmark is re-keyed at relocation.
- `clock_getcpuclockid` for *another* process, and `prlimit64` lowering a third
  party's `RLIMIT_STACK`, both take effect later than Linux would apply them.
