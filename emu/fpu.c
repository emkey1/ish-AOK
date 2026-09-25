// I don't remember if the interpreter was supposed to use this in addition to the jit
#include <fenv.h>
#include <math.h>
#include <string.h>
#include "emu/cpu.h"
#include "emu/float80.h"
#include "emu/fpenv.h"
#include "emu/fpu.h"
#include "emu/fxsave.h"

#define ST(i) cpu->fp[(cpu->top + i) % 8]

// Around every arithmetic helper: float80 reports what the operation raised
// (f80_exceptions in status-word order, f80_inexact for PE) and which way it
// rounded, and it lands in the status word. The exception flags are sticky --
// only FNCLEX, FNINIT and FLDENV/FRSTOR clear them -- while C1 describes the
// most recent operation alone, so it is assigned rather than accumulated.
// FPU_END_FLAGS is for the operations whose C1 means something else.
#define FPU_BEGIN() do { f80_inexact = 0; f80_rounded_up = 0; f80_exceptions = 0; } while (0)
#define FPU_END_FLAGS() do {                            \
    cpu->fsw |= (f80_exceptions & 0x1f) | (f80_inexact ? 0x20 : 0); \
} while (0)
#define FPU_END() do {                                  \
    FPU_END_FLAGS();                                    \
    cpu->c1 = f80_rounded_up ? 1 : 0;                   \
} while (0)

// The operations still computed in host double (the transcendentals) run in
// the host's own round-to-nearest, whatever the guest's SSE mode has put in
// the host FPCR, and what they raise stays out of the guest's MXCSR -- an
// x87 instruction reports to the x87 status word only.
static double host_libm2(double (*fn)(double, double), double x, double y) {
    fenv_t env;
    feholdexcept(&env);
    fesetround(FE_TONEAREST);
    double r = fn(x, y);
    fesetenv(&env);
    return r;
}
static double host_libm1(double (*fn)(double), double x) {
    fenv_t env;
    feholdexcept(&env);
    fesetround(FE_TONEAREST);
    double r = fn(x);
    fesetenv(&env);
    return r;
}
// A memory operand, widened bit for bit (see f80_from_float).
#define FPU_M(x) _Generic((x), float: f80_from_float, double: f80_from_double)(x)

// A transcendental result is inexact unless it is the exact zero it has at 0.
static void fpu_transcendental_inexact(struct cpu_state *cpu, float80 result) {
    if (!f80_iszero(result) && !f80_isnan(result))
        cpu->pe = 1;
}

static void fpu_push(struct cpu_state *cpu, float80 f) {
    cpu->top--;
    ST(0) = f;
}
#define fpush(f) fpu_push(cpu, f)
void fpu_pop(struct cpu_state *cpu) {
    cpu->top++;
}

void fpu_xch(struct cpu_state *cpu, int i) {
    float80 tmp = ST(0);
    ST(0) = ST(i);
    ST(i) = tmp;
}

void fpu_incstp(struct cpu_state *cpu) {
    // This is different from just popping the stack, it doesn't tag the stack
    // element as free. We don't have stack tagging yet so in practice there's
    // no difference.
    cpu->top++;
}

// FDECSTP (D9 F6), fincstp's mirror: TOP moves down one and nothing else
// happens. Missing, like FPTAN and FYL2XP1 below, so it raised SIGILL.
void fpu_decstp(struct cpu_state *cpu) {
    cpu->top--;
}

// loads

void fpu_ld(struct cpu_state *cpu, int i) {
    fpush(ST(i));
}

void fpu_ldc(struct cpu_state *cpu, enum fpu_const c) {
    fpush(fpu_consts[c]);
}

void fpu_ild16(struct cpu_state *cpu, int16_t *i) {
    fpush(f80_from_int(*i));
}
void fpu_ild32(struct cpu_state *cpu, int32_t *i) {
    fpush(f80_from_int(*i));
}
void fpu_ild64(struct cpu_state *cpu, int64_t *i) {
    fpush(f80_from_int(*i));
}

// FLD m32/m64 raise IE for a signalling NaN and DE for a denormal.
void fpu_ldm32(struct cpu_state *cpu, float32 *f) {
    FPU_BEGIN();
    fpush(f80_from_float(*f));
    FPU_END();
}
void fpu_ldm64(struct cpu_state *cpu, float64 *f) {
    FPU_BEGIN();
    fpush(FPU_M(*f));
    FPU_END();
}
void fpu_ldm80(struct cpu_state *cpu, float80 *f) {
    fpush(*f);
}

