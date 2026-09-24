#ifndef ISH_ACCEL_PIX_H
#define ISH_ACCEL_PIX_H

#include <stdbool.h>
#include <stdint.h>

// Host-native pixel kernels for the pixman accelerator (ISH_SYS_PIXOP,
// kernel/ish_accel_pix.c). Operate on DIRECT host pointers already resolved
// by kernel/user.c's user_transform_rect/user_transform_rect_two -- these
// functions never touch guest memory or locks themselves. Every kernel here
// must be bit-exact against the corresponding real pixman operation on
// a8r8g8b8 pixel data (validated by tests/manual/pixman_accel.c's
// differential harness); any op that can't be made bit-exact must be
// declined by the caller (kernel/ish_accel_pix.c's dispatch), never
// approximated here.

// Fill `pixels` consecutive a8r8g8b8 pixels at `dst` with `value` (plain
// overwrite, matching pixman_fill's actual semantics -- NOT a real XOR
// despite the historical `_xor` parameter name in pixman's own API,
// verified empirically against real pixman before this was written).
void ish_pix_fill_row(void *dst, uint32_t pixels, uint32_t value);

// Copy `pixels` consecutive a8r8g8b8/x8r8g8b8 pixels from `src` to `dst`
// (PIXMAN_OP_SRC). Straight forward memcpy -- the caller (ish_accel_pix.c)
// has already declined any request where src and dst regions could overlap,
// so this never needs memmove-style direction handling.
void ish_pix_copy_row(const void *src, void *dst, uint32_t pixels);

// PIXMAN_OP_SRC from an x8r8g8b8 source into an a8r8g8b8 destination: pixman
// does NOT copy the source's padding byte, it writes 0xff alpha (its
// src_x888_8888 fast path; checked against real pixman, which copies the
// byte unchanged for every other a8r8g8b8/x8r8g8b8 pairing).
void ish_pix_copy_row_set_alpha(const void *src, void *dst, uint32_t pixels);

// Premultiplied-alpha OVER of `pixels` consecutive a8r8g8b8 src pixels onto
// a8r8g8b8 dst pixels: out_c = src_c + mul_un8(dst_c, 255 - src_a), applied
// uniformly to all 4 byte lanes (A,R,G,B, each saturating). If
// `src_is_opaque` is true (the src surface is x8r8g8b8, whose top byte
// pixman treats as always-opaque padding rather than real alpha), src_a is
// forced to 255 for every pixel regardless of what's actually in that byte.
//
// `dst_is_opaque` (dst surface is x8r8g8b8) only changes behavior when
// `src_is_opaque` is ALSO true: real pixman then takes a fast path that
// degenerates OVER to a literal word-for-word copy of src, INCLUDING
// whatever garbage happens to be in src's own top byte -- it does NOT
// compute and store 0xff there. This is genuinely different from the
// src_is_opaque-only case (dst NOT opaque), where pixman legitimately
// computes real alpha=0xff and writes it, because that dst's alpha channel
// is meaningful. Verified empirically on the mint oracle
// (xrgb_src_opaque_check.c, pixman_accel_plan.md) before being written here
// -- do not "simplify" this back to always forcing 0xff, it was tried and is
// provably wrong for the opaque-src+opaque-dst combination.
void ish_pix_over_row(const void *src, void *dst, uint32_t pixels, bool src_is_opaque, bool dst_is_opaque);

// Masked premultiplied-alpha OVER (PIXMAN_OP_OVER with a non-NULL a8 mask):
// each src pixel's premultiplied channels (including its own alpha) are
// first scaled by mask_alpha/255 (mul_un8), then blended onto dst with the
// same formula as ish_pix_over_row. Validated the same way -- differential
// vs real pixman on a8r8g8b8 src/dst + a8 mask, 300k+ edge+random cases,
// 0 mismatches, before being written here. `src_is_opaque` has the same
// meaning as ish_pix_over_row's (x8r8g8b8 src forces alpha=255 before mask
// scaling is applied).
void ish_pix_over_mask_row(const void *src, const uint8_t *mask, void *dst, uint32_t pixels, bool src_is_opaque);

// The same two blends with a SOLID source: every source pixel is `src`, a
// premultiplied a8r8g8b8 value (pixman's color_32 for a solid-fill image,
// alpha real). pixman turns an OVER of an opaque solid into SRC and skips a
// fully transparent one; both land on the same bytes as this formula, on
// an a8r8g8b8 or an x8r8g8b8 destination alike. Validated against real
// pixman before being written here (see pixman_accel_plan.md).
void ish_pix_over_solid_row(uint32_t src, void *dst, uint32_t pixels);
void ish_pix_over_solid_mask_row(uint32_t src, const uint8_t *mask, void *dst, uint32_t pixels);

#endif
