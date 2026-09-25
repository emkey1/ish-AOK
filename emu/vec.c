#include <math.h>
#include <string.h>

#include "emu/vec.h"
#include "emu/fpenv.h"
#include "emu/cpu.h"

union vec {
    uint8_t u8[16];
    uint16_t u16[8];
    uint32_t u32[4];
    uint64_t u64[2];
    __uint128_t u128[1];
    __uint128_t dqw;
};

static inline void zero_xmm(union xmm_reg *xmm) {
    xmm->qw[0] = 0;
    xmm->qw[1] = 0;
}

static inline int32_t satsw(int32_t dw) {
    if (dw > 0xff80)
        dw &= 0xff;
    else if (dw > 0x7fff)
        dw = 0x80;
    else if (dw > 0x7f)
        dw = 0x7f;
    return dw;
}
static inline uint32_t satud(uint32_t dw) {
    if (dw > 0xffff8000)
        dw &= 0xffff;
    else if (dw > 0x7fffffff)
        dw = 0x8000;
    else if (dw > 0x7fff)
        dw = 0x7fff;
    return dw;
}
static inline uint32_t satub(uint32_t dw) {
    if (dw >= 0x8000)
        dw = 0;
    else if (dw > 0xff)
        dw = 0xff;
    return dw;
}
static inline uint32_t satsb(uint32_t dw) {
    if (dw > 0xffffff80)
        dw &= 0xff;
    else if (dw > 0x7fffffff)
        dw = 0x80;
    else if (dw > 0x7f)
        dw = 0x7f;
    return dw;
}

#define VEC_ZERO_COPY(zero, copy) \
    void vec_zero##zero##_copy##copy(NO_CPU, const void *src, void *dst) { \
        memcpy(dst, src, copy/8); \
        memset((char *) dst + copy/8, 0, (zero-copy)/8); \
    }
VEC_ZERO_COPY(128, 128)
VEC_ZERO_COPY(128, 64)
VEC_ZERO_COPY(128, 32)
VEC_ZERO_COPY(64, 64)
VEC_ZERO_COPY(64, 32)
VEC_ZERO_COPY(32, 32)

void vec_merge32(NO_CPU, const void *src, void *dst) {
    memcpy(dst, src, 4);
}
void vec_merge64(NO_CPU, const void *src, void *dst) {
    memcpy(dst, src, 8);
}
void vec_merge128(NO_CPU, const void *src, void *dst) {
    memcpy(dst, src, 16);
}

#define _SHIFT(op, size) \
    do { \
        if (unlikely(amount > (size)-1)) { \
            zero_xmm(dst); \
        } else { \
            union vec d = { .dqw = dst->u128 }; \
            for (unsigned i = 0; i < array_size(d.u##size); i++) \
                d.u##size[i] op##= amount; \
            dst->u128 = d.dqw; \
        } \
    } while (0)

#define VEC_SSE_SHIFT(dir, suffix, op, size) \
    void vec_shift##dir##_##suffix##128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        const uint8_t amount = src->u8[0]; \
        _SHIFT(op, size); \
    } \
    void vec_imm_shift##dir##_##suffix##128(NO_CPU, const uint8_t amount, union xmm_reg *dst) { \
        _SHIFT(op, size); \
    }

#define _VEC_SSE_CMP(sgn, usgn, suffix, relop, size) \
    void vec_compare##sgn##_##suffix##128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        union vec s = { .dqw = src->u128 }, d = { .dqw = dst->u128 }; \
        for (unsigned i = 0; i < array_size(s.u##size); i++) \
            d.u##size[i] = (usgn##int##size##_t)d.u##size[i] relop (usgn##int##size##_t)s.u##size[i] ? ~0 : 0;\
        dst->u128 = d.dqw; \
    }

#define VEC_SSE_CMPD(suffix, relop, size) \
    _VEC_SSE_CMP(, u, suffix, relop, size)
#define VEC_SSE_CMPS(suffix, relop, size) \
    _VEC_SSE_CMP(s,, suffix, relop, size)

#define VEC_SSE_OP(name, suffix, op, size) \
    void vec_##name##_##suffix##128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        union vec s = { .dqw = src->u128 }, d = { .dqw = dst->u128 }; \
        for (unsigned i = 0; i < array_size(s.u##size); i++) \
            d.u##size[i] op##= s.u##size[i]; \
        dst->u128 = d.dqw; \
    }

VEC_SSE_SHIFT(r, w, >>, 16)
VEC_SSE_SHIFT(r, d, >>, 32)
VEC_SSE_SHIFT(r, q, >>, 64)

VEC_SSE_SHIFT(l, w, <<, 16)
VEC_SSE_SHIFT(l, d, <<, 32)
VEC_SSE_SHIFT(l, q, <<, 64)

VEC_SSE_CMPD(eqb, ==,  8)
VEC_SSE_CMPD(eqw, ==, 16)
VEC_SSE_CMPD(eqd, ==, 32)

VEC_SSE_CMPS(gtb, >,  8)
VEC_SSE_CMPS(gtw, >, 16)
VEC_SSE_CMPS(gtd, >, 32)

VEC_SSE_OP(add, b, +, 8)
VEC_SSE_OP(add, w, +, 16)
VEC_SSE_OP(add, d, +, 32)
VEC_SSE_OP(add, q, +, 64)

VEC_SSE_OP(sub, b, -, 8)
VEC_SSE_OP(sub, w, -, 16)
VEC_SSE_OP(sub, d, -, 32)
VEC_SSE_OP(sub, q, -, 64)

VEC_SSE_OP(and, dq, &, 128)
VEC_SSE_OP(or,  dq, |, 128)
VEC_SSE_OP(xor, dq, ^, 128)

void vec_imm_shiftl_dq128(NO_CPU, uint8_t amount, union xmm_reg *dst) {
    if (amount >= 16)
        zero_xmm(dst);
    else
        dst->u128 <<= amount * 8;
}
void vec_imm_shiftr_dq128(NO_CPU, uint8_t amount, union xmm_reg *dst) {
    if (amount >= 16)
        zero_xmm(dst);
    else
        dst->u128 >>= amount * 8;
}
void vec_shiftrs_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    const uint8_t amount = src->u8[0];
    for (unsigned i = 0; i < 8; i++) {
        if (unlikely(amount > 15))
            dst->u16[i] = ((dst->u16[i] >> 15) & (uint16_t)1) ? 0xffff : 0;
        else
            dst->u16[i] = ((int16_t)(dst->u16[i])) >> amount;
    }
}
void vec_shiftrs_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    const uint8_t amount = src->u8[0];
    for (unsigned i = 0; i < 4; i++) {
        if (unlikely(amount > 31))
            dst->u32[i] = ((dst->u32[i] >> 31) & (uint32_t)1) ? 0xffffffff : 0;
        else
            dst->u32[i] = ((int32_t)(dst->u32[i])) >> amount;
    }
}
void vec_imm_shiftrs_w128(NO_CPU, const uint8_t amount, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++) {
        if (unlikely(amount > 15))
            dst->u16[i] = ((dst->u16[i] >> 15) & (uint16_t)1) ? 0xffff : 0;
        else
            dst->u16[i] = ((int16_t)(dst->u16[i])) >> amount;
    }
}
void vec_imm_shiftrs_d128(NO_CPU, const uint8_t amount, union xmm_reg *dst) {
    for (unsigned i = 0; i < 4; i++) {
        if (unlikely(amount > 31))
            dst->u32[i] = ((dst->u32[i] >> 31) & (uint32_t)1) ? 0xffffffff : 0;
        else
            dst->u32[i] = ((int32_t)(dst->u32[i])) >> amount;
    }
}

void vec_addus_b128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 16; i++) {
        const int32_t sb = dst->u8[i] + src->u8[i];
        dst->u8[i] = sb > 0xff ? 0xff : sb;
    }
}
void vec_addus_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++) {
        const int32_t sw = dst->u16[i] + src->u16[i];
        dst->u16[i] = sw > 0xffff ? 0xffff : sw;
    }
}
void vec_addss_b128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 16; i++)
        dst->u8[i] = satsb((int8_t)dst->u8[i] + (int8_t)src->u8[i]);
}
void vec_addss_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++)
        dst->u16[i] = satud((int16_t)dst->u16[i] + (int16_t)src->u16[i]);
}

