// The RISC-V V extension (RVV 1.0, with Zvbb, Zvfhmin and Zvkt) for the
// riscv64 guest: RVA23U64 makes it mandatory, and a distribution built for
// RVA23 vectorizes its code freely (docs/build_558_musts.md section 1).
//
// Correctness first: every vector instruction is one gadget
// (guest-riscv64/vector.S riscv64_vop) that calls riscv64_vector_exec with
// the encoding, and the instruction runs here in C, element by element,
// driven by vtype. Hot instructions can later get gadgets of their own, as
// the scalar ISA has; this file is then their oracle.
//
// Implementation choices the spec leaves open:
//   - VLEN = 128 (vlenb 16), ELEN = 64: the NEON width, 32 x 16 bytes in
//     cpu_state.riscv64_v.
//   - vsetvl: vl = min(AVL, VLMAX).
//   - Tail and masked-off elements are left undisturbed whatever vta/vma
//     say (agnostic permits it).
//   - A trap in a memory instruction leaves vstart at the element that
//     faulted; the instruction restarts from there.
//   - Every instruction reads its operands from a copy of the register file
//     taken at its start, so any overlap of a destination with a source
//     reads the pre-instruction values (what the spec requires where an
//     overlap is legal; where it is reserved, any result is allowed).
//
// Floating point runs on the host FPU, which while guest code runs is set to
// frm with default-NaN mode (emu/fpenv.c), so results and the canonical NaN
// come out as RISC-V's; flags the host raises are gathered into fflags like
// the scalar gadgets'. Exceptions to that are done here by hand (the
// saturating conversions, fmin/fmax's signed zeroes).

#include <fenv.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "emu/cpu.h"
#include "emu/tlb.h"
#include "emu/interrupt.h"

#define VLENB 16
#define VLEN (VLENB * 8)

// ---- vtype ----
struct vcfg {
    unsigned sew;      // element width in bits: 8..64
    unsigned sewb;     // ... in bytes
    int lmul_log2;     // -3..3
    unsigned vlmax;
    bool vill;
};

static struct vcfg vcfg_of(uint64_t vtype) {
    struct vcfg c = {0};
    unsigned vlmul = vtype & 7, vsew = (vtype >> 3) & 7;
    c.vill = (vtype >> 63) & 1;
    // reserved: vlmul 4, vsew > 3, any bit above vma other than vill
    if (vlmul == 4 || vsew > 3 || (vtype & ~(0xffull | (1ull << 63))) != 0)
        c.vill = true;
    if (c.vill)
        return c;
    c.sew = 8u << vsew;
    c.sewb = c.sew / 8;
    c.lmul_log2 = vlmul < 4 ? (int) vlmul : (int) vlmul - 8;
    // fractional LMUL must leave room for one element: SEW <= LMUL * ELEN
    if (c.lmul_log2 < 0 && c.sew > (64u >> -c.lmul_log2)) {
        c.vill = true;
        return c;
    }
    c.vlmax = c.lmul_log2 >= 0 ? (VLEN << c.lmul_log2) / c.sew : (VLEN >> -c.lmul_log2) / c.sew;
    return c;
}

// ---- the register file, as elements ----
static inline uint8_t *vreg(struct cpu_state *cpu) { return (uint8_t *) cpu->riscv64_v; }

static inline uint64_t eget(const uint8_t *file, unsigned v, unsigned i, unsigned eb) {
    const uint8_t *p = file + v * VLENB + i * eb;
    switch (eb) {
    case 1: return *p;
    case 2: { uint16_t x; memcpy(&x, p, 2); return x; }
    case 4: { uint32_t x; memcpy(&x, p, 4); return x; }
    default: { uint64_t x; memcpy(&x, p, 8); return x; }
    }
}
static inline void eset(uint8_t *file, unsigned v, unsigned i, unsigned eb, uint64_t x) {
    uint8_t *p = file + v * VLENB + i * eb;
    switch (eb) {
    case 1: *p = (uint8_t) x; break;
    case 2: { uint16_t y = (uint16_t) x; memcpy(p, &y, 2); break; }
    case 4: { uint32_t y = (uint32_t) x; memcpy(p, &y, 4); break; }
    default: memcpy(p, &x, 8); break;
    }
}
static inline int64_t sext(uint64_t x, unsigned bits) {
    return bits >= 64 ? (int64_t) x : (int64_t) (x << (64 - bits)) >> (64 - bits);
}
static inline uint64_t trunc_to(uint64_t x, unsigned bits) {
    return bits >= 64 ? x : x & ((1ull << bits) - 1);
}
static inline bool mbit(const uint8_t *file, unsigned v, unsigned i) {
    return (file[v * VLENB + (i >> 3)] >> (i & 7)) & 1;
}
static inline void mset(uint8_t *file, unsigned v, unsigned i, bool b) {
    uint8_t *p = &file[v * VLENB + (i >> 3)];
    *p = (uint8_t) ((*p & ~(1u << (i & 7))) | ((unsigned) b << (i & 7)));
}

// A register group of EMUL = 2^emul_log2 starting at v must be aligned to it
// and fit in v0-v31.
static bool group_ok(unsigned v, int emul_log2) {
    if (emul_log2 > 3)
        return false;
    if (emul_log2 <= 0)
        return true;
    unsigned n = 1u << emul_log2;
    return v % n == 0 && v + n <= 32;
}

// ---- encodings ----
#define F_VD(i) (((i) >> 7) & 31)
#define F_RS1(i) (((i) >> 15) & 31)
#define F_RS2(i) (((i) >> 20) & 31)
#define F_VM(i) (((i) >> 25) & 1)
#define F_FUNCT3(i) (((i) >> 12) & 7)
#define F_FUNCT6(i) ((i) >> 26)
#define F_SIMM5(i) (sext(F_RS1(i), 5))

static int fault(struct cpu_state *cpu, struct tlb *tlb, bool write, uint64_t vstart) {
    cpu->segfault_addr = tlb->segfault_addr;
    cpu->segfault_was_write = write;
    cpu->riscv64_vstart = vstart;
    return INT_PF;
}

// ---- vsetvli / vsetivli / vsetvl ----
static int vsetvl(struct cpu_state *cpu, uint32_t insn) {
    unsigned rd = F_VD(insn), rs1 = F_RS1(insn);
    uint64_t vtype, avl;
    bool keep_vl = false;
    if ((insn >> 31) == 0) { // vsetvli
        vtype = (insn >> 20) & 0x7ff;
    } else if ((insn >> 30) == 3) { // vsetivli: uimm AVL in rs1
        vtype = (insn >> 20) & 0x3ff;
    } else { // vsetvl
        if (((insn >> 25) & 0x7f) != 0x40)
            return INT_UNDEFINED;
        vtype = cpu->riscv64_regs[F_RS2(insn)];
    }
    if ((insn >> 30) == 3) {
        avl = rs1;
    } else if (rs1 != 0) {
        avl = cpu->riscv64_regs[rs1];
    } else if (rd != 0) {
        avl = UINT64_MAX;
    } else {
        keep_vl = true;
        avl = cpu->riscv64_vl;
    }
    struct vcfg c = vcfg_of(vtype);
    if (keep_vl && !c.vill) {
        // vsetvli x0, x0 keeps vl; only legal if VLMAX does not change it
        struct vcfg old = vcfg_of(cpu->riscv64_vtype);
        if (old.vill || old.vlmax != c.vlmax)
            c.vill = true;
    }
    if (c.vill) {
        cpu->riscv64_vtype = 1ull << 63;
        cpu->riscv64_vl = 0;
    } else {
        cpu->riscv64_vtype = vtype;
        cpu->riscv64_vl = avl < c.vlmax ? avl : c.vlmax;
    }
    cpu->riscv64_vstart = 0;
    if (rd != 0)
        cpu->riscv64_regs[rd] = cpu->riscv64_vl;
    return INT_NONE;
}

// ---- vector CSRs: vstart vxsat vxrm vcsr (rw), vl vtype vlenb (ro) ----
bool riscv64_vector_csr(unsigned csr) {
    return csr == 0x008 || csr == 0x009 || csr == 0x00a || csr == 0x00f ||
           csr == 0xc20 || csr == 0xc21 || csr == 0xc22;
}