// stores

void fpu_st(struct cpu_state *cpu, int i) {
    ST(i) = ST(0);
}

// FIST: a value that does not fit is the integer indefinite, and invalid --
// which also means nothing about rounding is reported.
static int64_t fpu_fit_int(int64_t res, int64_t min, int64_t max) {
    if (res < min || res > max) {
        f80_exceptions |= F80_EXC_INVALID;
        f80_inexact = f80_rounded_up = 0;
        return min;
    }
    return res;
}
void fpu_ist16(struct cpu_state *cpu, int16_t *i) {
    FPU_BEGIN();
    *i = (int16_t) fpu_fit_int(f80_to_int(ST(0)), INT16_MIN, INT16_MAX);
    FPU_END();
}
void fpu_ist32(struct cpu_state *cpu, int32_t *i) {
    FPU_BEGIN();
    *i = (int32_t) fpu_fit_int(f80_to_int(ST(0)), INT32_MIN, INT32_MAX);
    FPU_END();
}
void fpu_ist64(struct cpu_state *cpu, int64_t *i) {
    FPU_BEGIN();
    *i = f80_to_int(ST(0));
    FPU_END();
}

// fisttp (SSE3): store ST(0) as an integer with truncation toward zero,
// regardless of the control-word rounding mode (the caller then pops). gcc
// -msse3 emits this for float/double -> int casts.
void fpu_istt16(struct cpu_state *cpu, int16_t *i) {
    enum f80_rounding_mode old_mode = f80_rounding_mode;
    f80_rounding_mode = round_chop;
    FPU_BEGIN();
    *i = (int16_t) fpu_fit_int(f80_to_int(ST(0)), INT16_MIN, INT16_MAX);
    FPU_END();
    f80_rounding_mode = old_mode;
}
void fpu_istt32(struct cpu_state *cpu, int32_t *i) {
    enum f80_rounding_mode old_mode = f80_rounding_mode;
    f80_rounding_mode = round_chop;
    FPU_BEGIN();
    *i = (int32_t) fpu_fit_int(f80_to_int(ST(0)), INT32_MIN, INT32_MAX);
    FPU_END();
    f80_rounding_mode = old_mode;
}
void fpu_istt64(struct cpu_state *cpu, int64_t *i) {
    enum f80_rounding_mode old_mode = f80_rounding_mode;
    f80_rounding_mode = round_chop;
    FPU_BEGIN();
    *i = f80_to_int(ST(0));
    FPU_END();
    f80_rounding_mode = old_mode;
}

// FST m32/m64 round in the x87's own mode, straight to the destination: the
// single-precision store used to round to double first and then again on
// the host, in the host's (the guest's SSE) mode.
void fpu_stm32(struct cpu_state *cpu, float32 *f) {
    FPU_BEGIN();
    *f = f80_to_float(ST(0));
    FPU_END();
}
void fpu_stm64(struct cpu_state *cpu, float64 *f) {
    FPU_BEGIN();
    *f = f80_to_double(ST(0));
    FPU_END();
}
void fpu_stm80(struct cpu_state *cpu, float80 *f) {
    // intel guarantees this will only write 10 bytes, not 12 or anything weird like that
    memcpy(f, &ST(0), 10);
}

// moves

#define FCMOVcc(instr, cond) \
    void fpu_cmov##instr(struct cpu_state *cpu, int i) { \
        if (cond) \
            ST(0) = ST(i); \
    }
// Read the conditions through the ZF/PF/CF macros, not cpu->zf and friends
// directly: ZF and PF are lazy, and while the flag-producing instruction's
// result is still sitting in cpu->res the raw fields hold whatever was last
// materialized. glibc's i386 sin()/cos() apply their sign with
// "and $0x2,%ecx; fchs; fcmove %st(1),%st" -- an fcmove on a ZF that was only
// just set -- so a stale read silently negated the result: exactly the right
// magnitude, wrong sign.
FCMOVcc(b, CF)
FCMOVcc(e, ZF)
FCMOVcc(be, CF | ZF)
FCMOVcc(u, PF)
FCMOVcc(nb, !CF)
FCMOVcc(ne, !ZF)
FCMOVcc(nbe, !(CF | ZF))
FCMOVcc(nu, !PF)

