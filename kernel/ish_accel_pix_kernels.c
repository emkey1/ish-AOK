// Pixel kernels for the pixman accelerator. Pure functions over host memory
// (no guest awareness, no locking) -- see ish_accel_pix.h for the contract
// and kernel/ish_accel_pix.c for the guest-facing dispatch that resolves
// direct pointers via kernel/user.c and calls these per row-span.
#include <string.h>
#include "kernel/ish_accel_pix.h"

void ish_pix_fill_row(void *dst, uint32_t pixels, uint32_t value) {
    uint32_t *d = (uint32_t *) dst;
    for (uint32_t i = 0; i < pixels; i++)
        d[i] = value;
}

void ish_pix_copy_row(const void *src, void *dst, uint32_t pixels) {
    memcpy(dst, src, (size_t) pixels * 4);
}

void ish_pix_copy_row_set_alpha(const void *src, void *dst, uint32_t pixels) {
    const uint32_t *s = (const uint32_t *) src;
    uint32_t *d = (uint32_t *) dst;
    for (uint32_t i = 0; i < pixels; i++)
        d[i] = s[i] | 0xff000000u;
}

// Fast approximate divide-by-255, exact for all inputs in [0,255] -- the
// same identity pixman itself uses internally (verified empirically: see
// the OVER validation this kernel is derived from).
static inline uint8_t mul_un8(uint8_t a, uint8_t b) {
    uint32_t t = (uint32_t) a * b + 0x80;
    return (uint8_t) (((t >> 8) + t) >> 8);
}

static inline uint8_t add_sat_un8(uint8_t a, uint8_t b) {
    unsigned t = (unsigned) a + b;
    return t > 255 ? 255 : (uint8_t) t;
}

void ish_pix_over_row(const void *src, void *dst, uint32_t pixels, bool src_is_opaque, bool dst_is_opaque) {
    const uint32_t *s = (const uint32_t *) src;
    uint32_t *d = (uint32_t *) dst;
    if (src_is_opaque && dst_is_opaque) {
        // Both surfaces are opaque (x8r8g8b8): real pixman's fast path is a
        // literal copy, garbage top byte and all -- see ish_accel_pix.h.
        memcpy(d, s, (size_t) pixels * 4);
        return;
    }
    for (uint32_t i = 0; i < pixels; i++) {
        uint32_t sp = s[i], dp = d[i];
        uint8_t sa = src_is_opaque ? 0xff : (uint8_t) (sp >> 24);
        uint8_t sr = (uint8_t) (sp >> 16), sg = (uint8_t) (sp >> 8), sb = (uint8_t) sp;
        uint8_t da = (uint8_t) (dp >> 24), dr = (uint8_t) (dp >> 16), dg = (uint8_t) (dp >> 8), db = (uint8_t) dp;
        uint8_t inv = (uint8_t) (255 - sa);
        uint8_t oa = add_sat_un8(sa, mul_un8(da, inv));
        uint8_t or_ = add_sat_un8(sr, mul_un8(dr, inv));
        uint8_t og = add_sat_un8(sg, mul_un8(dg, inv));
        uint8_t ob = add_sat_un8(sb, mul_un8(db, inv));
        d[i] = ((uint32_t) oa << 24) | ((uint32_t) or_ << 16) | ((uint32_t) og << 8) | ob;
    }
}

void ish_pix_over_mask_row(const void *src, const uint8_t *mask, void *dst, uint32_t pixels, bool src_is_opaque) {
    const uint32_t *s = (const uint32_t *) src;
    uint32_t *d = (uint32_t *) dst;
    for (uint32_t i = 0; i < pixels; i++) {
        uint32_t sp = s[i], dp = d[i];
        uint8_t ma = mask[i];
        uint8_t sa = src_is_opaque ? 0xff : (uint8_t) (sp >> 24);
        uint8_t sr = (uint8_t) (sp >> 16), sg = (uint8_t) (sp >> 8), sb = (uint8_t) sp;
        // Scale src's premultiplied channels (incl. its own alpha) by the
        // mask's alpha first -- this is the whole difference from plain
        // OVER, verified against real pixman before writing this.
        sa = mul_un8(sa, ma);
        sr = mul_un8(sr, ma);
        sg = mul_un8(sg, ma);
        sb = mul_un8(sb, ma);
        uint8_t da = (uint8_t) (dp >> 24), dr = (uint8_t) (dp >> 16), dg = (uint8_t) (dp >> 8), db = (uint8_t) dp;
        uint8_t inv = (uint8_t) (255 - sa);
        uint8_t oa = add_sat_un8(sa, mul_un8(da, inv));
        uint8_t or_ = add_sat_un8(sr, mul_un8(dr, inv));
        uint8_t og = add_sat_un8(sg, mul_un8(dg, inv));
        uint8_t ob = add_sat_un8(sb, mul_un8(db, inv));
        d[i] = ((uint32_t) oa << 24) | ((uint32_t) or_ << 16) | ((uint32_t) og << 8) | ob;
    }
}

// One premultiplied OVER of s (already scaled by any mask) onto d, the
// formula ish_pix_over_row and ish_pix_over_mask_row share.
static inline uint32_t over_px(uint8_t sa, uint8_t sr, uint8_t sg, uint8_t sb, uint32_t dp) {
    uint8_t da = (uint8_t) (dp >> 24), dr = (uint8_t) (dp >> 16), dg = (uint8_t) (dp >> 8), db = (uint8_t) dp;
    uint8_t inv = (uint8_t) (255 - sa);
    uint8_t oa = add_sat_un8(sa, mul_un8(da, inv));
    uint8_t or_ = add_sat_un8(sr, mul_un8(dr, inv));
    uint8_t og = add_sat_un8(sg, mul_un8(dg, inv));
    uint8_t ob = add_sat_un8(sb, mul_un8(db, inv));
    return ((uint32_t) oa << 24) | ((uint32_t) or_ << 16) | ((uint32_t) og << 8) | ob;
}

void ish_pix_over_solid_row(uint32_t src, void *dst, uint32_t pixels) {
    uint32_t *d = (uint32_t *) dst;
    uint8_t sa = (uint8_t) (src >> 24), sr = (uint8_t) (src >> 16), sg = (uint8_t) (src >> 8), sb = (uint8_t) src;
    for (uint32_t i = 0; i < pixels; i++)
        d[i] = over_px(sa, sr, sg, sb, d[i]);
}

void ish_pix_over_solid_mask_row(uint32_t src, const uint8_t *mask, void *dst, uint32_t pixels) {
    uint32_t *d = (uint32_t *) dst;
    uint8_t sa = (uint8_t) (src >> 24), sr = (uint8_t) (src >> 16), sg = (uint8_t) (src >> 8), sb = (uint8_t) src;
    for (uint32_t i = 0; i < pixels; i++) {
        uint8_t ma = mask[i];
        d[i] = over_px(mul_un8(sa, ma), mul_un8(sr, ma), mul_un8(sg, ma), mul_un8(sb, ma), d[i]);
    }
}
