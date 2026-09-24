// iSH pixman accelerator delivery shim: an LD_PRELOAD library interposing
// pixman's public API (pixman has no provider/plugin mechanism, unlike
// OpenSSL) to route eligible composite/fill calls through the host-native
// kernel accelerator (kernel/ish_accel_pix.c, ISH_SYS_PIXOP = 0xacc1)
// instead of running pixman's own C implementation instruction-by-
// instruction under emulation. See pixman_accel_plan.md for the full
// design; this is Phase 2.
//
// SAFETY CONTRACT: every interposed function calls straight through to the
// REAL pixman implementation (dlsym RTLD_NEXT) whenever the call doesn't
// exactly match a shape the accelerator covers (see README.md: 32bpp
// a8r8g8b8/x8r8g8b8 destinations, bits or solid sources, SRC and OVER, OVER
// through an a8 mask, a destination clip region, fills, blits). Nothing here
// approximates -- only "accelerate bit-exactly" or "decline to real pixman"
// are possible outcomes, exactly like the kernel side, and every shape is
// checked byte for byte against real pixman by tests/manual/pixman_shim.c.
// Only PUBLIC pixman accessors are used to inspect an image's state; the
// properties pixman has no getter for (transform/repeat/filter/alpha-map/
// clip region and its rectangles) are shadow-tracked
// by interposing their SETTERS, with the shadow entry for an image
// invalidated the moment real pixman_image_unref() reports the image was
// actually freed (verified empirically: it returns nonzero exactly on the
// unref that drops the refcount to zero, never before) -- so a later
// malloc() reusing the same address can never inherit stale shadow state.
// An image's KIND (bits, solid fill, or one of the gradients) has no getter
// either -- pixman_image_get_format() answers PIXMAN_null for all four
// non-bits kinds -- so the create_* constructors for those are interposed
// too and record it the same way.
//
// Fails closed everywhere: probing ISH_SYS_PIXOP happens once at load; on
// ENOSYS (stock iSH, real Linux, or the accelerator simply disabled) every
// interposed function becomes a pure pass-through for the rest of the
// process's life. A malloc failure while shadow-tracking an image is
// likewise treated as "assume the worst" (mark the image as not simple)
// rather than losing track of a property.
//
// Environment:
//   ISH_PIXMAN_STATS=1      count accelerated/declined calls, pixels, decline
//                           shapes and time spent inside pixman; dump to
//                           stderr at exit.
//   ISH_PIXMAN_STATS=/dir   the same, written to /dir/pixman-stats.<pid>,
//                           refreshed about once a second -- so a process
//                           that is killed (labwc and foot usually are, at
//                           session end) still leaves its numbers behind.
//   ISH_PIXMAN_SHIM_OFF=1   never accelerate, but keep counting and timing:
//                           the "off" arm of an A/B with the same instrument.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stddef.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
// The system's pixman-1 headers when they are installed (build-shim.sh adds
// -I/usr/include/pixman-1), else the copy vendored beside this file -- the
// shim uses only long-stable public API, so pixman 0.44's header serves any
// libpixman-1 it meets.
#if defined(__has_include)
#  if __has_include(<pixman.h>)
#    include <pixman.h>
#  else
#    include "pixman.h"
#  endif
#else
#  include <pixman.h>
#endif

extern long syscall(long, ...);
#define ISH_SYS_PIXOP 0xacc1
enum { PIX_OP_FILL = 0, PIX_OP_COPY = 1, PIX_OP_OVER = 2, PIX_OP_OVER_MASK = 3, PIX_OP_COPY_SET_ALPHA = 4 };
enum { PIX_FLAG_SRC_OPAQUE = 1u << 0, PIX_FLAG_DST_OPAQUE = 1u << 1, PIX_FLAG_SRC_SOLID = 1u << 2 };

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

// ---- accelerator availability probe -------------------------------------
static int g_accel_available = -1;
static pthread_once_t g_probe_once = PTHREAD_ONCE_INIT;

static void probe_accel(void) {
    if (getenv("ISH_PIXMAN_SHIM_OFF") != NULL) {
        g_accel_available = 0;
        return;
    }
    // A width=0 FILL is a defined no-op in the kernel (returns 0 success)
    // whenever the accelerator is enabled and passed self-test, and ENOSYS
    // otherwise (disabled, or the syscall doesn't exist at all -- real
    // Linux, or stock iSH). dst_stride must still satisfy the kernel's own
    // alignment check even though width=0 short-circuits before it's used.
    long ret = pixop(PIX_OP_FILL, 0, NULL, 4, 0, 0, NULL, 0, 0, 0, 0, 0, 0);
    g_accel_available = (ret == 0) ? 1 : 0;
}

static int accel_available(void) {
    pthread_once(&g_probe_once, probe_accel);
    return g_accel_available == 1;
}

