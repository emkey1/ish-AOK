# Runtime tuning, resource limits, and hardening

A few environment variables let you tune how iSH-AOK presents itself
to the guest. These apply when you can control the process environment —
building and running the standalone CLI emulator, or launching the app
from Xcode with a custom scheme environment — rather than something an
App Store install lets you change day to day.

The rest of this page is not variables: it is what the emulator enforces
unconditionally — resource limits, and security hardening a guest program
can rely on the same way it would on Linux.

## `ISH_GUEST_CPU_COUNT`

Overrides the CPU count iSH-AOK reports to the guest — `/proc/cpuinfo` and
`/proc/stat`, and through them what `nproc` and `sched_getaffinity` answer.

On iOS, and only on a device with more than two cores, iSH-AOK reserves roughly
a third of them (at least one) back from the scheduler-sizing queries `nproc`
and `sched_getaffinity`, so that `make -j$(nproc)` and programs that size
themselves from the affinity mask leave the app some headroom. Every core is
still online: `/proc/cpuinfo`, `/proc/stat` and `/sys/devices/system/cpu` list
all of them, and the affinity mask and `Cpus_allowed` name the ones a program
may run on, the way Linux looks under a cpuset or `taskset` (so glibc's
`sysconf(_SC_NPROCESSORS_ONLN)` counts every core, as it does there). On a
9-core M4 iPad that is 9 CPUs listed and `nproc` 6. The standalone CLI on Apple silicon does **not** do
that — with no override it runs a fixed 4 emulated CPUs regardless of the host's
core count. Set `ISH_GUEST_CPU_COUNT` to override either:

```sh
ISH_GUEST_CPU_COUNT=6 ./ish -f build/alpine /bin/sh   # match a specific core count
ISH_GUEST_CPU_COUNT=1 ./ish -f build/alpine /bin/sh   # force a serial (single-core) guest
```

Forcing `=1` is particularly useful when debugging a concurrency bug: it
rules out cross-core races as the cause by construction.

The reservation above is itself driven by an environment variable,
`ISH_GUEST_CPU_RESERVE`: set it (to anything) on a non-iOS host to get the
same "leave the app some headroom" behavior the app applies automatically on
a device with more than two cores. It exists so a test can exercise the
reservation logic without an iPhone.

## `ISH_RANDOMIZE_VA_SPACE`

Controls ASLR on `exec`, the same way `/proc/sys/kernel/randomize_va_space`
does on Linux: `0` disables it, `1` randomizes the mmap base, PIE base and
heap start, `2` (the default, whether or not the variable is set) is the
same plus a randomized stack top. An invalid value falls back to `2`. It
moves where the loader, shared libraries, the vDSO/sigpage, a dynamic
`arm64`/`riscv64` PIE, and any mapping made without an address land — not
just the stack.

ASLR is off regardless for a process exec'd with `ADDR_NO_RANDOMIZE`
(`setarch -R`; this is gdb's default, so a debugged program's addresses are
reproducible), and a set-id exec clears that flag first so a privileged
program cannot inherit a debugger's disabled-ASLR setting.

```sh
ISH_RANDOMIZE_VA_SPACE=0 ./ish -f build/alpine /bin/sh   # reproducible layouts, for A/B testing
```

## `ISH_VDSO`

64-bit guests (`amd64`, `arm64`, `riscv64`) read the clock through a vDSO
mapped into every process — `vdso/amd64`, `arm64` or `riscv64/vdso.S` — rather
than making a system call for `clock_gettime` and friends. `ISH_VDSO=0` in
the host environment leaves it out entirely: no page is mapped, no
`AT_SYSINFO_EHDR` goes in the aux vector, and the C library falls back to the
real system calls the vDSO replaces. It exists to A/B a problem against a
build with the vDSO in the picture — with it set, `strace` sees
`clock_gettime` calls again, which a vDSO read never generates.

## `ISH_GUEST_PROFILE`

A guest-PC sampling profiler, host-side and with no guest cooperation needed:
it answers where a workload's wall time actually goes, split across the
shared objects (libraries) it has mapped, plus how much of that time is
kernel work or blocked rather than guest code at all. It exists to settle
whether a native stand-in for a hot library function — the same idea behind
`ISH_HLE` and the [crypto accelerator](crypto-accel.md) — would actually pay
for a given workload, rather than guessing.

```sh
ISH_GUEST_PROFILE=1 ./ish -f build/alpine /bin/sh          # sample every 1000 us (the default)
ISH_GUEST_PROFILE=200 ./ish -f build/alpine /bin/sh        # sample every 200 us
ISH_GUEST_PROFILE_OUT=/tmp/prof.txt ./ish -f build/alpine /bin/sh   # write the report there instead of stderr
```