static int vcsr_op(struct cpu_state *cpu, uint32_t insn) {
    unsigned csr = insn >> 20, rd = F_VD(insn), rs1 = F_RS1(insn), f3 = F_FUNCT3(insn);
    uint64_t old;
    switch (csr) {
    case 0x008: old = cpu->riscv64_vstart; break;
    case 0x009: old = cpu->riscv64_vxsat; break;
    case 0x00a: old = cpu->riscv64_vxrm; break;
    case 0x00f: old = cpu->riscv64_vxrm << 1 | cpu->riscv64_vxsat; break;
    case 0xc20: old = cpu->riscv64_vl; break;
    case 0xc21: old = cpu->riscv64_vtype; break;
    default: old = VLENB; break;
    }
    uint64_t src = (f3 & 4) ? rs1 : cpu->riscv64_regs[rs1];
    bool write = (f3 & 3) == 1 || rs1 != 0;
    if (write) {
        if (csr >= 0xc00)
            return INT_UNDEFINED; // read-only
        uint64_t nv = (f3 & 3) == 1 ? src : (f3 & 3) == 2 ? (old | src) : (old & ~src);
        switch (csr) {
        case 0x008: cpu->riscv64_vstart = nv & (VLEN - 1); break; // enough bits for any element index
        case 0x009: cpu->riscv64_vxsat = nv & 1; break;
        case 0x00a: cpu->riscv64_vxrm = nv & 3; break;
        case 0x00f: cpu->riscv64_vxrm = (nv >> 1) & 3; cpu->riscv64_vxsat = nv & 1; break;
        }
    }
    if (rd != 0)
        cpu->riscv64_regs[rd] = old;
    return INT_NONE;
}

// ---- loads and stores ----
// LOAD-FP/STORE-FP with width 0/5/6/7: EEW 8/16/32/64.
static int vmem(struct cpu_state *cpu, struct tlb *tlb, uint32_t insn, const uint8_t *snap) {
    bool store = (insn & 0x7f) == 0x27;
    unsigned width = F_FUNCT3(insn);
    unsigned eewb = width == 0 ? 1 : width == 5 ? 2 : width == 6 ? 4 : 8;
    unsigned vd = F_VD(insn), rs1 = F_RS1(insn), vs2 = F_RS2(insn);
    unsigned mop = (insn >> 26) & 3, nf = ((insn >> 29) & 7) + 1, vm = F_VM(insn);
    if ((insn >> 28) & 1) // mew: reserved
        return INT_UNDEFINED;
    uint64_t base = cpu->riscv64_regs[rs1];
    uint8_t *file = vreg(cpu);

    if (mop == 0 && vs2 == 8) { // whole-register: vl<nf>re<eew>.v / vs<nf>r.v
        if (!vm || (nf != 1 && nf != 2 && nf != 4 && nf != 8) || vd % nf != 0)
            return INT_UNDEFINED;
        if (store && width != 0)
            return INT_UNDEFINED; // vs<nf>r is encoded with EEW 8 only
        unsigned n = nf * VLENB / eewb;
        for (uint64_t i = cpu->riscv64_vstart; i < n; i++) {
            uint64_t a = base + i * eewb, x;
            if (store) {
                x = eget(snap, vd, (unsigned) i, eewb);
                if (!tlb_write(tlb, a, &x, eewb))
                    return fault(cpu, tlb, true, i);
            } else {
                x = 0;
                if (!tlb_read(tlb, a, &x, eewb))
                    return fault(cpu, tlb, false, i);
                eset(file, vd, (unsigned) i, eewb, x);
            }
        }
        cpu->riscv64_vstart = 0;
        return INT_NONE;
    }

    struct vcfg c = vcfg_of(cpu->riscv64_vtype);
    if (c.vill)
        return INT_UNDEFINED;
    uint64_t vl = cpu->riscv64_vl;

    if (mop == 0 && vs2 == 0xb) { // vlm.v / vsm.v: a mask, ceil(vl/8) bytes
        if (width != 0 || nf != 1 || !vm)
            return INT_UNDEFINED;
        uint64_t n = (vl + 7) / 8;
        for (uint64_t i = cpu->riscv64_vstart; i < n; i++) {
            uint8_t x;
            if (store) {
                x = snap[vd * VLENB + i];
                if (!tlb_write(tlb, base + i, &x, 1))
                    return fault(cpu, tlb, true, i);
            } else {
                if (!tlb_read(tlb, base + i, &x, 1))
                    return fault(cpu, tlb, false, i);
                file[vd * VLENB + i] = x;
            }
        }
        cpu->riscv64_vstart = 0;
        return INT_NONE;
    }

    bool indexed = mop == 1 || mop == 3;
    bool ff = !store && mop == 0 && vs2 == 0x10;
    if (mop == 0 && vs2 != 0 && !ff)
        return INT_UNDEFINED;
    // The data's EEW is the instruction's for unit-stride and strided, SEW
    // for indexed (the index vector has the instruction's EEW).
    unsigned deb = indexed ? c.sewb : eewb;
    int demul = indexed ? c.lmul_log2
                        : c.lmul_log2 + (int) __builtin_ctz(eewb * 8) - (int) __builtin_ctz(c.sew);
    int iemul = c.lmul_log2 + (int) __builtin_ctz(eewb * 8) - (int) __builtin_ctz(c.sew);
    if (demul < -3 || demul > 3 || (demul > 0 ? (1 << demul) : 1) * nf > 8)
        return INT_UNDEFINED;
    if (!group_ok(vd, demul) || vd + (demul > 0 ? (1u << demul) : 1u) * nf > 32)
        return INT_UNDEFINED;
    if (indexed && (iemul < -3 || iemul > 3 || !group_ok(vs2, iemul)))
        return INT_UNDEFINED;
    if (!store && !vm && vd == 0)
        return INT_UNDEFINED; // a masked load may not write v0
    unsigned regs_per_field = demul > 0 ? 1u << demul : 1u;
    int64_t stride = mop == 2 ? (int64_t) cpu->riscv64_regs[vs2] : (int64_t) (nf * deb);

    // Fast path: unmasked unit-stride with one field, within one page -- a
    // single host copy. A fault-only-first load only gets here when nothing
    // can fault, so it needs no trimming.
    if (mop == 0 && nf == 1 && vm && cpu->riscv64_vstart < vl) {
        uint64_t first = base + cpu->riscv64_vstart * deb, len = (vl - cpu->riscv64_vstart) * deb;
        if ((first & (PAGE_SIZE - 1)) + len <= PAGE_SIZE) {
            void *host = store ? __tlb_write_ptr(tlb, first) : __tlb_read_ptr(tlb, first);
            if (host != NULL) {
                uint8_t *regs = file + vd * VLENB + cpu->riscv64_vstart * deb;
                if (store)
                    memcpy(host, regs, len);
                else
                    memcpy(regs, host, len);
                cpu->riscv64_vstart = 0;
                return INT_NONE;
            }
        }
    }

    for (uint64_t i = cpu->riscv64_vstart; i < vl; i++) {
        if (!vm && !mbit(snap, 0, (unsigned) i))
            continue;
        uint64_t ea;
        if (indexed)
            ea = base + eget(snap, vs2, (unsigned) i, eewb);
        else
            ea = base + (uint64_t) ((int64_t) i * stride);
        for (unsigned f = 0; f < nf; f++) {
            uint64_t a = ea + (uint64_t) f * deb;
            unsigned reg = vd + f * regs_per_field;
            if (store) {
                uint64_t x = eget(snap, reg, (unsigned) i, deb);
                if (!tlb_write(tlb, a, &x, deb))
                    return fault(cpu, tlb, true, i);
            } else {
                uint64_t x = 0;
                if (!tlb_read(tlb, a, &x, deb)) {
                    if (ff && i > 0) { // fault-only-first: trim vl, no trap
                        cpu->riscv64_vl = i;
                        cpu->riscv64_vstart = 0;
                        return INT_NONE;
                    }
                    return fault(cpu, tlb, false, i);
                }
                eset(file, reg, (unsigned) i, deb, x);
            }
        }
    }
    cpu->riscv64_vstart = 0;
    return INT_NONE;
}

// ---- fixed point: vxrm rounding (spec 3.8) ----
static uint64_t round_inc(unsigned __int128 v, unsigned d, unsigned vxrm) {
    if (d == 0)
        return 0;
    unsigned bit_d1 = (unsigned) ((v >> (d - 1)) & 1);
    unsigned __int128 below = d >= 2 ? v & (((unsigned __int128) 1 << (d - 1)) - 1) : 0;
    unsigned bit_d = (unsigned) ((v >> d) & 1);
    switch (vxrm & 3) {
    case 0: return bit_d1;                                 // rnu
    case 1: return bit_d1 & ((below != 0) | bit_d);         // rne
    case 2: return 0;                                       // rdn
    default: return !bit_d & ((bit_d1 | (below != 0)) != 0); // rod
    }
}
static uint64_t roundoff_u(unsigned __int128 v, unsigned d, unsigned vxrm) {
    return (uint64_t) ((v >> d) + round_inc(v, d, vxrm));
}
static int64_t roundoff_s(__int128 v, unsigned d, unsigned vxrm) {
    return (int64_t) ((v >> d) + (__int128) round_inc((unsigned __int128) v, d, vxrm));
}
static uint64_t sat_u(unsigned __int128 v, unsigned bits, bool *sat) {
    unsigned __int128 max = bits >= 64 ? UINT64_MAX : (1ull << bits) - 1;
    if (v > max) { *sat = true; return (uint64_t) max; }
    return (uint64_t) v;
}
static int64_t sat_s(__int128 v, unsigned bits, bool *sat) {
    __int128 max = ((__int128) 1 << (bits - 1)) - 1, min = -((__int128) 1 << (bits - 1));
    if (v > max) { *sat = true; return (int64_t) max; }
    if (v < min) { *sat = true; return (int64_t) min; }
    return (int64_t) v;
}

