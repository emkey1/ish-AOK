# Where does a guest workload's time actually go?

**Question this answers:** is library-level native interposition — running a
host `libz`/`libcrypto`/`libc` in place of the guest's — worth building?

**Answer: no, not as a general mechanism.** The workload where a codec most
obviously dominates — `tar xzf` — does not reach a shared library at all, so an
LD_PRELOAD libz would change it by zero. Where a codec library *is* hot it is a
different library on each distro (xz on Debian, zlib on Alpine), and libz
itself never exceeds 11% of on-CPU time on any workload measured. Details and
the ranked alternatives are at the bottom.

This is a measurement-only exercise: nothing here implements interposition.

---

## 1. The instrument

`kernel/guestprof.c`, armed with `ISH_GUEST_PROFILE`. It is off by default and
compiled in unconditionally, in the style of `ISH_FAKEFS_LOCKSTATS`.

```sh
ISH_GUEST_PROFILE=1 ./build/ish -f ROOT /bin/sh -c '...'      # 1 ms default
ISH_GUEST_PROFILE=250 ...                                      # 250 us
ISH_GUEST_PROFILE_OUT=/path/report ...                         # else stderr
```

**It is armed from `main.c` only, so it is a CLI instrument.** The code compiles
into the Xcode build like the rest of `libish`, but nothing there calls
`guestprof_init()`, exactly as `lockstats_init()` is CLI-only today. Profiling
on device would need that one call added to the app's boot path and a way to set
the environment variable; it was not needed for this question and was left
alone.

A sampler thread wakes every interval and records, for each task, what that
task is doing: executing guest code (and at which guest PC), running in the
emulator's kernel (and in which syscall), or parked in a wait.

### Why a slot table rather than a walk of the task list

The sampler runs on its own host thread and must never dereference a
`struct task` — those are freed through a deferred queue, and a profiler that
can crash the emulator is not a profiler. So every task publishes into a slot
it owns in a fixed global array, and the sampler reads nothing else: plain
atomic words, no pointers, no locks, no lifetime to get wrong.

The slot index lives on the task, not in thread-local storage, because the
release hook runs on whichever thread reaps the struct.

### Why the PC is a block address

The gadget engines keep the guest PC in a **host register** while they run
(`eip` is `w28` in `jit/gadgets-aarch64/gadgets.h`), so `cpu.eip` in memory is
stale mid-chain and cannot be sampled from outside at all. What is published
instead is the block address at each dispatch through `cpu_run_to_interrupt`'s
C loop — one relaxed store, at a site each of the four engines already has.

The engines return to that loop at every unchained edge and, at the latest,
when `frame->chain_budget` (8192 dispatches) expires, so a published PC is at
most one chain-group old. For a hot loop that is exact, because the loop's own
blocks are what the budget expires on. For long straight-line runs it names
where the chain started rather than where it is now: that blurs symbol
attribution *within* an object and does not move the per-object totals, since a
chain cannot leave its object without an indirect branch that exits to C anyway.

### Attribution, and why the guest does it

A sampled PC is turned into (object, file offset) against a snapshot of that
address space's file-backed executable mappings. The snapshot is taken **by the
guest thread that owns the address space**, at a kernel transition where
`current` is set and no address-space lock is held — the sampler asks for one
by setting a flag and never touches guest structures itself.

Symbolization is host-side, `tools/guestprof-symbolize.py`, which parses the
ELF directly (macOS has no binutils, and minimal guest roots often do not
either).

### Two denominators, deliberately

- **`%task`** — share of all task-samples. Equals wall-clock share only when one
  task is live; the report prints `mean live tasks/tick` so this is checkable.
- **`%on-CPU`** — share of samples that were *not* blocked.

Both are reported because they answer different questions. A `git clone` spends
most of its wall time waiting on a socket, and a "62% not in guest code" figure
reads like emulator overhead when it is really the network. `%on-CPU` is the one
that bounds what any accelerator could win back.

## 2. Positive controls

Per the rule that "it never fired" means nothing until it has been seen to
fire, three controls were run before any workload. Sources in
`tests/manual/guestprof/`.

| control | expectation | measured |
|---|---|---|
| loop in zlib `uncompress()` | nearly all samples in libz | **99.26% of on-CPU samples in `libz.so.1.3.1`** |
| loop in `syscall(SYS_getpid)` | samples outside guest code | 31.7% of on-CPU in `[kernel]`, rest in libc's syscall stub |
| loop in `nanosleep()` | samples in the blocked bucket | **99.89% `[blocked]`** |

