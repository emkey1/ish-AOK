#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "float80.h"
#include "misc.h"

typedef unsigned __int128 uint128_t;

// If you don't understand why something is the way it is, change it and run
// the test suite and all will become clear.

// exponent is stored with a constant added to it, because that's apparently
// easier than just saying the exponent is two's complement
#define BIAS80 0x3fff
#define EXP_MAX 0x7ffe
#define EXP_MIN 0x0001
#define EXP_SPECIAL 0x7fff
#define EXP_DENORMAL 0
static unsigned bias(int exp) {
    return exp + BIAS80;
}
static int unbias(unsigned exp) {
    return exp - BIAS80;
}
// returns the correct answer for denormal numbers
static int unbias_denormal(unsigned exp) {
    if (exp == EXP_DENORMAL)
        return unbias(EXP_MIN);
    return unbias(exp);
}

#define CURSED_BIT (1ul << 63)

__thread enum f80_rounding_mode f80_rounding_mode;
__thread int f80_precision = 64;
__thread int f80_inexact;
__thread int f80_rounded_up;
__thread int f80_exceptions;

static bool round_away_from_zero(int sign) {
    return (f80_rounding_mode == round_up && !sign) ||
        (f80_rounding_mode == round_down && sign);
}

// shift a 128 bit integer right but using the floating point rounding mode
// used by f80_shift_right and to round the 128-bit result of multiplying significands
// sign is necessary to decide which way to round when mode is round_up or round_down
static uint128_t u128_shift_right_round(uint128_t i, int shift, int sign) {
    // we're going to be shifting stuff by shift - 1, so stay safe
    if (shift == 0)
        return i;
    if (shift > 127) {
        if (i != 0)
            f80_inexact = 1;
        // If we should be rounding away from zero, and there are any nonzero
        // bits involved, an infinite amount of right shift should give 1
        if (round_away_from_zero(sign) && i != 0) {
            f80_rounded_up = 1;
            return 1;
        }
        return 0;
    }

    // stuff necessary for rounding to nearest or even. reference: https://stackoverflow.com/a/8984135
    // grab the guard bit, the last bit shifted out
    int guard = (i >> (shift - 1)) & 1;
    // now grab the rest of the bits being shifted out -- all of them. This
    // was a uint64_t, which dropped every sticky bit above bit 63: a rounding
    // of a 128-bit significand to 53 or 24 bits (precision control) then saw
    // an exact halfway, or nothing at all, and got both the result and PE
    // wrong.
    uint128_t rest = i & ~((uint128_t) -1 << (shift - 1));

    i >>= shift;
    // if all the bits shifted out were zeroes, we're done
    if (guard == 0 && rest == 0)
        return i;

    // Bits were discarded, so the result is inexact regardless of which way it
    // ends up going.
    f80_inexact = 1;
    if (round_away_from_zero(sign)) {
        i++;
        f80_rounded_up = 1;
    } else if (f80_rounding_mode == round_to_nearest && guard) {
        if (rest != 0) {
            i++; // round up
            f80_rounded_up = 1;
        } else if (i & 1) {
            i++; // round to nearest even
            f80_rounded_up = 1;
        }
    }
    return i;
}

// Shift right keeping a sticky bit: anything shifted out leaves the lowest bit
// set, so a later rounding still knows the value was not exact and which side
// of it the exact value lies. Rounding here instead, as the adder used to,
// rounded twice and lost the direction (C1) when an operand shifted out whole.
static uint128_t u128_shift_right_sticky(uint128_t i, int shift) {
    if (shift <= 0)
        return i;
    if (shift > 127)
        return i != 0;
    return (i >> shift) | ((i & (((uint128_t) 1 << shift) - 1)) != 0);
}

// may overflow
static float80 f80_shift_left(float80 f, int shift) {
    f.signif <<= shift;
    f.exp -= shift;
    return f;
}

// may lose precision
static float80 f80_shift_right(float80 f, int shift) {
    f.signif = u128_shift_right_round(f.signif, shift, f.sign);
    f.exp += shift;
    return f;
}

// a number is unsupported if the cursed bit (first bit of the significand,
// also known as the integer bit) is incorrect. it must be 0 for denormals and
// 1 for any other type of number.
bool f80_is_supported(float80 f) {
    if (f.exp == EXP_DENORMAL)
        return f.signif >> 63 == 0;
    return f.signif >> 63 == 1;
}

bool f80_isnan(float80 f) {
    return f.exp == EXP_SPECIAL && (f.signif & (-1ul >> 1)) != 0;
}
bool f80_isinf(float80 f) {
    return f.exp == EXP_SPECIAL && (f.signif & (-1ul >> 1)) == 0;
}
bool f80_iszero(float80 f) {
    return f.exp == EXP_DENORMAL && f.signif == 0;
}
bool f80_isdenormal(float80 f) {
    return f.exp == EXP_DENORMAL && f.signif != 0;
}
bool f80_issnan(float80 f) {
    return f80_isnan(f) && !(f.signif & (1ull << 62));
}

static float80 f80_normalize(float80 f) {
    // this function probably can't handle unsupported numbers
    // except cursed normals which are just unnormals, and working with them is the point of this function
    if (f.exp == EXP_DENORMAL || f.exp == EXP_SPECIAL)
        assert(f80_is_supported(f));

    // denormals (and zero) are already normalized (unlike the name suggests)
    if (f.exp == EXP_DENORMAL)
        return f;
    // shift left as many times as possible without overflow
    // number of leading zeroes = how many times we can shift out a leading digit before overflow
    int shift;
    if (f.signif != 0)
        shift = __builtin_clzl(f.signif);
    else
        shift = 64; // __builtin_clzl has undefined result with zero
    if (f.exp - shift < EXP_MIN) {
        // if we shifted this much, exponent would go below its minimum
        // so shift as much as possible and create a denormal
        f = f80_shift_left(f, f.exp - EXP_MIN);
        f.exp = EXP_DENORMAL;
        return f;
    }
    return f80_shift_left(f, shift);
}

