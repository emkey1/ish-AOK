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
// exactly match the accelerator's supported shape (32bpp a8r8g8b8/
// x8r8g8b8 src/dst; SRC or OVER; OVER may carry an a8 mask (the glyph/
// text-rendering shape) but SRC-with-mask and any other op+mask
// combination decline (real, but not yet differential-tested); no
// transform, no non-identity filter, no repeat, no alpha map, no clip on
// ANY of dst/src/mask; in-bounds). Nothing here approximates -- only
// "accelerate bit-exactly" or "decline to real pixman" are possible
// outcomes, exactly like the kernel side. Only PUBLIC pixman accessors are
// used to inspect an image's state; the five properties pixman has no
// getter for (transform/repeat/filter/alpha-map/clip) are shadow-tracked
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
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pixman.h>

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
    int has_transform;
    int has_alpha_map;
    int has_clip;
    int nontrivial_filter; // filter set to anything other than NEAREST
    pixman_repeat_t repeat;
    struct image_state *next;
};
#define STATE_BUCKETS 256
static struct image_state *g_buckets[STATE_BUCKETS];
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;

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
static void state_set_kind(pixman_image_t *img, int kind) {
    if (img == NULL)
        return;
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_get_or_create(img);
    if (s != NULL)
        s->kind = kind;
    else
        g_kind_lost = 1;
    pthread_mutex_unlock(&g_state_lock);
}

static int image_kind(pixman_image_t *img) {
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_find(img);
    int kind = s != NULL ? s->kind : KIND_BITS;
    pthread_mutex_unlock(&g_state_lock);
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
    STAT_DECLINE_KIND_LOST,
    STAT_COUNT,
};
static const char *const stat_names[STAT_COUNT] = {
    "accelerated-composite", "accelerated-fill", "accelerated-mask",
    "decline-mask-format", "decline-op", "decline-format",
    "decline-transform", "decline-repeat", "decline-filter",
    "decline-alpha-map", "decline-clip", "decline-component-alpha",
    "decline-bounds", "decline-accel-unavailable", "decline-syscall-refused",
    "decline-fill-bpp", "decline-src-kind", "decline-mask-kind",
    "decline-kind-lost",
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
#define OP_FILL 0x100 // pseudo-op for pixman_fill, which has no pixman_op_t
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
                why, e->op == OP_FILL ? "FILL" : op_name(e->op, b0, sizeof(b0)),
                describe_name(e->src, b1, sizeof(b1)), describe_name(e->mask, b2, sizeof(b2)),
                e->op == OP_FILL ? (snprintf(b3, sizeof(b3), "%ubpp", e->dst), b3) : describe_name(e->dst, b3, sizeof(b3)),
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
    state_set_kind(img, KIND_SOLID);
    return img;
}

pixman_image_t *pixman_image_create_linear_gradient(const pixman_point_fixed_t *p1, const pixman_point_fixed_t *p2,
        const pixman_gradient_stop_t *stops, int n_stops) {
    RESOLVE(real, create_linear_fn, "pixman_image_create_linear_gradient");
    pixman_image_t *img = real(p1, p2, stops, n_stops);
    state_set_kind(img, KIND_LINEAR);
    return img;
}

pixman_image_t *pixman_image_create_radial_gradient(const pixman_point_fixed_t *inner, const pixman_point_fixed_t *outer,
        pixman_fixed_t inner_radius, pixman_fixed_t outer_radius, const pixman_gradient_stop_t *stops, int n_stops) {
    RESOLVE(real, create_radial_fn, "pixman_image_create_radial_gradient");
    pixman_image_t *img = real(inner, outer, inner_radius, outer_radius, stops, n_stops);
    state_set_kind(img, KIND_RADIAL);
    return img;
}

pixman_image_t *pixman_image_create_conical_gradient(const pixman_point_fixed_t *center, pixman_fixed_t angle,
        const pixman_gradient_stop_t *stops, int n_stops) {
    RESOLVE(real, create_conical_fn, "pixman_image_create_conical_gradient");
    pixman_image_t *img = real(center, angle, stops, n_stops);
    state_set_kind(img, KIND_CONICAL);
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
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_get_or_create(image);
    if (s != NULL)
        s->has_transform = (transform != NULL);
    pthread_mutex_unlock(&g_state_lock);
    return ret;
}

