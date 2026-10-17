// The ppc64 guest's translator: one instruction at a time, into the gadgets
// of jit/guest-ppc64/ (docs/ppc64le_guest_plan.md). The shape is
// gen_step_riscv64's (jit/gen.c): operands are byte offsets into cpu_state,
// a memory gadget's last word is its instruction's address, a branch ends
// the block with its targets tagged for chaining, and an instruction with no
// gadget logs its encoding and raises SIGILL there -- the bring-up tool.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "debug.h"
#include "jit/gen.h"
#include "emu/cpu.h"
#include "emu/interrupt.h"
#include "emu/arch/ppc64/decode.h"

#define PPC64_GADGETS(_) \
    _(interrupt) _(sc) _(mov_const) _(b) _(bl) \
    _(add) _(subf) _(and) _(andc) _(or) _(orc) _(xor) _(nand) _(nor) _(eqv) \
    _(mulld) _(mulhd) _(mulhdu) _(mullw) _(mulhw) _(mulhwu) \
    _(divd) _(divdu) _(divw) _(divwu) _(modsd) _(modud) _(modsw) _(moduw) \
    _(addo) _(subfo) _(addco) _(subfco) _(addeo) _(subfeo) _(addzeo) _(addmeo) \
    _(subfzeo) _(subfmeo) _(nego) _(mulldo) _(mullwo) _(divdo) _(divduo) _(divwo) _(divwuo) \
    _(divweu) _(divwe) _(divdeu) _(divde) _(divweuo) _(divweo) _(divdeuo) _(divdeo) \
    _(vaddfp) _(vsubfp) _(vmaxfp) _(vminfp) _(vcmpeqfp) _(vcmpgefp) _(vcmpgtfp) _(vcmpbfp) \
    _(vrfin) _(vrfiz) _(vrfip) _(vrfim) _(vrefp) _(vrsqrtefp) _(vexptefp) _(vlogefp) \
    _(vcfsx) _(vcfux) _(vctsxs) _(vctuxs) _(vmaddfp) _(vnmsubfp) \
    _(xvcvdpuxws) _(xvcvspsxds) _(xvcvspuxds) _(xvcvsxdsp) _(xvcvuxdsp) _(vclock) \
    _(sld) _(srd) _(slw) _(srw) _(srad) _(sraw) _(cmpb) \
    _(addi) _(andi) _(ori) _(xori) _(mulli) \
    _(addc) _(adde) _(subfc) _(subfe) _(addic) _(subfic) \
    _(addze) _(addme) _(subfze) _(subfme) \
    _(neg) _(extsb) _(extsh) _(extsw) _(cntlzw) _(cntlzd) _(cnttzw) _(cnttzd) \
    _(popcntb) _(popcntw) _(popcntd) _(mov) \
    _(sradi) _(srawi) _(rotd) _(rotd_r) _(rotd_ins) _(rotw) _(rotw_r) _(rotw_ins) \
    _(cmpd) _(cmpw) _(cmpld) _(cmplw) _(cmpdi) _(cmpwi) _(cmpldi) _(cmplwi) \
    _(cr0) _(isel) _(mfcr) _(mtcrf) _(mcrf) \
    _(crand) _(cror) _(crxor) _(creqv) _(crnand) _(crnor) _(crandc) _(crorc) \
    _(mfxer) _(mtxer) _(mftb) _(mftbu) _(tw) _(twi) _(td) _(tdi) _(bpermd) \
    _(bc_t) _(bc_f) _(bdnz) _(bdz) _(bc) _(bclr_c) _(bcctr_c) _(bclrl_c) _(bcctrl_c) \
    _(blr) _(bctr) _(blrl) _(bctrl) \
    _(lbz) _(lbzx) _(lbzu) _(lbzux) _(lhz) _(lhzx) _(lhzu) _(lhzux) \
    _(lha) _(lhax) _(lhau) _(lhaux) _(lwz) _(lwzx) _(lwzu) _(lwzux) \
    _(lwa) _(lwax) _(lwau) _(lwaux) _(ld) _(ldx) _(ldu) _(ldux) \
    _(lhbrx) _(lwbrx) _(ldbrx) \
    _(stb) _(stbx) _(stbu) _(stbux) _(sth) _(sthx) _(sthu) _(sthux) \
    _(stw) _(stwx) _(stwu) _(stwux) _(std) _(stdx) _(stdu) _(stdux) \
    _(sthbrx) _(stwbrx) _(stdbrx) \
    _(lfd) _(lfdx) _(lfdu) _(lfdux) _(lfs) _(lfsx) _(lfsu) _(lfsux) \
    _(lfiwax) _(lfiwzx) \
    _(stfd) _(stfdx) _(stfdu) _(stfdux) _(stfs) _(stfsx) _(stfsu) _(stfsux) _(stfiwx) \
    _(lxvd2x) _(lxvw4x) _(lvx) _(stxvd2x) _(stxvw4x) _(stvx) _(lxvdsx) \
    _(stvebx) _(stvehx) _(stvewx) _(lvsl) _(lvsr) \
    _(dcbz) _(sync) \
    _(lbarx) _(lharx) _(lwarx) _(ldarx) _(stbcx) _(sthcx) _(stwcx) _(stdcx) \
    _(fadd) _(fsub) _(fmul) _(fdiv) _(fadds) _(fsubs) _(fmuls) _(fdivs) \
    _(xsmaxdp) _(xsmindp) \
    _(fsqrt) _(fsqrts) _(fre) _(fres) _(frsqrte) _(frsqrtes) _(frsp) \
    _(frin) _(friz) _(frip) _(frim) _(fric) \
    _(fcfid) _(fcfidu) _(fcfids) _(fcfidus) \
    _(fctid) _(fctidz) _(fctiw) _(fctiwz) _(fctidu) _(fctiduz) _(fctiwu) _(fctiwuz) \
    _(fmr) _(fneg) _(fabs) _(fnabs) _(fcpsgn) \
    _(fmadd) _(fmsub) _(fnmadd) _(fnmsub) _(fmadds) _(fmsubs) _(fnmadds) _(fnmsubs) \
    _(fsel) _(fcmpu) _(mffs) _(mtfsf) _(mtfsfi) _(mcrfs) _(cr1) \
    _(mtvsrd) _(mtvsrwa) _(mtvsrwz) _(mfvsrwz) \
    _(xscvdpspn) _(xscvdpsp) _(xscvspdpn) _(xscvspdp) \
    _(vand) _(vor) _(vxor) _(vandc) _(vorc) _(vnor) _(vnand) _(veqv) \
    _(vaddubm) _(vadduhm) _(vadduwm) _(vaddudm) _(vsububm) _(vsubuhm) _(vsubuwm) _(vsubudm) \
    _(vmaxub) _(vmaxuh) _(vmaxuw) _(vmaxsb) _(vmaxsh) _(vmaxsw) \
    _(vminub) _(vminuh) _(vminuw) _(vminsb) _(vminsh) _(vminsw) \
    _(vaddubs) _(vadduhs) _(vadduws) _(vaddsbs) _(vaddshs) _(vaddsws) \
    _(vsububs) _(vsubuhs) _(vsubuws) _(vsubsbs) _(vsubshs) _(vsubsws) \
    _(vavgub) _(vavguh) _(vavguw) _(vavgsb) _(vavgsh) _(vavgsw) _(vmuluwm) \
    _(vcmpequb) _(vcmpequh) _(vcmpequw) _(vcmpequd) \
    _(vcmpgtub) _(vcmpgtuh) _(vcmpgtuw) _(vcmpgtud) \
    _(vcmpgtsb) _(vcmpgtsh) _(vcmpgtsw) _(vcmpgtsd) \
    _(vslb) _(vslh) _(vslw) _(vsrb) _(vsrh) _(vsrw) _(vsrab) _(vsrah) _(vsraw) \
    _(vsld) _(vsrd) _(vsrad) _(vrlb) _(vrlh) _(vrlw) \
    _(vmrghb) _(vmrghh) _(vmrghw) _(vmrglb) _(vmrglh) _(vmrglw) \
    _(xxsel) _(vperm) _(xxpermdi) _(vmov) _(vconst) _(vspltb) _(vsplth) _(vspltw) \
    _(xvnegdp) _(xvabsdp) _(xvnabsdp) _(cr6) _(vgbbd) _(vslo) _(vsro) _(vsl) _(vsr) _(mfvscr) _(mtvscr) _(vbpermq) \
    _(vmuleub) _(vmuloub) _(vmuleuh) _(vmulouh) _(vmuleuw) _(vmulouw) _(vmulesb) _(vmulosb) _(vmulesh) _(vmulosh) _(vmulesw) _(vmulosw) _(vpkuhum) _(vpkuwum) _(vpkudum) _(vpkshss) _(vpkswss) _(vpksdss) _(vpkshus) _(vpkswus) _(vpksdus) _(vpkuhus) _(vpkuwus) _(vpkudus) _(vsumsws) _(vsum2sws) _(vsum4ubs) _(vsum4sbs) _(vsum4shs) _(vadduqm) _(vsubuqm) _(vaddcuw) _(vsubcuw) _(vmrgew) _(vmrgow) _(vmaxsd) _(vmaxud) _(vminsd) _(vminud) _(vrld) _(vupkhsb) _(vupklsb) _(vupkhsh) _(vupklsh) _(vupkhsw) _(vupklsw) _(vpopcntb) _(vpopcnth) _(vpopcntw) _(vpopcntd) _(vclzb) _(vclzh) _(vclzw) _(vclzd) \
    _(xvadddp) _(xvaddsp) _(xvsubdp) _(xvsubsp) _(xvmuldp) _(xvmulsp) _(xvdivdp) _(xvdivsp) _(xvmaxdp) _(xvmaxsp) _(xvmindp) _(xvminsp) _(xvcmpeqdp) _(xvcmpeqsp) _(xvcmpgtdp) _(xvcmpgtsp) _(xvcmpgedp) _(xvcmpgesp) _(xvcpsgndp) _(xvcpsgnsp) _(xvmaddadp) _(xvmaddmdp) _(xvmaddasp) _(xvmaddmsp) _(xvmsubadp) _(xvmsubmdp) _(xvmsubasp) _(xvmsubmsp) _(xvnmaddadp) _(xvnmaddmdp) _(xvnmaddasp) _(xvnmaddmsp) _(xvnmsubadp) _(xvnmsubmdp) _(xvnmsubasp) _(xvnmsubmsp) _(xvsqrtdp) _(xvsqrtsp) _(xvabssp) _(xvnegsp) _(xvnabssp) _(xvrdpi) _(xvrdpiz) _(xvrdpip) _(xvrdpim) _(xvrdpic) _(xvrspi) _(xvrspiz) _(xvrspip) _(xvrspim) _(xvrspic) _(xvcvsxwsp) _(xvcvuxwsp) _(xvcvspsxws) _(xvcvspuxws) _(xvcvsxddp) _(xvcvuxddp) _(xvcvdpsxds) _(xvcvdpuxds) _(xvredp) _(xvresp) _(xvrsqrtedp) _(xvrsqrtesp) _(xvcvspdp) _(xvcvsxwdp) _(xvcvuxwdp) _(xvcvdpsp) _(xvcvdpsxws) \
    _(vsldoi1) _(vsldoi2) _(vsldoi3) _(vsldoi4) _(vsldoi5) _(vsldoi6) _(vsldoi7) _(vsldoi8) \
    _(vsldoi9) _(vsldoi10) _(vsldoi11) _(vsldoi12) _(vsldoi13) _(vsldoi14) _(vsldoi15)

