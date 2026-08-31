# 8. High-level emulation: skipping the instructions entirely

A guest running `sort` on a 124 MB file is not mostly doing comparisons. It is
mostly inside `memcpy`, `memcmp` and `strlen` — functions whose semantics the
host already implements, in hand-tuned assembly, and which the emulator is
faithfully translating one instruction at a time.

High-level emulation is the observation that translating those functions is
unnecessary. If a block of guest code *is* `memcpy`, the emulator does not have
to execute `memcpy`; it has to produce `memcpy`'s effect. That is one host call
instead of a few hundred dispatches.

The idea is old — full-system emulators have long intercepted BIOS and firmware
calls this way — but applying it to libc inside a user-mode emulator has a
specific problem: you have to know that the block is `memcpy`, and the guest is
not going to tell you.

## 8.1 Three ways to recognize a function

AOK uses three recognizers, tried in that order, and they get progressively more
general.

### Fingerprints

The first is an exact 64-byte prologue match. `jit/hle-table.inc` holds 64
fingerprints covering 22 functions across three libcs — glibc 2.41 on arm64,
musl on arm64, musl on riscv64:

```
memcpy memmove memset memcmp memchr memrchr strlen strnlen strcmp strncmp
strchr strrchr rawmemchr strspn strcspn strpbrk strcpy stpcpy strncpy
stpncpy strcat strncat
```

Each entry is the literal first 64 bytes of the function, hashed at translation
time against the block's start address.

The interesting part is how the table is generated. `tools/hle_fingerprint_guest.c`
runs **inside the guest**, and uses `dlsym`:

> `dlsym()` returns the RESOLVED implementation address — for glibc's ifunc
> symbols (`memcpy` & friends on aarch64) that is the per-hwcap variant the
> dynamic linker actually selected under iSH's advertised `AT_HWCAP`, i.e.
> exactly the entry point block translation will see when guest code calls
> through the PLT.

Fingerprinting the symbol as it appears in the `.so` on disk would fingerprint
the *resolver*, and the resolver is never what runs. The tool is simpler than
offline ELF parsing and more faithful, and it only works because AOK can run
guest code — the emulator is used to build a table for the emulator.

### The symbol table

A fingerprint is exact, which means it is also brittle: a distribution updates
its libc, the prologue changes by one instruction, and the entry stops matching.

So on a miss, if the block start lies inside a file-backed mapping whose name
looks like a libc — `libc.so.6`, `libc-*.so`, `ld-musl-*.so.1`,
`libc.musl-*.so.1` — the ELF's dynamic symbol table is parsed through the
mapping's retained `struct fd`, and the address is matched against the known
function names directly.

Two details in that path are worth naming, because both are the difference
between a cache and a bug. Parsed results are keyed by a content hash of the
ELF header block, so identical libc files parse once. And the cache stores
**file-relative offsets, never guest addresses**; the load bias is recomputed
from the live page tables on every lookup, so a remap cannot leave a stale entry
pointing at somebody else's memory.

One exclusion: glibc's `STT_GNU_IFUNC` string functions are deliberately
skipped here, because their `st_value` is the resolver rather than the
implementation. Those stay covered by the fingerprints, which were taken through
`dlsym` for exactly that reason.

### Loop shapes

Both of the above can only catch a *call*. An open-coded copy loop — an inlined
`memcpy` at `-O0` or `-O1`, LZMA's match copy, somebody's hand-rolled byte loop —
has no entry point to attach a name to.

But such a loop is still a block. The backward conditional branch at the bottom
makes its head a block start, so the same translation hook can look at the whole
block and ask whether it *is* a bulk operation. The recognizer accepts a small
set of shapes, all post-index, forward, unit stride, with the trailing branch
targeting the block start exactly:

```
copy: LDR{B,H,W,X} Rt,[Rs],#k ; STR Rt,[Rd],#k ; <counter> ; <branch>
fill:                          STR Rt,[Rd],#k ; <counter> ; <branch>
counter/branch: SUBS Rc,Rc,#1 + B.NE | SUB Rc,Rc,#1 + CBNZ Rc
              | CMP Ra,Re + B.NE
```