void vec_subus_b128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 16; i++) {
        const int32_t sb = dst->u8[i] - src->u8[i];
        dst->u8[i] = sb < 0 ? 0 : sb;
    }
}
void vec_subus_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++) {
        const int32_t sw = dst->u16[i] - src->u16[i];
        dst->u16[i] = sw < 0 ? 0 : sw;
    }
}
void vec_subss_b128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 16; i++)
        dst->u8[i] = satsb((int8_t)dst->u8[i] - (int8_t)src->u8[i]);
}
void vec_subss_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++)
        dst->u16[i] = satud((int16_t)dst->u16[i] - (int16_t)src->u16[i]);
}

void vec_madd_d128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[0] = (int32_t)((int16_t)dst->u16[0] * (int16_t)src->u16[0]) +
                  (int32_t)((int16_t)dst->u16[1] * (int16_t)src->u16[1]);
    dst->u32[1] = (int32_t)((int16_t)dst->u16[2] * (int16_t)src->u16[2]) +
                  (int32_t)((int16_t)dst->u16[3] * (int16_t)src->u16[3]);
    dst->u32[2] = (int32_t)((int16_t)dst->u16[4] * (int16_t)src->u16[4]) +
                  (int32_t)((int16_t)dst->u16[5] * (int16_t)src->u16[5]);
    dst->u32[3] = (int32_t)((int16_t)dst->u16[6] * (int16_t)src->u16[6]) +
                  (int32_t)((int16_t)dst->u16[7] * (int16_t)src->u16[7]);
}

void vec_sumabs_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    uint32_t sum[2] = { 0, 0 };
    for (unsigned i = 0; i < 8; i++) {
        int32_t difflo = dst->u8[i + 0] - src->u8[i + 0];
        int32_t diffhi = dst->u8[i + 8] - src->u8[i + 8];
        sum[0] += (difflo < 0) ? -(uint32_t)difflo : difflo;
        sum[1] += (diffhi < 0) ? -(uint32_t)diffhi : diffhi;
    }
    dst->u32[0] = sum[0];
    dst->u32[2] = sum[1];
    dst->u32[1] = dst->u32[3] = 0;
}

void vec_mulu_dq128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    dst->qw[0] = (uint64_t) src->u32[0] * dst->u32[0];
    dst->qw[1] = (uint64_t) src->u32[2] * dst->u32[2];
}

void vec_andn128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    dst->qw[0] = ~dst->qw[0] & src->qw[0];
    dst->qw[1] = ~dst->qw[1] & src->qw[1];
}

void vec_min_ub128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < array_size(src->u8); i++)
        if (src->u8[i] < dst->u8[i])
            dst->u8[i] = src->u8[i];
}
void vec_max_ub128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < array_size(src->u8); i++)
        if (src->u8[i] > dst->u8[i])
            dst->u8[i] = src->u8[i];
}
void vec_mins_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++)
        dst->u16[i] = (int16_t)dst->u16[i] < (int16_t)src->u16[i] ? dst->u16[i] : src->u16[i];
}

void vec_maxs_w128(NO_CPU, union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++)
        dst->u16[i] = (int16_t)dst->u16[i] > (int16_t)src->u16[i] ? dst->u16[i] : src->u16[i];
}

static bool cmpd(double a, double b, int type) {
    bool res;
    switch (type % 4) {
        case 0: res = a == b; break;
        case 1: res = a < b; break;
        case 2: res = a <= b; break;
        case 3: res = isnan(a) || isnan(b); break;
    }
    if (type >= 4) res = !res;
    return res;
}
static bool cmps(float a, float b, int type) {
    bool res;
    switch (type % 4) {
        case 0: res = a == b; break;
        case 1: res = a < b; break;
        case 2: res = a <= b; break;
        case 3: res = isnan(a) || isnan(b); break;
    }
    if (type >= 4) res = !res;
    return res;
}

void vec_single_fcmp64(NO_CPU, const double *src, union xmm_reg *dst, uint8_t type) {
    dst->qw[0] = cmpd(dst->f64[0], *src, type) ? -1 : 0;
}
void vec_single_fcmp32(NO_CPU, const float *src, union xmm_reg *dst, uint8_t type) {
    dst->u32[0] = cmps(dst->f32[0], *src, type) ? -1 : 0;
}

void vec_single_fadd64(NO_CPU, const double *src, double *dst) { *dst += *src; }
void vec_single_fadd32(NO_CPU, const float *src, float *dst) { *dst += *src; }
void vec_single_fmul64(NO_CPU, const double *src, double *dst) { *dst *= *src; }
void vec_single_fmul32(NO_CPU, const float *src, float *dst) { *dst *= *src; }
void vec_single_fsub64(NO_CPU, const double *src, double *dst) { *dst -= *src; }
void vec_single_fsub32(NO_CPU, const float *src, float *dst) { *dst -= *src; }
void vec_single_fdiv64(NO_CPU, const double *src, double *dst) { *dst /= *src; }
void vec_single_fdiv32(NO_CPU, const float *src, float *dst) { *dst /= *src; }

void vec_single_fsqrt64(NO_CPU, const double *src, double *dst) { *dst = sqrt(*src); }
void vec_single_fsqrt32(NO_CPU, const float *src, float *dst) { *dst = sqrtf(*src); }

// x86 [max|min]s[s|d]: DEST is kept ONLY when (DEST > SRC) / (DEST < SRC);
// in every other case -- SRC greater/less, the two equal (incl. +0 vs -0), or
// either NaN -- the SRC operand is returned. So the predicate is !(dst cmp src),
// NOT (src cmp dst): the latter keeps dst on the equal/unordered tie, which
// differs from real x86 (and from the amd64 path's `dst>src ? dst : src`) on
// the +0/-0 case. Using !(dst cmp src) also folds in the NaN rule for free
// (a NaN compare is false, so !false -> take src).
void vec_single_fmax64(NO_CPU, const double *src, double *dst) {
    if (!(*dst > *src)) *dst = *src;
}
void vec_single_fmin64(NO_CPU, const double *src, double *dst) {
    if (!(*dst < *src)) *dst = *src;
}
void vec_single_fmax32(NO_CPU, const float *src, float *dst) {
    if (!(*dst > *src)) *dst = *src;
}
void vec_single_fmin32(NO_CPU, const float *src, float *dst) {
    if (!(*dst < *src)) *dst = *src;
}

static void vec_set_ucomi_flags(struct cpu_state *cpu, bool unordered, bool equal, bool less) {
    cpu->zf_res = 0;
    cpu->pf_res = 0;
    cpu->sf_res = 0;
    cpu->af_ops = 0;

    cpu->zf = unordered || equal;
    cpu->pf = unordered;
    cpu->cf = unordered || less;
    cpu->sf = 0;
    cpu->af = 0;
    cpu->of = 0;
    cpu->cf_bit = cpu->cf;
    cpu->of_bit = 0;
}

void vec_single_ucomi32(struct cpu_state *cpu, const float *src, const float *dst) {
    bool unordered = isnan(*src) || isnan(*dst);
    vec_set_ucomi_flags(cpu, unordered, *src == *dst, *dst < *src);
}

void vec_single_ucomi64(struct cpu_state *cpu, const double *src, const double *dst) {
    bool unordered = isnan(*src) || isnan(*dst);
    vec_set_ucomi_flags(cpu, unordered, *src == *dst, *dst < *src);
}

// COMISS/COMISD: the same flags, but a signalling compare -- a quiet NaN is an
// invalid operation too. The quiet host compares above already raise it for a
// signalling one; emu/fpenv.c carries it into MXCSR.
void vec_single_comi32(struct cpu_state *cpu, const float *src, const float *dst) {
    if (isnan(*src) || isnan(*dst))
        fpenv_raise_invalid();
    vec_single_ucomi32(cpu, src, dst);
}

void vec_single_comi64(struct cpu_state *cpu, const double *src, const double *dst) {
    if (isnan(*src) || isnan(*dst))
        fpenv_raise_invalid();
    vec_single_ucomi64(cpu, src, dst);
}

