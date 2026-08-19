# Runtime tuning: CPU count and memory headroom

A couple of environment variables let you tune how iSH-AOK presents itself
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
guest new memory: once the app's available-memory budget drops under it, `mmap`
and `mremap` growth are **refused with `ENOMEM`** — rather than throttled — so a
runaway guest cannot get the whole app jetsammed. Defaults to 192 MB; set it to
`0` to disable the guard entirely.

This guard is **iOS-only**. On a macOS or Linux host there is no per-process
jetsam budget, so it is compiled out and the variable has no effect on the
standalone CLI — set it in the Xcode scheme environment for a run on a device:

```
ISH_GUEST_MEM_HEADROOM_MB = 0
```

## Logging

Log channel selection (`strace` — syscall parameters and return values, the most
useful — plus `verbose` and friends) is a **build-time** setting, controlled via
`ISH_LOG` in `app/iSH.xcconfig` for the iOS app, or `meson configure -Dlog=...`
for the CLI; `-Dnolog=...` turns individual channels back off. There is no
runtime switch, so a build compiled without `strace` cannot be made to trace —
which is why this is not in the list of environment variables above.
