// iSH pixman accelerator: guest-facing syscall glue around host-native pixel
// kernels (kernel/ish_accel_pix_kernels.c). A guest issues syscall
// ISH_SYS_PIXOP with a pointer to a struct ish_pix_req; the host runs FILL/
// COPY(SRC)/OVER directly on the guest's pixel buffers (via
// kernel/user.c's user_transform_rect/user_transform_rect_two -- no bounce
// buffer) instead of the guest emulating pixman's own C implementation
// instruction by instruction. Off by default (doEnablePixAccel); enabled
// only if a self-test against known-good values passes.
//
// Delivery is a guest-side LD_PRELOAD shim (opt/AOK/pixman/, Phase 2)
// interposing pixman's public entry points: it probes this syscall once,
// declines (falls through to real pixman) for anything outside the op/
// format set below, and forwards the exact same semantics otherwise -- so
// this file's contract IS "must be bit-exact with real pixman for every
// case it accepts, or refuse the case", never "close enough". See
// pixman_accel_plan.md for the full design and phasing.

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "kernel/calls.h"
#include "kernel/task.h"
#include "kernel/errno.h"
#include "kernel/ish_accel_pix.h"
#include "debug.h"

bool doEnablePixAccel = false;
static bool pix_selftest_ok = false;
static pthread_once_t pix_selftest_once = PTHREAD_ONCE_INIT;

// Known-good (src, dst) -> expected OVER results, independently verified
// against real pixman_image_composite32 on a8r8g8b8 1x1 images before this
// kernel was written (200,125-case random+edge-case sweep, 0 mismatches;
// see pixman_accel_plan.md's Phase 1 notes). A handful of those cases,
// including the "invalid premultiplied" saturating-add edge case that the
// naive (non-saturating) formula gets wrong, are enough to catch a
// regression in ish_pix_over_row without needing pixman linked into the
// emulator itself.
static bool pix_selftest_over(void) {
    static const struct { uint32_t src, dst, expected; } cases[] = {
        {0x00000000u, 0xffaabbccu, 0xffaabbccu}, // fully transparent src -> dst unchanged
        {0xffaabbccu, 0x00000000u, 0xffaabbccu}, // fully opaque src -> src unchanged
        {0x80808080u, 0x80808080u, 0xc0c0c0c0u}, // half-over-half, all channels equal (every channel blends identically, not just alpha)
        {0x01ffffffu, 0x01ffffffu, 0x02ffffffu}, // saturating-add edge case (invalid premultiplied input)
        {0xfffe0000u, 0xff0000ffu, 0xfffe0000u}, // near-opaque src red over opaque dst blue
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        uint32_t dst = cases[i].dst;
        ish_pix_over_row(&cases[i].src, &dst, 1, false, false);
        if (dst != cases[i].expected)
            return false;
    }
    // src_is_opaque forces alpha=255 regardless of the src's actual top
    // byte -- degenerates to a straight copy of the RGB bytes plus 0xff alpha,
    // as long as dst is NOT ALSO opaque (dst has a meaningful alpha channel).
    uint32_t src_garbage_alpha = 0x11aabbccu, dst2 = 0xffffffffu;
    ish_pix_over_row(&src_garbage_alpha, &dst2, 1, true, false);
    if (dst2 != 0xffaabbccu)
        return false;
    // src_is_opaque AND dst_is_opaque: real pixman's fast path is a literal
    // copy INCLUDING src's own garbage top byte, not a computed 0xff -- see
    // ish_accel_pix.h and pixman_accel_plan.md (xrgb_src_opaque_check.c).
    uint32_t src_garbage_alpha2 = 0x37ff0000u, dst3 = 0x800000ffu;
    ish_pix_over_row(&src_garbage_alpha2, &dst3, 1, true, true);
    if (dst3 != 0x37ff0000u)
        return false;
    uint32_t fillbuf[3] = {0, 0, 0};
    ish_pix_fill_row(fillbuf, 3, 0x11223344u);
    if (fillbuf[0] != 0x11223344u || fillbuf[1] != 0x11223344u || fillbuf[2] != 0x11223344u)
        return false;
    uint32_t copysrc[2] = {0xdeadbeefu, 0x12345678u}, copydst[2] = {0, 0};
    ish_pix_copy_row(copysrc, copydst, 2);
    if (copydst[0] != 0xdeadbeefu || copydst[1] != 0x12345678u)
        return false;
    // x8r8g8b8 -> a8r8g8b8: real pixman 0.44 turns 0x12345678 into ff345678
    ish_pix_copy_row_set_alpha(copysrc, copydst, 2);
    if (copydst[0] != 0xffadbeefu || copydst[1] != 0xff345678u)
        return false;
    // OVER_MASK: half-alpha src through a half-alpha mask onto half-alpha
    // dst, all channels equal -- part of the same edge-case combination
    // independently verified against real pixman (300k+ cases, 0
    // mismatches) before ish_pix_over_mask_row was written.
    uint32_t mask_src = 0x80808080u, mask_dst = 0x80808080u;
    uint8_t mask_alpha = 0x80;
    ish_pix_over_mask_row(&mask_src, &mask_alpha, &mask_dst, 1, false);
    if (mask_dst != 0xa0a0a0a0u)
        return false;
    // Solid source, the same two blends, and a zero mask leaving dst alone:
    // values checked against real pixman 0.44 on the camd oracle (see
    // pixman_accel_plan.md, "solid sources").
    uint32_t solid_dst[2] = {0x80808080u, 0x37123456u};
    ish_pix_over_solid_row(0x80808080u, solid_dst, 1);
    if (solid_dst[0] != 0xc0c0c0c0u)
        return false;
    uint8_t solid_mask[2] = {0x80, 0x00};
    solid_dst[0] = 0x80808080u;
    ish_pix_over_solid_mask_row(0x80808080u, solid_mask, solid_dst, 2);
    if (solid_dst[0] != 0xa0a0a0a0u || solid_dst[1] != 0x37123456u)
        return false;
    return true;
}