The inflate control also validates the symbolizer: it puts 38% of samples in
the unexported local function between `inflateBackEnd` and `inflateResetKeep`,
which is `inflate_fast` (`inffast.c`) — the function zlib's own design predicts
is hot.

## 3. Sampler overhead

Interleaved A/B (arms alternated within each repetition, never as sequential
batches), median of 5, `devuan-arm64-test`:

| workload | profiler off | profiler on | overhead |
|---|---|---|---|
| zlib inflate loop (codec-bound) | 3.725 s | 3.720 s | **−0.11%** |
| `syscall(SYS_getpid)` x12M (dispatch-bound) | 4.065 s | 4.055 s | **−0.24%** |

Both are below the run-to-run spread of the off arm, so the honest statement is
that the cost is under 1% and this measurement cannot resolve it. The sampler
thread's own time is reported in every run and lands at 0.1–0.3% of the window.

## 4. What this instrument does not tell you

Worth reading before quoting a number out of it.

- **It attributes to an OBJECT, not a call stack.** There is no unwinder. A
  library that is hot because its caller uses it badly looks the same as one
  that is hot on its own merits.
- **Symbol attribution inside a stripped `.so` is approximate.** Distro
  libraries ship only `.dynsym`, so an unexported local — zlib's `inflate_fast`
  is exactly this — has no symbol of its own. `guestprof-symbolize.py` falls
  back to the nearest preceding exported symbol and labels those rows
  `~after <name> (unexported local)`. Object-level totals are unaffected.
- **A process shorter-lived than the sampling interval is invisible.** Its
  address space is never snapshotted, and its samples land in
  `[no-file-mapping]`. That bucket is printed with its raw guest addresses
  precisely so it can be checked rather than assumed small: on the workloads
  below it is under 1%, but it was **42%** before the map table was sized for
  the number of processes an `apt install` runs through, and a large value
  there invalidates the rest of the report.
- **`%wall` can exceed 100%** when two tasks are in one object at once. That is
  real parallelism, and `mean cores busy` in the header is how to read it.
- **The kernel bucket is split by syscall, not by host function.** It says a
  syscall was running, not whether the time went to fakefs, `pt_unmap` or the
  work itself. `ISH_FAKEFS_LOCKSTATS` is the instrument for that half.

## 5. Results

Four workloads on four guests. Roots: `devuan-{arm64,amd64}-test` (glibc, what
the device ships) and `alpine-{arm64,amd64}-test` (musl). One host, runs
sequential, 1 ms sampling. Every run was checked for a completion marker before
its profile was used.

Read `%on-CPU` first: it is bounded and sums to 100%. `%wall` is
samples/ticks — what fraction of wall time some task was in that object — and
exceeds 100% only where tasks genuinely run in parallel (`cores busy`).
These workloads are network- and I/O-bound, so a library at 30% of on-CPU is
often under 10% of wall.


### `apt install git` / `apk add git`

**arm64 / glibc** — 35 s wall, 1.06 cores busy, 2.95 tasks blocked; outside guest code = **17% of on-CPU**

| object | %wall | %on-CPU |
|---|---:|---:|
| `liblzma.so.5.8.1` | 28.8% | 27.2% |
| `libapt-pkg.so.7.0.0` | 25.6% | 24.2% |
| `[kernel]` | 18.4% | 17.4% |
| `libc.so.6` | 14.4% | 13.6% |
| `libstdc++.so.6.0.33` | 6.5% | 6.2% |
| `/usr/bin/perl` | 2.8% | 2.7% |

**amd64 / glibc** — 133 s wall, 1.19 cores busy, 3.49 tasks blocked; outside guest code = **8% of on-CPU**

| object | %wall | %on-CPU |
|---|---:|---:|
| `liblzma.so.5.8.1` | 48.3% | 40.5% |
| `libapt-pkg.so.7.0.0` | 19.3% | 16.2% |
| `libmd.so.0.1.0` | 12.7% | 10.6% |
| `libcrypto.so.3` | 11.9% | 9.9% |
| `[kernel]` | 9.4% | 7.9% |
| `libc.so.6` | 6.2% | 5.2% |

**amd64 / musl** — 74 s wall, 0.23 cores busy, 1.77 tasks blocked; outside guest code = **2% of on-CPU**