static int u128_clz(uint128_t x) {
    // correctly counting leading zeros on a 128-bit int is interesting
    int zeros;
    if (x >> 64 != 0)
        zeros = __builtin_clzl((uint64_t) (x >> 64));
    else if (x != 0)
        zeros = 64 + __builtin_clzl((uint64_t) x);
    else
        zeros = 128;
    return zeros;
}

// The width precision control rounds significands to, as a count of low bits
// that stay zero: 0 for extended, 11 for double, 40 for single.
static int f80_precision_extra(void) {
    int extra = 64 - f80_precision;
    if (extra < 0 || extra > 40)
        extra = 0;
    return extra;
}

// An overflowed result: infinity, or the largest finite value (at the current
// precision) when the rounding mode points back toward zero. Either way OE and
// PE, and C1 says whether that was away from zero.
static float80 f80_overflow(int sign) {
    float80 f;
    if ((f80_rounding_mode == round_up && sign) ||
            (f80_rounding_mode == round_down && !sign) ||
            f80_rounding_mode == round_chop) {
        f = (float80) {.exp = EXP_MAX, .signif = (uint64_t) -1 << f80_precision_extra()};
        f80_rounded_up = 0;
    } else {
        f = F80_INF;
        f80_rounded_up = 1;
    }
    f.sign = sign;
    f80_exceptions |= F80_EXC_OVERFLOW;
    f80_inexact = 1;
    return f;
}

// Round a 128-bit significand to the current precision. The value is
// signif * 2^(exp - 127). Every arithmetic result comes through here, so this
// is where overflow, underflow and inexact are decided.
static float80 u128_normalize_round(uint128_t signif, int exp, int sign) {
    if (signif == 0)
        return (float80) {.sign = sign};

    // Underflow is a tiny result that also lost bits, so this call's own
    // inexactness has to be told apart from what the caller already had.
    int caller_inexact = f80_inexact;
    f80_inexact = 0;
    bool tiny = false;
    int extra = f80_precision_extra();

    int shift = u128_clz(signif);
    // now shift left
    if (exp - shift < unbias(EXP_MIN)) {
        // The x87 decides tininess after rounding with an unbounded exponent:
        // a value just below the smallest normal that rounds up to it at the
        // current precision is not tiny, even though the denormal it becomes
        // has fewer bits and may still round.
        tiny = true;
        if (exp - shift == unbias(EXP_MIN) - 1) {
            int saved_inexact = f80_inexact, saved_up = f80_rounded_up;
            uint128_t r = u128_shift_right_round(signif << shift, 64 + extra, sign);
            if (r >> (64 - extra))
                tiny = false;
            f80_inexact = saved_inexact;
            f80_rounded_up = saved_up;
        }
        if (exp > unbias(EXP_MIN))
            signif <<= exp - unbias(EXP_MIN);
        else
            // sticky, not rounded: the one rounding is below
            signif = u128_shift_right_sticky(signif, unbias(EXP_MIN) - exp);
        exp = unbias(EXP_DENORMAL);
    } else if (exp - shift > unbias(EXP_MAX)) {
        return f80_overflow(sign);
    } else {
        signif <<= shift;
        exp -= shift;
    }
    // and round
    float80 f;
    f.exp = bias(exp);
    // Round straight to the precision-control width rather than to 64 bits and
    // then again to the target -- double rounding would give a different answer
    // in the halfway cases. extra == 0 (the default, PC = extended) reduces
    // this to exactly what it did before.
    // hack around cases where u128_shift_right_round returns 0x10000000000000000
    // such as signif = 0xffffffffffffffff0000000000000000
    signif = u128_shift_right_round(signif, 64 + extra, sign);
    if (signif >> (64 - extra) != 0) {
        signif >>= 1;
        f.exp++;
        // that carry can take the largest exponent past the top
        if (f.exp > EXP_MAX)
            return f80_overflow(sign);
    }
    signif <<= extra;
    // A denormal that rounded up into the integer bit is the smallest normal.
    if (f.exp == EXP_DENORMAL && (signif >> 63))
        f.exp = EXP_MIN;
    f.signif = signif;
    f.sign = sign;
    if (tiny && f80_inexact)
        f80_exceptions |= F80_EXC_UNDERFLOW;
    f80_inexact |= caller_inexact;
    return f;
}

float80 f80_from_int(int64_t i) {
    // stick i in the significand, give it an exponent of 2^63 to offset the
    // implicit binary point after the first bit, and then normalize
    float80 f = {
        .signif = i,
        .exp = bias(63),
        .sign = 0,
    };
    if (i == 0)
        f.exp = 0;
    if (i < 0) {
        f.sign = 1;
        f.signif = -(uint64_t) i;
    }
    return f80_normalize(f);
}

int64_t f80_to_int(float80 f) {
    // A NaN, an infinity, or anything whose magnitude needs an exponent past
    // 2^63 has no 64-bit integer: the integer indefinite, and invalid.
    if (!f80_is_supported(f) || f.exp > bias(63)) {
        f80_exceptions |= F80_EXC_INVALID;
        return INT64_MIN;
    }
    // shift right (reduce precision) until the exponent is 2^63
    f = f80_shift_right(f, bias(63) - f.exp);
    // Rounding can still carry past the range: 2^63 - 0.5 rounds up to 2^63,
    // which only a negative result can hold. An invalid operation reports
    // nothing about rounding, so PE and C1 go again.
    if (f.signif > (uint64_t) INT64_MAX + f.sign) {
        f80_exceptions |= F80_EXC_INVALID;
        f80_inexact = f80_rounded_up = 0;
        return INT64_MIN;
    }
    // and the answer should be the significand!
    return !f.sign ? f.signif : -f.signif;
}