What makes this legitimate rather than reckless is that the executor reproduces
the loop's exact architectural contract, not just its memory effect: the address
registers advance per iteration, `Rt` holds the last element loaded, NZCV
reflects the last executed `SUBS`/`CMP`, and a fault leaves state at the precise
iteration boundary — or mid-iteration for a store fault, with pc on the store —
so ordinary translation can take over from there. Work is chunked, so no single
host call runs unbounded and an unfinished chunk re-enters the same block with
pc at the loop head.

There is even a fidelity detail for overlapping forward copies: LZ77 match
copies with distance less than length depend on byte-forward propagation —
the periodic tiling of the leading bytes — and the executor reproduces that
rather than doing a `memmove`, because the loop the guest wrote does not do a
`memmove`.

## 8.2 What the block becomes

A recognized block is not translated instruction by instruction. It becomes what
the source calls a "giant gadget": one bridge into a host C implementation that
performs the function's entire contract against guest memory, writes the ABI
return register, sets the guest pc to the return address, and exits the block.

The encoding is worth looking at, because it shows how little state a gadget
gets to carry. `hle_emit` appends a spec word, the guest ip, and the prologue
length into the ordinary code array (Chapter 6) via `gen_raw`. The spec word's
low byte is the function id, and bit 8 says which guest ABI's registers to read:

```c
hle_emit_word(state, (unsigned long) fn | (riscv64 ? 0x100ul : 0), ip,
        HLE_PROLOGUE_LEN, riscv64);
```

That bit exists for a reason worth noting in passing — `struct cpu_state` has no
ABI field. The ABI lives on `struct task` (Chapter 7), and a gadget has `_cpu`,
not `current`, so the translator has to fold the answer into the instruction
stream at translation time.

The loop recognizer packs considerably more into one word, since a recognized
loop has no name to look up:

```
bit  8       riscv64 (always 0: arm64-only recognizer for now)
bits 12-16   Rt  (data register; 31 = WZR, meaning fill)
bits 17-21   Rs  (source address register; 31 = none, meaning fill)
bits 22-26   Rd  (destination address register)
bits 27-31   Rc  (count register, or end register for CMP loops)
bits 32-35   log2 of the element size
bit  36      fill (store-only loop)
bits 37-38   counter kind: SUB+CBNZ, SUBS+B.NE, or CMP+B.NE
bit  39      CMP compares the source register (else the destination)
bits 40-47   loop length in bytes
```

One 64-bit word describes the entire loop the executor must reproduce. This is
what "the array is data" buys: a translation-time decision can be arbitrarily
rich, as long as it fits in words the gadget can read back.

## 8.3 The correctness posture

HLE is the most obviously dangerous optimization in the tree, and the reason it
is not is a single design commitment, stated at the top of `jit/hle.c`: **the
ABI boundary is the contract.**

Only architectural state at function entry and exit has to be right.
Caller-saved registers are legally dead across a call, so they are simply not
touched — the values they held at entry persist, which is *a valid possible
execution* of the function. Nothing conforming can tell.

The rest follows:

- **An unknown libc never matches.** Execution falls through to ordinary
  translation. HLE is a pure fast path and plain emulation is always the correct
  fallback, so a guest upgrading its libc loses speed and never correctness.
- **`memcpy` is implemented with `memmove` semantics.** For overlapping buffers
  the guest program's behavior is undefined anyway; a deterministic superset is
  the conservative choice.
- **`memcmp` returns the difference of the first differing bytes** — exactly
  musl's behavior, and sign-compatible with any conforming caller of glibc's.
- **A fault mid-operation delivers a guest `SIGSEGV` with pc rewound to the
  function entry**, as if the first instruction had faulted. Partial writes may
  already have happened, which is also what real hardware does when it faults in
  the middle of a `memcpy`.

That list is the shape of every safe fast path in this system: define the
contract narrowly, make the fallback the correct implementation, and enumerate
the observable differences rather than hoping there are none.

## 8.4 The load-bearing fix, and the lesson in it

The first implementation was **about twice as slow as plain emulation**.

It bounced data through a 256-byte stack buffer using `tlb_read` and
`tlb_write` — the same primitives ordinary gadgets use. That is the obvious way
to write it, and it is obviously wrong once stated plainly: the JIT's
*translated* `memcpy` was already writing host memory directly, with NEON,
because the guest's own `memcpy` is NEON code and the gadgets that implement its
loads and stores resolve host pointers. Replacing that with a C loop over a
256-byte staging buffer replaced good code with worse code and added a layer.

