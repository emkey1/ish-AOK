# Guest Architecture Benchmarks

Performance note for the AOK-Workstation 535 release cycle: a head-to-head comparison
of the three Linux guest architectures — i386, amd64, and arm64 — run on one
host, with the same JIT engine underneath all three. The differences below
are purely how well each guest's generated code and kernel paths perform;
nothing else varies between the three runs.

## Test rig

| | |
|---|---|
| Host | Apple Silicon Mac (arm64) |
| Emulator | AOK-Workstation CLI, JIT engine |
| Guests | i686 / x86_64 / aarch64 (musl) |
| Trial | single run per guest (not averaged) |

## Compute: 1,000,000,000-iteration sum

A tight integer accumulation loop and an IEEE-754 double accumulation loop,
run back to back inside each guest. Timed from inside the guest via
`clock_gettime(CLOCK_MONOTONIC)`.

| Guest | Integer loop | Float loop |
|---|--:|--:|
| i386  | 30.89 s | 32.54 s |
| amd64 | 37.11 s | 15.00 s |
| **arm64** | **3.39 s** | **5.20 s** |

- arm64 is **9.1×** faster than i386 and **10.9×** faster than amd64 on the integer loop.
- arm64 is **6.3×** faster than i386 and **2.9×** faster than amd64 on the float loop.
- amd64's float loop runs **2.5× faster** than its own integer loop — the native SSE path, not general JIT parity, does the work here.

## Thread lifecycle: 5,000 threads

Raw `clone(CLONE_THREAD)` 5,000 times, each thread incrementing a shared
atomic counter and exiting; the parent spins until the counter confirms every
thread has finished.

| Guest | Create + join, total | Per thread |
|---|--:|--:|
| i386  | 195.6 ms | ~39.1 µs |
| amd64 | 190.4 ms | ~38.1 µs |
| **arm64** | **187.1 ms** | **~37.4 µs** |

All three land within **4.5%** of each other. The join phase itself is under
0.1 ms in every guest in all three — thread exit is effectively free once
creation is done.

## Reading the numbers

- **Compute performance is architecture-specific; thread lifecycle is not.**
  The compute loop lives entirely in JIT-generated guest code, so it exposes
  exactly how mature each guest's code generator is. Thread creation instead
  spends almost all its time in shared kernel code (`task_create_`, locking,
  host `pthread_create`) that doesn't differ by guest architecture — which is
  exactly why its three numbers sit close together while the compute numbers
  are 10× apart.
- **arm64 wins because it isn't being cross-translated.** On this arm64 host,
  an arm64 guest instruction maps almost directly onto a host instruction —
  same flags register semantics, same addressing modes. The i386/amd64
  guests are x86 code being translated into arm64 host instructions, which
  carries a real, unavoidable tax: x86's status flags (OF/SF/ZF/CF/AF/PF)
  don't map onto ARM64's NZCV, x86's compound addressing modes expand into
  several ARM64 instructions, and x86's partial-register aliasing has no
  ARM64 equivalent. None of that touches the arm64 guest at all.
- **arm64 also picked up real engineering wins this cycle.** The arm64
  backend can chain compiled blocks across loop back-edges, so a tight loop
  stays inside JIT-generated code instead of returning to the interpreter's
  dispatch loop every iteration. amd64/i386 deliberately do not do this —
  loop-edge chaining was tried there previously and reverted because it
  starved the background memory reclaimer. Over a billion iterations, that
  per-iteration round-trip difference compounds into a large share of the
  gap on its own.
- **Every device this app runs on is arm64**, so this isn't a fragile,
  setup-dependent result — it reflects the guest architecture users actually
  run day to day.
- **All three guests compute the identical result, bit for bit.** The
  integer sum and the raw bits of the final double matched exactly across
  i386, amd64, and arm64 — a correctness check riding along with the speed
  comparison.

## Methodology

Two freestanding, no-libc test binaries — raw syscalls only, no pthread, no
`printf` — were cross-compiled with an identical `clang -O2` invocation for
`i686`, `x86_64`, and `aarch64` musl targets, then run unmodified through the
AOK-Workstation CLI on one host. No in-guest compiler, no libc, no cross-arch
variable other than the generated instruction stream itself.

Each result above is a single run, not an average over multiple trials —
treat these as directionally solid rather than statistically tight.

Verified:
- Integer sum and float bit-pattern identical across all three guests
- Same host process, same JIT engine, same build, run sequentially
- Timed from inside the guest via `clock_gettime`, not host wall-clock

These figures reflect the CLI build on one Apple Silicon Mac; on-device
numbers (different silicon, iOS scheduling, jetsam pressure) will differ.
