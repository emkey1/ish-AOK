// ish_wl_release_guard -- LD_PRELOAD for Wayland clients in AOK's desktop
// sessions (start-wayland.sh): one wl_buffer.release for a shm buffer, however
// many times it was committed.
//
// wlroots (labwc, sway, Wayfire) copies a shm buffer when a commit lands and
// releases it straight away -- once per commit. A buffer committed twice
// before the first release arrives is released twice. GTK 3 cannot take
// that: it takes the first release to mean the buffer is free, starts drawing
// its next frame into it, and the second release then frees the surface it
// is drawing into -- "buffer_release_callback: runtime check failed:
// (impl->staging_cairo_surface != cairo_surface)", then a cairo reference
// count assertion, and the program aborts.
//
// GTK gets there by itself. It attaches a frame's buffer and holds the commit
// back while its window's updates are frozen; opening a popover creates a
// subsurface and the parent is committed straight away, pending buffer and
// all; then GTK commits that buffer again. On a desktop whose frames arrive
// late -- an A10X iPad, a desktop the app has just come back to -- the frozen
// window is the usual case, and wf-panel died when its menu was opened (bip,
// 2026-10-02: wl_buffer#50 committed at 1905194 and 1905215, released twice
// at 1905876, GTK aborted).
//
// So for a buffer made from a wl_shm_pool, the commits that carried it are
// counted, and the program is told of its release once, with the last one:
// the buffer is free when every commit is done with it, which is when this
// says so. A buffer committed once is released exactly as before. Buffers
// that are not shm (EGL's dma-bufs, which wlroots holds while they are shown
// and releases once) are left alone.
//
// Built in the guest (build.sh), like the pixman shim beside it.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// libwayland's ABI, declared here so this builds with nothing but a C
// compiler (wayland-util.h and wayland-client-core.h have these, unchanged
// since 1.0; wl_proxy_marshal_array_flags since 1.20).
struct wl_proxy;
struct wl_object;
typedef int32_t wl_fixed_t;
struct wl_array { size_t size, alloc; void *data; };
struct wl_message { const char *name, *signature; const struct wl_interface **types; };
struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const struct wl_message *methods;
    int event_count;
    const struct wl_message *events;
};
union wl_argument {
    int32_t i;
    uint32_t u;
    wl_fixed_t f;
    const char *s;
    struct wl_object *o;
    uint32_t n;
    struct wl_array *a;
    int32_t h;
};

// wl_surface and wl_shm_pool request opcodes, and wl_buffer's.
enum { SURFACE_DESTROY = 0, SURFACE_ATTACH = 1, SURFACE_COMMIT = 6 };
enum { SHM_POOL_CREATE_BUFFER = 0 };
enum { BUFFER_DESTROY = 0 };

struct buffer_listener { void (*release)(void *data, struct wl_proxy *buffer); };

struct guarded {
    struct wl_proxy *proxy;
    bool listening;                     // the program's listener is ours to call
    const struct buffer_listener *listener;
    void *data;
    unsigned outstanding;               // commits not yet released
};

struct surface_state {
    struct wl_proxy *proxy;
    struct wl_proxy *pending;           // attached, not yet committed
    bool attached;
};

static pthread_mutex_t guard_lock = PTHREAD_MUTEX_INITIALIZER;
static struct guarded **buffers;
static size_t nbuffers, capbuffers;
static struct surface_state *surfaces;
static size_t nsurfaces, capsurfaces;

static struct wl_proxy *(*real_marshal_array_flags)(struct wl_proxy *, uint32_t,
        const struct wl_interface *, uint32_t, uint32_t, union wl_argument *);
static void (*real_marshal_array)(struct wl_proxy *, uint32_t, union wl_argument *);
static int (*real_add_listener)(struct wl_proxy *, void (**)(void), void *);
static void *(*real_get_user_data)(struct wl_proxy *);
static void (*real_set_user_data)(struct wl_proxy *, void *);

