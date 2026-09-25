#include <math.h>

#include "emu/avx.h"
#include "emu/fpenv.h"

// Elementwise binary ops. AVX defines wide integer ops as independent
// per-128-bit-lane operations, but for a purely elementwise op (lane width
// divides evenly into 128 bits, no lane reads another lane's data) iterating
// the flat buffer is identical to iterating lane-by-lane, so these need no
// lane bookkeeping. Anything that *does* cross lanes (shuffles, packs,
// permutes) gets its own helper below.

void avx_binop(enum avx_op op, unsigned lb, unsigned vlen,
        const uint8_t *s1, const uint8_t *s2, uint8_t *d) {
    unsigned total = vlen / 8;

    if (op == AVX_XOR || op == AVX_AND || op == AVX_ANDN || op == AVX_OR) {
        for (unsigned i = 0; i < total; i++) {
            switch (op) {
            case AVX_XOR: d[i] = s1[i] ^ s2[i]; break;
            case AVX_AND: d[i] = s1[i] & s2[i]; break;
            case AVX_ANDN: d[i] = (uint8_t) (~s1[i] & s2[i]); break;
            case AVX_OR: d[i] = s1[i] | s2[i]; break;
            default: break;
            }
        }
        return;
    }

    uint64_t mask = avx_lane_mask(lb);
    int64_t smax = lb >= 8 ? INT64_MAX : (INT64_C(1) << (lb * 8 - 1)) - 1;
    int64_t smin = lb >= 8 ? INT64_MIN : -(INT64_C(1) << (lb * 8 - 1));

    for (unsigned i = 0; i < total; i += lb) {
        uint64_t a = avx_lane_get(s1 + i, lb), b = avx_lane_get(s2 + i, lb), r = 0;
        int64_t sa = avx_lane_sext(a, lb), sb = avx_lane_sext(b, lb);
        switch (op) {
        case AVX_ADD: r = (a + b) & mask; break;
        case AVX_SUB: r = (a - b) & mask; break;
        case AVX_ADDS: {
            int64_t t = sa + sb;
            r = (uint64_t) (t > smax ? smax : t < smin ? smin : t) & mask;
            break;
        }
        case AVX_SUBS: {
            int64_t t = sa - sb;
            r = (uint64_t) (t > smax ? smax : t < smin ? smin : t) & mask;
            break;
        }
        case AVX_ADDUS: {
            uint64_t t = (a + b) & mask;
            r = t < a ? mask : t; // wrapped within the lane -> saturate high
            break;
        }
        case AVX_SUBUS: r = a > b ? a - b : 0; break;
        case AVX_CMPEQ: r = a == b ? mask : 0; break;
        case AVX_CMPGT: r = sa > sb ? mask : 0; break;
        case AVX_MINS: r = (uint64_t) (sa < sb ? sa : sb) & mask; break;
        case AVX_MAXS: r = (uint64_t) (sa > sb ? sa : sb) & mask; break;
        case AVX_MINU: r = a < b ? a : b; break;
        case AVX_MAXU: r = a > b ? a : b; break;
        case AVX_AVG: r = (a + b + 1) >> 1; break;
        case AVX_MULLO: r = (a * b) & mask; break;
        case AVX_MULHI: r = (uint64_t) ((sa * sb) >> (lb * 8)) & mask; break;
        case AVX_MULHIU: r = ((a * b) >> (lb * 8)) & mask; break;
        default: break;
        }
        avx_lane_put(d + i, lb, r);
    }
}

// Shifts. A count >= the lane width produces 0 (or an all-sign-bits result
// for an arithmetic right shift) rather than C's undefined over-shift.

void avx_shift(enum avx_shift_kind kind, unsigned lb, unsigned vlen,
        const uint8_t *s, uint64_t count, uint8_t *d) {
    unsigned total = vlen / 8;
    unsigned bits = lb * 8;
    uint64_t mask = avx_lane_mask(lb);
    for (unsigned i = 0; i < total; i += lb) {
        uint64_t v = avx_lane_get(s + i, lb), r;
        if (count >= bits) {
            r = kind == AVX_SAR ? (uint64_t) (avx_lane_sext(v, lb) < 0 ? mask : 0) : 0;
        } else if (kind == AVX_SHL) {
            r = (v << count) & mask;
        } else if (kind == AVX_SHR) {
            r = v >> count;
        } else {
            r = (uint64_t) (avx_lane_sext(v, lb) >> count) & mask;
        }
        avx_lane_put(d + i, lb, r);
    }
}

// Variable per-lane shift (VPSLLVD/VPSRLVD/VPSRAVD/...): the count comes from
// the corresponding lane of a second vector rather than a scalar.
void avx_shift_var(enum avx_shift_kind kind, unsigned lb, unsigned vlen,
        const uint8_t *s, const uint8_t *counts, uint8_t *d) {
    unsigned total = vlen / 8;
    unsigned bits = lb * 8;
    uint64_t mask = avx_lane_mask(lb);
    for (unsigned i = 0; i < total; i += lb) {
        uint64_t v = avx_lane_get(s + i, lb);
        uint64_t count = avx_lane_get(counts + i, lb);
        uint64_t r;
        if (count >= bits) {
            r = kind == AVX_SAR ? (uint64_t) (avx_lane_sext(v, lb) < 0 ? mask : 0) : 0;
        } else if (kind == AVX_SHL) {
            r = (v << count) & mask;
        } else if (kind == AVX_SHR) {
            r = v >> count;
        } else {
            r = (uint64_t) (avx_lane_sext(v, lb) >> count) & mask;
        }
        avx_lane_put(d + i, lb, r);
    }
}

// VPSLLDQ/VPSRLDQ: whole-lane BYTE shift, independently within each 128-bit
// lane (not a bit shift, and not across the lane boundary).
void avx_byte_shift(bool left, unsigned vlen, unsigned count, const uint8_t *s, uint8_t *d) {
    for (unsigned lane = 0; lane < vlen / 8; lane += 16) {
        uint8_t tmp[16];
        for (unsigned j = 0; j < 16; j++) {
            unsigned src = left ? j - count : j + count;
            tmp[j] = (left ? (j >= count) : (j + count < 16)) ? s[lane + src] : 0;
        }
        memcpy(d + lane, tmp, 16);
    }
}

// VPSHUFB: byte shuffle, lane-local. Index bit 7 forces a zero byte; only the
// low 4 bits select, so a 256-bit shuffle can never reach the other lane.
void avx_pshufb(unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d) {
    for (unsigned lane = 0; lane < vlen / 8; lane += 16) {
        uint8_t tmp[16];
        for (unsigned j = 0; j < 16; j++) {
            uint8_t idx = s2[lane + j];
            tmp[j] = (idx & 0x80) ? 0 : s1[lane + (idx & 0xf)];
        }
        memcpy(d + lane, tmp, 16);
    }
}

// VPSHUFD / VPSHUFHW / VPSHUFLW: dword or word shuffle by imm8, lane-local.
void avx_pshufd(unsigned vlen, byte_t imm, const uint8_t *s, uint8_t *d) {
    for (unsigned lane = 0; lane < vlen / 8; lane += 16) {
        uint32_t in[4], out[4];
        memcpy(in, s + lane, 16);
        for (unsigned j = 0; j < 4; j++)
            out[j] = in[(imm >> (2 * j)) & 3];
        memcpy(d + lane, out, 16);
    }
}

void avx_pshufw_half(unsigned vlen, byte_t imm, bool high, const uint8_t *s, uint8_t *d) {
    for (unsigned lane = 0; lane < vlen / 8; lane += 16) {
        uint16_t in[8], out[8];
        memcpy(in, s + lane, 16);
        memcpy(out, in, 16);
        unsigned base = high ? 4 : 0;
        for (unsigned j = 0; j < 4; j++)
            out[base + j] = in[base + ((imm >> (2 * j)) & 3)];
        memcpy(d + lane, out, 16);
    }
}