void pixman_image_set_repeat(pixman_image_t *image, pixman_repeat_t repeat) {
    RESOLVE(real, set_repeat_fn, "pixman_image_set_repeat");
    real(image, repeat);
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_get_or_create(image);
    if (s != NULL)
        s->repeat = repeat;
    pthread_mutex_unlock(&g_state_lock);
}

pixman_bool_t pixman_image_set_filter(pixman_image_t *image, pixman_filter_t filter,
        const pixman_fixed_t *filter_params, int n_filter_params) {
    RESOLVE(real, set_filter_fn, "pixman_image_set_filter");
    pixman_bool_t ret = real(image, filter, filter_params, n_filter_params);
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_get_or_create(image);
    if (s != NULL)
        s->nontrivial_filter = (filter != PIXMAN_FILTER_NEAREST);
    pthread_mutex_unlock(&g_state_lock);
    return ret;
}

void pixman_image_set_alpha_map(pixman_image_t *image, pixman_image_t *alpha_map, int16_t x, int16_t y) {
    RESOLVE(real, set_alpha_map_fn, "pixman_image_set_alpha_map");
    real(image, alpha_map, x, y);
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_get_or_create(image);
    if (s != NULL)
        s->has_alpha_map = (alpha_map != NULL);
    pthread_mutex_unlock(&g_state_lock);
}

pixman_bool_t pixman_image_set_clip_region(pixman_image_t *image, const pixman_region16_t *region) {
    RESOLVE(real, set_clip_region_fn, "pixman_image_set_clip_region");
    pixman_bool_t ret = real(image, region);
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_get_or_create(image);
    if (s != NULL)
        s->has_clip = (region != NULL);
    pthread_mutex_unlock(&g_state_lock);
    return ret;
}

pixman_bool_t pixman_image_set_clip_region32(pixman_image_t *image, const pixman_region32_t *region) {
    RESOLVE(real, set_clip_region32_fn, "pixman_image_set_clip_region32");
    pixman_bool_t ret = real(image, region);
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_get_or_create(image);
    if (s != NULL)
        s->has_clip = (region != NULL);
    pthread_mutex_unlock(&g_state_lock);
    return ret;
}

void pixman_image_set_has_client_clip(pixman_image_t *image, pixman_bool_t client_clip) {
    RESOLVE(real, set_has_client_clip_fn, "pixman_image_set_has_client_clip");
    real(image, client_clip);
    // Conservative: client-clip means pixman defers clipping to the
    // caller, which we have no way to inspect -- treat it the same as a
    // real clip region (decline) rather than assuming it's a no-op.
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_get_or_create(image);
    if (s != NULL && client_clip)
        s->has_clip = 1;
    pthread_mutex_unlock(&g_state_lock);
}

pixman_bool_t pixman_image_unref(pixman_image_t *image) {
    RESOLVE(real, unref_fn, "pixman_image_unref");
    pixman_bool_t freed = real(image);
    if (freed) {
        pthread_mutex_lock(&g_state_lock);
        state_remove(image);
        pthread_mutex_unlock(&g_state_lock);
    }
    return freed;
}

// ---- accelerability check ------------------------------------------------
// The set of STAT_DECLINE_* reasons `image` is outside the accelerator's
// supported shape, as bits (0 = within it) -- which ISH_PIXMAN_STATS
// reports, so the output says what to widen next.
static unsigned image_decline_reasons(pixman_image_t *image) {
    RESOLVE(real_gca, get_component_alpha_fn, "pixman_image_get_component_alpha");
    unsigned reasons = 0;
    if (real_gca(image))
        reasons |= 1u << STAT_DECLINE_COMPONENT_ALPHA;
    pthread_mutex_lock(&g_state_lock);
    struct image_state *s = state_find(image);
    if (s != NULL) {
        if (s->has_transform) reasons |= 1u << STAT_DECLINE_TRANSFORM;
        if (s->repeat != PIXMAN_REPEAT_NONE) reasons |= 1u << STAT_DECLINE_REPEAT;
        if (s->nontrivial_filter) reasons |= 1u << STAT_DECLINE_FILTER;
        if (s->has_alpha_map) reasons |= 1u << STAT_DECLINE_ALPHA_MAP;
        if (s->has_clip) reasons |= 1u << STAT_DECLINE_CLIP;
    }
    pthread_mutex_unlock(&g_state_lock);
    return reasons;
}