// math

void fpu_prem(struct cpu_state *cpu) {
    FPU_BEGIN();
    ST(0) = f80_mod(ST(0), ST(1));
    FPU_END_FLAGS();
    cpu->c2 = 0; // say we finished the entire remainder
}

// FPREM1 (D9 F5): the IEEE-754 remainder, which rounds the implied quotient to
// nearest-even where FPREM truncates it. It was missing entirely -- f80_rem had
// been declared for it and never written -- so every FPREM1 raised SIGILL.
// glibc's remainder()/remquo() and drem() are the callers that matter.
void fpu_prem1(struct cpu_state *cpu) {
    FPU_BEGIN();
    ST(0) = f80_rem(ST(0), ST(1));
    FPU_END_FLAGS();
    cpu->c2 = 0; // complete reduction, as fpu_prem also reports
}

// ST(1) is truncated inside f80_fscale. Converting it to a C int here made a
// scale of 2^32, an infinity and a NaN all 0.
void fpu_scale(struct cpu_state *cpu) {
    FPU_BEGIN();
    ST(0) = f80_fscale(ST(0), ST(1));
    FPU_END();
}

void fpu_rndint(struct cpu_state *cpu) {
    if (f80_isinf(ST(0)) || f80_isnan(ST(0)))
        return;
    FPU_BEGIN();
    ST(0) = f80_round(ST(0));
    FPU_END();
}

void fpu_sqrt(struct cpu_state *cpu) {
    FPU_BEGIN();
    ST(0) = f80_sqrt(ST(0));
    FPU_END();
}

void fpu_yl2x(struct cpu_state *cpu) {
    FPU_BEGIN();
    ST(1) = f80_mul(ST(1), f80_log2(ST(0)));
    FPU_END();
    fpu_pop(cpu);
}

// FYL2XP1 (D9 F9): ST(1) = ST(1) * log2(ST(0) + 1), then pop. It exists for
// accuracy when ST(0) is tiny, where forming 1 + ST(0) first throws the
// answer away; f80_log2p1 never forms it.
void fpu_yl2xp1(struct cpu_state *cpu) {
    FPU_BEGIN();
    ST(1) = f80_mul(ST(1), f80_log2p1(ST(0)));
    FPU_END();
    fpu_pop(cpu);
}

void fpu_2xm1(struct cpu_state *cpu) {
    // an example of the ancient chinese art of chi ting
    ST(0) = f80_from_double(host_libm2(pow, 2, f80_to_double(ST(0))) - 1);
    fpu_transcendental_inexact(cpu, ST(0));
}

// FCOM, FCOMI and FTST are signalling compares: any NaN is an invalid
// operation. FUCOM and FUCOMI raise IE only for a signalling one.
static void fpu_compare_invalid(struct cpu_state *cpu, float80 a, float80 b, bool quiet) {
    if (!f80_is_supported(a) || !f80_is_supported(b) ||
            f80_issnan(a) || f80_issnan(b) ||
            (!quiet && (f80_isnan(a) || f80_isnan(b))))
        cpu->ie = 1;
}