// ---- real pixman entry points, resolved lazily --------------------------
typedef void (*composite32_fn)(pixman_op_t, pixman_image_t *, pixman_image_t *, pixman_image_t *,
        int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
typedef void (*composite_fn)(pixman_op_t, pixman_image_t *, pixman_image_t *, pixman_image_t *,
        int16_t, int16_t, int16_t, int16_t, int16_t, int16_t, uint16_t, uint16_t);
typedef pixman_bool_t (*fill_fn)(uint32_t *, int, int, int, int, int, int, uint32_t);
typedef pixman_bool_t (*blt_fn)(uint32_t *, uint32_t *, int, int, int, int, int, int, int, int, int, int);
typedef pixman_bool_t (*fill_boxes_fn)(pixman_op_t, pixman_image_t *, const pixman_color_t *,
        int, const pixman_box32_t *);
typedef pixman_bool_t (*fill_rectangles_fn)(pixman_op_t, pixman_image_t *, const pixman_color_t *,
        int, const pixman_rectangle16_t *);
typedef void (*glyphs_fn)(pixman_op_t, pixman_image_t *, pixman_image_t *, pixman_format_code_t,
        int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
        pixman_glyph_cache_t *, int, const pixman_glyph_t *);
typedef void (*glyphs_no_mask_fn)(pixman_op_t, pixman_image_t *, pixman_image_t *,
        int32_t, int32_t, int32_t, int32_t, pixman_glyph_cache_t *, int, const pixman_glyph_t *);
typedef void (*traps_fn)(pixman_op_t, pixman_image_t *, pixman_image_t *, pixman_format_code_t,
        int, int, int, int, int, const pixman_trapezoid_t *);
typedef void (*tris_fn)(pixman_op_t, pixman_image_t *, pixman_image_t *, pixman_format_code_t,
        int, int, int, int, int, const pixman_triangle_t *);
typedef void (*add_traps_fn)(pixman_image_t *, int16_t, int16_t, int, const pixman_trap_t *);
typedef void (*add_trapezoids_fn)(pixman_image_t *, int16_t, int, int, const pixman_trapezoid_t *);
typedef void (*rasterize_trapezoid_fn)(pixman_image_t *, const pixman_trapezoid_t *, int, int);
typedef pixman_image_t *(*create_solid_fn)(const pixman_color_t *);
typedef pixman_image_t *(*create_linear_fn)(const pixman_point_fixed_t *, const pixman_point_fixed_t *,
        const pixman_gradient_stop_t *, int);
typedef pixman_image_t *(*create_radial_fn)(const pixman_point_fixed_t *, const pixman_point_fixed_t *,
        pixman_fixed_t, pixman_fixed_t, const pixman_gradient_stop_t *, int);
typedef pixman_image_t *(*create_conical_fn)(const pixman_point_fixed_t *, pixman_fixed_t,
        const pixman_gradient_stop_t *, int);
typedef pixman_bool_t (*unref_fn)(pixman_image_t *);
typedef pixman_bool_t (*set_transform_fn)(pixman_image_t *, const pixman_transform_t *);
typedef void (*set_repeat_fn)(pixman_image_t *, pixman_repeat_t);
typedef pixman_bool_t (*set_filter_fn)(pixman_image_t *, pixman_filter_t, const pixman_fixed_t *, int);
typedef void (*set_alpha_map_fn)(pixman_image_t *, pixman_image_t *, int16_t, int16_t);
typedef pixman_bool_t (*set_clip_region_fn)(pixman_image_t *, const pixman_region16_t *);
typedef pixman_bool_t (*set_clip_region32_fn)(pixman_image_t *, const pixman_region32_t *);
typedef void (*set_has_client_clip_fn)(pixman_image_t *, pixman_bool_t);
typedef pixman_bool_t (*get_component_alpha_fn)(pixman_image_t *);
typedef pixman_box32_t *(*region32_rectangles_fn)(const pixman_region32_t *, int *);
typedef pixman_box16_t *(*region16_rectangles_fn)(const pixman_region16_t *, int *);
typedef uint32_t *(*get_data_fn)(pixman_image_t *);
typedef int (*get_stride_fn)(pixman_image_t *);
typedef int (*get_width_fn)(pixman_image_t *);
typedef int (*get_height_fn)(pixman_image_t *);
typedef pixman_format_code_t (*get_format_fn)(pixman_image_t *);

#define RESOLVE(var, type, name) \
    static type var; \
    if (var == NULL) var = (type) dlsym(RTLD_NEXT, name)

// ---- per-image shadow state (properties pixman has no public getter for)
// A chained hash table keyed by the pixman_image_t pointer. An image
// absent from the table is, by construction, a bits image still at pixman's
// own defaults (no transform, REPEAT_NONE, NEAREST filter, no alpha map, no
// clip) -- entries are only ever created lazily by a setter or a non-bits
// constructor, and removed the instant real pixman_image_unref() reports
// the image was actually freed, so a stale entry can never outlive the
// object it describes.
enum { KIND_BITS = 0, KIND_SOLID, KIND_LINEAR, KIND_RADIAL, KIND_CONICAL };
struct image_state {
    pixman_image_t *key;
    int kind;
    uint32_t solid;        // KIND_SOLID: pixman's color_32 for the fill colour
    int has_transform;
    int has_alpha_map;
    int has_clip;          // a clip region is set
    int client_clip;       // pixman_image_set_has_client_clip(TRUE)
    // The clip region's rectangles, copied when it is set: the only way to
    // honour a destination clip without asking pixman, which has no getter
    // for it. NULL with has_clip means they could not be copied (declines).
    pixman_box32_t *clip_boxes;
    int n_clip_boxes;
    int clip_known;        // clip_boxes/n_clip_boxes describe the region
    int nontrivial_filter; // filter set to anything other than NEAREST
    pixman_repeat_t repeat;
    struct image_state *next;
};
#define STATE_BUCKETS 256
static struct image_state *g_buckets[STATE_BUCKETS];
// A reader-writer lock: every composite looks images up (several threads at
// once in foot, which renders on a pool), and only setters, constructors and
// the freeing unref write.
static pthread_rwlock_t g_state_lock = PTHREAD_RWLOCK_INITIALIZER;
#define STATE_READ() pthread_rwlock_rdlock(&g_state_lock)
#define STATE_WRITE() pthread_rwlock_wrlock(&g_state_lock)
#define STATE_UNLOCK() pthread_rwlock_unlock(&g_state_lock)

static unsigned state_hash(pixman_image_t *img) {
    uintptr_t v = (uintptr_t) img;
    return (unsigned) ((v >> 4) ^ (v >> 13)) % STATE_BUCKETS;
}

// Caller must hold g_state_lock.
static struct image_state *state_find(pixman_image_t *img) {
    struct image_state *s = g_buckets[state_hash(img)];
    while (s != NULL && s->key != img)
        s = s->next;
    return s;
}

// Caller must hold g_state_lock. Returns NULL on allocation failure --
// callers treat that as "assume not simple" (fail closed), never silently
// dropping a property that was actually set.
static struct image_state *state_get_or_create(pixman_image_t *img) {
    struct image_state *s = state_find(img);
    if (s != NULL)
        return s;
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return NULL;
    s->key = img;
    s->repeat = PIXMAN_REPEAT_NONE;
    unsigned h = state_hash(img);
    s->next = g_buckets[h];
    g_buckets[h] = s;
    return s;
}

static void state_remove(pixman_image_t *img) {
    unsigned h = state_hash(img);
    struct image_state **pp = &g_buckets[h];
    while (*pp != NULL) {
        if ((*pp)->key == img) {
            struct image_state *dead = *pp;
            *pp = dead->next;
            free(dead->clip_boxes);
            free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
}

// Records a non-bits image's kind at construction. An allocation failure
// here would leave the image looking like a plain bits image, which is not
// failing closed -- so it is remembered in g_kind_lost and every later
// composite declines instead.
static int g_kind_lost;
static void state_set_kind(pixman_image_t *img, int kind, uint32_t solid) {
    if (img == NULL)
        return;
    STATE_WRITE();
    struct image_state *s = state_get_or_create(img);
    if (s != NULL) {
        s->kind = kind;
        s->solid = solid;
    } else {
        g_kind_lost = 1;
    }
    STATE_UNLOCK();
}

static int image_kind(pixman_image_t *img) {
    STATE_READ();
    struct image_state *s = state_find(img);
    int kind = s != NULL ? s->kind : KIND_BITS;
    STATE_UNLOCK();
    return kind;
}

// ---- ISH_PIXMAN_STATS accounting ------------------------------------------
enum {
    STAT_ACCEL_COMPOSITE, STAT_ACCEL_FILL, STAT_ACCEL_MASK,
    STAT_DECLINE_MASK_FORMAT, STAT_DECLINE_OP, STAT_DECLINE_FORMAT,
    STAT_DECLINE_TRANSFORM, STAT_DECLINE_REPEAT, STAT_DECLINE_FILTER,
    STAT_DECLINE_ALPHA_MAP, STAT_DECLINE_CLIP, STAT_DECLINE_COMPONENT_ALPHA,
    STAT_DECLINE_BOUNDS, STAT_DECLINE_UNAVAILABLE, STAT_DECLINE_SYSCALL,
    STAT_DECLINE_FILL_BPP, STAT_DECLINE_SRC_KIND, STAT_DECLINE_MASK_KIND,
    STAT_DECLINE_KIND_LOST, STAT_DECLINE_SMALL,
    STAT_COUNT,
};
static const char *const stat_names[STAT_COUNT] = {
    "accelerated-composite", "accelerated-fill", "accelerated-mask",
    "decline-mask-format", "decline-op", "decline-format",
    "decline-transform", "decline-repeat", "decline-filter",
    "decline-alpha-map", "decline-clip", "decline-component-alpha",
    "decline-bounds", "decline-accel-unavailable", "decline-syscall-refused",
    "decline-fill-bpp", "decline-src-kind", "decline-mask-kind",
    "decline-kind-lost", "decline-small",
};

// Outermost interposed entry points, for the time-in-pixman figure. Nested
// calls (fill_rectangles -> fill_boxes -> pixman_fill, when pixman's own
// internal calls go through the PLT) are charged to the outermost one only.
enum {
    ENTRY_COMPOSITE32, ENTRY_COMPOSITE, ENTRY_FILL, ENTRY_BLT, ENTRY_FILL_BOXES,
    ENTRY_FILL_RECTANGLES, ENTRY_GLYPHS, ENTRY_GLYPHS_NO_MASK, ENTRY_TRAPEZOIDS,
    ENTRY_TRIANGLES, ENTRY_ADD_TRAPS, ENTRY_ADD_TRAPEZOIDS, ENTRY_RASTERIZE_TRAPEZOID,
    ENTRY_COUNT,
};
static const char *const entry_names[ENTRY_COUNT] = {
    "composite32", "composite", "fill", "blt", "fill_boxes",
    "fill_rectangles", "composite_glyphs", "composite_glyphs_no_mask", "composite_trapezoids",
    "composite_triangles", "add_traps", "add_trapezoids", "rasterize_trapezoid",
};

struct counter { unsigned long calls; unsigned long long pixels; };
static struct counter g_stats[STAT_COUNT];
static struct counter g_entry[ENTRY_COUNT]; // .pixels holds nanoseconds here

// Every composite, keyed by its whole shape, so the list says what to build
// next: (reasons, op, src, mask, dst) where reasons is the set of
// STAT_DECLINE_* bits that refused it (0 = accelerated) and src/mask are a
// format code for a bits image or a small KIND_* number for anything else.
// Each shape carries the time its calls spent inside pixman, which is what
// ranks it. A full table just stops recording new shapes (counted in
// g_shapes_dropped).
#define OP_FILL 0x100       // pseudo-ops for entry points with no pixman_op_t:
#define OP_BLT 0x101        //   pixman_fill, pixman_blt, and the fills the
#define OP_FILL_BOXES 0x102 //   shim's own fill_boxes makes
struct shape {
    int used;
    unsigned reasons;
    int op;
    uint32_t src, mask, dst;
    struct counter n;
    unsigned long long ns;
};
#define SHAPE_SLOTS 512
static struct shape g_shapes[SHAPE_SLOTS];
static unsigned long g_shapes_dropped;
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *g_stats_dir;
static int stats_enabled(void) {
    static int enabled = -1;
    if (enabled == -1) {
        const char *v = getenv("ISH_PIXMAN_STATS");
        if (v != NULL && v[0] == '/')
            g_stats_dir = v;
        enabled = v != NULL ? 1 : 0;
    }
    return enabled;
}

static void note(int stat, uint64_t pixels) {
    if (!stats_enabled())
        return;
    __atomic_fetch_add(&g_stats[stat].calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_stats[stat].pixels, pixels, __ATOMIC_RELAXED);
}

// A bits image is described by its format; anything else by KIND_*.
static uint32_t describe(pixman_image_t *img) {
    if (img == NULL)
        return 0;
    int kind = image_kind(img);
    if (kind != KIND_BITS)
        return (uint32_t) kind;
    RESOLVE(get_format, get_format_fn, "pixman_image_get_format");
    return (uint32_t) get_format(img);
}

static void note_shape_raw(unsigned reasons, int op, uint32_t s, uint32_t m, uint32_t d,
        uint64_t pixels, uint64_t ns) {
    unsigned h = (reasons * 31u + (unsigned) op * 7u + s * 13u + m * 17u + d) % SHAPE_SLOTS;
    pthread_mutex_lock(&g_stats_lock);
    for (unsigned i = 0; i < SHAPE_SLOTS; i++) {
        struct shape *e = &g_shapes[(h + i) % SHAPE_SLOTS];
        if (!e->used) {
            *e = (struct shape) { .used = 1, .reasons = reasons, .op = op, .src = s, .mask = m, .dst = d };
        } else if (e->reasons != reasons || e->op != op || e->src != s || e->mask != m || e->dst != d) {
            continue;
        }
        e->n.calls++;
        e->n.pixels += pixels;
        e->ns += ns;
        pthread_mutex_unlock(&g_stats_lock);
        return;
    }
    g_shapes_dropped++;
    pthread_mutex_unlock(&g_stats_lock);
}

static void note_shape(unsigned reasons, pixman_op_t op, pixman_image_t *src, pixman_image_t *mask,
        pixman_image_t *dst, uint64_t pixels, uint64_t ns) {
    note_shape_raw(reasons, op, describe(src), describe(mask), describe(dst), pixels, ns);
}

// The lowest-numbered reason in a set is the one the per-reason counters use.
static int primary_reason(unsigned reasons) {
    return __builtin_ctz(reasons);
}

static const char *describe_name(uint32_t d, char *buf, size_t len) {
    switch (d) {
        case 0: return "-";
        case KIND_SOLID: return "solid";
        case KIND_LINEAR: return "linear";
        case KIND_RADIAL: return "radial";
        case KIND_CONICAL: return "conical";
        case PIXMAN_a8r8g8b8: return "a8r8g8b8";
        case PIXMAN_x8r8g8b8: return "x8r8g8b8";
        case PIXMAN_a8b8g8r8: return "a8b8g8r8";
        case PIXMAN_x8b8g8r8: return "x8b8g8r8";
        case PIXMAN_b8g8r8a8: return "b8g8r8a8";
        case PIXMAN_r5g6b5: return "r5g6b5";
        case PIXMAN_a2r10g10b10: return "a2r10g10b10";
        case PIXMAN_x2r10g10b10: return "x2r10g10b10";
        case PIXMAN_a8: return "a8";
        case PIXMAN_a1: return "a1";
    }
    snprintf(buf, len, "fmt:%08x", d);
    return buf;
}

static const char *op_name(int op, char *buf, size_t len) {
    switch (op) {
        case PIXMAN_OP_CLEAR: return "CLEAR";
        case PIXMAN_OP_SRC: return "SRC";
        case PIXMAN_OP_DST: return "DST";
        case PIXMAN_OP_OVER: return "OVER";
        case PIXMAN_OP_OVER_REVERSE: return "OVER_REVERSE";
        case PIXMAN_OP_IN: return "IN";
        case PIXMAN_OP_IN_REVERSE: return "IN_REVERSE";
        case PIXMAN_OP_OUT: return "OUT";
        case PIXMAN_OP_OUT_REVERSE: return "OUT_REVERSE";
        case PIXMAN_OP_ATOP: return "ATOP";
        case PIXMAN_OP_XOR: return "XOR";
        case PIXMAN_OP_ADD: return "ADD";
        case PIXMAN_OP_SATURATE: return "SATURATE";
    }
    snprintf(buf, len, "op%d", op);
    return buf;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000u + (uint64_t) ts.tv_nsec;
}

static void write_stats(FILE *f) {
    char comm[32] = "?";
    int fd = open("/proc/self/comm", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, comm, sizeof(comm) - 1);
        comm[n > 0 ? n : 0] = '\0';
        char *nl = strchr(comm, '\n');
        if (nl != NULL)
            *nl = '\0';
        close(fd);
    }
    fprintf(f, "[ish-pixman-shim] pid=%d comm=%s accel=%s\n", (int) getpid(), comm,
            g_accel_available == 1 ? "on" : g_accel_available == 0 ? "off" : "unprobed");
    for (int i = 0; i < ENTRY_COUNT; i++) {
        if (g_entry[i].calls != 0)
            fprintf(f, "[ish-pixman-shim]   time %-26s %8lu calls %10.1f ms\n", entry_names[i],
                    g_entry[i].calls, g_entry[i].pixels / 1e6);
    }
    for (int i = 0; i < STAT_COUNT; i++) {
        if (g_stats[i].calls != 0)
            fprintf(f, "[ish-pixman-shim]   %-28s %8lu calls %12llu px\n", stat_names[i],
                    g_stats[i].calls, g_stats[i].pixels);
    }
    pthread_mutex_lock(&g_stats_lock);
    for (int i = 0; i < SHAPE_SLOTS; i++) {
        struct shape *e = &g_shapes[i];
        if (!e->used)
            continue;
        char b0[16], b1[16], b2[16], b3[16], why[256] = "";
        if (e->reasons == 0)
            snprintf(why, sizeof(why), "accelerated");
        for (int r = 0; r < STAT_COUNT; r++) {
            if (e->reasons & (1u << r)) {
                size_t len = strlen(why);
                snprintf(why + len, sizeof(why) - len, "%s%s", len ? "+" : "",
                        stat_names[r] + (strncmp(stat_names[r], "decline-", 8) == 0 ? 8 : 0));
            }
        }
        fprintf(f, "[ish-pixman-shim]   shape %-28s %-5s src=%-9s mask=%-6s dst=%-9s %8lu calls %12llu px %9.1f ms\n",
                why, e->op == OP_FILL ? "FILL" : e->op == OP_BLT ? "BLT" : e->op == OP_FILL_BOXES ? "BOXES" :
                op_name(e->op, b0, sizeof(b0)),
                describe_name(e->src, b1, sizeof(b1)), describe_name(e->mask, b2, sizeof(b2)),
                e->op >= OP_FILL ? (snprintf(b3, sizeof(b3), "%ubpp", e->dst), b3) : describe_name(e->dst, b3, sizeof(b3)),
                e->n.calls, e->n.pixels, e->ns / 1e6);
    }
    if (g_shapes_dropped != 0)
        fprintf(f, "[ish-pixman-shim]   shapes-dropped %lu\n", g_shapes_dropped);
    pthread_mutex_unlock(&g_stats_lock);
}

static void write_stats_file(void) {
    char path[512], tmp[520];
    snprintf(path, sizeof(path), "%s/pixman-stats.%d", g_stats_dir, (int) getpid());
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f == NULL)
        return;
    write_stats(f);
    fclose(f);
    rename(tmp, path);
}

static uint64_t g_last_flush;
static pthread_mutex_t g_flush_lock = PTHREAD_MUTEX_INITIALIZER;
static void maybe_flush(uint64_t now) {
    if (g_stats_dir == NULL || now - __atomic_load_n(&g_last_flush, __ATOMIC_RELAXED) < 1000000000u)
        return;
    if (pthread_mutex_trylock(&g_flush_lock) != 0)
        return;
    __atomic_store_n(&g_last_flush, now, __ATOMIC_RELAXED);
    write_stats_file();
    pthread_mutex_unlock(&g_flush_lock);
}

static __thread int t_depth;
struct timing { int outer; uint64_t t0; };
static struct timing timing_begin(void) {
    struct timing t = { 0, 0 };
    if (stats_enabled() && t_depth++ == 0) {
        t.outer = 1;
        t.t0 = now_ns();
    }
    return t;
}
static void timing_end(int entry, struct timing t) {
    if (!stats_enabled())
        return;
    t_depth--;
    if (!t.outer)
        return;
    uint64_t now = now_ns();
    __atomic_fetch_add(&g_entry[entry].calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_entry[entry].pixels, now - t.t0, __ATOMIC_RELAXED);
    maybe_flush(now);
}

__attribute__((destructor))
static void dump_stats(void) {
    if (!stats_enabled())
        return;
    // Most processes in a session never call pixman at all.
    int any = 0;
    for (int i = 0; i < ENTRY_COUNT; i++)
        any |= g_entry[i].calls != 0;
    if (!any)
        return;
    if (g_stats_dir != NULL)
        write_stats_file();
    else
        write_stats(stderr);
}

// ---- interposed constructors for non-bits images ---------------------------

pixman_image_t *pixman_image_create_solid_fill(const pixman_color_t *color) {
    RESOLVE(real, create_solid_fn, "pixman_image_create_solid_fill");
    pixman_image_t *img = real(color);
    // pixman's color_to_uint32: each 16-bit channel's high byte, truncated
    // (not rounded) -- what its fast paths composite with; checked against
    // real pixman with channels whose low byte would round up.
    uint32_t c32 = ((uint32_t) (color->alpha >> 8) << 24) | ((uint32_t) (color->red >> 8) << 16) |
            ((uint32_t) color->green & 0xff00) | ((uint32_t) color->blue >> 8);
    state_set_kind(img, KIND_SOLID, c32);
    return img;
}

pixman_image_t *pixman_image_create_linear_gradient(const pixman_point_fixed_t *p1, const pixman_point_fixed_t *p2,
        const pixman_gradient_stop_t *stops, int n_stops) {
    RESOLVE(real, create_linear_fn, "pixman_image_create_linear_gradient");
    pixman_image_t *img = real(p1, p2, stops, n_stops);
    state_set_kind(img, KIND_LINEAR, 0);
    return img;
}

pixman_image_t *pixman_image_create_radial_gradient(const pixman_point_fixed_t *inner, const pixman_point_fixed_t *outer,
        pixman_fixed_t inner_radius, pixman_fixed_t outer_radius, const pixman_gradient_stop_t *stops, int n_stops) {
    RESOLVE(real, create_radial_fn, "pixman_image_create_radial_gradient");
    pixman_image_t *img = real(inner, outer, inner_radius, outer_radius, stops, n_stops);
    state_set_kind(img, KIND_RADIAL, 0);
    return img;
}

pixman_image_t *pixman_image_create_conical_gradient(const pixman_point_fixed_t *center, pixman_fixed_t angle,
        const pixman_gradient_stop_t *stops, int n_stops) {
    RESOLVE(real, create_conical_fn, "pixman_image_create_conical_gradient");
    pixman_image_t *img = real(center, angle, stops, n_stops);
    state_set_kind(img, KIND_CONICAL, 0);
    return img;
}

// ---- interposed state-tracking setters ----------------------------------
// Every one of these calls through to the real pixman implementation
// FIRST (so a real failure return is honored exactly as pixman intends),
// then updates the shadow table to match -- the shadow always describes
// what pixman was actually told, never what we assumed it would accept.

pixman_bool_t pixman_image_set_transform(pixman_image_t *image, const pixman_transform_t *transform) {
    RESOLVE(real, set_transform_fn, "pixman_image_set_transform");
    pixman_bool_t ret = real(image, transform);
    STATE_WRITE();
    struct image_state *s = state_get_or_create(image);
    if (s != NULL)
        s->has_transform = (transform != NULL);
    STATE_UNLOCK();
    return ret;
}

void pixman_image_set_repeat(pixman_image_t *image, pixman_repeat_t repeat) {
    RESOLVE(real, set_repeat_fn, "pixman_image_set_repeat");
    real(image, repeat);
    STATE_WRITE();
    struct image_state *s = state_get_or_create(image);
    if (s != NULL)
        s->repeat = repeat;
    STATE_UNLOCK();
}

pixman_bool_t pixman_image_set_filter(pixman_image_t *image, pixman_filter_t filter,
        const pixman_fixed_t *filter_params, int n_filter_params) {
    RESOLVE(real, set_filter_fn, "pixman_image_set_filter");
    pixman_bool_t ret = real(image, filter, filter_params, n_filter_params);
    STATE_WRITE();
    struct image_state *s = state_get_or_create(image);
    if (s != NULL)
        s->nontrivial_filter = (filter != PIXMAN_FILTER_NEAREST);
    STATE_UNLOCK();
    return ret;
}

void pixman_image_set_alpha_map(pixman_image_t *image, pixman_image_t *alpha_map, int16_t x, int16_t y) {
    RESOLVE(real, set_alpha_map_fn, "pixman_image_set_alpha_map");
    real(image, alpha_map, x, y);
    STATE_WRITE();
    struct image_state *s = state_get_or_create(image);
    if (s != NULL)
        s->has_alpha_map = (alpha_map != NULL);
    STATE_UNLOCK();
}

// Records what pixman was told about an image's clip: none (region NULL, the
// set succeeded), or the region's rectangles -- copied, because pixman has no
// getter and a destination clip is what wlroots and foot put on almost every
// composite. `boxes` is a malloc'd copy the state takes over, or NULL with
// known == 0 when the set failed or the copy could not be made, which
// declines every composite onto the image until the clip is set again.
static void state_set_clip(pixman_image_t *image, int has_clip, int known,
        pixman_box32_t *boxes, int n) {
    STATE_WRITE();
    struct image_state *s = state_get_or_create(image);
    if (s != NULL) {
        free(s->clip_boxes);
        s->has_clip = has_clip;
        s->clip_known = known;
        s->clip_boxes = boxes;
        s->n_clip_boxes = n;
        boxes = NULL;
    }
    STATE_UNLOCK();
    free(boxes); // not taken: the state could not be allocated
}

pixman_bool_t pixman_image_set_clip_region(pixman_image_t *image, const pixman_region16_t *region) {
    RESOLVE(real, set_clip_region_fn, "pixman_image_set_clip_region");
    RESOLVE(rects, region16_rectangles_fn, "pixman_region_rectangles");
    pixman_bool_t ret = real(image, region);
    if (!ret) {
        state_set_clip(image, 1, 0, NULL, 0);
    } else if (region == NULL) {
        state_set_clip(image, 0, 1, NULL, 0);
    } else {
        int n = 0;
        const pixman_box16_t *b = rects != NULL ? rects(region, &n) : NULL;
        pixman_box32_t *copy = n > 0 ? malloc((size_t) n * sizeof(*copy)) : NULL;
        int known = rects != NULL && (n == 0 || copy != NULL);
        for (int i = 0; known && i < n; i++)
            copy[i] = (pixman_box32_t) { b[i].x1, b[i].y1, b[i].x2, b[i].y2 };
        state_set_clip(image, 1, known, known ? copy : NULL, known ? n : 0);
        if (!known)
            free(copy);
    }
    return ret;
}

pixman_bool_t pixman_image_set_clip_region32(pixman_image_t *image, const pixman_region32_t *region) {
    RESOLVE(real, set_clip_region32_fn, "pixman_image_set_clip_region32");
    RESOLVE(rects, region32_rectangles_fn, "pixman_region32_rectangles");
    pixman_bool_t ret = real(image, region);
    if (!ret) {
        state_set_clip(image, 1, 0, NULL, 0);
    } else if (region == NULL) {
        state_set_clip(image, 0, 1, NULL, 0);
    } else {
        int n = 0;
        const pixman_box32_t *b = rects != NULL ? rects(region, &n) : NULL;
        pixman_box32_t *copy = n > 0 ? malloc((size_t) n * sizeof(*copy)) : NULL;
        int known = rects != NULL && (n == 0 || copy != NULL);
        if (known && n > 0)
            memcpy(copy, b, (size_t) n * sizeof(*copy));
        state_set_clip(image, 1, known, known ? copy : NULL, known ? n : 0);
        if (!known)
            free(copy);
    }
    return ret;
}

// Client clip only changes how pixman clips an image used as a SOURCE (with
// source clipping on), and a source with any clip region declines anyway;
// it is recorded so that stays true even if the region is set later.
void pixman_image_set_has_client_clip(pixman_image_t *image, pixman_bool_t client_clip) {
    RESOLVE(real, set_has_client_clip_fn, "pixman_image_set_has_client_clip");
    real(image, client_clip);
    STATE_WRITE();
    struct image_state *s = state_get_or_create(image);
    if (s != NULL)
        s->client_clip = client_clip != 0;
    STATE_UNLOCK();
}

pixman_bool_t pixman_image_unref(pixman_image_t *image) {
    RESOLVE(real, unref_fn, "pixman_image_unref");
    pixman_bool_t freed = real(image);
    if (freed) {
        STATE_WRITE();
        state_remove(image);
        STATE_UNLOCK();
    }
    return freed;
}

// ---- size floors ---------------------------------------------------------
// Below these the guest's own pixman beats the syscall once several threads
// of one process are making them: each request takes that process's
// jit->lock once (kernel/user.c mem_write_prepare_rect), and a terminal like
// foot fills one-cell rectangles from several render threads at a time.
// Measured 2026-09-24 with 4 guest threads filling in parallel: 8x17 cost
// 4.0 us accelerated against 0.9 in pixman, 32x32 7.9 against 6.1, 48x48
// 8.4 against 11.4. Single-threaded the accelerator wins at every size, by
// well under a microsecond below these floors. No floor for OVER: pixman's
// own OVER costs ~6x its fill per pixel, so it is past the crossover from
// the smallest glyph up.
#define FILL_FLOOR_PIXELS 1024
#define COPY_FLOOR_PIXELS 512

// ---- accelerability check ------------------------------------------------
enum { ROLE_SRC, ROLE_MASK, ROLE_DEST };

// The rectangles of the destination a composite writes: its rect clamped to
// the image (pixman clamps to the destination's bounds) and cut by the
// image's clip region if it has one. Disjoint, because a pixman region's
// rectangles are, so one kernel request per rectangle is exactly pixman's
// result. More than DEST_BOXES_MAX of them declines.
#define DEST_BOXES_MAX 64
struct dest_boxes {
    int n;
    pixman_box32_t box[DEST_BOXES_MAX];
};

static pixman_box32_t box_intersect(pixman_box32_t a, pixman_box32_t b) {
    pixman_box32_t r = {
        a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1,
        a.x2 < b.x2 ? a.x2 : b.x2, a.y2 < b.y2 ? a.y2 : b.y2,
    };
    return r;
}

static int box_empty(pixman_box32_t b) {
    return b.x1 >= b.x2 || b.y1 >= b.y2;
}

// `rect` (x, y, w, h) clamped to the image, as a box; empty if disjoint.
static pixman_box32_t clamp_to_image(pixman_image_t *img, int32_t x, int32_t y, int32_t w, int32_t h) {
    RESOLVE(get_width, get_width_fn, "pixman_image_get_width");
    RESOLVE(get_height, get_height_fn, "pixman_image_get_height");
    int64_t x1 = x, y1 = y, x2 = (int64_t) x + w, y2 = (int64_t) y + h;
    int64_t iw = get_width(img), ih = get_height(img);
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > iw) x2 = iw;
    if (y2 > ih) y2 = ih;
    if (x2 < x1) x2 = x1;
    if (y2 < y1) y2 = y1;
    pixman_box32_t b = { (int32_t) x1, (int32_t) y1, (int32_t) x2, (int32_t) y2 };
    return b;
}