// VPUNPCK{L,H}*: interleave elements from the low (or high) half of each
// 128-bit lane.
void avx_unpack(bool high, unsigned lb, unsigned vlen,
        const uint8_t *s1, const uint8_t *s2, uint8_t *d) {
    for (unsigned lane = 0; lane < vlen / 8; lane += 16) {
        uint8_t tmp[16];
        unsigned base = high ? 8 : 0;
        unsigned o = 0;
        for (unsigned j = 0; j < 8 / lb; j++) {
            memcpy(tmp + o, s1 + lane + base + j * lb, lb); o += lb;
            memcpy(tmp + o, s2 + lane + base + j * lb, lb); o += lb;
        }
        memcpy(d + lane, tmp, 16);
    }
}

// VPACK{SS,US}{WB,DW}: narrow two sources to half-width with saturation,
// lane-local (src1's results fill the low half of each destination lane).
void avx_pack(bool to_signed, unsigned src_lb, unsigned vlen,
        const uint8_t *s1, const uint8_t *s2, uint8_t *d) {
    unsigned dst_lb = src_lb / 2;
    int64_t smax = (INT64_C(1) << (dst_lb * 8 - 1)) - 1;
    int64_t smin = -(INT64_C(1) << (dst_lb * 8 - 1));
    uint64_t umax = avx_lane_mask(dst_lb);
    for (unsigned lane = 0; lane < vlen / 8; lane += 16) {
        uint8_t tmp[16];
        unsigned o = 0;
        for (unsigned which = 0; which < 2; which++) {
            const uint8_t *src = which == 0 ? s1 : s2;
            for (unsigned j = 0; j < 16; j += src_lb) {
                int64_t v = avx_lane_sext(avx_lane_get(src + lane + j, src_lb), src_lb);
                uint64_t r = to_signed
                    ? (uint64_t) (v > smax ? smax : v < smin ? smin : v) & umax
                    : (uint64_t) (v < 0 ? 0 : ((uint64_t) v > umax ? umax : (uint64_t) v));
                avx_lane_put(tmp + o, dst_lb, r);
                o += dst_lb;
            }
        }
        memcpy(d + lane, tmp, 16);
    }
}

// VPMOVZX*/VPMOVSX*: widen the low elements of the source. Unlike most AVX2
// ops these read a *contiguous* source half (vlen/2 bits for a 2x widen),
// not one per 128-bit lane, so the source is walked linearly.
void avx_widen(bool sign, unsigned src_lb, unsigned dst_lb, unsigned vlen,
        const uint8_t *s, uint8_t *d) {
    unsigned lanes = (vlen / 8) / dst_lb;
    for (unsigned j = 0; j < lanes; j++) {
        uint64_t v = avx_lane_get(s + j * src_lb, src_lb);
        uint64_t r = sign ? (uint64_t) avx_lane_sext(v, src_lb) & avx_lane_mask(dst_lb) : v;
        avx_lane_put(d + j * dst_lb, dst_lb, r);
    }
}

void avx_broadcast(unsigned lb, unsigned vlen, const uint8_t *s, uint8_t *d) {
    uint64_t v = avx_lane_get(s, lb);
    for (unsigned i = 0; i < vlen / 8; i += lb)
        avx_lane_put(d + i, lb, v);
}

// VPALIGNR: per 128-bit lane, concatenate src1:src2 (src1 high) and extract
// 16 bytes starting `imm` bytes in from the low end.
void avx_palignr(unsigned vlen, byte_t imm, const uint8_t *s1, const uint8_t *s2, uint8_t *d) {
    for (unsigned lane = 0; lane < vlen / 8; lane += 16) {
        uint8_t cat[32], tmp[16];
        memcpy(cat, s2 + lane, 16);
        memcpy(cat + 16, s1 + lane, 16);
        for (unsigned j = 0; j < 16; j++) {
            unsigned idx = imm + j;
            tmp[j] = idx < 32 ? cat[idx] : 0;
        }
        memcpy(d + lane, tmp, 16);
    }
}

// VPMADDWD: signed word pairs multiplied and summed into dwords.
void avx_pmaddwd(unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d) {
    for (unsigned i = 0; i < vlen / 8; i += 4) {
        int32_t a0 = (int16_t) avx_lane_get(s1 + i, 2);
        int32_t a1 = (int16_t) avx_lane_get(s1 + i + 2, 2);
        int32_t b0 = (int16_t) avx_lane_get(s2 + i, 2);
        int32_t b1 = (int16_t) avx_lane_get(s2 + i + 2, 2);
        avx_lane_put(d + i, 4, (uint32_t) (a0 * b0 + a1 * b1));
    }
}

// VPMADDUBSW: unsigned bytes from src1 times signed bytes from src2, summed
// in pairs into signed-saturated words.
void avx_pmaddubsw(unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d) {
    for (unsigned i = 0; i < vlen / 8; i += 2) {
        int32_t a0 = s1[i], a1 = s1[i + 1];
        int32_t b0 = (int8_t) s2[i], b1 = (int8_t) s2[i + 1];
        int32_t t = a0 * b0 + a1 * b1;
        if (t > 32767) t = 32767;
        if (t < -32768) t = -32768;
        avx_lane_put(d + i, 2, (uint16_t) t);
    }
}

// VPMULUDQ: low 32 bits of each 64-bit lane, multiplied to a full 64-bit result.
void avx_pmuludq(unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d) {
    for (unsigned i = 0; i < vlen / 8; i += 8) {
        uint64_t a = (uint32_t) avx_lane_get(s1 + i, 4);
        uint64_t b = (uint32_t) avx_lane_get(s2 + i, 4);
        avx_lane_put(d + i, 8, a * b);
    }
}

// VPSADBW: sum of absolute byte differences, one 16-bit total per 64-bit lane.
void avx_psadbw(unsigned vlen, const uint8_t *s1, const uint8_t *s2, uint8_t *d) {
    for (unsigned i = 0; i < vlen / 8; i += 8) {
        unsigned sum = 0;
        for (unsigned j = 0; j < 8; j++) {
            int diff = (int) s1[i + j] - (int) s2[i + j];
            sum += (unsigned) (diff < 0 ? -diff : diff);
        }
        avx_lane_put(d + i, 8, sum);
    }
}

void avx_abs(unsigned lb, unsigned vlen, const uint8_t *s, uint8_t *d) {
    uint64_t mask = avx_lane_mask(lb);
    for (unsigned i = 0; i < vlen / 8; i += lb) {
        int64_t v = avx_lane_sext(avx_lane_get(s + i, lb), lb);
        avx_lane_put(d + i, lb, (uint64_t) (v < 0 ? -v : v) & mask);
    }
}

// ---- floating point ----
//
// Emulated with host float/double arithmetic. x86's MIN/MAX are deliberately
// NOT symmetric and NOT NaN-propagating: both are defined as "compare, and on
// anything other than a strict ordered win for src1, return src2", so a NaN in
// either operand yields src2. Writing these as fminf()/C ternaries with the
// operands the other way round is a classic silent-wrong-answer bug.

double avx_fp_apply(enum avx_fp_op op, double a, double b) {
    switch (op) {
    case AVX_FADD: return a + b;
    case AVX_FSUB: return a - b;
    case AVX_FMUL: return a * b;
    case AVX_FDIV: return a / b;
    case AVX_FMIN: return a < b ? a : b;
    case AVX_FMAX: return a > b ? a : b;
    }
    return 0;
}

// x86 NaN handling, done on the RAW BITS, because neither C nor the host FPU
// reproduces it and the differences are invisible until you diff against real
// silicon. Three separate things were wrong before this, all measured against
// an x86_64 host:
//
//  1. The single-precision path computed in double and cast back. That
//     round-trip QUIETS a signalling NaN, so VMAXPS returning src2 verbatim
//     (which it must) returned 0x7fc000ff where hardware returns 0x7f8000ff.
//  2. Division with NaN operands lost the SIGN: 0xffffffff / 0xffffffff gave
//     0x7fffffff where hardware gives 0xffffffff. C says nothing about which
//     NaN an operation returns, so relying on it was never going to hold.
//  3. VSQRTPS of a negative returned a positive NaN instead of the x86 real
//     indefinite. That one already had a fix for the SSE path
//     (amd64_sse_sqrt_f32) which the VEX path did not use.
//
// The rules implemented, from the SDM's operand-precedence table:
//  - ADD/SUB/MUL/DIV: a signalling NaN in src1 wins, quieted; else a
//    signalling NaN in src2, quieted; else a quiet NaN in src1 unchanged;
//    else a quiet NaN in src2 unchanged.
//  - MIN/MAX: if EITHER operand is a NaN of any kind the result is src2
//    UNCHANGED -- not quieted, not canonicalised. The same rule makes
//    MIN/MAX of +0.0 and -0.0 return src2, which is why zeros are handled
//    here too rather than left to the comparison.
static inline bool avx_f32_is_nan(uint32_t b) {
    return (b & 0x7f800000u) == 0x7f800000u && (b & 0x007fffffu) != 0;
}
static inline bool avx_f32_is_snan(uint32_t b) {
    return avx_f32_is_nan(b) && (b & 0x00400000u) == 0;
}
static inline bool avx_f64_is_nan(uint64_t b) {
    return (b & 0x7ff0000000000000ull) == 0x7ff0000000000000ull &&
           (b & 0x000fffffffffffffull) != 0;
}
static inline bool avx_f64_is_snan(uint64_t b) {
    return avx_f64_is_nan(b) && (b & 0x0008000000000000ull) == 0;
}