#define VEC_PACKED_OP(name, op, field, size, n) \
    void vec_##name##size(NO_CPU, union xmm_reg *src, union xmm_reg *dst) { \
        for (int i = 0; i < n; ++i) { \
            dst->field[i] op##= src->field[i]; \
        } \
    }

VEC_PACKED_OP(add_p, +, f64, 64, 2)
VEC_PACKED_OP(add_p, +, f32, 32, 4)
VEC_PACKED_OP(sub_p, -, f64, 64, 2)
VEC_PACKED_OP(sub_p, -, f32, 32, 4)
VEC_PACKED_OP(mul_p, *, f64, 64, 2)
VEC_PACKED_OP(mul_p, *, f32, 32, 4)
VEC_PACKED_OP(div_p, /, f64, 64, 2)
VEC_PACKED_OP(div_p, /, f32, 32, 4)

// Packed min/max mirror the scalar single_fmin/single_fmax rule exactly: keep
// dst only when (dst cmp src) holds, else take src -- so the equal/unordered
// (incl. +0 vs -0) and NaN cases both return src, matching real x86.
#define VEC_PACKED_MINMAX(name, cmp, field, size, n) \
    void vec_##name##size(NO_CPU, union xmm_reg *src, union xmm_reg *dst) { \
        for (int i = 0; i < n; ++i) { \
            if (!(dst->field[i] cmp src->field[i])) \
                dst->field[i] = src->field[i]; \
        } \
    }
VEC_PACKED_MINMAX(min_p, <, f64, 64, 2)
VEC_PACKED_MINMAX(min_p, <, f32, 32, 4)
VEC_PACKED_MINMAX(max_p, >, f64, 64, 2)
VEC_PACKED_MINMAX(max_p, >, f32, 32, 4)

// sqrtpd/sqrtps: dst[i] = sqrt(src[i]) (unary; not an accumulate). Read all
// source lanes before writing in case src and dst are the same register.
void vec_sqrt_p64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    double s0 = src->f64[0], s1 = src->f64[1];
    dst->f64[0] = sqrt(s0);
    dst->f64[1] = sqrt(s1);
}
void vec_sqrt_p32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    float s0 = src->f32[0], s1 = src->f32[1], s2 = src->f32[2], s3 = src->f32[3];
    dst->f32[0] = sqrtf(s0);
    dst->f32[1] = sqrtf(s1);
    dst->f32[2] = sqrtf(s2);
    dst->f32[3] = sqrtf(s3);
}

// cvtps2pd: two packed floats (low 64 bits of src) -> two doubles.
void vec_cvtps2pd64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    float s0 = src->f32[0], s1 = src->f32[1];
    dst->f64[0] = s0;
    dst->f64[1] = s1;
}
// cvtpd2ps: two doubles -> two floats in the low 64 bits; high 64 bits zeroed.
void vec_cvtpd2ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    double s0 = src->f64[0], s1 = src->f64[1];
    dst->f32[0] = (float) s0;
    dst->f32[1] = (float) s1;
    dst->f32[2] = 0;
    dst->f32[3] = 0;
}
// cvtdq2ps: four packed signed int32 -> four floats (same lane count/width).
void vec_cvtdq2ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    int32_t s0 = (int32_t) src->u32[0], s1 = (int32_t) src->u32[1];
    int32_t s2 = (int32_t) src->u32[2], s3 = (int32_t) src->u32[3];
    dst->f32[0] = s0;
    dst->f32[1] = s1;
    dst->f32[2] = s2;
    dst->f32[3] = s3;
}

void vec_fcmp_p64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t type) {
    for (size_t i = 0; i < sizeof(dst->f64) / sizeof(*dst->f64); ++i) {
        dst->qw[i] = cmpd(dst->f64[i], src->f64[i], type) ? -1 : 0;
    }
}

// come to the dark side of macros
#define _ISNAN_int32_t(x) false
#define VEC_CAST(src, dst, src_t, dst_t, n) \
    do { \
        for (int i = 0; i < n; ++i) \
            ((dst_t *)dst)[i] = ((src_t *)src)[i]; \
    } while (0)

// x86 cvtt* (truncating float/double -> signed int32) return the integer
// indefinite 0x80000000 for NaN AND for any value outside the int32 range. A
// bare C cast is wrong on the arm64 host: float/double -> int32 SATURATES there
// (e.g. (int32_t)1e30 -> 0x7fffffff), whereas x86 yields 0x80000000 for overflow
// in BOTH directions. Range-check explicitly, matching amd64_cvtt_scalar_to_int
// (emu/amd64_interp.c). Every user of this macro has an int32_t destination.
// The indefinite is an invalid operation, which no host instruction ran to
// raise, so it is raised here; an inexact truncation raises its own flag.
#define VEC_TRUNC_INT(src, dst, src_t, dst_t, n) \
    do { \
        for (int i = 0; i < n; ++i) { \
            src_t _v = ((src_t *)src)[i]; \
            if (isnan(_v) || _v >= 2147483648.0 || _v < -2147483648.0) { \
                ((dst_t *)dst)[i] = INT32_MIN; \
                fpenv_raise_invalid(); \
            } else { \
                ((dst_t *)dst)[i] = (dst_t) _v; \
            } \
        } \
    } while (0)

#define VEC_CVT(name, src_t, dst_t) \
    void vec_cvt##name(NO_CPU, const src_t *src, dst_t *dst) { \
        VEC_CAST(src, dst, src_t, dst_t, 1); \
    }

#define VEC_CVTT(name, src_t, dst_t) \
    void vec_cvt##name(NO_CPU, const src_t *src, dst_t *dst) { \
        VEC_TRUNC_INT(src, dst, src_t, dst_t, 1); \
    }

#define PACKED_VEC_CVTT(name, src_field, dst_field, src_t, dst_t, n) \
    void vec_cvt##name(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        VEC_TRUNC_INT(src->src_field, dst->dst_field, src_t, dst_t, n); \
        /* Note: this needs to be second, because src and dst may alias */ \
        memset(dst->dst_field + n, 0, sizeof(*dst) - n * sizeof(*dst->dst_field)); \
    }

// cvtsd2si/cvtss2si: rounded by MXCSR.RC, which is the host's mode while
// guest code runs (emu/fpenv.c) -- rint rounds in it and raises inexact --
// then the same indefinite rule as the truncating forms. They were missing
// from the i386 decoder altogether, so both raised SIGILL.
#define VEC_CVTR(name, src_t, dst_t) \
    void vec_cvt##name(NO_CPU, const src_t *src, dst_t *dst) { \
        src_t r = (src_t) rint(*src); \
        VEC_TRUNC_INT(&r, dst, src_t, dst_t, 1); \
    }

VEC_CVT(si2sd32, int32_t, double)
VEC_CVTT(tsd2si64, double, int32_t)
VEC_CVTR(sd2si64, double, int32_t)
VEC_CVTR(ss2si32, float, int32_t)
VEC_CVT(sd2ss64, double, float)
VEC_CVT(si2ss32, int32_t, float)
VEC_CVTT(tss2si32, float, int32_t)
VEC_CVT(ss2sd32, float, double)

PACKED_VEC_CVTT(tpd2dq64, f64, u32, double, int32_t, 2)
PACKED_VEC_CVTT(tps2dq32, f32, u32, float, int32_t, 4)

// cvtdq2pd: two packed signed int32 from the low 64 bits of src -> two
// doubles in dst. Read both source dwords before writing any result: src and
// dst may be the same register, and writing dst->f64[0] clobbers src->u32[1].
void vec_cvtdq2pd64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    int32_t s0 = (int32_t) src->u32[0];
    int32_t s1 = (int32_t) src->u32[1];
    dst->f64[0] = s0;
    dst->f64[1] = s1;
}