// Is [x, x+w) x [y, y+h) inside a bits image?
static int in_bounds(pixman_image_t *img, int32_t x, int32_t y, int32_t w, int32_t h) {
    RESOLVE(get_width, get_width_fn, "pixman_image_get_width");
    RESOLVE(get_height, get_height_fn, "pixman_image_get_height");
    return x >= 0 && y >= 0 &&
            (int64_t) x + w <= get_width(img) && (int64_t) y + h <= get_height(img);
}

struct image_info {
    int kind;
    uint32_t solid;
    unsigned reasons; // 1u << STAT_DECLINE_* bits
};

// One read of the shadow state per image per call: kind, solid colour, and
// every reason the image is outside the accelerator's shape in `role`. For
// the destination, `rect` (already clamped) is also cut by the clip into
// `out` while the lock is held, so the rectangles used are the ones in force.
// A source or mask with a clip region declines: pixman ignores such a clip
// unless source clipping is on, which the shim does not track.
static struct image_info image_lookup(pixman_image_t *image, int role,
        const pixman_box32_t *rect, struct dest_boxes *out) {
    RESOLVE(real_gca, get_component_alpha_fn, "pixman_image_get_component_alpha");
    struct image_info info = { KIND_BITS, 0, 0 };
    if (real_gca(image))
        info.reasons |= 1u << STAT_DECLINE_COMPONENT_ALPHA;
    if (out != NULL)
        out->n = 0;
    STATE_READ();
    struct image_state *s = state_find(image);
    if (s != NULL) {
        info.kind = s->kind;
        info.solid = s->solid;
        if (s->has_transform) info.reasons |= 1u << STAT_DECLINE_TRANSFORM;
        if (s->repeat != PIXMAN_REPEAT_NONE) info.reasons |= 1u << STAT_DECLINE_REPEAT;
        if (s->nontrivial_filter) info.reasons |= 1u << STAT_DECLINE_FILTER;
        if (s->has_alpha_map) info.reasons |= 1u << STAT_DECLINE_ALPHA_MAP;
        if (s->has_clip && (role != ROLE_DEST || !s->clip_known))
            info.reasons |= 1u << STAT_DECLINE_CLIP;
    }
    if (role == ROLE_DEST && out != NULL && info.reasons == 0 && !box_empty(*rect)) {
        if (s != NULL && s->has_clip) {
            for (int i = 0; i < s->n_clip_boxes; i++) {
                pixman_box32_t b = box_intersect(*rect, s->clip_boxes[i]);
                if (box_empty(b))
                    continue;
                if (out->n == DEST_BOXES_MAX) {
                    info.reasons |= 1u << STAT_DECLINE_CLIP;
                    break;
                }
                out->box[out->n++] = b;
            }
        } else {
            out->box[out->n++] = *rect;
        }
    }
    STATE_UNLOCK();
    return info;
}