#define EXP64_MAX 0x7fe
#define EXP64_MIN 0x001
#define EXP64_SPECIAL 0x7ff
#define EXP64_DENORMAL 0x000
#define SIGNIF64_MASK ((UINT64_C(1) << 52) - 1)

// unsupported?
float80 f80_from_double(double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    unsigned sign = bits >> 63;
    unsigned exp = (bits >> 52) & 0x7ff;
    uint64_t signif = bits & SIGNIF64_MASK;
    float80 f;

    if (exp == EXP64_SPECIAL)
        f.exp = EXP_SPECIAL;
    else if (exp == EXP64_DENORMAL)
        // denormals actually have an exponent of EXP_MIN, the special exponent
        // is needed to indicate the integer bit is 0
        // zeroes have the same exponent as denormals but need to be handled
        // differently
        f.exp = signif == 0 ? 0 : bias(1 - 0x3ff);
    else
        f.exp = bias((int) exp - 0x3ff);

    f.signif = signif << 11;
    if (exp != EXP64_DENORMAL)
        f.signif |= CURSED_BIT;
    f.sign = sign;
    if (exp == EXP64_DENORMAL && signif != 0)
        f80_exceptions |= F80_EXC_DENORMAL;
    if (f80_issnan(f)) {
        f80_exceptions |= F80_EXC_INVALID;
        f.signif |= 1ull << 62;
    }
    return f80_normalize(f);
}

float80 f80_from_float(float fl) {
    uint32_t bits;
    memcpy(&bits, &fl, sizeof(bits));
    unsigned sign = bits >> 31;
    unsigned exp = (bits >> 23) & 0xff;
    uint64_t signif = bits & 0x7fffff;
    float80 f;
    if (exp == 0xff)
        f.exp = EXP_SPECIAL;
    else if (exp == 0)
        f.exp = signif == 0 ? 0 : bias(1 - 0x7f);
    else
        f.exp = bias((int) exp - 0x7f);
    f.signif = signif << 40;
    if (exp != 0)
        f.signif |= CURSED_BIT;
    f.sign = sign;
    if (exp == 0 && signif != 0)
        f80_exceptions |= F80_EXC_DENORMAL;
    if (f80_issnan(f)) {
        f80_exceptions |= F80_EXC_INVALID;
        f.signif |= 1ull << 62;
    }
    return f80_normalize(f);
}

// FST m32/m64: round to an IEEE format with frac_bits fraction bits and
// exp_bits exponent bits, in the current rounding mode, raising what the x87
// raises -- overflow to infinity or to the largest finite value depending on
// the mode, underflow for a tiny inexact result, invalid for a signalling NaN
// (which comes out quieted, its payload truncated as the hardware does).
static uint64_t f80_to_ieee(float80 f, int frac_bits, int exp_bits) {
    const int ieee_bias = (1 << (exp_bits - 1)) - 1;
    const uint64_t exp_special = (1u << exp_bits) - 1;
    const uint64_t frac_mask = ((uint64_t) 1 << frac_bits) - 1;
    const uint64_t quiet = (uint64_t) 1 << (frac_bits - 1);
    uint64_t sign = (uint64_t) f.sign << (frac_bits + exp_bits);

    if (!f80_is_supported(f)) {
        f80_exceptions |= F80_EXC_INVALID;
        return (exp_special << frac_bits) | quiet;
    }
    if (f.exp == EXP_SPECIAL) {
        if (f80_isinf(f))
            return sign | (exp_special << frac_bits);
        if (f80_issnan(f))
            f80_exceptions |= F80_EXC_INVALID;
        return sign | (exp_special << frac_bits) | ((f.signif >> (63 - frac_bits)) & frac_mask) | quiet;
    }
    if (f80_iszero(f))
        return sign;

    // value = signif * 2^(e - 63), with the leading 1 at bit 63
    uint64_t signif = f.signif;
    int e = unbias_denormal(f.exp);
    int lz = __builtin_clzl(signif);
    signif <<= lz;
    e -= lz;

    int shift = 63 - frac_bits;
    bool tiny = e < 1 - ieee_bias;
    if (tiny)
        shift += (1 - ieee_bias) - e;
    int caller_inexact = f80_inexact;
    f80_inexact = 0;
    uint64_t r = (uint64_t) u128_shift_right_round(signif, shift, f.sign);
    uint64_t bits;
    if (tiny) {
        // A denormal, or zero -- or the smallest normal, when rounding carried
        // into bit frac_bits, which is exactly where exponent field 1 goes.
        bits = r;
        if (f80_inexact && r < ((uint64_t) 1 << frac_bits))
            f80_exceptions |= F80_EXC_UNDERFLOW;
    } else {
        if (r >> (frac_bits + 1)) {
            r >>= 1;
            e++;
        }
        if (e > ieee_bias) {
            f80_exceptions |= F80_EXC_OVERFLOW;
            f80_inexact = 1;
            bool to_max = f80_rounding_mode == round_chop ||
                (f80_rounding_mode == round_up && f.sign) ||
                (f80_rounding_mode == round_down && !f.sign);
            bits = to_max ? ((exp_special - 1) << frac_bits) | frac_mask : exp_special << frac_bits;
            f80_rounded_up = !to_max;
        } else {
            bits = ((uint64_t) (e + ieee_bias) << frac_bits) | (r & frac_mask);
        }
    }
    f80_inexact |= caller_inexact;
    return sign | bits;
}

