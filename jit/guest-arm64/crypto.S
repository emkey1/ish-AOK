// AArch64 guest crypto-extension gadgets: AES, SHA1, SHA256, SHA512, and
// the SHA3 four-register ops. Ported from OpenMinis/ish-arm64
// gadgets-aarch64/crypto.S (GPLv3, see /CREDITS-aarch64.md); their
// bodies use raw .inst encodings — this port uses the mnemonics with
// .arch_extension. FEAT_AES/SHA1/SHA256 are v8.0 crypto, present on
// every 64-bit Apple SoC (A7+). FEAT_SHA512/SHA3 only arrived with A13,
// so: the SHA3 ops (RAX1/EOR3/BCAX/XAR) are synthesized from baseline
// NEON here, and the native SHA512 gadgets are only handed out by gen.c
// when the host has the instructions — otherwise gen.c emits sha512_soft,
// the same ops in 64-bit scalar instructions.
//
// Code streams:
//   1-src:  [gadget][rd | rn<<8]
//   2-src:  [gadget][rd | rn<<8 | rm<<16]     (rd is the accumulator)
//   4-reg:  [gadget][rd | rn<<8 | rm<<16 | ra<<24]

#include "gadgets.h"

.arch_extension aes
.arch_extension sha2
.arch_extension sha3

.text

// Vd = op(Vd, Vn): AESE/AESD (state in Vd, round key in Vn).
.macro crypto_2acc name, insn
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    add x12, _cpu, #CPU_v0
    add x13, x12, x9, lsl #4
    ldr q0, [x13]
    add x14, x12, x10, lsl #4
    ldr q1, [x14]
    \insn
    str q0, [x13]
    gret
.endm

crypto_2acc aese,      "aese v0.16b, v1.16b"
crypto_2acc aesd,      "aesd v0.16b, v1.16b"
crypto_2acc sha1su1,   "sha1su1 v0.4s, v1.4s"
crypto_2acc sha256su0, "sha256su0 v0.4s, v1.4s"
crypto_2acc sha512su0, "sha512su0 v0.2d, v1.2d"

// Vd = op(Vn): AESMC/AESIMC/SHA1H (pure functions of Vn).
.macro crypto_1src name, insn
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    add x12, _cpu, #CPU_v0
    add x14, x12, x10, lsl #4
    ldr q0, [x14]
    \insn
    add x13, x12, x9, lsl #4
    str q0, [x13]
    gret
.endm

crypto_1src aesmc,  "aesmc v0.16b, v0.16b"
crypto_1src aesimc, "aesimc v0.16b, v0.16b"
crypto_1src sha1h,  "sha1h s0, s0"          // scalar result: upper bits zeroed

// Vd = op(Vd, Vn, Vm): the three-register hash steps.
.macro crypto_3acc name, insn
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    add x12, _cpu, #CPU_v0
    add x13, x12, x9, lsl #4
    ldr q0, [x13]
    add x14, x12, x10, lsl #4
    ldr q1, [x14]
    add x14, x12, x11, lsl #4
    ldr q2, [x14]
    \insn
    str q0, [x13]
    gret
.endm

crypto_3acc sha1c,     "sha1c q0, s1, v2.4s"
crypto_3acc sha1p,     "sha1p q0, s1, v2.4s"
crypto_3acc sha1m,     "sha1m q0, s1, v2.4s"
crypto_3acc sha1su0,   "sha1su0 v0.4s, v1.4s, v2.4s"
crypto_3acc sha256h,   "sha256h q0, q1, v2.4s"
crypto_3acc sha256h2,  "sha256h2 q0, q1, v2.4s"
crypto_3acc sha256su1, "sha256su1 v0.4s, v1.4s, v2.4s"
crypto_3acc sha512h,   "sha512h q0, q1, v2.2d"
crypto_3acc sha512h2,  "sha512h2 q0, q1, v2.2d"
crypto_3acc sha512su1, "sha512su1 v0.2d, v1.2d, v2.2d"

// RAX1 Vd.2d = Vn ^ ROL(Vm, 1) — non-accumulating. Synthesized from
// baseline NEON (shl + usra build the per-lane rotate; the shifted-in
// bits are disjoint, so accumulate == insert) so pre-FEAT_SHA3 hosts
// run it too. Verified bit-exact against native rax1 on an M-series
// host over random vectors.
// [gadget][rd | rn<<8 | rm<<16]
.gadget rax1
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    add x12, _cpu, #CPU_v0
    add x14, x12, x10, lsl #4
    ldr q1, [x14]
    add x14, x12, x11, lsl #4
    ldr q2, [x14]
    shl v0.2d, v2.2d, #1
    usra v0.2d, v2.2d, #63
    eor v0.16b, v0.16b, v1.16b
    add x13, x12, x9, lsl #4
    str q0, [x13]
    gret

// SHA3 four-register: Vd = op(Vn, Vm, Va) — not accumulating. Bodies are
// baseline NEON (EOR3 = eor+eor, BCAX = bic+eor), so no FEAT_SHA3 needed.
.macro crypto_4reg name, insn1, insn2
.gadget \name
    ldr x8, [_ip]
    add _ip, _ip, #8
    and x9, x8, #0x1f
    ubfx x10, x8, #8, #5
    ubfx x11, x8, #16, #5
    ubfx x15, x8, #24, #5
    add x12, _cpu, #CPU_v0
    add x14, x12, x10, lsl #4
    ldr q1, [x14]
    add x14, x12, x11, lsl #4
    ldr q2, [x14]
    add x14, x12, x15, lsl #4
    ldr q3, [x14]
    \insn1
    \insn2
    add x13, x12, x9, lsl #4
    str q0, [x13]
    gret