The report is written at process exit. It is a no-op unless set, so it costs
nothing in ordinary use.

## `ISH_GUEST_MEM_HEADROOM_MB`

Sets the free-memory threshold (in MB) below which iSH-AOK stops handing the
guest new memory: once the app's available-memory budget drops under it, `mmap`,
`mremap` and `brk` growth are **refused** — rather than throttled — so a
runaway guest cannot get the whole app jetsammed. `mmap` and `mremap` fail with
`ENOMEM`; `brk` returns the break unchanged, which is how `malloc` sees it, so
plain heap growth stops working too. Defaults to 192 MB; set it to `0` to
disable the guard entirely.

On iOS the budget this is measured against is the app's own jetsam limit, which
the OS reports. A macOS or Linux host has no such per-process limit, so with
nothing else set the guard has nothing to measure and never fires on the
standalone CLI — set `ISH_GUEST_MEM_BUDGET_MB` below to give it one.

To disable the guard on a device, set it in the Xcode scheme environment:

```
ISH_GUEST_MEM_HEADROOM_MB = 0
```

## `ISH_GUEST_MEM_BUDGET_MB`

Tells iSH-AOK to behave as though the process had a memory limit of this many
MB. It exists so the low-memory path above can be exercised anywhere, rather
than only on a device that is genuinely close to being jetsammed — before this,
the guard was unreachable on the CLI and so was almost impossible to test.

The guard then refuses guest memory growth once the process is within
`ISH_GUEST_MEM_HEADROOM_MB` of that budget, so the two compose exactly as they
do on a device. Measured in an arm64 Alpine guest on an Apple silicon Mac, with
a guest that maps and dirties 32 MB at a time:

```sh
./ish -f build/alpine-arm64-test /tmp/b                        # never refused, mapped 6400 MB
ISH_GUEST_MEM_BUDGET_MB=512 ./ish -f build/alpine-arm64-test /tmp/b   # refused after 320 MB
```

320 is 512 minus the default 192 MB floor, which is the whole arithmetic. Set
`ISH_GUEST_MEM_HEADROOM_MB=0` alongside it and the guard is off again even with
a budget set.

Leave it unset for ordinary use. On iOS it is not needed, because the real limit
is available; setting it anyway makes the guard use whichever of the two leaves
less room, so it can only ever make the guest more conservative, never less.

## `ISH_GUEST_SWAP_MB`

Turns simulated swap on at launch with this many MB of backing store, and is
the only way to reach it outside the app's Settings.

Swap is **off by default and stays off** unless someone asks for it: paging
guest memory writes to the device's flash and takes container space, so it is
opt-in, with a size the user chooses rather than one iSH-AOK picks. On iOS that
choice lives in Settings; this variable is the equivalent for the standalone CLI
and for an Xcode scheme.

```sh
ISH_GUEST_SWAP_MB=512 ./ish -f build/alpine /bin/sh
cat /proc/ish/swap        # what the area looks like
```

Setting it also makes `/proc/ish/swap` writable by guest root, so a size in MB
turns swap on and `0` turns it off:

```sh
echo 0 > /proc/ish/swap    # page everything back in and release the file
echo 256 > /proc/ish/swap  # re-enable at a different size
```

That write is **refused on an installed app**, where the variable is never set:
on a device, enabling swap is the user's decision in Settings, not something a
guest process can do to their flash. It exists so that turning swap off -- which
has to bring every evicted page back before it releases the file -- is testable
outside the app.

The backing file is created with `mkstemp` in `TMPDIR` and unlinked
immediately, so it is never visible in the container, never backed up, and never
reachable through the File Provider. It is truncated and closed when swap is
turned off.

## `ISH_GUEST_SWAP_WRITE_BUDGET_MB`

Caps how much simulated swap may write in a rolling 24-hour window, in MB.
Once the window is spent, eviction is refused and reported in
`/proc/ish/swap` as `budget_refusals`; faulting pages back keeps working,
because refusing a read would be a SIGBUS on memory the guest mapped
correctly.

The built-in backstop is 4096 MB, and it is deliberately well above the figure
Apple's disk-writes instrumentation is said to notice (1 GB a day is the
number that gets quoted) rather than at it: it is there to stop a pathological
workload writing tens of gigabytes to the user's flash, not to shape a normal
one. `0` turns the cap off entirely.