#define DECL(name) extern void gadget_ppc64_##name(void);
PPC64_GADGETS(DECL)
#undef DECL
#define G(name) ((unsigned long) gadget_ppc64_##name)

#define TAG 0x8000000000000000ULL

static inline void emit(struct gen_state *state, unsigned long word) {
    gen_raw(state, word);
}

// ---- operands ---------------------------------------------------------------------
static unsigned long gpr(unsigned r) {
    return offsetof(struct cpu_state, ppc64_gpr) + r * sizeof(qword_t);
}
// (rA|0): rA, or a word that is always zero
static unsigned long gpr0(unsigned r) {
    return r == 0 ? offsetof(struct cpu_state, ppc64_zero) : gpr(r);
}
static unsigned long vsr(unsigned r) {
    return offsetof(struct cpu_state, ppc64_vsr) + r * 16;
}
// a VSR's doubleword 0, the high one; FPR n is VSR n's
static unsigned long dw0(unsigned r) {
    return vsr(r) + 8;
}
static unsigned long fpr(unsigned r) {
    return dw0(r);
}
static unsigned long crf(unsigned n) {
    return offsetof(struct cpu_state, ppc64_cr) + n * 4;
}
// CR bit b: its field's word, and the bit's value there (LT 8 ... SO 1)
static unsigned long crbit_field(unsigned b) {
    return crf(b / 4);
}
static unsigned long crbit_mask(unsigned b) {
    return 8u >> (b % 4);
}
#define LR_OFF offsetof(struct cpu_state, ppc64_lr)
#define CTR_OFF offsetof(struct cpu_state, ppc64_ctr)

// MB..ME, the ISA's bit numbers (0 the most significant); wrapping when
// MB > ME
static uint64_t mask64(unsigned mb, unsigned me) {
    uint64_t from = ~0ULL >> mb;
    uint64_t to = ~0ULL << (63 - me);
    return mb <= me ? from & to : from | to;
}

// ---- emitters ---------------------------------------------------------------------
static int gen_ppc64_interrupt_at(struct gen_state *state, int code, guest_addr_t pc, guest_addr_t addr) {
    emit(state, G(interrupt));
    emit(state, code);
    emit(state, pc);
    emit(state, addr);
    return 0;
}

static int gen_ppc64_undefined(struct gen_state *state, uint32_t insn) {
    printk("WARNING: ppc64 JIT: no gadget for insn %#010x at pc %#llx\n",
           insn, (unsigned long long) state->ppc64_orig_ip);
    return gen_ppc64_interrupt_at(state, INT_UNDEFINED, state->ppc64_orig_ip, state->ppc64_orig_ip);
}

static void mov_const(struct gen_state *state, unsigned long dst_off, uint64_t value) {
    emit(state, G(mov_const));
    emit(state, dst_off);
    emit(state, value);
}

static void three(struct gen_state *state, unsigned long gadget, unsigned long a,
        unsigned long b, unsigned long c) {
    emit(state, gadget);
    emit(state, a);
    emit(state, b);
    emit(state, c);
}

static void two(struct gen_state *state, unsigned long gadget, unsigned long a, unsigned long b) {
    emit(state, gadget);
    emit(state, a);
    emit(state, b);
}

static void record(struct gen_state *state, uint32_t insn, unsigned long dst) {
    if (ppc_rcbit(insn)) {
        emit(state, G(cr0));
        emit(state, dst);
    }
}

// A chainable target in jump slot `slot`.
static void target(struct gen_state *state, int slot, guest_addr_t addr) {
    emit(state, addr | TAG);
    state->jump_ip[slot] = state->size - 1;
}

// A load or store, D or X form: [gadget][rt][ra][disp or rb][pc]
static void ldst(struct gen_state *state, unsigned long gadget, unsigned long rt,
        unsigned long ra, uint64_t third) {
    emit(state, gadget);
    emit(state, rt);
    emit(state, ra);
    emit(state, third);
    emit(state, state->ppc64_orig_ip);
}

bool gen_start_ppc64(guest_addr_t addr, struct gen_state *state) {
    if (!gen_start(addr, state))
        return false;
    state->ppc64 = true;
    state->ppc64_ip = addr;
    state->ppc64_orig_ip = addr;
    return true;
}

void gen_exit_ppc64(struct gen_state *state) {
    // the block hit its size cap: on to the next instruction, chainable
    emit(state, G(b));
    target(state, 0, state->ppc64_ip);
}

// ---- branches -----------------------------------------------------------------------
// bc's BO and BI, and the taken target; LK has set LR already.
static int gen_bc(struct gen_state *state, unsigned bo, unsigned bi, guest_addr_t taken,
        guest_addr_t next) {
    bool no_ctr = bo & 4, no_cond = bo & 16;
    if (no_ctr && no_cond) {
        emit(state, G(b));
        target(state, 0, taken);
        return 0;
    }
    if (no_ctr) {
        emit(state, (bo & 8) ? G(bc_t) : G(bc_f));
        emit(state, crbit_field(bi));
        emit(state, crbit_mask(bi));
    } else if (no_cond) {
        emit(state, (bo & 2) ? G(bdz) : G(bdnz));
    } else {
        emit(state, G(bc));
        emit(state, bo);
        emit(state, crbit_field(bi));
        emit(state, crbit_mask(bi));
    }
    target(state, 0, taken);
    target(state, 1, next);
    return 0;
}

// bclr, bcctr. ctr: the CTR form (BO must not decrement CTR there).
static int gen_bclr(struct gen_state *state, uint32_t insn, bool ctr, guest_addr_t next) {
    unsigned bo = ppc_rt(insn), bi = ppc_ra(insn);
    bool lk = ppc_rcbit(insn);
    if (ctr && !(bo & 4))
        return gen_ppc64_undefined(state, insn);
    if ((bo & 0x14) == 0x14) {
        if (lk) {
            emit(state, ctr ? G(bctrl) : G(blrl));
            emit(state, next);
        } else {
            emit(state, ctr ? G(bctr) : G(blr));
        }
        return 0;
    }
    emit(state, lk ? (ctr ? G(bcctrl_c) : G(bclrl_c)) : (ctr ? G(bcctr_c) : G(bclr_c)));
    emit(state, bo);
    emit(state, crbit_field(bi));
    emit(state, crbit_mask(bi));
    if (lk)
        emit(state, next);
    target(state, 1, next);
    return 0;
}