static void run_pix_selftest(void) {
    pix_selftest_ok = pix_selftest_over();
    if (!pix_selftest_ok)
        printk("ish-accel-pix: self-test FAILED, pixman accelerator disabled\n");
}

static bool pix_accel_ready(void) {
    pthread_once(&pix_selftest_once, run_pix_selftest);
    return pix_selftest_ok;
}

void ish_accel_pix_init(void) {
    (void) pix_accel_ready();
}

// COPY_SET_ALPHA is COPY from an x8r8g8b8 source into an a8r8g8b8
// destination (alpha forced to 0xff, as pixman does). An op of its own rather
// than a flag on COPY, because COPY never looked at its flags: a kernel that
// predates it refuses the unknown op, where it would have silently ignored a
// new flag and copied the padding byte.
enum { ISH_PIX_OP_FILL = 0, ISH_PIX_OP_COPY = 1, ISH_PIX_OP_OVER = 2, ISH_PIX_OP_OVER_MASK = 3,
       ISH_PIX_OP_COPY_SET_ALPHA = 4, ISH_PIX_OP_SCALE_BILINEAR = 5 };
enum { ISH_PIX_FLAG_SRC_OPAQUE = 1u << 0, ISH_PIX_FLAG_DST_OPAQUE = 1u << 1, ISH_PIX_FLAG_SRC_SOLID = 1u << 2,
       ISH_PIX_FLAG_RAW_TAPS = 1u << 3 };

// Guest ABI, fixed-layout (identical on arm64/riscv64): the two leading u32s,
// then every 64-bit field together (matches struct ish_aead_req's
// convention in kernel/ish_accel.c), then 32-bit fields. All pointers are
// guest addresses; dst's format is selected by ISH_PIX_FLAG_DST_OPAQUE
// (unset = a8r8g8b8, real alpha channel; set = x8r8g8b8) and only matters
// for plain OVER (see ish_pix_over_row's dst_is_opaque doc -- it changes
// nothing for FILL/COPY/OVER_MASK, which are already format-agnostic or
// already validated dst-format-independent); src's format is selected by
// ISH_PIX_FLAG_SRC_OPAQUE (unset = a8r8g8b8, set = x8r8g8b8 i.e. top byte is
// not real alpha) and matters for OVER/OVER_MASK. mask is a8 (1 byte/pixel),
// only used for OVER_MASK. ISH_PIX_FLAG_SRC_SOLID (OVER and OVER_MASK only)
// replaces the source image with one premultiplied a8r8g8b8 value, carried
// in fill_pixel -- a pixman solid fill's color_32; src/src_stride are then
// ignored. A shim sends src=NULL and src_stride=0 with it, so a kernel that
// predates the flag refuses the request (src_stride < width*4) rather than
// reading a source that isn't there, and the shim falls back to pixman.
struct ish_pix_req {
    uint32_t op;
    uint32_t flags;
    uint64_t dst;
    uint64_t src;         // ignored for FILL
    uint64_t mask;        // only used for OVER_MASK
    uint32_t dst_stride;  // bytes/row
    uint32_t src_stride;  // ignored for FILL
    uint32_t mask_stride; // bytes/row, a8; ignored unless OVER_MASK
    int32_t dst_x, dst_y;
    int32_t src_x, src_y;   // ignored for FILL
    int32_t mask_x, mask_y; // ignored unless OVER_MASK
    uint32_t width, height;
    uint32_t fill_pixel;  // FILL, and the source colour for SRC_SOLID
};