// Returns true and fills *out when x86 selects one of the operands' NaNs (or,
// for MIN/MAX, src2) rather than computing anything.
static bool avx_fp_nan_result32(enum avx_fp_op op, uint32_t a, uint32_t b,
        uint32_t *out) {
    if (op == AVX_FMIN || op == AVX_FMAX) {
        bool zeros = (a & 0x7fffffffu) == 0 && (b & 0x7fffffffu) == 0;
        if (avx_f32_is_nan(a) || avx_f32_is_nan(b) || zeros) {
            // MIN/MAX are invalid on any NaN, quiet or not.
            if (!zeros || avx_f32_is_nan(a) || avx_f32_is_nan(b))
                fpenv_raise_invalid();
            *out = b;
            return true;
        }
        return false;
    }
    if (avx_f32_is_snan(a)) { fpenv_raise_invalid(); *out = a | 0x00400000u; return true; }
    if (avx_f32_is_snan(b)) { fpenv_raise_invalid(); *out = b | 0x00400000u; return true; }
    if (avx_f32_is_nan(a))  { *out = a; return true; }
    if (avx_f32_is_nan(b))  { *out = b; return true; }
    // Invalid-operation cases with no NaN operand. x86 answers all of them with
    // the real indefinite, 0xffc00000 -- SIGN SET -- where arm64 produces the
    // positive default NaN 0x7fc00000. Measured: 0.0/0.0 differed by exactly
    // that bit. Same divergence as sqrt of a negative, and it has to be listed
    // explicitly because C says nothing about which NaN an operation yields.
    {
        bool az = (a & 0x7fffffffu) == 0, bz = (b & 0x7fffffffu) == 0;
        bool ai = (a & 0x7fffffffu) == 0x7f800000u;
        bool bi = (b & 0x7fffffffu) == 0x7f800000u;
        bool invalid =
            (op == AVX_FDIV && ((az && bz) || (ai && bi))) ||
            (op == AVX_FMUL && ((az && bi) || (ai && bz))) ||
            (op == AVX_FADD && ai && bi && ((a ^ b) & 0x80000000u) != 0) ||
            (op == AVX_FSUB && ai && bi && ((a ^ b) & 0x80000000u) == 0);
        if (invalid) { fpenv_raise_invalid(); *out = 0xffc00000u; return true; }
    }
    return false;
}

static bool avx_fp_nan_result64(enum avx_fp_op op, uint64_t a, uint64_t b,
        uint64_t *out) {
    if (op == AVX_FMIN || op == AVX_FMAX) {
        bool zeros = (a & 0x7fffffffffffffffull) == 0 &&
                     (b & 0x7fffffffffffffffull) == 0;
        if (avx_f64_is_nan(a) || avx_f64_is_nan(b) || zeros) {
            if (!zeros || avx_f64_is_nan(a) || avx_f64_is_nan(b))
                fpenv_raise_invalid();
            *out = b;
            return true;
        }
        return false;
    }
    if (avx_f64_is_snan(a)) { fpenv_raise_invalid(); *out = a | 0x0008000000000000ull; return true; }
    if (avx_f64_is_snan(b)) { fpenv_raise_invalid(); *out = b | 0x0008000000000000ull; return true; }
    if (avx_f64_is_nan(a))  { *out = a; return true; }
    if (avx_f64_is_nan(b))  { *out = b; return true; }
    {
        bool az = (a & 0x7fffffffffffffffull) == 0;
        bool bz = (b & 0x7fffffffffffffffull) == 0;
        bool ai = (a & 0x7fffffffffffffffull) == 0x7ff0000000000000ull;
        bool bi = (b & 0x7fffffffffffffffull) == 0x7ff0000000000000ull;
        bool invalid =
            (op == AVX_FDIV && ((az && bz) || (ai && bi))) ||
            (op == AVX_FMUL && ((az && bi) || (ai && bz))) ||
            (op == AVX_FADD && ai && bi &&
                ((a ^ b) & 0x8000000000000000ull) != 0) ||
            (op == AVX_FSUB && ai && bi &&
                ((a ^ b) & 0x8000000000000000ull) == 0);
        if (invalid) { fpenv_raise_invalid(); *out = 0xfff8000000000000ull; return true; }
    }
    return false;
}

// x86 SQRT of a negative (non-NaN) operand is the real indefinite --
// 0xffc00000 / 0xfff8000000000000, sign bit SET. arm64 yields a positive NaN
// there. A NaN input falls through to the host, which quiets it as x86 does,
// and -0.0 is not < 0 so sqrt(-0) = -0 as required.
float avx_sqrt_f32(float x) {
    if (x < 0.0f) {
        fpenv_raise_invalid();
        uint32_t bits = 0xffc00000u;
        float r;
        memcpy(&r, &bits, sizeof(r));
        return r;
    }
    return sqrtf(x);
}

double avx_sqrt_f64(double x) {
    if (x < 0.0) {
        fpenv_raise_invalid();
        uint64_t bits = 0xfff8000000000000ull;
        double r;
        memcpy(&r, &bits, sizeof(r));
        return r;
    }
    return sqrt(x);
}

void avx_fp_binop(enum avx_fp_op op, bool is_double, unsigned vlen,
        const uint8_t *s1, const uint8_t *s2, uint8_t *d) {
    unsigned lb = is_double ? 8 : 4;
    for (unsigned i = 0; i < vlen / 8; i += lb) {
        if (is_double) {
            uint64_t ab, bb, rb;
            memcpy(&ab, s1 + i, 8);
            memcpy(&bb, s2 + i, 8);
            if (avx_fp_nan_result64(op, ab, bb, &rb)) {
                memcpy(d + i, &rb, 8);
                continue;
            }
            double a, b, r;
            memcpy(&a, &ab, 8);
            memcpy(&b, &bb, 8);
            r = avx_fp_apply(op, a, b);
            memcpy(d + i, &r, 8);
        } else {
            uint32_t ab, bb, rb;
            memcpy(&ab, s1 + i, 4);
            memcpy(&bb, s2 + i, 4);
            if (avx_fp_nan_result32(op, ab, bb, &rb)) {
                memcpy(d + i, &rb, 4);
                continue;
            }
            // Computed in FLOAT, not promoted to double and cast back: the
            // round-trip is what quieted signalling NaNs, and it is also the
            // only thing that could perturb a double-rounded result.
            float a, b, r;
            memcpy(&a, &ab, 4);
            memcpy(&b, &bb, 4);
            switch (op) {
            case AVX_FADD: r = a + b; break;
            case AVX_FSUB: r = a - b; break;
            case AVX_FMUL: r = a * b; break;
            case AVX_FDIV: r = a / b; break;
            case AVX_FMIN: r = a < b ? a : b; break;
            case AVX_FMAX: r = a > b ? a : b; break;
            default: r = 0; break;
            }
            memcpy(d + i, &r, 4);
        }
    }
}

