# Benchmarks: bmm, bmt, and the amd64 JIT bench harness

iSH-AOK ships a couple of small, self-contained CPU/thread microbenchmarks
you can build and run from any guest root, useful for comparing performance
across devices, guest architectures, or JIT engine changes.

## `bmm` and `bmt`

`/AOK/tools/iSH_benchmark.tgz` is a small tar.gz containing a `benchmark/`
directory with two programs (plus `-O2`-built variants `bmm2`/`bmt2`,
alongside the default `-O0` builds `bmm`/`bmt` — historical benchmark
numbers were captured at `-O0`, which also happens to exercise the JIT's
stack-slot code paths more than an optimized build would):

- **`bmm`** ("benchmark math") — a CPU microbenchmark: a tight integer
  accumulate loop and a floating-point accumulate loop (1e9 iterations by
  default), each timed with `clock()`. Despite the name, this measures
  integer/float arithmetic throughput, not memory bandwidth.
- **`bmt`** ("benchmark threads") — spawns and joins 10,000 near-no-op
  pthreads back-to-back, timing total thread create/destroy overhead.

### Building and running

A companion script, `/AOK/tools/setup-ish-benchmark.sh`, extracts the
archive and builds it in one step:

```sh
sh /AOK/tools/setup-ish-benchmark.sh          # build everything
sh /AOK/tools/setup-ish-benchmark.sh bmm      # build just the bmm target
```

Useful environment overrides:

| Variable | Default | Purpose |
|---|---|---|
| `ISH_AOK_BENCHMARK_ARCHIVE` | `/AOK/tools/iSH_benchmark.tgz` | source archive |
| `ISH_AOK_BENCHMARK_DIR` | `/tmp/iSH_benchmark` | extraction/build directory |
| `CC` | `gcc` | compiler to use |

Then run the built binaries directly, e.g.:

```sh
/tmp/iSH_benchmark/benchmark/bmm
/tmp/iSH_benchmark/benchmark/bmt
```

Run the same binary across different installed roots (via
[`mount-root.sh`](roots.md)) to compare guest architectures on the same
device, or across devices to compare hardware.

## amd64 JIT translation-mode benchmark

For amd64-specific JIT tuning, the regression suite (`/AOK/tests`) also
ships `x86/amd64_jit_guest_bench.sh`. It
toggles `/proc/ish/amd64_jit` across the available JIT modes and times a
set of representative guest commands (`true`, `uname -a`, `busybox
--help`, `apk --help`) in each mode, writing a `summary.tsv` (columns:
case, iteration, mode, rc, wall_s) to `$ISH_AOK_BENCH_OUT` (default
`/tmp/amd64-jit-bench-<timestamp>`). This is a JIT translation-mode
comparison, distinct from the general-purpose `bmm`/`bmt` benchmarks
above.