// ---- op 31 -----------------------------------------------------------------------
static int gen_op31(struct gen_state *state, uint32_t insn) {
    unsigned rt = ppc_rt(insn), ra = ppc_ra(insn), rb = ppc_rb(insn);
    unsigned xo = ppc_xo10(insn);
    unsigned long g;

    // isel (A form)
    if (ppc_xo5(insn) == 15) {
        unsigned bc = ppc_rc(insn);
        emit(state, G(isel));
        emit(state, gpr(rt));
        emit(state, gpr0(ra));
        emit(state, gpr(rb));
        emit(state, crbit_field(bc));
        emit(state, crbit_mask(bc));
        return 1;
    }
    // sradi (XS form)
    if (ppc_xs_xo(insn) == 413) {
        unsigned sh = ppc_md_sh(insn);
        three(state, G(sradi), gpr(ra), gpr(rt), sh);
        record(state, insn, gpr(ra));
        return 1;
    }

    // The XO-form arithmetic; OE (bit 21) takes the form that sets XER[OV]
    unsigned long go = 0;
    switch (ppc_xo9(insn)) {
        case 266: g = G(add); go = G(addo); goto xo_rr;
        case 40: g = G(subf); go = G(subfo); goto xo_rr;
        case 10: g = G(addc); go = G(addco); goto xo_rr;
        case 8: g = G(subfc); go = G(subfco); goto xo_rr;
        case 138: g = G(adde); go = G(addeo); goto xo_rr;
        case 136: g = G(subfe); go = G(subfeo); goto xo_rr;
        case 233: g = G(mulld); go = G(mulldo); goto xo_rr;
        case 235: g = G(mullw); go = G(mullwo); goto xo_rr;
        case 457: g = G(divdu); go = G(divduo); goto xo_rr;
        case 489: g = G(divd); go = G(divdo); goto xo_rr;
        case 459: g = G(divwu); go = G(divwuo); goto xo_rr;
        case 491: g = G(divw); go = G(divwo); goto xo_rr;
        case 395: g = G(divweu); go = G(divweuo); goto xo_rr;
        case 427: g = G(divwe); go = G(divweo); goto xo_rr;
        case 393: g = G(divdeu); go = G(divdeuo); goto xo_rr;
        case 425: g = G(divde); go = G(divdeo); goto xo_rr;
        case 9: g = G(mulhdu); goto xo_rr;     // (no OE form: bit 21 is 0)
        case 73: g = G(mulhd); goto xo_rr;
        case 11: g = G(mulhwu); goto xo_rr;
        case 75: g = G(mulhw); goto xo_rr;
        case 104: g = G(neg); go = G(nego); goto xo_r1;
        case 202: g = G(addze); go = G(addzeo); goto xo_r1;
        case 234: g = G(addme); go = G(addmeo); goto xo_r1;
        case 200: g = G(subfze); go = G(subfzeo); goto xo_r1;
        case 232: g = G(subfme); go = G(subfmeo); goto xo_r1;
        default: break;
        xo_rr:
            if (ppc_oe(insn)) {
                if (!go)
                    return gen_ppc64_undefined(state, insn);
                g = go;
            }
            three(state, g, gpr(rt), gpr(ra), gpr(rb));
            record(state, insn, gpr(rt));
            return 1;
        xo_r1:
            if (ppc_oe(insn))
                g = go;
            two(state, g, gpr(rt), gpr(ra));
            record(state, insn, gpr(rt));
            return 1;
    }

    switch (xo) {
        // compares
        case 0: // cmp
        case 32: // cmpl
            if (xo == 0)
                g = ppc_l(insn) ? G(cmpd) : G(cmpw);
            else
                g = ppc_l(insn) ? G(cmpld) : G(cmplw);
            three(state, g, crf(ppc_bf(insn)), gpr(ra), gpr(rb));
            return 1;

        // logic and shifts: rA = op(rS, rB)
        case 28: g = G(and); goto logic;
        case 60: g = G(andc); goto logic;
        case 444: // or, and mr when rS == rB
            if (rt == rb) {
                two(state, G(mov), gpr(ra), gpr(rt));
                record(state, insn, gpr(ra));
                return 1;
            }
            g = G(or); goto logic;
        case 412: g = G(orc); goto logic;
        case 316: g = G(xor); goto logic;
        case 476: g = G(nand); goto logic;
        case 124: g = G(nor); goto logic;
        case 284: g = G(eqv); goto logic;
        case 27: g = G(sld); goto logic;
        case 539: g = G(srd); goto logic;
        case 24: g = G(slw); goto logic;
        case 536: g = G(srw); goto logic;
        case 794: g = G(srad); goto logic;
        case 792: g = G(sraw); goto logic;
        case 508: g = G(cmpb); goto logic;
        case 252: g = G(bpermd); goto logic;
        case 777: g = G(modsd); goto arith;
        case 265: g = G(modud); goto arith;
        case 779: g = G(modsw); goto arith;
        case 267: g = G(moduw); goto arith;
        logic:
            three(state, g, gpr(ra), gpr(rt), gpr(rb));
            record(state, insn, gpr(ra));
            return 1;
        arith:
            three(state, g, gpr(rt), gpr(ra), gpr(rb));
            return 1;
        case 824: // srawi
            three(state, G(srawi), gpr(ra), gpr(rt), rb);
            record(state, insn, gpr(ra));
            return 1;
        case 922: g = G(extsh); goto unary;
        case 954: g = G(extsb); goto unary;
        case 986: g = G(extsw); goto unary;
        case 26: g = G(cntlzw); goto unary;
        case 58: g = G(cntlzd); goto unary;
        case 538: g = G(cnttzw); goto unary;
        case 570: g = G(cnttzd); goto unary;
        case 122: g = G(popcntb); goto unary;
        case 378: g = G(popcntw); goto unary;
        case 506: g = G(popcntd); goto unary;
        unary:
            two(state, g, gpr(ra), gpr(rt));
            record(state, insn, gpr(ra));
            return 1;

        // the condition register, SPRs
        case 19: // mfcr, mfocrf
            emit(state, G(mfcr));
            emit(state, gpr(rt));
            return 1;
        case 144: // mtcrf, mtocrf
            two(state, G(mtcrf), gpr(rt), ppc_fxm(insn));
            return 1;
        case 339: // mfspr
        case 371: { // mftb
            unsigned spr = ppc_spr(insn);
            switch (spr) {
                case 1: emit(state, G(mfxer)); emit(state, gpr(rt)); return 1;
                case 8: two(state, G(mov), gpr(rt), LR_OFF); return 1;
                case 9: two(state, G(mov), gpr(rt), CTR_OFF); return 1;
                case 256: two(state, G(mov), gpr(rt), offsetof(struct cpu_state, ppc64_vrsave)); return 1;
                case 268: emit(state, G(mftb)); emit(state, gpr(rt)); return 1;
                // AOK_VCLOCK, the clock only AOK's vDSO reads (vdso/ppc64/
                // vdso.S): a kernel service, like a system call
                case 777: emit(state, G(vclock)); emit(state, gpr(rt)); return 1;
                case 269: emit(state, G(mftbu)); emit(state, gpr(rt)); return 1;
            }
            return gen_ppc64_undefined(state, insn);
        }
        case 467: { // mtspr
            unsigned spr = ppc_spr(insn);
            switch (spr) {
                case 1: emit(state, G(mtxer)); emit(state, gpr(rt)); return 1;
                case 8: two(state, G(mov), LR_OFF, gpr(rt)); return 1;
                case 9: two(state, G(mov), CTR_OFF, gpr(rt)); return 1;
                case 256: two(state, G(mov), offsetof(struct cpu_state, ppc64_vrsave), gpr(rt)); return 1;
            }
            return gen_ppc64_undefined(state, insn);
        }

        // loads and stores, X forms
#define LX(n, name) case n: ldst(state, G(name), gpr(rt), gpr0(ra), gpr(rb)); return 1;
#define LUX(n, name) case n: \
            if (ra == 0 || ra == rt) return gen_ppc64_undefined(state, insn); \
            ldst(state, G(name), gpr(rt), gpr(ra), gpr(rb)); return 1;
#define SUX(n, name) case n: \
            if (ra == 0) return gen_ppc64_undefined(state, insn); \
            ldst(state, G(name), gpr(rt), gpr(ra), gpr(rb)); return 1;
        LX(87, lbzx) LUX(119, lbzux) LX(279, lhzx) LUX(311, lhzux)
        LX(343, lhax) LUX(375, lhaux) LX(23, lwzx) LUX(55, lwzux)
        LX(341, lwax) LUX(373, lwaux) LX(21, ldx) LUX(53, ldux)
        LX(790, lhbrx) LX(534, lwbrx) LX(532, ldbrx)
        LX(215, stbx) SUX(247, stbux) LX(407, sthx) SUX(439, sthux)
        LX(151, stwx) SUX(183, stwux) LX(149, stdx) SUX(181, stdux)
        LX(918, sthbrx) LX(662, stwbrx) LX(660, stdbrx)
        LX(52, lbarx) LX(116, lharx) LX(20, lwarx) LX(84, ldarx)
        LX(694, stbcx) LX(726, sthcx) LX(150, stwcx) LX(214, stdcx)
#undef LX
#undef LUX
#undef SUX
        // floating-point X forms: rt is the FPR
#define FX(n, name) case n: ldst(state, G(name), fpr(rt), gpr0(ra), gpr(rb)); return 1;
#define FUX(n, name) case n: \
            if (ra == 0) return gen_ppc64_undefined(state, insn); \
            ldst(state, G(name), fpr(rt), gpr(ra), gpr(rb)); return 1;
        FX(599, lfdx) FUX(631, lfdux) FX(535, lfsx) FUX(567, lfsux)
        FX(727, stfdx) FUX(759, stfdux) FX(663, stfsx) FUX(695, stfsux)
        FX(855, lfiwax) FX(887, lfiwzx) FX(983, stfiwx)
#undef FX
#undef FUX
        // VSX loads and stores (XX1): the VSR with TX
#define VX1(n, name, off) case n: ldst(state, G(name), off(ppc_xt(insn)), gpr0(ra), gpr(rb)); return 1;
        VX1(588, lfdx, dw0) VX1(716, stfdx, dw0) VX1(524, lfsx, dw0) VX1(652, stfsx, dw0)
        VX1(76, lfiwax, dw0) VX1(12, lfiwzx, dw0) VX1(140, stfiwx, dw0)
        VX1(844, lxvd2x, vsr) VX1(972, stxvd2x, vsr) VX1(780, lxvw4x, vsr) VX1(908, stxvw4x, vsr)
        VX1(332, lxvdsx, vsr)
#undef VX1
        // VMX loads and stores: VR n is VSR 32 + n
        case 103: case 359: // lvx, lvxl
        case 7: case 39: case 71: // lvebx, lvehx, lvewx: the whole quadword, as lvx
            ldst(state, G(lvx), vsr(32 + rt), gpr0(ra), gpr(rb));
            return 1;
        case 231: case 487: // stvx, stvxl
            ldst(state, G(stvx), vsr(32 + rt), gpr0(ra), gpr(rb));
            return 1;
        case 135: ldst(state, G(stvebx), vsr(32 + rt), gpr0(ra), gpr(rb)); return 1;
        case 167: ldst(state, G(stvehx), vsr(32 + rt), gpr0(ra), gpr(rb)); return 1;
        case 199: ldst(state, G(stvewx), vsr(32 + rt), gpr0(ra), gpr(rb)); return 1;
        case 6: three(state, G(lvsl), vsr(32 + rt), gpr0(ra), gpr(rb)); return 1;
        case 38: three(state, G(lvsr), vsr(32 + rt), gpr0(ra), gpr(rb)); return 1;

        // moves between GPRs and VSRs
        case 51: two(state, G(mov), gpr(ra), dw0(ppc_xs_x1(insn))); return 1;      // mfvsrd
        case 115: two(state, G(mfvsrwz), gpr(ra), dw0(ppc_xs_x1(insn))); return 1; // mfvsrwz
        case 179: two(state, G(mtvsrd), dw0(ppc_xt(insn)), gpr(ra)); return 1;
        case 211: two(state, G(mtvsrwa), dw0(ppc_xt(insn)), gpr(ra)); return 1;
        case 243: two(state, G(mtvsrwz), dw0(ppc_xt(insn)), gpr(ra)); return 1;

        // cache blocks, barriers
        case 1014: // dcbz
            emit(state, G(dcbz));
            emit(state, gpr0(ra));
            emit(state, gpr(rb));
            emit(state, state->ppc64_orig_ip);
            return 1;
        case 598: // sync, lwsync, ptesync
        case 854: // eieio
            emit(state, G(sync));
            return 1;
        case 54: case 86: case 246: case 278: // dcbst, dcbf, dcbtst, dcbt
            return 1;
        case 982: // icbi: the JIT sees code written through the TLB's write path
            return 1;
        case 4: // tw: TO 31 is trap, TO 0 nothing
        case 68: // td
            if (rt == 0)
                return 1;
            if (rt == 31)
                return gen_ppc64_interrupt_at(state, INT_BREAKPOINT, state->ppc64_orig_ip, state->ppc64_orig_ip);
            emit(state, xo == 4 ? G(tw) : G(td));
            emit(state, gpr(ra));
            emit(state, gpr(rb));
            emit(state, rt);
            emit(state, state->ppc64_orig_ip);
            return 1;
    }
    return gen_ppc64_undefined(state, insn);
}