Like the other knobs here it is read only on a launch that sets
`ISH_GUEST_SWAP_MB`, so an installed app always gets the built-in value. It
exists to make the budget reachable in a test -- the real one would take a day
and four gigabytes of writes to hit:

```sh
ISH_GUEST_SWAP_MB=128 ISH_GUEST_SWAP_WRITE_BUDGET_MB=16 ./ish -f build/alpine /bin/sh
```

## `mlock` and swap

`mlock(2)`, `mlockall(2)` and their `mun*` counterparts are real once swap is
on: iSH-AOK will not page out a locked page. Eviction refuses any 16 KiB host
frame with a locked guest page in it, so one locked page keeps its three
neighbours resident too.

Before the pager existed these were a range check and nothing more, which was
defensible then -- there was no swap for a lock to be advisory against. It is
not defensible now, because keeping a secret out of swap is the whole reason
the call exists.

**The honest scope**: this is AOK's promise, not the operating system's. iOS
manages its own memory and can page the app out regardless; iSH-AOK has no way
to pin host pages and never has. So a locked page will not be written to the
swap file, and that is all `mlock` can mean here.

The lock is visible where Linux shows it: `VmLck` in `/proc/<pid>/status`,
`Locked` and the `lo` flag in `smaps`, and a separate `maps` line wherever a
lock starts or ends. It belongs to the mapping, so it stays with a page that is
copied after a `fork` or a debugger's write, and with a mapping that grows or
moves -- the stack of a locked process, an `mremap` -- while a `fork`'s child
starts with no locks at all.

`mlockall(MCL_CURRENT)` locks everything already mapped; `MCL_FUTURE`, and the
`MAP_LOCKED` flag to `mmap`, lock new mappings too -- `mmap`, `brk` and
`shmat` alike. As on Linux, what is locked is populated, readable or writable
parts of large untouched mappings included; `PROT_NONE` mappings have each page
locked when it is first touched.

`mlock2(2)` is `mlock` with flags, and its one flag, `MLOCK_ONFAULT`, is the
on-fault lock that `MCL_ONFAULT` gives `mlockall`: the mapping is locked and
counted in `VmLck` at once, but nothing is brought in until it is touched --
not by the call, and not later when `mremap` grows the mapping or `mprotect`
makes it writable, both of which populate an ordinary locked mapping. `smaps`
shows such a mapping as `lo lf`, and `maps` starts a new line where the `lf`
starts or ends. A later plain `mlock` (or `mlockall(MCL_CURRENT)`) turns it
back into an ordinary lock and populates it.

`RLIMIT_MEMLOCK` is enforced the way Linux enforces it: with a limit of 0,
`mlock`, `mlockall` and `MAP_LOCKED` fail with `EPERM`; past the limit `mlock`
and `mlockall(MCL_CURRENT)` fail with `ENOMEM`, a locked `mmap` or `mremap` with
`EAGAIN`, and a locked stack that would grow past it gets `SIGSEGV`. The
standalone CLI runs as **root**, which is exempt (Linux exempts
`CAP_IPC_LOCK`), so testing the limit means dropping privilege first -- as root
every `mlock` simply succeeds.

Not there yet: a process restored after iOS suspended the app comes back
holding no locks. And locking does not copy a page the process still shares
copy-on-write with its parent or child after a `fork`, as Linux does: `smaps`
counts such a page at half in `Pss` and `Locked` until one of them writes to
it.

## `ISH_GUEST_SWAP_FAIL_READS`

Makes every swap slot read fail, so the error path can be exercised. A real
failure needs a truncated file, a failing volume or corrupt data -- none of
which a test can arrange from inside a guest, and an error path that has never
run is one that is broken when it finally does.

With it set, touching an evicted page delivers **SIGBUS** with `si_code`
`BUS_ADRERR`, which is Linux's answer for a fault whose address is valid but
whose contents could not be fetched. Not SIGSEGV: the mapping is fine, and a
program with a SIGBUS handler takes a different branch entirely.

Read only on a launch that already set `ISH_GUEST_SWAP_MB`, so it is
unreachable from an installed app.

```sh
ISH_GUEST_SWAP_MB=64 ISH_GUEST_SWAP_FAIL_READS=1 ./ish -f build/alpine /bin/sh
```

## The suspension gate

Not an environment variable, but the same file. Paging writes to a file, and
being mid-write when iOS freezes the app is not a state to be in, so the app's
suspension handler holds a gate that stops new eviction and waits for anything
in flight. Faults are deliberately left alone: nothing runs guest code while
suspended, and a fault already in flight has to finish or the frame it is
restoring stays unreadable with its bytes only on disk.

