# POWER9 (Power ISA 3.0) on the iSH-AOK ppc64le guest — a scope

Date: 2026-11-13. **Investigation only: nothing here is implemented, and no
decision is assumed.** It answers what adding ISA 3.0 to the ppc64le guest
(`docs/ppc64le_guest_plan.md`, landed in `bcefba166`) would actually involve,
with measurements rather than a reading of the manual's chapter list.

Headline: **two to three working weeks gets a POWER9-advertised userland that
works, and the hard part — IEEE-128 quad precision — turns out to be
separable, because glibc gates it on its own HWCAP2 bit.** Without that
separation the number would be four to six weeks. The single biggest obstacle
is quad precision: the aarch64 host has no binary128 type at all, not even in
the host compiler, so all five arithmetic kernels would be written from
scratch in gadget assembly.

## 1. What the guest is today, and who decides it

The guest is a **POWER8 (Power ISA 2.07), 64-bit little-endian, ELFv2**. Five
places say so, and they have to move together:

| What | Where | Says |
|---|---|---|
| `AT_PLATFORM` | `kernel/abi.h:67` | `power8` |
| `AT_HWCAP` / `AT_HWCAP2` | `kernel/exec.c:1504` | 32\|64\|ALTIVEC\|FPU\|MMU\|ARCH_2_05\|ARCH_2_06\|VSX\|TRUE_LE; HWCAP2 ARCH_2_07\|ISEL |
| `/proc/cpuinfo` | `fs/proc/root.c:245` | `cpu : POWER8 (raw), altivec supported`, `revision : 2.1 (pvr 004d 0201)` |
| the decoder | `jit/gen_ppc64.c` | 481 gadgets; anything else is a printk and `INT_UNDEFINED` |
| the documented claim | `opt/AOK/docs/guest-cpus.md:109` | "the ppc64le guest is a **POWER8**" |

`AT_BASE_PLATFORM` is not written at all (grep: no `BASE_PLATFORM` anywhere in
the tree), although `docs/ppc64le_guest_plan.md` lists it. glibc's `ld.so`
uses it for its platform subdirectory search, so a POWER9 identity wants it
added rather than only changed.

`opt/AOK/docs/guest-cpus.md` states the project's rule for all of this: *"a
feature is reported only when every instruction behind it runs, as gadgets in
the JIT, and the guest suite runs an instruction for every bit it sets."*
Everything below is organised around that rule — the advertisement is the last
patch, not the first.

### Six ISA 3.0 instructions are already in

`docs/TODO.md` says "nothing of ISA 3.0". That is very nearly right but not
quite: `modsw`, `moduw`, `modsd`, `modud` (`jit/gen_ppc64.c:366`) and
`cnttzw`, `cnttzd` (`jit/gen_ppc64.c:386`) are ISA 3.0 instructions with
gadgets today — binutils refuses all six at `-mpower8` and accepts them at
`-mpower9`. They came along with the XO-form arm they share. Nothing else of
ISA 3.0 decodes, and three whole major opcodes are absent from
`gen_step_ppc64_insn`: **56** (`lq`), **57** (`lfdp`/`lxsd`/`lxssp`) and **61**
(`stfdp`/`lxv`/`stxv`/`stxsd`/`stxssp`), which is where much of ISA 3.0's new
load/store encoding lives.

### What the distributions actually require

Measured on camd's Debian 13 (trixie) `powerpc64le-linux-gnu` toolchain and
its ppc64el glibc, which is a real distro build of the real libc:

- **Debian ppc64el is built for POWER8.** `gcc -Q --help=target` shows
  `-mpower8-vector` and `-mpower8-internal` enabled, `-mpower9-misc` and
  `-mpower9-minmax` disabled, with no `-mcpu` default override.
- **Its `long double` is IBM double-double**, not IEEE binary128:
  `__LDBL_MANT_DIG__` is 106. So ordinary `long double` code needs no
  quad-precision instruction.
- **Its glibc nevertheless contains 43 distinct ISA 3.0 instructions** —
  and every one of them is in an ifunc variant selected at run time. Across
  `libc.a` and `libm.a`, 468 object members hold ISA 3.0 code and essentially
  all are named `*-power9.o` or `*-power10.o`
  (`strcpy-power9.o`, `e_j1f128-power9.o`, `memcmp-power10.o`, …). Two
  exceptions are inline-with-a-runtime-branch rather than unconditional:
  `scv` (411 sites, in `pthread_create.o`, `nptl_setxid.o`, `pthread_kill.o`,
  behind a HWCAP2_SCV test) and `mffsl` (18 sites, in `strtod_l.o` and
  friends). The one remaining oddity is not code at all — see §2's caveat.

