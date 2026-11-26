# AOK-Workstation Performance Optimizations — July 2026

A record of the emulator-performance work landed on `working` between
2026-07-19 and 2026-07-20 (commits `7532e8ea` … `6a163c2f`, plus merged
PR #504), with the benchmarks behind each change and the measurement
lessons learned along the way. Everything here is validated by differential
testing (accelerated vs. reference output must be bit-identical) before any
performance claim.

**Measurement note (applies throughout):** the CLI dev build is
`buildtype=debug` / `-O0`, which badly understates (and once inverted) the
results for any host-C-heavy path. All production-representative numbers
below were taken on a `-O2` emulator build (`meson setup build-o2
-Doptimization=2`), matching what the Release iOS app ships. Timings are
best-of-3 on staged files unless noted.

---

## 1. amd64 JIT: native SSE gadgets for the top 13 interpreter-bridge opcodes

**Commit `7532e8ea`** · continuation of the "eliminate the amd64 interpreter"
effort.

The `ISH_TRACE_AMD64_JIT_STATS` histogram showed 131k+ per-instruction
bridges to the interpreter on a small reference workload (`ls -laR`,
`md5sum`, gzip round-trip), dominated by 13 SSE2 opcodes — glibc's SSE2
`str*`/`mem*` functions and zlib hot loops.

New native aarch64-host gadgets, each in register *and* memory-operand form:

- `punpcklbw/wd/dq/qdq` (NEON `zip1`), `paddw`/`psubusw`/`por`
  (arrangement-parameterized NEON binop macro), `pcmpeqb` mem form
- the `movq`/`movd` move family (`F3 0F 7E`, `66 0F 7E`, `66 0F 6E`,
  `66 0F D6`) with new 4/8-byte TLB-fast-path load/store gadgets
- `pshufd`/`pshuflw`/`pshufhw` + `pslldq`/`psrldq` via **one** generic `tbl`
  gadget whose 16-byte shuffle mask is baked into the code stream at
  translation time (the imm8 is a compile-time constant)

| metric | before | after |
|---|---|---|
| vec-bridges, reference workload | 131,072 | 1,024 (**128×** fewer) |

Validated: full-range forced-interpreter vs. JIT differential runs
bit-identical (md5/sha1/sha256, gzip -9 round-trips, grep/sort/tr/perl);
`amd64_regress` PASS in-guest compiled by the guest's own gcc; i386/arm64/
riscv64 guests unaffected.

Remaining tail (documented for the next round): `0F 2A/12/C6/2F/DA/DB`
(a few hundred/run) and the block-level fallbacks PSHUFB / PALIGNR /
PCMPISTRI / FXSAVE — the first two are `tbl`-able but need the decoder
extended to 3-byte opcodes first.

## 2. arm64 guest JIT: ANDS/TST + B.cond fusion (PR #504, merged `4067a8db`)

Community PR fusing the ANDS(imm)/ANDS(reg)/TST + B.cond instruction pairs
into single gadget dispatches (8–15 % of conditional branches follow an ANDS
test). Review caught a real bug — the TST skip-store check (`cmp x9, #31`)
clobbered NZCV before `b.cc` read it, breaking `/bin/echo` at `ld.so` — which
the author fixed after a register-form/immediate-form bisection isolated it.

Merged with a new dedicated regression test
(`tests/manual/arm64/ands_bcond_fusion.c`): all 14 condition codes ×
{imm,reg} × {32,64-bit} × {TST,normal-Rd}, 140/140 probes pass natively on
Apple Silicon and in-guest with fusion enabled. Runs in the on-device suite
via `fs/aok-tests.manifest`.

## 3. HLE: high-level emulation of guest libc functions (arm64/riscv64)

**Commits `62895a14` … `cce222b9`** · default **OFF**
(CLI `ISH_HLE=1`, app: "HLE Accel" switch in Settings).

When enabled, block translation checks whether a block starts at the entry
point of a known libc function (exact 64-byte prologue fingerprint from the
bundled rootfs libcs). On a match the whole block becomes one "giant gadget"
that bridges into a host-C implementation running the function's entire
contract, then exits at the guest return address. An updated/unknown libc
simply never matches — plain emulation is always the correct fallback, so
nothing breaks on guest upgrades.

**22 functions** across glibc-2.41 (arm64), musl (arm64), musl (riscv64) —
64 fingerprints: `memcpy` `memmove` `memset` `memcmp` `memchr` `memrchr`
`strlen` `strnlen` `strcmp` `strncmp` `strchr` `strrchr` `rawmemchr`
`strspn` `strcspn` `strpbrk` `strcpy` `stpcpy` `strncpy` `stpncpy`
`strcat` `strncat`.

### The load-bearing fix: direct host pointers (`643ea9eb`)

The first implementation bounced data through a 256-byte stack buffer via
`tlb_read`/`tlb_write` and benchmarked **~2× slower** than plain emulation —
the JIT's translated memcpy already writes host memory directly with NEON.
Rewritten to resolve a direct host pointer per guest page and run one native
`memcpy`/`memset`/`memcmp`/`memchr` per in-page span:

**Microbenchmark** (memcpy/memset/memcmp/strlen loop, arm64 guest, best-of-3):

| buffer | speedup (HLE on vs off) |
|---|---|
| 256 B | 1.23× |
| 4 KB | 3.16× |
| 64 KB | **7.17×** |
| 1 MB | 6.68× |

riscv64: a consistent ~2.0× across the same sizes.

### Real-workload picture (honest)

Broad sweep on staged 124 MB files, arm64, best-of-3:

| workload | off | on | delta |
|---|---|---|---|
| `sort` (4M records) | 8.35 s | 6.49 s | **+22 %** |
| `base64` | 2.37 s | 2.25 s | +5 % |
| `wc` | 1.72 s | 1.67 s | +3 % |
| `sort \| uniq` | 8.14 s | 7.88 s | +3 % |
| `md5` / `tr` / `cut` / `sha256` | — | — | neutral |

HLE meaningfully helps data-movement-heavy workloads and is neutral where a
program's own arithmetic dominates. It does **not** help ssh/scp (crypto-
bound — see §4). Validated throughout by bit-identical differential runs on
all three images, dedicated edge-case tests (both libcs), and the full
69-test guest regression suite passing under `ISH_HLE=1` (the single failure,
`fcntl_setown`, is pre-existing and arch-independent — spun off separately).

