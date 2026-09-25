#ifndef FLOAT80_H
#define FLOAT80_H

#include "misc.h"

typedef struct {
    uint64_t signif;
    union {
        uint16_t signExp;
        struct {
            unsigned exp:15;
            unsigned sign:1;
        };
    };
} float80;

float80 f80_from_int(int64_t i);
int64_t f80_to_int(float80 f);
float80 f80_from_double(double d);
double f80_to_double(float80 f);
// The single-precision pair, bit for bit. Going through a host double instead
// rounds twice, and runs the host FPU under the guest's SSE mode.
float80 f80_from_float(float f);
float f80_to_float(float80 f);
float80 f80_round(float80 f);

bool f80_isnan(float80 f);
bool f80_isinf(float80 f);
bool f80_iszero(float80 f);
bool f80_isdenormal(float80 f);
bool f80_is_supported(float80 f);

float80 f80_add(float80 a, float80 b);
float80 f80_sub(float80 a, float80 b);
float80 f80_mul(float80 a, float80 b);
float80 f80_div(float80 a, float80 b);
float80 f80_mod(float80 a, float80 b);
float80 f80_rem(float80 a, float80 b);

bool f80_lt(float80 a, float80 b);
bool f80_eq(float80 a, float80 b);
bool f80_uncomparable(float80 a, float80 b);

float80 f80_neg(float80 f);
float80 f80_abs(float80 f);

float80 f80_log2(float80 x);
float80 f80_sqrt(float80 x);

float80 f80_scale(float80 x, int scale);

// Used to implement fxtract
void f80_xtract(float80 f, int *exp, float80 *signif);

enum f80_rounding_mode {
    round_to_nearest = 0,
    round_down = 1,
    round_up = 2,
    round_chop = 3,
};
extern __thread enum f80_rounding_mode f80_rounding_mode;

// x87 precision control: the number of significand bits arithmetic results are
// rounded to. 64 (extended, the default), 53 (double) or 24 (single). Set from
// the control word's PC field, exactly like the rounding mode above -- glibc's
// i386 sin()/cos() switch to 53 and depend on every step rounding there.
extern __thread int f80_precision;

// Set by the rounding path on every operation that had to discard bits:
// f80_inexact says the result was rounded at all, f80_rounded_up says that
// rounding went away from zero. The x87 layer turns these into the status
// word's PE (a sticky exception flag) and C1 (the rounding-direction
// indicator, which reflects only the most recent operation). Callers clear
// them before an operation and read them after.
extern __thread int f80_inexact;
extern __thread int f80_rounded_up;

// The other exceptions an operation raised, in the x87 status word's own bit
// order, so the x87 layer can OR them straight in. Cleared by the caller
// before an operation, like f80_inexact, which stays the record of PE.
#define F80_EXC_INVALID   (1 << 0)
#define F80_EXC_DENORMAL  (1 << 1)
#define F80_EXC_DIVZERO   (1 << 2)
#define F80_EXC_OVERFLOW  (1 << 3)
#define F80_EXC_UNDERFLOW (1 << 4)
extern __thread int f80_exceptions;

// A NaN whose quiet bit (bit 62 of the significand) is clear. Using one as an
// operand is an invalid operation, and the NaN that comes out is quieted.
bool f80_issnan(float80 f);

#define F80_NAN ((float80) {.signif = 0xc000000000000000, .exp = 0x7fff, .sign = 0})
// The x87 "real indefinite": the QNaN an invalid operation produces, and its
// SIGN BIT IS SET. F80_NAN above is the positive quiet NaN, which is what a
// widened host NaN looks like -- not what FPREM of a zero divisor, or any other
// invalid x87 operation, returns. Measured against real x86_64 hardware: the
// two differ by exactly that bit.
#define F80_INDEFINITE ((float80) {.signif = 0xc000000000000000, .exp = 0x7fff, .sign = 1})
#define F80_INF ((float80) {.signif = 0x8000000000000000, .exp = 0x7fff, .sign = 0})

#endif