That is why a POWER8-advertised AOK guest runs Debian ppc64el and Alpine
ppc64le today even though their libc is full of instructions it cannot decode:
**the JIT translates lazily, so an untaken ifunc branch is never translated,
and an instruction that is never translated costs nothing.** The flip side is
the important part for this scope — see §3's "cliff".

### Which bit switches what (the decisive measurement)

Disassembling the resolvers in Debian's own ppc64el glibc:

- `strlen`'s resolver tests `andis. r10,r10,128`, i.e. HWCAP2
  **0x00800000 = ARCH_3_00**, to pick `__strlen_power9`.
- `acosf128` / `__acosieee128`'s resolver tests `andis. r9,r9,64`, i.e. HWCAP2
  **0x00400000 = HAS_IEEE128**, to pick the quad-precision variant.

**ARCH_3_00 and HAS_IEEE128 are independent.** A guest may advertise
ARCH_3_00 — and get the power9 string, memory and fenv paths — while leaving
HAS_IEEE128 (and DARN, 0x00200000, and SCV, 0x00100000) clear, and glibc keeps
its soft-fp `float128` route. qemu agrees that these are separate knobs:
`qemu-ppc64le -cpu power9` reports hwcap2 `0x8ee00000` against power8's
`0x8e000000`, the difference being exactly ARCH_3_00 | HAS_IEEE128 | DARN.

### Alpine 3.24.2 ppc64le — an open question

**I could not settle what the bundled Alpine root was built for from the
repository or from any machine here.** There is no Alpine ppc64le root or
tarball on this Mac, on camd, on m4 or on mbp5 (searched for
`*ppc64le*`/`*ppc64el*`), and fetching one is a download, which needs your
yes. What the repository does support:

- `docs/ppc64le_guest_plan.md` records the port's own histogram of musl +
  busybox as 263 mnemonics "mostly integer", naming `lxvd2x`/`stxvd2x`,
  `mtfprd`/`mffprd`, `xscvdpspn`, `lwa`, `ldbrx` — all ISA 2.07 or older.
- The behavioural argument is strong on its own: every undecodable opcode
  prints `WARNING: ppc64 JIT: no gadget for insn` and raises `INT_UNDEFINED`
  (`jit/gen_ppc64.c:145`), and apk-over-HTTPS, gcc in the guest, Python,
  strace and a 362-test manual-suite pass all ran on that root. Whatever Alpine 3.24.2 ppc64le executes, it is inside the POWER8 set
  plus those six instructions.
- musl has no ifunc dispatch for its string functions, so unlike glibc it
  carries no dormant POWER9 code at all.

To settle it properly: unpack `alpine-minirootfs-3.24.2-ppc64le.tar.gz` and
run the §2 scan over its `lib/ld-musl-powerpc64le.so.1` and `bin/busybox`.

## 2. The inventory, with evidence

Three probes, all on camd; the scripts and raw output are in this session's
notes (`power9-scope-evidence.md`).

1. **What is ISA 3.0 at all** — assemble each candidate mnemonic at
   `-mpower8` and `-mpower9`: **181 mnemonics are ISA 3.0-only**. Two
   corrections this turned up: `xsrsp` is ISA 2.07 (already the `frsp`
   gadget), and `lxvx`/`stxvx` are dual-meaning mnemonics — at `-mpower8`
   they assemble to `lxvd2x`/`stxvd2x`, at `-mpower9` to a genuinely new
   X-form encoding, so a disassembly that *prints* `lxvx` is the new one.
2. **What real binaries hold** — disassemble 49 Debian ppc64el glibc files
   (4.6M instructions) and intersect.
3. **What a POWER9-targeted build emits** — compile the same 233-file C corpus
   (`tests/manual/*.c`) at `-O2 -mcpu=power8` and `-O2 -mcpu=power9` and diff
   the mnemonic sets. This is the stand-in for RHEL 9 / Ubuntu 24.04+, which
   are built that way.