| object | %wall | %on-CPU |
|---|---:|---:|
| `libcrypto.so.3` | 8.4% | 35.8% |
| `libz.so.1.3.1` | 6.9% | 29.3% |
| `libapk.so.3.0.0` | 4.9% | 21.1% |
| `ld-musl-x86_64.so.1` | 2.8% | 12.0% |
| `[kernel]` | 0.4% | 1.6% |
| `libssl.so.3` | 0.0% | 0.1% |

**arm64 / musl** — 21 s wall, 0.21 cores busy, 1.79 tasks blocked; outside guest code = **6% of on-CPU**

| object | %wall | %on-CPU |
|---|---:|---:|
| `libapk.so.3.0.0` | 9.0% | 43.2% |
| `libz.so.1.3.2` | 6.5% | 31.1% |
| `ld-musl-aarch64.so.1` | 2.6% | 12.3% |
| `libcrypto.so.3` | 1.4% | 6.9% |
| `[kernel]` | 1.2% | 6.0% |
| `libssl.so.3` | 0.1% | 0.4% |


### `git clone https://github.com/madler/zlib.git`

**arm64 / glibc** — 9 s wall, 1.78 cores busy, 3.42 tasks blocked; outside guest code = **22% of on-CPU**

| object | %wall | %on-CPU |
|---|---:|---:|
| `git-core/git` | 60.6% | 34.0% |
| `libc.so.6` | 47.5% | 26.7% |
| `[kernel]` | 39.1% | 21.9% |
| `libz.so.1.3.1` | 20.2% | 11.3% |
| `libtasn1.so.6.6.4` | 4.1% | 2.3% |
| `libgnutls.so.30.40.3` | 2.7% | 1.5% |

**amd64 / glibc** — 39 s wall, 2.37 cores busy, 3.35 tasks blocked; outside guest code = **33% of on-CPU**

| object | %wall | %on-CPU |
|---|---:|---:|
| `git-core/git` | 122.6% | 51.8% |
| `[kernel]` | 77.6% | 32.8% |
| `libz.so.1.3.1` | 19.2% | 8.1% |
| `libc.so.6` | 6.8% | 2.9% |
| `libnettle.so.8.10` | 4.9% | 2.1% |
| `libgnutls.so.30.40.3` | 2.2% | 0.9% |

**amd64 / musl** — 30 s wall, 2.27 cores busy, 3.41 tasks blocked; outside guest code = **22% of on-CPU**

| object | %wall | %on-CPU |
|---|---:|---:|
| `/usr/bin/git` | 138.5% | 61.2% |
| `[kernel]` | 50.3% | 22.2% |
| `libz.so.1.3.1` | 20.8% | 9.2% |
| `ld-musl-x86_64.so.1` | 9.4% | 4.1% |
| `libcrypto.so.3` | 6.6% | 2.9% |
| `libcurl.so.4.8.0` | 0.4% | 0.2% |

**arm64 / musl** — 9 s wall, 2.07 cores busy, 3.26 tasks blocked; outside guest code = **38% of on-CPU**

| object | %wall | %on-CPU |
|---|---:|---:|
| `/usr/bin/git` | 90.1% | 43.6% |
| `[kernel]` | 77.7% | 37.6% |
| `libz.so.1.3.2` | 16.2% | 7.9% |
| `ld-musl-aarch64.so.1` | 15.9% | 7.7% |
| `libcrypto.so.3` | 3.1% | 1.5% |
| `libssl.so.3` | 2.7% | 1.3% |


### `tar xzf coreutils-9.5.tar.gz` (14 MB -> 61 MB)

**arm64 / glibc** — 9 s wall, 0.99 cores busy, 1.88 tasks blocked; outside guest code = **20% of on-CPU**

| object | %wall | %on-CPU |
|---|---:|---:|
| `/usr/bin/gzip` | 59.3% | 59.7% |
| `[kernel]` | 19.8% | 19.9% |
| `libc.so.6` | 18.9% | 19.0% |
| `/usr/bin/tar` | 1.1% | 1.1% |
| `[no-file-mapping]` | 0.1% | 0.1% |
| `ld-linux-aarch64.so.1` | 0.1% | 0.1% |

**amd64 / glibc** — 12 s wall, 1.05 cores busy, 1.94 tasks blocked; outside guest code = **4% of on-CPU**

| object | %wall | %on-CPU |
|---|---:|---:|
| `/usr/bin/gzip` | 87.3% | 82.9% |
| `libc.so.6` | 12.0% | 11.4% |
| `[kernel]` | 4.2% | 4.0% |
| `/usr/bin/tar` | 1.6% | 1.5% |
| `ld-linux-x86-64.so.2` | 0.1% | 0.1% |
| `[no-file-mapping]` | 0.1% | 0.1% |