// The real libwayland-client functions. RTLD_NEXT finds them when
// libwayland-client is in the global scope, as it is for a program linked
// against it (GTK). Qt loads its Wayland platform plugin with dlopen and
// RTLD_LOCAL, so libwayland-client is NOT global there -- the plugin's calls
// still bind to these definitions, preloaded, but RTLD_NEXT finds nothing,
// and the first one jumped to address 0: kclock crashed at start (bip,
// 2026-10-03). So it falls back to the library's own handle, which
// RTLD_NOLOAD finds however it was loaded.
static void *wl_sym(const char *name) {
    void *sym = dlsym(RTLD_NEXT, name);
    if (sym != NULL)
        return sym;
    void *lib = dlopen("libwayland-client.so.0", RTLD_LAZY | RTLD_NOLOAD);
    if (lib == NULL)
        lib = dlopen("libwayland-client.so.0", RTLD_LAZY);
    return lib != NULL ? dlsym(lib, name) : NULL;
}

static void resolve_once(void) {
    real_marshal_array = wl_sym("wl_proxy_marshal_array");
    real_add_listener = wl_sym("wl_proxy_add_listener");
    real_get_user_data = wl_sym("wl_proxy_get_user_data");
    real_set_user_data = wl_sym("wl_proxy_set_user_data");
    real_marshal_array_flags = wl_sym("wl_proxy_marshal_array_flags");
    if (real_marshal_array == NULL || real_add_listener == NULL || real_get_user_data == NULL ||
            real_set_user_data == NULL || real_marshal_array_flags == NULL) {
        static const char msg[] = "ish_wl_release_guard: libwayland-client not found\n";
        (void) !write(2, msg, sizeof(msg) - 1);
        abort();
    }
}

static pthread_once_t resolved = PTHREAD_ONCE_INIT;

static void resolve(void) {
    pthread_once(&resolved, resolve_once);
}

// A proxy begins with its wl_object, which begins with its interface: part of
// libwayland's ABI (struct wl_proxy { struct wl_object object; ... }).
static const struct wl_interface *proxy_interface(struct wl_proxy *proxy) {
    return proxy != NULL ? *(const struct wl_interface *const *) proxy : NULL;
}

static bool proxy_is(struct wl_proxy *proxy, const char *name) {
    const struct wl_interface *iface = proxy_interface(proxy);
    return iface != NULL && iface->name != NULL && strcmp(iface->name, name) == 0;
}

// Caller holds guard_lock.
static struct guarded *find_buffer(struct wl_proxy *proxy, size_t *index) {
    for (size_t i = 0; i < nbuffers; i++) {
        if (buffers[i]->proxy == proxy) {
            if (index != NULL)
                *index = i;
            return buffers[i];
        }
    }
    return NULL;
}

static struct surface_state *find_surface(struct wl_proxy *proxy, bool create) {
    for (size_t i = 0; i < nsurfaces; i++)
        if (surfaces[i].proxy == proxy)
            return &surfaces[i];
    if (!create)
        return NULL;
    if (nsurfaces == capsurfaces) {
        size_t cap = capsurfaces ? capsurfaces * 2 : 32;
        struct surface_state *grown = realloc(surfaces, cap * sizeof(*grown));
        if (grown == NULL)
            return NULL;
        surfaces = grown;
        capsurfaces = cap;
    }
    surfaces[nsurfaces] = (struct surface_state) {.proxy = proxy};
    return &surfaces[nsurfaces++];
}

static void forget_surface(struct wl_proxy *proxy) {
    for (size_t i = 0; i < nsurfaces; i++) {
        if (surfaces[i].proxy == proxy) {
            surfaces[i] = surfaces[--nsurfaces];
            return;
        }
    }
}