static void fpu_comparei(struct cpu_state *cpu, float80 x) {
    cpu->zf_res = cpu->pf_res = 0;
    cpu->zf = cpu->pf = cpu->cf = 0;
    cpu->cf = f80_lt(ST(0), x);
    cpu->zf = f80_eq(ST(0), x);
    if (f80_uncomparable(ST(0), x))
        cpu->zf = cpu->pf = cpu->cf = 1;
}
static void fpu_compare(struct cpu_state *cpu, float80 x) {
    cpu->c2 = cpu->c1 = 0;
    cpu->c0 = f80_lt(ST(0), x);
    cpu->c3 = f80_eq(ST(0), x);
    if (f80_uncomparable(ST(0), x))
        cpu->c0 = cpu->c2 = cpu->c3 = 1;
}
void fpu_com(struct cpu_state *cpu, int i) {
    fpu_compare_invalid(cpu, ST(0), ST(i), false);
    fpu_compare(cpu, ST(i));
}
void fpu_ucom(struct cpu_state *cpu, int i) {
    fpu_compare_invalid(cpu, ST(0), ST(i), true);
    fpu_compare(cpu, ST(i));
}
void fpu_comi(struct cpu_state *cpu, int i) {
    fpu_compare_invalid(cpu, ST(0), ST(i), false);
    fpu_comparei(cpu, ST(i));
}
void fpu_ucomi(struct cpu_state *cpu, int i) {
    fpu_compare_invalid(cpu, ST(0), ST(i), true);
    fpu_comparei(cpu, ST(i));
}
void fpu_comm32(struct cpu_state *cpu, float *f) {
    FPU_BEGIN();
    float80 x = f80_from_float(*f);
    FPU_END_FLAGS();
    fpu_compare_invalid(cpu, ST(0), x, false);
    fpu_compare(cpu, x);
}
void fpu_comm64(struct cpu_state *cpu, double *f) {
    FPU_BEGIN();
    float80 x = FPU_M(*f);
    FPU_END_FLAGS();
    fpu_compare_invalid(cpu, ST(0), x, false);
    fpu_compare(cpu, x);
}
void fpu_icom16(struct cpu_state *cpu, int16_t *i) {
    fpu_compare(cpu, f80_from_int(*i));
}
void fpu_icom32(struct cpu_state *cpu, int32_t *i) {
    fpu_compare(cpu, f80_from_int(*i));
}
void fpu_tst(struct cpu_state *cpu) {
    fpu_compare_invalid(cpu, ST(0), fpu_consts[fconst_zero], false);
    fpu_compare(cpu, fpu_consts[fconst_zero]);
}

void fpu_abs(struct cpu_state *cpu) {
    ST(0) = f80_abs(ST(0));
}

void fpu_chs(struct cpu_state *cpu) {
    ST(0) = f80_neg(ST(0));
}

void fpu_add(struct cpu_state *cpu, int srci, int dsti) {
    FPU_BEGIN();
    ST(dsti) = f80_add(ST(dsti), ST(srci));
    FPU_END();
}
void fpu_sub(struct cpu_state *cpu, int srci, int dsti) {
    FPU_BEGIN();
    ST(dsti) = f80_sub(ST(dsti), ST(srci));
    FPU_END();
}
void fpu_subr(struct cpu_state *cpu, int srci, int dsti) {
    FPU_BEGIN();
    ST(dsti) = f80_sub(ST(srci), ST(dsti));
    FPU_END();
}
void fpu_mul(struct cpu_state *cpu, int srci, int dsti) {
    FPU_BEGIN();
    ST(dsti) = f80_mul(ST(dsti), ST(srci));
    FPU_END();
}
void fpu_div(struct cpu_state *cpu, int srci, int dsti) {
    FPU_BEGIN();
    ST(dsti) = f80_div(ST(dsti), ST(srci));
    FPU_END();
}
void fpu_divr(struct cpu_state *cpu, int srci, int dsti) {
    FPU_BEGIN();
    ST(dsti) = f80_div(ST(srci), ST(dsti));
    FPU_END();
}

void fpu_iadd16(struct cpu_state *cpu, int16_t *i) {
    FPU_BEGIN();
    ST(0) = f80_add(ST(0), f80_from_int(*i));
    FPU_END();
}
void fpu_isub16(struct cpu_state *cpu, int16_t *i) {
    FPU_BEGIN();
    ST(0) = f80_sub(ST(0), f80_from_int(*i));
    FPU_END();
}
void fpu_isubr16(struct cpu_state *cpu, int16_t *i) {
    FPU_BEGIN();
    ST(0) = f80_sub(f80_from_int(*i), ST(0));
    FPU_END();
}
void fpu_imul16(struct cpu_state *cpu, int16_t *i) {
    FPU_BEGIN();
    ST(0) = f80_mul(ST(0), f80_from_int(*i));
    FPU_END();
}
void fpu_idiv16(struct cpu_state *cpu, int16_t *i) {
    FPU_BEGIN();
    ST(0) = f80_div(ST(0), f80_from_int(*i));
    FPU_END();
}
void fpu_idivr16(struct cpu_state *cpu, int16_t *i) {
    FPU_BEGIN();
    ST(0) = f80_div(f80_from_int(*i), ST(0));
    FPU_END();
}