A caveat on method, because it bit me: `objdump -d` disassembles data in
`.text` too, and the corpus holds 129,373 words it printed as `.long`
(jump tables, alignment). Small jump-table offsets can decode as plausible
instructions — `libc.a(wordexp.o)`'s 40 apparent `xsnmsubqp` are a jump table
after a `bctr`, not code. What makes the finding safe is the attribution: a
hit inside a `*-power9.o` member is real, a hit in a member with no business
holding quad arithmetic deserves a look at its context first.

**56 of the 181 are observed; 125 are not.** Counts (glibc sites / power9-build
sites):

| Group | Observed members | Verdict |
|---|---|---|
| New VSX load/store | `lxv` 9312/199, `stxv` 1588/845, `lxvb16x` 0/2122, `stxvx` 0/100, `lxvx` 87/6, `lxsd` 10/2 | **needed** |
| Length-indexed load/store | `lxvl` 30/0, `stxvl` 138/0 | **needed** (power9 `strcpy`/`stpcpy`/`strncpy`, power10 `memcmp`/`memmove`) |
| VMX 3.0 integer | `vcmpnezb` 36/1061, `vextublx` 0/532, `vclzlsbb` 0/266, `vctzlsbb` 184/0, `vextubrx` 40/0, `vextsb2d` 0/40, `vextsw2d` 30/0, `vcmpneb` 32/0, `vcmpnew` 0/2, `vextractuh` 0/2 | **needed** |
| VSX 3.0 misc | `xxspltib` 559/205, `mtvsrdd` 390/3, `mfvsrld` 199/0, `mtvsrws` 0/48 | **needed** |
| Integer | `extswsli` 30/38, `maddld` 0/33, `setb` 0/1 | **needed** |
| FPSCR | `mffsl` 44/0, `mffscrn` 51/0, `mffscrni` 21/0 | **needed** (glibc fenv, runtime-gated) |
| Scalar DP compares | `xscmpgtdp` 0/2, `xscmpgedp` 0/1, `xsmincdp` 0/1 | **needed** |
| Quad precision | 23 of the `*qp` set, 0 in the power9 build: `xsmaddqp` 3683, `xscmpuqp` 2705, `xsmulqp` 1867, `xsaddqp` 1170, `xssubqp` 1064, `xsdivqp` 958, `xsabsqp` 796, `xscpsgnqp` 374, `xsnegqp` 318, `xssqrtqp` 135, `xsmsubqp` 140, `xststdcqp` 114, `xsrqpi` 48, `xsxexpqp` 6 and the nine `xscvqp*`/`xscv*qp` converters | **needed only with HAS_IEEE128** |
| `darn` | 8/0 (libstdc++'s `std::random_device`) | **needed only with DARN** |
| `scv` | 818/0 | **needed only with SCV** |

The 125 unobserved mnemonics fall into two different kinds, and conflating
them would be a mistake:

- **Siblings of observed ops** (`lxvh8x`/`stxvh8x`/`stxvb16x`, `lxssp`/
  `stxsd`/`stxssp`, `lxvwsx`, `lxsibzx` and the other scalar byte/half forms,
  `lxvll`/`stxvll`, the rest of `vcmpne*`/`vcmpnez*`, `vextract*`/`vinsert*`,
  `vext[hw][lr]x`, `vexts*2*`, `vctz[bhwd]`, `vnegw/d`, `vprtyb*`,
  `vrl[dw]mi/nm`, `vslv`/`vsrv`, `vmsumudm`, `vpermr`, `xxbr[hwq]`,
  `xxextractuw`/`xxinsertw`, `xxperm`/`xxpermr`, the `xviexp`/`xvxexp`/
  `xvxsig`/`xvtstdc` family, the half-precision converters, `addpcis`,
  `cmpeqb`, `mcrxrx`, `addex`, `maddhd(u)`, `mffsce`, `mffscdrn(i)`). These
  are unobserved only because one libc and one corpus is a narrow sample.
  They are each one or two instructions' work once the decode arm exists, and
  implementing a family whole is cheaper than returning to it when the next
  distribution picks a different member. **Implement with the family.**
- **Families with no observed member and a reason** — these are the real
  `#UD` candidates:
  - **BCD / packed decimal** (`bcdcfn`, `bcdctn`, `bcdcfz`, `bcdctz`,
    `bcdcfsq`, `bcdctsq`, `bcdcpsgn`, `bcds`, `bcdsr`, `bcdtrunc`, `bcdus`,
    `bcdutrunc` — and ISA 2.07's `bcdadd`/`bcdsub`, also missing today): no
    occurrence anywhere in 4.6M instructions of glibc, no compiler emits them,
    no Linux userland path. **#UD.**
  - **Atomic memory operations** (`lwat`, `ldat`, `stwat`, `stdat`): zero
    occurrences; glibc's atomics are `larx`/`stcx.`, which already work.
    **#UD.**
  - **Copy-paste** (`copy`, `paste.`, `cp_abort`): zero occurrences; the
    accelerator path (libnxz) only, and there is no accelerator here. **#UD.**
  - **Privileged / supervisor** (`slbieg`, `slbsync`, `msgsync`, `stop`,
    `wait`, `rfscv`, the privileged SPRs): a user-mode guest cannot reach
    them. **#UD**, as today.
  - **`ldmx`**: removed from the architecture before any hardware. **#UD.**
  - **Vector crypto and `vpmsum*`** are not ISA 3.0 at all (2.07) and are
    already on the TODO list; `AT_HWCAP2` does not offer VEC_CRYPTO, so
    OpenSSL does not try them.

Worth recording as an aside: GCC never emitted `modsw`/`moduw`/`modsd`/
`modud` in the power9 corpus — it still computes `a % b` as divide, multiply,
subtract. Those gadgets exist anyway, so nothing follows from it.

## 3. Per group: gadget arms, what is reusable, what is hard

**Calibration.** The POWER8 base is **481 gadgets** (distinct `G(...)` names
in `jit/gen_ppc64.c`) in **3,641 lines** of aarch64 assembly (`alu.S` 994,
`fp.S` 1783, `memory.S` 404, `core.S` 317, `branch.S` 143) plus a
**1,369-line** translator — about 7.5 lines of assembly per gadget, because
nearly every gadget comes out of a `.macro`. The numbers below are in the same
unit and are estimates, not measurements.

### 3.1 Identity and advertisement — 0 gadgets
PVR `004e 1202` and `POWER9 (raw)` in `fs/proc/root.c`, `elf_platform`
`power9` in `kernel/abi.h`, `AT_BASE_PLATFORM` added, `AT_HWCAP2 |=
ARCH_3_00`, and the `guest-cpus.md` text. This is the **last** patch, by the
project's own rule. Note that it is a **cliff, not a slope**: the instant
ARCH_3_00 appears, glibc's resolvers switch `strlen`, `strcmp`, `strncmp`,
`strcpy`, `stpcpy`, `strncpy`, `stpncpy`, `rawmemchr` and the fenv inlines
onto their power9 variants wholesale, and every instruction those use must
already work. There is no way to advertise half of ARCH_3_00.

### 3.2 New VSX load/store encodings — ~7 new gadgets
`lxv`/`stxv` (DQ-form, major 61/57) and `lxvx`/`stxvx` (X-form) are plain
unaligned 16-byte accesses in a little-endian guest: the `read_prep 128` /
`write_prep 128` path in `jit/guest-ppc64/gadgets.h:108` already handles
128-bit accesses and their page crossings, so these are two gadget bodies
reused by four encodings. The existing `lvx`/`stvx` gadgets
cannot simply be renamed: `lvx` masks the low four address bits, which `lxv`
must not.
`lxvb16x`/`stxvb16x` add a full 16-byte reverse (`rev64` + `ext`, or one
constant `tbl`); `lxvh8x`/`stxvh8x` a `rev16`; `lxvw4x`/`stxvw4x` exist
already. `lxvwsx` is `ld1r {v.4s}`. `lxsd`/`lxssp`/`stxsd`/`stxssp` and
`lxsibzx`/`lxsihzx`/`stxsibx`/`stxsihx` need **no new gadgets at all** — the
existing `lfd`/`lfs`/`lbz`/`lhz` bodies already load into an 8-byte slot, and
a VSR's doubleword 0 is such a slot (`dw0()` in the translator); it is decode
work only, mostly in the two missing major opcodes. Low risk.

### 3.3 Length-indexed load/store — 4 gadgets, and the one real novelty
`lxvl`, `stxvl`, `lxvll`, `stxvll` transfer a *run-time* number of bytes
(0–16) and must not fault on the bytes they do not touch. `read_prep` takes a
**static** size and folds the alignment test into the TLB tag compare, so it
cannot serve a dynamic length directly. Two workable shapes: a byte loop
through `read_prep 8` (simple, correct, 16 iterations worst case), or a
length dispatch with a `read_prep 128` fast arm for the full-width case and a
byte loop for the tail. These are the instructions glibc's power9 `strcpy`
family uses for its last partial vector, so they are on the critical path and
their fault behaviour has to be right. Call it ~200 lines and the one piece
that needs new memory-model thought. Medium risk.

### 3.4 VSX 3.0 non-quad — ~38 gadgets
Nearly all of this maps one-to-one onto NEON: `xxspltib` is `dup`;
`xxbrh`/`xxbrw`/`xxbrd` are `rev16`/`rev32`/`rev64` and `xxbrq` a
`rev64`+`ext`; `xxperm`/`xxpermr` reuse the existing `vperm` `tbl` body with a
different index polarity; `xxextractuw`/`xxinsertw` are immediate-index lane
moves; `xscmpeqdp`/`xscmpgtdp`/`xscmpgedp` are `fcmeq`/`fcmgt`/`fcmge` on the
scalar doubleword writing a mask rather than CR; `xsiexpdp`/`xsxexpdp`/
`xsxsigdp` and the `xviexp`/`xvxexp`/`xvxsig` vector forms are bitfield
inserts and extracts; `mfvsrld`/`mtvsrdd`/`mtvsrws` are lane moves. Two notes:
`xsmaxcdp`/`xsmincdp`/`xsmaxjdp`/`xsminjdp` have **different NaN and
zero-sign rules** from the `xsmaxdp`/`xsmindp` already implemented, so they
are new bodies rather than aliases; and the half-precision converters
(`xscvhpdp`, `xscvdphp`, `xvcvhpsp`, `xvcvsphp`) are free, because aarch64 has
FP16 `fcvtl`/`fcvtn` natively. Low risk.

### 3.5 VMX 3.0 integer — ~51 gadgets
Mostly direct too: `vabsdu*` is `uabd`; `vcmpne*` is `cmeq` + `not`;
`vcmpnez*` is `cmeq` or'd with `cmeq #0` of both operands; `vctz[bhwd]` is
`rbit` + `clz`; `vexts*2*` is `sxtl`/`shl`+`sshr`; `vnegw`/`vnegd` is `neg`;
`vextract*`/`vinsert*` with immediate indices are lane moves. Five need real
thought:
- `vclzlsbb`/`vctzlsbb` — count leading/trailing elements whose LSB is clear:
  mask bit 0, narrow, reverse, `clz`; about 8 instructions. Mind the element
  order (§4's trap).
- `vext[bhw][lr]x` — extract at a **register** index. `tbl` with an index
  vector built from the register handles it without a stack round trip.
- `vslv`/`vsrv` — byte-wise variable shift *across the whole 128-bit vector*,
  with bits crossing byte boundaries. No single-instruction analogue; a
  two-shift-and-or sequence, ~20 lines.
- `vmsumudm` — 64×64→128 multiply-accumulate per doubleword pair. aarch64 has
  no 64×64→128 *vector* multiply, so this drops to scalar `mul`/`umulh`
  through the VSR slots, ~15 lines.
- `vmul10uq`/`vmul10cuq`/`vmul10euq`/`vmul10ecuq` — 128-bit decimal ×10 with
  carry. No analogue; a scalar 128-bit multiply-by-10, ~12 lines each.

While in this decode arm it would be worth closing an existing POWER8 gap:
the VA-form `vmsumubm`/`vmsummbm`/`vmsumuhm`/`vmsumuhs`/`vmsumshm`/
`vmsumshs`/`vmhaddshs`/`vmhraddshs`/`vmladduhm` (ISA 2.03) have no gadgets
today (`gen_vmx` at `jit/gen_ppc64.c:914` handles only VA opcodes 42, 43,
44, 46 and 47) — and `vmsumudm` lands beside them. ~10 further gadgets. Low-to-medium risk.

### 3.6 Integer and misc — ~11 gadgets
`addpcis` (the translator knows the pc; a `mov_const`), `setb`, `mcrxrx`,
`cmpeqb`, `cmprb`, `addex`, `extswsli`, `maddhd`, `maddhdu`, `maddld`
(`madd`/`umulh`), `darn`. `darn` is the only one with a design question: it
must be unpredictable, so it reads a kernel-side source, in the shape
`mftb`/`vclock` already use (`bl` into C from `jit/guest-ppc64/alu.S:904`) —
a kernel service, not arithmetic in C. All trivial. Low risk.

### 3.7 FPSCR access — ~6 gadgets
`mffsl`, `mffsce`, `mffscrn`, `mffscrni`, `mffscdrn`, `mffscdrni`. These are
small but they touch the lazy model at `emu/fpenv.c:275`: `mffsl` has to
fold the host FPSR's accumulated flags exactly as `mffs` does, and the `mffscrn` family both
*reads* FPSCR and *sets* the rounding mode, so it must reinstall the host
FPCR through `ppc64_control()`. glibc's `fegetround`/`SET_RESTORE_ROUND` use
them behind a runtime ARCH_3_00 test, so they go live with the
advertisement. Low risk, but fpenv is the kind of code where a wrong guess
shows up as a wrong `printf` three layers up.

### 3.8 Quad precision — ~41 gadgets, and the genuinely hard part
The `*qp` set: nine arithmetic operations each with a round-to-odd variant
(`xsaddqp(o)`, `xssubqp(o)`, `xsmulqp(o)`, `xsdivqp(o)`, `xssqrtqp(o)`,
`xsmaddqp(o)`, `xsmsubqp(o)`, `xsnmaddqp(o)`, `xsnmsubqp(o)`) = 18; three
compares (`xscmpoqp`, `xscmpuqp`, `xscmpexpqp`); nine converters
(`xscvdpqp`, `xscvqpdp(o)`, `xscvqpsdz`, `xscvqpswz`, `xscvqpudz`,
`xscvqpuwz`, `xscvsdqp`, `xscvudqp`); three rounders (`xsrqpi`, `xsrqpix`,
`xsrqpxp`); and eight sign/bit/class ops (`xsabsqp`, `xsnegqp`, `xsnabsqp`,
`xscpsgnqp`, `xsiexpqp`, `xsxexpqp`, `xsxsigqp`, `xststdcqp`).

The last eight are trivial bit manipulation. Everything else has **no host
analogue whatsoever**, and the evidence is blunter than "aarch64 lacks FP128
instructions":

```
$ clang -c f128.c            # on this Mac, arm64
error: __float128 is not supported on this target
```

Darwin arm64's `long double` is `double` (`__LDBL_MANT_DIG__` 53) and clang
refuses `__float128` outright, so there is no host type, no `__addtf3` in
compiler-rt to lean on, and nothing to crib. What it would really take:

- a 113-bit significand add/subtract with alignment, sticky collection and
  four rounding modes, plus round-to-odd;
- a 113×113→226-bit multiply, which on aarch64 is four `mul`/`umulh` pairs
  plus the carry chain, then normalise and round;
- a divide — Newton–Raphson on the reciprocal to 113 bits with a final
  correctly-rounded remainder step, or a restoring division;
- a square root on the same basis;
- a fused multiply-add that keeps the full 226-bit product before the single
  rounding (this is the one that cannot be composed from the others);
- all five IEEE exception flags folded into FPSCR through `emu/fpenv.c`, and
  tininess/inexact detected after rounding, POWER's way.

That is 800–1500 lines of assembly with a bit-exactness bar. It is the single
biggest obstacle in this whole scope.

**But it is separable.** §1's resolver measurement shows glibc selects its
quad variants on HAS_IEEE128, not ARCH_3_00, so a POWER9 guest can advertise
ARCH_3_00 and leave quad precision unimplemented and unadvertised
indefinitely; glibc keeps its soft-fp `float128` path, which is ordinary
POWER8 integer code the guest already runs. The two risks to watch, neither
of them live today:
- a distribution compiled `-mabi=ieeelongdouble` turns every `long double`
  operation into `xsaddqp` and friends *regardless of HWCAP*. Debian and
  Ubuntu ppc64el are still IBM double-double (`__LDBL_MANT_DIG__` 106,
  measured), but this is the direction the ecosystem has been moving.
- a program that uses `__float128` explicitly **and** is built `-mcpu=power9`
  gets inline `qp` instructions too (measured: `__float128` at `-mcpu=power9`
  is inline `xsaddqp` even with the IBM long-double ABI). That is rare —
  libquadmath users — and lands as a clean SIGILL with the `no gadget`
  printk naming the opcode.

### 3.9 `scv` — 1 gadget plus kernel work
Not an arithmetic instruction but a second system-call entry with its own
return convention (no CR0[SO]; a negative errno in r3) and its own vector.
`docs/ppc64le_guest_plan.md` deliberately left it out. It needs the interrupt
path, the restart rules, and agreement from ptrace, seccomp and
`/proc/cpuinfo` — the same surface the existing `sc` path covers in
`kernel/calls.c`. It has its own HWCAP2 bit, so it is independent of
everything above and can be left clear.

## 4. The oracle problem

There is no POWER9 hardware here, and qemu is already known wrong on `divde`/
`divdeo` and `vexptefp` (`docs/TODO.md`). So each group needs its own answer.
Two experiments settle most of it.

**Experiment A — a spec model inside the test.** `tests/ppc64/gen_*_diff.py`
diff AOK against qemu; the riscv64 vector work went further and generated a
test that checks each instruction against *a model of the instruction written
from the spec, independent of the implementation*
(`tools/gen-rvv-gadget-test.py`, 686 lines
generating 148k checks). That is the right pattern here. To prove it carries,
I wrote exactly such a model for `lxvl`, `stxvl`, `vctzlsbb`, `vclzlsbb`,
`vcmpnezb`, `vextublx`, `vextubrx`, `xxspltib`, `lxvb16x`, `xxbrd`, `xxbrq`,
`maddld`, `setb` and `extswsli`, ran it under `qemu-ppc64le -cpu power9`, and
got **896 checks, 0 mismatches** over 64 random rounds. The same binary
SIGILLs under `-cpu power8`. So for every integer, vector and load/store group
in §3.2–§3.7 there are *two* independent oracles that already agree, and the
model is the one that can be committed as a test.

**Experiment B — quad precision against a second IEEE implementation.** Quad
is the group with no hardware, no host type, and a spec model that would be as
hard to write as the gadget. The way out is a second, independent binary128
implementation: x86-64's libgcc soft-fp. A 24-round random test of add,
subtract, multiply, divide, fma, sqrt, compare and the integer/double
conversions, run under `qemu -cpu power9` and then compiled natively for
x86-64, produced **264 of 264 output lines identical** apart from 14 lines of
`(long long)(double)x` out-of-range saturation, which is architecture-defined
and not a float128 difference. So qemu's POWER9 quad arithmetic and libgcc's
soft-fp agree bit for bit, and either can serve as the oracle for AOK's
gadgets. Rounding modes other than nearest, and the round-to-odd `(o)` forms,
were **not** covered by that run and would need their own pass — round-to-odd
has no soft-fp analogue on x86, so it needs a spec model or qemu alone.

Per group, then:

| Group | Oracle | Confidence |
|---|---|---|
| New loads/stores, length-indexed, VMX 3.0, VSX 3.0 misc, integer | spec model in the test, cross-checked against `qemu -cpu power9` | **high** — measured, 896/896 |
| Scalar DP compares, min/max-C/J | spec model (the NaN and zero-sign rules are the whole content) | high |
| FPSCR (`mffsl` family) | compiler-generated `fenv` code plus a model; glibc's own `fesetround`/`fegetround` tests on a ppc64le root are the end-to-end check | medium — qemu's FPSCR corners are not independently verified |
| Quad precision, nearest rounding | qemu **and** x86-64 libgcc soft-fp, which agree | **high** — measured |
| Quad precision, directed rounding and round-to-odd | qemu only, or a spec model | **low — state this plainly.** qemu is already known wrong on `fnmadd`/`fnmsub` under directed rounding on this target, which is exactly this neighbourhood |
| `darn` | no oracle possible, and none needed: the test is statistical | n/a |
| `scv` | Linux's own ABI, checked with strace and the existing syscall tests on a ppc64le root | high |
| BCD, AMO, copy-paste, privileged | not implemented; the test is that they SIGILL | n/a |

**A trap worth writing down.** My first `vclzlsbb`/`vctzlsbb` model had the
two the wrong way round and produced 86 "mismatches" that were mine, not
qemu's. In a little-endian guest, big-endian element 0 is the host's byte 15,
so ISA "leading" counts *down* from the top byte of the host register. The
same reversal governs `vextublx` versus `vextubrx` and every `vextract`/
`vinsert` index. Expect this to be the main source of wrong gadgets in
§3.5, and make the model assert element order explicitly.

## 5. Effort, and the order to do it in

Estimates in working days at this project's pace, using the POWER8 port (481
gadgets) and the riscv64 extension commits as the yardstick — RVA23's scalar
extensions landed as one 1,327-line commit, RVV 1.0's scaffolding as one
1,823-line commit with the gadgets and a generated 148k-check model test
following the same day, and deliberately **"not yet advertised"**
(`75e85ce52`), which is the pattern to copy.

| # | Group | New gadgets | Days | Notes |
|---|---|---|---|---|
| 1 | VSX load/store encodings (§3.2) | ~7 | 1 | opens major opcodes 57 and 61 |
| 2 | Integer and misc (§3.6) | ~11 | 0.5 | all small |
| 3 | VSX 3.0 non-quad (§3.4) | ~38 | 1–2 | half-precision is free |
| 4 | VMX 3.0 integer (§3.5) | ~51 (+10 POWER8 VA-form) | 2–3 | `vslv`/`vsrv`, `vmsumudm`, `vmul10*` need thought |
| 5 | Length-indexed load/store (§3.3) | 4 | 1–2 | the only memory-model novelty |
| 6 | FPSCR family (§3.7) | ~6 | 1 | touches `emu/fpenv.c` |
| 7 | Model test generator + a ppc64le root pass (§4) | — | 2–3 | `tools/gen-ppc64-isa30-test.py`, in `gen-rvv-gadget-test.py`'s image |
| 8 | Identity and advertisement (§3.1) | 0 | 1 | PVR, platform, HWCAP2 ARCH_3_00, cpuinfo, docs, suite agreement |
| | **subtotal: a working POWER9 userland** | **~117** (+10 for the POWER8 VA-form gap) | **9.5–13.5** | |
| 9 | Quad precision (§3.8) | ~41 | 8–15 | the hard part; needs HAS_IEEE128 to be worth anything |
| 10 | `scv` (§3.9) | 1 + kernel | 1–2 | independent HWCAP2 bit |
| | **total** | **~159** | **18.5–30.5** | |

So: **two to three working weeks for POWER9 as a userland target**, and two
to three more for IEEE-128 quad precision if and when it is wanted. For scale,
~159 new gadgets against the 481 the POWER8 base has — a third again as much
gadget surface, with the difficulty very unevenly spread.

**Recommended order:** 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8, then stop. That order
is not arbitrary:

- Items 1–5 are exactly the instructions glibc's power9 string functions and a
  `-mcpu=power9` compiler emit, which is what "a POWER9-targeted userland"
  means in practice. Nothing before item 8 changes what any existing root
  does, because the new encodings stay unreachable until ARCH_3_00 is
  advertised — the only visible difference is that a binary hand-built
  `-mcpu=power9` starts working instead of dying. So each item can land on
  its own with the other four guests green, the way the ppc64le port's ten
  patches did.
- Item 7 before item 8, because the project's own rule is that the suite runs
  an instruction for every bit it sets, and item 8 is the cliff: ARCH_3_00
  switches glibc's resolvers over wholesale and there is no partial setting.
- Items 9 and 10 are separate bits (HAS_IEEE128, SCV) and therefore separate
  decisions. Leaving both clear is a complete, honest POWER9: that is what
  qemu's own `-cpu power9` would look like if you masked those two bits, and
  glibc copes by design.
- The honest check on every count in this document is to set `AT_HWCAP2`
  ARCH_3_00 **on an experimental build only** (an `ISH_*` knob, never
  shipped), run a Debian ppc64el root and collect the `no gadget for insn`
  printks. Note what it is and is not: with nothing implemented it SIGILLs at
  the first unimplemented opcode, so it surveys roughly one opcode per run —
  useful as a slow probe now, and genuinely valuable after items 1–6, when
  the list should come back empty. It is the measurement that would turn §2's
  corpus argument into a fact about this specific root.

## Open questions

1. **What Alpine 3.24.2 ppc64le was built for** is argued here from the
   port's behaviour, not from its binaries — there is no such root on any
   machine here and fetching one is a download. §1 says how to settle it.
2. **Whether qemu is right on directed-rounding and round-to-odd quad
   arithmetic.** It is known wrong on `fnmadd`/`fnmsub` under directed
   rounding already. If quad is ever attempted, this needs a spec model or a
   real POWER9, and no amount of agreement at round-to-nearest substitutes.
3. **Whether a cloud POWER9 is worth renting.** Not needed for anything in
   items 1–8, on the evidence above. It would matter only for item 9's corners
   and for `darn`'s hardware behaviour.
4. **`AT_MINSIGSTKSZ` and `AT_BASE_PLATFORM`** are both absent from the ppc64
   aux vector today (the first is already noted in `docs/TODO.md` as
   unverified). A POWER9 identity pass is the natural moment to settle both,
   but neither is an ISA 3.0 question.