// ---- op 19 -----------------------------------------------------------------------
static int gen_op19(struct gen_state *state, uint32_t insn, guest_addr_t next) {
    unsigned bt = ppc_rt(insn), ba = ppc_ra(insn), bb = ppc_rb(insn);
    unsigned long g;
    switch (ppc_xo10(insn)) {
        case 16: return gen_bclr(state, insn, false, next);
        case 528: return gen_bclr(state, insn, true, next);
        case 0: // mcrf
            two(state, G(mcrf), crf(ppc_bf(insn)), crf(ppc_bfa(insn)));
            return 1;
        case 150: // isync
            return 1;
        case 257: g = G(crand); goto crop;
        case 449: g = G(cror); goto crop;
        case 193: g = G(crxor); goto crop;
        case 289: g = G(creqv); goto crop;
        case 225: g = G(crnand); goto crop;
        case 33: g = G(crnor); goto crop;
        case 129: g = G(crandc); goto crop;
        case 417: g = G(crorc); goto crop;
        crop:
            emit(state, g);
            emit(state, crbit_field(bt));
            emit(state, crbit_mask(bt));
            emit(state, crbit_field(ba));
            emit(state, crbit_mask(ba));
            emit(state, crbit_field(bb));
            emit(state, crbit_mask(bb));
            return 1;
    }
    return gen_ppc64_undefined(state, insn);
}

// ---- op 30: the doubleword rotates ------------------------------------------------
static int gen_op30(struct gen_state *state, uint32_t insn) {
    unsigned rs = ppc_rt(insn), ra = ppc_ra(insn), rb = ppc_rb(insn);
    unsigned sh = ppc_md_sh(insn), mb = ppc_md_mb(insn);
    unsigned long ror = (64 - sh) & 63;
    switch (ppc_md_xo(insn)) {
        case 0: // rldicl
            emit(state, G(rotd)); emit(state, gpr(ra)); emit(state, gpr(rs));
            emit(state, ror); emit(state, mask64(mb, 63));
            break;
        case 1: // rldicr (the field is ME)
            emit(state, G(rotd)); emit(state, gpr(ra)); emit(state, gpr(rs));
            emit(state, ror); emit(state, mask64(0, mb));
            break;
        case 2: // rldic
            emit(state, G(rotd)); emit(state, gpr(ra)); emit(state, gpr(rs));
            emit(state, ror); emit(state, mask64(mb, 63 - sh));
            break;
        case 3: // rldimi
            emit(state, G(rotd_ins)); emit(state, gpr(ra)); emit(state, gpr(rs));
            emit(state, ror); emit(state, mask64(mb, 63 - sh));
            break;
        case 4: // MDS: rldcl (8), rldcr (9)
            if (ppc_mds_xo(insn) == 8) {
                emit(state, G(rotd_r)); emit(state, gpr(ra)); emit(state, gpr(rs));
                emit(state, gpr(rb)); emit(state, mask64(mb, 63));
                break;
            }
            if (ppc_mds_xo(insn) == 9) {
                emit(state, G(rotd_r)); emit(state, gpr(ra)); emit(state, gpr(rs));
                emit(state, gpr(rb)); emit(state, mask64(0, mb));
                break;
            }
            return gen_ppc64_undefined(state, insn);
        default:
            return gen_ppc64_undefined(state, insn);
    }
    record(state, insn, gpr(ra));
    return 1;
}