// VCMPPS/VCMPPD imm8 predicates. Only the low 5 bits select; this implements
// the original SSE set (0-7), which is what compilers emit outside AVX-512
// masked code. "Q"/"S" (quiet vs signalling) differ only in whether a QNaN
// raises the invalid flag -- the emulator does not model FP exception flags,
// so they behave identically here.
bool avx_fp_compare(unsigned pred, double a, double b) {
    bool unordered = a != a || b != b;
    switch (pred & 7) {
    case 0: return a == b;                  // EQ_OQ
    case 1: return !unordered && a < b;     // LT_OS
    case 2: return !unordered && a <= b;    // LE_OS
    case 3: return unordered;               // UNORD_Q
    case 4: return unordered || a != b;     // NEQ_UQ
    case 5: return !(!unordered && a < b);  // NLT_US
    case 6: return !(!unordered && a <= b); // NLE_US
    default: return !unordered;             // ORD_Q
    }
}

void avx_fp_cmp(unsigned pred, bool is_double, unsigned vlen,
        const uint8_t *s1, const uint8_t *s2, uint8_t *d) {
    unsigned lb = is_double ? 8 : 4;
    for (unsigned i = 0; i < vlen / 8; i += lb) {
        double a, b;
        if (is_double) {
            memcpy(&a, s1 + i, 8);
            memcpy(&b, s2 + i, 8);
        } else {
            float fa, fb;
            memcpy(&fa, s1 + i, 4);
            memcpy(&fb, s2 + i, 4);
            a = fa; b = fb;
        }
        // A true compare produces all-ones in that lane, not 1.
        avx_lane_put(d + i, lb, avx_fp_compare(pred, a, b) ? avx_lane_mask(lb) : 0);
    }
}

// FMA. The 132/213/231 suffix names which operands are multiplied and which is
// the addend, all writing back to the first: 213 is dst = src2*dst + src3.
void avx_fma(unsigned form, bool is_double, bool negate_mul, bool subtract,
        unsigned vlen, const uint8_t *dst_in, const uint8_t *s2, const uint8_t *s3, uint8_t *d) {
    unsigned lb = is_double ? 8 : 4;
    for (unsigned i = 0; i < vlen / 8; i += lb) {
        double x, y, z, r;
        if (is_double) {
            memcpy(&x, dst_in + i, 8);
            memcpy(&y, s2 + i, 8);
            memcpy(&z, s3 + i, 8);
        } else {
            float fx, fy, fz;
            memcpy(&fx, dst_in + i, 4);
            memcpy(&fy, s2 + i, 4);
            memcpy(&fz, s3 + i, 4);
            x = fx; y = fy; z = fz;
        }
        double mul_a, mul_b, addend;
        if (form == 132) { mul_a = x; mul_b = z; addend = y; }
        else if (form == 213) { mul_a = y; mul_b = x; addend = z; }
        else { mul_a = y; mul_b = z; addend = x; } // 231
        double product = mul_a * mul_b;
        if (negate_mul)
            product = -product;
        r = subtract ? product - addend : product + addend;
        if (is_double) {
            memcpy(d + i, &r, 8);
        } else {
            float fr = (float) r;
            memcpy(d + i, &fr, 4);
        }
    }
}

// ---- crypto ----

static const uint8_t avx_aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static const uint8_t avx_aes_inv_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d,
};

// GF(2^8) multiply for MixColumns, reducing by the AES polynomial 0x11b.
static inline uint8_t avx_gf_mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1)
            r ^= a;
        bool hi = (a & 0x80) != 0;
        a = (uint8_t) (a << 1);
        if (hi)
            a ^= 0x1b;
        b >>= 1;
    }
    return r;
}

// One 128-bit AES round. x86 orders an encrypt round as ShiftRows, SubBytes,
// MixColumns (omitted for the "last" round), then XOR the round key -- note
// this differs from ARM's AESE, which does AddRoundKey first, so the two are
// not a drop-in substitution for each other.
void avx_aes_round(bool decrypt, bool last, const uint8_t *state,
        const uint8_t *round_key, uint8_t *out) {
    static const uint8_t enc_shift[16] = {0,5,10,15,4,9,14,3,8,13,2,7,12,1,6,11};
    static const uint8_t dec_shift[16] = {0,13,10,7,4,1,14,11,8,5,2,15,12,9,6,3};
    const uint8_t *shift = decrypt ? dec_shift : enc_shift;
    const uint8_t *sbox = decrypt ? avx_aes_inv_sbox : avx_aes_sbox;

    uint8_t tmp[16];
    for (int i = 0; i < 16; i++)
        tmp[i] = sbox[state[shift[i]]];

    if (!last) {
        uint8_t mixed[16];
        for (int col = 0; col < 4; col++) {
            const uint8_t *s = tmp + col * 4;
            uint8_t *o = mixed + col * 4;
            if (decrypt) {
                o[0] = avx_gf_mul(s[0],14) ^ avx_gf_mul(s[1],11) ^ avx_gf_mul(s[2],13) ^ avx_gf_mul(s[3],9);
                o[1] = avx_gf_mul(s[0],9)  ^ avx_gf_mul(s[1],14) ^ avx_gf_mul(s[2],11) ^ avx_gf_mul(s[3],13);
                o[2] = avx_gf_mul(s[0],13) ^ avx_gf_mul(s[1],9)  ^ avx_gf_mul(s[2],14) ^ avx_gf_mul(s[3],11);
                o[3] = avx_gf_mul(s[0],11) ^ avx_gf_mul(s[1],13) ^ avx_gf_mul(s[2],9)  ^ avx_gf_mul(s[3],14);
            } else {
                o[0] = (uint8_t) (avx_gf_mul(s[0],2) ^ avx_gf_mul(s[1],3) ^ s[2] ^ s[3]);
                o[1] = (uint8_t) (s[0] ^ avx_gf_mul(s[1],2) ^ avx_gf_mul(s[2],3) ^ s[3]);
                o[2] = (uint8_t) (s[0] ^ s[1] ^ avx_gf_mul(s[2],2) ^ avx_gf_mul(s[3],3));
                o[3] = (uint8_t) (avx_gf_mul(s[0],3) ^ s[1] ^ s[2] ^ avx_gf_mul(s[3],2));
            }
        }
        memcpy(tmp, mixed, 16);
    }

    for (int i = 0; i < 16; i++)
        out[i] = tmp[i] ^ round_key[i];
}

// VPCLMULQDQ: carryless (XOR-based) 64x64 -> 128 multiply, per 128-bit lane.
// imm8 bit 0 picks src1's qword, bit 4 picks src2's.
void avx_pclmulqdq(unsigned vlen, byte_t imm, const uint8_t *s1, const uint8_t *s2, uint8_t *d) {
    for (unsigned lane = 0; lane < vlen / 8; lane += 16) {
        uint64_t a = avx_lane_get(s1 + lane + ((imm & 0x01) ? 8 : 0), 8);
        uint64_t b = avx_lane_get(s2 + lane + ((imm & 0x10) ? 8 : 0), 8);
        uint64_t lo = 0, hi = 0;
        for (unsigned i = 0; i < 64; i++) {
            if ((b >> i) & 1) {
                lo ^= a << i;
                // Shifting by 64 is undefined in C, so the i==0 case (where
                // nothing spills into the high word) must be special-cased.
                if (i != 0)
                    hi ^= a >> (64 - i);
            }
        }
        avx_lane_put(d + lane, 8, lo);
        avx_lane_put(d + lane + 8, 8, hi);
    }
}

// VGF2P8AFFINEQB: per byte, an 8x8 GF(2) matrix-vector product against the
// qword-wide matrix operand, then XOR the imm8 constant.
void avx_gf2p8affine(unsigned vlen, byte_t imm, const uint8_t *s1, const uint8_t *s2, uint8_t *d) {
    for (unsigned q = 0; q < vlen / 8; q += 8) {
        for (unsigned j = 0; j < 8; j++) {
            uint8_t x = s1[q + j];
            uint8_t r = 0;
            for (unsigned bit = 0; bit < 8; bit++) {
                // Row `bit` of the matrix is byte 7-bit of the qword; the
                // result bit is the parity of (row & x).
                uint8_t row = s2[q + 7 - bit];
                uint8_t v = row & x;
                unsigned parity = 0;
                for (unsigned k = 0; k < 8; k++)
                    parity ^= (v >> k) & 1;
                if (parity)
                    r |= (uint8_t) (1u << bit);
            }
            d[q + j] = r ^ imm;
        }
    }
}