static uint64_t brev8(uint64_t x, unsigned bytes) {
    uint64_t r = 0;
    for (unsigned b = 0; b < bytes; b++) {
        uint8_t v = (uint8_t) (x >> (8 * b)), o = 0;
        for (int k = 0; k < 8; k++)
            o |= (uint8_t) (((v >> k) & 1) << (7 - k));
        r |= (uint64_t) o << (8 * b);
    }
    return r;
}
static uint64_t rev8(uint64_t x, unsigned bytes) {
    uint64_t r = 0;
    for (unsigned b = 0; b < bytes; b++)
        r |= ((x >> (8 * b)) & 0xff) << (8 * (bytes - 1 - b));
    return r;
}

// One single-width integer op on SEW-bit operands (already truncated to SEW,
// a = vs2[i], b = vs1[i] / rs1 / imm, d = vd[i] for the multiply-adds).
// Returns false if (funct3, funct6) is not one.
static bool int_op(unsigned f3, unsigned f6, unsigned sew, uint64_t a, uint64_t b, uint64_t d,
        unsigned vxrm, bool *sat, uint64_t *out) {
    int64_t sa = sext(a, sew), sb = sext(b, sew), sd = sext(d, sew);
    unsigned sh = (unsigned) (b & (sew - 1));
    uint64_t r;
    bool opi = f3 == 0 || f3 == 3 || f3 == 4, opm = f3 == 2 || f3 == 6;
    if (opi) {
        switch (f6) {
        case 0x00: r = a + b; break;                          // vadd
        case 0x01: if (f3 == 3) return false; r = a & ~b; break; // vandn (Zvbb)
        case 0x02: if (f3 == 3) return false; r = a - b; break; // vsub
        case 0x03: if (f3 == 0) return false; r = b - a; break; // vrsub
        case 0x04: if (f3 == 3) return false; r = a < b ? a : b; break; // vminu
        case 0x05: if (f3 == 3) return false; r = sa < sb ? a : b; break; // vmin
        case 0x06: if (f3 == 3) return false; r = a > b ? a : b; break; // vmaxu
        case 0x07: if (f3 == 3) return false; r = sa > sb ? a : b; break; // vmax
        case 0x09: r = a & b; break;
        case 0x0a: r = a | b; break;
        case 0x0b: r = a ^ b; break;
        case 0x14: case 0x15: // vror (VV/VX: 0x14, VI: 0x14/0x15 with imm bit 5), vrol (VV/VX 0x15)
            if (f6 == 0x15 && f3 != 3) { // vrol
                r = sh ? (a << sh | a >> (sew - sh)) : a;
            } else {
                unsigned s = f3 == 3 ? (unsigned) ((b & 31) | (f6 & 1) << 5) & (sew - 1) : sh;
                r = s ? (a >> s | a << (sew - s)) : a;
            }
            break;
        case 0x20: r = sat_u((unsigned __int128) a + b, sew, sat); break;          // vsaddu
        case 0x21: r = (uint64_t) sat_s((__int128) sa + sb, sew, sat); break;       // vsadd
        case 0x22: if (f3 == 3) return false; r = a < b ? (*sat = true, 0) : a - b; break; // vssubu
        case 0x23: if (f3 == 3) return false; r = (uint64_t) sat_s((__int128) sa - sb, sew, sat); break; // vssub
        case 0x25: r = a << sh; break;                                              // vsll
        case 0x27: { // vsmul
            if (f3 == 3) return false;
            if (sa == sb && sa == INT64_MIN >> (64 - sew)) { *sat = true; r = (uint64_t) ((1ull << (sew - 1)) - 1); break; }
            __int128 p = (__int128) sa * sb;
            r = (uint64_t) roundoff_s(p, sew - 1, vxrm);
            break;
        }
        case 0x28: r = a >> sh; break;                                              // vsrl
        case 0x29: r = (uint64_t) (sa >> sh); break;                                // vsra
        case 0x2a: r = roundoff_u(a, sh, vxrm); break;                              // vssrl
        case 0x2b: r = (uint64_t) roundoff_s(sa, sh, vxrm); break;                  // vssra
        default: return false;
        }
    } else if (opm) {
        switch (f6) {
        case 0x08: r = (uint64_t) ((((unsigned __int128) a + b) + round_inc((unsigned __int128) a + b, 1, vxrm)) >> 1); break; // vaaddu
        case 0x09: r = (uint64_t) roundoff_s((__int128) sa + sb, 1, vxrm); break;  // vaadd
        case 0x0a: r = (uint64_t) roundoff_s((__int128) a - (__int128) b, 1, vxrm); break; // vasubu
        case 0x0b: r = (uint64_t) roundoff_s((__int128) sa - sb, 1, vxrm); break;  // vasub
        case 0x20: r = b == 0 ? UINT64_MAX : a / b; break;                           // vdivu
        case 0x21: r = sb == 0 ? UINT64_MAX : (sb == -1 && sa == sext(1ull << (sew - 1), sew)) ? a : (uint64_t) (sa / sb); break; // vdiv
        case 0x22: r = b == 0 ? a : a % b; break;                                     // vremu
        case 0x23: r = sb == 0 ? a : sb == -1 ? 0 : (uint64_t) (sa % sb); break;      // vrem
        case 0x24: r = (uint64_t) (((unsigned __int128) a * b) >> sew); break;         // vmulhu
        case 0x25: r = a * b; break;                                                  // vmul
        case 0x26: r = (uint64_t) (((__int128) sa * (__int128) (unsigned __int128) b) >> sew); break; // vmulhsu
        case 0x27: r = (uint64_t) (((__int128) sa * sb) >> sew); break;                // vmulh
        case 0x29: r = a * d + b; break;  // vmadd: vd = vs1*vd + vs2 -> (b*d)+a; fixed below
        case 0x2b: r = 0; break;          // vnmsub (below)
        case 0x2d: r = 0; break;          // vmacc
        case 0x2f: r = 0; break;          // vnmsac
        default: return false;
        }
        // multiply-adds: vmadd vd = (vs1 * vd) + vs2; vnmsub vd = -(vs1 * vd) + vs2;
        // vmacc vd = (vs1 * vs2) + vd; vnmsac vd = -(vs1 * vs2) + vd
        if (f6 == 0x29) r = b * d + a;
        if (f6 == 0x2b) r = a - b * d;
        if (f6 == 0x2d) r = b * a + d;
        if (f6 == 0x2f) r = d - b * a;
    } else {
        return false;
    }
    (void) sd;
    *out = trunc_to(r, sew);
    return true;
}

// Operand b of element i: vs1[i] (VV), x[rs1] (VX) or the 5-bit immediate
// (VI: sign-extended, but zero-extended for shifts, vror and vwsll),
// truncated to `bits`.
static uint64_t opb(struct cpu_state *cpu, const uint8_t *snap, uint32_t insn, unsigned i,
        unsigned eb, unsigned bits, bool uimm) {
    unsigned f3 = F_FUNCT3(insn);
    if (f3 == 0 || f3 == 2)
        return eget(snap, F_RS1(insn), i, eb);
    if (f3 == 3)
        return trunc_to(uimm ? F_RS1(insn) : (uint64_t) F_SIMM5(insn), bits);
    return trunc_to(cpu->riscv64_regs[F_RS1(insn)], bits);
}

static int lmul_regs(int log2) { return log2 > 0 ? 1 << log2 : 1; }

// The commonest unmasked single-width integer ops as typed loops, the op and
// the width chosen once rather than per element. False if not one of them.
#define FAST_INT_LOOP(T, expr) do { \
    T *d_ = (T *) (vreg(cpu) + vd * VLENB); \
    const T *a_ = (const T *) (snap + vs2 * VLENB); \
    const T *b_ = (const T *) (snap + vs1 * VLENB); \
    T s_ = (T) scalar; \
    for (uint64_t i = vstart; i < vl; i++) { \
        T x = a_[i], y = vv ? b_[i] : s_; (void) y; \
        d_[i] = (T) (expr); \
    } } while (0)