// What the kernel is asked for, once per destination rectangle.
struct plan {
    uint32_t op, flags, solid;
    uint32_t *dst_bits, *src_bits;
    uint8_t *mask_bits;
    uint32_t dst_stride, src_stride, mask_stride;
    int nothing;       // pixman leaves the destination as it is
    uint64_t pixels;   // what the rectangles cover
    struct dest_boxes boxes;
};

static int is_32bpp_rgb(pixman_format_code_t f) {
    return f == PIXMAN_a8r8g8b8 || f == PIXMAN_x8r8g8b8;
}

// Plan a composite, or return every reason it is outside the accelerator's
// shape (1u << STAT_DECLINE_* bits). All of them, not just the first: a shape
// refused for two reasons needs both fixed before it pays.
static unsigned composite_plan(struct plan *p, pixman_op_t op, pixman_image_t *src, pixman_image_t *mask,
        pixman_image_t *dest, int32_t src_x, int32_t src_y, int32_t mask_x, int32_t mask_y,
        int32_t dest_x, int32_t dest_y, int32_t width, int32_t height) {
    RESOLVE(get_format, get_format_fn, "pixman_image_get_format");
    RESOLVE(get_data, get_data_fn, "pixman_image_get_data");
    RESOLVE(get_stride, get_stride_fn, "pixman_image_get_stride");
    unsigned reasons = g_kind_lost ? 1u << STAT_DECLINE_KIND_LOST : 0;
    p->op = p->flags = p->solid = 0;
    p->nothing = 0;
    p->pixels = 0;

    // v1 validated SRC and OVER without a mask and OVER with one (the glyph
    // shape); any other op, or SRC through a mask, is left to pixman.
    if (mask != NULL ? op != PIXMAN_OP_OVER : (op != PIXMAN_OP_SRC && op != PIXMAN_OP_OVER))
        reasons |= 1u << STAT_DECLINE_OP;

    pixman_box32_t rect = clamp_to_image(dest, dest_x, dest_y, width, height);
    struct image_info d = image_lookup(dest, ROLE_DEST, &rect, &p->boxes);
    struct image_info si = image_lookup(src, ROLE_SRC, NULL, NULL);
    struct image_info mi = { KIND_BITS, 0, 0 };
    if (mask != NULL)
        mi = image_lookup(mask, ROLE_MASK, NULL, NULL);
    reasons |= d.reasons | si.reasons | mi.reasons;

    pixman_format_code_t dst_fmt = d.kind == KIND_BITS ? get_format(dest) : PIXMAN_a8;
    if (!is_32bpp_rgb(dst_fmt))
        reasons |= 1u << STAT_DECLINE_FORMAT;
    int src_solid = si.kind == KIND_SOLID;
    pixman_format_code_t src_fmt = PIXMAN_a8r8g8b8;
    if (si.kind == KIND_BITS) {
        src_fmt = get_format(src);
        if (!is_32bpp_rgb(src_fmt))
            reasons |= 1u << STAT_DECLINE_FORMAT;
    } else if (!src_solid) {
        reasons |= 1u << STAT_DECLINE_SRC_KIND;
    }
    if (mask != NULL) {
        if (mi.kind != KIND_BITS)
            reasons |= 1u << STAT_DECLINE_MASK_KIND;
        else if (get_format(mask) != PIXMAN_a8)
            reasons |= 1u << STAT_DECLINE_MASK_FORMAT;
    }
    if (reasons != 0)
        return reasons;

    // A bits source (or mask) outside its own bounds reads as transparent in
    // pixman; that is left to pixman. Checked per destination rectangle,
    // since only the rectangles are read.
    for (int i = 0; i < p->boxes.n; i++) {
        pixman_box32_t b = p->boxes.box[i];
        int32_t bw = b.x2 - b.x1, bh = b.y2 - b.y1;
        if (si.kind == KIND_BITS &&
                !in_bounds(src, src_x + (b.x1 - dest_x), src_y + (b.y1 - dest_y), bw, bh))
            reasons |= 1u << STAT_DECLINE_BOUNDS;
        if (mask != NULL &&
                !in_bounds(mask, mask_x + (b.x1 - dest_x), mask_y + (b.y1 - dest_y), bw, bh))
            reasons |= 1u << STAT_DECLINE_BOUNDS;
        p->pixels += (uint64_t) bw * (uint64_t) bh;
    }
    if (reasons != 0)
        return reasons;

    p->dst_bits = get_data(dest);
    p->dst_stride = (uint32_t) get_stride(dest); // bytes
    // DST_OPAQUE changes only plain OVER of an opaque source (ish_accel_pix.h).
    uint32_t dst_flag = dst_fmt == PIXMAN_x8r8g8b8 ? PIX_FLAG_DST_OPAQUE : 0;
    if (src_solid) {
        // An opaque solid: pixman turns OVER into SRC, and SRC of a solid is
        // a fill with its color_32 on either destination format. A fully
        // transparent one (color_32 == 0) OVER anything changes nothing.
        // Both checked against real pixman (pixman_accel_plan.md).
        if (op == PIXMAN_OP_SRC || (mask == NULL && (si.solid >> 24) == 0xff)) {
            p->op = PIX_OP_FILL;
            p->solid = si.solid;
            if (p->pixels < FILL_FLOOR_PIXELS)
                reasons |= 1u << STAT_DECLINE_SMALL;
        } else if (si.solid == 0) {
            p->nothing = 1;
        } else {
            p->op = mask != NULL ? PIX_OP_OVER_MASK : PIX_OP_OVER;
            p->flags = PIX_FLAG_SRC_SOLID | dst_flag;
            p->solid = si.solid;
        }
    } else {
        int src_opaque = src_fmt == PIXMAN_x8r8g8b8;
        p->src_bits = get_data(src);
        p->src_stride = (uint32_t) get_stride(src);
        p->flags = (src_opaque ? PIX_FLAG_SRC_OPAQUE : 0) | dst_flag;
        if (mask != NULL) {
            p->op = PIX_OP_OVER_MASK;
        } else if (op == PIXMAN_OP_OVER) {
            p->op = PIX_OP_OVER;
        } else {
            // SRC from x8r8g8b8 into a8r8g8b8 writes 0xff alpha in pixman;
            // every other pairing copies the top byte through.
            p->op = src_opaque && dst_fmt == PIXMAN_a8r8g8b8 ? PIX_OP_COPY_SET_ALPHA : PIX_OP_COPY;
            if (p->pixels < COPY_FLOOR_PIXELS)
                reasons |= 1u << STAT_DECLINE_SMALL;
        }
    }
    if (mask != NULL) {
        p->mask_bits = (uint8_t *) get_data(mask);
        p->mask_stride = (uint32_t) get_stride(mask);
    }
    return reasons;
}

