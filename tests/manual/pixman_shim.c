// Differential test for the pixman accelerator's guest shim
// (opt/AOK/tools/pixman/ish_pixman_shim.c), the layer pixman_accel.c does not
// reach: which calls the shim accelerates, how it cuts a composite by the
// destination's clip region and bounds, how it turns a solid source into a
// fill, how it reimplements pixman_image_fill_boxes/fill_rectangles, and
// that everything it refuses still ends up exactly as pixman leaves it.
//
// The shim is compiled INTO this test (included below, from where every
// guest has it, /AOK/tools/pixman), so the pixman_* calls the test makes by
// name go through the shim and reach the kernel accelerator. The oracle is
// real pixman: the same functions looked up with dlsym on libpixman-1's own
// handle, which bypasses the shim. Every scenario runs both ways on identical
// buffers, and every byte -- padding included -- must match. The shim finds
// libpixman through dlsym(RTLD_NEXT), so the library is dlopen'd GLOBAL first.
//
// A differential test that never accelerates passes by testing nothing: the
// shim's own counters are read at the end, and each accelerated path must
// have been taken.
//
// Requires ISH_PIX_ACCEL=1 and libpixman-1 (both SKIP cleanly), and nothing
// else: the shim builds against the pixman.h vendored beside it.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_common.h"

#ifndef ISH_PIXMAN_SHIM_SOURCE
#define ISH_PIXMAN_SHIM_SOURCE "/AOK/tools/pixman/ish_pixman_shim.c"
#endif
#if defined(__has_include)
#  if __has_include(ISH_PIXMAN_SHIM_SOURCE)
#    define HAVE_SHIM 1
#  endif
#endif

#ifndef HAVE_SHIM
// Built outside a guest (tier0 builds every test on the host): no shim here.
int main(void) {
    printf("pixman_shim: SKIP (no %s to test)\n", ISH_PIXMAN_SHIM_SOURCE);
    return 0;
}
#else
#include ISH_PIXMAN_SHIM_SOURCE