void fpu_iadd32(struct cpu_state *cpu, int32_t *i) {
    FPU_BEGIN();
    ST(0) = f80_add(ST(0), f80_from_int(*i));
    FPU_END();
}
void fpu_isub32(struct cpu_state *cpu, int32_t *i) {
    FPU_BEGIN();
    ST(0) = f80_sub(ST(0), f80_from_int(*i));
    FPU_END();
}
void fpu_isubr32(struct cpu_state *cpu, int32_t *i) {
    FPU_BEGIN();
    ST(0) = f80_sub(f80_from_int(*i), ST(0));
    FPU_END();
}
void fpu_imul32(struct cpu_state *cpu, int32_t *i) {
    FPU_BEGIN();
    ST(0) = f80_mul(ST(0), f80_from_int(*i));
    FPU_END();
}
void fpu_idiv32(struct cpu_state *cpu, int32_t *i) {
    FPU_BEGIN();
    ST(0) = f80_div(ST(0), f80_from_int(*i));
    FPU_END();
}
void fpu_idivr32(struct cpu_state *cpu, int32_t *i) {
    FPU_BEGIN();
    ST(0) = f80_div(f80_from_int(*i), ST(0));
    FPU_END();
}

void fpu_addm32(struct cpu_state *cpu, float32 *f) {
    FPU_BEGIN();
    ST(0) = f80_add(ST(0), FPU_M(*f));
    FPU_END();
}
void fpu_subm32(struct cpu_state *cpu, float32 *f) {
    FPU_BEGIN();
    ST(0) = f80_sub(ST(0), FPU_M(*f));
    FPU_END();
}
void fpu_subrm32(struct cpu_state *cpu, float32 *f) {
    FPU_BEGIN();
    ST(0) = f80_sub(FPU_M(*f), ST(0));
    FPU_END();
}
void fpu_mulm32(struct cpu_state *cpu, float32 *f) {
    FPU_BEGIN();
    ST(0) = f80_mul(ST(0), FPU_M(*f));
    FPU_END();
}
void fpu_divm32(struct cpu_state *cpu, float32 *f) {
    FPU_BEGIN();
    ST(0) = f80_div(ST(0), FPU_M(*f));
    FPU_END();
}
void fpu_divrm32(struct cpu_state *cpu, float32 *f) {
    FPU_BEGIN();
    ST(0) = f80_div(FPU_M(*f), ST(0));
    FPU_END();
}

void fpu_addm64(struct cpu_state *cpu, float64 *f) {
    FPU_BEGIN();
    ST(0) = f80_add(ST(0), FPU_M(*f));
    FPU_END();
}
void fpu_subm64(struct cpu_state *cpu, float64 *f) {
    FPU_BEGIN();
    ST(0) = f80_sub(ST(0), FPU_M(*f));
    FPU_END();
}
void fpu_subrm64(struct cpu_state *cpu, float64 *f) {
    FPU_BEGIN();
    ST(0) = f80_sub(FPU_M(*f), ST(0));
    FPU_END();
}
void fpu_mulm64(struct cpu_state *cpu, float64 *f) {
    FPU_BEGIN();
    ST(0) = f80_mul(ST(0), FPU_M(*f));
    FPU_END();
}
void fpu_divm64(struct cpu_state *cpu, float64 *f) {
    FPU_BEGIN();
    ST(0) = f80_div(ST(0), FPU_M(*f));
    FPU_END();
}
void fpu_divrm64(struct cpu_state *cpu, float64 *f) {
    FPU_BEGIN();
    ST(0) = f80_div(FPU_M(*f), ST(0));
    FPU_END();
}

void fpu_patan(struct cpu_state *cpu) {
    // there's no native atan2 for 80-bit float yet.
    ST(1) = f80_from_double(host_libm2(atan2, f80_to_double(ST(1)), f80_to_double(ST(0))));
    fpu_transcendental_inexact(cpu, ST(1));
    fpu_pop(cpu);
}