double f80_to_double(float80 f) {
    uint64_t bits = f80_to_ieee(f, 52, 11);
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

float f80_to_float(float80 f) {
    uint32_t bits = (uint32_t) f80_to_ieee(f, 23, 8);
    float fl;
    memcpy(&fl, &bits, sizeof(fl));
    return fl;
}

float80 f80_round(float80 f) {
    if (!f80_is_supported(f)) {
        f80_exceptions |= F80_EXC_INVALID;
        return F80_NAN;
    }
    if (f80_issnan(f)) {
        f80_exceptions |= F80_EXC_INVALID;
        f.signif |= 1ull << 62;
        return f;
    }
    // Shift out all the bits to the right of the point (early exit if there are none)
    int bits_to_clear = 63 - unbias(f.exp);
    if (bits_to_clear <= 0)
        return f;

    f = f80_shift_right(f, bits_to_clear);
    if (f.signif == 0) {
        // If that just totally eradicated the significand, guess the answer is 0
        f.exp = EXP_DENORMAL;
    } else {
        f = f80_normalize(f);
    }
    return f;
}

float80 f80_neg(float80 f) {
    f.sign = ~f.sign;
    return f;
}
float80 f80_abs(float80 f) {
    f.sign = 0;
    return f;
}

#define handle_nans(a, b) do { \
    if (!f80_is_supported(a) || !f80_is_supported(b)) { \
        f80_exceptions |= F80_EXC_INVALID; \
        return F80_NAN; \
    } \
    if (f80_issnan(a) || f80_issnan(b)) { \
        f80_exceptions |= F80_EXC_INVALID; \
        a.signif |= f80_isnan(a) ? 1ull << 62 : 0; \
        b.signif |= f80_isnan(b) ? 1ull << 62 : 0; \
    } \
    /* this case is bizarre but hey I don't make the chips. though the amd spec
     * says it's undefined which nan is returned if both have the same
     * significant and different sign, so why am I doing this */\
    if (f80_isnan(a) && f80_isnan(b) && a.sign && !b.sign) \
        return b; \
    if (f80_isnan(a)) \
        return a; \
    if (f80_isnan(b)) \
        return b; \
} while(0)

float80 f80_add(float80 a, float80 b) {
    handle_nans(a, b);

    // An infinite operand is exact: it is the answer, unless the other one
    // is the opposite infinity. Letting it through to the rounding path below
    // made it look like an overflow (OE and PE on inf + 1).
    if (f80_isinf(a) || f80_isinf(b)) {
        if (f80_isinf(a) && f80_isinf(b) && a.sign != b.sign) {
            f80_exceptions |= F80_EXC_INVALID;
            return F80_NAN;
        }
        return f80_isinf(a) ? a : b;
    }
    // Zeros of opposite sign sum to +0, except rounding down, where it is -0.
    if (f80_iszero(a) && f80_iszero(b)) {
        float80 z = {0};
        z.sign = a.sign == b.sign ? a.sign : f80_rounding_mode == round_down;
        return z;
    }

    // a has larger exponent, b has smaller exponent
    if (a.exp < b.exp) {
        float80 tmp = a;
        a = b;
        b = tmp;
    }

    // reduce the number of cases to deal with
    bool flipped = false;
    if (a.sign) {
        a.sign = ~a.sign;
        b.sign = ~b.sign;
        flipped = true;
    }
    // now either both are positive (addition) or a is positive and b is
    // negative (subtraction)

    // do the addition in insane precision to fix that bug with adding 2^64 and 1.5
    uint128_t a_signif = (uint128_t) a.signif << 64;
    uint128_t b_signif = (uint128_t) b.signif << 64;
    // shift b (smaller exponent) right until the exponents are equal -- the
    // real exponents: a denormal's field of 0 means the same scale as 1
    b_signif = u128_shift_right_sticky(b_signif,
            unbias_denormal(a.exp) - unbias_denormal(b.exp));

    int sign = a.sign;
    int exp = unbias_denormal(a.exp);
    uint128_t signif = a_signif;
    if (!b.sign) {
        // b is positive, so add
        if (!f80_isinf(a)) {
            if (__builtin_add_overflow(a_signif, b_signif, &signif)) {
                // in case of overflow, lose 1 bit of precision
                signif = u128_shift_right_sticky(signif, 1);
                signif |= (uint128_t) 1 << 127; // recover the bit lost by the overflow
                exp++;
            }
        }
    } else {
        // b is negative, so subtract
        // but first, special case time!

        // infinity - infinity is indefinite, not zero
        if (f80_isinf(a) && f80_isinf(b)) {
            f80_exceptions |= F80_EXC_INVALID;
            return F80_NAN;
        }

        // When subtracting a (relatively) very small number in chop mode, all
        // the bits will get shifted out and nothing will happen, but this
        // should give a smaller result.
        if (f80_rounding_mode == round_chop && b_signif == 0 && b.signif != 0)
            b_signif = 1;

        // Depending on the rounding mode, it's possible that shifting out all
        // the bits produced 1 instead of 0. Subtracting 1 from infinity would
        // give 2^16384-1 which is wrong.
        if (f80_isinf(a))
            b_signif = 0;

        if (a_signif >= b_signif) {
            // we can subtract without underflow
            signif = a_signif - b_signif;
        } else {
            // the answer will be negative
            signif = b_signif - a_signif;
            sign = 1;
        }

        // a bizarre special case https://twitter.com/tblodt/status/1262145524620234752
        if (signif == 0 && a_signif != 0 && f80_rounding_mode == round_down)
            return (float80) {.sign = 1};

        // a - a = 0
        if (signif == 0)
            return (float80) {0};
    }

    if (flipped)
        sign = !sign;
    float80 f = u128_normalize_round(signif, exp, sign);
    assert(f80_is_supported(f));
    return f;
}
float80 f80_sub(float80 a, float80 b) {
    return f80_add(a, f80_neg(b));
}