void vec_unpackl_bw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 7; i >= 0; i--) {
        dst->u8[i*2 + 1] = src->u8[i];
        dst->u8[i*2] = dst->u8[i];
    }
}
void vec_unpackl_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 3; i >= 0; i--) {
        dst->u16[i*2 + 1] = src->u16[i];
        dst->u16[i*2] = dst->u16[i];
    }
}
void vec_unpackl_dq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[3] = src->u32[1];
    dst->u32[2] = dst->u32[1];
    dst->u32[1] = src->u32[0];
}
void vec_unpackl_qdq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->qw[1] = src->qw[0];
}
void vec_unpackl_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[2] = dst->u32[1];
    dst->u32[1] = src->u32[0];
    dst->u32[3] = src->u32[1];
}
void vec_unpackl_pd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->f64[1] = src->f64[0];
}
void vec_unpackh_bw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 8; i++) {
        dst->u8[2 * i + 0] = dst->u8[i + 8];
        dst->u8[2 * i + 1] = src->u8[i + 8];
    }
}
void vec_unpackh_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 4; i++) {
        dst->u16[2 * i + 0] = dst->u16[i + 4];
        dst->u16[2 * i + 1] = src->u16[i + 4];
    }
}
void vec_unpackh_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[0] = dst->u32[2];
    dst->u32[1] = src->u32[2];
    dst->u32[2] = dst->u32[3];
    dst->u32[3] = src->u32[3];
}
void vec_unpackh_dq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->qw[0] = dst->qw[1];
    dst->qw[1] = src->qw[1];
}
void vec_unpackh_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[0] = dst->u32[2];
    dst->u32[1] = src->u32[2];
    dst->u32[2] = dst->u32[3];
    dst->u32[3] = src->u32[3];
}
void vec_unpackh_pd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->f64[0] = dst->f64[1];
    dst->f64[1] = src->f64[1];
}

void vec_packss_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[0] = (satsw(dst->u16[0]) << 0x00) | (satsw(dst->u16[1]) << 0x08) |
                  (satsw(dst->u16[2]) << 0x10) | (satsw(dst->u16[3]) << 0x18);
    dst->u32[1] = (satsw(dst->u16[4]) << 0x00) | (satsw(dst->u16[5]) << 0x08) |
                  (satsw(dst->u16[6]) << 0x10) | (satsw(dst->u16[7]) << 0x18);
    dst->u32[2] = (satsw(src->u16[0]) << 0x00) | (satsw(src->u16[1]) << 0x08) |
                  (satsw(src->u16[2]) << 0x10) | (satsw(src->u16[3]) << 0x18);
    dst->u32[3] = (satsw(src->u16[4]) << 0x00) | (satsw(src->u16[5]) << 0x08) |
                  (satsw(src->u16[6]) << 0x10) | (satsw(src->u16[7]) << 0x18);
}
void vec_packss_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[0] = satud(dst->u32[0]) | (satud(dst->u32[1]) << 16);
    dst->u32[1] = satud(dst->u32[2]) | (satud(dst->u32[3]) << 16);
    dst->u32[2] = satud(src->u32[0]) | (satud(src->u32[1]) << 16);
    dst->u32[3] = satud(src->u32[2]) | (satud(src->u32[3]) << 16);
}
void vec_packsu_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->u32[0] = (satub(dst->u16[0]) << 0x00) | (satub(dst->u16[1]) << 0x08) |
                  (satub(dst->u16[2]) << 0x10) | (satub(dst->u16[3]) << 0x18);
    dst->u32[1] = (satub(dst->u16[4]) << 0x00) | (satub(dst->u16[5]) << 0x08) |
                  (satub(dst->u16[6]) << 0x10) | (satub(dst->u16[7]) << 0x18);
    dst->u32[2] = (satub(src->u16[0]) << 0x00) | (satub(src->u16[1]) << 0x08) |
                  (satub(src->u16[2]) << 0x10) | (satub(src->u16[3]) << 0x18);
    dst->u32[3] = (satub(src->u16[4]) << 0x00) | (satub(src->u16[5]) << 0x08) |
                  (satub(src->u16[6]) << 0x10) | (satub(src->u16[7]) << 0x18);
}

void vec_shuffle_lw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding) {
    union xmm_reg src_copy = *src;
    for (int i = 0; i < 4; i++)
        dst->u16[i] = src_copy.u16[(encoding >> (i*2)) % 4];
    dst->qw[1] = src->qw[1];
}
void vec_shuffle_hw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding) {
    union xmm_reg src_copy = *src;
    dst->qw[0] = src->qw[0];
    dst->u32[2] = src_copy.u16[(encoding >> 0 & 3) | 4] | src_copy.u16[(encoding >> 2 & 3) | 4] << 16;
    dst->u32[3] = src_copy.u16[(encoding >> 4 & 3) | 4] | src_copy.u16[(encoding >> 6 & 3) | 4] << 16;
}

void vec_shuffle_d128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding) {
    union xmm_reg src_copy = *src;
    for (int i = 0; i < 4; i++)
        dst->u32[i] = src_copy.u32[(encoding >> (i*2)) % 4];
}
void vec_shuffle_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding) {
    // The low two lanes come from dst, the high two from src. Snapshot both
    // first: writing dst->u32[0] before reading the lane-1 selector (which may
    // point back at lane 0) otherwise corrupts the result, and src may alias dst.
    union xmm_reg d = *dst, s = *src;
    dst->u32[0] = d.u32[(encoding >> 0) & 3];
    dst->u32[1] = d.u32[(encoding >> 2) & 3];
    dst->u32[2] = s.u32[(encoding >> 4) & 3];
    dst->u32[3] = s.u32[(encoding >> 6) & 3];
}
void vec_shuffle_pd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t encoding) {
    union xmm_reg d = *dst, s = *src;
    dst->qw[0] = d.qw[(encoding >> 0) & 1];
    dst->qw[1] = s.qw[(encoding >> 1) & 1];
}

void vec_movmask_b128(NO_CPU, const union xmm_reg *src, uint32_t *dst) {
    *dst = 0;
    for (unsigned i = 0; i < array_size(src->u8); i++) {
        if (src->u8[i] & (1 << 7))
            *dst |= 1 << i;
    }
}
void vec_fmovmask_s128(NO_CPU, const union xmm_reg *src, uint32_t *dst) {
    *dst = 0;
    for (unsigned i = 0; i < array_size(src->f32); i++) {
        if (signbit(src->f32[i]))
            *dst |= 1 << i;
    }
}
void vec_fmovmask_d128(NO_CPU, const union xmm_reg *src, uint32_t *dst) {
    *dst = 0;
    for (unsigned i = 0; i < array_size(src->f64); i++) {
        if (signbit(src->f64[i]))
            *dst |= 1 << i;
    }
}

void vec_movl_p64(NO_CPU, const uint64_t *src, union xmm_reg *dst) {
    dst->qw[0] = *src;
}
void vec_movl_pm64(NO_CPU, const union xmm_reg *src, uint64_t *dst) {
    *dst = src->qw[0];
}
void vec_movh_p64(NO_CPU, const uint64_t *src, union xmm_reg *dst) {
    dst->qw[1] = *src;
}
void vec_movh_pm64(NO_CPU, const union xmm_reg *src, uint64_t *dst) {
    *dst = src->qw[1];
}

void vec_insert_w128(NO_CPU, const uint32_t *src, union xmm_reg *dst, uint8_t index) {
    dst->u16[index % 8] = (uint16_t)*src;
}
void vec_extract_w128(NO_CPU, const union xmm_reg *src, uint32_t *dst, uint8_t index) {
    *dst = src->u16[index % 8];
}

void vec_avg_b128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 16; i++)
        dst->u8[i] = (1 + dst->u8[i] + src->u8[i]) >> 1;
}
void vec_avg_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (unsigned i = 0; i < 8; i++)
        dst->u16[i] = (1 + dst->u16[i] + src->u16[i]) >> 1;
}

void vec_mull128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 8; i++) {
        dst->u16[i] = (uint16_t)(dst->u16[i] * src->u16[i]);
    }
}

void vec_mulu128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 8; i++) {
        uint32_t res = ((int16_t)dst->u16[i] * (int16_t)src->u16[i]);
        dst->u16[i] = ((res >> 16) & 0xffff);
    }
}
void vec_muluu128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 8; i++) {
        uint32_t res = dst->u16[i] * src->u16[i];
        dst->u16[i] = ((res >> 16) & 0xffff);
    }
}

// ---------------------------------------------------------------------------
// SSSE3 / SSE4.1 (three-byte 0F 38 / 0F 3A opcodes).
//
// These are the helpers behind the i386 JIT's three-byte vector dispatch (see
// emu/decode.h). They are validated bit-exact against real Intel silicon via
// tests/remote/corpus/sse4.c. The `z` numeric suffix is the memory-operand
// access width the gadget uses for that op (the source width for the widening
// pmovsx/pmovzx moves, the destination width for byte/word extracts), NOT the
// xmm register width -- which is always 128.
// ---------------------------------------------------------------------------

