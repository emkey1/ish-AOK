# Adding RISC-V vendor/user instructions to iSH-AOK

This document walks through the mechanism iSH-AOK uses to run non-standard
RISC-V instructions under the JIT — no interpreter, no forked engine, and no
risk to the ratified ISA the JIT already implements. It doubles as the
worked example for anyone who wants to add their own instruction, whether
that's a real silicon vendor's extension (T-Head, Andes, SiFive, and others
all ship real ones) or a one-off you're using to experiment.

The reference implementation lives in `jit/riscv64_vendor_ext.c`. Read this
document and that file side by side — if one changes, the other should.

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

The vendor hook adds one case to that decoder, ahead of the
undefined-instruction fallback:

```c
case RISCV64_OP_CUSTOM0: case RISCV64_OP_CUSTOM1:
case RISCV64_OP_CUSTOM2: case RISCV64_OP_CUSTOM3: {
    if (riscv64_vendor_ext_enabled() && riscv64_vendor_ext_lookup(insn) != NULL) {
        gen(state, (unsigned long) gadget_riscv64_call_helper);
        gen(state, (unsigned long) riscv64_vendor_ext_dispatch);
        gen(state, (unsigned long) insn);
        return 1;
    }
    return gen_riscv64_undefined(state, insn);
}
```

Three things happen when a custom-opcode instruction is decoded:

1. **Is the pack enabled?** Off by default — see "Enabling" below.
2. **Does the raw instruction word match a registered entry?** A tiny
   linear scan over a `{mask, match, mnemonic, handler}` table.
3. **If both yes:** emit one gadget — `call_helper`, already present in
   `jit/guest-riscv64/fp.S` for the CSR and `fclass` implementations —
   that saves the gadget register file, calls a plain C function with
   `(cpu_state *, unsigned long arg)`, and restores. The `arg` here is
   just the raw 32-bit instruction word; the C handler re-extracts
   `rd`/`rs1`/whatever fields it needs using the same `emu/arch/riscv64/
   decode.h` helpers the rest of the decoder uses.

No new gadget assembly was needed for this. That's deliberate: `call_helper`
already solves "run some C code with the right cpu_state pointer and
without corrupting the JIT's own register conventions" — see its header
comment in `fp.S` for the AAPCS64 aliasing hazard it works around (`_cpu`
IS x1; read it before you overwrite x1 with your own argument). Any new
vendor instruction that can be expressed as C reading/writing `cpu_state`
fields rides this same gadget for free.

If decode fails (wrong mask, or the pack is disabled), execution falls
through to the exact same `gen_riscv64_undefined` path any other
unimplemented instruction takes. **This hook only ever narrows what's
legal — it never silently widens it.**

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
bit-compatibility with any of them. The four instructions above do fill a
real gap, though: this port doesn't implement the standard `Zbb`
bit-manipulation extension, so `clz`/`ctz`/`popcount`/byte-swap have no
other way to execute under this engine today — a legitimate (if invented)
motivation for a vendor extension to exist. Swap in a real, verified
encoding here if you're targeting actual hardware; the mechanism doesn't
care what bit pattern you choose, only that it lives in the reserved
space.

Each instruction is encoded R-type-shaped (`rd`, `funct3`, `rs1`, `funct7`,
`rs2`, `opcode`), with `funct7` pinned to `0` and `rs2`/the rest of the
encoding unused. Pinning `funct7` is a narrowing choice, not a requirement:
it claims as little of the custom-0 space as this pack actually needs,
leaving the rest of the `funct7` range free for some *other* pack sharing
the same opcode. When you add your own instructions, claim only what you
use.

## Writing a handler

A handler is a plain C function:

```c
static void riscv64_vext_clz(struct cpu_state *cpu, uint32_t insn) {
    unsigned rd = riscv64_rd(insn), rs1 = riscv64_rs1(insn);
    uint64_t v = cpu->riscv64_regs[rs1];
    if (rd != 0)
        cpu->riscv64_regs[rd] = v == 0 ? 64 : (uint64_t) __builtin_clzll(v);
}
```

Two invariants every handler must respect (both already enforced
everywhere else in the riscv64 engine — see `emu/cpu.h`'s riscv64 register
block comment):

- **Never write `cpu->riscv64_regs[0]`.** `x0` is hardwired zero. Guard
  every register write with `if (rd != 0)`. Reads don't need a guard:
  `riscv64_regs[0]` is never written by anything else either, so it's
  always zero to read.
- **Don't touch `cpu->riscv64_pc`.** The gadget stream already advances
  `pc` past your instruction before and after the callback runs (it's a
  gadget just like any other, `gret`-terminated); a handler that also
  moves `pc` will corrupt control flow. If you need a *branching* vendor
  instruction, that's a materially different design (you'd need the
  handler to signal a target back to the gadget rather than exiting
  normally) — out of scope for this pack, flagged here so you don't
  discover it the hard way.

Register the handler in the static table with a `{mask, match}` pair that
pins the opcode field (bits `[6:0]`) to one of the four custom opcodes.
`RISCV64_VENDOR_MASK`/`RISCV64_VENDOR_MATCH` in the reference file are the
mask/match pattern for "opcode + funct3, funct7 forced to 0"; adjust if
your instruction needs to look at different bits.

## Enabling

The pack is **off by default**. A vendor extension changes what encodings
are legal to execute — that's a deliberate opt-in, not ambient behavior,
the same way real hardware needs a `mstatus`/misa or vendor-specific CSR
bit set before custom instructions are legal to issue.

```sh
ISH_RISCV64_VENDOR_EXT=1 ish -r / your-binary
```

This is checked once per process (a cached `getenv`, the same idiom used
throughout this codebase — see e.g. `jit/jit.c`'s `ISH_AMD64_CC1_TRACE`
handling) — a **process-lifetime** toggle, not a hot runtime one. That's
sufficient for a built-in pack like this: the table is a static C array,
compiled once, and every process either has the feature for its whole
lifetime or doesn't.

A more ambitious tier — a CLI-loadable plugin registering handlers into a
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
be a no-op) — the one invariant that's easy to get wrong in a new handler
and easy to verify mechanically.

## Generalizing beyond riscv64

The design here — a decode-miss registry consulted before the
undefined-instruction path, gated by a per-arch opcode-space rule, riding
a generic "call a C handler" gadget — isn't riscv64-specific in spirit.
arm64 has architecturally unallocated encodings that could play the same
role as RISC-V's custom-0..3 opcodes (with a correspondingly stricter,
hand-curated allow-list, since arm64 doesn't reserve a clean opcode field
the ISA promises to leave alone), and x86 has encodings that currently
`#UD`. Neither is implemented as of this writing; this file and this
document are the concrete pattern to follow if that changes.