float80 f80_mul(float80 a, float80 b) {
    handle_nans(a, b);

    if (f80_isinf(a) || f80_isinf(b)) {
        // infinity times zero is undefined
        if (f80_iszero(a) || f80_iszero(b)) {
            f80_exceptions |= F80_EXC_INVALID;
            return F80_NAN;
        }
        // infinity times anything else is infinity
        float80 f = F80_INF;
        f.sign = a.sign ^ b.sign;
        return f;
    }

    // add exponents (the +1 is necessary to be correct in 128-bit precision)
    int f_exp = unbias_denormal(a.exp) + unbias_denormal(b.exp) + 1;
    // multiply significands
    uint128_t f_signif = (uint128_t) a.signif * b.signif;
    // normalize and round the 128-bit result
    float80 f = u128_normalize_round(f_signif, f_exp, a.sign ^ b.sign);
    // xor signs
    f.sign = a.sign ^ b.sign;
    return f;
}

float80 f80_div(float80 a, float80 b) {
    handle_nans(a, b);

    float80 f;
    if (f80_isinf(a)) {
        // dividing into infinity gives infinity
        f = F80_INF;
        // except infinity / infinity, which is an invalid operation and so
        // gives the real indefinite (sign set), not the positive quiet NaN
        if (f80_isinf(b)) {
            f80_exceptions |= F80_EXC_INVALID;
            return F80_INDEFINITE;
        }
    } else if (f80_isinf(b)) {
        // dividing by infinity gives zero
        f = (float80) {0};
    } else if (f80_iszero(b)) {
        // division by zero gives infinity
        f = F80_INF;
        // except 0 / 0, likewise an invalid operation. Returned directly:
        // the `f.sign = a.sign ^ b.sign` below would otherwise clear the
        // indefinite's sign bit, which is the whole thing that distinguishes
        // it from an ordinary quiet NaN.
        if (f80_iszero(a)) {
            f80_exceptions |= F80_EXC_INVALID;
            return F80_INDEFINITE;
        }
        f80_exceptions |= F80_EXC_DIVZERO;
    } else if (f80_iszero(a)) {
        f = (float80) {0};
    } else {
        // Normalize both significands (denormals have no integer bit), then
        // take 128 quotient bits in two long divisions and fold the final
        // remainder in as a sticky bit, so rounding sees the exact quotient.
        // The old version kept no sticky bit and could round 1/(2^63 - 0.5)
        // down where the hardware rounds up.
        uint64_t as = a.signif, bs = b.signif;
        int ea = unbias_denormal(a.exp), eb = unbias_denormal(b.exp);
        int la = __builtin_clzl(as), lb = __builtin_clzl(bs);
        as <<= la;
        bs <<= lb;
        ea -= la;
        eb -= lb;
        uint128_t n = (uint128_t) as << 63;
        uint128_t q_hi = n / bs;
        uint128_t r = n % bs;
        uint128_t q_lo = (r << 64) / bs;
        r = (r << 64) % bs;
        uint128_t signif = (q_hi << 64) | q_lo;
        if (r != 0)
            signif |= 1;
        f = u128_normalize_round(signif, ea - eb, a.sign ^ b.sign);
    }

    f.sign = a.sign ^ b.sign;
    return f;
}

// An instruction computed from several float80 steps runs them at 64 bits and
// round-to-nearest, whatever precision control and the rounding mode say, and
// the steps' own flags are not the instruction's: f80_full_precision_end puts
// back the flags from before, adding only PE if the caller says so.
struct f80_mode_save_ {
    enum f80_rounding_mode rounding;
    int precision, exceptions, inexact, rounded_up;
};
static struct f80_mode_save_ f80_full_precision_begin(void) {
    struct f80_mode_save_ saved = {f80_rounding_mode, f80_precision,
                                   f80_exceptions, f80_inexact, f80_rounded_up};
    f80_rounding_mode = round_to_nearest;
    f80_precision = 64;
    return saved;
}
static void f80_full_precision_end(struct f80_mode_save_ saved, bool inexact) {
    f80_rounding_mode = saved.rounding;
    f80_precision = saved.precision;
    f80_exceptions = saved.exceptions;
    f80_inexact = saved.inexact || inexact;
    f80_rounded_up = saved.rounded_up;
}

// The exponent of x's leading 1, for a finite nonzero x: a denormal's is below
// the smallest normal's, by the zeros above its first set bit.
static int f80_exponent(float80 x) {
    if (x.exp == EXP_DENORMAL)
        return unbias(EXP_MIN) - __builtin_clzll(x.signif);
    return unbias(x.exp);
}