// pinsrd: insert a dword (from r/m32) into lane (imm & 3). dst is always xmm.
void vec_insert_d32(NO_CPU, const uint32_t *src, union xmm_reg *dst, uint8_t index) {
    dst->u32[index & 3] = *src;
}
// pinsrb: insert the low byte of r/m into byte lane (imm & 15).
void vec_insert_b8(NO_CPU, const uint8_t *src, union xmm_reg *dst, uint8_t index) {
    dst->u8[index & 15] = *src;
}
// pextrd / extractps: extract dword lane (imm & 3) to r/m32 (4-byte write is
// correct for both a memory destination and a full GP register).
void vec_extract_d32(NO_CPU, const union xmm_reg *src, uint32_t *dst, uint8_t index) {
    *dst = src->u32[index & 3];
}
// pextrb to a *memory* destination (m8): write exactly one byte.
void vec_extract_b8(NO_CPU, const union xmm_reg *src, uint8_t *dst, uint8_t index) {
    *dst = src->u8[index & 15];
}
// pextrb to a *register* destination: the byte is zero-extended to 32 bits.
void vec_extract_b_reg128(NO_CPU, const union xmm_reg *src, uint32_t *dst, uint8_t index) {
    *dst = src->u8[index & 15];
}
// pextrw (0F 3A 15) to a *memory* destination (m16): write exactly two bytes.
// (The register-destination form reuses vec_extract_w128, which zero-extends.)
void vec_extract_w_mem16(NO_CPU, const union xmm_reg *src, uint16_t *dst, uint8_t index) {
    *dst = src->u16[index & 7];
}

// palignr (SSSE3): concatenate dst:src (dst high, src low) into 32 bytes, shift
// right by `shift` bytes, take the low 16 into dst. shift >= 32 -> all zero.
void vec_palignr128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t shift) {
    uint8_t tmp[32];
    memcpy(&tmp[0], src->u8, 16);
    memcpy(&tmp[16], dst->u8, 16);
    union xmm_reg res;
    for (int i = 0; i < 16; i++) {
        int idx = i + shift;
        res.u8[i] = idx < 32 ? tmp[idx] : 0;
    }
    *dst = res;
}

// pshufb (SSSE3): dst (=reg) is BOTH the byte source and the destination; the
// control vector is r/m. result[i] = ctrl[i]&0x80 ? 0 : dst_orig[ctrl[i]&0xF].
void vec_pshufb128(NO_CPU, const union xmm_reg *control, union xmm_reg *dst) {
    union xmm_reg orig = *dst;
    union xmm_reg res;
    for (int i = 0; i < 16; i++)
        res.u8[i] = (control->u8[i] & 0x80) ? 0 : orig.u8[control->u8[i] & 0x0f];
    *dst = res;
}

// pabsb/w/d (SSSE3): dst = |src| per lane (signed).
void vec_pabsb128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 16; i++) { int8_t v = (int8_t)src->u8[i]; dst->u8[i] = v < 0 ? -v : v; }
}
void vec_pabsw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 8; i++) { int16_t v = (int16_t)src->u16[i]; dst->u16[i] = v < 0 ? -v : v; }
}
void vec_pabsd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 4; i++) { int32_t v = (int32_t)src->u32[i]; dst->u32[i] = v < 0 ? -(uint32_t)v : (uint32_t)v; }
}

// pmovsx/pmovzx (SSE4.1): sign-/zero-extend the low packed elements of the
// source. The `z` suffix is the source width consumed (so a memory source
// reads exactly that many bytes).
void vec_pmovzxbw64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 8; i++) r.u16[i] = src->u8[i]; *dst = r;
}
void vec_pmovsxbw64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 8; i++) r.u16[i] = (int16_t)(int8_t)src->u8[i]; *dst = r;
}
void vec_pmovzxbd32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 4; i++) r.u32[i] = src->u8[i]; *dst = r;
}
void vec_pmovsxbd32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 4; i++) r.u32[i] = (int32_t)(int8_t)src->u8[i]; *dst = r;
}
void vec_pmovzxbq16(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 2; i++) r.qw[i] = src->u8[i]; *dst = r;
}
void vec_pmovsxbq16(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 2; i++) r.qw[i] = (uint64_t)(int64_t)(int8_t)src->u8[i]; *dst = r;
}
void vec_pmovzxwd64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 4; i++) r.u32[i] = src->u16[i]; *dst = r;
}
void vec_pmovsxwd64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 4; i++) r.u32[i] = (int32_t)(int16_t)src->u16[i]; *dst = r;
}
void vec_pmovzxwq32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 2; i++) r.qw[i] = src->u16[i]; *dst = r;
}
void vec_pmovsxwq32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 2; i++) r.qw[i] = (uint64_t)(int64_t)(int16_t)src->u16[i]; *dst = r;
}
void vec_pmovzxdq64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 2; i++) r.qw[i] = src->u32[i]; *dst = r;
}
void vec_pmovsxdq64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r; for (int i = 0; i < 2; i++) r.qw[i] = (uint64_t)(int64_t)(int32_t)src->u32[i]; *dst = r;
}

// pmulld (SSE4.1): packed 32-bit low product.
void vec_pmulld128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 4; i++) dst->u32[i] = (uint32_t)(dst->u32[i] * src->u32[i]);
}
// pmuldq (SSE4.1): signed 32x32 -> 64 on lanes 0 and 2 -> qwords 0 and 1.
void vec_pmuldq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    int64_t r0 = (int64_t)(int32_t)dst->u32[0] * (int64_t)(int32_t)src->u32[0];
    int64_t r1 = (int64_t)(int32_t)dst->u32[2] * (int64_t)(int32_t)src->u32[2];
    dst->qw[0] = (uint64_t)r0; dst->qw[1] = (uint64_t)r1;
}
// pcmpeqq (SSE4.1) / pcmpgtq (SSE4.2): packed 64-bit compare -> all-ones/zero.
void vec_pcmpeqq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 2; i++) dst->qw[i] = dst->qw[i] == src->qw[i] ? ~0ULL : 0;
}
void vec_pcmpgtq128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 2; i++) dst->qw[i] = (int64_t)dst->qw[i] > (int64_t)src->qw[i] ? ~0ULL : 0;
}
// packusdw (SSE4.1): pack signed dwords (dst then src) to unsigned words, sat.
static inline uint16_t satusw(int32_t v) {
    if (v < 0) return 0;
    if (v > 0xffff) return 0xffff;
    return (uint16_t)v;
}
void vec_packusdw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r;
    for (int i = 0; i < 4; i++) r.u16[i]     = satusw((int32_t)dst->u32[i]);
    for (int i = 0; i < 4; i++) r.u16[4 + i] = satusw((int32_t)src->u32[i]);
    *dst = r;
}

// pmin*/pmax* (SSE4.1): the variants SSE2 lacks (signed byte, signed/unsigned
// dword, unsigned word). result keeps dst when the predicate holds, else src.
#define VEC_MINMAX(name, lane, ctype, cmp) \
    void vec_##name##128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) { \
        for (unsigned i = 0; i < array_size(dst->lane); i++) \
            dst->lane[i] = (ctype)dst->lane[i] cmp (ctype)src->lane[i] ? dst->lane[i] : src->lane[i]; \
    }
VEC_MINMAX(pminsb, u8,  int8_t,   <)
VEC_MINMAX(pmaxsb, u8,  int8_t,   >)
VEC_MINMAX(pminuw, u16, uint16_t, <)
VEC_MINMAX(pmaxuw, u16, uint16_t, >)
VEC_MINMAX(pminsd, u32, int32_t,  <)
VEC_MINMAX(pmaxsd, u32, int32_t,  >)
VEC_MINMAX(pminud, u32, uint32_t, <)
VEC_MINMAX(pmaxud, u32, uint32_t, >)
#undef VEC_MINMAX