// VPDPBUSD / VPDPWSSD: multiply-accumulate dot products into the destination's
// existing dword lanes (the original GH #525 finding -- a vectorized checksum).
void avx_vpdpbusd(unsigned vlen, const uint8_t *acc, const uint8_t *s1,
        const uint8_t *s2, uint8_t *d) {
    for (unsigned i = 0; i < vlen / 8; i += 4) {
        int64_t sum = (int32_t) avx_lane_get(acc + i, 4);
        for (unsigned j = 0; j < 4; j++)
            sum += (int32_t) s1[i + j] * (int32_t) (int8_t) s2[i + j];
        avx_lane_put(d + i, 4, (uint32_t) sum);
    }
}

void avx_vpdpwssd(unsigned vlen, const uint8_t *acc, const uint8_t *s1,
        const uint8_t *s2, uint8_t *d) {
    for (unsigned i = 0; i < vlen / 8; i += 4) {
        int64_t sum = (int32_t) avx_lane_get(acc + i, 4);
        for (unsigned j = 0; j < 2; j++) {
            int32_t a = (int16_t) avx_lane_get(s1 + i + j * 2, 2);
            int32_t b = (int16_t) avx_lane_get(s2 + i + j * 2, 2);
            sum += a * b;
        }
        avx_lane_put(d + i, 4, (uint32_t) sum);
    }
}

// VPTERNLOG{D,Q}: imm8 is a full 3-input truth table -- bit (a<<2|b<<1|c) of
// the immediate gives the output for that combination of source bits. Bitwise
// across the whole register, so the lane width is irrelevant to the result.
void avx_pternlog(unsigned vlen, byte_t imm, const uint8_t *s1,
        const uint8_t *s2, const uint8_t *s3, uint8_t *d) {
    for (unsigned i = 0; i < vlen / 8; i++) {
        uint8_t r = 0;
        for (unsigned bit = 0; bit < 8; bit++) {
            unsigned a = (s1[i] >> bit) & 1;
            unsigned b = (s2[i] >> bit) & 1;
            unsigned c = (s3[i] >> bit) & 1;
            if ((imm >> ((a << 2) | (b << 1) | c)) & 1)
                r |= (uint8_t) (1u << bit);
        }
        d[i] = r;
    }
}


// ---- register-file access ----

void avx_reg_read(struct cpu_state *cpu, unsigned idx, unsigned vlen, uint8_t *out) {
    memcpy(out, avx_xmm(cpu, idx), 16);
    if (vlen >= 256)
        memcpy(out + 16, &cpu->ymm_hi[idx], 16);
    if (vlen >= 512)
        memcpy(out + 32, cpu->zmm_hi[idx].u8, 32);
}

void avx_reg_write(struct cpu_state *cpu, unsigned idx, unsigned vlen, const uint8_t *in) {
    memcpy(avx_xmm(cpu, idx), in, 16);
    if (vlen >= 256)
        memcpy(&cpu->ymm_hi[idx], in + 16, 16);
    else
        memset(&cpu->ymm_hi[idx], 0, 16);
    if (vlen >= 512)
        memcpy(cpu->zmm_hi[idx].u8, in + 32, 32);
    else
        memset(cpu->zmm_hi[idx].u8, 0, 32);
}

// ---- i386 front-end ----

bool avx32_rm_is_dst(unsigned map, unsigned op, unsigned pp) {
    if (map == 1)
        return op == 0x11 || op == 0x29 || op == 0x7f || op == 0xd6
            || (op == 0x7e && pp == 1);
    if (map == 3)
        return op == 0x39 || op == 0x14 || op == 0x15 || op == 0x16;
    return false;
}

bool avx32_has_modrm(unsigned map, unsigned op, unsigned pp) {
    // VZEROUPPER (L=0) / VZEROALL (L=1) are the only operand-less forms here.
    if (map == 1 && op == 0x77 && pp == 0)
        return false;
    return true;
}

bool avx32_has_imm8(unsigned map, unsigned op, unsigned pp) {
    if (map == 3)
        return true; // every 0F3A form this front-end implements takes one
    if (map == 1)
        return op == 0x70 || op == 0x71 || op == 0x72 || op == 0x73 || op == 0xc2 || op == 0xc6;
    (void) pp;
    return false;
}

unsigned avx32_mem_bits(unsigned map, unsigned op, unsigned pp, unsigned w, unsigned vlen) {
    if (map == 1) {
        switch (op) {
        // Full-width moves and the elementwise/lane ops: the memory operand
        // is the operation width.
        case 0x10: case 0x11:
            return pp >= 2 ? (pp == 3 ? 64 : 32) : vlen;  // MOVSS/MOVSD are scalar
        case 0x28: case 0x29: case 0x6f: case 0x7f:
        case 0x54: case 0x55: case 0x56: case 0x57:
        case 0x14: case 0x15: case 0xc6:
        case 0xef: case 0xeb: case 0xdb: case 0xdf:
        case 0xfc: case 0xfd: case 0xfe: case 0xd4:
        case 0xf8: case 0xf9: case 0xfa: case 0xfb:
        case 0xec: case 0xed: case 0xdc: case 0xdd:
        case 0xe8: case 0xe9: case 0xd8: case 0xd9:
        case 0x74: case 0x75: case 0x76:
        case 0x64: case 0x65: case 0x66:
        case 0xda: case 0xde: case 0xea: case 0xee:
        case 0xe0: case 0xe3: case 0xd5: case 0xe5: case 0xe4:
        case 0x60: case 0x61: case 0x62: case 0x6c:
        case 0x68: case 0x69: case 0x6a: case 0x6d:
        case 0x63: case 0x67: case 0x6b:
        case 0x70: case 0xf4: case 0xf5: case 0xf6:
            return vlen;
        // Packed/scalar FP: scalar forms touch one element only.
        case 0x58: case 0x59: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
        case 0x51: case 0xc2:
            return pp >= 2 ? (pp == 3 ? 64 : 32) : vlen;
        // Shift-by-xmm always reads a 128-bit count operand.
        case 0xd1: case 0xd2: case 0xd3:
        case 0xe1: case 0xe2:
        case 0xf1: case 0xf2: case 0xf3:
            return 128;
        case 0x6e: case 0x7e:
            return w ? 64 : 32;
        case 0xd6:
            return 64;
        // Shift-by-imm8 and VZEROUPPER have no memory operand at all.
        case 0x71: case 0x72: case 0x73: case 0x77: case 0xd7: case 0x50:
            return 0;
        default:
            return 0;
        }
    }
    if (map == 2) {
        switch (op) {
        case 0x78: return 8;    // vpbroadcastb
        case 0x79: return 16;   // vpbroadcastw
        case 0x58: case 0x18: return 32;   // vpbroadcastd / vbroadcastss
        case 0x59: case 0x19: return 64;   // vpbroadcastq / vbroadcastsd
        case 0x5a: return 128;  // vbroadcasti128
        // Sign/zero widening reads a fraction of the destination width.
        case 0x20: case 0x30: return vlen / 2;   // bw
        case 0x21: case 0x31: return vlen / 4;   // bd
        case 0x22: case 0x32: return vlen / 8;   // bq
        case 0x23: case 0x33: return vlen / 2;   // wd
        case 0x24: case 0x34: return vlen / 4;   // wq
        case 0x25: case 0x35: return vlen / 2;   // dq
        case 0x1c: case 0x1d: case 0x1e:
        case 0x29: case 0x37:
        case 0x38: case 0x39: case 0x3a: case 0x3b:
        case 0x3c: case 0x3d: case 0x3e: case 0x3f:
        case 0x40: case 0x00: case 0x04: case 0x36:
        case 0x45: case 0x46: case 0x47:
        case 0xdc: case 0xdd: case 0xde: case 0xdf:
        case 0x50: case 0x52:
            return vlen;
        default:
            return 0;
        }
    }
    if (map == 3) {
        switch (op) {
        // 0x01 is VPERMPD, sized like its VPERMQ twin at 0x00.
        case 0x0f: case 0x00: case 0x01: case 0x46: case 0x02: case 0x0e:
        case 0x4c: case 0x44: case 0xce: case 0x25:
            return vlen;
        case 0x38: case 0x39: return 128;  // vinserti128 / vextracti128
        case 0x14: return 8;
        case 0x15: return 16;
        case 0x16: case 0x22: return w ? 64 : 32;
        case 0x20: return 8;
        default:
            return 0;
        }
    }
    return 0;
}