// One destination rectangle through the kernel; 0 when it did the work.
static long plan_issue(const struct plan *p, pixman_box32_t b, int32_t src_dx, int32_t src_dy,
        int32_t mask_dx, int32_t mask_dy) {
    int solid = (p->flags & PIX_FLAG_SRC_SOLID) != 0;
    struct ish_pix_req r = {
        .op = p->op, .flags = p->flags,
        .dst = (uint64_t) (uintptr_t) p->dst_bits, .dst_stride = p->dst_stride,
        .dst_x = b.x1, .dst_y = b.y1,
        .width = (uint32_t) (b.x2 - b.x1), .height = (uint32_t) (b.y2 - b.y1),
        .fill_pixel = p->solid,
    };
    if (p->op != PIX_OP_FILL && !solid) {
        r.src = (uint64_t) (uintptr_t) p->src_bits;
        r.src_stride = p->src_stride;
        r.src_x = b.x1 + src_dx;
        r.src_y = b.y1 + src_dy;
    }
    if (p->op == PIX_OP_OVER_MASK) {
        r.mask = (uint64_t) (uintptr_t) p->mask_bits;
        r.mask_stride = p->mask_stride;
        r.mask_x = b.x1 + mask_dx;
        r.mask_y = b.y1 + mask_dy;
    }
    return syscall(ISH_SYS_PIXOP, &r);
}