// ---- floating point: ops 59 and 63 ------------------------------------------------
static int gen_fp(struct gen_state *state, uint32_t insn, bool single) {
    unsigned t = ppc_rt(insn), a = ppc_ra(insn), b = ppc_rb(insn), c = ppc_rc(insn);
    unsigned long g = 0;
    switch (ppc_xo5(insn)) {
        case 18: g = single ? G(fdivs) : G(fdiv); goto ab;
        case 20: g = single ? G(fsubs) : G(fsub); goto ab;
        case 21: g = single ? G(fadds) : G(fadd); goto ab;
        case 25: // fmul: FRA and FRC
            three(state, single ? G(fmuls) : G(fmul), fpr(t), fpr(a), fpr(c));
            goto rc;
        case 22: g = single ? G(fsqrts) : G(fsqrt); goto unary;
        case 24: g = single ? G(fres) : G(fre); goto unary;
        case 26: g = single ? G(frsqrtes) : G(frsqrte); goto unary;
        case 23: // fsel
            if (single)
                break;
            emit(state, G(fsel)); emit(state, fpr(t)); emit(state, fpr(a));
            emit(state, fpr(c)); emit(state, fpr(b));
            goto rc;
        case 28: g = single ? G(fmsubs) : G(fmsub); goto fma;
        case 29: g = single ? G(fmadds) : G(fmadd); goto fma;
        case 30: g = single ? G(fnmsubs) : G(fnmsub); goto fma;
        case 31: g = single ? G(fnmadds) : G(fnmadd); goto fma;
        ab:
            three(state, g, fpr(t), fpr(a), fpr(b));
            goto rc;
        unary:
            two(state, g, fpr(t), fpr(b));
            goto rc;
        fma:
            emit(state, g); emit(state, fpr(t)); emit(state, fpr(a));
            emit(state, fpr(c)); emit(state, fpr(b));
            goto rc;
    }
    switch (ppc_xo10(insn)) {
        case 846: g = single ? G(fcfids) : G(fcfid); goto unary;
        case 974: g = single ? G(fcfidus) : G(fcfidu); goto unary;
    }
    if (single)
        return gen_ppc64_undefined(state, insn);
    switch (ppc_xo10(insn)) {
        case 0: case 32: // fcmpu, fcmpo
            three(state, G(fcmpu), crf(ppc_bf(insn)), fpr(a), fpr(b));
            return 1;
        case 12: g = G(frsp); goto unary;
        case 14: g = G(fctiw); goto unary;
        case 15: g = G(fctiwz); goto unary;
        case 142: g = G(fctiwu); goto unary;
        case 143: g = G(fctiwuz); goto unary;
        case 814: g = G(fctid); goto unary;
        case 815: g = G(fctidz); goto unary;
        case 942: g = G(fctidu); goto unary;
        case 943: g = G(fctiduz); goto unary;
        case 72: g = G(fmr); goto unary;
        case 40: g = G(fneg); goto unary;
        case 264: g = G(fabs); goto unary;
        case 136: g = G(fnabs); goto unary;
        case 392: g = G(frin); goto unary;
        case 424: g = G(friz); goto unary;
        case 456: g = G(frip); goto unary;
        case 488: g = G(frim); goto unary;
        case 8: // fcpsgn
            three(state, G(fcpsgn), fpr(t), fpr(a), fpr(b));
            goto rc;
        case 583: // mffs (and ISA 3.0's mffsl and kin, which this decodes as mffs)
            emit(state, G(mffs));
            emit(state, fpr(t));
            goto rc;
        case 711: { // mtfsf FLM, FRB (L: all; W: the other half, none here)
            bool l = (insn >> 25) & 1;
            unsigned flm = (insn >> 17) & 0xff;
            uint32_t bits = 0;
            if (l)
                bits = 0xffffffff;
            else
                for (int i = 0; i < 8; i++)
                    if (flm & (0x80 >> i))
                        bits |= 0xfu << (28 - 4 * i);
            two(state, G(mtfsf), fpr(b), bits);
            goto rc;
        }
        case 134: { // mtfsfi BF, U
            unsigned bf = ppc_bf(insn), u = (insn >> 12) & 15;
            two(state, G(mtfsfi), 0xfu << (28 - 4 * bf), (uint32_t) u << (28 - 4 * bf));
            goto rc;
        }
        case 70: // mtfsb0 BT
            two(state, G(mtfsfi), 0x80000000u >> t, 0);
            goto rc;
        case 38: // mtfsb1 BT
            two(state, G(mtfsfi), 0, 0x80000000u >> t);
            goto rc;
        case 64: // mcrfs BF, BFA
            two(state, G(mcrfs), crf(ppc_bf(insn)), 28 - 4 * ppc_bfa(insn));
            return 1;
    }
    return gen_ppc64_undefined(state, insn);
rc:
    if (ppc_rcbit(insn))
        emit(state, G(cr1));
    return 1;
}

// ---- op 60: VSX ------------------------------------------------------------------
static int gen_vsx(struct gen_state *state, uint32_t insn) {
    unsigned xt = ppc_xt(insn), xa = ppc_xa(insn), xb = ppc_xb(insn);
    unsigned long g;
    // XX4: xxsel
    if (((insn >> 4) & 3) == 3) {
        emit(state, G(xxsel)); emit(state, vsr(xt)); emit(state, vsr(xa));
        emit(state, vsr(xb)); emit(state, vsr(ppc_xc(insn)));
        return 1;
    }
    unsigned xo3 = ppc_xx3_xo(insn);
    if ((xo3 & 0x9f) == 0x0a) { // xxpermdi XT, XA, XB, DM
        unsigned dm = (insn >> 8) & 3;
        three(state, G(xxpermdi), vsr(xt), (dm & 2) ? vsr(xa) : dw0(xa), (dm & 1) ? vsr(xb) : dw0(xb));
        return 1;
    }
    if ((xo3 & 0x9f) == 0x02) { // xxsldwi XT, XA, XB, SHW
        unsigned shw = (insn >> 8) & 3;
        if (shw == 0) {
            two(state, G(vmov), vsr(xt), vsr(xa));
            return 1;
        }
        g = shw == 1 ? G(vsldoi4) : shw == 2 ? G(vsldoi8) : G(vsldoi12);
        three(state, g, vsr(xt), vsr(xa), vsr(xb));
        return 1;
    }
    switch (xo3) {
        case 154: // xxlxor
            if (xa == xb) {
                three(state, G(vconst), vsr(xt), 0, 0);
                return 1;
            }
            g = G(vxor); goto v3;
        case 146: // xxlor
            if (xa == xb) {
                two(state, G(vmov), vsr(xt), vsr(xa));
                return 1;
            }
            g = G(vor); goto v3;
        case 130: g = G(vand); goto v3;
        case 138: g = G(vandc); goto v3;
        case 162: g = G(vnor); goto v3;
        case 170: g = G(vorc); goto v3;
        case 178: g = G(vnand); goto v3;
        case 186: g = G(veqv); goto v3;
case 96: g = G(xvadddp); goto v3;
        case 64: g = G(xvaddsp); goto v3;
        case 104: g = G(xvsubdp); goto v3;
        case 72: g = G(xvsubsp); goto v3;
        case 112: g = G(xvmuldp); goto v3;
        case 80: g = G(xvmulsp); goto v3;
        case 120: g = G(xvdivdp); goto v3;
        case 88: g = G(xvdivsp); goto v3;
        case 97: g = G(xvmaddadp); goto v3;
        case 105: g = G(xvmaddmdp); goto v3;
        case 65: g = G(xvmaddasp); goto v3;
        case 73: g = G(xvmaddmsp); goto v3;
        case 113: g = G(xvmsubadp); goto v3;
        case 121: g = G(xvmsubmdp); goto v3;
        case 81: g = G(xvmsubasp); goto v3;
        case 89: g = G(xvmsubmsp); goto v3;
        case 225: g = G(xvnmaddadp); goto v3;
        case 233: g = G(xvnmaddmdp); goto v3;
        case 193: g = G(xvnmaddasp); goto v3;
        case 201: g = G(xvnmaddmsp); goto v3;
        case 241: g = G(xvnmsubadp); goto v3;
        case 249: g = G(xvnmsubmdp); goto v3;
        case 209: g = G(xvnmsubasp); goto v3;
        case 217: g = G(xvnmsubmsp); goto v3;
        case 224: g = G(xvmaxdp); goto v3;
        case 192: g = G(xvmaxsp); goto v3;
        case 232: g = G(xvmindp); goto v3;
        case 200: g = G(xvminsp); goto v3;
        case 240: g = G(xvcpsgndp); goto v3;
        case 208: g = G(xvcpsgnsp); goto v3;
        case 99: case 227: g = G(xvcmpeqdp); goto vcmp;
        case 67: case 195: g = G(xvcmpeqsp); goto vcmp;
        case 107: case 235: g = G(xvcmpgtdp); goto vcmp;
        case 75: case 203: g = G(xvcmpgtsp); goto vcmp;
        case 115: case 243: g = G(xvcmpgedp); goto vcmp;
        case 83: case 211: g = G(xvcmpgesp); goto vcmp;
        case 18: g = G(vmrghw); goto v3;
        case 50: g = G(vmrglw); goto v3;
        v3:
            three(state, g, vsr(xt), vsr(xa), vsr(xb));
            return 1;
        vcmp: // the record forms (bit 21) set CR6
            three(state, g, vsr(xt), vsr(xa), vsr(xb));
            if ((insn >> 10) & 1) {
                emit(state, G(cr6));
                emit(state, vsr(xt));
            }
            return 1;
        // scalar double: the FPR gadgets on doubleword 0
        case 32: g = G(fadd); goto s3;
        case 40: g = G(fsub); goto s3;
        case 48: g = G(fmul); goto s3;
        case 56: g = G(fdiv); goto s3;
        case 0: g = G(fadds); goto s3;
        case 8: g = G(fsubs); goto s3;
        case 16: g = G(fmuls); goto s3;
        case 24: g = G(fdivs); goto s3;
        case 160: g = G(xsmaxdp); goto s3;
        case 168: g = G(xsmindp); goto s3;
        case 176: g = G(fcpsgn); goto s3;
        s3:
            three(state, g, dw0(xt), dw0(xa), dw0(xb));
            return 1;
        case 35: case 43: // xscmpudp, xscmpodp BF, XA, XB
            three(state, G(fcmpu), crf(ppc_bf(insn)), dw0(xa), dw0(xb));
            return 1;
        // the multiply-adds: the A forms take XT as the addend (XT = XA*XB + XT),
        // the M forms as the multiplicand (XT = XA*XT + XB)
        case 33: g = G(fmadd); goto ma;     case 41: g = G(fmadd); goto mm;
        case 49: g = G(fmsub); goto ma;     case 57: g = G(fmsub); goto mm;
        case 161: g = G(fnmadd); goto ma;   case 169: g = G(fnmadd); goto mm;
        case 177: g = G(fnmsub); goto ma;   case 185: g = G(fnmsub); goto mm;
        case 1: g = G(fmadds); goto ma;     case 9: g = G(fmadds); goto mm;
        case 17: g = G(fmsubs); goto ma;    case 25: g = G(fmsubs); goto mm;
        case 129: g = G(fnmadds); goto ma;  case 137: g = G(fnmadds); goto mm;
        case 145: g = G(fnmsubs); goto ma;  case 153: g = G(fnmsubs); goto mm;
        ma: // [frt][fra][frc][frb]: XA * XB + XT
            emit(state, g); emit(state, dw0(xt)); emit(state, dw0(xa));
            emit(state, dw0(xb)); emit(state, dw0(xt));
            return 1;
        mm: // XA * XT + XB
            emit(state, g); emit(state, dw0(xt)); emit(state, dw0(xa));
            emit(state, dw0(xt)); emit(state, dw0(xb));
            return 1;
    }
    switch (ppc_xx2_xo(insn)) {
        case 164: { // xxspltw XT, XB, UIM: big-endian word UIM is host word 3 - UIM
            unsigned uim = (insn >> 16) & 3;
            two(state, G(vspltw), vsr(xt), vsr(xb) + 4 * (3 - uim));
            return 1;
        }
        case 267: two(state, G(xscvdpspn), dw0(xt), dw0(xb)); return 1;
        case 265: two(state, G(xscvdpsp), dw0(xt), dw0(xb)); return 1;
        case 331: two(state, G(xscvspdpn), dw0(xt), dw0(xb)); return 1;
        case 329: two(state, G(xscvspdp), dw0(xt), dw0(xb)); return 1;
        case 344: g = G(fctidz); goto s2;
        case 88: g = G(fctiwz); goto s2;
        case 328: g = G(fctiduz); goto s2;
        case 72: g = G(fctiwuz); goto s2;
        case 376: g = G(fcfid); goto s2;
        case 360: g = G(fcfidu); goto s2;
        case 312: g = G(fcfids); goto s2;
        case 296: g = G(fcfidus); goto s2;
        case 345: g = G(fabs); goto s2;
        case 361: g = G(fnabs); goto s2;
        case 377: g = G(fneg); goto s2;
        case 75: g = G(fsqrt); goto s2;
        case 11: g = G(fsqrts); goto s2;
        case 73: g = G(frin); goto s2;
        case 89: g = G(friz); goto s2;
        case 105: g = G(frip); goto s2;
        case 121: g = G(frim); goto s2;
        case 107: g = G(fric); goto s2;
        case 281: g = G(frsp); goto s2;
        case 90: g = G(fre); goto s2;
        case 74: g = G(frsqrte); goto s2;
        s2:
            two(state, g, dw0(xt), dw0(xb));
            return 1;
        case 203: g = G(xvsqrtdp); goto v2;
        case 139: g = G(xvsqrtsp); goto v2;
        case 409: g = G(xvabssp); goto v2;
        case 441: g = G(xvnegsp); goto v2;
        case 425: g = G(xvnabssp); goto v2;
        case 393: g = G(xvcvdpsp); goto v2;
        case 457: g = G(xvcvspdp); goto v2;
        case 184: g = G(xvcvsxwsp); goto v2;
        case 168: g = G(xvcvuxwsp); goto v2;
        case 152: g = G(xvcvspsxws); goto v2;
        case 136: g = G(xvcvspuxws); goto v2;
        case 504: g = G(xvcvsxddp); goto v2;
        case 488: g = G(xvcvuxddp); goto v2;
        case 472: g = G(xvcvdpsxds); goto v2;
        case 456: g = G(xvcvdpuxds); goto v2;
        case 248: g = G(xvcvsxwdp); goto v2;
        case 232: g = G(xvcvuxwdp); goto v2;
        case 216: g = G(xvcvdpsxws); goto v2;
        case 200: g = G(xvcvdpuxws); goto v2;
        case 408: g = G(xvcvspsxds); goto v2;
        case 392: g = G(xvcvspuxds); goto v2;
        case 440: g = G(xvcvsxdsp); goto v2;
        case 424: g = G(xvcvuxdsp); goto v2;
        case 201: g = G(xvrdpi); goto v2;
        case 217: g = G(xvrdpiz); goto v2;
        case 233: g = G(xvrdpip); goto v2;
        case 249: g = G(xvrdpim); goto v2;
        case 235: g = G(xvrdpic); goto v2;
        case 137: g = G(xvrspi); goto v2;
        case 153: g = G(xvrspiz); goto v2;
        case 169: g = G(xvrspip); goto v2;
        case 185: g = G(xvrspim); goto v2;
        case 171: g = G(xvrspic); goto v2;
        case 218: g = G(xvredp); goto v2;
        case 154: g = G(xvresp); goto v2;
        case 202: g = G(xvrsqrtedp); goto v2;
        case 138: g = G(xvrsqrtesp); goto v2;
        v2:
            two(state, g, vsr(xt), vsr(xb));
            return 1;
        case 505: two(state, G(xvnegdp), vsr(xt), vsr(xb)); return 1;
        case 473: two(state, G(xvabsdp), vsr(xt), vsr(xb)); return 1;
        case 489: two(state, G(xvnabsdp), vsr(xt), vsr(xb)); return 1;
    }
    return gen_ppc64_undefined(state, insn);
}