// SCALE_BILINEAR: a bilinear stretch of a bits source onto the destination
// rectangle -- pixman_image_composite32 with a scale+translate transform,
// PIXMAN_FILTER_BILINEAR and REPEAT_NONE on the source, SRC or OVER, no mask,
// a8r8g8b8/x8r8g8b8 on both sides. What the Wayland compositor does to every
// scale-1 client at an output scale of 2, and the shape that kept labwc at
// 1.7 fps on an A10X (docs/TODO.md). Its own request layout, the same 80
// bytes as struct ish_pix_req so a kernel that predates it reads the whole
// thing and refuses the op. The shim does the fixed-point set-up (where the
// first pixel samples, x0/y0, already less half a pixel, and the step per
// pixel and per row) and declines anything pixman would not compute exactly
// that way; the kernel steps, interpolates (ish_pix_bilinear_row) and
// combines. ISH_PIX_FLAG_SRC_OPAQUE: the source is x8r8g8b8, whose taps
// pixman forces opaque -- unless ISH_PIX_FLAG_RAW_TAPS, which is pixman's
// NEON SRC x8->x8 path interpolating the raw padding byte. OVER blends onto
// the destination's raw value, padding byte included, as pixman does.
struct ish_pix_scale_req {
    uint32_t op;
    uint32_t flags;
    uint64_t dst;
    uint64_t src;
    uint32_t dst_stride;
    uint32_t src_stride;
    uint32_t src_width, src_height;
    int32_t dst_x, dst_y;
    uint32_t width, height;
    int32_t x0, y0;       // 16.16: the first pixel's sample point, less half a pixel
    int32_t ux, uy;       // 16.16: the step per destination pixel and per row
    uint32_t composite;   // 0 SRC, 1 OVER
    uint32_t reserved;    // 0
};
_Static_assert(sizeof(struct ish_pix_scale_req) == 80, "scale request must stay 80 bytes");

// Widest source span or destination row a stretch may use: a bound on the
// host buffers it allocates, far beyond any real surface.
#define ISH_PIX_SCALE_MAX_ROW 16384u

// Bound width*height so a bogus/adversarial request can't tie up the host
// thread on an absurd synthetic size -- real desktop surfaces at any
// plausible resolution are far under this (a 4K-wide by 4K-tall 32bpp
// region is 64M pixels / 256 MiB, already generous for a single composite
// call).
#define ISH_PIX_MAX_PIXELS (64u * 1024 * 1024)

struct pix_fill_ctx { uint32_t value; };
static void pix_fill_span(void *host, uint32_t pixels, void *ctx) {
    ish_pix_fill_row(host, pixels, ((struct pix_fill_ctx *) ctx)->value);
}

static void pix_copy_span(const void *src_host, void *dst_host, uint32_t pixels, void *ctx) {
    (void) ctx;
    ish_pix_copy_row(src_host, dst_host, pixels);
}

static void pix_copy_set_alpha_span(const void *src_host, void *dst_host, uint32_t pixels, void *ctx) {
    (void) ctx;
    ish_pix_copy_row_set_alpha(src_host, dst_host, pixels);
}

struct pix_over_ctx { bool src_is_opaque; bool dst_is_opaque; };
static void pix_over_span(const void *src_host, void *dst_host, uint32_t pixels, void *ctx) {
    struct pix_over_ctx *c = (struct pix_over_ctx *) ctx;
    ish_pix_over_row(src_host, dst_host, pixels, c->src_is_opaque, c->dst_is_opaque);
}

static void pix_over_mask_span(const void *src_host, const void *mask_host, void *dst_host,
        uint32_t pixels, void *ctx) {
    ish_pix_over_mask_row(src_host, (const uint8_t *) mask_host, dst_host, pixels,
            ((struct pix_over_ctx *) ctx)->src_is_opaque);
}

