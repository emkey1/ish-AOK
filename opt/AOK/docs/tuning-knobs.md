# Runtime tuning: CPU count and memory headroom

A few environment variables let you tune how iSH-AOK presents itself
to the guest. These apply when you can control the process environment —
building and running the standalone CLI emulator, or launching the app
from Xcode with a custom scheme environment — rather than something an
App Store install lets you change day to day.

## `ISH_GUEST_CPU_COUNT`

Overrides the CPU count iSH-AOK reports to the guest — `/proc/cpuinfo` and
`/proc/stat`, and through them what `nproc` and `sched_getaffinity` answer.

On iOS, and only on a device with more than two cores, iSH-AOK reserves roughly
a third of them (at least one) back from the scheduler-sizing queries `nproc`
and `sched_getaffinity`, so that `make -j$(nproc)` and programs that size
themselves from the affinity mask leave the app some headroom; `/proc/cpuinfo`
still shows every core. The standalone CLI on Apple silicon does **not** do
that — with no override it runs a fixed 4 emulated CPUs regardless of the host's
core count. Set `ISH_GUEST_CPU_COUNT` to override either:

```sh
ISH_GUEST_CPU_COUNT=6 ./ish -f build/alpine /bin/sh   # match a specific core count
ISH_GUEST_CPU_COUNT=1 ./ish -f build/alpine /bin/sh   # force a serial (single-core) guest
```

Forcing `=1` is particularly useful when debugging a concurrency bug: it
rules out cross-core races as the cause by construction.

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

`RLIMIT_MEMLOCK` is enforced, and exceeding it returns `ENOMEM` as Linux has
since 2.6.9. Note that the standalone CLI runs as **root**, which is exempt
(Linux exempts `CAP_IPC_LOCK`), so testing the limit means dropping privilege
first -- as root every `mlock` simply succeeds.

`mlockall(MCL_FUTURE)` is accepted and recorded but not yet acted on: it would
have to reach the page-table layer, which knows nothing about a process's
flags. `MCL_CURRENT` locks everything already mapped, which is the half that
works today.

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

## Logging

Log channel selection (`strace` — syscall parameters and return values, the most
useful — plus `verbose` and friends) is a **build-time** setting, controlled via
`ISH_LOG` in `app/iSH.xcconfig` for the iOS app, or `meson configure -Dlog=...`
for the CLI; `-Dnolog=...` turns individual channels back off. There is no
runtime switch, so a build compiled without `strace` cannot be made to trace —
which is why this is not in the list of environment variables above.