// ---- op 4: VMX ----------------------------------------------------------------------
static int gen_vmx(struct gen_state *state, uint32_t insn) {
    unsigned vt = 32 + ppc_rt(insn), va = 32 + ppc_ra(insn), vb = 32 + ppc_rb(insn);
    unsigned vc = 32 + ppc_rc(insn);
    unsigned long g;
    switch (ppc_vxo6(insn)) {
        case 42: // vsel: vc selects vb
            emit(state, G(xxsel)); emit(state, vsr(vt)); emit(state, vsr(va));
            emit(state, vsr(vb)); emit(state, vsr(vc));
            return 1;
        case 43: // vperm
            emit(state, G(vperm)); emit(state, vsr(vt)); emit(state, vsr(va));
            emit(state, vsr(vb)); emit(state, vsr(vc));
            return 1;
        case 46: // vmaddfp vt = va * vc + vb
        case 47: // vnmsubfp vt = -(va * vc - vb)
            emit(state, ppc_vxo6(insn) == 46 ? G(vmaddfp) : G(vnmsubfp));
            emit(state, vsr(vt)); emit(state, vsr(va));
            emit(state, vsr(vb)); emit(state, vsr(vc));
            return 1;
        case 44: { // vsldoi
            static const unsigned long sldoi[16] = {0,
                G(vsldoi1), G(vsldoi2), G(vsldoi3), G(vsldoi4), G(vsldoi5), G(vsldoi6),
                G(vsldoi7), G(vsldoi8), G(vsldoi9), G(vsldoi10), G(vsldoi11), G(vsldoi12),
                G(vsldoi13), G(vsldoi14), G(vsldoi15)};
            unsigned shb = (insn >> 6) & 15;
            if (shb == 0)
                two(state, G(vmov), vsr(vt), vsr(va));
            else
                three(state, sldoi[shb], vsr(vt), vsr(va), vsr(vb));
            return 1;
        }
    }
    switch (ppc_vxo11(insn)) {
        // VMX floating point
        case 10: g = G(vaddfp); goto v3;
        case 74: g = G(vsubfp); goto v3;
        case 1034: g = G(vmaxfp); goto v3;
        case 1098: g = G(vminfp); goto v3;
        case 266: g = G(vrefp); goto vf2;
        case 330: g = G(vrsqrtefp); goto vf2;
        case 394: g = G(vexptefp); goto vf2;
        case 458: g = G(vlogefp); goto vf2;
        case 522: g = G(vrfin); goto vf2;
        case 586: g = G(vrfiz); goto vf2;
        case 650: g = G(vrfip); goto vf2;
        case 714: g = G(vrfim); goto vf2;
        vf2:
            two(state, g, vsr(vt), vsr(vb));
            return 1;
        // the fixed-point conversions: UIM in the RA field, the scale an
        // operand (2^-UIM in single, 2^UIM in double)
        case 778: case 842:
            three(state, ppc_vxo11(insn) == 842 ? G(vcfsx) : G(vcfux), vsr(vt), vsr(vb),
                  (uint64_t) (127 - ppc_ra(insn)) << 23);
            return 1;
        case 906: case 970:
            three(state, ppc_vxo11(insn) == 970 ? G(vctsxs) : G(vctuxs), vsr(vt), vsr(vb),
                  (uint64_t) (1023 + ppc_ra(insn)) << 52);
            return 1;
        case 1028: g = G(vand); goto v3;
        case 1092: g = G(vandc); goto v3;
        case 1156: // vor, and vmr
            if (va == vb) {
                two(state, G(vmov), vsr(vt), vsr(va));
                return 1;
            }
            g = G(vor); goto v3;
        case 1220: // vxor
            if (va == vb) {
                three(state, G(vconst), vsr(vt), 0, 0);
                return 1;
            }
            g = G(vxor); goto v3;
        case 1284: g = G(vnor); goto v3;
        case 1348: g = G(vorc); goto v3;
        case 1412: g = G(vnand); goto v3;
        case 1668: g = G(veqv); goto v3;
        case 0: g = G(vaddubm); goto v3;
        case 64: g = G(vadduhm); goto v3;
        case 128: g = G(vadduwm); goto v3;
        case 192: g = G(vaddudm); goto v3;
        case 1024: g = G(vsububm); goto v3;
        case 1088: g = G(vsubuhm); goto v3;
        case 1152: g = G(vsubuwm); goto v3;
        case 1216: g = G(vsubudm); goto v3;
        case 2: g = G(vmaxub); goto v3;
        case 66: g = G(vmaxuh); goto v3;
        case 130: g = G(vmaxuw); goto v3;
        case 258: g = G(vmaxsb); goto v3;
        case 322: g = G(vmaxsh); goto v3;
        case 386: g = G(vmaxsw); goto v3;
        case 514: g = G(vminub); goto v3;
        case 578: g = G(vminuh); goto v3;
        case 642: g = G(vminuw); goto v3;
        case 770: g = G(vminsb); goto v3;
        case 834: g = G(vminsh); goto v3;
        case 898: g = G(vminsw); goto v3;
        case 512: g = G(vaddubs); goto v3;
        case 576: g = G(vadduhs); goto v3;
        case 640: g = G(vadduws); goto v3;
        case 768: g = G(vaddsbs); goto v3;
        case 832: g = G(vaddshs); goto v3;
        case 896: g = G(vaddsws); goto v3;
        case 1536: g = G(vsububs); goto v3;
        case 1600: g = G(vsubuhs); goto v3;
        case 1664: g = G(vsubuws); goto v3;
        case 1792: g = G(vsubsbs); goto v3;
        case 1856: g = G(vsubshs); goto v3;
        case 1920: g = G(vsubsws); goto v3;
        case 1026: g = G(vavgub); goto v3;
        case 1090: g = G(vavguh); goto v3;
        case 1154: g = G(vavguw); goto v3;
        case 1282: g = G(vavgsb); goto v3;
        case 1346: g = G(vavgsh); goto v3;
        case 1410: g = G(vavgsw); goto v3;
        case 137: g = G(vmuluwm); goto v3;
        case 260: g = G(vslb); goto v3;
        case 324: g = G(vslh); goto v3;
        case 388: g = G(vslw); goto v3;
        case 1476: g = G(vsld); goto v3;
        case 516: g = G(vsrb); goto v3;
        case 580: g = G(vsrh); goto v3;
        case 644: g = G(vsrw); goto v3;
        case 1732: g = G(vsrd); goto v3;
        case 772: g = G(vsrab); goto v3;
        case 836: g = G(vsrah); goto v3;
        case 900: g = G(vsraw); goto v3;
        case 964: g = G(vsrad); goto v3;
        case 4: g = G(vrlb); goto v3;
        case 68: g = G(vrlh); goto v3;
        case 132: g = G(vrlw); goto v3;
        case 12: g = G(vmrghb); goto v3;
        case 76: g = G(vmrghh); goto v3;
        case 140: g = G(vmrghw); goto v3;
        case 268: g = G(vmrglb); goto v3;
        case 332: g = G(vmrglh); goto v3;
        case 396: g = G(vmrglw); goto v3;
        v3:
            three(state, g, vsr(vt), vsr(va), vsr(vb));
            return 1;
        // the splats: UIM in the RA field, big-endian element numbering
        case 1292: two(state, G(vgbbd), vsr(vt), vsr(vb)); return 1;
        case 520: g = G(vmuleub); goto v3;
        case 8: g = G(vmuloub); goto v3;
        case 584: g = G(vmuleuh); goto v3;
        case 72: g = G(vmulouh); goto v3;
        case 648: g = G(vmuleuw); goto v3;
        case 136: g = G(vmulouw); goto v3;
        case 776: g = G(vmulesb); goto v3;
        case 264: g = G(vmulosb); goto v3;
        case 840: g = G(vmulesh); goto v3;
        case 328: g = G(vmulosh); goto v3;
        case 904: g = G(vmulesw); goto v3;
        case 392: g = G(vmulosw); goto v3;
        case 14: g = G(vpkuhum); goto v3;
        case 78: g = G(vpkuwum); goto v3;
        case 1102: g = G(vpkudum); goto v3;
        case 398: g = G(vpkshss); goto v3;
        case 462: g = G(vpkswss); goto v3;
        case 1486: g = G(vpksdss); goto v3;
        case 270: g = G(vpkshus); goto v3;
        case 334: g = G(vpkswus); goto v3;
        case 1358: g = G(vpksdus); goto v3;
        case 142: g = G(vpkuhus); goto v3;
        case 206: g = G(vpkuwus); goto v3;
        case 1230: g = G(vpkudus); goto v3;
        case 1928: g = G(vsumsws); goto v3;
        case 1672: g = G(vsum2sws); goto v3;
        case 1544: g = G(vsum4ubs); goto v3;
        case 1800: g = G(vsum4sbs); goto v3;
        case 1608: g = G(vsum4shs); goto v3;
        case 256: g = G(vadduqm); goto v3;
        case 1280: g = G(vsubuqm); goto v3;
        case 384: g = G(vaddcuw); goto v3;
        case 1408: g = G(vsubcuw); goto v3;
        case 1932: g = G(vmrgew); goto v3;
        case 1676: g = G(vmrgow); goto v3;
        case 450: g = G(vmaxsd); goto v3;
        case 194: g = G(vmaxud); goto v3;
        case 962: g = G(vminsd); goto v3;
        case 706: g = G(vminud); goto v3;
        case 196: g = G(vrld); goto v3;
        case 526: two(state, G(vupkhsb), vsr(vt), vsr(vb)); return 1;
        case 654: two(state, G(vupklsb), vsr(vt), vsr(vb)); return 1;
        case 590: two(state, G(vupkhsh), vsr(vt), vsr(vb)); return 1;
        case 718: two(state, G(vupklsh), vsr(vt), vsr(vb)); return 1;
        case 1614: two(state, G(vupkhsw), vsr(vt), vsr(vb)); return 1;
        case 1742: two(state, G(vupklsw), vsr(vt), vsr(vb)); return 1;
        case 1795: two(state, G(vpopcntb), vsr(vt), vsr(vb)); return 1;
        case 1859: two(state, G(vpopcnth), vsr(vt), vsr(vb)); return 1;
        case 1923: two(state, G(vpopcntw), vsr(vt), vsr(vb)); return 1;
        case 1987: two(state, G(vpopcntd), vsr(vt), vsr(vb)); return 1;
        case 1794: two(state, G(vclzb), vsr(vt), vsr(vb)); return 1;
        case 1858: two(state, G(vclzh), vsr(vt), vsr(vb)); return 1;
        case 1922: two(state, G(vclzw), vsr(vt), vsr(vb)); return 1;
        case 1986: two(state, G(vclzd), vsr(vt), vsr(vb)); return 1;
        case 1356: g = G(vbpermq); goto v3;
        case 1036: g = G(vslo); goto v3;
        case 1100: g = G(vsro); goto v3;
        case 452: g = G(vsl); goto v3;
        case 708: g = G(vsr); goto v3;
        case 1540: emit(state, G(mfvscr)); emit(state, vsr(vt)); return 1;
        case 1604: emit(state, G(mtvscr)); emit(state, vsr(vb)); return 1;
        case 524: // vspltb
            two(state, G(vspltb), vsr(vt), vsr(vb) + (15 - (ppc_ra(insn) & 15)));
            return 1;
        case 588: // vsplth
            two(state, G(vsplth), vsr(vt), vsr(vb) + 2 * (7 - (ppc_ra(insn) & 7)));
            return 1;
        case 652: // vspltw
            two(state, G(vspltw), vsr(vt), vsr(vb) + 4 * (3 - (ppc_ra(insn) & 3)));
            return 1;
        case 780: case 844: case 908: { // vspltisb, vspltish, vspltisw: SIM, a 5-bit signed value
            int64_t sim = (int64_t) ((int32_t) (ppc_ra(insn) << 27) >> 27);
            uint64_t v;
            if (ppc_vxo11(insn) == 780)
                v = 0x0101010101010101ULL * (uint8_t) sim;
            else if (ppc_vxo11(insn) == 844)
                v = 0x0001000100010001ULL * (uint16_t) sim;
            else
                v = 0x0000000100000001ULL * (uint32_t) sim;
            three(state, G(vconst), vsr(vt), v, v);
            return 1;
        }
    }
    // the compares (VC form); a record form sets CR6 from the result
    {
        switch (ppc_vxo10(insn)) {
            case 6: g = G(vcmpequb); goto vc;
            case 70: g = G(vcmpequh); goto vc;
            case 134: g = G(vcmpequw); goto vc;
            case 199: g = G(vcmpequd); goto vc;
            case 518: g = G(vcmpgtub); goto vc;
            case 582: g = G(vcmpgtuh); goto vc;
            case 646: g = G(vcmpgtuw); goto vc;
            case 711: g = G(vcmpgtud); goto vc;
            case 774: g = G(vcmpgtsb); goto vc;
            case 838: g = G(vcmpgtsh); goto vc;
            case 902: g = G(vcmpgtsw); goto vc;
            case 967: g = G(vcmpgtsd); goto vc;
            case 198: g = G(vcmpeqfp); goto vc;
            case 454: g = G(vcmpgefp); goto vc;
            case 710: g = G(vcmpgtfp); goto vc;
            case 966: g = G(vcmpbfp); goto vc;
            vc:
                three(state, g, vsr(vt), vsr(va), vsr(vb));
                if ((insn >> 10) & 1) {
                    emit(state, G(cr6));
                    emit(state, vsr(vt));
                }
                return 1;
        }
    }
    return gen_ppc64_undefined(state, insn);
}