The rewrite resolves a direct host pointer per guest page and runs one native
`memcpy`/`memset`/`memcmp`/`memchr` per in-page span. That is where the numbers
come from.

The generalizable lesson — and Chapter 38 leans on it — is that "native code
must beat translated code" is an assumption, not a fact. The translated code
was the output of a good translator over hand-tuned assembly. Beating it
required being native *and* doing less work, not merely being native.

## 8.5 The numbers, and the honest version of them

Microbenchmark first — a `memcpy`/`memset`/`memcmp`/`strlen` loop on an arm64
guest, best of three, HLE on versus off in the same build:

| buffer | speedup |
|---|--:|
| 256 B | 1.23x |
| 4 KB | 3.16x |
| 64 KB | **7.17x** |
| 1 MB | 6.68x |

riscv64 shows a consistent ~2.0x across the same sizes.

Then the part that a less careful project would leave out — a broad sweep over
staged 124 MB files, arm64, best of three:

| workload | off | on | delta |
|---|--:|--:|--:|
| `sort` (4M records) | 8.35 s | 6.49 s | **+22%** |
| `base64` | 2.37 s | 2.25 s | +5% |
| `wc` | 1.72 s | 1.67 s | +3% |
| `sort \| uniq` | 8.14 s | 7.88 s | +3% |
| `md5`, `tr`, `cut`, `sha256` | — | — | neutral |

HLE meaningfully helps data-movement-heavy work and is neutral where a
program's own arithmetic dominates. It does not help `ssh` or `scp` at all,
because those are crypto-bound — which is what the crypto accelerator in
Chapter 33 exists for.

A 7.17x microbenchmark and a 3% `wc` are both true, and quoting only the first
would be the kind of number that makes a reader distrust every other number in a
book.

Validation was differential: bit-identical runs on all three images, dedicated
edge-case tests for both libcs, and the full guest regression suite passing
under `ISH_HLE=1`.

## 8.6 Where it does not apply

HLE is gated to the arm64 and riscv64 guests. `ISH_HLE=1` on an i386 or amd64
guest silently does nothing.

The reason is that those are the guests whose remaining cost is dominated by
dispatch rather than by the semantic work inside each gadget (Chapter 7). On
x86 guests there is more to gain from fusion and from the translator itself, and
the fingerprints would have to be maintained for a different instruction set
besides.

The word doing the most work in that paragraph is *silently*. A flag that is
accepted and ignored is a small instance of the failure mode Chapter 40 calls a
capability lie, and the honest description of the current state is that the
gating is right and the silence is a wart.

## 8.7 Tooling, and one required release step

Because a fingerprint miss is invisible — the guest gets the right answer,
slowly — HLE ships with instrumentation that makes the invisible countable:

- `ISH_HLE_STATS=1` prints per-function call counts and a size histogram at
  exit. This is how you find out whether the thing you fingerprinted is
  actually hot.
- `ISH_HLE_TRACE=1` logs every attach, plus a near-miss tracer that ranks
  recurring *unmatched* function prologues as candidates for the next
  fingerprint. The symbol-table path is what lets it print candidate names
  rather than raw hashes.
- `ISH_HLE_FP=0`, `ISH_HLE_SYMTAB=0` and `ISH_HLE_LOOPS=0` disable one
  recognizer each, so a suspect attach can be isolated to the path that made it.

And one operational rule, which belongs in Chapter 37's release checklist as
much as here: if the bundled rootfs libcs change, the fingerprint table must be
regenerated. The consequence of forgetting is stated in the perf document in the
form every such note should take —

> a stale table costs speed, never correctness.

That sentence is the entire design in six words, and it is the reason this
optimization is allowed to exist in a system whose first commitment is fidelity.

---

*Anchors:* [jit/hle.c](../../jit/hle.c), [jit/hle-table.inc](../../jit/hle-table.inc),
[jit/gen.h](../../jit/gen.h) (`hle_try_emit`), [jit/jit.c](../../jit/jit.c),
[tools/hle_fingerprint_guest.c](../../tools/hle_fingerprint_guest.c),
[docs/performance-optimizations-2026-07.md](../../docs/performance-optimizations-2026-07.md).

*Story:* the first HLE implementation was 2x slower than plain emulation,
because it staged through a 256-byte buffer while the translated code it
replaced was already writing host memory directly with NEON.
