# Adding RISC-V vendor/user instructions to iSH-AOK

This document walks through the mechanism iSH-AOK uses to run non-standard
RISC-V instructions under the JIT — no interpreter, no forked engine, and no
risk to the ratified ISA the JIT already implements. It doubles as the
worked example for anyone who wants to add their own instruction, whether
that's a real silicon vendor's extension (T-Head, Andes, SiFive, and others
all ship real ones) or a one-off you're using to experiment.

The reference implementation lives in `jit/riscv64_vendor_ext.c`. Read this
document and that file side by side — if one changes, the other should.

## What the ratified ISA already covers

Check here before reaching for a vendor instruction: what you want may already
be standard. The riscv64 guest runs RV64GC plus the RVA23 extensions, all as
gadgets:

- **Scalar:** Zba, Zbb, Zbs, Zicond, Zcb, Zfa, Zfhmin, Zicbom/Zicbop/Zicboz
  (`cbo.zero` really zeroes its 64-byte block), Zimop/Zcmop, Zawrs, Zihintntl
  and Zihintpause.
- **Vector:** V, with Zvbb, Zvkb, Zvkt and Zvfhmin — and it is **advertised**,
  so software that picks a vector path at run time (OpenSSL's ChaCha20, for
  one) takes it.

A program finds them the way it would on Linux: `riscv_hwprobe` (syscall 258),
`AT_HWCAP` (`v` included), and the `isa` line of `/proc/cpuinfo`:

```sh
grep -m1 '^isa' /proc/cpuinfo    # rv64imafdcv_zicbom_..._zvkb_zvkt
```

`PR_RISCV_V_SET_CONTROL`/`PR_RISCV_V_GET_CONTROL` behave as in Linux's
`arch/riscv/kernel/vector.c`: V reads as on, a running program cannot turn it
off (`EPERM`), but it can set what its next `exec` gets and whether that is
inherited. A program exec'd with V off sees no `v` in `AT_HWCAP`, and every
vector instruction is illegal to it.

## Why this is safe: the opcode-space rule

The RISC-V ISA permanently reserves four major opcodes for non-standard
use:

| Name | Opcode (bits [6:0]) |
|---|---|
| custom-0 | `0x0B` |
| custom-1 | `0x2B` |
| custom-2 | `0x5B` |
| custom-3 | `0x7B` |

The specification guarantees no ratified standard extension will ever claim
these encodings. That single fact is what makes it safe to leave this hook
permanently wired into the decoder rather than gating it behind a special
build: every registered instruction is validated to sit entirely inside
that space (`riscv64_vendor_ext_register_is_valid` in the reference file),
so it can never shadow, alias, or race an instruction the JIT implements
for the standard ISA — today or after some future patch adds one. **The
JIT remains the only engine for the ratified ISA.** This hook only ever
fires on encodings nothing else claims, and only when explicitly enabled
(see below).

If you're implementing a real vendor's extension, you still don't need to
touch anything outside custom-0..3 — that's the whole point of the
reservation. If your instruction's real encoding lands outside that space,
it isn't a legitimate vendor extension by the ISA's own rules, and this
mechanism will correctly refuse to let you register it that way.

## How it fits into the JIT