Support tooling that shipped with it: `ISH_HLE_TRACE=1` attach logging plus a
near-miss tracer that ranks recurring unmatched function prologues as future
fingerprint candidates; `tools/hle_fingerprint_guest.c` regenerates the
table (required release step if bundled libcs change — a stale table costs
speed, never correctness).

## 4. Crypto accelerator: host-native ChaCha20(-Poly1305) (arm64/riscv64)

**Commits `84e95739` … `6a163c2f`** · default **OFF** (`ISH_CRYPTO_ACCEL=1`;
app toggle not yet wired). Motivated by "would large scp's benefit?" —
measured answer: scp is cipher-bound, and the riscv64 guest runs OpenSSL's
*scalar* ChaCha20 (RV64GC has no vector extension; NEON is ARM-only), ~4.4×
slower than arm64's NEON path even before this work.

Fingerprinting libcrypto was investigated and rejected (stripped internal
symbols, stateful Poly1305, fastest-rotting update treadmill). Instead, a
paravirt design:

- **`kernel/ish_accel_crypto.c`** — plain, auditable RFC 8439
  ChaCha20 + Poly1305 + AEAD, one-shot *and* streaming (state carries across
  arbitrary spans). Validated against the RFC vectors and **3200/3000-case
  differential fuzz vs. OpenSSL** (one-shot / streaming with random span
  splits) — zero mismatches; startup self-test gates the feature.
- **`ISH_SYS_AEAD` (syscall `0xacc0`)** — guest submits a request struct;
  the host runs seal/open/raw-stream over the guest's buffers.
- **Zero-copy** (`5a21306f`) — `user_transform_two()` walks both guest
  buffers in lockstep page-spans and hands the streaming cipher direct host
  pointers (write pointer resolved before read per span; safe against
  `mem_ptr`'s COW/growsdown lock upgrades). On auth failure the output is
  scrubbed before returning.
- **Raw ChaCha20 stream op** (`dfd42c7a`) — `EVP_chacha20`-compatible
  (32-bit LE counter + 96-bit nonce IV), because OpenSSH's default
  `chacha20-