// FPREM / FPREM1 share one EXACT remainder.
//
// The old f80_mod computed x - trunc(x/y)*y, which is the right formula and the
// wrong arithmetic: f80_div rounds, and the product of a rounded quotient with
// y is not x's remainder. x87's FPREM is exact by construction, and the error
// grew with the quotient -- measured, 78 differing cases against real x86_64
// hardware over ordinary long-double inputs.
//
// The exact algorithm is repeated scaled subtraction. At each step the divisor
// is scaled to just below the running remainder, so r lies in [t, 2t) whenever
// a subtraction happens -- and by Sterbenz's lemma r - t is then exactly
// representable, so no step loses a bit. What comes out is the true remainder
// with the quotient truncated toward zero, plus the quotient's low bit, which
// is all FPREM1 needs to break its tie.
//
// Both return a COMPLETE reduction. Real hardware may reduce partially and set
// C2 to ask the caller to loop; software that loops on C2 simply exits at once,
// which is what fpu_prem already assumed.
static float80 f80_remainder_common(float80 x, float80 y, bool ieee) {
    // Invalid operations -- a zero divisor, or an infinite dividend -- give the
    // x87 real indefinite, which is the NEGATIVE quiet NaN. A propagated NaN
    // operand is returned as-is rather than replaced -- unless the other one
    // is unsupported, which is invalid even beside a quiet NaN.
    if (!f80_is_supported(x) || !f80_is_supported(y)) {
        f80_exceptions |= F80_EXC_INVALID;
        return F80_INDEFINITE;
    }
    if (f80_issnan(x) || f80_issnan(y))
        f80_exceptions |= F80_EXC_INVALID;
    if (f80_isnan(x)) {
        x.signif |= 1ull << 62;
        return x;
    }
    if (f80_isnan(y)) {
        y.signif |= 1ull << 62;
        return y;
    }
    if (f80_isinf(x) || f80_iszero(y)) {
        f80_exceptions |= F80_EXC_INVALID;
        return F80_INDEFINITE;
    }
    if (f80_isdenormal(x) || f80_isdenormal(y))
        f80_exceptions |= F80_EXC_DENORMAL;
    if (f80_iszero(x) || f80_isinf(y))
        return x;

    // Every step is exact, but f80_lt subtracts to compare, a difference of
    // far-apart values is not, and FPREM1 doubles the remainder, which can
    // overflow: without this, FPREM reported PE, FPREM1 near the top OE, and
    // precision control rounded the steps (10 mod 3.0000000001 was 1.0).
    struct f80_mode_save_ saved = f80_full_precision_begin();
    float80 r = f80_abs(x);
    float80 d = f80_abs(y);
    int quotient_odd = 0;

    if (!f80_lt(r, d)) {
        for (int i = f80_exponent(r) - f80_exponent(d); i >= 0; i--) {
            float80 t = f80_scale(d, i);
            if (f80_lt(r, t))
                continue;
            r = f80_sub(r, t);
            if (i == 0)
                quotient_odd = 1;
        }
    }

    // FPREM1 rounds the quotient to nearest-even instead of truncating, so it
    // takes one more subtraction when the remainder is past halfway -- or
    // exactly at halfway with an odd quotient. |d - r| is exact there for the
    // same Sterbenz reason, since the branch implies r >= d/2.
    bool flip = false;
    if (ieee) {
        float80 twice_r = f80_add(r, r);
        if (f80_lt(d, twice_r) || (f80_eq(twice_r, d) && quotient_odd)) {
            r = f80_sub(d, r);
            flip = true;
        }
    }

    f80_full_precision_end(saved, false);
    r.sign = flip ? !x.sign : x.sign;
    return r;
}

float80 f80_mod(float80 x, float80 y) {
    return f80_remainder_common(x, y, false);
}

// FPREM1 (D9 F5): the IEEE-754 remainder. Declared in float80.h since the x87
// work began and never written, which is why FPREM1 raised SIGILL.
float80 f80_rem(float80 x, float80 y) {
    return f80_remainder_common(x, y, true);
}

bool f80_uncomparable(float80 a, float80 b) {
    if (!f80_is_supported(a) || !f80_is_supported(b))
        return true;
    if (f80_isnan(a) || f80_isnan(b))
        return true;
    return false;
}

bool f80_lt(float80 a, float80 b) {
    if (f80_uncomparable(a, b))
        return false;
    // same signed infinities are equal, not less (though subtraction would produce nan)
    if (f80_isinf(a) && f80_isinf(b) && a.sign == b.sign)
        return false;
    // zeroes are always equal
    if (f80_iszero(a) && f80_iszero(b))
        return false;
    // if a < b then a - b < 0
    float80 diff = f80_sub(a, b);
    return diff.sign == 1 && !f80_iszero(diff);
}
bool f80_eq(float80 a, float80 b) {
    if (f80_uncomparable(a, b))
        return false;
    if (f80_iszero(a)) a.sign = 0;
    if (f80_iszero(b)) b.sign = 0;
    return a.sign == b.sign && a.exp == b.exp && a.signif == b.signif;
}

bool f80_lte(float80 a, float80 b) {
    return f80_lt(a, b) || f80_eq(a, b);
}
bool f80_gt(float80 a, float80 b) {
    return !f80_lte(a, b);
}

// ln((den + num) / (den - num)) as 2 atanh(num / den):
//     2 (t + t^3/3 + t^5/5 + ...),  t = num / den.
// Both callers keep |t| at or below 3 - 2 sqrt(2) = 0.172, so each term is at
// least 33 times smaller than the one before and a 64-bit sum needs about 13.
// The ratio form is the point: num is formed exactly (m - 1 for m near 1 is
// exact, and FYL2XP1 is handed its x directly), so a result near zero keeps
// its full relative precision.
static float80 f80_ln_atanh(float80 num, float80 den) {
    float80 t = f80_div(num, den);
    float80 t2 = f80_mul(t, t);
    float80 sum = t;
    float80 power = t;
    for (int k = 3; k < 400; k += 2) {
        power = f80_mul(power, t2);
        float80 next = f80_add(sum, f80_div(power, f80_from_int(k)));
        if (f80_eq(next, sum))
            break;
        sum = next;
    }
    // Doubled by adding, which is exact.
    return f80_add(sum, sum);
}