// ---- the step ---------------------------------------------------------------------
static int gen_step_ppc64_insn(struct gen_state *state, struct tlb *tlb);

// ISH_PPC64_ONE_PER_BLOCK=1: every instruction its own block, to tell a
// gadget's bug from one in how a block strings them together.
int gen_step_ppc64(struct gen_state *state, struct tlb *tlb) {
    static int one = -1;
    if (one < 0) {
        const char *e = getenv("ISH_PPC64_ONE_PER_BLOCK");
        one = e != NULL && e[0] == '1';
    }
    int ret = gen_step_ppc64_insn(state, tlb);
    if (one && ret) {
        gen_exit_ppc64(state);
        return 0;
    }
    return ret;
}

static int gen_step_ppc64_insn(struct gen_state *state, struct tlb *tlb) {
    state->ppc64_orig_ip = state->ppc64_ip;
    state->orig_ip_extra = 0;
    uint32_t insn;
    // an instruction fetch: a page without PROT_EXEC refuses it (as gen.c's
    // fetches all do), and the fault is INT_PF_EXEC
    if (!tlb_fetch(tlb, state->ppc64_ip, &insn, sizeof(insn)))
        return gen_ppc64_interrupt_at(state, tlb->fetch_denied ? INT_PF_EXEC : INT_GPF,
                state->ppc64_orig_ip, state->ppc64_orig_ip);
    guest_addr_t pc = state->ppc64_ip;
    guest_addr_t next = pc + 4;
    state->ppc64_ip = next;

    unsigned rt = ppc_rt(insn), ra = ppc_ra(insn);
    int64_t si = ppc_si(insn);
    uint64_t ui = ppc_ui(insn);

    switch (ppc_op(insn)) {
        case 2: case 3: // tdi, twi: TO 0 is nothing
            if (rt == 0)
                return 1;
            emit(state, ppc_op(insn) == 3 ? G(twi) : G(tdi));
            emit(state, gpr(ra));
            emit(state, (uint64_t) si);
            emit(state, rt);
            emit(state, state->ppc64_orig_ip);
            return 1;
        case 4:
            return gen_vmx(state, insn);
        case 7:
            three(state, G(mulli), gpr(rt), gpr(ra), (uint64_t) si);
            return 1;
        case 8:
            three(state, G(subfic), gpr(rt), gpr(ra), (uint64_t) si);
            return 1;
        case 10: // cmpli
            three(state, ppc_l(insn) ? G(cmpldi) : G(cmplwi), crf(ppc_bf(insn)), gpr(ra), ui);
            return 1;
        case 11: // cmpi
            three(state, ppc_l(insn) ? G(cmpdi) : G(cmpwi), crf(ppc_bf(insn)), gpr(ra), (uint64_t) si);
            return 1;
        case 12: case 13: // addic, addic.
            three(state, G(addic), gpr(rt), gpr(ra), (uint64_t) si);
            if (ppc_op(insn) == 13) {
                emit(state, G(cr0));
                emit(state, gpr(rt));
            }
            return 1;
        case 14: // addi, li
            if (ra == 0)
                mov_const(state, gpr(rt), (uint64_t) si);
            else
                three(state, G(addi), gpr(rt), gpr(ra), (uint64_t) si);
            return 1;
        case 15: // addis, lis
            if (ra == 0)
                mov_const(state, gpr(rt), (uint64_t) (si << 16));
            else
                three(state, G(addi), gpr(rt), gpr(ra), (uint64_t) (si << 16));
            return 1;
        case 16: { // bc
            unsigned bo = rt, bi = ra;
            guest_addr_t taken = (ppc_aa(insn) ? 0 : pc) + ppc_bd(insn);
            if (ppc_rcbit(insn)) {
                // bcl 20,31,$+4, the way to read the pc: only LR changes
                if ((bo & 0x14) == 0x14 && taken == next) {
                    mov_const(state, LR_OFF, next);
                    return 1;
                }
                mov_const(state, LR_OFF, next);
            }
            return gen_bc(state, bo, bi, taken, next);
        }
        case 17: // sc (LEV 0); scv is not offered
            if ((insn & 3) != 2)
                return gen_ppc64_undefined(state, insn);
            emit(state, G(sc));
            emit(state, next);
            return 0;
        case 18: { // b, ba, bl, bla
            guest_addr_t dest = (ppc_aa(insn) ? 0 : pc) + ppc_li(insn);
            if (ppc_rcbit(insn)) {
                emit(state, G(bl));
                emit(state, next);
            } else {
                emit(state, G(b));
            }
            target(state, 0, dest);
            return 0;
        }
        case 19:
            return gen_op19(state, insn, next);
        case 20: case 21: case 23: { // rlwimi, rlwinm, rlwnm
            unsigned mb = ppc_rc(insn), me = ppc_me(insn), sh = ppc_rb(insn);
            uint64_t m = mask64(mb + 32, me + 32);
            unsigned long g = ppc_op(insn) == 20 ? G(rotw_ins) : ppc_op(insn) == 21 ? G(rotw) : G(rotw_r);
            emit(state, g);
            emit(state, gpr(ra));
            emit(state, gpr(rt));
            emit(state, ppc_op(insn) == 23 ? gpr(sh) : (unsigned long) ((32 - sh) & 31));
            emit(state, m);
            record(state, insn, gpr(ra));
            return 1;
        }
        case 24: // ori (ori 0,0,0 is nop)
            if (rt == 0 && ra == 0 && ui == 0)
                return 1;
            three(state, G(ori), gpr(ra), gpr(rt), ui);
            return 1;
        case 25:
            three(state, G(ori), gpr(ra), gpr(rt), ui << 16);
            return 1;
        case 26:
            three(state, G(xori), gpr(ra), gpr(rt), ui);
            return 1;
        case 27:
            three(state, G(xori), gpr(ra), gpr(rt), ui << 16);
            return 1;
        case 28: case 29: // andi., andis.
            three(state, G(andi), gpr(ra), gpr(rt), ppc_op(insn) == 28 ? ui : ui << 16);
            emit(state, G(cr0));
            emit(state, gpr(ra));
            return 1;
        case 30:
            return gen_op30(state, insn);
        case 31:
            return gen_op31(state, insn);

        // D-form loads and stores
#define LD(n, name) case n: ldst(state, G(name), gpr(rt), gpr0(ra), (uint64_t) si); return 1;
#define LDU(n, name) case n: \
            if (ra == 0 || ra == rt) return gen_ppc64_undefined(state, insn); \
            ldst(state, G(name), gpr(rt), gpr(ra), (uint64_t) si); return 1;
#define STU(n, name) case n: \
            if (ra == 0) return gen_ppc64_undefined(state, insn); \
            ldst(state, G(name), gpr(rt), gpr(ra), (uint64_t) si); return 1;
        LD(32, lwz) LDU(33, lwzu) LD(34, lbz) LDU(35, lbzu)
        LD(36, stw) STU(37, stwu) LD(38, stb) STU(39, stbu)
        LD(40, lhz) LDU(41, lhzu) LD(42, lha) LDU(43, lhau)
        LD(44, sth) STU(45, sthu)
#undef LD
#undef LDU
#undef STU
#define FD(n, name) case n: ldst(state, G(name), fpr(rt), gpr0(ra), (uint64_t) si); return 1;
#define FDU(n, name) case n: \
            if (ra == 0) return gen_ppc64_undefined(state, insn); \
            ldst(state, G(name), fpr(rt), gpr(ra), (uint64_t) si); return 1;
        FD(48, lfs) FDU(49, lfsu) FD(50, lfd) FDU(51, lfdu)
        FD(52, stfs) FDU(53, stfsu) FD(54, stfd) FDU(55, stfdu)
#undef FD
#undef FDU
        case 58: { // DS: ld, ldu, lwa
            int64_t ds = ppc_ds(insn);
            switch (ppc_ds_xo(insn)) {
                case 0: ldst(state, G(ld), gpr(rt), gpr0(ra), (uint64_t) ds); return 1;
                case 1:
                    if (ra == 0 || ra == rt)
                        return gen_ppc64_undefined(state, insn);
                    ldst(state, G(ldu), gpr(rt), gpr(ra), (uint64_t) ds);
                    return 1;
                case 2: ldst(state, G(lwa), gpr(rt), gpr0(ra), (uint64_t) ds); return 1;
            }
            return gen_ppc64_undefined(state, insn);
        }
        case 62: { // DS: std, stdu
            int64_t ds = ppc_ds(insn);
            switch (ppc_ds_xo(insn)) {
                case 0: ldst(state, G(std), gpr(rt), gpr0(ra), (uint64_t) ds); return 1;
                case 1:
                    if (ra == 0)
                        return gen_ppc64_undefined(state, insn);
                    ldst(state, G(stdu), gpr(rt), gpr(ra), (uint64_t) ds);
                    return 1;
            }
            return gen_ppc64_undefined(state, insn);
        }
        case 59:
            return gen_fp(state, insn, true);
        case 63:
            return gen_fp(state, insn, false);
        case 60:
            return gen_vsx(state, insn);
    }
    return gen_ppc64_undefined(state, insn);
}