// fsin/fcos/fsincos report an operand the hardware can't range-reduce
// (|arg| >= 2^63) by setting C2 and leaving the stack untouched, and clear C2
// otherwise. We were doing neither: C2 was left at whatever a previous op set,
// and an out-of-range operand got sin()/cos() of a huge double, which is
// meaningless -- fsin(1e30) returned 0.00933 where hardware leaves 1e30 in
// place for the caller to reduce itself.
static bool fpu_trig_out_of_range(struct cpu_state *cpu, double arg) {
    if (fabs(arg) >= 9223372036854775808.0) { // 2^63
        cpu->c2 = 1;
        return true;
    }
    cpu->c2 = 0;
    return false;
}

void fpu_sin(struct cpu_state *cpu) {
    double arg = f80_to_double(ST(0));
    if (fpu_trig_out_of_range(cpu, arg))
        return;
    ST(0) = f80_from_double(host_libm1(sin, arg));
    fpu_transcendental_inexact(cpu, ST(0));
}
void fpu_cos(struct cpu_state *cpu) {
    double arg = f80_to_double(ST(0));
    if (fpu_trig_out_of_range(cpu, arg))
        return;
    ST(0) = f80_from_double(host_libm1(cos, arg));
    fpu_transcendental_inexact(cpu, ST(0));
}
// FPTAN (D9 F2): ST(0) = tan(ST(0)), then push 1.0 -- an 8087 leftover, so
// that an FDIVR after it gives the cotangent. Everything that uses it pops the
// 1.0 straight off again (32-bit HotSpot's Math.tan does exactly that). An
// operand out of range sets C2 and leaves the stack alone, as for fsin.
void fpu_ptan(struct cpu_state *cpu) {
    double arg = f80_to_double(ST(0));
    if (fpu_trig_out_of_range(cpu, arg))
        return;
    ST(0) = f80_from_double(host_libm1(tan, arg));
    fpu_transcendental_inexact(cpu, ST(0));
    fpush(fpu_consts[fconst_one]);
}
void fpu_sincos(struct cpu_state *cpu) {
    // ST(0) is replaced by sin, then cos is pushed, so on exit ST(0) is cos
    // and ST(1) is sin. Like fsin/fcos this goes through double rather than
    // computing at 80-bit precision.
    double arg = f80_to_double(ST(0));
    if (fpu_trig_out_of_range(cpu, arg))
        return;
    ST(0) = f80_from_double(host_libm1(sin, arg));
    fpu_transcendental_inexact(cpu, ST(0));
    fpush(f80_from_double(host_libm1(cos, arg)));
    cpu->pe = 1;
}

void fpu_xtract(struct cpu_state *cpu) {
    float80 exp, signif;
    FPU_BEGIN();
    f80_xtract(ST(0), &exp, &signif);
    FPU_END();
    ST(0) = exp;
    fpush(signif);
}

void fpu_xam(struct cpu_state *cpu) {
    float80 f = ST(0);
    int outflags = 0;
    if (!f80_is_supported(f)) {
        outflags = 0b000;
    } else if (f80_isnan(f)) {
        outflags = 0b001;
    } else if (f80_isinf(f)) {
        outflags = 0b011;
    } else if (f80_iszero(f)) {
        outflags = 0b100;
    } else if (f80_isdenormal(f)) {
        outflags = 0b110;
    } else {
        // normal.
        // todo: empty
        outflags = 0b010;
    }
    cpu->c1 = f.sign;
    cpu->c0 = outflags & 1;
    cpu->c2 = (outflags >> 1) & 1;
    cpu->c3 = (outflags >> 2) & 1;
}

// meta

void fpu_stcw16(struct cpu_state *cpu, uint16_t *i) {
    *i = cpu->fcw;
}
void fpu_stsw16(struct cpu_state *cpu, uint16_t *i) {
    *i = cpu->fsw;
}
// Control-word PC field -> significand bits. 01b is reserved; Intel treats it
// as extended, so do the same.
static int f80_precision_from_pc(unsigned pc) {
    switch (pc) {
        case 0: return 24;
        case 2: return 53;
        default: return 64;
    }
}

// float80's rounding mode and precision live in host-thread-local variables;
// the control word they follow belongs to the guest thread. fpenv_enter calls
// this on every entry to guest code, since sigreturn, ptrace, exec, fork and
// checkpoints all change cpu->fcw without an x87 instruction.
void fpu_sync_control(struct cpu_state *cpu) {
    f80_rounding_mode = cpu->rc;
    f80_precision = f80_precision_from_pc(cpu->pc);
}