#define FAST_INT_SEW(expr) do { \
    switch (c.sew) { \
    case 8: FAST_INT_LOOP(uint8_t, expr); break; \
    case 16: FAST_INT_LOOP(uint16_t, expr); break; \
    case 32: FAST_INT_LOOP(uint32_t, expr); break; \
    default: FAST_INT_LOOP(uint64_t, expr); break; \
    } } while (0)
static bool fast_int(struct cpu_state *cpu, uint32_t insn, const uint8_t *snap, struct vcfg c) {
    unsigned f3 = F_FUNCT3(insn), f6 = F_FUNCT6(insn);
    unsigned vd = F_VD(insn), vs1 = F_RS1(insn), vs2 = F_RS2(insn);
    uint64_t vl = cpu->riscv64_vl, vstart = cpu->riscv64_vstart;
    bool vv = f3 == 0 || f3 == 2;
    uint64_t scalar = f3 == 3 ? (uint64_t) F_SIMM5(insn) : cpu->riscv64_regs[vs1];
    unsigned shmask = c.sew - 1;
    if (f3 == 0 || f3 == 3 || f3 == 4) {
        switch (f6) {
        case 0x00: FAST_INT_SEW(x + y); return true;
        case 0x02: if (f3 == 3) return false; FAST_INT_SEW(x - y); return true;
        case 0x03: if (f3 == 0) return false; FAST_INT_SEW(y - x); return true;
        case 0x09: FAST_INT_SEW(x & y); return true;
        case 0x0a: FAST_INT_SEW(x | y); return true;
        case 0x0b: FAST_INT_SEW(x ^ y); return true;
        case 0x25: if (f3 == 3) scalar = F_RS1(insn); FAST_INT_SEW(x << (y & shmask)); return true;
        case 0x28: if (f3 == 3) scalar = F_RS1(insn); FAST_INT_SEW(x >> (y & shmask)); return true;
        }
        return false;
    }
    if (f3 == 2 || f3 == 6) {
        if (f6 == 0x25) { FAST_INT_SEW(x * y); return true; } // vmul
    }
    return false;
}