// ---- the oracle: real pixman, straight from its own handle ---------------
static struct {
    pixman_image_t *(*create_bits)(pixman_format_code_t, int, int, uint32_t *, int);
    pixman_image_t *(*create_solid_fill)(const pixman_color_t *);
    pixman_bool_t (*unref)(pixman_image_t *);
    void (*composite32)(pixman_op_t, pixman_image_t *, pixman_image_t *, pixman_image_t *,
            int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
    pixman_bool_t (*fill_boxes)(pixman_op_t, pixman_image_t *, const pixman_color_t *, int,
            const pixman_box32_t *);
    pixman_bool_t (*fill_rectangles)(pixman_op_t, pixman_image_t *, const pixman_color_t *, int,
            const pixman_rectangle16_t *);
    pixman_bool_t (*blt)(uint32_t *, uint32_t *, int, int, int, int, int, int, int, int, int, int);
    pixman_bool_t (*fill)(uint32_t *, int, int, int, int, int, int, uint32_t);
    pixman_bool_t (*set_clip_region32)(pixman_image_t *, const pixman_region32_t *);
    void (*region32_init_rects)(pixman_region32_t *, const pixman_box32_t *, int);
    void (*region32_fini)(pixman_region32_t *);
} real_px;

static int load_pixman(void) {
    void *h = dlopen("libpixman-1.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (h == NULL)
        return 0;
#define GET(field, name) real_px.field = (__typeof__(real_px.field)) dlsym(h, name)
    GET(create_bits, "pixman_image_create_bits");
    GET(create_solid_fill, "pixman_image_create_solid_fill");
    GET(unref, "pixman_image_unref");
    GET(composite32, "pixman_image_composite32");
    GET(fill_boxes, "pixman_image_fill_boxes");
    GET(fill_rectangles, "pixman_image_fill_rectangles");
    GET(blt, "pixman_blt");
    GET(fill, "pixman_fill");
    GET(set_clip_region32, "pixman_image_set_clip_region32");
    GET(region32_init_rects, "pixman_region32_init_rects");
    GET(region32_fini, "pixman_region32_fini");
#undef GET
    return real_px.create_bits && real_px.create_solid_fill && real_px.unref &&
            real_px.composite32 && real_px.fill_boxes && real_px.fill_rectangles &&
            real_px.blt && real_px.fill && real_px.set_clip_region32 && real_px.region32_init_rects &&
            real_px.region32_fini;
}

// ---- helpers -------------------------------------------------------------
static uint32_t rnd(void) {
    return ((uint32_t) rand() << 16) ^ (uint32_t) rand();
}

struct buf {
    uint32_t *bits;
    int w, h, stride; // stride in bytes
};

static struct buf buf_new(int w, int h, int bpp) {
    struct buf b = { NULL, w, h, 0 };
    b.stride = ((w * bpp / 8) + 3 + (rand() % 3) * 4) & ~3; // sometimes padded
    b.bits = malloc((size_t) b.stride * h);
    for (size_t i = 0; i < (size_t) b.stride * h / 4; i++)
        b.bits[i] = rnd();
    return b;
}

static struct buf buf_clone(struct buf b) {
    struct buf c = b;
    c.bits = malloc((size_t) b.stride * b.h);
    memcpy(c.bits, b.bits, (size_t) b.stride * b.h);
    return c;
}

static int buf_same(struct buf a, struct buf b) {
    return memcmp(a.bits, b.bits, (size_t) a.stride * a.h) == 0;
}

static void check(int cond, const char *label) {
    test_log_if(!cond, "%s: %s\n", cond ? "ok" : "FAIL", label);
    if (!cond)
        failures_total++;
}

static pixman_color_t rand_color(void) {
    pixman_color_t c = { (uint16_t) rnd(), (uint16_t) rnd(), (uint16_t) rnd(), (uint16_t) rnd() };
    switch (rand() % 5) {
    case 0: c.alpha = 0xffff; break;                       // opaque
    case 1: c = (pixman_color_t) { 0, 0, 0, 0 }; break;    // transparent
    case 2:                                                // valid premultiplied
        c.red = (uint16_t) ((uint32_t) c.red * c.alpha / 0xffff);
        c.green = (uint16_t) ((uint32_t) c.green * c.alpha / 0xffff);
        c.blue = (uint16_t) ((uint32_t) c.blue * c.alpha / 0xffff);
        break;
    default: break;                                        // anything, invalid included
    }
    return c;
}

// 0-4 random boxes inside (and a little outside) a w x h image, some empty.
static int rand_boxes(pixman_box32_t *boxes, int w, int h, int allow_outside) {
    int n = rand() % 5;
    for (int i = 0; i < n; i++) {
        int x1 = rand() % (w + (allow_outside ? 8 : 0)) - (allow_outside ? 4 : 0);
        int y1 = rand() % (h + (allow_outside ? 8 : 0)) - (allow_outside ? 4 : 0);
        int x2 = x1 + rand() % (w / 2 + 2), y2 = y1 + rand() % (h / 2 + 2);
        if (!allow_outside) {
            if (x1 < 0) x1 = 0;
            if (y1 < 0) y1 = 0;
            if (x2 > w) x2 = w;
            if (y2 > h) y2 = h;
        }
        boxes[i] = (pixman_box32_t) { x1, y1, x2, y2 };
    }
    return n;
}

// Gives both destination images the same clip region (or none).
static void set_clip(pixman_image_t *shim_img, pixman_image_t *real_img, const pixman_box32_t *boxes, int n,
        int on) {
    if (!on)
        return;
    pixman_region32_t r;
    real_px.region32_init_rects(&r, boxes, n);
    pixman_image_set_clip_region32(shim_img, &r); // the shim records it
    real_px.set_clip_region32(real_img, &r);
    real_px.region32_fini(&r);
}

// ---- scenarios -----------------------------------------------------------
static const pixman_format_code_t rgb_formats[2] = { PIXMAN_a8r8g8b8, PIXMAN_x8r8g8b8 };

// pixman_image_composite32 with a bits or solid source, an optional a8 mask,
// SRC or OVER (and ADD, which must be left to pixman), onto a clipped or
// unclipped destination, the rect sometimes past the destination's edges and
// the source sometimes past its own. `directed` >= 0 picks the combination
// deterministically and uses a rect big enough to clear the size floors,
// so every shape the shim accelerates is certainly exercised; random
// scenarios then cover the edges.
static void scenario_composite(int iter, int directed) {
    int dw = 8 + rand() % 120, dh = 4 + rand() % 60;
    pixman_format_code_t dfmt = rgb_formats[rand() & 1], sfmt = rgb_formats[rand() & 1];
    int src_kind = rand() % 3;                  // 0 bits, 1 solid, 2 bits again
    int use_mask = rand() % 3 == 0;
    pixman_op_t ops[] = { PIXMAN_OP_SRC, PIXMAN_OP_OVER, PIXMAN_OP_OVER, PIXMAN_OP_ADD };
    pixman_op_t op = ops[rand() % 4];
    int use_clip = rand() % 3 == 0;
    if (directed >= 0) {
        // bit 0 op, bit 1 solid source, bit 2 source x8, bit 3 mask, bit 4
        // destination x8, bit 5 clip
        op = directed & 1 ? PIXMAN_OP_OVER : PIXMAN_OP_SRC;
        src_kind = directed & 2 ? 1 : 0;
        sfmt = rgb_formats[(directed >> 2) & 1];
        use_mask = (directed >> 3) & 1;
        dfmt = rgb_formats[(directed >> 4) & 1];
        use_clip = (directed >> 5) & 1;
        dw = 120;
        dh = 50;
    }
    int sw = dw + rand() % 20, sh = dh + rand() % 20;

    struct buf d_shim = buf_new(dw, dh, 32), d_real = buf_clone(d_shim);
    struct buf s = buf_new(sw, sh, 32), m = buf_new(sw, sh, 8);
    pixman_color_t color = rand_color();

    pixman_image_t *ds = real_px.create_bits(dfmt, dw, dh, d_shim.bits, d_shim.stride);
    pixman_image_t *dr = real_px.create_bits(dfmt, dw, dh, d_real.bits, d_real.stride);
    pixman_image_t *ss, *sr;
    if (src_kind == 1) {
        ss = pixman_image_create_solid_fill(&color); // through the shim: it learns the colour
        sr = real_px.create_solid_fill(&color);
    } else {
        ss = real_px.create_bits(sfmt, sw, sh, s.bits, s.stride);
        sr = real_px.create_bits(sfmt, sw, sh, s.bits, s.stride);
    }
    pixman_image_t *ms = use_mask ? real_px.create_bits(PIXMAN_a8, sw, sh, m.bits, m.stride) : NULL;

    pixman_box32_t clip[8];
    int nclip = rand_boxes(clip, dw, dh, 1);
    if (directed >= 0 && use_clip) {
        clip[0] = (pixman_box32_t) { 0, 0, dw / 2, dh };      // two big boxes
        clip[1] = (pixman_box32_t) { dw / 2 + 3, 5, dw, dh - 4 };
        nclip = 2;
    }
    set_clip(ds, dr, clip, nclip, use_clip);

    int x = rand() % (dw + 10) - 5, y = rand() % (dh + 10) - 5;
    int w = 1 + rand() % dw, h = 1 + rand() % dh;
    int sx = rand() % 8 == 0 ? rand() % 12 - 6 : rand() % (sw - (w < sw ? w : sw) + 1);
    int sy = rand() % 8 == 0 ? rand() % 12 - 6 : rand() % (sh - (h < sh ? h : sh) + 1);
    if (directed >= 0) {
        x = 2;
        y = 1;
        w = dw - 4;
        h = dh - 2;
        sx = 3;
        sy = 2;
    }

    pixman_image_composite32(op, ss, ms, ds, sx, sy, sx, sy, x, y, w, h);
    real_px.composite32(op, sr, ms, dr, sx, sy, sx, sy, x, y, w, h);

    char label[240];
    snprintf(label, sizeof label, "composite #%d op=%d src=%s%s mask=%d dst=%s %dx%d @%d,%d in %dx%d src@%d,%d clip=%d color=%04x,%04x,%04x,%04x",
            iter, op, src_kind == 1 ? "solid" : "bits",
            src_kind == 1 ? "" : sfmt == PIXMAN_x8r8g8b8 ? "/x8" : "/a8",
            use_mask, dfmt == PIXMAN_x8r8g8b8 ? "x8" : "a8", w, h, x, y, dw, dh, sx, sy, nclip,
            color.alpha, color.red, color.green, color.blue);
    check(buf_same(d_shim, d_real), label);

    if (ms != NULL)
        real_px.unref(ms);
    pixman_image_unref(ss);
    real_px.unref(sr);
    pixman_image_unref(ds);
    real_px.unref(dr);
    free(d_shim.bits);
    free(d_real.bits);
    free(s.bits);
    free(m.bits);
}

// pixman_image_fill_boxes and fill_rectangles: SRC, OVER, CLEAR and ADD,
// opaque and translucent colours, empty and overlapping boxes, a clip or
// none. Boxes stay inside the image: pixman writes outside it unchecked, so
// the oracle itself would be undefined there.
static void scenario_fill_boxes(int iter) {
    int dw = 8 + rand() % 150, dh = 4 + rand() % 80;
    pixman_format_code_t dfmt = rgb_formats[rand() & 1];
    pixman_op_t ops[] = { PIXMAN_OP_SRC, PIXMAN_OP_SRC, PIXMAN_OP_OVER, PIXMAN_OP_CLEAR, PIXMAN_OP_ADD };
    pixman_op_t op = ops[rand() % 5];
    struct buf d_shim = buf_new(dw, dh, 32), d_real = buf_clone(d_shim);
    pixman_image_t *ds = real_px.create_bits(dfmt, dw, dh, d_shim.bits, d_shim.stride);
    pixman_image_t *dr = real_px.create_bits(dfmt, dw, dh, d_real.bits, d_real.stride);
    pixman_box32_t clip[8];
    int nclip = rand_boxes(clip, dw, dh, 1);
    set_clip(ds, dr, clip, nclip, rand() % 3 == 0);
    pixman_color_t color = rand_color();

    pixman_box32_t boxes[8];
    int n = rand_boxes(boxes, dw, dh, 0);
    if (rand() % 4 == 0 && n < 8) // one big box, past the fill floor
        boxes[n++] = (pixman_box32_t) { 0, 0, dw, dh };
    int as_rects = rand() & 1;
    pixman_bool_t rs, rr;
    if (as_rects) {
        pixman_rectangle16_t rects[8];
        for (int i = 0; i < n; i++)
            rects[i] = (pixman_rectangle16_t) { (int16_t) boxes[i].x1, (int16_t) boxes[i].y1,
                    (uint16_t) (boxes[i].x2 > boxes[i].x1 ? boxes[i].x2 - boxes[i].x1 : 0),
                    (uint16_t) (boxes[i].y2 > boxes[i].y1 ? boxes[i].y2 - boxes[i].y1 : 0) };
        rs = pixman_image_fill_rectangles(op, ds, &color, n, rects);
        rr = real_px.fill_rectangles(op, dr, &color, n, rects);
    } else {
        rs = pixman_image_fill_boxes(op, ds, &color, n, boxes);
        rr = real_px.fill_boxes(op, dr, &color, n, boxes);
    }
    char label[200];
    snprintf(label, sizeof label, "fill_%s #%d op=%d dst=%s %dx%d boxes=%d clip=%d color=%04x,%04x,%04x,%04x",
            as_rects ? "rectangles" : "boxes", iter, op, dfmt == PIXMAN_x8r8g8b8 ? "x8" : "a8", dw, dh, n, nclip,
            color.alpha, color.red, color.green, color.blue);
    check(buf_same(d_shim, d_real), label);
    check(!rs == !rr, label);
    pixman_image_unref(ds);
    real_px.unref(dr);
    free(d_shim.bits);
    free(d_real.bits);
}

// pixman_blt, 32bpp: separate buffers, and the same buffer overlapping
// itself (which the kernel refuses and pixman must then do its own way).
static void scenario_blt(int iter) {
    int w = 4 + rand() % 200, h = 2 + rand() % 100;
    struct buf a = buf_new(w, h, 32), a_real = buf_clone(a);
    struct buf b = buf_new(w, h, 32), b_real = buf_clone(b);
    int same = rand() % 4 == 0;
    int bw = 1 + rand() % (w - 2), bh = 1 + rand() % (h - 1);
    int sx = rand() % (w - bw + 1), sy = rand() % (h - bh + 1);
    int dx = rand() % (w - bw + 1), dy = rand() % (h - bh + 1);
    struct buf *dst = same ? &a : &b, *dst_real = same ? &a_real : &b_real;
    pixman_bool_t rs = pixman_blt(a.bits, dst->bits, a.stride / 4, dst->stride / 4, 32, 32,
            sx, sy, dx, dy, bw, bh);
    pixman_bool_t rr = real_px.blt(a_real.bits, dst_real->bits, a_real.stride / 4, dst_real->stride / 4, 32, 32,
            sx, sy, dx, dy, bw, bh);
    char label[160];
    snprintf(label, sizeof label, "blt #%d %dx%d %d,%d -> %d,%d in %dx%d same=%d", iter, bw, bh, sx, sy, dx, dy, w, h, same);
    check(buf_same(a, a_real) && buf_same(b, b_real), label);
    check(!rs == !rr, label);
    free(a.bits);
    free(a_real.bits);
    free(b.bits);
    free(b_real.bits);
}

// pixman_fill at 32bpp (stride in words): under the fill floor the shim fills
// in the guest, over it through the kernel; both must be pixman's bytes.
static void scenario_fill(int iter) {
    int w = 4 + rand() % 200, h = 2 + rand() % 100;
    struct buf a = buf_new(w, h, 32), a_real = buf_clone(a);
    int fw = 1 + rand() % w, fh = 1 + rand() % h;
    int x = rand() % (w - fw + 1), y = rand() % (h - fh + 1);
    uint32_t v = rnd();
    pixman_bool_t rs = pixman_fill(a.bits, a.stride / 4, 32, x, y, fw, fh, v);
    pixman_bool_t rr = real_px.fill(a_real.bits, a_real.stride / 4, 32, x, y, fw, fh, v);
    char label[120];
    snprintf(label, sizeof label, "fill #%d %dx%d @%d,%d in %dx%d", iter, fw, fh, x, y, w, h);
    check(buf_same(a, a_real), label);
    check(!rs == !rr, label);
    free(a.bits);
    free(a_real.bits);
}

static sigjmp_buf probe_jmp;
static void probe_sigsys(int sig) {
    (void) sig;
    siglongjmp(probe_jmp, 1);
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(120));
    srand(0x5117);
    setenv("ISH_PIXMAN_STATS", "1", 1); // the shim's counters are the positive control

    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = probe_sigsys;
    sigaction(SIGSYS, &sa, &old);
    int available = 0;
    if (sigsetjmp(probe_jmp, 1) == 0)
        available = accel_available();
    sigaction(SIGSYS, &old, NULL);
    if (!available) {
        printf("pixman_shim: SKIP (accelerator unavailable -- needs ISH_PIX_ACCEL=1)\n");
        return 0;
    }
    if (!load_pixman()) {
        printf("pixman_shim: SKIP (libpixman-1 not found on this rootfs)\n");
        return 0;
    }

    for (int i = 0; i < 64; i++) {
        if ((i & 8) && !(i & 1))
            continue; // a mask is OVER only
        scenario_composite(i, i);
    }
    for (int i = 0; i < 600; i++)
        scenario_composite(i, -1);
    for (int i = 0; i < 300; i++)
        scenario_fill_boxes(i);
    for (int i = 0; i < 150; i++)
        scenario_blt(i);
    for (int i = 0; i < 150; i++)
        scenario_fill(i);

    // The positive control: each accelerated path was really taken.
    check(g_stats[STAT_ACCEL_COMPOSITE].calls > 0, "some composites and blits were accelerated");
    check(g_stats[STAT_ACCEL_MASK].calls > 0, "some masked composites were accelerated");
    check(g_stats[STAT_ACCEL_FILL].calls > 0, "some fills were accelerated");
    int solid_shapes = 0;
    for (int i = 0; i < SHAPE_SLOTS; i++) {
        if (g_shapes[i].used && g_shapes[i].reasons == 0 && g_shapes[i].src == KIND_SOLID)
            solid_shapes++;
    }
    check(solid_shapes > 0, "some solid-source composites were accelerated");
    test_logf("accelerated: composite %lu, mask %lu, fill %lu; declined bounds %lu, small %lu, op %lu\n",
            g_stats[STAT_ACCEL_COMPOSITE].calls, g_stats[STAT_ACCEL_MASK].calls, g_stats[STAT_ACCEL_FILL].calls,
            g_stats[STAT_DECLINE_BOUNDS].calls, g_stats[STAT_DECLINE_SMALL].calls, g_stats[STAT_DECLINE_OP].calls);
    // Keep the shim's exit-time statistics dump quiet: this is a test.
    memset(g_entry, 0, sizeof g_entry);
    return finish_suite("pixman_shim");
}
#endif