void fpu_ldcw16(struct cpu_state *cpu, uint16_t *i) {
    cpu->fcw = *i;
    fpu_sync_control(cpu);
}

struct fpu_env32 {
    uint32_t control;
    uint32_t status;
    uint32_t tag;
    uint32_t ip;
    uint32_t ip_selector;
    uint32_t operand;
    uint32_t operand_selector;
};

void fpu_stenv32(struct cpu_state *cpu, struct fpu_env32 *env) {
    env->control = cpu->fcw;
    env->status = cpu->fsw;
    // hope nobody looks at these
    env->tag = 0;
    env->ip = env->ip_selector = 0;
    env->operand = env->operand_selector = 0;
}
void fpu_ldenv32(struct cpu_state *cpu, struct fpu_env32 *env) {
    cpu->fcw = env->control;
    cpu->fsw = env->status;
    // frstor/fldenv restore the control word too, so the live rounding and
    // precision state has to follow it.
    fpu_sync_control(cpu);
}

struct fpu_state32 {
    struct fpu_env32 env;
    uint8_t regs[8][10];
};

void fpu_save32(struct cpu_state *cpu, struct fpu_state32 *state) {
    fpu_stenv32(cpu, &state->env);
    for (int i = 0; i < 8; i++)
        memcpy(state->regs[i], &ST(i), 10);
}

void fpu_restore32(struct cpu_state *cpu, struct fpu_state32 *state) {
    fpu_ldenv32(cpu, &state->env);
    for (int i = 0; i < 8; i++)
        memcpy(&ST(i), state->regs[i], 10);
}

// FXSAVE/FXRSTOR and the MXCSR accessors for the i386 guest. The area layout
// and the cpu_state conversions are shared with the amd64 engine (emu/fxsave.h);
// 32-bit mode sees eight XMM registers, and the slots for the other eight stay
// zeroed as the reserved region of the 32-bit area requires.
//
// These existed only on the amd64 side until now. The i386 decoder folded the
// whole 0f ae group into a single "fence" case that read the modrm byte and
// fell through, so FXSAVE, FXRSTOR, LDMXCSR and STMXCSR were all silently
// skipped -- no fault, no diagnostic, just stale state -- while CPUID kept
// advertising fxsr and sse. tests/manual/x86/cpuid_xsave.c is what caught it
// and is what keeps it caught.
// MXCSR's flags are gathered from the host as the guest reads them, and its
// rounding mode goes onto the host as the guest writes it (emu/fpenv.c).
void fpu_fxsave32(struct cpu_state *cpu, struct fxsave_area *area) {
    fpenv_x86_sync_mxcsr(cpu);
    fxsave_fill(cpu, area, 8);
}

void fpu_fxrestore32(struct cpu_state *cpu, struct fxsave_area *area) {
    fxsave_restore(cpu, area, 8);
    fpenv_x86_load_mxcsr(cpu);
}

void fpu_stmxcsr32(struct cpu_state *cpu, dword_t *value) {
    fpenv_x86_sync_mxcsr(cpu);
    *value = cpu->mxcsr;
}

void fpu_ldmxcsr32(struct cpu_state *cpu, dword_t *value) {
    cpu->mxcsr = *value & 0xffff;
    fpenv_x86_load_mxcsr(cpu);
}

// FNINIT: control word back to 0x037f (all exceptions masked, round to
// nearest, extended precision), status word cleared -- which also resets TOP,
// since it is a field of fsw -- and every register marked empty. We do not
// model the tag word, so there is nothing to write for that. The control word
// changing means the live rounding and precision state has to follow it, the
// same way fldcw and frstor do.
void fpu_init(struct cpu_state *cpu) {
    cpu->fcw = 0x037f;
    cpu->fsw = 0;
    fpu_sync_control(cpu);
}

// FNCLEX clears the exception flags, the stack fault, ES and B. This cleared
// `sf` -- the sign flag of EFLAGS, not the status word's stack fault (stf) --
// so a branch on the sign after FNCLEX could go the wrong way.
void fpu_clex(struct cpu_state *cpu) {
    cpu->pe = cpu->ue = cpu->oe = cpu->ze = cpu->de = cpu->ie = cpu->es = cpu->stf = cpu->b = 0;
}