// log2(e) and sqrt(2), rounded to 64 bits.
static const float80 f80_log2e_ = {.signif = 0xb8aa3b295c17f0bc, .signExp = 0x3fff};
static const float80 f80_sqrt2_ = {.signif = 0xb504f333f9de6484, .signExp = 0x3fff};

// FYL2X and FYL2XP1 are transcendental instructions, which the x87 computes at
// full precision whatever the precision-control field says, so the series runs
// at 64 bits and round-to-nearest (f80_full_precision_begin). Its own steps'
// flags are not the instruction's either -- a tiny term underflows without the
// result doing so -- so they are put back as they were, and the caller is told
// only whether the result is inexact. The x87 reports PE for every FYL2X
// operand but 1, powers of two included (checked on camd), so that is what it
// says.

// log2(x) = e + log2(m), x = m * 2^e with m in [sqrt(1/2), sqrt(2)).
//
// This was a bit-at-a-time loop that squared x once per result bit. Each
// squaring doubles the relative error already in x, so the low bits of the
// answer were noise, and near x = 1, where the answer is small, that noise was
// most of a double: Java's Math.log(1.001) on the i386 guest was 125 ulp out,
// and musl's i386 log() is the same FYL2X. It also looped forever on +inf.
float80 f80_log2(float80 x) {
    float80 zero = f80_from_int(0);
    float80 one = f80_from_int(1);
    // FYL2X's special operands. log2(0) is -infinity and a division by zero
    // -- musl's i386 log() is FYL2X, and log(0.0) came back NaN -- a negative
    // operand is invalid, and infinity and 1 are exact.
    if (!f80_is_supported(x)) {
        f80_exceptions |= F80_EXC_INVALID;
        return F80_INDEFINITE;
    }
    if (f80_isnan(x)) {
        if (f80_issnan(x)) {
            f80_exceptions |= F80_EXC_INVALID;
            x.signif |= 1ull << 62;
        }
        return x;
    }
    if (f80_iszero(x)) {
        f80_exceptions |= F80_EXC_DIVZERO;
        return f80_neg(F80_INF);
    }
    if (x.sign) {
        f80_exceptions |= F80_EXC_INVALID;
        return F80_INDEFINITE;
    }
    if (f80_isinf(x))
        return x;
    if (f80_eq(x, one))
        return zero;

    if (f80_isdenormal(x))
        f80_exceptions |= F80_EXC_DENORMAL;
    struct f80_mode_save_ saved = f80_full_precision_begin();
    int e;
    float80 m = x;
    if (f80_isdenormal(x)) {
        int shift = __builtin_clzll(x.signif);
        m.signif = x.signif << shift;
        e = unbias(EXP_MIN) - shift;
    } else {
        e = unbias(x.exp);
    }
    m.exp = bias(0);
    if (f80_gt(m, f80_sqrt2_)) {
        m.exp = bias(-1);
        e++;
    }
    float80 ln_m = f80_ln_atanh(f80_sub(m, one), f80_add(m, one));
    float80 res = f80_add(f80_from_int(e), f80_mul(ln_m, f80_log2e_));
    f80_full_precision_end(saved, true);
    return res;
}

// log2(1 + x) for FYL2XP1, as 2 atanh(x / (2 + x)) * log2(e): x is never added
// to 1, which is what would throw a tiny x away. The instruction is defined
// for |x| < 1 - sqrt(2)/2, where |t| stays under 0.172; beyond that the
// series still converges, more slowly, for any x > -1. At and below -1 the
// hardware's answer is undefined (a real x87 returned -1 and -2 for -1 and
// -2), so this gives the logarithm's own: -inf and ZE, or invalid.
float80 f80_log2p1(float80 x) {
    // The special operands as for f80_log2, at x + 1: log2(0) at x = -1.
    if (!f80_is_supported(x)) {
        f80_exceptions |= F80_EXC_INVALID;
        return F80_INDEFINITE;
    }
    if (f80_isnan(x)) {
        if (f80_issnan(x)) {
            f80_exceptions |= F80_EXC_INVALID;
            x.signif |= 1ull << 62;
        }
        return x;
    }
    float80 minus_one = f80_from_int(-1);
    if (f80_eq(x, minus_one)) {
        f80_exceptions |= F80_EXC_DIVZERO;
        return f80_neg(F80_INF);
    }
    if (f80_lt(x, minus_one)) {
        f80_exceptions |= F80_EXC_INVALID;
        return F80_INDEFINITE;
    }
    if (f80_isinf(x) || f80_iszero(x))
        return x;
    if (f80_isdenormal(x))
        f80_exceptions |= F80_EXC_DENORMAL;
    struct f80_mode_save_ saved = f80_full_precision_begin();
    float80 res;
    if (x.exp < bias(-64)) {
        // log2(1 + x) = x log2(e) (1 - x/2 + ...), and below 2^-64 the
        // correction is under half an ulp: one rounding, where the series
        // would round a denormal t and then its product again.
        res = f80_mul(x, f80_log2e_);
    } else {
        float80 ln = f80_ln_atanh(x, f80_add(f80_from_int(2), x));
        res = f80_mul(ln, f80_log2e_);
    }
    f80_full_precision_end(saved, true);
    // A tiny operand gives a denormal result, which is an underflow.
    if (f80_isdenormal(res))
        f80_exceptions |= F80_EXC_UNDERFLOW;
    return res;
}

// floor(sqrt(n)) and the remainder n - floor(sqrt(n))^2, digit by digit.
static uint64_t u128_isqrt(uint128_t n, uint128_t *rem) {
    uint128_t res = 0;
    uint128_t bit = (uint128_t) 1 << 126;
    while (bit > n)
        bit >>= 2;
    while (bit != 0) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    *rem = n;
    return (uint64_t) res;
}