// ---- the size floors' other side: the same stores, in the guest ----------
// Under FILL_FLOOR_PIXELS / COPY_FLOOR_PIXELS a syscall costs more than it
// saves, but handing the call back to pixman costs more still: its per-call
// setup (region, fast-path lookup, dispatch) is most of a one-cell fill's
// time. A fill or a plain copy is simple enough to do here, with the same
// bytes pixman writes -- which tests/manual/pixman_shim.c checks, mostly on
// small rectangles.
static void fill_in_guest(uint32_t *bits, int stride_words, int x, int y, int w, int h, uint32_t v) {
    for (int row = 0; row < h; row++) {
        uint32_t *d = bits + (ptrdiff_t) (y + row) * stride_words + x;
        for (int i = 0; i < w; i++)
            d[i] = v;
    }
}

// Returns 0 when it did the work; 1 leaves it to pixman.
static int plan_in_guest(const struct plan *p, pixman_box32_t b, int32_t src_dx, int32_t src_dy) {
    int w = b.x2 - b.x1, h = b.y2 - b.y1;
    if (p->op == PIX_OP_FILL) {
        fill_in_guest(p->dst_bits, (int) (p->dst_stride / 4), b.x1, b.y1, w, h, p->solid);
        return 0;
    }
    if (p->op != PIX_OP_COPY && p->op != PIX_OP_COPY_SET_ALPHA)
        return 1;
    // One buffer as both source and destination: pixman's own overlap
    // behaviour is not memcpy's, so that is left to it.
    if (p->src_bits == p->dst_bits)
        return 1;
    for (int row = 0; row < h; row++) {
        uint32_t *d = (uint32_t *) ((char *) p->dst_bits + (ptrdiff_t) (b.y1 + row) * p->dst_stride) + b.x1;
        const uint32_t *sp = (const uint32_t *) ((const char *) p->src_bits +
                (ptrdiff_t) (b.y1 + row + src_dy) * p->src_stride) + b.x1 + src_dx;
        if (p->op == PIX_OP_COPY) {
            memcpy(d, sp, (size_t) w * 4);
        } else {
            for (int i = 0; i < w; i++)
                d[i] = sp[i] | 0xff000000u;
        }
    }
    return 0;
}

// ---- interposed composite/fill entry points ------------------------------

static void composite32(pixman_op_t op, pixman_image_t *src, pixman_image_t *mask, pixman_image_t *dest,
        int32_t src_x, int32_t src_y, int32_t mask_x, int32_t mask_y,
        int32_t dest_x, int32_t dest_y, int32_t width, int32_t height) {
    RESOLVE(real, composite32_fn, "pixman_image_composite32");
    // Without the statistics, nothing but the accelerator cares what shape
    // this call was: skip the classification when it is unavailable.
    int stats = stats_enabled();
    if ((!stats && !accel_available()) || width <= 0 || height <= 0) {
        real(op, src, mask, dest, src_x, src_y, mask_x, mask_y, dest_x, dest_y, width, height);
        return;
    }
    uint64_t t0 = stats ? now_ns() : 0;
    uint64_t pixels = (uint64_t) width * (uint64_t) height;
    struct plan p;
    unsigned reasons = composite_plan(&p, op, src, mask, dest, src_x, src_y, mask_x, mask_y,
            dest_x, dest_y, width, height);
    if (reasons == 0 && !accel_available())
        reasons = 1u << STAT_DECLINE_UNAVAILABLE;
    if (reasons == 1u << STAT_DECLINE_SMALL && accel_available()) {
        // Planned, only too small for the kernel: the same stores, here.
        int handed_back = 0;
        for (int i = 0; !handed_back && i < p.boxes.n; i++)
            handed_back = plan_in_guest(&p, p.boxes.box[i], src_x - dest_x, src_y - dest_y);
        if (!handed_back) {
            if (stats) {
                note(STAT_DECLINE_SMALL, pixels);
                note_shape(reasons, op, src, mask, dest, pixels, now_ns() - t0);
            }
            return;
        }
        // Only the first rectangle can have refused (a shared buffer is a
        // property of the call, not of a rectangle), so nothing was written.
    }
    if (reasons != 0) {
        real(op, src, mask, dest, src_x, src_y, mask_x, mask_y, dest_x, dest_y, width, height);
        if (stats) {
            note(primary_reason(reasons), pixels);
            note_shape(reasons, op, src, mask, dest, pixels, now_ns() - t0);
        }
        return;
    }
    // Each rectangle lies inside the clip and the image, so a rectangle the
    // kernel refuses is handed to pixman ON ITS OWN, with the image's clip
    // still set: pixman's result for it is exactly its share of the whole
    // call's, and a rectangle already done is never done twice (which OVER
    // could not survive).
    int32_t src_dx = src_x - dest_x, src_dy = src_y - dest_y;
    int32_t mask_dx = mask_x - dest_x, mask_dy = mask_y - dest_y;
    int refused = 0;
    for (int i = 0; !p.nothing && i < p.boxes.n; i++) {
        pixman_box32_t b = p.boxes.box[i];
        if (plan_issue(&p, b, src_dx, src_dy, mask_dx, mask_dy) != 0) {
            refused++;
            real(op, src, mask, dest, b.x1 + src_dx, b.y1 + src_dy, b.x1 + mask_dx, b.y1 + mask_dy,
                    b.x1, b.y1, b.x2 - b.x1, b.y2 - b.y1);
        }
    }
    if (stats) {
        if (refused != 0) {
            note(STAT_DECLINE_SYSCALL, pixels);
            note_shape(1u << STAT_DECLINE_SYSCALL, op, src, mask, dest, pixels, now_ns() - t0);
        } else {
            note(mask != NULL ? STAT_ACCEL_MASK : STAT_ACCEL_COMPOSITE, pixels);
            note_shape(0, op, src, mask, dest, pixels, now_ns() - t0);
        }
    }
}