struct pix_solid_ctx { uint32_t src; };
static void pix_over_solid_span(void *dst_host, uint32_t pixels, void *ctx) {
    ish_pix_over_solid_row(((struct pix_solid_ctx *) ctx)->src, dst_host, pixels);
}

// Solid source through a mask: user_transform_rect_three with the mask
// standing in for the source as well (its span pointer is ignored), which
// keeps the one three-image walk rather than growing a mixed-bpp two-image
// one for this.
static void pix_over_solid_mask_span(const void *unused, const void *mask_host, void *dst_host,
        uint32_t pixels, void *ctx) {
    (void) unused;
    ish_pix_over_solid_mask_row(((struct pix_solid_ctx *) ctx)->src, (const uint8_t *) mask_host,
            dst_host, pixels);
}

// Conservative byte-range overlap check between the dst and src rectangles'
// full bounding boxes (their linear backing spans, top-left to
// bottom-right-inclusive-of-stride-padding) -- used to decline COPY/OVER
// whenever the two regions could possibly alias. COPY has no memmove-style
// direction handling (kernel/user.c's user_transform_rect_two always walks
// forward), and OVER reads dst before writing it per pixel but doesn't
// reason about a SECOND read of already-blended data from an overlapping
// src either, so both decline on any possible overlap rather than trying to
// get overlap semantics bit-exact.
static bool pix_ranges_overlap(uint64_t dst, uint32_t dst_stride, int32_t dst_y, uint32_t height,
        uint64_t src, uint32_t src_stride, int32_t src_y, uint32_t src_height) {
    uint64_t dst_lo = dst + (uint64_t) (int64_t) dst_y * dst_stride;
    uint64_t dst_hi = dst_lo + (uint64_t) height * dst_stride;
    uint64_t src_lo = src + (uint64_t) (int64_t) src_y * src_stride;
    uint64_t src_hi = src_lo + (uint64_t) src_height * src_stride;
    return dst_lo < src_hi && src_lo < dst_hi;
}

struct pix_scale_row_ctx {
    const uint32_t *row;  // the interpolated destination row
    uint32_t done;        // pixels of it already written
    bool over;
};
static void pix_scale_store_span(void *dst_host, uint32_t pixels, void *ctx) {
    struct pix_scale_row_ctx *c = (struct pix_scale_row_ctx *) ctx;
    if (c->over)
        ish_pix_over_row(c->row + c->done, dst_host, pixels, false, false);
    else
        memcpy(dst_host, c->row + c->done, (size_t) pixels * 4);
    c->done += pixels;
}

// A source row of the stretch, read once: a 2x stretch samples each pair of
// rows for two destination rows running.
struct pix_scale_cached_row {
    int32_t y;            // INT32_MIN when empty
    uint32_t *pixels;
};

