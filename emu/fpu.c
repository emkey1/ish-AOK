// I don't remember if the interpreter was supposed to use this in addition to the jit
#include <math.h>
#include <string.h>
#include "emu/cpu.h"
#include "emu/float80.h"
#include "emu/fpu.h"
#include "emu/fxsave.h"

#define ST(i) cpu->fp[(cpu->top + i) % 8]

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

void fpu_ldm32(struct cpu_state *cpu, float32 *f) {
    fpush(f80_from_double(*f));
}
void fpu_ldm64(struct cpu_state *cpu, float64 *f) {
    fpush(f80_from_double(*f));
}
void fpu_ldm80(struct cpu_state *cpu, float80 *f) {
    fpush(*f);
}

// stores

void fpu_st(struct cpu_state *cpu, int i) {
    ST(i) = ST(0);
}

void fpu_ist16(struct cpu_state *cpu, int16_t *i) {
    int64_t res = f80_to_int(ST(0));
    if (res < INT16_MIN || res > INT16_MAX)
        res = INT16_MIN;
    *i = (int16_t) res;
}
void fpu_ist32(struct cpu_state *cpu, int32_t *i) {
    int64_t res = f80_to_int(ST(0));
    if (res < INT32_MIN || res > INT32_MAX)
        res = INT32_MIN;
    *i = (int32_t) res;
}
void fpu_ist64(struct cpu_state *cpu, int64_t *i) {
    *i = f80_to_int(ST(0));
}

// fisttp (SSE3): store ST(0) as an integer with truncation toward zero,
// regardless of the control-word rounding mode (the caller then pops). gcc
// -msse3 emits this for float/double -> int casts.
void fpu_istt16(struct cpu_state *cpu, int16_t *i) {
    enum f80_rounding_mode old_mode = f80_rounding_mode;
    f80_rounding_mode = round_chop;
    int64_t res = f80_to_int(ST(0));
    f80_rounding_mode = old_mode;
    if (res < INT16_MIN || res > INT16_MAX)
        res = INT16_MIN;
    *i = (int16_t) res;
}
void fpu_istt32(struct cpu_state *cpu, int32_t *i) {
    enum f80_rounding_mode old_mode = f80_rounding_mode;
    f80_rounding_mode = round_chop;
    int64_t res = f80_to_int(ST(0));
    f80_rounding_mode = old_mode;
    if (res < INT32_MIN || res > INT32_MAX)
        res = INT32_MIN;
    *i = (int32_t) res;
}
void fpu_istt64(struct cpu_state *cpu, int64_t *i) {
    enum f80_rounding_mode old_mode = f80_rounding_mode;
    f80_rounding_mode = round_chop;
    *i = f80_to_int(ST(0));
    f80_rounding_mode = old_mode;
}

void fpu_stm32(struct cpu_state *cpu, float32 *f) {
    *f = f80_to_double(ST(0));
}
void fpu_stm64(struct cpu_state *cpu, float64 *f) {
    *f = f80_to_double(ST(0));
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

// The status word's PE is a sticky exception flag: any operation whose result
// had to be rounded sets it, and only fclex/fldenv clear it. C1 is not sticky
// -- it reports the rounding direction of the most recent operation -- so it is
// assigned each time rather than accumulated. f80_inexact/f80_rounded_up are
// set by the rounding path in float80.c.
#define FPU_ROUND_BEGIN() do { f80_inexact = 0; f80_rounded_up = 0; } while (0)
#define FPU_ROUND_END() do {                    \
    if (f80_inexact)                            \
        cpu->pe = 1;                            \
    cpu->c1 = f80_rounded_up ? 1 : 0;           \
} while (0)


void fpu_prem(struct cpu_state *cpu) {
    ST(0) = f80_mod(ST(0), ST(1));
    cpu->c2 = 0; // say we finished the entire remainder
}

void fpu_scale(struct cpu_state *cpu) {
    enum f80_rounding_mode old_mode = f80_rounding_mode;
    f80_rounding_mode = round_chop;
    int scale = f80_to_int(ST(1));
    f80_rounding_mode = old_mode;
    ST(0) = f80_scale(ST(0), scale);
}

void fpu_rndint(struct cpu_state *cpu) {
    if (f80_isinf(ST(0)) || f80_isnan(ST(0)))
        return;
    FPU_ROUND_BEGIN();
    ST(0) = f80_round(ST(0));
    FPU_ROUND_END();
}

void fpu_sqrt(struct cpu_state *cpu) {
    FPU_ROUND_BEGIN();
    ST(0) = f80_sqrt(ST(0));
    FPU_ROUND_END();
}