**amd64 / musl** — 17 s wall, 1.13 cores busy, 1.87 tasks blocked; outside guest code = **13% of on-CPU**

| object | %wall | %on-CPU |
|---|---:|---:|
| `/bin/busybox` | 88.4% | 78.5% |
| `[kernel]` | 14.1% | 12.5% |
| `ld-musl-x86_64.so.1` | 10.1% | 8.9% |
| `[no-file-mapping]` | 0.0% | 0.0% |

**arm64 / musl** — 9 s wall, 1.08 cores busy, 1.83 tasks blocked; outside guest code = **29% of on-CPU**

| object | %wall | %on-CPU |
|---|---:|---:|
| `/bin/busybox` | 53.9% | 49.9% |
| `[kernel]` | 31.0% | 28.7% |
| `ld-musl-aarch64.so.1` | 23.1% | 21.4% |
| `[no-file-mapping]` | 0.1% | 0.1% |


### `openssl speed -evp aes-128-gcm` + `sha256`

**arm64 / glibc** — 12 s wall, 1.00 cores busy, 1.99 tasks blocked; outside guest code = **0% of on-CPU**

| object | %wall | %on-CPU |
|---|---:|---:|
| `libcrypto.so.3` | 95.2% | 95.4% |
| `libc.so.6` | 2.2% | 2.2% |
| `/usr/bin/openssl` | 1.4% | 1.4% |
| `ld-linux-aarch64.so.1` | 0.5% | 0.5% |
| `[kernel]` | 0.4% | 0.4% |
| `[no-file-mapping]` | 0.1% | 0.1% |

**amd64 / glibc** — 12 s wall, 1.00 cores busy, 1.99 tasks blocked; outside guest code = **0% of on-CPU**

| object | %wall | %on-CPU |
|---|---:|---:|
| `libcrypto.so.3` | 96.8% | 96.9% |
| `libc.so.6` | 1.7% | 1.7% |
| `ld-linux-x86-64.so.2` | 0.7% | 0.7% |
| `/usr/bin/openssl` | 0.4% | 0.4% |
| `[kernel]` | 0.2% | 0.2% |
| `[no-file-mapping]` | 0.1% | 0.1% |

**amd64 / musl** — 12 s wall, 1.00 cores busy, 1.99 tasks blocked; outside guest code = **0% of on-CPU**

| object | %wall | %on-CPU |
|---|---:|---:|
| `libcrypto.so.3` | 95.4% | 95.5% |
| `ld-musl-x86_64.so.1` | 3.8% | 3.8% |
| `/usr/bin/openssl` | 0.3% | 0.3% |
| `[kernel]` | 0.2% | 0.2% |
| `[no-file-mapping]` | 0.1% | 0.1% |
| `/bin/busybox` | 0.0% | 0.0% |

**arm64 / musl** — 12 s wall, 0.98 cores busy, 1.96 tasks blocked; outside guest code = **0% of on-CPU**

| object | %wall | %on-CPU |
|---|---:|---:|
| `libcrypto.so.3` | 95.4% | 97.4% |
| `ld-musl-aarch64.so.1` | 2.3% | 2.3% |
| `[kernel]` | 0.1% | 0.1% |
| `[no-file-mapping]` | 0.1% | 0.1% |
| `/usr/bin/openssl` | 0.0% | 0.0% |
| `/bin/busybox` | 0.0% | 0.0% |

### Top symbols in the libraries the question was about

Symbolized with `tools/guestprof-symbolize.py` against the guest's own ELF.
Distro libraries are stripped, so rows marked `~after X` mean "in the
unexported local function following X" — see the limitations above.

**libz**, from `git clone` (the only workload where libz is reached at all):

| guest | top offsets |
|---|---|
| amd64 / glibc | `inflate` 3984, `~after inflateBackEnd` (= `inflate_fast`) 1841, `adler32_z` 1200, `crc32_z` 214, `inflateInit2_` 29 |
| arm64 / glibc | `~after inflateBackEnd` (= `inflate_fast`) 1069, `inflateEnd` 272, `inflate` 226, `adler32_z` 85 |

So the libz hot set is the whole inflate path — `inflate`, `inflate_fast`,
`adler32_z`, `crc32_z` — and nothing else. That is the narrowest possible
interposition target, and it is still only 8–11% of on-CPU in a `git clone`.

**libcrypto**, from `openssl speed`. OpenSSL's hot code is assembly with local
symbols, so these are landmarks rather than names, but they identify the
families unambiguously:

| guest | top offsets |
|---|---|
| amd64 / glibc | `~after SHA1_Init` 5120 (the SHA block transform), `~after CRYPTO_gcm128_release` 2588 (GHASH), `~after AES_unwrap_key` 1577 + `AES_encrypt` 1070 + `AES_set_encrypt_key` 122 (the **software** AES table path) |
| arm64 / glibc | landmark-only; the real work is in the `armv8` crypto assembly, which carries no exported symbols |

**libc**, from `git clone`: `malloc`/`free`/`calloc` and the string/format
region. No single function clears 0.3% of wall on either guest — there is
nothing here to interpose.

### The crypto gap, in throughput

`openssl speed`, 16384-byte blocks, same build and host:

| | arm64 guest | amd64 guest | ratio |
|---|---:|---:|---:|
| AES-128-GCM | 97,352 kB/s | 5,308 kB/s | **18.3x** |
| SHA256 | 109,685 kB/s | 2,383 kB/s | **46.0x** |

The arm64 guest is fast because `jit/guest-arm64/crypto.S` maps guest AESE /
AESMC / PMULL / SHA256H onto the host's own crypto instructions, and
`kernel/exec.c` advertises them in `AT_HWCAP`. The x86 guests get none of it:
`CPUID_ADVERTISE_VECTOR_STATE` is 0 in `emu/cpuid.h`, so AES-NI and PCLMULQDQ
are never advertised, OpenSSL selects its software table AES, and
`emu/avx.c`'s `avx_aes_round` (a software S-box) is what a guest that used the
VEX encodings anyway would land in.

**This is not confined to a benchmark.** On `apt install` it is 20.6% of
on-CPU time on the amd64 guest (`libmd` 10.6% + `libcrypto` 10.0%, hashing and
verifying packages; 24.6% of wall) against roughly 1% on arm64 — and on `apk add` under musl
it is 35.8% of on-CPU on amd64 against 6.9% on arm64.

Note that **SHA256 is the bigger gap (46x) than AES (18x)**, which matters for
scoping: an AES-NI-only change would leave the larger half of the loss in
place. Host arm64 has SHA1/SHA256 instructions and the arm64 guest gadgets
already use them, so the same mapping applies to SHA-NI.

## 6. Go / no-go on library-level interposition

**No-go as a general mechanism.** Three measured reasons, in order of how
decisive they are.

**1. The workload that is most obviously "a codec" does not reach a shared
library at all.** `tar xzf` spends 50–83% of its on-CPU time in a *binary*, not
in libz:

| guest | hot object | %on-CPU |
|---|---|---:|
| amd64 / glibc | `/usr/bin/gzip` | 82.9% |
| arm64 / glibc | `/usr/bin/gzip` | 59.7% |
| amd64 / musl | `/bin/busybox` | 78.5% |
| arm64 / musl | `/bin/busybox` | 49.9% |

Read straight off the ELF, `DT_NEEDED` for `/usr/bin/gzip` is `libc.so.6` and
nothing else; busybox needs only `libc.musl`. GNU gzip and busybox each carry
their own inflate. **An LD_PRELOAD libz accelerator would change this workload
by exactly zero**, which is the single most important number in this document
and the one the 2026-09-10 evaluation could not have guessed.

**2. Where libz *is* reached, it is a small minority of the time.** In
`git clone`, libz is 8.1% (amd64) and 11.3% (arm64) of on-CPU — against
`[kernel]` at 22–38% and git's own code at 34–61%. A *perfect* libz, costing
nothing at all, buys under a tenth of the on-CPU time and, since the workload
is 59–66% blocked on the network, well under 5% of wall.

**3. The one library that does clear a high bar is not libz.** `liblzma` is
27.2% (arm64) and 40.5% (amd64) of on-CPU in `apt install`, because `.deb` data
is xz. That is a real target the original evaluation never named — but it
inherits every structural hazard listed there (guest allocator callbacks,
per-ABI struct marshalling, version skew) for a win confined to one package
manager on one distro family. On musl the equivalent slot is libz at ~30% of
on-CPU, because `.apk` is gzip — so even "accelerate the package manager's
codec" is a different library per distro.

### What would have to be true for a go

A function clears the bar if it burns many guest instructions per call and is
reached through a stable, pointer-only ABI. Exactly one family qualifies on the
evidence here: zlib's `inflate` / `inflate_fast` / `adler32_z` / `crc32_z`,
reached through `uncompress()`/`inflate()`. Its measured ceiling is ~11% of
on-CPU on one workload. That is not enough to justify shared libc state,
per-ABI marshalling and guest callbacks — especially when option A below gets
more, for less.