void pixman_image_composite32(pixman_op_t op, pixman_image_t *src, pixman_image_t *mask, pixman_image_t *dest,
        int32_t src_x, int32_t src_y, int32_t mask_x, int32_t mask_y,
        int32_t dest_x, int32_t dest_y, int32_t width, int32_t height) {
    struct timing t = timing_begin();
    composite32(op, src, mask, dest, src_x, src_y, mask_x, mask_y, dest_x, dest_y, width, height);
    timing_end(ENTRY_COMPOSITE32, t);
}

// The 16-bit-coordinate original is just a widening wrapper in pixman; route
// it through the same path directly rather than rely on pixman's internal
// call reaching composite32 above through the PLT.
void pixman_image_composite(pixman_op_t op, pixman_image_t *src, pixman_image_t *mask, pixman_image_t *dest,
        int16_t src_x, int16_t src_y, int16_t mask_x, int16_t mask_y,
        int16_t dest_x, int16_t dest_y, uint16_t width, uint16_t height) {
    struct timing t = timing_begin();
    composite32(op, src, mask, dest, src_x, src_y, mask_x, mask_y, dest_x, dest_y, width, height);
    timing_end(ENTRY_COMPOSITE, t);
}

static pixman_bool_t fill(uint32_t *bits, int stride, int bpp, int x, int y, int width, int height, uint32_t xor_) {
    RESOLVE(real, fill_fn, "pixman_fill");
    int stats = stats_enabled();
    uint64_t t0 = stats ? now_ns() : 0;
    uint64_t pixels = width > 0 && height > 0 ? (uint64_t) width * (uint64_t) height : 0;
    int reason;
    if (pixels == 0) {
        return real(bits, stride, bpp, x, y, width, height, xor_);
    } else if (bpp != 32) {
        reason = STAT_DECLINE_FILL_BPP;
    } else if (pixels < FILL_FLOOR_PIXELS && accel_available()) {
        fill_in_guest(bits, stride, x, y, width, height, xor_);
        if (stats) {
            note(STAT_DECLINE_SMALL, pixels);
            note_shape_raw(1u << STAT_DECLINE_SMALL, OP_FILL, 0, 0, (uint32_t) bpp, pixels, now_ns() - t0);
        }
        return 1;
    } else if (pixels < FILL_FLOOR_PIXELS) {
        reason = STAT_DECLINE_SMALL;
    } else if (!accel_available()) {
        reason = STAT_DECLINE_UNAVAILABLE;
    } else {
        // pixman_fill's `stride` is documented (and verified empirically
        // against real pixman before this shim was written -- see
        // pixman_accel_plan.md) to be in 32-bit WORDS; our syscall's
        // dst_stride is bytes throughout.
        uint32_t stride_bytes = (uint32_t) stride * 4;
        long ret = pixop(PIX_OP_FILL, 0, bits, stride_bytes, x, y,
                NULL, 0, 0, 0, (uint32_t) width, (uint32_t) height, xor_);
        if (ret == 0) {
            if (stats) {
                note(STAT_ACCEL_FILL, pixels);
                note_shape_raw(0, OP_FILL, 0, 0, (uint32_t) bpp, pixels, now_ns() - t0);
            }
            return 1;
        }
        reason = STAT_DECLINE_SYSCALL;
    }
    pixman_bool_t ret = real(bits, stride, bpp, x, y, width, height, xor_);
    if (stats) {
        note(reason, pixels);
        note_shape_raw(1u << reason, OP_FILL, 0, 0, (uint32_t) bpp, pixels, now_ns() - t0);
    }
    return ret;
}

pixman_bool_t pixman_fill(uint32_t *bits, int stride, int bpp, int x, int y, int width, int height, uint32_t xor_) {
    struct timing t = timing_begin();
    pixman_bool_t ret = fill(bits, stride, bpp, x, y, width, height, xor_);
    timing_end(ENTRY_FILL, t);
    return ret;
}

// pixman_blt: a raw copy between two 32bpp buffers (strides in 32-bit words,
// as pixman_fill's are), which GTK's cairo uses for surface-to-surface
// copies. No formats, so no alpha rule: bytes through. Overlapping source and
// destination are refused by the kernel and left to pixman.
//
// Only where pixman's own blt works: pixman implements it in its SIMD
// backends (NEON, SSE2, MMX) and NOT in the plain C one, where it copies
// nothing and returns FALSE -- the caller (cairo) then copies by itself. On a
// riscv64 guest the shim answering TRUE would have changed what the caller
// saw. So the first call probes real pixman once on a scratch pixel.
static int blt_supported(void) {
    static int supported = -1;
    if (supported == -1) {
        RESOLVE(real, blt_fn, "pixman_blt");
        uint32_t a[1] = { 0x12345678u }, b[1] = { 0 };
        supported = real != NULL && real(a, b, 1, 1, 32, 32, 0, 0, 0, 0, 1, 1) && b[0] == a[0];
    }
    return supported;
}

pixman_bool_t pixman_blt(uint32_t *src_bits, uint32_t *dst_bits, int src_stride, int dst_stride,
        int src_bpp, int dst_bpp, int src_x, int src_y, int dest_x, int dest_y, int width, int height) {
    RESOLVE(real, blt_fn, "pixman_blt");
    struct timing t = timing_begin();
    int stats = stats_enabled();
    uint64_t t0 = stats ? now_ns() : 0;
    uint64_t pixels = width > 0 && height > 0 ? (uint64_t) width * (uint64_t) height : 0;
    int reason = -1;
    pixman_bool_t ret;
    if (pixels == 0 || src_bpp != 32 || dst_bpp != 32 || !blt_supported()) {
        reason = STAT_DECLINE_FORMAT;
    } else if (pixels < COPY_FLOOR_PIXELS) {
        reason = STAT_DECLINE_SMALL;
    } else if (!accel_available()) {
        reason = STAT_DECLINE_UNAVAILABLE;
    } else if (pixop(PIX_OP_COPY, 0, dst_bits, (uint32_t) dst_stride * 4, dest_x, dest_y,
                src_bits, (uint32_t) src_stride * 4, src_x, src_y,
                (uint32_t) width, (uint32_t) height, 0) != 0) {
        reason = STAT_DECLINE_SYSCALL;
    }
    if (reason < 0) {
        ret = 1;
        if (stats) {
            note(STAT_ACCEL_COMPOSITE, pixels);
            note_shape_raw(0, OP_BLT, 0, 0, (uint32_t) dst_bpp, pixels, now_ns() - t0);
        }
    } else {
        ret = real(src_bits, dst_bits, src_stride, dst_stride, src_bpp, dst_bpp,
                src_x, src_y, dest_x, dest_y, width, height);
        if (stats && pixels != 0) {
            note(reason, pixels);
            note_shape_raw(1u << reason, OP_BLT, 0, 0, (uint32_t) dst_bpp, pixels, now_ns() - t0);
        }
    }
    timing_end(ENTRY_BLT, t);
    return ret;
}