void fpu_yl2x(struct cpu_state *cpu) {
    FPU_ROUND_BEGIN();
    ST(1) = f80_mul(ST(1), f80_log2(ST(0)));
    FPU_ROUND_END();
    fpu_pop(cpu);
}

void fpu_2xm1(struct cpu_state *cpu) {
    // an example of the ancient chinese art of chi ting
    ST(0) = f80_from_double(pow(2, f80_to_double(ST(0))) - 1);
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
    fpu_compare(cpu, ST(i));
}
void fpu_comi(struct cpu_state *cpu, int i) {
    fpu_comparei(cpu, ST(i));
}
void fpu_comm32(struct cpu_state *cpu, float *f) {
    fpu_compare(cpu, f80_from_double(*f));
}
void fpu_comm64(struct cpu_state *cpu, double *f) {
    fpu_compare(cpu, f80_from_double(*f));
}
void fpu_icom16(struct cpu_state *cpu, int16_t *i) {
    fpu_compare(cpu, f80_from_int(*i));
}
void fpu_icom32(struct cpu_state *cpu, int32_t *i) {
    fpu_compare(cpu, f80_from_int(*i));
}
void fpu_tst(struct cpu_state *cpu) {
    fpu_compare(cpu, fpu_consts[fconst_zero]);
}

void fpu_abs(struct cpu_state *cpu) {
    ST(0) = f80_abs(ST(0));
}

void fpu_chs(struct cpu_state *cpu) {
    ST(0) = f80_neg(ST(0));
}

void fpu_add(struct cpu_state *cpu, int srci, int dsti) {
    FPU_ROUND_BEGIN();
    ST(dsti) = f80_add(ST(dsti), ST(srci));
    FPU_ROUND_END();
}
void fpu_sub(struct cpu_state *cpu, int srci, int dsti) {
    FPU_ROUND_BEGIN();
    ST(dsti) = f80_sub(ST(dsti), ST(srci));
    FPU_ROUND_END();
}
void fpu_subr(struct cpu_state *cpu, int srci, int dsti) {
    FPU_ROUND_BEGIN();
    ST(dsti) = f80_sub(ST(srci), ST(dsti));
    FPU_ROUND_END();
}
void fpu_mul(struct cpu_state *cpu, int srci, int dsti) {
    FPU_ROUND_BEGIN();
    ST(dsti) = f80_mul(ST(dsti), ST(srci));
    FPU_ROUND_END();
}
void fpu_div(struct cpu_state *cpu, int srci, int dsti) {
    FPU_ROUND_BEGIN();
    ST(dsti) = f80_div(ST(dsti), ST(srci));
    FPU_ROUND_END();
}
void fpu_divr(struct cpu_state *cpu, int srci, int dsti) {
    FPU_ROUND_BEGIN();
    ST(dsti) = f80_div(ST(srci), ST(dsti));
    FPU_ROUND_END();
}

void fpu_iadd16(struct cpu_state *cpu, int16_t *i) {
    ST(0) = f80_add(ST(0), f80_from_int(*i));
}
void fpu_isub16(struct cpu_state *cpu, int16_t *i) {
    ST(0) = f80_sub(ST(0), f80_from_int(*i));
}
void fpu_isubr16(struct cpu_state *cpu, int16_t *i) {
    ST(0) = f80_sub(f80_from_int(*i), ST(0));
}
void fpu_imul16(struct cpu_state *cpu, int16_t *i) {
    ST(0) = f80_mul(ST(0), f80_from_int(*i));
}
void fpu_idiv16(struct cpu_state *cpu, int16_t *i) {
    ST(0) = f80_div(ST(0), f80_from_int(*i));
}
void fpu_idivr16(struct cpu_state *cpu, int16_t *i) {
    ST(0) = f80_div(f80_from_int(*i), ST(0));
}

void fpu_iadd32(struct cpu_state *cpu, int32_t *i) {
    ST(0) = f80_add(ST(0), f80_from_int(*i));
}
void fpu_isub32(struct cpu_state *cpu, int32_t *i) {
    ST(0) = f80_sub(ST(0), f80_from_int(*i));
}
void fpu_isubr32(struct cpu_state *cpu, int32_t *i) {
    ST(0) = f80_sub(f80_from_int(*i), ST(0));
}
void fpu_imul32(struct cpu_state *cpu, int32_t *i) {
    ST(0) = f80_mul(ST(0), f80_from_int(*i));
}
void fpu_idiv32(struct cpu_state *cpu, int32_t *i) {
    ST(0) = f80_div(ST(0), f80_from_int(*i));
}
void fpu_idivr32(struct cpu_state *cpu, int32_t *i) {
    ST(0) = f80_div(f80_from_int(*i), ST(0));
}