// Runtime execution. Operands are gathered into flat buffers, handed to the
// shared avx_* semantics, and written back. Register operands come from
// cpu->xmm/ymm_hi via the indices packed into `desc`; the single memory
// operand (if any) arrives as the gadget's already-translated host pointer,
// so page faults and crosspage staging are resolved before this is entered.
void vec_avx32(struct cpu_state *cpu, const void *src, void *dst, uint32_t desc) {
    unsigned map = desc & 3;
    unsigned pp = (desc >> 2) & 3;
    unsigned op = (desc >> 4) & 0xff;
    unsigned vlen = ((desc >> 12) & 1) ? 256 : 128;
    unsigned w = (desc >> 13) & 1;
    unsigned vvvv = (desc >> 14) & 7;
    unsigned reg = (desc >> 17) & 7;
    bool rm_mem = (desc >> 20) & 1;
    unsigned rm = (desc >> 21) & 7;
    uint8_t imm = (desc >> 24) & 0xff;

    bool rm_dst = avx32_rm_is_dst(map, op, pp);
    unsigned mem_bits = avx32_mem_bits(map, op, pp, w, vlen);
    unsigned mem_bytes = mem_bits / 8;
    // For a store the memory pointer arrives as `dst`; otherwise as `src`.
    const void *rm_ptr = rm_dst ? (const void *) dst : src;

    uint8_t a[64] = {0}, b[64] = {0}, out[64] = {0};

    // Gather the r/m operand unless it is the destination.
    if (!rm_dst) {
        if (rm_mem)
            memcpy(b, rm_ptr, mem_bytes);
        else
            avx_reg_read(cpu, rm, vlen, b);
    }

    if (map == 1) {
        // VZEROUPPER (L=0) / VZEROALL (L=1): no operands. 32-bit mode has
        // only xmm0-7 to clear.
        if (op == 0x77 && pp == 0) {
            for (unsigned i = 0; i < 8; i++) {
                memset(&cpu->ymm_hi[i], 0, sizeof(cpu->ymm_hi[i]));
                memset(cpu->zmm_hi[i].u8, 0, sizeof(cpu->zmm_hi[i].u8));
                if (vlen == 256)
                    memset(&cpu->xmm[i], 0, sizeof(cpu->xmm[i]));
            }
            return;
        }

        // Scalar MOVSS/MOVSD: register-to-register merges the upper lanes
        // from vvvv; a load from memory zeroes them.
        if ((op == 0x10 || op == 0x11) && pp >= 2) {
            if (op == 0x10) {
                if (rm_mem) {
                    memcpy(out, rm_ptr, mem_bytes);
                } else {
                    avx_reg_read(cpu, vvvv, 128, out);
                    memcpy(out, b, mem_bytes);
                }
                avx_reg_write(cpu, reg, 128, out);
            } else {
                avx_reg_read(cpu, reg, 128, a);
                if (rm_mem) {
                    memcpy(dst, a, mem_bytes);
                } else {
                    avx_reg_read(cpu, vvvv, 128, out);
                    memcpy(out, a, mem_bytes);
                    avx_reg_write(cpu, rm, 128, out);
                }
            }
            return;
        }

        // Full-width moves.
        if (op == 0x10 || op == 0x28 || op == 0x6f) {
            avx_reg_write(cpu, reg, vlen, b);
            return;
        }
        if (op == 0x11 || op == 0x29 || op == 0x7f) {
            avx_reg_read(cpu, reg, vlen, out);
            if (rm_mem)
                memcpy(dst, out, mem_bytes);
            else
                avx_reg_write(cpu, rm, vlen, out);
            return;
        }

        // MOVD/MOVQ. 32-bit mode has no 64-bit GPRs, so the register forms
        // are always 32-bit; the 64-bit memory form (0F D6) still exists.
        if (op == 0x6e) {
            uint32_t v = rm_mem ? (uint32_t) avx_lane_get(rm_ptr, 4) : cpu->regs[rm];
            memcpy(out, &v, 4);
            avx_reg_write(cpu, reg, 128, out);
            return;
        }
        if (op == 0x7e && pp == 1) {
            avx_reg_read(cpu, reg, 128, a);
            if (rm_mem)
                memcpy(dst, a, 4);
            else
                cpu->regs[rm] = (uint32_t) avx_lane_get(a, 4);
            return;
        }
        if (op == 0x7e && pp == 2) { // F3 0F 7E: load low 64 bits, zero the rest
            memcpy(out, b, 8);
            avx_reg_write(cpu, reg, 128, out);
            return;
        }
        if (op == 0xd6) { // store low 64 bits
            avx_reg_read(cpu, reg, 128, a);
            if (rm_mem)
                memcpy(dst, a, 8);
            else {
                memcpy(out, a, 8);
                avx_reg_write(cpu, rm, 128, out);
            }
            return;
        }

        // Mask extraction into a GPR.
        if (op == 0xd7) { // vpmovmskb
            uint32_t mask = 0;
            for (unsigned i = 0; i < vlen / 8; i++)
                if (b[i] & 0x80)
                    mask |= 1u << i;
            cpu->regs[reg] = mask;
            return;
        }
        if (op == 0x50) { // vmovmskps/pd
            unsigned lb = pp == 1 ? 8 : 4;
            uint32_t mask = 0;
            for (unsigned i = 0, bit = 0; i < vlen / 8; i += lb, bit++)
                if (b[i + lb - 1] & 0x80)
                    mask |= 1u << bit;
            cpu->regs[reg] = mask;
            return;
        }

        // Shift by imm8. NDD form: destination is vvvv, source is r/m.
        if (op == 0x71 || op == 0x72 || op == 0x73) {
            unsigned lb = op == 0x71 ? 2 : op == 0x72 ? 4 : 8;
            unsigned sub = reg & 7; // the modrm.reg field is the sub-opcode here
            if (op == 0x73 && (sub == 3 || sub == 7))
                avx_byte_shift(sub == 7, vlen, imm > 16 ? 16 : imm, b, out);
            else if (sub == 2)
                avx_shift(AVX_SHR, lb, vlen, b, imm, out);
            else if (sub == 4 && lb != 8)
                avx_shift(AVX_SAR, lb, vlen, b, imm, out);
            else if (sub == 6)
                avx_shift(AVX_SHL, lb, vlen, b, imm, out);
            else
                return;
            avx_reg_write(cpu, vvvv, vlen, out);
            return;
        }

        avx_reg_read(cpu, vvvv, vlen, a);

        // Shift by a 128-bit count operand.
        if (op == 0xd1 || op == 0xd2 || op == 0xd3 ||
            op == 0xe1 || op == 0xe2 ||
            op == 0xf1 || op == 0xf2 || op == 0xf3) {
            unsigned lb = (op == 0xd1 || op == 0xe1 || op == 0xf1) ? 2
                        : (op == 0xd2 || op == 0xe2 || op == 0xf2) ? 4 : 8;
            enum avx_shift_kind kind = op >= 0xf1 ? AVX_SHL : op >= 0xe1 ? AVX_SAR : AVX_SHR;
            avx_shift(kind, lb, vlen, a, avx_lane_get(b, 8), out);
            avx_reg_write(cpu, reg, vlen, out);
            return;
        }

        if (op == 0x70) { // pshufd / pshufhw / pshuflw
            if (pp == 1)
                avx_pshufd(vlen, imm, b, out);
            else
                avx_pshufw_half(vlen, imm, pp == 2, b, out);
            avx_reg_write(cpu, reg, vlen, out);
            return;
        }

        // Packed / scalar floating point.
        {
            enum avx_fp_op fop;
            bool matched = true;
            switch (op) {
            case 0x58: fop = AVX_FADD; break;
            case 0x59: fop = AVX_FMUL; break;
            case 0x5c: fop = AVX_FSUB; break;
            case 0x5d: fop = AVX_FMIN; break;
            case 0x5e: fop = AVX_FDIV; break;
            case 0x5f: fop = AVX_FMAX; break;
            default: matched = false; break;
            }
            if (matched) {
                bool scalar = pp >= 2, is_double = pp == 1 || pp == 3;
                unsigned width = scalar ? 128 : vlen;
                avx_reg_read(cpu, vvvv, width, out);
                avx_fp_binop(fop, is_double, scalar ? (is_double ? 64 : 32) : vlen, a, b, out);
                avx_reg_write(cpu, reg, width, out);
                return;
            }
        }
        if (op == 0x51) { // vsqrt
            bool scalar = pp >= 2, is_double = pp == 1 || pp == 3;
            unsigned width = scalar ? 128 : vlen;
            unsigned lb = is_double ? 8 : 4;
            unsigned span = scalar ? lb : width / 8;
            if (scalar)
                avx_reg_read(cpu, vvvv, 128, out);
            for (unsigned i = 0; i < span; i += lb) {
                if (is_double) {
                    double v;
                    memcpy(&v, b + i, 8);
                    v = avx_sqrt_f64(v);
                    memcpy(out + i, &v, 8);
                } else {
                    float v;
                    memcpy(&v, b + i, 4);
                    v = avx_sqrt_f32(v);
                    memcpy(out + i, &v, 4);
                }
            }
            avx_reg_write(cpu, reg, width, out);
            return;
        }
        if (op == 0xc2) { // vcmp
            bool scalar = pp >= 2, is_double = pp == 1 || pp == 3;
            unsigned width = scalar ? 128 : vlen;
            avx_reg_read(cpu, vvvv, width, out);
            avx_fp_cmp(imm, is_double, scalar ? (is_double ? 64 : 32) : width, a, b, out);
            avx_reg_write(cpu, reg, width, out);
            return;
        }
        if (op == 0xc6) { // vshufps / vshufpd
            for (unsigned lane = 0; lane < vlen / 8; lane += 16) {
                uint8_t tmp[16];
                if (pp == 1) {
                    memcpy(tmp, a + lane + (((imm >> (lane / 8)) & 1) ? 8 : 0), 8);
                    memcpy(tmp + 8, b + lane + (((imm >> (lane / 8 + 1)) & 1) ? 8 : 0), 8);
                } else {
                    for (unsigned j = 0; j < 4; j++) {
                        const uint8_t *s = j < 2 ? a : b;
                        memcpy(tmp + j * 4, s + lane + ((imm >> (2 * j)) & 3) * 4, 4);
                    }
                }
                memcpy(out + lane, tmp, 16);
            }
            avx_reg_write(cpu, reg, vlen, out);
            return;
        }

        // Unpacks (FP and integer share the shape, only the lane width differs).
        if (op == 0x14 || op == 0x15) {
            avx_unpack(op == 0x15, pp == 1 ? 8 : 4, vlen, a, b, out);
            avx_reg_write(cpu, reg, vlen, out);
            return;
        }
        if (op == 0x60 || op == 0x61 || op == 0x62 || op == 0x6c ||
            op == 0x68 || op == 0x69 || op == 0x6a || op == 0x6d) {
            // 0x6c is punpckLqdq and 0x6d is punpckHqdq, so a bare
            // `op >= 0x68` calls the LOW form high. The opcode space is not
            // ordered the way the test assumes: 60-62 are the low bw/wd/dq,
            // 68-6a the high ones, and then the qdq pair follows as 6c/6d.
            // Measured against real x86_64 hardware: vpunpcklqdq returned the
            // HIGH qword of each 128-bit lane in both widths. 0x6d happened to
            // be right by luck.
            bool high = (op >= 0x68 && op <= 0x6a) || op == 0x6d;
            unsigned lb = (op == 0x60 || op == 0x68) ? 1
                        : (op == 0x61 || op == 0x69) ? 2
                        : (op == 0x62 || op == 0x6a) ? 4 : 8;
            avx_unpack(high, lb, vlen, a, b, out);
            avx_reg_write(cpu, reg, vlen, out);
            return;
        }
        if (op == 0x63 || op == 0x67 || op == 0x6b) {
            avx_pack(op != 0x67, op == 0x6b ? 4 : 2, vlen, a, b, out);
            avx_reg_write(cpu, reg, vlen, out);
            return;
        }
        if (op == 0xf4) { avx_pmuludq(vlen, a, b, out); avx_reg_write(cpu, reg, vlen, out); return; }
        if (op == 0xf5) { avx_pmaddwd(vlen, a, b, out); avx_reg_write(cpu, reg, vlen, out); return; }
        if (op == 0xf6) { avx_psadbw(vlen, a, b, out); avx_reg_write(cpu, reg, vlen, out); return; }

        // Elementwise integer / logical.
        {
            enum avx_op kind;
            unsigned lb = 1;
            bool matched = true;
            switch (op) {
            case 0x57: kind = AVX_XOR; break;
            case 0x54: kind = AVX_AND; break;
            case 0x55: kind = AVX_ANDN; break;
            case 0x56: kind = AVX_OR; break;
            case 0xef: kind = AVX_XOR; break;
            case 0xeb: kind = AVX_OR; break;
            case 0xdb: kind = AVX_AND; break;
            case 0xdf: kind = AVX_ANDN; break;
            case 0xfc: kind = AVX_ADD; lb = 1; break;
            case 0xfd: kind = AVX_ADD; lb = 2; break;
            case 0xfe: kind = AVX_ADD; lb = 4; break;
            case 0xd4: kind = AVX_ADD; lb = 8; break;
            case 0xf8: kind = AVX_SUB; lb = 1; break;
            case 0xf9: kind = AVX_SUB; lb = 2; break;
            case 0xfa: kind = AVX_SUB; lb = 4; break;
            case 0xfb: kind = AVX_SUB; lb = 8; break;
            case 0xec: kind = AVX_ADDS; lb = 1; break;
            case 0xed: kind = AVX_ADDS; lb = 2; break;
            case 0xdc: kind = AVX_ADDUS; lb = 1; break;
            case 0xdd: kind = AVX_ADDUS; lb = 2; break;
            case 0xe8: kind = AVX_SUBS; lb = 1; break;
            case 0xe9: kind = AVX_SUBS; lb = 2; break;
            case 0xd8: kind = AVX_SUBUS; lb = 1; break;
            case 0xd9: kind = AVX_SUBUS; lb = 2; break;
            case 0x74: kind = AVX_CMPEQ; lb = 1; break;
            case 0x75: kind = AVX_CMPEQ; lb = 2; break;
            case 0x76: kind = AVX_CMPEQ; lb = 4; break;
            case 0x64: kind = AVX_CMPGT; lb = 1; break;
            case 0x65: kind = AVX_CMPGT; lb = 2; break;
            case 0x66: kind = AVX_CMPGT; lb = 4; break;
            case 0xda: kind = AVX_MINU; lb = 1; break;
            case 0xde: kind = AVX_MAXU; lb = 1; break;
            case 0xea: kind = AVX_MINS; lb = 2; break;
            case 0xee: kind = AVX_MAXS; lb = 2; break;
            case 0xe0: kind = AVX_AVG; lb = 1; break;
            case 0xe3: kind = AVX_AVG; lb = 2; break;
            case 0xd5: kind = AVX_MULLO; lb = 2; break;
            case 0xe5: kind = AVX_MULHI; lb = 2; break;
            case 0xe4: kind = AVX_MULHIU; lb = 2; break;
            default: matched = false; break;
            }
            if (matched) {
                avx_binop(kind, lb, vlen, a, b, out);
                avx_reg_write(cpu, reg, vlen, out);
            }
            return;
        }
    }

    if (map == 2) {
        // Two-operand forms (no vvvv).
        if (op == 0x78 || op == 0x79 || op == 0x58 || op == 0x59 ||
            op == 0x18 || op == 0x19) {
            unsigned lb = (op == 0x78) ? 1 : (op == 0x79) ? 2
                        : (op == 0x58 || op == 0x18) ? 4 : 8;
            avx_broadcast(lb, vlen, b, out);
            avx_reg_write(cpu, reg, vlen, out);
            return;
        }
        if (op == 0x5a) { // vbroadcasti128
            for (unsigned i = 0; i < vlen / 8; i += 16)
                memcpy(out + i, b, 16);
            avx_reg_write(cpu, reg, vlen, out);
            return;
        }
        if ((op >= 0x20 && op <= 0x25) || (op >= 0x30 && op <= 0x35)) {
            bool sign = op < 0x30;
            unsigned kind = op & 0xf;
            unsigned src_lb = kind <= 2 ? 1 : kind <= 4 ? 2 : 4;
            unsigned dst_lb = kind == 0 ? 2 : kind == 1 ? 4 : kind == 2 ? 8
                            : kind == 3 ? 4 : 8;
            avx_widen(sign, src_lb, dst_lb, vlen, b, out);
            avx_reg_write(cpu, reg, vlen, out);
            return;
        }
        if (op >= 0x1c && op <= 0x1e) {
            avx_abs(op == 0x1c ? 1 : op == 0x1d ? 2 : 4, vlen, b, out);
            avx_reg_write(cpu, reg, vlen, out);
            return;
        }

        avx_reg_read(cpu, vvvv, vlen, a);

        if (op == 0x00) { avx_pshufb(vlen, a, b, out); avx_reg_write(cpu, reg, vlen, out); return; }
        if (op == 0x04) { avx_pmaddubsw(vlen, a, b, out); avx_reg_write(cpu, reg, vlen, out); return; }
        if (op == 0x36) { // vpermd -- crosses lanes by design
            unsigned n = (vlen / 8) / 4;
            for (unsigned i = 0; i < n; i++) {
                uint32_t idx = (uint32_t) avx_lane_get(a + i * 4, 4) & (n - 1);
                avx_lane_put(out + i * 4, 4, avx_lane_get(b + idx * 4, 4));
            }
            avx_reg_write(cpu, reg, vlen, out);
            return;
        }
        if (op == 0x45 || op == 0x46 || op == 0x47) {
            unsigned lb = w ? 8 : 4;
            if (op == 0x46 && w)
                return; // VPSRAVQ is AVX-512 only
            avx_shift_var(op == 0x45 ? AVX_SHR : op == 0x46 ? AVX_SAR : AVX_SHL,
                          lb, vlen, a, b, out);
            avx_reg_write(cpu, reg, vlen, out);
            return;
        }
        if (op >= 0xdc && op <= 0xdf) { // vaesenc / enclast / dec / declast
            for (unsigned lane = 0; lane < vlen / 8; lane += 16)
                avx_aes_round(op >= 0xde, op == 0xdd || op == 0xdf,
                              a + lane, b + lane, out + lane);
            avx_reg_write(cpu, reg, vlen, out);
            return;
        }
        if (op == 0x50 || op == 0x52) { // vpdpbusd / vpdpwssd accumulate into dst
            uint8_t acc[64];
            avx_reg_read(cpu, reg, vlen, acc);
            if (op == 0x50)
                avx_vpdpbusd(vlen, acc, a, b, out);
            else
                avx_vpdpwssd(vlen, acc, a, b, out);
            avx_reg_write(cpu, reg, vlen, out);
            return;
        }
        {
            enum avx_op kind;
            unsigned lb = 1;
            bool matched = true;
            switch (op) {
            case 0x29: kind = AVX_CMPEQ; lb = 8; break;
            case 0x37: kind = AVX_CMPGT; lb = 8; break;
            case 0x38: kind = AVX_MINS; lb = 1; break;
            case 0x39: kind = AVX_MINS; lb = 4; break;
            case 0x3a: kind = AVX_MINU; lb = 2; break;
            case 0x3b: kind = AVX_MINU; lb = 4; break;
            case 0x3c: kind = AVX_MAXS; lb = 1; break;
            case 0x3d: kind = AVX_MAXS; lb = 4; break;
            case 0x3e: kind = AVX_MAXU; lb = 2; break;
            case 0x3f: kind = AVX_MAXU; lb = 4; break;
            case 0x40: kind = AVX_MULLO; lb = 4; break;
            default: matched = false; break;
            }
            if (matched) {
                avx_binop(kind, lb, vlen, a, b, out);
                avx_reg_write(cpu, reg, vlen, out);
            }
            return;
        }
    }

    if (map == 3) {
        // 0x01 is VPERMPD: identical qword gather to VPERMQ, different operand
        // type in the manual and no difference at all in the bits moved.
        if (op == 0x00 || op == 0x01) { // vpermq / vpermpd -- cross lanes
            for (unsigned i = 0; i < 4; i++)
                avx_lane_put(out + i * 8, 8, avx_lane_get(b + ((imm >> (2 * i)) & 3) * 8, 8));
            avx_reg_write(cpu, reg, 256, out);
            return;
        }
        if (op == 0x39) { // vextracti128 -- r/m is the destination
            avx_reg_read(cpu, reg, 256, a);
            memcpy(out, a + ((imm & 1) ? 16 : 0), 16);
            if (rm_mem)
                memcpy(dst, out, 16);
            else
                avx_reg_write(cpu, rm, 128, out);
            return;
        }
        if (op == 0x14 || op == 0x15 || op == 0x16) { // vpextrb/w/d
            unsigned lb = op == 0x14 ? 1 : op == 0x15 ? 2 : 4;
            avx_reg_read(cpu, reg, 128, a);
            uint64_t v = avx_lane_get(a + (imm & (16 / lb - 1)) * lb, lb);
            if (rm_mem)
                memcpy(dst, &v, lb);
            else
                cpu->regs[rm] = (uint32_t) v;
            return;
        }
        if (op == 0x20 || op == 0x22) { // vpinsrb / vpinsrd
            unsigned lb = op == 0x20 ? 1 : 4;
            uint64_t v = rm_mem ? avx_lane_get(rm_ptr, lb) : cpu->regs[rm];
            avx_reg_read(cpu, vvvv, 128, out);
            avx_lane_put(out + (imm & (16 / lb - 1)) * lb, lb, v);
            avx_reg_write(cpu, reg, 128, out);
            return;
        }

        avx_reg_read(cpu, vvvv, vlen, a);

        if (op == 0x0f) { avx_palignr(vlen, imm, a, b, out); avx_reg_write(cpu, reg, vlen, out); return; }
        if (op == 0x44) { avx_pclmulqdq(vlen, imm, a, b, out); avx_reg_write(cpu, reg, vlen, out); return; }
        if (op == 0xce) { avx_gf2p8affine(vlen, imm, a, b, out); avx_reg_write(cpu, reg, vlen, out); return; }
        if (op == 0x25) { // vpternlog
            uint8_t dst_in[64];
            avx_reg_read(cpu, reg, vlen, dst_in);
            avx_pternlog(vlen, imm, dst_in, a, b, out);
            avx_reg_write(cpu, reg, vlen, out);
            return;
        }
        if (op == 0x38) { // vinserti128
            avx_reg_read(cpu, vvvv, 256, out);
            memcpy(out + ((imm & 1) ? 16 : 0), b, 16);
            avx_reg_write(cpu, reg, 256, out);
            return;
        }
        if (op == 0x46) { // vperm2i128
            for (unsigned half = 0; half < 2; half++) {
                unsigned sel = (imm >> (half * 4)) & 0xf;
                if (sel & 0x8) {
                    memset(out + half * 16, 0, 16);
                } else {
                    const uint8_t *s = (sel & 0x2) ? b : a;
                    memcpy(out + half * 16, s + ((sel & 1) ? 16 : 0), 16);
                }
            }
            avx_reg_write(cpu, reg, 256, out);
            return;
        }
        if (op == 0x02 || op == 0x0e) { // vpblendd / vpblendw
            unsigned lb = op == 0x02 ? 4 : 2;
            unsigned n = (vlen / 8) / lb;
            for (unsigned i = 0; i < n; i++) {
                unsigned bit = op == 0x02 ? i : (i & 7);
                const uint8_t *s = (imm >> bit) & 1 ? b : a;
                memcpy(out + i * lb, s + i * lb, lb);
            }
            avx_reg_write(cpu, reg, vlen, out);
            return;
        }
        if (op == 0x4c) { // vpblendvb -- mask register in the is4 immediate
            uint8_t mask[64];
            avx_reg_read(cpu, (imm >> 4) & 7, vlen, mask);
            for (unsigned i = 0; i < vlen / 8; i++)
                out[i] = (mask[i] & 0x80) ? b[i] : a[i];
            avx_reg_write(cpu, reg, vlen, out);
            return;
        }
    }
}