// Is [x, x+w) x [y, y+h) inside a bits image?
static int in_bounds(pixman_image_t *img, int32_t x, int32_t y, int32_t w, int32_t h) {
    RESOLVE(get_width, get_width_fn, "pixman_image_get_width");
    RESOLVE(get_height, get_height_fn, "pixman_image_get_height");
    return x >= 0 && y >= 0 &&
            (int64_t) x + w <= get_width(img) && (int64_t) y + h <= get_height(img);
}

// Every reason pixman_image_composite32 below would refuse this call, as a
// set of 1u << STAT_DECLINE_* bits, or 0 when it is exactly a shape the
// accelerator covers. All of them, not just the first: a shape refused for
// two reasons needs both fixed before it pays.
static unsigned composite_decline_reasons(pixman_op_t op, pixman_image_t *src, pixman_image_t *mask,
        pixman_image_t *dest, int32_t src_x, int32_t src_y, int32_t mask_x, int32_t mask_y,
        int32_t dest_x, int32_t dest_y, int32_t width, int32_t height) {
    unsigned reasons = 0;
    // v1 only validated OVER-with-mask (the glyph/text-rendering shape);
    // SRC-with-mask and any other op+mask combination are real pixman
    // operations we simply haven't differential-tested, so they decline
    // rather than guess at untested arithmetic.
    if (mask != NULL && op != PIXMAN_OP_OVER)
        reasons |= 1u << STAT_DECLINE_OP;
    if (mask == NULL && op != PIXMAN_OP_SRC && op != PIXMAN_OP_OVER)
        reasons |= 1u << STAT_DECLINE_OP;
    if (g_kind_lost)
        reasons |= 1u << STAT_DECLINE_KIND_LOST;

    RESOLVE(get_format, get_format_fn, "pixman_image_get_format");
    int src_bits = image_kind(src) == KIND_BITS;
    int mask_bits = mask == NULL || image_kind(mask) == KIND_BITS;
    if (!src_bits)
        reasons |= 1u << STAT_DECLINE_SRC_KIND;
    if (!mask_bits)
        reasons |= 1u << STAT_DECLINE_MASK_KIND;
    pixman_format_code_t dst_fmt = get_format(dest);
    if (dst_fmt != PIXMAN_a8r8g8b8 && dst_fmt != PIXMAN_x8r8g8b8)
        reasons |= 1u << STAT_DECLINE_FORMAT;
    if (src_bits) {
        pixman_format_code_t src_fmt = get_format(src);
        if (src_fmt != PIXMAN_a8r8g8b8 && src_fmt != PIXMAN_x8r8g8b8)
            reasons |= 1u << STAT_DECLINE_FORMAT;
    }
    if (mask != NULL && mask_bits && get_format(mask) != PIXMAN_a8)
        reasons |= 1u << STAT_DECLINE_MASK_FORMAT;
    reasons |= image_decline_reasons(src) | image_decline_reasons(dest);
    if (mask != NULL)
        reasons |= image_decline_reasons(mask);

    // Real pixman auto-clips a composite rect to the destination's bounds and
    // samples a bits source outside its bounds as transparent; replicating
    // either is extra complexity for no real payoff so far, so any part out
    // of bounds declines. A solid or gradient image has no bounds.
    if (!in_bounds(dest, dest_x, dest_y, width, height) ||
            (src_bits && !in_bounds(src, src_x, src_y, width, height)) ||
            (mask != NULL && mask_bits && !in_bounds(mask, mask_x, mask_y, width, height)))
        reasons |= 1u << STAT_DECLINE_BOUNDS;
    return reasons;
}

// ---- interposed composite/fill entry points ------------------------------