void fpu_addm32(struct cpu_state *cpu, float32 *f) {
    ST(0) = f80_add(ST(0), f80_from_double(*f));
}
void fpu_subm32(struct cpu_state *cpu, float32 *f) {
    ST(0) = f80_sub(ST(0), f80_from_double(*f));
}
void fpu_subrm32(struct cpu_state *cpu, float32 *f) {
    ST(0) = f80_sub(f80_from_double(*f), ST(0));
}
void fpu_mulm32(struct cpu_state *cpu, float32 *f) {
    ST(0) = f80_mul(ST(0), f80_from_double(*f));
}
void fpu_divm32(struct cpu_state *cpu, float32 *f) {
    ST(0) = f80_div(ST(0), f80_from_double(*f));
}
void fpu_divrm32(struct cpu_state *cpu, float32 *f) {
    ST(0) = f80_div(f80_from_double(*f), ST(0));
}

void fpu_addm64(struct cpu_state *cpu, float64 *f) {
    ST(0) = f80_add(ST(0), f80_from_double(*f));
}
void fpu_subm64(struct cpu_state *cpu, float64 *f) {
    ST(0) = f80_sub(ST(0), f80_from_double(*f));
}
void fpu_subrm64(struct cpu_state *cpu, float64 *f) {
    ST(0) = f80_sub(f80_from_double(*f), ST(0));
}
void fpu_mulm64(struct cpu_state *cpu, float64 *f) {
    ST(0) = f80_mul(ST(0), f80_from_double(*f));
}
void fpu_divm64(struct cpu_state *cpu, float64 *f) {
    ST(0) = f80_div(ST(0), f80_from_double(*f));
}
void fpu_divrm64(struct cpu_state *cpu, float64 *f) {
    ST(0) = f80_div(f80_from_double(*f), ST(0));
}

void fpu_patan(struct cpu_state *cpu) {
    // there's no native atan2 for 80-bit float yet.
    ST(1) = f80_from_double(atan2(f80_to_double(ST(1)), f80_to_double(ST(0))));
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
    FPU_ROUND_BEGIN();
    ST(0) = f80_from_double(sin(arg));
    FPU_ROUND_END();
}
void fpu_cos(struct cpu_state *cpu) {
    double arg = f80_to_double(ST(0));
    if (fpu_trig_out_of_range(cpu, arg))
        return;
    FPU_ROUND_BEGIN();
    ST(0) = f80_from_double(cos(arg));
    FPU_ROUND_END();
}
void fpu_sincos(struct cpu_state *cpu) {
    // ST(0) is replaced by sin, then cos is pushed, so on exit ST(0) is cos
    // and ST(1) is sin. Like fsin/fcos this goes through double rather than
    // computing at 80-bit precision.
    double arg = f80_to_double(ST(0));
    if (fpu_trig_out_of_range(cpu, arg))
        return;
    FPU_ROUND_BEGIN();
    ST(0) = f80_from_double(sin(arg));
    fpush(f80_from_double(cos(arg)));
    FPU_ROUND_END();
}

void fpu_xtract(struct cpu_state *cpu) {
    int exp;
    float80 signif;
    f80_xtract(ST(0), &exp, &signif);
    ST(0) = f80_from_int(exp);
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

void fpu_ldcw16(struct cpu_state *cpu, uint16_t *i) {
    cpu->fcw = *i;
    f80_rounding_mode = cpu->rc;
    f80_precision = f80_precision_from_pc(cpu->pc);
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
    f80_rounding_mode = cpu->rc;
    f80_precision = f80_precision_from_pc(cpu->pc);
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
void fpu_fxsave32(struct cpu_state *cpu, struct fxsave_area *area) {
    fxsave_fill(cpu, area, 8);
}

void fpu_fxrestore32(struct cpu_state *cpu, struct fxsave_area *area) {
    fxsave_restore(cpu, area, 8);
}

// SSE runs round-to-nearest with every exception masked, so the control bits
// are stored rather than honored -- the same deal cpu_state's mxcsr comment
// describes. Storing them still matters: a read-modify-write of the control
// word has to round-trip, and software reads MXCSR back to decide whether it
// is running on a CPU that accepted what it wrote.
void fpu_stmxcsr32(struct cpu_state *cpu, dword_t *value) {
    *value = cpu->mxcsr;
}

void fpu_ldmxcsr32(struct cpu_state *cpu, dword_t *value) {
    cpu->mxcsr = *value & 0xffff;
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
    f80_rounding_mode = cpu->rc;
    f80_precision = f80_precision_from_pc(cpu->pc);
}

void fpu_clex(struct cpu_state *cpu) {
    cpu->pe = cpu->ue = cpu->oe = cpu->ze = cpu->de = cpu->ie = cpu->es = cpu->sf = cpu->b = 0;
}