static dword_t pix_scale_bilinear(const struct ish_pix_scale_req *r) {
    if (r->width == 0 || r->height == 0)
        return 0;
    if ((uint64_t) r->width * r->height > ISH_PIX_MAX_PIXELS || r->width > ISH_PIX_SCALE_MAX_ROW)
        return _EMSGSIZE;
    if (r->composite > 1 || r->reserved != 0 || r->ux <= 0 || r->uy <= 0)
        return _EOPNOTSUPP;
    if (r->dst_stride < r->width * 4 || (r->dst_stride % 4) != 0 || (r->dst % 4) != 0)
        return _EOPNOTSUPP;
    if (r->src_width == 0 || r->src_height == 0 || r->src_width > 0x7fff || r->src_height > 0x7fff ||
            r->src_stride < r->src_width * 4 || (r->src_stride % 4) != 0 || (r->src % 4) != 0)
        return _EOPNOTSUPP;
    // The source is read while the destination is written: decline any
    // chance they share memory, as the other ops do.
    if (pix_ranges_overlap(r->dst, r->dst_stride, r->dst_y, r->height,
                            r->src, r->src_stride, 0, r->src_height))
        return _EOPNOTSUPP;
    // The sample points must stay pixman_fixed_t (the shim checks this too;
    // pixman steps them in 32 bits).
    int64_t x_last = (int64_t) r->x0 + (int64_t) (r->width - 1) * r->ux;
    int64_t y_last = (int64_t) r->y0 + (int64_t) (r->height - 1) * r->uy;
    if (x_last > INT32_MAX || y_last > INT32_MAX)
        return _EOPNOTSUPP;

    // The columns any destination pixel can reach, the same on every row.
    int32_t sw = (int32_t) r->src_width, sh = (int32_t) r->src_height;
    int64_t col_lo = (int64_t) (r->x0 >> 16), col_hi = (x_last >> 16) + 1;
    if (col_lo < 0) col_lo = 0;
    if (col_hi > sw - 1) col_hi = sw - 1;
    int32_t col0 = (int32_t) col_lo, cols = col_hi >= col_lo ? (int32_t) (col_hi - col_lo + 1) : 0;
    if ((uint32_t) cols > ISH_PIX_SCALE_MAX_ROW)
        return _EMSGSIZE;

    uint32_t *out = malloc((size_t) r->width * 4);
    struct pix_scale_cached_row cache[2] = {
        { INT32_MIN, cols > 0 ? malloc((size_t) cols * 4) : NULL },
        { INT32_MIN, cols > 0 ? malloc((size_t) cols * 4) : NULL },
    };
    dword_t err = 0;
    if (out == NULL || (cols > 0 && (cache[0].pixels == NULL || cache[1].pixels == NULL))) {
        err = _ENOMEM;
        goto done;
    }
    uint32_t tap_or = (r->flags & ISH_PIX_FLAG_SRC_OPAQUE) && !(r->flags & ISH_PIX_FLAG_RAW_TAPS)
            ? 0xff000000u : 0;

    for (uint32_t j = 0; j < r->height; j++) {
        int32_t y = (int32_t) ((int64_t) r->y0 + (int64_t) j * r->uy);
        int32_t y1 = y >> 16;
        int disty = (y >> 9) & 0x7f;
        const uint32_t *rows[2] = { NULL, NULL };
        for (int k = 0; k < 2; k++) {
            int32_t sy = y1 + k;
            if (cols == 0 || sy < 0 || sy >= sh)
                continue;
            struct pix_scale_cached_row *slot = &cache[sy & 1];
            if (slot->y != sy) {
                if (user_read(r->src + (uint64_t) sy * r->src_stride + (uint64_t) col0 * 4,
                        slot->pixels, (size_t) cols * 4)) {
                    err = _EFAULT;
                    goto done;
                }
                slot->y = sy;
            }
            rows[k] = slot->pixels;
        }
        ish_pix_bilinear_row(rows[0], rows[1], col0, cols, sw, r->x0, r->ux, disty, tap_or,
                out, r->width);
        struct pix_scale_row_ctx ctx = { out, 0, r->composite == 1 };
        if (user_transform_rect(r->dst, r->dst_stride, 4, r->dst_x, r->dst_y + (int32_t) j,
                r->width, 1, MEM_WRITE, pix_scale_store_span, &ctx)) {
            err = _EFAULT;
            goto done;
        }
    }
done:
    free(out);
    free(cache[0].pixels);
    free(cache[1].pixels);
    return err;
}