// OPIVV/OPIVX/OPIVI (funct3 0/4/3) and OPMVV/OPMVX (2/6).
static int vint(struct cpu_state *cpu, uint32_t insn, const uint8_t *snap, struct vcfg c) {
    unsigned f3 = F_FUNCT3(insn), f6 = F_FUNCT6(insn), vm = F_VM(insn);
    unsigned vd = F_VD(insn), vs1 = F_RS1(insn), vs2 = F_RS2(insn);
    unsigned sew = c.sew, eb = c.sewb;
    uint64_t vl = cpu->riscv64_vl, vstart = cpu->riscv64_vstart;
    uint8_t *file = vreg(cpu);
    bool opi = f3 == 0 || f3 == 3 || f3 == 4, opm = f3 == 2 || f3 == 6;
    bool vv = f3 == 0 || f3 == 2;
    bool sat = false;
    int L = c.lmul_log2;
#define ACTIVE(i) (vm || mbit(snap, 0, (i)))
#define DONE() do { cpu->riscv64_vstart = 0; if (sat) cpu->riscv64_vxsat = 1; return INT_NONE; } while (0)

    // -- moves and merges: vmv.v.* (vm=1), vmerge (vm=0); vmv<nr>r.v --
    if (opi && f6 == 0x17) {
        if (!group_ok(vd, L) || (!vm && vd == 0))
            return INT_UNDEFINED;
        for (uint64_t i = vstart; i < vl; i++) {
            uint64_t b = opb(cpu, snap, insn, (unsigned) i, eb, sew, false);
            uint64_t x = vm || mbit(snap, 0, (unsigned) i) ? b : eget(snap, vs2, (unsigned) i, eb);
            eset(file, vd, (unsigned) i, eb, x);
        }
        DONE();
    }
    if (f3 == 3 && f6 == 0x27) { // vmv<nr>r.v: whole registers, vtype-independent count
        unsigned nr = F_RS1(insn) + 1;
        if ((nr != 1 && nr != 2 && nr != 4 && nr != 8) || vd % nr || vs2 % nr)
            return INT_UNDEFINED;
        memmove(file + vd * VLENB + vstart * eb, snap + vs2 * VLENB + vstart * eb,
               nr * VLENB > vstart * eb ? nr * VLENB - vstart * eb : 0);
        DONE();
    }

    // -- mask-producing compares and carries --
    if (opi && f6 >= 0x18 && f6 <= 0x1f) { // vmseq .. vmsgt
        if (!group_ok(vs2, L))
            return INT_UNDEFINED;
        for (uint64_t i = vstart; i < vl; i++) {
            if (!ACTIVE(i))
                continue;
            uint64_t a = eget(snap, vs2, (unsigned) i, eb), b = opb(cpu, snap, insn, (unsigned) i, eb, sew, false);
            int64_t sa = sext(a, sew), sb = sext(b, sew);
            bool r;
            switch (f6) {
            case 0x18: r = a == b; break;
            case 0x19: r = a != b; break;
            case 0x1a: if (f3 == 3) return INT_UNDEFINED; r = a < b; break;
            case 0x1b: if (f3 == 3) return INT_UNDEFINED; r = sa < sb; break;
            case 0x1c: r = a <= b; break;
            case 0x1d: r = sa <= sb; break;
            case 0x1e: if (vv) return INT_UNDEFINED; r = a > b; break;
            default: if (vv) return INT_UNDEFINED; r = sa > sb; break;
            }
            mset(file, vd, (unsigned) i, r);
        }
        DONE();
    }
    if (opi && f6 >= 0x10 && f6 <= 0x13) { // vadc vmadc vsbc vmsbc
        bool sub = f6 >= 0x12, mask_out = f6 & 1;
        if ((f6 == 0x10 || f6 == 0x12) && (vm || vd == 0))
            return INT_UNDEFINED; // vadc/vsbc need vm=0 and vd != v0
        if (sub && f3 == 3)
            return INT_UNDEFINED;
        for (uint64_t i = vstart; i < vl; i++) {
            uint64_t a = eget(snap, vs2, (unsigned) i, eb), b = opb(cpu, snap, insn, (unsigned) i, eb, sew, false);
            unsigned carry = vm ? 0 : mbit(snap, 0, (unsigned) i);
            unsigned __int128 wide = sub ? (unsigned __int128) a - b - carry : (unsigned __int128) a + b + carry;
            if (mask_out)
                mset(file, vd, (unsigned) i, (wide >> sew) & 1);
            else
                eset(file, vd, (unsigned) i, eb, trunc_to((uint64_t) wide, sew));
        }
        DONE();
    }

    // -- narrowing: vnsrl vnsra vnclipu vnclip (vs2 is 2*SEW) --
    if (opi && f6 >= 0x2c && f6 <= 0x2f) {
        if (sew == 64 || !group_ok(vs2, L + 1) || !group_ok(vd, L))
            return INT_UNDEFINED;
        for (uint64_t i = vstart; i < vl; i++) {
            if (!ACTIVE(i))
                continue;
            uint64_t a = eget(snap, vs2, (unsigned) i, eb * 2);
            unsigned sh = (unsigned) (opb(cpu, snap, insn, (unsigned) i, eb, sew, true) & (2 * sew - 1));
            uint64_t r;
            switch (f6) {
            case 0x2c: r = a >> sh; break;
            case 0x2d: r = (uint64_t) (sext(a, 2 * sew) >> sh); break;
            case 0x2e: r = sat_u(roundoff_u(a, sh, cpu->riscv64_vxrm), sew, &sat); break;
            default: r = (uint64_t) sat_s(roundoff_s(sext(a, 2 * sew), sh, cpu->riscv64_vxrm), sew, &sat); break;
            }
            eset(file, vd, (unsigned) i, eb, trunc_to(r, sew));
        }
        DONE();
    }

    // -- widening integer arithmetic (OPM 0x30-0x3f), vwsll (OPI 0x35) --
    if ((opm && f6 >= 0x30) || (opi && f6 == 0x35)) {
        if (sew == 64 || !group_ok(vd, L + 1))
            return INT_UNDEFINED;
        bool wide_a = opm && f6 >= 0x34 && f6 <= 0x37; // .w forms: vs2 is already 2*SEW
        if (!group_ok(vs2, wide_a ? L + 1 : L))
            return INT_UNDEFINED;
        if (opm && (f6 == 0x39 || (f6 == 0x3e && vv)))
            return INT_UNDEFINED;
        unsigned w = 2 * sew;
        for (uint64_t i = vstart; i < vl; i++) {
            if (!ACTIVE(i))
                continue;
            uint64_t a = eget(snap, vs2, (unsigned) i, wide_a ? 2 * eb : eb);
            uint64_t b = opb(cpu, snap, insn, (unsigned) i, eb, sew, opi);
            uint64_t d = eget(snap, vd, (unsigned) i, 2 * eb);
            uint64_t ua = a, ub = b;
            int64_t sa = sext(a, wide_a ? w : sew), sb = sext(b, sew);
            uint64_t r;
            if (opi) { // vwsll: zero-extend vs2, shift by b mod 2*SEW
                r = ua << (b & (w - 1));
            } else switch (f6) {
            case 0x30: case 0x34: r = ua + ub; break;                    // vwaddu(.w)
            case 0x31: case 0x35: r = (uint64_t) (sa + sb); break;       // vwadd(.w)
            case 0x32: case 0x36: r = ua - ub; break;                    // vwsubu(.w)
            case 0x33: case 0x37: r = (uint64_t) (sa - sb); break;       // vwsub(.w)
            case 0x38: r = ua * ub; break;                               // vwmulu
            case 0x3a: r = (uint64_t) (sa * (int64_t) ub); break;        // vwmulsu: vs2 signed, vs1 unsigned
            case 0x3b: r = (uint64_t) (sa * sb); break;                  // vwmul
            case 0x3c: r = ub * ua + d; break;                           // vwmaccu
            case 0x3d: r = (uint64_t) (sb * sa) + d; break;              // vwmacc
            case 0x3e: r = (uint64_t) ((int64_t) ub * sa) + d; break;    // vwmaccus: rs1 unsigned, vs2 signed
            default:   r = (uint64_t) (sb * (int64_t) ua) + d; break;    // vwmaccsu: vs1 signed, vs2 unsigned
            }
            eset(file, vd, (unsigned) i, 2 * eb, trunc_to(r, w));
        }
        DONE();
    }

    // -- reductions: OPMVV 0x00-0x07, OPIVV 0x30/0x31 (widening sums) --
    if ((f3 == 2 && f6 <= 0x07) || (f3 == 0 && (f6 == 0x30 || f6 == 0x31))) {
        if (vstart != 0)
            return INT_UNDEFINED;
        bool widen = f3 == 0;
        if (widen && sew == 64)
            return INT_UNDEFINED;
        unsigned rb = widen ? 2 * eb : eb, rbits = rb * 8;
        uint64_t acc = eget(snap, vs1, 0, rb);
        for (uint64_t i = 0; i < vl; i++) {
            if (!ACTIVE(i))
                continue;
            uint64_t x = eget(snap, vs2, (unsigned) i, eb);
            if (widen) {
                x = f6 == 0x31 ? trunc_to((uint64_t) sext(x, sew), rbits) : x;
                acc = trunc_to(acc + x, rbits);
                continue;
            }
            int64_t sx = sext(x, sew), sacc = sext(acc, sew);
            switch (f6) {
            case 0: acc = trunc_to(acc + x, sew); break;
            case 1: acc &= x; break;
            case 2: acc |= x; break;
            case 3: acc ^= x; break;
            case 4: acc = x < acc ? x : acc; break;
            case 5: acc = sx < sacc ? x : acc; break;
            case 6: acc = x > acc ? x : acc; break;
            default: acc = sx > sacc ? x : acc; break;
            }
        }
        if (vl > 0)
            eset(file, vd, 0, rb, acc);
        DONE();
    }

    // -- permutations --
    if (opi && (f6 == 0x0c || (f6 == 0x0e && f3 == 0))) { // vrgather.vv/vx/vi, vrgatherei16.vv
        bool ei16 = f6 == 0x0e;
        unsigned ieb = ei16 ? 2 : eb;
        for (uint64_t i = vstart; i < vl; i++) {
            if (!ACTIVE(i))
                continue;
            uint64_t idx = vv ? eget(snap, vs1, (unsigned) i, ieb)
                          : f3 == 3 ? F_RS1(insn) : cpu->riscv64_regs[F_RS1(insn)];
            uint64_t x = idx >= c.vlmax ? 0 : eget(snap, vs2, (unsigned) idx, eb);
            eset(file, vd, (unsigned) i, eb, x);
        }
        DONE();
    }
    if (opi && (f6 == 0x0e || f6 == 0x0f) && !vv) { // vslideup / vslidedown .vx/.vi
        uint64_t off = f3 == 3 ? F_RS1(insn) : cpu->riscv64_regs[F_RS1(insn)];
        for (uint64_t i = vstart; i < vl; i++) {
            if (!ACTIVE(i))
                continue;
            if (f6 == 0x0e) {
                if (i < off)
                    continue; // vslideup leaves vd[0..off) alone
                eset(file, vd, (unsigned) i, eb, eget(snap, vs2, (unsigned) (i - off), eb));
            } else {
                uint64_t src = i + off;
                eset(file, vd, (unsigned) i, eb, src < c.vlmax && src >= i ? eget(snap, vs2, (unsigned) src, eb) : 0);
            }
        }
        DONE();
    }
    if (f3 == 6 && (f6 == 0x0e || f6 == 0x0f)) { // vslide1up / vslide1down .vx
        uint64_t x = trunc_to(cpu->riscv64_regs[F_RS1(insn)], sew);
        for (uint64_t i = vstart; i < vl; i++) {
            if (!ACTIVE(i))
                continue;
            uint64_t v;
            if (f6 == 0x0e)
                v = i == 0 ? x : eget(snap, vs2, (unsigned) (i - 1), eb);
            else
                v = i + 1 == vl ? x : eget(snap, vs2, (unsigned) (i + 1), eb);
            eset(file, vd, (unsigned) i, eb, v);
        }
        DONE();
    }
    if (f3 == 2 && f6 == 0x17) { // vcompress.vm
        if (!vm || vstart != 0)
            return INT_UNDEFINED;
        unsigned k = 0;
        for (uint64_t i = 0; i < vl; i++)
            if (mbit(snap, vs1, (unsigned) i))
                eset(file, vd, k++, eb, eget(snap, vs2, (unsigned) i, eb));
        DONE();
    }
    if (f3 == 2 && f6 == 0x10) { // VWXUNARY0: vmv.x.s, vcpop.m, vfirst.m
        unsigned rd = vd;
        uint64_t r;
        if (vs1 == 0) {
            r = (uint64_t) sext(eget(snap, vs2, 0, eb), sew);
        } else if (vs1 == 0x10 || vs1 == 0x11) {
            if (vstart != 0)
                return INT_UNDEFINED;
            uint64_t n = 0;
            int64_t first = -1;
            for (uint64_t i = 0; i < vl; i++) {
                if (!ACTIVE(i) || !mbit(snap, vs2, (unsigned) i))
                    continue;
                n++;
                if (first < 0)
                    first = (int64_t) i;
            }
            r = vs1 == 0x10 ? n : (uint64_t) first;
        } else {
            return INT_UNDEFINED;
        }
        if (rd != 0)
            cpu->riscv64_regs[rd] = r;
        cpu->riscv64_vstart = 0;
        return INT_NONE;
    }
    if (f3 == 6 && f6 == 0x10) { // VRXUNARY0: vmv.s.x
        if (vs2 != 0)
            return INT_UNDEFINED;
        if (vstart < vl)
            eset(file, vd, 0, eb, trunc_to(cpu->riscv64_regs[F_RS1(insn)], sew));
        DONE();
    }
    if (f3 == 2 && f6 == 0x12) { // VXUNARY0: vzext/vsext .vf2/4/8; Zvbb unary ops
        unsigned op = vs1;
        if (op >= 2 && op <= 7) {
            unsigned frac = op <= 3 ? 8 : op <= 5 ? 4 : 2;
            bool sx = op & 1;
            if (sew / frac < 8)
                return INT_UNDEFINED;
            unsigned seb = eb / frac;
            for (uint64_t i = vstart; i < vl; i++) {
                if (!ACTIVE(i))
                    continue;
                uint64_t x = eget(snap, vs2, (unsigned) i, seb);
                eset(file, vd, (unsigned) i, eb, trunc_to(sx ? (uint64_t) sext(x, seb * 8) : x, sew));
            }
            DONE();
        }
        if (op < 8 || op > 14 || op == 11)
            return INT_UNDEFINED;
        for (uint64_t i = vstart; i < vl; i++) {
            if (!ACTIVE(i))
                continue;
            uint64_t x = eget(snap, vs2, (unsigned) i, eb), r = 0;
            switch (op) {
            case 8: r = brev8(x, eb); break;                                   // vbrev8
            case 9: r = rev8(x, eb); break;                                    // vrev8
            case 10: for (unsigned k = 0; k < sew; k++) r |= ((x >> k) & 1) << (sew - 1 - k); break; // vbrev
            case 12: r = x == 0 ? sew : (unsigned) __builtin_clzll(x) - (64 - sew); break;          // vclz
            case 13: r = x == 0 ? sew : (unsigned) __builtin_ctzll(x); break;                       // vctz
            default: r = (unsigned) __builtin_popcountll(x); break;                                // vcpop.v
            }
            eset(file, vd, (unsigned) i, eb, r);
        }
        DONE();
    }
    if (f3 == 2 && f6 == 0x14) { // VMUNARY0: vmsbf vmsof vmsif viota vid
        unsigned op = vs1;
        if (op == 0x11) { // vid.v
            for (uint64_t i = vstart; i < vl; i++)
                if (ACTIVE(i))
                    eset(file, vd, (unsigned) i, eb, trunc_to(i, sew));
            DONE();
        }
        if (vstart != 0)
            return INT_UNDEFINED;
        if (op == 0x10) { // viota.m
            uint64_t n = 0;
            for (uint64_t i = 0; i < vl; i++) {
                if (!ACTIVE(i))
                    continue;
                eset(file, vd, (unsigned) i, eb, trunc_to(n, sew));
                n += mbit(snap, vs2, (unsigned) i);
            }
            DONE();
        }
        if (op != 1 && op != 2 && op != 3)
            return INT_UNDEFINED;
        bool seen = false;
        for (uint64_t i = 0; i < vl; i++) {
            if (!ACTIVE(i))
                continue;
            bool b = mbit(snap, vs2, (unsigned) i), r;
            if (op == 1) r = !seen && !b;            // vmsbf: before first
            else if (op == 2) r = !seen && b;        // vmsof: only first
            else r = !seen;                          // vmsif: including first
            if (b)
                seen = true;
            mset(file, vd, (unsigned) i, r);
        }
        DONE();
    }
    if (f3 == 2 && f6 >= 0x18 && f6 <= 0x1f) { // mask logicals (vl bits)
        for (uint64_t i = vstart; i < vl; i++) {
            bool a = mbit(snap, vs2, (unsigned) i), b = mbit(snap, vs1, (unsigned) i), r;
            switch (f6) {
            case 0x18: r = a && !b; break;   // vmandn
            case 0x19: r = a && b; break;
            case 0x1a: r = a || b; break;
            case 0x1b: r = a != b; break;
            case 0x1c: r = a || !b; break;   // vmorn
            case 0x1d: r = !(a && b); break;
            case 0x1e: r = !(a || b); break;
            default: r = a == b; break;      // vmxnor
            }
            mset(file, vd, (unsigned) i, r);
        }
        DONE();
    }

    // -- everything else is single-width, element-wise --
    if (!group_ok(vd, L) || !group_ok(vs2, L) || (vv && !group_ok(vs1, L)))
        return INT_UNDEFINED;
    if (vm && vstart < vl && fast_int(cpu, insn, snap, c))
        DONE();
    bool uimm = f3 == 3 && (f6 == 0x25 || f6 == 0x28 || f6 == 0x29 || f6 == 0x2a || f6 == 0x2b ||
                            f6 == 0x14 || f6 == 0x15);
    for (uint64_t i = vstart; i < vl; i++) {
        if (!ACTIVE(i))
            continue;
        uint64_t a = eget(snap, vs2, (unsigned) i, eb), b = opb(cpu, snap, insn, (unsigned) i, eb, sew, uimm);
        uint64_t d = eget(snap, vd, (unsigned) i, eb), r;
        if (!int_op(f3, f6, sew, a, b, d, cpu->riscv64_vxrm, &sat, &r))
            return INT_UNDEFINED;
        eset(file, vd, (unsigned) i, eb, r);
    }
    DONE();
#undef ACTIVE
#undef DONE
}