On a launch with guest control the gate can be driven by hand, which is the
only way to test it -- on a device it is engaged by a background-task expiry
that a test cannot schedule against:

```sh
echo quiesce > /proc/ish/swap   # hold it; eviction stops
echo resume > /proc/ish/swap    # lift it
```

## Resource limits

Not an environment variable — an ordinary `setrlimit`/`ulimit`, enforced the
way Linux enforces it:

- **`RLIMIT_CPU`**: `SIGXCPU` at the soft limit and each further second of
  CPU time, with the soft limit raised a second at a time (so `getrlimit`
  shows the new value); `SIGKILL` at the hard limit; nothing else when soft
  and hard are equal (`bash`'s `ulimit -t`).
- **`RLIMIT_AS`** and **`RLIMIT_DATA`**: checked the way Linux's `mmap`,
  `mremap`, `brk` and `shmat` check them, from a page-table walk of the
  process. `mprotect` making private pages writable counts against
  `RLIMIT_DATA` too, because that is how glibc's thread arenas grow.
- A soft limit above the hard one is refused (`EINVAL`); raising a hard limit
  needs `CAP_SYS_RESOURCE`; changing another process's limits needs your
  real, effective and saved ids to all match its.

```sh
sh -c 'ulimit -t 1; while :; do :; done'   # SIGXCPU after 1s of CPU, not wall time
```

**`tmpfs`'s `size=`/`nr_blocks=`** mount options are enforced too (bytes with
`k`/`m`/`g`/`t`/`p`/`e` suffixes, or a percentage of `MemTotal`; `0` is
unlimited; half of `MemTotal` when neither is given, as Linux defaults).
Each file is charged its size in pages; a write, `truncate` or `fallocate`
that would pass the limit is cut short and fails with `ENOSPC`. `statfs`
reports the limit and the current charge, so `df` on a tmpfs mount is no
longer fiction.

## BSD process accounting

`acct(2)` works now, so a guest can turn on BSD-style process accounting the
normal way:

```sh
touch /var/log/pacct
accton /var/log/pacct   # start writing a record for every process that exits
accton off              # or: accton with no file
```

Debian's `acct_v3` record format is produced (64 bytes, what `atopacctd` and
`sa`/`lastcomm` expect), written once per process from the same place Linux
writes it. Two fields are worth knowing if you read the records by hand
rather than through a tool: `ac_etime` is elapsed time in centisecond ticks,
not seconds, and `ac_exitcode` is the wait-encoded status (so `false` records
256, not 1). `ac_mem` is peak RSS rather than Linux's virtual size at exit —
the same units, without walking the address space on every process exit for
an advisory field. Off — which is every system that has never called
`acct(2)` — costs one atomic read on process exit and nothing else.

## Other hardening worth knowing about

Not configurable, and not previously enforced at all:

- **`seccomp(2)`** works: strict mode kills a thread that calls anything but
  `read`, `write`, `exit` and `sigreturn`; classic-BPF filter mode is checked
  and run the way Linux runs it, actions and all
  (`SECCOMP_RET_KILL_PROCESS`/`KILL_THREAD`/`TRAP`/`ERRNO`/`TRACE`/`LOG`/`ALLOW`).
  A sandbox built on it — OpenSSH's pre-authentication child, `systemd`'s
  `SystemCallFilter=`, `apt` and `man-db`'s helpers — is now actually confined
  rather than merely believing it is.
- **`PROT_EXEC` is enforced.** Code in a page mapped without it — the stack,
  the heap, an overflowed buffer — faults with `SIGSEGV`/`SEGV_ACCERR` instead
  of running.
- **Another user's process is not yours to inspect.** See
  [proc-ish.md](proc-ish.md#every-processs-architecture-and-who-may-ask) for
  the `ptrace`/`/proc` access gate.
- **`chgrp` is restricted to a group you are in.** An owner may move a file to
  their own `fsgid` or a supplementary group, or leave it where it is;
  anything else needs `CAP_CHOWN` — closing a path to running a program as an
  arbitrary group via `chgrp` + `chmod g+s`.

## Logging

Log channel selection (`strace` — syscall parameters and return values, the most
useful — plus `verbose` and friends) is a **build-time** setting, controlled via
`ISH_LOG` in `app/iSH.xcconfig` for the iOS app, or `meson configure -Dlog=...`
for the CLI; `-Dnolog=...` turns individual channels back off. There is no
runtime switch, so a build compiled without `strace` cannot be made to trace —
which is why this is not in the list of environment variables above.