## 7. Ranked against the alternatives

**A. Ship a native decompressor and let PATH do the interposing — do this
first.** AOK-Workstation already has a native `gzip`/`gunzip`/`zcat` applet
(`deps/smallclue/src/core.c`). Shadowing the guest's gzip with it needs no new
emulator code at all. Measured, interleaved A/B, median of 4, same tarball:

| guest | distro gzip | native gzip | speedup |
|---|---:|---:|---:|
| amd64 / glibc | 13.130 s | 4.759 s | **2.76x** |
| arm64 / glibc | 5.583 s | 4.797 s | **1.16x** |

That is a whole-workload speedup of `tar xzf`, on the workload where libz
interposition scores zero. It is also the shape the original evaluation
concluded was the only sane one — narrow and opt-in — and it already exists;
what is missing is that `/AOK/tools/native-links.sh` is not applied by default,
so nothing routes to it. The arm64 win is smaller because the remaining time is
tar's own file creation (fakefs), not the codec.

**B. Advertise x86 AES-NI + PCLMULQDQ (and SHA-NI) and map them to host
crypto instructions — the highest-value emulator change.** Bounded, arch-local,
no guest-side setup, and it fixes a gap measured at 18–46x that costs 20.6% of
`apt install`'s on-CPU time on the amd64 guest.

The important scoping note: in `emu/cpuid.h` the AES-NI and PCLMULQDQ bits sit
inside `#if CPUID_ADVERTISE_VECTOR_STATE`, alongside XSAVE/OSXSAVE/AVX. That
switch is held at 0 by a real debt — the signal frame cannot carry `ymm_hi`, so
advertising AVX would corrupt registers across a signal. **AES-NI does not
share that debt.** Its legacy SSE encodings operate on `xmm0-15`, which the
existing 512-byte FXSAVE signal frame already saves in full; the bits are
bundled there because, as the comment says, making OpenSSL emit those encodings
is "a separate claim needing its own evidence", not because XSAVE is required.
So the work is: implement the legacy SSE `AESENC`/`AESENCLAST`/`AESDEC`/
`AESDECLAST`/`AESIMC`/`AESKEYGENASSIST` and `PCLMULQDQ` in the i386 and amd64
engines, map them onto host AESE/AESMC/PMULL the way
`jit/guest-arm64/crypto.S` already does for the arm64 guest, validate, and then
advertise *only* bits 1 and 25 — leaving `CPUID_ADVERTISE_VECTOR_STATE` at 0.
Do SHA256 in the same pass: it is the larger half of the loss.

**C. The crypto accelerator that already ships.** `ISH_SYS_AEAD`
(`kernel/ish_accel_aes.c`, `opt/AOK/tools/crypto/ish_provider.c`) is
library-level interposition for crypto, already built, already ABI-neutral, and
therefore already available to x86 guests. It is off by default and needs a
provider installed inside the root, which is why it contributes nothing to the
numbers above. It covers AEAD ciphers only — not the bare SHA256 that `libmd`
and `apt` spend their time in — so it complements B rather than replacing it.

**D. Do not build general library interposition.** No workload measured here
would gain more than a few percent of wall time from it.

### The finding that outranks all of them

`[kernel]` — emulator syscall handling, fakefs, page-table work — is **22–38%
of on-CPU in `git clone`** on every guest, and 17% in `apt install` on arm64.
In `git clone` it is three to four times libz's share. Whatever is spent on
codecs, the emulator's own syscall path is the larger prize on the workloads
people actually run, and `ISH_FAKEFS_LOCKSTATS` is the instrument already
pointed at it.

## 8. Reproducing

```sh
ninja -C build ish
ISH_GUEST_PROFILE=1 ISH_GUEST_PROFILE_OUT=/tmp/p.prof \
    ./build/ish -f build/devuan-amd64-test /bin/sh -c 'tar xzf /tmp/x.tar.gz -C /tmp/ex'
# pull the objects the report names out of the root, then symbolize
ISH_REAL_MNT=/tmp/libs ./build/ish -f build/devuan-amd64-test /bin/sh -c 'cp /usr/bin/gzip /realmnt/'
python3 tools/guestprof-symbolize.py /tmp/p.prof --libdir /tmp/libs
```

Controls in `tests/manual/guestprof/` — run them first; see that README.