// Tries the accelerator for a call composite_decline_reasons() accepted;
// returns 0 when it did the work.
static long composite_accel(pixman_op_t op, pixman_image_t *src, pixman_image_t *mask, pixman_image_t *dest,
        int32_t src_x, int32_t src_y, int32_t mask_x, int32_t mask_y,
        int32_t dest_x, int32_t dest_y, int32_t width, int32_t height) {
    RESOLVE(get_format, get_format_fn, "pixman_image_get_format");
    RESOLVE(get_data, get_data_fn, "pixman_image_get_data");
    RESOLVE(get_stride, get_stride_fn, "pixman_image_get_stride");
    int src_opaque_fmt = get_format(src) == PIXMAN_x8r8g8b8;
    int dst_opaque_fmt = get_format(dest) == PIXMAN_x8r8g8b8;
    uint32_t *dst_bits = get_data(dest);
    uint32_t *src_bits = get_data(src);
    uint32_t dst_stride = (uint32_t) get_stride(dest); // pixman_image_get_stride is documented in BYTES
    uint32_t src_stride = (uint32_t) get_stride(src);
    // DST_OPAQUE only changes behavior for plain OVER (see ish_accel_
    // pix.h's ish_pix_over_row doc) -- passed unconditionally anyway
    // since the kernel simply ignores it for FILL/COPY/OVER_MASK, which
    // are already validated dst-format-independent.
    uint32_t dst_flag = dst_opaque_fmt ? PIX_FLAG_DST_OPAQUE : 0;
    if (mask != NULL) {
        uint32_t *mask_bits = get_data(mask);
        uint32_t mask_stride = (uint32_t) get_stride(mask);
        return pixop_mask((src_opaque_fmt ? PIX_FLAG_SRC_OPAQUE : 0) | dst_flag,
                dst_bits, dst_stride, dest_x, dest_y,
                src_bits, src_stride, src_x, src_y,
                mask_bits, mask_stride, mask_x, mask_y,
                (uint32_t) width, (uint32_t) height);
    }
    return pixop(op == PIXMAN_OP_SRC ? PIX_OP_COPY : PIX_OP_OVER,
            (src_opaque_fmt ? PIX_FLAG_SRC_OPAQUE : 0) | dst_flag,
            dst_bits, dst_stride, dest_x, dest_y,
            src_bits, src_stride, src_x, src_y,
            (uint32_t) width, (uint32_t) height, 0);
}

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
    unsigned reasons = composite_decline_reasons(op, src, mask, dest, src_x, src_y, mask_x, mask_y,
            dest_x, dest_y, width, height);
    if (reasons == 0 && !accel_available())
        reasons = 1u << STAT_DECLINE_UNAVAILABLE;
    if (reasons == 0) {
        if (composite_accel(op, src, mask, dest, src_x, src_y, mask_x, mask_y,
                    dest_x, dest_y, width, height) == 0) {
            if (stats) {
                note(mask != NULL ? STAT_ACCEL_MASK : STAT_ACCEL_COMPOSITE, pixels);
                note_shape(0, op, src, mask, dest, pixels, now_ns() - t0);
            }
            return;
        }
        reasons = 1u << STAT_DECLINE_SYSCALL;
    }
    real(op, src, mask, dest, src_x, src_y, mask_x, mask_y, dest_x, dest_y, width, height);
    if (stats) {
        note(primary_reason(reasons), pixels);
        note_shape(reasons, op, src, mask, dest, pixels, now_ns() - t0);
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

// ---- pass-through entry points, interposed only to time them --------------
// None of these accelerate (yet); they are here so ISH_PIXMAN_STATS can say
// how much of a client's pixman time they account for. Any pixman-internal
// call they make back into composite32/fill above still accelerates.

pixman_bool_t pixman_blt(uint32_t *src_bits, uint32_t *dst_bits, int src_stride, int dst_stride,
        int src_bpp, int dst_bpp, int src_x, int src_y, int dest_x, int dest_y, int width, int height) {
    RESOLVE(real, blt_fn, "pixman_blt");
    struct timing t = timing_begin();
    pixman_bool_t ret = real(src_bits, dst_bits, src_stride, dst_stride, src_bpp, dst_bpp,
            src_x, src_y, dest_x, dest_y, width, height);
    timing_end(ENTRY_BLT, t);
    return ret;
}

pixman_bool_t pixman_image_fill_boxes(pixman_op_t op, pixman_image_t *dest, const pixman_color_t *color,
        int n_boxes, const pixman_box32_t *boxes) {
    RESOLVE(real, fill_boxes_fn, "pixman_image_fill_boxes");
    struct timing t = timing_begin();
    pixman_bool_t ret = real(op, dest, color, n_boxes, boxes);
    timing_end(ENTRY_FILL_BOXES, t);
    return ret;
}

pixman_bool_t pixman_image_fill_rectangles(pixman_op_t op, pixman_image_t *dest, const pixman_color_t *color,
        int n_rects, const pixman_rectangle16_t *rects) {
    RESOLVE(real, fill_rectangles_fn, "pixman_image_fill_rectangles");
    struct timing t = timing_begin();
    pixman_bool_t ret = real(op, dest, color, n_rects, rects);
    timing_end(ENTRY_FILL_RECTANGLES, t);
    return ret;
}

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