.endm

crypto_4reg eor3, "eor v0.16b, v1.16b, v2.16b", "eor v0.16b, v0.16b, v3.16b"
crypto_4reg bcax, "bic v0.16b, v2.16b, v3.16b", "eor v0.16b, v0.16b, v1.16b"

// SHA512 for hosts without FEAT_SHA512 (pre-A13), in 64-bit scalar ops,
// keeping the guest-visible feature set (AT_HWCAP SHA512,
// ID_AA64ISAR0.SHA2=2) identical on every device. Every operand is read
// before Vd is written (Vd may alias Vn or Vm).
// op: 0=SHA512H 1=SHA512H2 2=SHA512SU1 3=SHA512SU0.
// [gadget][rd | rn<<8 | rm<<16 | op<<24]
// Sigma: \d = ror(\x, a) ^ ror(\x, b) ^ ror(\x, c) (or lsr for the last)
.macro sha_sig d, x, a, b, c, last=ror
    ror \d, \x, #\a
    eor \d, \d, \x, ror #\b
    eor \d, \d, \x, \last #\c
.endm
.gadget sha512_soft
    ldr x8, [_ip]
    add _ip, _ip, #8
    add x9, _cpu, #CPU_v0
    and x10, x8, #0x1f
    add x10, x9, x10, lsl #4    // &Vd
    ubfx x11, x8, #8, #5
    add x11, x9, x11, lsl #4
    ubfx x12, x8, #16, #5
    add x12, x9, x12, lsl #4
    ldp x3, x4, [x10]           // d0, d1
    ldp x5, x6, [x11]           // n0, n1
    ldp x13, x14, [x12]         // m0, m1
    ubfx x9, x8, #24, #2
    cmp x9, #1
    b.lo 1f
    b.eq 2f
    cmp x9, #2
    b.eq 3f
    // SHA512SU0: d0 += s0(d1), d1 += s0(n0); s0 = ror1 ^ ror8 ^ lsr7
    sha_sig x15, x4, 1, 8, 7, lsr
    sha_sig x16, x5, 1, 8, 7, lsr
    add x3, x3, x15
    add x4, x4, x16
    b 9f
1:  // SHA512H: d1 += S1(m1) + Ch(m1, n0, n1); t = d1 + m0;
    // d0 += S1(t) + Ch(t, m1, n0); S1 = ror14 ^ ror18 ^ ror41
    sha_sig x15, x14, 14, 18, 41
    eor x16, x5, x6
    and x16, x16, x14
    eor x16, x16, x6            // Ch(x, y, z) = (x & (y ^ z)) ^ z
    add x4, x4, x15
    add x4, x4, x16
    add x17, x4, x13            // t
    sha_sig x15, x17, 14, 18, 41
    eor x16, x14, x5
    and x16, x16, x17
    eor x16, x16, x5
    add x3, x3, x15
    add x3, x3, x16
    b 9f
2:  // SHA512H2: d1 += S0(m0) + Maj(m0, m1, n0);
    // d0 += S0(d1) + Maj(d1, m0, m1); S0 = ror28 ^ ror34 ^ ror39
    sha_sig x15, x13, 28, 34, 39
    and x16, x13, x14
    orr x17, x13, x14
    and x17, x17, x5
    orr x16, x16, x17           // Maj(x, y, z) = (x & y) | ((x | y) & z)
    add x4, x4, x15
    add x4, x4, x16
    sha_sig x15, x4, 28, 34, 39
    and x16, x4, x13
    orr x17, x4, x13
    and x17, x17, x14
    orr x16, x16, x17
    add x3, x3, x15
    add x3, x3, x16
    b 9f
3:  // SHA512SU1: d += s1(n) + m per lane; s1 = ror19 ^ ror61 ^ lsr6
    sha_sig x15, x5, 19, 61, 6, lsr
    sha_sig x16, x6, 19, 61, 6, lsr
    add x3, x3, x15
    add x3, x3, x13
    add x4, x4, x16
    add x4, x4, x14
9:  stp x3, x4, [x10]
    gret

// XAR Vd.2d, Vn.2d, Vm.2d, #imm6: (Vn ^ Vm) rotated right by imm per
// 64-bit lane. The host imm form can't take a runtime amount; rotate =
// (x >> n) | (x << (64-n)) via two broadcast register shifts.
// [gadget][rd | rn<<8 | rm<<16][neg_amount][64 - amount]
.gadget xar
    ldr x8, [_ip]
    ldr x9, [_ip, #8]          // -n
    ldr x14, [_ip, #16]        // 64-n
    add _ip, _ip, #24
    and x10, x8, #0x1f
    ubfx x11, x8, #8, #5
    ubfx x15, x8, #16, #5
    add x12, _cpu, #CPU_v0
    add x13, x12, x11, lsl #4
    ldr q0, [x13]
    add x13, x12, x15, lsl #4
    ldr q1, [x13]
    eor v0.16b, v0.16b, v1.16b
    dup v29.2d, x9
    dup v28.2d, x14
    ushl v1.2d, v0.2d, v29.2d  // x >> n
    ushl v0.2d, v0.2d, v28.2d  // x << (64-n)
    orr v0.16b, v0.16b, v1.16b
    add x13, x12, x10, lsl #4
    str q0, [x13]
    gret

# vim: ft=gas