static void track_buffer(struct wl_proxy *proxy) {
    struct guarded *g = calloc(1, sizeof(*g));
    if (g == NULL)
        return;
    g->proxy = proxy;
    pthread_mutex_lock(&guard_lock);
    if (nbuffers == capbuffers) {
        size_t cap = capbuffers ? capbuffers * 2 : 64;
        struct guarded **grown = realloc(buffers, cap * sizeof(*grown));
        if (grown == NULL) {
            pthread_mutex_unlock(&guard_lock);
            free(g);
            return;
        }
        buffers = grown;
        capbuffers = cap;
    }
    buffers[nbuffers++] = g;
    pthread_mutex_unlock(&guard_lock);
}

// A request is about to go out: what it does to the counts.
static void before_request(struct wl_proxy *proxy, uint32_t opcode, union wl_argument *args) {
    if (proxy_is(proxy, "wl_surface")) {
        pthread_mutex_lock(&guard_lock);
        if (opcode == SURFACE_ATTACH) {
            struct surface_state *s = find_surface(proxy, true);
            if (s != NULL) {
                s->pending = (struct wl_proxy *) args[0].o;
                s->attached = true;
            }
        } else if (opcode == SURFACE_COMMIT) {
            struct surface_state *s = find_surface(proxy, false);
            if (s != NULL && s->attached) {
                struct guarded *g = s->pending != NULL ? find_buffer(s->pending, NULL) : NULL;
                if (g != NULL)
                    g->outstanding++;
                s->pending = NULL;
                s->attached = false;
            }
        } else if (opcode == SURFACE_DESTROY) {
            forget_surface(proxy);
        }
        pthread_mutex_unlock(&guard_lock);
    } else if (opcode == BUFFER_DESTROY && proxy_is(proxy, "wl_buffer")) {
        pthread_mutex_lock(&guard_lock);
        size_t i;
        struct guarded *g = find_buffer(proxy, &i);
        if (g != NULL) {
            buffers[i] = buffers[--nbuffers];
            free(g);
        }
        // A surface still holding it as pending must not count it later.
        for (size_t j = 0; j < nsurfaces; j++)
            if (surfaces[j].pending == proxy)
                surfaces[j].pending = NULL;
        pthread_mutex_unlock(&guard_lock);
    }
}

static void after_request(struct wl_proxy *proxy, uint32_t opcode, struct wl_proxy *created) {
    if (created != NULL && opcode == SHM_POOL_CREATE_BUFFER && proxy_is(proxy, "wl_shm_pool"))
        track_buffer(created);
}

// Turns a request's variadic arguments into the array libwayland marshals,
// from the request's signature, as libwayland's own wl_argument_from_va_list
// does.
static void args_from_va_list(struct wl_proxy *proxy, uint32_t opcode, union wl_argument *args,
        int max, va_list ap) {
    const struct wl_interface *iface = proxy_interface(proxy);
    const char *sig = iface != NULL && (int) opcode < iface->method_count
            ? iface->methods[opcode].signature : "";
    int i = 0;
    for (; *sig != '\0' && i < max; sig++) {
        switch (*sig) {
            case 'i': args[i++].i = va_arg(ap, int32_t); break;
            case 'u': args[i++].u = va_arg(ap, uint32_t); break;
            case 'f': args[i++].f = va_arg(ap, wl_fixed_t); break;
            case 's': args[i++].s = va_arg(ap, const char *); break;
            case 'o': args[i++].o = va_arg(ap, struct wl_object *); break;
            case 'n': args[i++].o = va_arg(ap, struct wl_object *); break;
            case 'a': args[i++].a = va_arg(ap, struct wl_array *); break;
            case 'h': args[i++].h = va_arg(ap, int32_t); break;
            default: break;   // '?' and version digits
        }
    }
}

#define MAX_ARGS 20   // WL_CLOSURE_MAX_ARGS

// libwayland's own entry points can reach each other through the PLT -- the
// legacy wl_proxy_marshal_array goes on to wl_proxy_marshal_array_flags -- and
// so through this file twice. Only the outermost call counts.
static __thread int in_request;