// ptest (SSE4.1): ZF = (DEST & SRC)==0; CF = (SRC & ~DEST)==0; clears the rest.
// DEST is the reg operand (dst), SRC is r/m (src). Sets eager flags directly.
void vec_ptest128(struct cpu_state *cpu, const union xmm_reg *src, const union xmm_reg *dst) {
    unsigned __int128 s = src->u128, d = dst->u128;
    cpu->zf_res = cpu->pf_res = cpu->sf_res = 0;
    cpu->af_ops = 0;
    cpu->zf = (d & s) == 0;
    cpu->cf = (s & ~d) == 0;
    cpu->pf = cpu->sf = cpu->af = cpu->of = 0;
    cpu->cf_bit = cpu->cf;
    cpu->of_bit = 0;
}

// pblendvb / blendvps / blendvpd (SSE4.1): variable blend, mask is implicit
// XMM0 (high bit of each byte/dword/qword lane). Snapshot the mask first so a
// destination that aliases XMM0 stays correct.
void vec_pblendvb128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg mask = cpu->xmm[0];
    for (int i = 0; i < 16; i++) if (mask.u8[i] & 0x80) dst->u8[i] = src->u8[i];
}
void vec_blendvps128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg mask = cpu->xmm[0];
    for (int i = 0; i < 4; i++) if (mask.u32[i] & 0x80000000u) dst->u32[i] = src->u32[i];
}
void vec_blendvpd128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg mask = cpu->xmm[0];
    for (int i = 0; i < 2; i++) if (mask.qw[i] & 0x8000000000000000ULL) dst->qw[i] = src->qw[i];
}
// blendps / blendpd / pblendw (SSE4.1): imm-controlled blend (set bit -> src).
void vec_blend_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    for (int i = 0; i < 4; i++) if (imm & (1 << i)) dst->u32[i] = src->u32[i];
}
void vec_blend_pd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    for (int i = 0; i < 2; i++) if (imm & (1 << i)) dst->qw[i] = src->qw[i];
}
void vec_blend_w128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    for (int i = 0; i < 8; i++) if (imm & (1 << i)) dst->u16[i] = src->u16[i];
}

// roundps/pd/ss/sd (SSE4.1): imm8[1:0] picks the mode when imm8[2]==0
// (0 nearest-even, 1 floor, 2 ceil, 3 trunc); imm8[2]=1 means "use MXCSR",
// which iSH models as round-nearest-even. Scalar forms leave the upper lanes.
static inline float dc_round_f32(float x, uint8_t imm) {
    if (imm & 0x4) return rintf(x);
    switch (imm & 3) { case 1: return floorf(x); case 2: return ceilf(x); case 3: return truncf(x); default: return rintf(x); }
}
static inline double dc_round_f64(double x, uint8_t imm) {
    if (imm & 0x4) return rint(x);
    switch (imm & 3) { case 1: return floor(x); case 2: return ceil(x); case 3: return trunc(x); default: return rint(x); }
}
void vec_round_ps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    union xmm_reg r; for (int i = 0; i < 4; i++) r.f32[i] = dc_round_f32(src->f32[i], imm); *dst = r;
}
void vec_round_pd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    union xmm_reg r; for (int i = 0; i < 2; i++) r.f64[i] = dc_round_f64(src->f64[i], imm); *dst = r;
}
void vec_round_ss32(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    dst->f32[0] = dc_round_f32(src->f32[0], imm);
}
void vec_round_sd64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    dst->f64[0] = dc_round_f64(src->f64[0], imm);
}

// ---------------------------------------------------------------------------
// SSSE3 horizontal add/subtract, multiply-add, multiply-high-round, and sign.
// Completes the SSSE3 set begun above (pshufb/pabs/palignr). All forms are the
// 128-bit xmm variants; the deprecated MMX-register forms remain undefined, as
// elsewhere in this engine. Validated bit-exact vs real Intel (corpus/ssse3.c).
// ---------------------------------------------------------------------------

// signed saturate to 16 bits.
static inline uint16_t sat_i16(int32_t v) {
    if (v > 32767) return (uint16_t) 32767;
    if (v < -32768) return (uint16_t) -32768;
    return (uint16_t) v;
}

// phaddw (38 01): horizontal add of adjacent 16-bit pairs, wrap-around. The low
// 4 results come from dst's four pairs, the high 4 from src's four pairs.
void vec_phaddw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r;
    for (int i = 0; i < 4; i++) r.u16[i]     = (uint16_t) (dst->u16[2*i] + dst->u16[2*i+1]);
    for (int i = 0; i < 4; i++) r.u16[4 + i] = (uint16_t) (src->u16[2*i] + src->u16[2*i+1]);
    *dst = r;
}
// phaddd (38 02): horizontal add of adjacent 32-bit pairs.
void vec_phaddd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r;
    r.u32[0] = dst->u32[0] + dst->u32[1];
    r.u32[1] = dst->u32[2] + dst->u32[3];
    r.u32[2] = src->u32[0] + src->u32[1];
    r.u32[3] = src->u32[2] + src->u32[3];
    *dst = r;
}
// phaddsw (38 03): horizontal add of adjacent signed 16-bit pairs, signed sat.
void vec_phaddsw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r;
    for (int i = 0; i < 4; i++)
        r.u16[i]     = sat_i16((int32_t)(int16_t)dst->u16[2*i] + (int32_t)(int16_t)dst->u16[2*i+1]);
    for (int i = 0; i < 4; i++)
        r.u16[4 + i] = sat_i16((int32_t)(int16_t)src->u16[2*i] + (int32_t)(int16_t)src->u16[2*i+1]);
    *dst = r;
}
// pmaddubsw (38 04): dst bytes are UNSIGNED, src bytes SIGNED. Multiply, then
// add adjacent products and saturate to signed 16-bit. 8 word results.
void vec_pmaddubsw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r;
    for (int i = 0; i < 8; i++) {
        int32_t lo = (int32_t)(uint8_t)dst->u8[2*i]     * (int32_t)(int8_t)src->u8[2*i];
        int32_t hi = (int32_t)(uint8_t)dst->u8[2*i + 1] * (int32_t)(int8_t)src->u8[2*i + 1];
        r.u16[i] = sat_i16(lo + hi);
    }
    *dst = r;
}
// phsubw (38 05): horizontal subtract of adjacent 16-bit pairs (a[0]-a[1]).
void vec_phsubw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r;
    for (int i = 0; i < 4; i++) r.u16[i]     = (uint16_t) (dst->u16[2*i] - dst->u16[2*i+1]);
    for (int i = 0; i < 4; i++) r.u16[4 + i] = (uint16_t) (src->u16[2*i] - src->u16[2*i+1]);
    *dst = r;
}
// phsubd (38 06): horizontal subtract of adjacent 32-bit pairs.
void vec_phsubd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r;
    r.u32[0] = dst->u32[0] - dst->u32[1];
    r.u32[1] = dst->u32[2] - dst->u32[3];
    r.u32[2] = src->u32[0] - src->u32[1];
    r.u32[3] = src->u32[2] - src->u32[3];
    *dst = r;
}
// phsubsw (38 07): horizontal subtract of adjacent signed 16-bit pairs, sat.
void vec_phsubsw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r;
    for (int i = 0; i < 4; i++)
        r.u16[i]     = sat_i16((int32_t)(int16_t)dst->u16[2*i] - (int32_t)(int16_t)dst->u16[2*i+1]);
    for (int i = 0; i < 4; i++)
        r.u16[4 + i] = sat_i16((int32_t)(int16_t)src->u16[2*i] - (int32_t)(int16_t)src->u16[2*i+1]);
    *dst = r;
}
// psignb/w/d (38 08/09/0a): for each lane, negate dst when the src lane is
// negative, zero it when src is zero, leave it when src is positive. Unsigned
// negation avoids signed-overflow UB on the most-negative lane (matches HW).
void vec_psignb128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 16; i++) {
        int8_t s = (int8_t) src->u8[i];
        if (s < 0) dst->u8[i] = (uint8_t) (- (uint32_t) dst->u8[i]);
        else if (s == 0) dst->u8[i] = 0;
    }
}
void vec_psignw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 8; i++) {
        int16_t s = (int16_t) src->u16[i];
        if (s < 0) dst->u16[i] = (uint16_t) (- (uint32_t) dst->u16[i]);
        else if (s == 0) dst->u16[i] = 0;
    }
}
void vec_psignd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 4; i++) {
        int32_t s = (int32_t) src->u32[i];
        if (s < 0) dst->u32[i] = (uint32_t) (- (uint32_t) dst->u32[i]);
        else if (s == 0) dst->u32[i] = 0;
    }
}
// pmulhrsw (38 0b): signed 16-bit multiply, take bits [30:15] of the product,
// add 1, shift right 1 (round-to-nearest of the high word). Per lane.
void vec_pmulhrsw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    for (int i = 0; i < 8; i++) {
        int32_t p = (int32_t)(int16_t)dst->u16[i] * (int32_t)(int16_t)src->u16[i];
        dst->u16[i] = (uint16_t) (((p >> 14) + 1) >> 1);
    }
}

