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

## Logging

Log channel selection (`strace` — syscall parameters and return values, the most
useful — plus `verbose` and friends) is a **build-time** setting, controlled via
`ISH_LOG` in `app/iSH.xcconfig` for the iOS app, or `meson configure -Dlog=...`
for the CLI; `-Dnolog=...` turns individual channels back off. There is no
runtime switch, so a build compiled without `strace` cannot be made to trace —
which is why this is not in the list of environment variables above.