// pixman_image_fill_boxes, done here instead of in pixman: its own version
// builds a region from the boxes, intersects the clip and fills through
// pixman_fill -- ~200 us of emulated setup per call on an iPad for a
// terminal's one-cell background, which is most of what foot draws.
//
// Semantics copied from pixman.c, each checked against real pixman by
// tests/manual/pixman_shim.c: CLEAR is SRC of transparent black; OVER of an
// opaque colour is SRC; SRC fills every box (empty ones dropped) with the
// colour's color_32, cut by the destination's clip region if it has one and
// NOT clamped to the image (pixman does not clamp either, so a box outside
// the image is left to pixman rather than written); any other op composites
// a solid fill of the colour box by box. Returns 1 with *ret set when it
// handled the call, 0 to hand the whole call to pixman.
static int fill_boxes(pixman_op_t op, pixman_image_t *dest, const pixman_color_t *color,
        int n_boxes, const pixman_box32_t *boxes, pixman_bool_t *ret) {
    if (!accel_available() || n_boxes < 0)
        return 0;
    RESOLVE(get_format, get_format_fn, "pixman_image_get_format");
    RESOLVE(get_data, get_data_fn, "pixman_image_get_data");
    RESOLVE(get_stride, get_stride_fn, "pixman_image_get_stride");
    RESOLVE(get_width, get_width_fn, "pixman_image_get_width");
    RESOLVE(get_height, get_height_fn, "pixman_image_get_height");
    pixman_color_t c = *color;
    if (op == PIXMAN_OP_CLEAR) {
        c = (pixman_color_t) { 0, 0, 0, 0 };
        op = PIXMAN_OP_SRC;
    }
    if (op == PIXMAN_OP_OVER && c.alpha == 0xffff)
        op = PIXMAN_OP_SRC;
    if (op != PIXMAN_OP_SRC && op != PIXMAN_OP_OVER)
        return 0;
    if (image_kind(dest) != KIND_BITS || !is_32bpp_rgb(get_format(dest)))
        return 0;
    if (op == PIXMAN_OP_OVER) {
        // pixman composites a solid fill of the colour over each box in turn.
        pixman_image_t *solid = pixman_image_create_solid_fill(&c);
        if (solid == NULL)
            return 0;
        for (int i = 0; i < n_boxes; i++) {
            const pixman_box32_t *b = &boxes[i];
            composite32(PIXMAN_OP_OVER, solid, NULL, dest, 0, 0, 0, 0,
                    b->x1, b->y1, b->x2 - b->x1, b->y2 - b->y1);
        }
        pixman_image_unref(solid);
        *ret = 1;
        return 1;
    }
    // SRC. Refuse anything outside the image before writing a thing.
    int32_t iw = get_width(dest), ih = get_height(dest);
    for (int i = 0; i < n_boxes; i++) {
        const pixman_box32_t *b = &boxes[i];
        if (!box_empty(*b) && (b->x1 < 0 || b->y1 < 0 || b->x2 > iw || b->y2 > ih))
            return 0;
    }
    uint32_t pixel = ((uint32_t) (c.alpha >> 8) << 24) | ((uint32_t) (c.red >> 8) << 16) |
            ((uint32_t) c.green & 0xff00) | ((uint32_t) c.blue >> 8);
    uint32_t *bits = get_data(dest);
    int stride_words = get_stride(dest) / 4;
    int stats = stats_enabled();
    for (int i = 0; i < n_boxes; i++) {
        if (box_empty(boxes[i]))
            continue;
        struct dest_boxes pieces;
        struct image_info d = image_lookup(dest, ROLE_DEST, &boxes[i], &pieces);
        if (d.reasons != 0) {
            // A clip the shim cannot use: boxes already filled were SRC fills
            // of the same colour, so letting pixman redo the whole call writes
            // exactly the same bytes over them.
            return 0;
        }
        for (int k = 0; k < pieces.n; k++) {
            pixman_box32_t b = pieces.box[k];
            int w = b.x2 - b.x1, h = b.y2 - b.y1;
            uint64_t t0 = stats ? now_ns() : 0;
            uint64_t px = (uint64_t) w * (uint64_t) h;
            int accel = px >= FILL_FLOOR_PIXELS &&
                    pixop(PIX_OP_FILL, 0, bits, (uint32_t) stride_words * 4, b.x1, b.y1,
                            NULL, 0, 0, 0, (uint32_t) w, (uint32_t) h, pixel) == 0;
            if (!accel)
                fill_in_guest(bits, stride_words, b.x1, b.y1, w, h, pixel);
            if (stats) {
                int why = accel ? 0 : px < FILL_FLOOR_PIXELS ? STAT_DECLINE_SMALL : STAT_DECLINE_SYSCALL;
                note(accel ? STAT_ACCEL_FILL : why, px);
                note_shape_raw(accel ? 0 : 1u << why, OP_FILL_BOXES, 0, 0, 32, px, now_ns() - t0);
            }
        }
    }
    *ret = 1;
    return 1;
}

pixman_bool_t pixman_image_fill_boxes(pixman_op_t op, pixman_image_t *dest, const pixman_color_t *color,
        int n_boxes, const pixman_box32_t *boxes) {
    RESOLVE(real, fill_boxes_fn, "pixman_image_fill_boxes");
    struct timing t = timing_begin();
    pixman_bool_t ret;
    if (!fill_boxes(op, dest, color, n_boxes, boxes, &ret))
        ret = real(op, dest, color, n_boxes, boxes);
    timing_end(ENTRY_FILL_BOXES, t);
    return ret;
}

// pixman converts the rectangles to boxes and calls fill_boxes; so does this,
// straight into the shim's own.
pixman_bool_t pixman_image_fill_rectangles(pixman_op_t op, pixman_image_t *dest, const pixman_color_t *color,
        int n_rects, const pixman_rectangle16_t *rects) {
    RESOLVE(real, fill_rectangles_fn, "pixman_image_fill_rectangles");
    struct timing t = timing_begin();
    pixman_bool_t ret;
    pixman_box32_t stack_boxes[16];
    pixman_box32_t *boxes = n_rects <= 16 ? stack_boxes : malloc((size_t) n_rects * sizeof(*boxes));
    if (n_rects < 0 || boxes == NULL) {
        ret = real(op, dest, color, n_rects, rects);
    } else {
        for (int i = 0; i < n_rects; i++)
            boxes[i] = (pixman_box32_t) { rects[i].x, rects[i].y,
                    rects[i].x + rects[i].width, rects[i].y + rects[i].height };
        if (!fill_boxes(op, dest, color, n_rects, boxes, &ret))
            ret = real(op, dest, color, n_rects, rects);
        if (boxes != stack_boxes)
            free(boxes);
    }
    timing_end(ENTRY_FILL_RECTANGLES, t);
    return ret;
}

// ---- pass-through entry points, interposed only to time them --------------
// None of these accelerate; they are here so ISH_PIXMAN_STATS can say how
// much of a client's pixman time they account for. pixman composites glyphs
// through its own internal fast paths, not through the public entry points,
// so nothing a glyph call does inside pixman comes back through the shim.

void pixman_composite_glyphs(pixman_op_t op, pixman_image_t *src, pixman_image_t *dest,
        pixman_format_code_t mask_format, int32_t src_x, int32_t src_y, int32_t mask_x, int32_t mask_y,
        int32_t dest_x, int32_t dest_y, int32_t width, int32_t height,
        pixman_glyph_cache_t *cache, int n_glyphs, const pixman_glyph_t *glyphs) {
    RESOLVE(real, glyphs_fn, "pixman_composite_glyphs");
    struct timing t = timing_begin();
    real(op, src, dest, mask_format, src_x, src_y, mask_x, mask_y, dest_x, dest_y, width, height,
            cache, n_glyphs, glyphs);
    timing_end(ENTRY_GLYPHS, t);
}

void pixman_composite_glyphs_no_mask(pixman_op_t op, pixman_image_t *src, pixman_image_t *dest,
        int32_t src_x, int32_t src_y, int32_t dest_x, int32_t dest_y,
        pixman_glyph_cache_t *cache, int n_glyphs, const pixman_glyph_t *glyphs) {
    RESOLVE(real, glyphs_no_mask_fn, "pixman_composite_glyphs_no_mask");
    struct timing t = timing_begin();
    real(op, src, dest, src_x, src_y, dest_x, dest_y, cache, n_glyphs, glyphs);
    timing_end(ENTRY_GLYPHS_NO_MASK, t);
}

void pixman_composite_trapezoids(pixman_op_t op, pixman_image_t *src, pixman_image_t *dst,
        pixman_format_code_t mask_format, int x_src, int y_src, int x_dst, int y_dst,
        int n_traps, const pixman_trapezoid_t *traps) {
    RESOLVE(real, traps_fn, "pixman_composite_trapezoids");
    struct timing t = timing_begin();
    real(op, src, dst, mask_format, x_src, y_src, x_dst, y_dst, n_traps, traps);
    timing_end(ENTRY_TRAPEZOIDS, t);
}

void pixman_composite_triangles(pixman_op_t op, pixman_image_t *src, pixman_image_t *dst,
        pixman_format_code_t mask_format, int x_src, int y_src, int x_dst, int y_dst,
        int n_tris, const pixman_triangle_t *tris) {
    RESOLVE(real, tris_fn, "pixman_composite_triangles");
    struct timing t = timing_begin();
    real(op, src, dst, mask_format, x_src, y_src, x_dst, y_dst, n_tris, tris);
    timing_end(ENTRY_TRIANGLES, t);
}

void pixman_add_traps(pixman_image_t *image, int16_t x_off, int16_t y_off, int ntrap, const pixman_trap_t *traps) {
    RESOLVE(real, add_traps_fn, "pixman_add_traps");
    struct timing t = timing_begin();
    real(image, x_off, y_off, ntrap, traps);
    timing_end(ENTRY_ADD_TRAPS, t);
}

void pixman_add_trapezoids(pixman_image_t *image, int16_t x_off, int y_off, int ntraps,
        const pixman_trapezoid_t *traps) {
    RESOLVE(real, add_trapezoids_fn, "pixman_add_trapezoids");
    struct timing t = timing_begin();
    real(image, x_off, y_off, ntraps, traps);
    timing_end(ENTRY_ADD_TRAPEZOIDS, t);
}

void pixman_rasterize_trapezoid(pixman_image_t *image, const pixman_trapezoid_t *trap, int x_off, int y_off) {
    RESOLVE(real, rasterize_trapezoid_fn, "pixman_rasterize_trapezoid");
    struct timing t = timing_begin();
    real(image, trap, x_off, y_off);
    timing_end(ENTRY_RASTERIZE_TRAPEZOID, t);
}
