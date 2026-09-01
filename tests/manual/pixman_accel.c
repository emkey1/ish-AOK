// Differential test for the AOK pixman accelerator (ISH_SYS_PIXOP syscall,
// kernel/ish_accel_pix.c): every FILL/COPY/OVER result the accelerator
// produces must be bit-identical to real pixman's own composite/fill, or
// the accelerator must decline (a negative errno) rather than approximate.
// Oracle is dlopen'd (not linked) so this test SKIPs cleanly on a rootfs
// with no libpixman-1 installed, instead of failing to build/run.
//
// Requires ISH_PIX_ACCEL=1 (and a build with the accelerator compiled in);
// SKIPs if the accelerator syscall reports unavailable -- or SIGSYSes, the
// answer from an emulator build that hasn't wired 0xacc1 into this guest's
// ABI (see probe_accel below) -- same convention as ambient_caps.c/
// chroot_getcwd.c's privilege-gated SKIPs.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "test_common.h"

extern long syscall(long, ...);
#define ISH_SYS_PIXOP 0xacc1

enum { PIX_OP_FILL = 0, PIX_OP_COPY = 1, PIX_OP_OVER = 2, PIX_OP_OVER_MASK = 3 };
enum { PIX_FLAG_SRC_OPAQUE = 1u << 0, PIX_FLAG_DST_OPAQUE = 1u << 1 };

struct ish_pix_req {
    uint32_t op, flags;
    uint64_t dst, src, mask;
    uint32_t dst_stride, src_stride, mask_stride;
    int32_t dst_x, dst_y;
    int32_t src_x, src_y;
    int32_t mask_x, mask_y;
    uint32_t width, height;
    uint32_t fill_pixel;
};

static long pixop(uint32_t op, uint32_t flags,
        void *dst, uint32_t dst_stride, int32_t dst_x, int32_t dst_y,
        void *src, uint32_t src_stride, int32_t src_x, int32_t src_y,
        uint32_t width, uint32_t height, uint32_t fill_pixel) {
    struct ish_pix_req r = {
        .op = op, .flags = flags,
        .dst = (uint64_t) (uintptr_t) dst, .src = (uint64_t) (uintptr_t) src,
        .dst_stride = dst_stride, .src_stride = src_stride,
        .dst_x = dst_x, .dst_y = dst_y, .src_x = src_x, .src_y = src_y,
        .width = width, .height = height, .fill_pixel = fill_pixel,
    };
    return syscall(ISH_SYS_PIXOP, &r);
}

static long pixop_mask(uint32_t flags,
        void *dst, uint32_t dst_stride, int32_t dst_x, int32_t dst_y,
        void *src, uint32_t src_stride, int32_t src_x, int32_t src_y,
        void *mask, uint32_t mask_stride, int32_t mask_x, int32_t mask_y,
        uint32_t width, uint32_t height) {
    struct ish_pix_req r = {
        .op = PIX_OP_OVER_MASK, .flags = flags,
        .dst = (uint64_t) (uintptr_t) dst, .src = (uint64_t) (uintptr_t) src, .mask = (uint64_t) (uintptr_t) mask,
        .dst_stride = dst_stride, .src_stride = src_stride, .mask_stride = mask_stride,
        .dst_x = dst_x, .dst_y = dst_y, .src_x = src_x, .src_y = src_y, .mask_x = mask_x, .mask_y = mask_y,
        .width = width, .height = height,
    };
    return syscall(ISH_SYS_PIXOP, &r);
}

// ---- pixman oracle, dlopen'd -----------------------------------------
typedef int pixman_bool_t;
typedef struct pixman_image pixman_image_t;
typedef enum { PIXMAN_OP_SRC = 1, PIXMAN_OP_OVER = 3 } pixman_op_t;
// Real pixman.h's pixman_format_code_t values, confirmed on-target (not
// hand-derived from the PIXMAN_FORMAT macro pattern a second time -- that's
// exactly how the first version of these two constants got the bpp field
// wrong, 4 bytes instead of 32 bits, which silently fed pixman_image_create_
// bits a bogus format and made the ORACLE wrong while this test kept
// reporting the accelerator as broken. Printed via a tiny probe linked
// against real pixman: PIXMAN_a8r8g8b8=0x20028888, PIXMAN_x8r8g8b8=0x20020888.
#define PIXMAN_a8r8g8b8 0x20028888
#define PIXMAN_x8r8g8b8 0x20020888
// PIXMAN_a8, same "confirmed on-target, never hand-derived twice" discipline.
#define PIXMAN_a8 0x08018000