The riscv64 JIT (`jit/gen.c`'s `gen_step_riscv64`) decodes one instruction
at a time and, on decode success, emits a small sequence of "gadgets" —
pointers to pre-compiled AArch64 host code — into the compiled block. When
it can't decode an instruction at all, it falls through to
`gen_riscv64_undefined`, which raises `INT_UNDEFINED` (the guest gets
`SIGILL`, exactly like on real hardware).

A vendor instruction is defined as **a ratified-ISA instruction under
another encoding**, and the hook is a rewrite right after the fetch:

```c
unsigned op7 = insn & 0x7f;
if (op7 == RISCV64_OP_CUSTOM0 || op7 == RISCV64_OP_CUSTOM1 ||
        op7 == RISCV64_OP_CUSTOM2 || op7 == RISCV64_OP_CUSTOM3) {
    uint32_t standard = riscv64_vendor_ext_enabled() ? riscv64_vendor_ext_translate(insn, NULL) : 0;
    if (standard != 0)
        insn = standard;
}
```

Three things happen when a custom-opcode instruction is fetched:

1. **Is the pack enabled?** Off by default — see "Enabling" below.
2. **Does the raw instruction word match a registered entry?** A tiny
   linear scan over a `{mask, match, mnemonic, standard, carry}` table.
3. **If both yes:** the word is replaced by `standard` — the ratified
   instruction the entry names — with the vendor word's `rd`/`rs1`/`rs2`
   filled in where `carry` says, and decoding carries on as if that had
   been fetched. The JIT compiles it with the same gadgets as the real
   one, so a vendor instruction runs at native gadget speed and with
   nothing in C at run time.

If nothing matches (or the pack is disabled), the word reaches the
custom-opcode case of the decoder unchanged, which is the exact same
`gen_riscv64_undefined` path any other unimplemented instruction takes.
**This hook only ever narrows what's legal — it never silently widens it.**

An instruction that is not some ratified instruction under another name
needs its own gadget, as every instruction this engine runs has: write it
in `jit/guest-riscv64/` and emit it from the custom-opcode case instead.

## The example pack

Four demonstration instructions ship in `jit/riscv64_vendor_ext.c`, all
under custom-0 (`0x0B`), differentiated by `funct3`:

| Mnemonic | funct3 | Semantics |
|---|---|---|
| `ish.clz`   | 0 | `rd = rs1 == 0 ? 64 : count_leading_zeros(rs1)` |
| `ish.ctz`   | 1 | `rd = rs1 == 0 ? 64 : count_trailing_zeros(rs1)` |
| `ish.pcnt`  | 2 | `rd = popcount(rs1)` |
| `ish.bswap` | 3 | `rd = byteswap64(rs1)` |

**These are an iSH-AOK-invented demonstration, not a transcription of any
real vendor's silicon.** T-Head, Andes, SiFive, and others all ship real
custom-0..3 extensions, but this project has no way to verify a
hand-transcribed encoding against actual hardware, so it makes no claim of
bit-compatibility with any of them. They are `Zbb`'s `clz`, `ctz`, `cpop`
and `rev8` under other encodings — Zbb itself runs directly too — which is
what a vendor instruction mapped onto the ratified ISA looks like. Swap in a real, verified encoding here if
you're targeting actual hardware; the mechanism doesn't care what bit
pattern you choose, only that it lives in the reserved space.

Each instruction is encoded R-type-shaped (`rd`, `funct3`, `rs1`, `funct7`,
`rs2`, `opcode`), with `funct7` pinned to `0` and `rs2`/the rest of the
encoding unused. Pinning `funct7` is a narrowing choice, not a requirement:
it claims as little of the custom-0 space as this pack actually needs,
leaving the rest of the `funct7` range free for some *other* pack sharing
the same opcode. When you add your own instructions, claim only what you
use.

## Writing an entry

An entry names the vendor encoding and the ratified instruction it equals:

```c
{ RISCV64_VENDOR_MASK, RISCV64_VENDOR_MATCH(0), "ish.clz", ZBB_UNARY(0x600, 1), CARRY_RD | CARRY_RS1 },
```

`standard` is the ratified encoding with its register fields zero
(`ZBB_UNARY(0x600, 1)` is `clz x0, x0`), and `carry` says which of the
vendor word's `rd` (bits 11:7), `rs1` (19:15) and `rs2` (24:20) are copied
into it, at the same positions. Everything the ratified instruction does —
`rd == x0` discarding the result, the PC advancing past it — it does here
too, because it is that instruction by the time the JIT sees it.

Pin the opcode field (bits `[6:0]`) of every `{mask, match}` pair to one of
the four custom opcodes. `RISCV64_VENDOR_MASK`/`RISCV64_VENDOR_MATCH` in the
reference file are the mask/match pattern for "opcode + funct3, funct7
forced to 0"; adjust if your instruction needs to look at different bits.

## Enabling

The pack is **off by default**. A vendor extension changes what encodings
are legal to execute — that's a deliberate opt-in, not ambient behavior,
the same way real hardware needs a `mstatus`/misa or vendor-specific CSR
bit set before custom instructions are legal to issue.

```sh
ISH_RISCV64_VENDOR_EXT=1 ish -r / your-binary
```

Any value turns it on — the check is only whether the variable is set, so
`ISH_RISCV64_VENDOR_EXT=0` enables it too; unset it to turn the pack off.

This is checked once per process (a cached `getenv`, the same idiom used
throughout this codebase — see e.g. `jit/jit.c`'s `ISH_AMD64_CC1_TRACE`
handling) — a **process-lifetime** toggle, not a hot runtime one. That's
sufficient for a built-in pack like this: the table is a static C array,
compiled once, and every process either has the feature for its whole
lifetime or doesn't.

A more ambitious tier — a CLI-loadable plugin registering entries into a
*running* emulator, or toggling the pack on a live process — would need
one more piece this reference implementation doesn't build:  **JIT
block-cache invalidation on registration change.** Any block already
compiled before the toggle flipped has the old decision (undefined vs.
vendor-gadget) baked into its code stream; a hot toggle needs to flush
every cached block so nothing keeps running the stale decision. Nothing
here does that today — it's flagged as the next real step if this ever
grows past a build-time pack, not a subtle bug in what exists.

## Testing

`tests/riscv64/riscv64_vendor_ext.s` is a hand-assembled smoke test.
Standard RISC-V assemblers don't know custom mnemonics, so it encodes each
instruction as a raw `.word`:

```asm
li a1, 0xf
slli a1, a1, 32
.word 0x0005850b      # ish.clz a0, a1
```

To compute an encoding by hand for an R-type-shaped instruction:

```
insn = (funct7 << 25) | (rs2 << 20) | (rs1 << 15) | (funct3 << 12) | (rd << 7) | opcode
```

Run it both ways to confirm the opt-in gate itself, not just the
arithmetic:

```sh
ish -r / riscv64_vendor_ext          # expect SIGILL (exit 132): disabled by default
ISH_RISCV64_VENDOR_EXT=1 ish -r / riscv64_vendor_ext   # expect exit 0
```

The test also checks the `rd == x0` case explicitly (`ish.clz x0, a1` must
be a no-op) — easy to get wrong in a new entry's `carry` and easy to verify
mechanically.

## Generalizing beyond riscv64

The design here — a decode-miss registry consulted before the
undefined-instruction path, gated by a per-arch opcode-space rule, that
rewrites a vendor word into the ratified instruction it equals — isn't
riscv64-specific in spirit.
arm64 has architecturally unallocated encodings that could play the same
role as RISC-V's custom-0..3 opcodes (with a correspondingly stricter,
hand-curated allow-list, since arm64 doesn't reserve a clean opcode field
the ISA promises to leave alone), and x86 has encodings that currently
`#UD`. Neither is implemented as of this writing; this file and this
document are the concrete pattern to follow if that changes.