// ---------------------------------------------------------------------------
// SSE4.1 completion: insertps, dpps/dppd, mpsadbw, phminposuw. (movntdqa is a
// plain aligned 128-bit load and is wired directly in the decoders, no helper.)
// Validated bit-exact vs real Intel (corpus/sse4.c). The dot products use only
// finite test inputs and compute each multiply/add as a separate statement so
// no a*b+c is contracted into an FMA (which would round differently from x86).
// ---------------------------------------------------------------------------

// insertps, register source (66 0F 3A 21, mod==3): imm[7:6] picks the source
// dword, imm[5:4] the destination lane, imm[3:0] is a per-lane zero mask.
void vec_insertps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    uint32_t v = src->u32[(imm >> 6) & 3];
    dst->u32[(imm >> 4) & 3] = v;
    for (int i = 0; i < 4; i++)
        if (imm & (1 << i)) dst->u32[i] = 0;
}
// insertps, memory source (m32): the loaded dword is the value (imm[7:6] is
// ignored for the memory form); imm[5:4] dest lane, imm[3:0] zero mask.
void vec_insertps32(NO_CPU, const uint32_t *src, union xmm_reg *dst, uint8_t imm) {
    dst->u32[(imm >> 4) & 3] = *src;
    for (int i = 0; i < 4; i++)
        if (imm & (1 << i)) dst->u32[i] = 0;
}

// dpps (66 0F 3A 40): dot product of packed singles. imm[4..7] select which
// lane products enter the sum; imm[0..3] select which result lanes get it.
// Tree reduction order ((p0+p1)+(p2+p3)) matches Intel's documented pseudocode.
void vec_dpps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    float p0 = (imm & 0x10) ? dst->f32[0] * src->f32[0] : 0.0f;
    float p1 = (imm & 0x20) ? dst->f32[1] * src->f32[1] : 0.0f;
    float p2 = (imm & 0x40) ? dst->f32[2] * src->f32[2] : 0.0f;
    float p3 = (imm & 0x80) ? dst->f32[3] * src->f32[3] : 0.0f;
    float t01 = p0 + p1;
    float t23 = p2 + p3;
    float sum = t01 + t23;
    for (int j = 0; j < 4; j++)
        dst->f32[j] = (imm & (1 << j)) ? sum : 0.0f;
}
// dppd (66 0F 3A 41): dot product of packed doubles. imm[4..5] select the lane
// products; imm[0..1] select result lanes.
void vec_dppd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    double p0 = (imm & 0x10) ? dst->f64[0] * src->f64[0] : 0.0;
    double p1 = (imm & 0x20) ? dst->f64[1] * src->f64[1] : 0.0;
    double sum = p0 + p1;
    for (int j = 0; j < 2; j++)
        dst->f64[j] = (imm & (1 << j)) ? sum : 0.0;
}

// mpsadbw (66 0F 3A 42): eight sums of 4-byte absolute differences. The reg
// operand (dst, SRC1) supplies the sliding 11-byte window at offset imm[2]*4;
// the r/m operand (src, SRC2) supplies the fixed 4-byte block at imm[1:0]*4.
void vec_mpsadbw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    unsigned src_off = (imm & 3) * 4;
    unsigned dst_off = ((imm >> 2) & 1) * 4;
    union xmm_reg r;
    for (int i = 0; i < 8; i++) {
        int sum = 0;
        for (int j = 0; j < 4; j++) {
            int a = (int) dst->u8[dst_off + i + j];
            int b = (int) src->u8[src_off + j];
            sum += a > b ? a - b : b - a;
        }
        r.u16[i] = (uint16_t) sum;
    }
    *dst = r;
}

// phminposuw (66 0F 38 41): result word0 = the minimum unsigned word of src,
// word1 = its (leftmost) lane index, words 2..7 = 0. Unary (src -> dst).
void vec_phminposuw128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    uint16_t min = src->u16[0];
    uint16_t idx = 0;
    for (int i = 1; i < 8; i++)
        if (src->u16[i] < min) { min = src->u16[i]; idx = (uint16_t) i; }
    union xmm_reg r = {0};
    r.u16[0] = min;
    r.u16[1] = idx;
    *dst = r;
}

// ---------------------------------------------------------------------------
// SSE3 (PNI): duplicating moves and horizontal/alternating add-subtract. The
// add/sub ops are plain IEEE operations (no FMA contraction) so finite results
// match x86 bit-for-bit. Validated vs real Intel (corpus/sse3.c).
// ---------------------------------------------------------------------------

// movsldup (F3 0F 12): duplicate the even-indexed singles -> [s0,s0,s2,s2].
void vec_movsldup128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r;
    r.u32[0] = src->u32[0]; r.u32[1] = src->u32[0];
    r.u32[2] = src->u32[2]; r.u32[3] = src->u32[2];
    *dst = r;
}
// movshdup (F3 0F 16): duplicate the odd-indexed singles -> [s1,s1,s3,s3].
void vec_movshdup128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r;
    r.u32[0] = src->u32[1]; r.u32[1] = src->u32[1];
    r.u32[2] = src->u32[3]; r.u32[3] = src->u32[3];
    *dst = r;
}
// movddup (F2 0F 12): duplicate the low double into both lanes. The `64` suffix
// is the memory-source access width (m64); for a register source only qw[0] is
// read. dst's upper lane is overwritten, so this is not a partial-merge move.
void vec_movddup64(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    uint64_t lo = src->qw[0];
    dst->qw[0] = lo;
    dst->qw[1] = lo;
}
// addsubps/pd (F2/66 0F D0): subtract the even lanes, add the odd lanes.
void vec_addsubps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->f32[0] = dst->f32[0] - src->f32[0];
    dst->f32[1] = dst->f32[1] + src->f32[1];
    dst->f32[2] = dst->f32[2] - src->f32[2];
    dst->f32[3] = dst->f32[3] + src->f32[3];
}
void vec_addsubpd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    dst->f64[0] = dst->f64[0] - src->f64[0];
    dst->f64[1] = dst->f64[1] + src->f64[1];
}
// haddps/pd (F2/66 0F 7C): horizontal add. Low results come from dst's adjacent
// pairs, high results from src's pairs.
void vec_haddps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r;
    r.f32[0] = dst->f32[0] + dst->f32[1];
    r.f32[1] = dst->f32[2] + dst->f32[3];
    r.f32[2] = src->f32[0] + src->f32[1];
    r.f32[3] = src->f32[2] + src->f32[3];
    *dst = r;
}
void vec_haddpd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r;
    r.f64[0] = dst->f64[0] + dst->f64[1];
    r.f64[1] = src->f64[0] + src->f64[1];
    *dst = r;
}
// hsubps/pd (F2/66 0F 7D): horizontal subtract (a[even]-a[odd]).
void vec_hsubps128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r;
    r.f32[0] = dst->f32[0] - dst->f32[1];
    r.f32[1] = dst->f32[2] - dst->f32[3];
    r.f32[2] = src->f32[0] - src->f32[1];
    r.f32[3] = src->f32[2] - src->f32[3];
    *dst = r;
}
void vec_hsubpd128(NO_CPU, const union xmm_reg *src, union xmm_reg *dst) {
    union xmm_reg r;
    r.f64[0] = dst->f64[0] - dst->f64[1];
    r.f64[1] = src->f64[0] - src->f64[1];
    *dst = r;
}

// ---------------------------------------------------------------------------
// SSE4.2: crc32 (CRC32C / Castagnoli) and the pcmp{e,i}str{i,m} string ops.
// Validated bit-exact vs real Intel (corpus/sse42.c).
// ---------------------------------------------------------------------------