static pixman_image_t *(*p_create_bits)(int, int, int, uint32_t *, int);
static void (*p_composite32)(pixman_op_t, pixman_image_t *, pixman_image_t *, pixman_image_t *,
        int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
static pixman_bool_t (*p_fill)(uint32_t *, int, int, int, int, int, int, uint32_t);
static pixman_bool_t (*p_unref)(pixman_image_t *);

static int load_pixman(void) {
    void *h = dlopen("libpixman-1.so.0", RTLD_NOW) ?: dlopen("libpixman-1.so", RTLD_NOW);
    if (h == NULL)
        return 0;
    p_create_bits = (pixman_image_t *(*)(int, int, int, uint32_t *, int)) dlsym(h, "pixman_image_create_bits");
    p_composite32 = (void (*)(pixman_op_t, pixman_image_t *, pixman_image_t *, pixman_image_t *,
            int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t)) dlsym(h, "pixman_image_composite32");
    p_fill = (pixman_bool_t (*)(uint32_t *, int, int, int, int, int, int, uint32_t)) dlsym(h, "pixman_fill");
    p_unref = (pixman_bool_t (*)(pixman_image_t *)) dlsym(h, "pixman_image_unref");
    return p_create_bits && p_composite32 && p_fill && p_unref;
}

// oracle_fill: pixman_fill's stride parameter is in 32-bit WORDS, not bytes
// (verified empirically against real pixman before this was written --
// see pixman_accel_plan.md). Our own req struct's *_stride fields are
// bytes throughout, so this wrapper is the one place that converts.
static void oracle_fill(uint32_t *bits, uint32_t stride_bytes, int32_t x, int32_t y,
        uint32_t width, uint32_t height, uint32_t value) {
    p_fill(bits, (int) (stride_bytes / 4), 32, x, y, (int) width, (int) height, value);
}

static void oracle_composite(pixman_op_t op, uint32_t format,
        uint32_t *dst, uint32_t dst_stride, int32_t dst_x, int32_t dst_y,
        uint32_t src_format, uint32_t *src, uint32_t src_stride, int32_t src_x, int32_t src_y,
        uint32_t width, uint32_t height, uint32_t canvas_w, uint32_t canvas_h,
        uint32_t src_canvas_w, uint32_t src_canvas_h) {
    pixman_image_t *dimg = p_create_bits((int) format, (int) canvas_w, (int) canvas_h, dst, (int) dst_stride);
    pixman_image_t *simg = p_create_bits((int) src_format, (int) src_canvas_w, (int) src_canvas_h, src, (int) src_stride);
    p_composite32(op, simg, NULL, dimg, src_x, src_y, 0, 0, dst_x, dst_y, (int32_t) width, (int32_t) height);
    p_unref(simg);
    p_unref(dimg);
}

// OVER with a mask (pixman_image_create_bits's `bits` param is uint32_t* for
// every format including a8 -- it just interprets the byte layout per
// format/stride, so an a8 canvas's raw bytes are handed through the same
// pointer type as everywhere else).
static void oracle_composite_mask(uint32_t format,
        uint32_t *dst, uint32_t dst_stride, int32_t dst_x, int32_t dst_y,
        uint32_t src_format, uint32_t *src, uint32_t src_stride, int32_t src_x, int32_t src_y,
        uint32_t *mask, uint32_t mask_stride, int32_t mask_x, int32_t mask_y,
        uint32_t width, uint32_t height, uint32_t canvas_w, uint32_t canvas_h,
        uint32_t src_canvas_w, uint32_t src_canvas_h, uint32_t mask_canvas_w, uint32_t mask_canvas_h) {
    pixman_image_t *dimg = p_create_bits((int) format, (int) canvas_w, (int) canvas_h, dst, (int) dst_stride);
    pixman_image_t *simg = p_create_bits((int) src_format, (int) src_canvas_w, (int) src_canvas_h, src, (int) src_stride);
    pixman_image_t *mimg = p_create_bits((int) PIXMAN_a8, (int) mask_canvas_w, (int) mask_canvas_h, mask, (int) mask_stride);
    p_composite32(PIXMAN_OP_OVER, simg, mimg, dimg, src_x, src_y, mask_x, mask_y, dst_x, dst_y,
            (int32_t) width, (int32_t) height);
    p_unref(simg);
    p_unref(mimg);
    p_unref(dimg);
}

// ---- test harness ------------------------------------------------------
static void check(int cond, const char *what) {
    test_log_if(!cond, "%s: %s\n", cond ? "ok" : "FAIL", what);
    if (!cond)
        failures_total++;
}

static uint32_t rand_pixel(void) {
    return ((uint32_t) rand() << 1) ^ (uint32_t) rand();
}

// A canvas with `pad` extra columns/rows beyond the tested rect, at a
// deliberately non-zero (x,y) origin, so the accelerator's row/page walk
// (kernel/user.c's user_transform_rect*) is exercised over real stride
// padding and an offset origin, not just a tight 0,0 buffer.
struct canvas {
    uint32_t *bits;
    uint32_t w, h, stride_bytes;
};

static struct canvas make_canvas(uint32_t w, uint32_t h) {
    struct canvas c = { .w = w, .h = h, .stride_bytes = w * 4 };
    // Round the whole allocation up to page multiples so a canvas can be
    // deliberately made multi-page (exercises the per-row page-boundary
    // resolve path, not just single-page rects).
    size_t bytes = (size_t) c.stride_bytes * h;
    c.bits = malloc(bytes);
    for (size_t i = 0; i < bytes / 4; i++)
        ((uint32_t *) c.bits)[i] = rand_pixel();
    return c;
}

static void free_canvas(struct canvas *c) {
    free(c->bits);
    c->bits = NULL;
}

// a8 mask canvas: 1 byte/pixel, same random-fill + padding/offset intent as
// struct canvas above.
struct mask_canvas {
    uint8_t *bits;
    uint32_t w, h, stride_bytes;
};

static struct mask_canvas make_mask_canvas(uint32_t w, uint32_t h) {
    // real pixman_image_create_bits REQUIRES every stride to be a multiple
    // of 4 bytes, even for a8 (1 byte/pixel) images -- discovered the hard
    // way (verified on the mint oracle): an unaligned stride doesn't return
    // an error, it silently corrupts the created image internally, which
    // looked exactly like a kernel bug in early testing here (the kernel
    // itself has no such requirement -- an a8 byte can never straddle a
    // page boundary regardless of stride -- but any REAL pixman_image_t a
    // shim would ever see was already built by pixman itself, so it always
    // has a valid stride by construction; this alignment need is purely an
    // artifact of this test constructing raw a8 buffers directly).
    uint32_t stride = (w + 3u) & ~3u;
    struct mask_canvas c = { .w = w, .h = h, .stride_bytes = stride };
    size_t bytes = (size_t) c.stride_bytes * h;
    c.bits = malloc(bytes);
    for (size_t i = 0; i < bytes; i++)
        c.bits[i] = (uint8_t) rand();
    return c;
}

static void free_mask_canvas(struct mask_canvas *c) {
    free(c->bits);
    c->bits = NULL;
}

static void test_fill(uint32_t canvas_w, uint32_t canvas_h, int32_t x, int32_t y, uint32_t w, uint32_t h, uint32_t value) {
    struct canvas accel = make_canvas(canvas_w, canvas_h);
    struct canvas oracle = make_canvas(canvas_w, canvas_h);
    memcpy(oracle.bits, accel.bits, (size_t) accel.stride_bytes * accel.h);

    long ret = pixop(PIX_OP_FILL, 0, accel.bits, accel.stride_bytes, x, y,
            NULL, 0, 0, 0, w, h, value);
    oracle_fill(oracle.bits, oracle.stride_bytes, x, y, w, h, value);

    char label[128];
    snprintf(label, sizeof(label), "fill %ux%u@%d,%d in %ux%u canvas value=%08x",
            w, h, x, y, canvas_w, canvas_h, value);
    check(ret == 0, label);
    check(memcmp(accel.bits, oracle.bits, (size_t) accel.stride_bytes * accel.h) == 0, label);

    free_canvas(&accel);
    free_canvas(&oracle);
}

// dst_opaque: when set, the ORACLE's dst image is created as PIXMAN_x8r8g8b8
// instead of PIXMAN_a8r8g8b8. The accelerator side (pixop) is unaffected --
// verified on the mint oracle (xrgb_dst_check.c) that pixman does NOT
// special-case x8r8g8b8 as a destination during compositing: the top byte is
// read/written as an ordinary alpha channel, byte-identical to a8r8g8b8 math,
// garbage in/garbage out. The "x8" vs "a8" distinction only changes behavior
// when the format is used as a SOURCE (forced alpha=0xff), which is what
// PIX_FLAG_SRC_OPAQUE already covers. So dst_opaque exists purely to make the
// oracle construct its image with the other format code and confirm the
// kernel's existing dst handling is already correct for it -- no kernel or
// accelerator-side flag is needed.
static void test_composite(uint32_t op, int src_opaque, int dst_opaque,
        uint32_t dst_canvas_w, uint32_t dst_canvas_h, int32_t dst_x, int32_t dst_y,
        uint32_t src_canvas_w, uint32_t src_canvas_h, int32_t src_x, int32_t src_y,
        uint32_t w, uint32_t h) {
    struct canvas dst_accel = make_canvas(dst_canvas_w, dst_canvas_h);
    struct canvas src = make_canvas(src_canvas_w, src_canvas_h);
    struct canvas dst_oracle = make_canvas(dst_canvas_w, dst_canvas_h);
    memcpy(dst_oracle.bits, dst_accel.bits, (size_t) dst_accel.stride_bytes * dst_accel.h);

    uint32_t flags = (src_opaque ? PIX_FLAG_SRC_OPAQUE : 0) | (dst_opaque ? PIX_FLAG_DST_OPAQUE : 0);
    long ret = pixop(op == PIXMAN_OP_SRC ? PIX_OP_COPY : PIX_OP_OVER, flags,
            dst_accel.bits, dst_accel.stride_bytes, dst_x, dst_y,
            src.bits, src.stride_bytes, src_x, src_y, w, h, 0);

    uint32_t src_format = src_opaque ? PIXMAN_x8r8g8b8 : PIXMAN_a8r8g8b8;
    uint32_t dst_format = dst_opaque ? PIXMAN_x8r8g8b8 : PIXMAN_a8r8g8b8;
    oracle_composite((pixman_op_t) op, dst_format,
            dst_oracle.bits, dst_oracle.stride_bytes, dst_x, dst_y,
            src_format, src.bits, src.stride_bytes, src_x, src_y,
            w, h, dst_canvas_w, dst_canvas_h, src_canvas_w, src_canvas_h);

    char label[160];
    snprintf(label, sizeof(label),
            "%s %ux%u dst@%d,%d(%ux%u) src@%d,%d(%ux%u) opaque=%d dst_opaque=%d",
            op == PIXMAN_OP_SRC ? "copy" : "over", w, h, dst_x, dst_y, dst_canvas_w, dst_canvas_h,
            src_x, src_y, src_canvas_w, src_canvas_h, src_opaque, dst_opaque);
    check(ret == 0, label);
    check(memcmp(dst_accel.bits, dst_oracle.bits, (size_t) dst_accel.stride_bytes * dst_accel.h) == 0, label);

    free_canvas(&dst_accel);
    free_canvas(&src);
    free_canvas(&dst_oracle);
}

static void test_composite_mask(int src_opaque, int dst_opaque,
        uint32_t dst_canvas_w, uint32_t dst_canvas_h, int32_t dst_x, int32_t dst_y,
        uint32_t src_canvas_w, uint32_t src_canvas_h, int32_t src_x, int32_t src_y,
        uint32_t mask_canvas_w, uint32_t mask_canvas_h, int32_t mask_x, int32_t mask_y,
        uint32_t w, uint32_t h) {
    struct canvas dst_accel = make_canvas(dst_canvas_w, dst_canvas_h);
    struct canvas src = make_canvas(src_canvas_w, src_canvas_h);
    struct mask_canvas mask = make_mask_canvas(mask_canvas_w, mask_canvas_h);
    struct canvas dst_oracle = make_canvas(dst_canvas_w, dst_canvas_h);
    memcpy(dst_oracle.bits, dst_accel.bits, (size_t) dst_accel.stride_bytes * dst_accel.h);

    uint32_t flags = src_opaque ? PIX_FLAG_SRC_OPAQUE : 0;
    long ret = pixop_mask(flags,
            dst_accel.bits, dst_accel.stride_bytes, dst_x, dst_y,
            src.bits, src.stride_bytes, src_x, src_y,
            mask.bits, mask.stride_bytes, mask_x, mask_y, w, h);

    uint32_t src_format = src_opaque ? PIXMAN_x8r8g8b8 : PIXMAN_a8r8g8b8;
    uint32_t dst_format = dst_opaque ? PIXMAN_x8r8g8b8 : PIXMAN_a8r8g8b8;
    oracle_composite_mask(dst_format,
            dst_oracle.bits, dst_oracle.stride_bytes, dst_x, dst_y,
            src_format, src.bits, src.stride_bytes, src_x, src_y,
            (uint32_t *) mask.bits, mask.stride_bytes, mask_x, mask_y,
            w, h, dst_canvas_w, dst_canvas_h, src_canvas_w, src_canvas_h, mask_canvas_w, mask_canvas_h);

    char label[200];
    snprintf(label, sizeof(label),
            "over_mask %ux%u dst@%d,%d(%ux%u) src@%d,%d(%ux%u) mask@%d,%d(%ux%u) opaque=%d dst_opaque=%d",
            w, h, dst_x, dst_y, dst_canvas_w, dst_canvas_h,
            src_x, src_y, src_canvas_w, src_canvas_h,
            mask_x, mask_y, mask_canvas_w, mask_canvas_h, src_opaque, dst_opaque);
    check(ret == 0, label);
    check(memcmp(dst_accel.bits, dst_oracle.bits, (size_t) dst_accel.stride_bytes * dst_accel.h) == 0, label);

    free_canvas(&dst_accel);
    free_canvas(&src);
    free_mask_canvas(&mask);
    free_canvas(&dst_oracle);
}

// Probing for ISH_SYS_PIXOP is not simply "call it and read errno": AOK
// answers a syscall number it doesn't know with SIGSYS, not real Linux's
// ENOSYS, and 0xacc1 is unknown to any guest ABI the emulator hasn't wired it
// into (it was arm64/riscv64-only until the x86 ABIs were added alongside this
// test change). An uncaught SIGSYS kills the process mid-probe, which costs
// more than the test: a death emits no "pixman_accel: PASS"/"FAIL" line at
// all, so a suite run just exits 159 with nothing recorded pointing at why.
// Catch it, and report the same clean SKIP as any other unavailable-
// accelerator case so there is always a marker.
static sigjmp_buf probe_jmp;

static void probe_sigsys(int sig) {
    (void) sig;
    siglongjmp(probe_jmp, 1);
}

static long probe_accel(void) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = probe_sigsys;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSYS, &sa, &old);

    volatile long ret = -1; // volatile: written between sigsetjmp and siglongjmp
    if (sigsetjmp(probe_jmp, 1) == 0)
        ret = pixop(PIX_OP_FILL, 0, NULL, 4, 0, 0, NULL, 0, 0, 0, 0, 0, 0);

    sigaction(SIGSYS, &old, NULL);
    return ret;
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    srand(0xC0FFEE);

    long probe = probe_accel();
    if (probe != 0) {
        printf("pixman_accel: SKIP (accelerator unavailable, syscall ret %ld -- "
               "need a build with the accelerator + ISH_PIX_ACCEL=1)\n", probe);
        return 0;
    }
    if (!load_pixman()) {
        printf("pixman_accel: SKIP (libpixman-1 not found on this rootfs)\n");
        return 0;
    }

    // FILL: small tight rect, padded canvas with offset origin, multi-page
    // canvas (large enough that several rows individually span >1 host
    // page), edge values (0x00000000, 0xffffffff), 1x1 degenerate rect.
    test_fill(4, 4, 0, 0, 4, 4, 0x11223344u);
    test_fill(16, 16, 3, 2, 5, 6, 0xffffffffu);
    test_fill(16, 16, 3, 2, 5, 6, 0x00000000u);
    test_fill(2000, 600, 17, 11, 1900, 500, 0xdeadbeefu); // several MB, many pages/rows
    test_fill(8, 8, 0, 0, 1, 1, 0xaabbccddu);

    // COPY (SRC): tight, offset+padded, cross-page, format-irrelevant (COPY
    // is purely mechanical so src_opaque doesn't change its behavior --
    // exercised once for completeness, real semantic variation is OVER's).
    test_composite(PIXMAN_OP_SRC, 0, 0, 8, 8, 0, 0, 8, 8, 0, 0, 8, 8);
    test_composite(PIXMAN_OP_SRC, 0, 0, 20, 20, 4, 3, 20, 20, 2, 5, 10, 9);
    test_composite(PIXMAN_OP_SRC, 0, 0, 1500, 400, 13, 27, 1500, 400, 5, 9, 1400, 350);

    // OVER: both src formats (real alpha, and forced-opaque), tight and
    // offset+padded and cross-page geometries, and a run of purely random
    // pixel data (the differential fuzz -- canvases are already randomly
    // initialized by make_canvas, so this IS that fuzz for every case above
    // too; these two extra rounds add larger random rects specifically).
    test_composite(PIXMAN_OP_OVER, 0, 0, 8, 8, 0, 0, 8, 8, 0, 0, 8, 8);
    test_composite(PIXMAN_OP_OVER, 1, 0, 8, 8, 0, 0, 8, 8, 0, 0, 8, 8);
    test_composite(PIXMAN_OP_OVER, 0, 0, 24, 24, 5, 4, 24, 24, 3, 2, 12, 11);
    test_composite(PIXMAN_OP_OVER, 1, 0, 24, 24, 5, 4, 24, 24, 3, 2, 12, 11);
    test_composite(PIXMAN_OP_OVER, 0, 0, 1280, 720, 0, 0, 1280, 720, 0, 0, 1280, 720); // full HD-ish frame
    for (int i = 0; i < 30; i++) {
        uint32_t w = 1 + (unsigned) rand() % 300, h = 1 + (unsigned) rand() % 300;
        uint32_t cw = w + 1 + (unsigned) rand() % 50, ch = h + 1 + (unsigned) rand() % 50;
        int32_t x = (int32_t) ((unsigned) rand() % (cw - w + 1));
        int32_t y = (int32_t) ((unsigned) rand() % (ch - h + 1));
        test_composite(PIXMAN_OP_OVER, rand() & 1, 0, cw, ch, x, y, cw, ch, x, y, w, h);
    }

    // x8r8g8b8-as-DST (foot's terminal surface / opaque window content):
    // same OVER/COPY coverage as above, but the ORACLE's dst image is now
    // PIXMAN_x8r8g8b8. Confirms the empirical mint finding that pixman
    // treats x8r8g8b8 dst's top byte as an ordinary alpha byte during
    // compositing (garbage in/garbage out, same math as a8r8g8b8) -- no
    // kernel-side change was needed, only widening what the guest shim
    // accepts as dst_fmt.
    test_composite(PIXMAN_OP_SRC, 0, 1, 8, 8, 0, 0, 8, 8, 0, 0, 8, 8);
    test_composite(PIXMAN_OP_OVER, 0, 1, 8, 8, 0, 0, 8, 8, 0, 0, 8, 8);
    test_composite(PIXMAN_OP_OVER, 1, 1, 8, 8, 0, 0, 8, 8, 0, 0, 8, 8);
    test_composite(PIXMAN_OP_OVER, 0, 1, 24, 24, 5, 4, 24, 24, 3, 2, 12, 11);
    test_composite(PIXMAN_OP_OVER, 1, 1, 24, 24, 5, 4, 24, 24, 3, 2, 12, 11);
    test_composite(PIXMAN_OP_OVER, 0, 1, 1280, 720, 0, 0, 1280, 720, 0, 0, 1280, 720);
    for (int i = 0; i < 30; i++) {
        uint32_t w = 1 + (unsigned) rand() % 300, h = 1 + (unsigned) rand() % 300;
        uint32_t cw = w + 1 + (unsigned) rand() % 50, ch = h + 1 + (unsigned) rand() % 50;
        int32_t x = (int32_t) ((unsigned) rand() % (cw - w + 1));
        int32_t y = (int32_t) ((unsigned) rand() % (ch - h + 1));
        test_composite(PIXMAN_OP_OVER, rand() & 1, 1, cw, ch, x, y, cw, ch, x, y, w, h);
    }

    // OVER_MASK_A8: tight and offset+padded geometries, both src formats,
    // an all-same-origin case (the common glyph-blit shape: src/mask share
    // the dst's own drawing coordinates) and independently-offset canvases,
    // plus random fuzz.
    test_composite_mask(0, 0, 8, 8, 0, 0, 8, 8, 0, 0, 8, 8, 0, 0, 8, 8);
    test_composite_mask(1, 0, 8, 8, 0, 0, 8, 8, 0, 0, 8, 8, 0, 0, 8, 8);
    test_composite_mask(0, 0, 24, 24, 5, 4, 24, 24, 3, 2, 24, 24, 1, 6, 12, 11);
    test_composite_mask(1, 0, 24, 24, 5, 4, 24, 24, 3, 2, 24, 24, 1, 6, 12, 11);
    test_composite_mask(0, 0, 1280, 720, 0, 0, 1280, 720, 0, 0, 1280, 720, 0, 0, 1280, 720);
    for (int i = 0; i < 30; i++) {
        uint32_t w = 1 + (unsigned) rand() % 300, h = 1 + (unsigned) rand() % 300;
        uint32_t cw = w + 1 + (unsigned) rand() % 50, ch = h + 1 + (unsigned) rand() % 50;
        int32_t x = (int32_t) ((unsigned) rand() % (cw - w + 1));
        int32_t y = (int32_t) ((unsigned) rand() % (ch - h + 1));
        test_composite_mask(rand() & 1, 0, cw, ch, x, y, cw, ch, x, y, cw, ch, x, y, w, h);
    }

    // x8r8g8b8-as-DST for the mask path too -- this is the actual foot/glyph
    // shape (opaque terminal surface + a8 glyph mask).
    test_composite_mask(0, 1, 8, 8, 0, 0, 8, 8, 0, 0, 8, 8, 0, 0, 8, 8);
    test_composite_mask(1, 1, 8, 8, 0, 0, 8, 8, 0, 0, 8, 8, 0, 0, 8, 8);
    test_composite_mask(0, 1, 24, 24, 5, 4, 24, 24, 3, 2, 24, 24, 1, 6, 12, 11);
    test_composite_mask(1, 1, 24, 24, 5, 4, 24, 24, 3, 2, 24, 24, 1, 6, 12, 11);
    test_composite_mask(0, 1, 1280, 720, 0, 0, 1280, 720, 0, 0, 1280, 720, 0, 0, 1280, 720);
    for (int i = 0; i < 30; i++) {
        uint32_t w = 1 + (unsigned) rand() % 300, h = 1 + (unsigned) rand() % 300;
        uint32_t cw = w + 1 + (unsigned) rand() % 50, ch = h + 1 + (unsigned) rand() % 50;
        int32_t x = (int32_t) ((unsigned) rand() % (cw - w + 1));
        int32_t y = (int32_t) ((unsigned) rand() % (ch - h + 1));
        test_composite_mask(rand() & 1, 1, cw, ch, x, y, cw, ch, x, y, cw, ch, x, y, w, h);
    }

    // Mask overlapping dst must be DECLINED too (same reasoning as src/dst
    // overlap -- the 3-image walk has no defined behavior for it).
    {
        struct canvas dst = make_canvas(16, 16);
        struct canvas src = make_canvas(16, 16);
        long ret = pixop_mask(0, dst.bits, dst.stride_bytes, 0, 0,
                src.bits, src.stride_bytes, 0, 0,
                dst.bits, dst.stride_bytes, 0, 0, 8, 8);
        check(ret != 0, "mask overlapping dst is declined, not silently wrong");
        free_canvas(&dst);
        free_canvas(&src);
    }

    // Self-overlap must be DECLINED (a negative return), never a wrong
    // result: same buffer used as both src and dst with overlapping rects.
    {
        struct canvas c = make_canvas(16, 16);
        long ret = pixop(PIX_OP_COPY, 0, c.bits, c.stride_bytes, 2, 2,
                c.bits, c.stride_bytes, 0, 0, 8, 8, 0);
        check(ret != 0, "self-overlapping copy is declined, not silently wrong");
        free_canvas(&c);
    }
    // Oversized request must be DECLINED, not attempted.
    {
        long ret = pixop(PIX_OP_FILL, 0, (void *) 0x1000, 4, 0, 0, NULL, 0, 0, 0,
                20000, 20000, 0); // 400M pixels, over ISH_PIX_MAX_PIXELS
        check(ret != 0, "oversized request is declined");
    }
    // Misaligned stride must be DECLINED (never a torn pixel).
    {
        struct canvas c = make_canvas(8, 8);
        long ret = pixop(PIX_OP_FILL, 0, c.bits, 3 /* not a multiple of 4 */, 0, 0,
                NULL, 0, 0, 0, 2, 2, 0);
        check(ret != 0, "misaligned stride is declined");
        free_canvas(&c);
    }

    return finish_suite("pixman_accel");
}
