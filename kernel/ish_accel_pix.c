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
       ISH_PIX_OP_COPY_SET_ALPHA = 4 };
enum { ISH_PIX_FLAG_SRC_OPAQUE = 1u << 0, ISH_PIX_FLAG_DST_OPAQUE = 1u << 1, ISH_PIX_FLAG_SRC_SOLID = 1u << 2 };

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

dword_t sys_ish_pixop_guest(guest_addr_t req_addr) {
    if (!doEnablePixAccel || !pix_accel_ready())
        return _ENOSYS;

    struct ish_pix_req req;
    if (user_read(req_addr, &req, sizeof(req)))
        return _EFAULT;

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