// ---- floating point ----
static inline float f32_of(uint64_t b) { uint32_t w = (uint32_t) b; float f; memcpy(&f, &w, 4); return f; }
static inline double f64_of(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static inline uint64_t bits_f32(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }
static inline uint64_t bits_f64(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
#define CANON32 0x7fc00000ull
#define CANON64 0x7ff8000000000000ull

// RISC-V fmin/fmax: the non-NaN operand, -0 below +0 = AArch64 fminnm/fmaxnm.
static float fminnm32(float a, float b) { float r; __asm__("fminnm %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b)); return r; }
static float fmaxnm32(float a, float b) { float r; __asm__("fmaxnm %s0, %s1, %s2" : "=w"(r) : "w"(a), "w"(b)); return r; }
static double fminnm64(double a, double b) { double r; __asm__("fminnm %d0, %d1, %d2" : "=w"(r) : "w"(a), "w"(b)); return r; }
static double fmaxnm64(double a, double b) { double r; __asm__("fmaxnm %d0, %d1, %d2" : "=w"(r) : "w"(a), "w"(b)); return r; }

// f[rs1] as a SEW-bit operand: a single must be NaN-boxed, else it reads as
// the canonical NaN.
static uint64_t fscalar(struct cpu_state *cpu, unsigned rs1, unsigned sew) {
    uint64_t b = cpu->riscv64_f[rs1];
    if (sew == 32)
        return (b >> 32) == 0xffffffffu ? (uint32_t) b : CANON32;
    if (sew == 16)
        return (b >> 16) == 0xffffffffffffull ? (uint16_t) b : 0x7e00;
    return b;
}

static unsigned fclass_bits(uint64_t b, unsigned sew) {
    unsigned ebits = sew == 64 ? 11 : 8, mbits = sew == 64 ? 52 : 23;
    bool sign = (b >> (sew - 1)) & 1;
    uint64_t exp = (b >> mbits) & ((1u << ebits) - 1), frac = b & ((1ull << mbits) - 1);
    uint64_t emax = (1u << ebits) - 1;
    if (exp == emax)
        return frac == 0 ? (sign ? 1u << 0 : 1u << 7) : ((frac >> (mbits - 1)) ? 1u << 9 : 1u << 8);
    if (exp == 0)
        return frac == 0 ? (sign ? 1u << 3 : 1u << 4) : (sign ? 1u << 2 : 1u << 5);
    return sign ? 1u << 1 : 1u << 6;
}

// float -> integer of `bits`, rounding in the current mode (or toward zero),
// saturating as RISC-V does, with its flags (NV alone when out of range).
static uint64_t f2i(double x, bool sgn, unsigned bits, bool rtz) {
    if (isnan(x)) {
        feraiseexcept(FE_INVALID);
        return sgn ? (uint64_t) (((uint64_t) 1 << (bits - 1)) - 1) : trunc_to(UINT64_MAX, bits);
    }
    double r = rtz ? trunc(x) : nearbyint(x);
    double lo = sgn ? -ldexp(1.0, (int) bits - 1) : 0.0;
    double hi = sgn ? ldexp(1.0, (int) bits - 1) : ldexp(1.0, (int) bits); // exclusive
    if (r < lo || r >= hi) {
        feraiseexcept(FE_INVALID);
        if (sgn)
            return trunc_to(r < lo ? (uint64_t) 1 << (bits - 1) : ((uint64_t) 1 << (bits - 1)) - 1, bits);
        return r < lo ? 0 : trunc_to(UINT64_MAX, bits);
    }
    if (r != x)
        feraiseexcept(FE_INEXACT);
    if (sgn)
        return trunc_to((uint64_t) (int64_t) r, bits);
    return (uint64_t) r;
}

// integer of `bits` -> float of `fsew`, in the current rounding mode
static uint64_t i2f(uint64_t x, bool sgn, unsigned bits, unsigned fsew) {
    if (sgn) {
        int64_t s = sext(x, bits);
        if (fsew == 64) return bits_f64((double) s);
        if (fsew == 32) return bits_f32((float) s);
        _Float16 h = (_Float16) s; uint16_t w; memcpy(&w, &h, 2); return w;
    }
    if (fsew == 64) return bits_f64((double) x);
    if (fsew == 32) return bits_f32((float) x);
    _Float16 h = (_Float16) x; uint16_t w; memcpy(&w, &h, 2); return w;
}

// a float of `sew` bits (16/32/64) as a double (exact)
static double fval(uint64_t b, unsigned sew) {
    if (sew == 64) return f64_of(b);
    if (sew == 32) return (double) f32_of(b);
    _Float16 h; uint16_t w = (uint16_t) b; memcpy(&h, &w, 2); return (double) h;
}
// a double rounded to `sew` bits in the current mode; NaN canonical
static uint64_t fto(double d, unsigned sew) {
    if (sew == 64) return isnan(d) ? CANON64 : bits_f64(d);
    if (sew == 32) return isnan(d) ? CANON32 : bits_f32((float) d);
    if (isnan(d)) return 0x7e00;
    _Float16 h = (_Float16) d; uint16_t w; memcpy(&w, &h, 2); return w;
}
// round-to-odd narrowing (vfncvt.rod.f.f.w): truncate, then set the result's
// last bit if anything was dropped
static uint64_t fto_rod(double d, unsigned sew) {
    if (isnan(d))
        return sew == 32 ? CANON32 : 0x7e00;
    int old = fegetround();
    fesetround(FE_TOWARDZERO);
    uint64_t r = fto(d, sew);
    fesetround(old);
    if (!isinf(d) && fval(r, sew) != d)
        r |= 1;
    return r;
}

static bool fp_sew_ok(unsigned sew) { return sew == 32 || sew == 64; }

// single-width FP op on raw SEW-bit operands; a = vs2[i], b = vs1[i]/f[rs1],
// d = vd[i]
static bool fp_op(unsigned f6, bool vf, unsigned sew, uint64_t a, uint64_t b, uint64_t d, uint64_t *out) {
    if (sew == 32) {
        float x = f32_of(a), y = f32_of(b), z = f32_of(d), r;
        switch (f6) {
        case 0x00: r = x + y; break;
        case 0x02: r = x - y; break;
        case 0x04: r = fminnm32(x, y); break;
        case 0x06: r = fmaxnm32(x, y); break;
        case 0x08: *out = (a & 0x7fffffff) | (b & 0x80000000); return true;
        case 0x09: *out = (a & 0x7fffffff) | (~b & 0x80000000); return true;
        case 0x0a: *out = a ^ (b & 0x80000000); return true;
        case 0x20: r = x / y; break;
        case 0x21: if (!vf) return false; r = y / x; break;
        case 0x24: r = x * y; break;
        case 0x27: if (!vf) return false; r = y - x; break;
        case 0x28: r = fmaf(y, z, x); break;    // vfmadd  vd = vs1*vd + vs2
        case 0x29: r = fmaf(-y, z, -x); break;  // vfnmadd vd = -(vs1*vd) - vs2
        case 0x2a: r = fmaf(y, z, -x); break;   // vfmsub  vd = vs1*vd - vs2
        case 0x2b: r = fmaf(-y, z, x); break;   // vfnmsub vd = -(vs1*vd) + vs2
        case 0x2c: r = fmaf(y, x, z); break;    // vfmacc  vd = vs1*vs2 + vd
        case 0x2d: r = fmaf(-y, x, -z); break;  // vfnmacc
        case 0x2e: r = fmaf(y, x, -z); break;   // vfmsac
        case 0x2f: r = fmaf(-y, x, z); break;   // vfnmsac
        default: return false;
        }
        *out = isnan(r) ? CANON32 : bits_f32(r);
        return true;
    }
    double x = f64_of(a), y = f64_of(b), z = f64_of(d), r;
    switch (f6) {
    case 0x00: r = x + y; break;
    case 0x02: r = x - y; break;
    case 0x04: r = fminnm64(x, y); break;
    case 0x06: r = fmaxnm64(x, y); break;
    case 0x08: *out = (a & ~(1ull << 63)) | (b & (1ull << 63)); return true;
    case 0x09: *out = (a & ~(1ull << 63)) | (~b & (1ull << 63)); return true;
    case 0x0a: *out = a ^ (b & (1ull << 63)); return true;
    case 0x20: r = x / y; break;
    case 0x21: if (!vf) return false; r = y / x; break;
    case 0x24: r = x * y; break;
    case 0x27: if (!vf) return false; r = y - x; break;
    case 0x28: r = fma(y, z, x); break;
    case 0x29: r = fma(-y, z, -x); break;
    case 0x2a: r = fma(y, z, -x); break;
    case 0x2b: r = fma(-y, z, x); break;
    case 0x2c: r = fma(y, x, z); break;
    case 0x2d: r = fma(-y, x, -z); break;
    case 0x2e: r = fma(y, x, -z); break;
    case 0x2f: r = fma(-y, x, z); break;
    default: return false;
    }
    *out = isnan(r) ? CANON64 : bits_f64(r);
    return true;
}

// OPFVV (funct3 1) and OPFVF (5)
static int vfp(struct cpu_state *cpu, uint32_t insn, const uint8_t *snap, struct vcfg c) {
    unsigned f3 = F_FUNCT3(insn), f6 = F_FUNCT6(insn), vm = F_VM(insn);
    unsigned vd = F_VD(insn), vs1 = F_RS1(insn), vs2 = F_RS2(insn);
    unsigned sew = c.sew, eb = c.sewb;
    bool vf = f3 == 5;
    uint64_t vl = cpu->riscv64_vl, vstart = cpu->riscv64_vstart;
    uint8_t *file = vreg(cpu);
    int L = c.lmul_log2;
#define ACTIVE(i) (vm || mbit(snap, 0, (i)))
#define DONE() do { cpu->riscv64_vstart = 0; return INT_NONE; } while (0)
#define B(i) (vf ? fscalar(cpu, vs1, sew) : eget(snap, vs1, (i), eb))

    // conversions (VFUNARY0) come first: some are legal at SEW 16
    if (!vf && f6 == 0x12) {
        unsigned op = vs1;
        if (op <= 7 && op != 4 && op != 5) { // single-width
            if (!fp_sew_ok(sew))
                return INT_UNDEFINED;
            for (uint64_t i = vstart; i < vl; i++) {
                if (!ACTIVE(i))
                    continue;
                uint64_t a = eget(snap, vs2, (unsigned) i, eb), r;
                if (op == 2 || op == 3)
                    r = i2f(a, op == 3, sew, sew);
                else
                    r = f2i(fval(a, sew), op & 1, sew, op >= 6);
                eset(file, vd, (unsigned) i, eb, r);
            }
            DONE();
        }
        if (op >= 8 && op <= 15 && op != 13) { // widening: SEW -> 2*SEW
            bool ff = op == 12, from_int = op == 10 || op == 11;
            if (sew == 64 || !group_ok(vd, L + 1))
                return INT_UNDEFINED;
            if (ff && sew != 16 && sew != 32)
                return INT_UNDEFINED;
            if (!ff && !from_int && sew != 32)
                return INT_UNDEFINED; // float -> int from f16 needs Zvfh
            if (from_int && sew == 8)
                return INT_UNDEFINED; // int8 -> f16 needs Zvfh
            for (uint64_t i = vstart; i < vl; i++) {
                if (!ACTIVE(i))
                    continue;
                uint64_t a = eget(snap, vs2, (unsigned) i, eb), r;
                if (ff)
                    r = fto(fval(a, sew), 2 * sew); // exact
                else if (from_int)
                    r = i2f(a, op == 11, sew, 2 * sew);
                else
                    r = f2i(fval(a, sew), op & 1, 2 * sew, op >= 14);
                eset(file, vd, (unsigned) i, 2 * eb, r);
            }
            DONE();
        }
        if (op >= 16 && op <= 23) { // narrowing: 2*SEW -> SEW
            bool ff = op == 20 || op == 21, from_int = op == 18 || op == 19;
            if (sew == 64 || !group_ok(vs2, L + 1))
                return INT_UNDEFINED;
            if (ff && sew != 16 && sew != 32)
                return INT_UNDEFINED;
            if (from_int && sew != 32 && sew != 16)
                return INT_UNDEFINED; // int16 -> f8 does not exist; int32 -> f16 needs Zvfh below
            if (from_int && sew == 16)
                return INT_UNDEFINED;
            if (!ff && !from_int && sew == 8)
                return INT_UNDEFINED; // f16 -> int8 needs Zvfh
            for (uint64_t i = vstart; i < vl; i++) {
                if (!ACTIVE(i))
                    continue;
                uint64_t a = eget(snap, vs2, (unsigned) i, 2 * eb), r;
                if (op == 21)
                    r = fto_rod(fval(a, 2 * sew), sew);
                else if (ff)
                    r = fto(fval(a, 2 * sew), sew);
                else if (from_int)
                    r = i2f(a, op == 19, 2 * sew, sew);
                else
                    r = f2i(fval(a, 2 * sew), op & 1, sew, op >= 22);
                eset(file, vd, (unsigned) i, eb, r);
            }
            DONE();
        }
        return INT_UNDEFINED;
    }

    if (!fp_sew_ok(sew))
        return INT_UNDEFINED; // no Zvfh: half-precision arithmetic is reserved

    if (!vf && f6 == 0x13) { // VFUNARY1: vfsqrt vfclass (vfrsqrt7/vfrec7: not yet)
        if (vs1 != 0 && vs1 != 0x10)
            return INT_UNDEFINED;
        for (uint64_t i = vstart; i < vl; i++) {
            if (!ACTIVE(i))
                continue;
            uint64_t a = eget(snap, vs2, (unsigned) i, eb), r;
            if (vs1 == 0x10)
                r = fclass_bits(a, sew);
            else
                r = sew == 32 ? (isnan(sqrtf(f32_of(a))) ? CANON32 : bits_f32(sqrtf(f32_of(a))))
                              : (isnan(sqrt(f64_of(a))) ? CANON64 : bits_f64(sqrt(f64_of(a))));
            eset(file, vd, (unsigned) i, eb, r);
        }
        DONE();
    }
    if (f6 == 0x10) { // vfmv.f.s (VV, vs1 0) / vfmv.s.f (VF, vs2 0)
        if (!vf) {
            if (vs1 != 0)
                return INT_UNDEFINED;
            uint64_t x = eget(snap, vs2, 0, eb);
            cpu->riscv64_f[vd] = sew == 32 ? 0xffffffff00000000ull | x : x;
        } else {
            if (vs2 != 0)
                return INT_UNDEFINED;
            if (vstart < vl)
                eset(file, vd, 0, eb, fscalar(cpu, vs1, sew));
        }
        DONE();
    }
    if (vf && f6 == 0x17) { // vfmerge.vfm (vm=0) / vfmv.v.f (vm=1)
        if (!vm && vd == 0)
            return INT_UNDEFINED;
        uint64_t x = fscalar(cpu, vs1, sew);
        for (uint64_t i = vstart; i < vl; i++)
            eset(file, vd, (unsigned) i, eb, vm || mbit(snap, 0, (unsigned) i) ? x : eget(snap, vs2, (unsigned) i, eb));
        DONE();
    }
    if (vf && (f6 == 0x0e || f6 == 0x0f)) { // vfslide1up / vfslide1down
        uint64_t x = fscalar(cpu, vs1, sew);
        for (uint64_t i = vstart; i < vl; i++) {
            if (!ACTIVE(i))
                continue;
            uint64_t v = f6 == 0x0e ? (i == 0 ? x : eget(snap, vs2, (unsigned) (i - 1), eb))
                                    : (i + 1 == vl ? x : eget(snap, vs2, (unsigned) (i + 1), eb));
            eset(file, vd, (unsigned) i, eb, v);
        }
        DONE();
    }
    if (f6 >= 0x18 && f6 <= 0x1f && f6 != 0x1a && f6 != 0x1e) { // compares into a mask
        if ((f6 == 0x1d || f6 == 0x1f) && !vf)
            return INT_UNDEFINED;
        for (uint64_t i = vstart; i < vl; i++) {
            if (!ACTIVE(i))
                continue;
            double x = fval(eget(snap, vs2, (unsigned) i, eb), sew), y = fval(B((unsigned) i), sew);
            bool r;
            switch (f6) {
            case 0x18: r = x == y; break;                                   // vmfeq (quiet)
            case 0x19: if (isnan(x) || isnan(y)) feraiseexcept(FE_INVALID); r = x <= y; break; // vmfle
            case 0x1b: if (isnan(x) || isnan(y)) feraiseexcept(FE_INVALID); r = x < y; break;  // vmflt
            case 0x1c: r = !(x == y); break;                                // vmfne (quiet)
            case 0x1d: if (isnan(x) || isnan(y)) feraiseexcept(FE_INVALID); r = x > y; break;  // vmfgt
            default: if (isnan(x) || isnan(y)) feraiseexcept(FE_INVALID); r = x >= y; break;   // vmfge
            }
            mset(file, vd, (unsigned) i, r);
        }
        DONE();
    }
    if (!vf && (f6 == 0x01 || f6 == 0x03 || f6 == 0x05 || f6 == 0x07)) { // reductions
        if (vstart != 0)
            return INT_UNDEFINED;
        uint64_t acc = eget(snap, vs1, 0, eb);
        for (uint64_t i = 0; i < vl; i++) {
            if (!ACTIVE(i))
                continue;
            uint64_t x = eget(snap, vs2, (unsigned) i, eb), r;
            unsigned op = f6 == 0x05 ? 0x04 : f6 == 0x07 ? 0x06 : 0x00; // min/max/add
            fp_op(op, false, sew, acc, x, 0, &r);
            acc = r;
        }
        if (vl > 0)
            eset(file, vd, 0, eb, acc);
        DONE();
    }
    if (!vf && (f6 == 0x31 || f6 == 0x33)) { // widening sums
        if (sew != 32 || vstart != 0)
            return INT_UNDEFINED;
        double acc = f64_of(eget(snap, vs1, 0, 8));
        for (uint64_t i = 0; i < vl; i++)
            if (ACTIVE(i))
                acc = acc + (double) f32_of(eget(snap, vs2, (unsigned) i, 4));
        if (vl > 0)
            eset(file, vd, 0, 8, isnan(acc) ? CANON64 : bits_f64(acc));
        DONE();
    }
    if (f6 >= 0x30) { // widening arithmetic: SEW 32 -> 64
        if (sew != 32 || !group_ok(vd, L + 1))
            return INT_UNDEFINED;
        bool wide_a = f6 == 0x34 || f6 == 0x36;
        for (uint64_t i = vstart; i < vl; i++) {
            if (!ACTIVE(i))
                continue;
            double a = wide_a ? f64_of(eget(snap, vs2, (unsigned) i, 8)) : (double) f32_of(eget(snap, vs2, (unsigned) i, 4));
            double b = (double) f32_of(B((unsigned) i)), d = f64_of(eget(snap, vd, (unsigned) i, 8)), r;
            switch (f6) {
            case 0x30: case 0x34: r = a + b; break;
            case 0x32: case 0x36: r = a - b; break;
            case 0x38: r = a * b; break;
            case 0x3c: r = fma(b, a, d); break;
            case 0x3d: r = fma(-b, a, -d); break;
            case 0x3e: r = fma(b, a, -d); break;
            case 0x3f: r = fma(-b, a, d); break;
            default: return INT_UNDEFINED;
            }
            eset(file, vd, (unsigned) i, 8, isnan(r) ? CANON64 : bits_f64(r));
        }
        DONE();
    }
    // single-width element-wise
    if (!group_ok(vd, L) || !group_ok(vs2, L) || (!vf && !group_ok(vs1, L)))
        return INT_UNDEFINED;
    for (uint64_t i = vstart; i < vl; i++) {
        if (!ACTIVE(i))
            continue;
        uint64_t r;
        if (!fp_op(f6, vf, sew, eget(snap, vs2, (unsigned) i, eb), B((unsigned) i),
                   eget(snap, vd, (unsigned) i, eb), &r))
            return INT_UNDEFINED;
        eset(file, vd, (unsigned) i, eb, r);
    }
    DONE();
#undef ACTIVE
#undef DONE
#undef B
}

// ---- entry ----
// Whether an OP-V instruction must read its operands from a copy of the
// register file: where a destination may overlap a source of another element
// width (widening, narrowing, extension, conversion) or another element
// index (permutations). Element-wise ops of one width read element i before
// writing it, and memory ops write only the destination, so they read the
// live file and skip the copy.
static bool needs_snapshot(uint32_t insn) {
    unsigned f3 = F_FUNCT3(insn), f6 = F_FUNCT6(insn);
    bool opi = f3 == 0 || f3 == 3 || f3 == 4, opm = f3 == 2 || f3 == 6, opf = f3 == 1 || f3 == 5;
    if (opi)
        return f6 == 0x0c || f6 == 0x0e || f6 == 0x0f || (f6 >= 0x2c && f6 <= 0x2f) ||
               f6 == 0x30 || f6 == 0x31 || f6 == 0x35;
    if (opm)
        return f6 == 0x0e || f6 == 0x0f || f6 == 0x12 || f6 == 0x14 || f6 == 0x17 || f6 >= 0x30;
    if (opf)
        return f6 == 0x0e || f6 == 0x0f || f6 == 0x12 || f6 >= 0x30;
    return true;
}

int riscv64_vector_exec(struct cpu_state *cpu, struct tlb *tlb, uint32_t insn) {
    unsigned opcode = insn & 0x7f;
    if (opcode == 0x73)
        return vcsr_op(cpu, insn);
    if (opcode == 0x57 && F_FUNCT3(insn) == 7)
        return vsetvl(cpu, insn);
    if (opcode == 0x07 || opcode == 0x27)
        return vmem(cpu, tlb, insn, vreg(cpu));
    uint8_t copy[32 * VLENB];
    const uint8_t *snap = vreg(cpu);
    if (needs_snapshot(insn)) {
        memcpy(copy, vreg(cpu), sizeof(copy));
        snap = copy;
    }
    struct vcfg c = vcfg_of(cpu->riscv64_vtype);
    if (c.vill)
        return INT_UNDEFINED;
    unsigned f3 = F_FUNCT3(insn);
    if (f3 == 1 || f3 == 5)
        return vfp(cpu, insn, snap, c);
    return vint(cpu, insn, snap, c);
}