dword_t sys_ish_pixop_guest(guest_addr_t req_addr) {
    if (!doEnablePixAccel || !pix_accel_ready())
        return _ENOSYS;

    // Every request is 80 bytes; the op says which layout.
    union {
        struct ish_pix_req req;
        struct ish_pix_scale_req scale;
    } u;
    _Static_assert(sizeof(struct ish_pix_req) == sizeof(struct ish_pix_scale_req),
            "the two request layouts must be the same size");
    if (user_read(req_addr, &u, sizeof(u)))
        return _EFAULT;
    if (u.req.op == ISH_PIX_OP_SCALE_BILINEAR)
        return pix_scale_bilinear(&u.scale);
    struct ish_pix_req req = u.req;

    if (req.op != ISH_PIX_OP_FILL && req.op != ISH_PIX_OP_COPY &&
            req.op != ISH_PIX_OP_OVER && req.op != ISH_PIX_OP_OVER_MASK &&
            req.op != ISH_PIX_OP_COPY_SET_ALPHA)
        return _EOPNOTSUPP;
    if (req.width == 0 || req.height == 0)
        return 0; // no-op, matches pixman's own empty-rect behavior
    if ((uint64_t) req.width * req.height > ISH_PIX_MAX_PIXELS)
        return _EMSGSIZE; // too big for the fast path; caller falls back
    // bpp is fixed at 4 (a8r8g8b8/x8r8g8b8 only in v1) -- every address this
    // request touches must stay bpp-aligned so no pixel can ever straddle a
    // host page boundary (see user_transform_rect's contract). A stride
    // smaller than one full row is simply invalid.
    if (req.dst_stride < req.width * 4 || (req.dst_stride % 4) != 0 || (req.dst % 4) != 0)
        return _EOPNOTSUPP;
    bool solid = (req.flags & ISH_PIX_FLAG_SRC_SOLID) != 0;
    if (solid && req.op != ISH_PIX_OP_OVER && req.op != ISH_PIX_OP_OVER_MASK)
        return _EOPNOTSUPP;
    bool has_src = req.op != ISH_PIX_OP_FILL && !solid;
    if (has_src &&
            (req.src_stride < req.width * 4 || (req.src_stride % 4) != 0 || (req.src % 4) != 0))
        return _EOPNOTSUPP;
    if (has_src &&
            pix_ranges_overlap(req.dst, req.dst_stride, req.dst_y, req.height,
                                req.src, req.src_stride, req.src_y, req.height))
        return _EOPNOTSUPP;
    // mask is a8 (1 byte/pixel) -- alignment is trivially always satisfied
    // (bpp=1 can never straddle a page boundary), only the minimum-stride
    // check applies. Also decline if the mask could alias dst (mask reads
    // are page-resolved independently of dst's writes in the 3-image walk,
    // so an overlapping mask+dst has no defined behavior here either).
    if (req.op == ISH_PIX_OP_OVER_MASK) {
        if (req.mask_stride < req.width)
            return _EOPNOTSUPP;
        if (pix_ranges_overlap(req.dst, req.dst_stride, req.dst_y, req.height,
                                req.mask, req.mask_stride, req.mask_y, req.height))
            return _EOPNOTSUPP;
    }

    if (req.op == ISH_PIX_OP_FILL) {
        struct pix_fill_ctx ctx = { .value = req.fill_pixel };
        if (user_transform_rect(req.dst, req.dst_stride, 4, req.dst_x, req.dst_y,
                req.width, req.height, MEM_WRITE, pix_fill_span, &ctx))
            return _EFAULT;
        return 0;
    }

    if (req.op == ISH_PIX_OP_COPY || req.op == ISH_PIX_OP_COPY_SET_ALPHA) {
        if (user_transform_rect_two(req.dst, req.dst_stride, req.dst_x, req.dst_y,
                req.src, req.src_stride, req.src_x, req.src_y,
                4, req.width, req.height,
                req.op == ISH_PIX_OP_COPY ? pix_copy_span : pix_copy_set_alpha_span, NULL))
            return _EFAULT;
        return 0;
    }

    if (req.op == ISH_PIX_OP_OVER_MASK && solid) {
        struct pix_solid_ctx ctx = { .src = req.fill_pixel };
        if (user_transform_rect_three(
                req.dst, req.dst_stride, req.dst_x, req.dst_y, 4,
                req.mask, req.mask_stride, req.mask_x, req.mask_y, 1,
                req.mask, req.mask_stride, req.mask_x, req.mask_y, 1,
                req.width, req.height, pix_over_solid_mask_span, &ctx))
            return _EFAULT;
        return 0;
    }

    if (req.op == ISH_PIX_OP_OVER && solid) {
        struct pix_solid_ctx ctx = { .src = req.fill_pixel };
        if (user_transform_rect(req.dst, req.dst_stride, 4, req.dst_x, req.dst_y,
                req.width, req.height, MEM_WRITE, pix_over_solid_span, &ctx))
            return _EFAULT;
        return 0;
    }

    if (req.op == ISH_PIX_OP_OVER_MASK) {
        struct pix_over_ctx ctx = { .src_is_opaque = (req.flags & ISH_PIX_FLAG_SRC_OPAQUE) != 0 };
        if (user_transform_rect_three(
                req.dst, req.dst_stride, req.dst_x, req.dst_y, 4,
                req.src, req.src_stride, req.src_x, req.src_y, 4,
                req.mask, req.mask_stride, req.mask_x, req.mask_y, 1,
                req.width, req.height, pix_over_mask_span, &ctx))
            return _EFAULT;
        return 0;
    }

    // OVER
    struct pix_over_ctx ctx = {
        .src_is_opaque = (req.flags & ISH_PIX_FLAG_SRC_OPAQUE) != 0,
        .dst_is_opaque = (req.flags & ISH_PIX_FLAG_DST_OPAQUE) != 0,
    };
    if (user_transform_rect_two(req.dst, req.dst_stride, req.dst_x, req.dst_y,
            req.src, req.src_stride, req.src_x, req.src_y,
            4, req.width, req.height, pix_over_span, &ctx))
        return _EFAULT;
    return 0;
}