// CRC32C accumulates with the reflected Castagnoli polynomial 0x82F63B78.
static inline uint32_t crc32c_step(uint32_t crc, uint8_t b) {
    crc ^= b;
    for (int i = 0; i < 8; i++)
        crc = (crc >> 1) ^ (0x82f63b78u & (uint32_t) -(int32_t)(crc & 1));
    return crc;
}
// crc32 r32, r/m8/16/32 (and r64,r/m64): fold the source bytes (low to high)
// into the 32-bit accumulator held in the low bits of dst. The 64-bit dst form
// zero-extends, which the caller handles by writing through a uint64_t.
void vec_crc32_8(NO_CPU, const uint8_t *src, uint32_t *dst) {
    *dst = crc32c_step(*dst, src[0]);
}
void vec_crc32_16(NO_CPU, const uint16_t *src, uint32_t *dst) {
    uint32_t c = *dst;
    c = crc32c_step(c, (uint8_t) src[0]);
    c = crc32c_step(c, (uint8_t) (src[0] >> 8));
    *dst = c;
}
void vec_crc32_32(NO_CPU, const uint32_t *src, uint32_t *dst) {
    uint32_t c = *dst, v = src[0];
    for (int i = 0; i < 4; i++) c = crc32c_step(c, (uint8_t) (v >> (i * 8)));
    *dst = c;
}
void vec_crc32_64(NO_CPU, const uint64_t *src, uint64_t *dst) {
    uint32_t c = (uint32_t) *dst; uint64_t v = src[0];
    for (int i = 0; i < 8; i++) c = crc32c_step(c, (uint8_t) (v >> (i * 8)));
    *dst = c; // zero-extended into the 64-bit destination
}

// pcmpstr core. s1 is the reg operand (xmm1), s2 the r/m operand (xmm2/m128).
// len1/len2 are the valid element counts (explicit forms pass EAX/EDX, implicit
// forms pass the index of the first null element). Writes ECX (index forms) or
// XMM0 (mask forms) and sets EFLAGS exactly as the hardware does.
static void pcmpstr_core(struct cpu_state *cpu, const union xmm_reg *s2,
        const union xmm_reg *s1, uint8_t imm, int len1, int len2, bool mask_out) {
    bool word = imm & 1;            // element size: 0=byte, 1=word
    bool sign = (imm >> 1) & 1;     // 0=unsigned, 1=signed
    int agg = (imm >> 2) & 3;       // 0=any, 1=ranges, 2=each, 3=ordered
    int pol = (imm >> 4) & 3;       // polarity
    bool msb = (imm >> 6) & 1;      // index: MSB vs LSB; mask: expanded vs bit
    int n = word ? 8 : 16;
    if (len1 < 0) len1 = -len1;
    if (len2 < 0) len2 = -len2;
    if (len1 > n) len1 = n;
    if (len2 > n) len2 = n;

    int e1[16], e2[16];
    for (int i = 0; i < n; i++) {
        if (word) {
            e1[i] = sign ? (int)(int16_t) s1->u16[i] : (int) s1->u16[i];
            e2[i] = sign ? (int)(int16_t) s2->u16[i] : (int) s2->u16[i];
        } else {
            e1[i] = sign ? (int)(int8_t) s1->u8[i] : (int) s1->u8[i];
            e2[i] = sign ? (int)(int8_t) s2->u8[i] : (int) s2->u8[i];
        }
    }

    int IntRes1 = 0;
    for (int j = 0; j < n; j++) {
        bool r = false;
        switch (agg) {
            case 0: // EqualAny: src2[j] matches any valid src1[i]
                for (int i = 0; i < n; i++)
                    if (i < len1 && j < len2 && e1[i] == e2[j]) r = true;
                break;
            case 1: // Ranges: src1 holds (low,high) pairs
                for (int i = 0; i + 1 < n; i += 2)
                    if (i < len1 && (i + 1) < len1 && j < len2 &&
                            e2[j] >= e1[i] && e2[j] <= e1[i + 1]) r = true;
                break;
            case 2: { // EqualEach: element-wise src1[j]==src2[j]
                bool v1 = j < len1, v2 = j < len2;
                if (v1 && v2) r = (e1[j] == e2[j]);
                else if (!v1 && !v2) r = true;   // both invalid -> equal
                else r = false;
                break;
            }
            default: { // EqualOrdered: substring of src1 starting at src2[j]
                r = true;
                for (int i = 0; i + j < n; i++) {
                    bool v1 = i < len1, v2 = (i + j) < len2;
                    if (!v1) break;              // needle exhausted -> still a match
                    if (v1 && !v2) { r = false; break; } // needle longer than tail
                    if (e1[i] != e2[i + j]) { r = false; break; }
                }
                break;
            }
        }
        if (r) IntRes1 |= (1 << j);
    }

    // polarity -> IntRes2
    int valid_mask = (n == 16) ? 0xffff : 0xff;
    int IntRes2;
    switch (pol) {
        case 1: IntRes2 = (~IntRes1) & valid_mask; break;            // negate all
        case 3: { // negate only where src2 element is valid
            int m = (len2 >= n) ? valid_mask : ((1 << len2) - 1);
            IntRes2 = (IntRes1 ^ m) & valid_mask;
            break;
        }
        default: IntRes2 = IntRes1 & valid_mask; break;              // 0,2: positive
    }

    // outputs
    if (mask_out) {
        union xmm_reg res = {0};
        if (msb) {
            // expanded mask: each element all-ones if its IntRes2 bit is set
            for (int j = 0; j < n; j++) {
                if (IntRes2 & (1 << j)) {
                    if (word) res.u16[j] = 0xffff;
                    else res.u8[j] = 0xff;
                }
            }
        } else {
            res.u16[0] = (uint16_t) (IntRes2 & 0xffff); // bit mask, zero-extended
        }
        cpu->xmm[0] = res;
    } else {
        int idx;
        if (IntRes2 == 0) {
            idx = n;
        } else if (msb) {
            idx = 0;
            for (int j = 0; j < n; j++) if (IntRes2 & (1 << j)) idx = j;
        } else {
            idx = 0;
            while (!(IntRes2 & (1 << idx))) idx++;
        }
        cpu->ecx = (dword_t) idx;
    }

    // flags: CF=IntRes2!=0, ZF=len2<n, SF=len1<n, OF=IntRes2[0]; AF=PF=0.
    cpu->zf_res = cpu->sf_res = cpu->pf_res = 0;
    cpu->af_ops = 0;
    cpu->cf = IntRes2 != 0;
    cpu->zf = len2 < n;
    cpu->sf = len1 < n;
    cpu->of = IntRes2 & 1;
    cpu->af = 0;
    cpu->pf = 0;
    cpu->cf_bit = cpu->cf;
    cpu->of_bit = cpu->of;
}

// Implicit length: number of valid elements before the first null element.
static int pcmpstr_implicit_len(const union xmm_reg *x, bool word) {
    int n = word ? 8 : 16;
    for (int i = 0; i < n; i++) {
        if (word) { if (x->u16[i] == 0) return i; }
        else { if (x->u8[i] == 0) return i; }
    }
    return n;
}

// pcmpestrm/estri (explicit length in EAX=src1, EDX=src2) and pcmpistrm/istri
// (implicit length). src = xmm2/m128 (the r/m operand), dst = xmm1 (the reg).
void vec_pcmpestrm128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    pcmpstr_core(cpu, src, dst, imm, (int)(int32_t) cpu->eax, (int)(int32_t) cpu->edx, true);
}
void vec_pcmpestri128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    pcmpstr_core(cpu, src, dst, imm, (int)(int32_t) cpu->eax, (int)(int32_t) cpu->edx, false);
}
void vec_pcmpistrm128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    bool word = imm & 1;
    pcmpstr_core(cpu, src, dst, imm, pcmpstr_implicit_len(dst, word), pcmpstr_implicit_len(src, word), true);
}
void vec_pcmpistri128(struct cpu_state *cpu, const union xmm_reg *src, union xmm_reg *dst, uint8_t imm) {
    bool word = imm & 1;
    pcmpstr_core(cpu, src, dst, imm, pcmpstr_implicit_len(dst, word), pcmpstr_implicit_len(src, word), false);
}