// FSQRT, correctly rounded in every mode and precision. This used to iterate
// Newton's method through f80_div, which is not correctly rounded outside
// round-to-nearest, and whose intermediate roundings set PE and C1 even for
// an exact root (sqrt(4)).
float80 f80_sqrt(float80 x) {
    if (!f80_is_supported(x)) {
        f80_exceptions |= F80_EXC_INVALID;
        return F80_INDEFINITE;
    }
    if (f80_iszero(x))
        return x;
    if (f80_isnan(x)) {
        if (f80_issnan(x)) {
            f80_exceptions |= F80_EXC_INVALID;
            x.signif |= 1ull << 62;
        }
        return x;
    }
    // Invalid operation: x87 answers with the real indefinite, whose sign bit
    // is SET. Returning the positive quiet NaN differed from hardware by
    // exactly that bit.
    if (x.sign) {
        f80_exceptions |= F80_EXC_INVALID;
        return F80_INDEFINITE;
    }
    if (f80_isinf(x))
        return x;
    if (f80_isdenormal(x))
        f80_exceptions |= F80_EXC_DENORMAL;

    // x = m * 2^E with m's leading 1 at bit 63. Put an even power of two
    // outside and take the integer root of what is left: 64 bits of it.
    uint64_t m = x.signif;
    int E = unbias_denormal(x.exp) - 63;
    int lz = __builtin_clzl(m);
    m <<= lz;
    E -= lz;
    uint128_t n = (E & 1) ? (uint128_t) m << 63 : (uint128_t) m << 64;
    int half = (E & 1) ? (E - 63) / 2 : (E - 64) / 2;
    uint128_t rem;
    uint64_t r = u128_isqrt(n, &rem);
    // Below r's last bit, all that rounding needs: the half bit (the root is
    // past r + 1/2 exactly when rem > r; never exactly at it) and whether any
    // bit at all is set.
    uint128_t signif = (uint128_t) r << 64;
    if (rem != 0)
        signif |= rem > r ? (uint128_t) 3 << 62 : 1;
    return u128_normalize_round(signif, half + 63, 0);
}

// x * 2^scale, rounded once in the current rounding mode -- and always to 64
// bits, since precision control does not apply to FSCALE (checked on camd: a
// 64-bit significand scaled under PC=24 keeps every bit).
float80 f80_scale(float80 x, int scale) {
    if (!f80_is_supported(x) || f80_isnan(x))
        return F80_NAN;
    if (f80_isinf(x) || f80_iszero(x))
        return x;
    // 2^16 takes any finite value out of range either way; clamping there
    // keeps the exponent sum below from overflowing an int.
    if (scale > 0x10000)
        scale = 0x10000;
    if (scale < -0x10000)
        scale = -0x10000;
    int precision = f80_precision;
    f80_precision = 64;
    // A denormal's exponent field of 0 means the same scale as 1: unbias(0)
    // is one short of it, which halved every denormal FSCALE touched.
    float80 r = u128_normalize_round((uint128_t) x.signif << 64,
            unbias_denormal(x.exp) + scale, x.sign);
    f80_precision = precision;
    return r;
}

// FSCALE: x * 2^trunc(y). The truncation raises nothing; the scaling raises
// what any rounding does. The infinite scales are exact, except the two with
// no value, 0 * 2^+inf and inf * 2^-inf.
float80 f80_fscale(float80 x, float80 y) {
    handle_nans(x, y);
    if (f80_isdenormal(x) || f80_isdenormal(y))
        f80_exceptions |= F80_EXC_DENORMAL;
    if (f80_isinf(y)) {
        if (y.sign ? f80_isinf(x) : f80_iszero(x)) {
            f80_exceptions |= F80_EXC_INVALID;
            return F80_INDEFINITE;
        }
        if (f80_iszero(x) || f80_isinf(x))
            return x;
        float80 r = y.sign ? (float80) {0} : F80_INF;
        r.sign = x.sign;
        return r;
    }
    int scale;
    if (y.exp >= bias(16))
        scale = 0x10000; // f80_scale's clamp
    else if (y.exp < bias(0))
        scale = 0; // including zero and the denormals
    else
        scale = (int) (y.signif >> (63 - unbias(y.exp)));
    if (y.sign)
        scale = -scale;
    return f80_scale(x, scale);
}

// FXTRACT: x = signif * 2^exp with signif in [1, 2) and x's sign -- a denormal
// is normalized first, its exponent below the smallest normal's. Zero has no
// exponent: -infinity, and divide-by-zero. An infinity's is +infinity.
void f80_xtract(float80 x, float80 *exp, float80 *signif) {
    if (!f80_is_supported(x)) {
        f80_exceptions |= F80_EXC_INVALID;
        *exp = *signif = F80_INDEFINITE;
        return;
    }
    if (f80_isnan(x)) {
        if (f80_issnan(x)) {
            f80_exceptions |= F80_EXC_INVALID;
            x.signif |= 1ull << 62;
        }
        *exp = *signif = x;
        return;
    }
    *signif = x;
    if (f80_isinf(x)) {
        *exp = F80_INF;
        return;
    }
    if (f80_iszero(x)) {
        f80_exceptions |= F80_EXC_DIVZERO;
        *exp = F80_INF;
        exp->sign = 1;
        return;
    }
    if (f80_isdenormal(x))
        f80_exceptions |= F80_EXC_DENORMAL;
    *exp = f80_from_int(f80_exponent(x));
    signif->signif = x.signif << __builtin_clzll(x.signif);
    signif->exp = bias(0);
}