struct wl_proxy *wl_proxy_marshal_array_flags(struct wl_proxy *proxy, uint32_t opcode,
        const struct wl_interface *interface, uint32_t version, uint32_t flags,
        union wl_argument *args) {
    resolve();
    if (in_request)
        return real_marshal_array_flags(proxy, opcode, interface, version, flags, args);
    in_request++;
    before_request(proxy, opcode, args);
    struct wl_proxy *created = real_marshal_array_flags(proxy, opcode, interface, version, flags, args);
    after_request(proxy, opcode, created);
    in_request--;
    return created;
}

struct wl_proxy *wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
        const struct wl_interface *interface, uint32_t version, uint32_t flags, ...) {
    union wl_argument args[MAX_ARGS];
    memset(args, 0, sizeof(args));
    va_list ap;
    va_start(ap, flags);
    args_from_va_list(proxy, opcode, args, MAX_ARGS, ap);
    va_end(ap);
    return wl_proxy_marshal_array_flags(proxy, opcode, interface, version, flags, args);
}

void wl_proxy_marshal_array(struct wl_proxy *proxy, uint32_t opcode, union wl_argument *args) {
    resolve();
    if (in_request) {
        real_marshal_array(proxy, opcode, args);
        return;
    }
    in_request++;
    before_request(proxy, opcode, args);
    real_marshal_array(proxy, opcode, args);
    in_request--;
}

void wl_proxy_marshal(struct wl_proxy *proxy, uint32_t opcode, ...) {
    union wl_argument args[MAX_ARGS];
    memset(args, 0, sizeof(args));
    va_list ap;
    va_start(ap, opcode);
    args_from_va_list(proxy, opcode, args, MAX_ARGS, ap);
    va_end(ap);
    wl_proxy_marshal_array(proxy, opcode, args);
}

static void guarded_release(void *data, struct wl_proxy *buffer) {
    struct guarded *g = data;
    pthread_mutex_lock(&guard_lock);
    if (g->outstanding > 1) {
        g->outstanding--;
        pthread_mutex_unlock(&guard_lock);
        return;
    }
    g->outstanding = 0;
    const struct buffer_listener *listener = g->listener;
    void *user = g->data;
    pthread_mutex_unlock(&guard_lock);
    if (listener != NULL && listener->release != NULL)
        listener->release(user, buffer);
}

static const struct buffer_listener guard_listener = {guarded_release};

int wl_proxy_add_listener(struct wl_proxy *proxy, void (**implementation)(void), void *data) {
    resolve();
    pthread_mutex_lock(&guard_lock);
    struct guarded *g = find_buffer(proxy, NULL);
    if (g != NULL && !g->listening) {
        g->listening = true;
        g->listener = (const struct buffer_listener *) implementation;
        g->data = data;
        pthread_mutex_unlock(&guard_lock);
        return real_add_listener(proxy, (void (**)(void)) &guard_listener, g);
    }
    pthread_mutex_unlock(&guard_lock);
    return real_add_listener(proxy, implementation, data);
}

// The program's own data, not ours, for a buffer it listens to through us.
void *wl_proxy_get_user_data(struct wl_proxy *proxy) {
    resolve();
    pthread_mutex_lock(&guard_lock);
    struct guarded *g = find_buffer(proxy, NULL);
    if (g != NULL && g->listening) {
        void *data = g->data;
        pthread_mutex_unlock(&guard_lock);
        return data;
    }
    pthread_mutex_unlock(&guard_lock);
    return real_get_user_data(proxy);
}

void wl_proxy_set_user_data(struct wl_proxy *proxy, void *user_data) {
    resolve();
    pthread_mutex_lock(&guard_lock);
    struct guarded *g = find_buffer(proxy, NULL);
    if (g != NULL && g->listening) {
        g->data = user_data;
        pthread_mutex_unlock(&guard_lock);
        return;
    }
    pthread_mutex_unlock(&guard_lock);
    real_set_user_data(proxy, user_data);
}
