// wl-present -- hands the Wayland desktop's frames straight to the app (#484).
//
// A native program (kernel/native.c): host code on the guest task's thread,
// talking to the guest's compositor through the libc shim like any guest
// client would. start-wayland.sh runs it when the compositor draws on the GPU.
//
// Each frame, it asks the compositor (wlr-screencopy, with damage) to copy the
// output into one of two buffers of its own -- DRM dumb buffers on the render
// node, handed over as dma-bufs, so the copy is the compositor's GPU blit and
// the result is host shared memory. Then virtgpu_present_fd (fs/virtgpu.c)
// gives that memory to the app, which draws it with Metal. No pixel passes
// through emulated code.
//
// It also carries the app's side of the desktop: pointer and keyboard (the
// compositor's virtual pointer and keyboard), the clipboard (wlr-data-control)
// and the desktop's size (wlr-output-management), arriving from the app on a
// kernel descriptor (virtgpu_presenter_input). With all of that here, wayvnc
// has nothing left to do once the app takes frames, and it screen-copies for
// as long as a client is connected; so wl-present then writes "detach" on its
// standard output, and "attach" when it ends, and start-wayland.sh passes
// those to wayvncctl. The app's VNC connection stays up throughout, for the
// app to fall back on.
//
// The Wayland client is written out here rather than linked: the handful of
// requests this needs is less code than a libwayland built for the host, and
// the protocols are stable. Everything is in one struct -- a native program is
// a function the app may call again, so no state may outlive a run
// (kernel/native.h), least of all a descriptor.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "fs/virtgpu.h"

// Object ids this client creates first; the rest are numbered from NEXT_ID.
enum { ID_DISPLAY = 1, ID_REGISTRY = 2, ID_SYNC = 3, NEXT_ID = 4 };

// Requests and events, by interface.
enum {
    DISPLAY_SYNC = 0, DISPLAY_GET_REGISTRY = 1,
    DISPLAY_EV_ERROR = 0,
    REGISTRY_BIND = 0,
    REGISTRY_EV_GLOBAL = 0,
    OUTPUT_EV_NAME = 4,
    SCREENCOPY_CAPTURE_OUTPUT = 0,
    FRAME_COPY = 0, FRAME_DESTROY = 1, FRAME_COPY_WITH_DAMAGE = 2,
    FRAME_EV_BUFFER = 0, FRAME_EV_FLAGS = 1, FRAME_EV_READY = 2, FRAME_EV_FAILED = 3,
    FRAME_EV_DAMAGE = 4, FRAME_EV_LINUX_DMABUF = 5, FRAME_EV_BUFFER_DONE = 6,
    DMABUF_CREATE_PARAMS = 1,
    PARAMS_DESTROY = 0, PARAMS_ADD = 1, PARAMS_CREATE_IMMED = 3,
    BUFFER_DESTROY = 0,
    // zwlr_virtual_pointer_manager_v1, zwlr_virtual_pointer_v1
    VPTR_MGR_CREATE = 0, VPTR_MGR_CREATE_WITH_OUTPUT = 2,
    VPTR_MOTION_ABSOLUTE = 1, VPTR_BUTTON = 2, VPTR_FRAME = 4, VPTR_AXIS_SOURCE = 5,
    VPTR_AXIS_DISCRETE = 7,
    // zwp_virtual_keyboard_manager_v1, zwp_virtual_keyboard_v1
    VKBD_MGR_CREATE = 0,
    VKBD_KEYMAP = 0, VKBD_KEY = 1, VKBD_MODIFIERS = 2,
    // zwlr_data_control_manager_v1 and friends
    DC_MGR_CREATE_SOURCE = 0, DC_MGR_GET_DEVICE = 1,
    DC_DEVICE_SET_SELECTION = 0,
    DC_DEVICE_EV_DATA_OFFER = 0, DC_DEVICE_EV_SELECTION = 1, DC_DEVICE_EV_FINISHED = 2,
    DC_SOURCE_OFFER = 0, DC_SOURCE_DESTROY = 1,
    DC_SOURCE_EV_SEND = 0, DC_SOURCE_EV_CANCELLED = 1,
    DC_OFFER_RECEIVE = 0, DC_OFFER_DESTROY = 1,
    DC_OFFER_EV_OFFER = 0,
    // zwlr_output_manager_v1 and friends
    OM_CREATE_CONFIGURATION = 0,
    OM_EV_HEAD = 0, OM_EV_DONE = 1,
    HEAD_EV_NAME = 0, HEAD_EV_ENABLED = 4, HEAD_EV_FINISHED = 9,
    CFG_ENABLE_HEAD = 0, CFG_DISABLE_HEAD = 1, CFG_APPLY = 2, CFG_DESTROY = 4,
    CFG_EV_SUCCEEDED = 0, CFG_EV_FAILED = 1, CFG_EV_CANCELLED = 2,
    CFG_HEAD_SET_CUSTOM_MODE = 1,
};

// Text types a clipboard offer may come in, best first; a source of ours
// offers them all.
static const char *const text_mimes[] = {
    "text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING",
};
#define TEXT_MIMES ((int) (sizeof(text_mimes) / sizeof(text_mimes[0])))
#define CLIP_MAX (4u << 20)

// The buffers come straight from the kernel (virtgpu_presenter_buffer), not
// through DRM ioctls: the shim passes only terminal ioctls, and this program
// is kernel-resident code anyway.
struct wp_buffer {
    int fd;                     // a dma-buf of host shared memory
    uint32_t stride;
    uint32_t id;                // wl_buffer
};

struct wp_head {
    uint32_t id;
    char name[64];
    bool enabled;
};

struct wp_key {
    uint32_t keysym, code;
    bool shifted;               // Shift was pressed for it and goes up with it
};

struct wp {
    int sock, drm;
    uint32_t next_id;
    // what the compositor offers
    uint32_t output_name, screencopy_name, dmabuf_name;
    uint32_t output_version, screencopy_version;
    uint32_t seat_name, vptr_mgr_name, vkbd_mgr_name, dc_mgr_name, om_name;
    uint32_t vptr_mgr_version, om_version;
    uint32_t output, screencopy, dmabuf;
    char output_label[64];      // wl_output's name, for finding its head
    // the frame in flight
    uint32_t frame;
    uint32_t format, width, height;
    bool have_dmabuf, buffer_done, ready, failed;
    bool fatal;                 // a protocol error: the connection is over
    int32_t dx0, dy0, dx1, dy1;     // damage, as a bounding box
    // two buffers, alternated
    struct wp_buffer buf[2];
    uint32_t buf_width, buf_height, buf_format;
    // incoming bytes, and the descriptors that came with them
    uint8_t in[16384];
    size_t in_len;
    int in_fds[32];
    int in_fd_count;
    bool synced, quiet;
    bool watched;       // the app took the last frame
    bool detached;      // wayvnc was told to let go of the compositor
    uint32_t display;   // the desktop's VNC port, naming it to the app

    // the app's input (virtgpu_presenter_input)
    int input;
    uint8_t *ibuf;
    size_t ilen, icap;
    uint32_t seat, vptr, vkbd;
    uint32_t buttons;           // the RFB button mask last sent
    uint32_t mods_depressed, mods_locked, mods_sent_depressed, mods_sent_locked;
    struct wp_key keys[16];
    int key_count;

    // the clipboard
    uint32_t dc_mgr, dc_device;
    uint32_t sources[8];        // ours, the newest holding clip
    int source_count;
    char *clip;
    size_t clip_len;
    uint32_t offer_new, offer_sel;
    int offer_mime;             // best of text_mimes offer_new has, or -1
    int clip_in;                // reading the selection, or -1
    char *clip_in_buf;
    size_t clip_in_len;

    // the desktop's size
    uint32_t om, om_serial, cfg;
    struct wp_head heads[8];
    int head_count;
    uint32_t want_width, want_height;   // asked for and not yet applied
    uint32_t cfg_width, cfg_height;     // in the configuration in flight
};

static void wp_log(struct wp *w, const char *fmt, ...) __attribute__((format(__printf__, 2, 3)));
static void wp_log(struct wp *w, const char *fmt, ...) {
    if (w->quiet)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "wl-present: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t) ((uint64_t) ts.tv_sec * 1000 + (uint64_t) ts.tv_nsec / 1000000);
}

// ---- the wire ----------------------------------------------------------------

struct msg {
    uint32_t w[64];
    size_t n;
};

static void msg_u32(struct msg *m, uint32_t v) {
    if (m->n < 64)
        m->w[m->n++] = v;
}

static void msg_str(struct msg *m, const char *s) {
    size_t len = strlen(s) + 1;
    msg_u32(m, (uint32_t) len);
    size_t words = (len + 3) / 4;
    if (m->n + words > 64)
        return;
    memset(&m->w[m->n], 0, words * 4);
    memcpy(&m->w[m->n], s, len);
    m->n += words;
}

// Send a request, with one descriptor attached when fd >= 0.
static int wp_send(struct wp *w, uint32_t object, uint16_t opcode, struct msg *args, int fd) {
    uint32_t buf[66];
    size_t n = args != NULL ? args->n : 0;
    buf[0] = object;
    buf[1] = (uint32_t) ((8 + n * 4) << 16) | opcode;
    if (n > 0)
        memcpy(&buf[2], args->w, n * 4);
    struct iovec iov = {.iov_base = buf, .iov_len = 8 + n * 4};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};
    union {
        struct cmsghdr h;
        char space[CMSG_SPACE(sizeof(int))];
    } cmsg;
    if (fd >= 0) {
        memset(&cmsg, 0, sizeof(cmsg));
        mh.msg_control = &cmsg;
        mh.msg_controllen = sizeof(cmsg.space);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    ssize_t sent = sendmsg(w->sock, &mh, MSG_NOSIGNAL);
    return sent == (ssize_t) iov.iov_len ? 0 : -1;
}

// Requests with only integer arguments, the common case.
static void wp_send_u32(struct wp *w, uint32_t object, uint16_t opcode, int count, ...) {
    struct msg m = {.n = 0};
    va_list ap;
    va_start(ap, count);
    for (int i = 0; i < count; i++)
        msg_u32(&m, va_arg(ap, uint32_t));
    va_end(ap);
    wp_send(w, object, opcode, &m, -1);
}

static const char *arg_str(const uint32_t *args, size_t words, size_t *at) {
    if (*at >= words)
        return "";
    uint32_t len = args[(*at)++];
    const char *s = (const char *) &args[*at];
    *at += (len + 3) / 4;
    if (*at > words || len == 0)
        return "";
    return s;
}

// The next descriptor that came in, for an event that carries one.
static int take_fd(struct wp *w) {
    if (w->in_fd_count == 0)
        return -1;
    int fd = w->in_fds[0];
    memmove(&w->in_fds[0], &w->in_fds[1], (size_t) (w->in_fd_count - 1) * sizeof(int));
    w->in_fd_count--;
    return fd;
}

static uint32_t wp_bind(struct wp *w, uint32_t name, const char *iface, uint32_t version) {
    uint32_t id = w->next_id++;
    struct msg m = {.n = 0};
    msg_u32(&m, name);
    msg_str(&m, iface);
    msg_u32(&m, version);
    msg_u32(&m, id);
    wp_send(w, ID_REGISTRY, REGISTRY_BIND, &m, -1);
    return id;
}

static uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

// ---- the desktop's size ------------------------------------------------------

static struct wp_head *head_find(struct wp *w, uint32_t id) {
    for (int i = 0; i < w->head_count; i++)
        if (w->heads[i].id == id)
            return &w->heads[i];
    return NULL;
}

// Apply the size asked for, when no configuration is in flight and the
// compositor has described its outputs. Every head must be named in a
// configuration: the output's gets the new mode, the others stay as they are.
static void resize_try(struct wp *w) {
    if (w->want_width == 0 || w->cfg != 0 || w->om_serial == 0 || w->head_count == 0)
        return;
    int target = -1;
    for (int i = 0; i < w->head_count; i++)
        if (w->heads[i].enabled && (w->output_label[0] == '\0' || !strcmp(w->heads[i].name, w->output_label))) {
            target = i;
            break;
        }
    if (target < 0)
        return;
    w->cfg = w->next_id++;
    wp_send_u32(w, w->om, OM_CREATE_CONFIGURATION, 2, w->cfg, w->om_serial);
    for (int i = 0; i < w->head_count; i++) {
        struct wp_head *h = &w->heads[i];
        if (!h->enabled) {
            wp_send_u32(w, w->cfg, CFG_DISABLE_HEAD, 1, h->id);
            continue;
        }
        uint32_t cfg_head = w->next_id++;
        wp_send_u32(w, w->cfg, CFG_ENABLE_HEAD, 2, cfg_head, h->id);
        if (i == target)
            wp_send_u32(w, cfg_head, CFG_HEAD_SET_CUSTOM_MODE, 3, w->want_width, w->want_height, 0);
    }
    wp_send(w, w->cfg, CFG_APPLY, NULL, -1);
    w->cfg_width = w->want_width;
    w->cfg_height = w->want_height;
}

static void resize_done(struct wp *w, uint16_t opcode) {
    wp_send(w, w->cfg, CFG_DESTROY, NULL, -1);
    w->cfg = 0;
    if (opcode == CFG_EV_FAILED)
        wp_log(w, "the compositor refused %ux%u", w->cfg_width, w->cfg_height);
    // Cancelled (the outputs changed meanwhile): try again with the new
    // serial. Otherwise it is settled, unless another size came since.
    if (opcode != CFG_EV_CANCELLED && w->want_width == w->cfg_width && w->want_height == w->cfg_height)
        w->want_width = w->want_height = 0;
    resize_try(w);
}

// ---- the clipboard -----------------------------------------------------------

// The app's clipboard becomes the desktop's: a source of ours, offering text.
static void clip_set(struct wp *w, const uint8_t *text, size_t len) {
    if (w->dc_device == 0)
        return;
    char *copy = malloc(len + 1);
    if (copy == NULL)
        return;
    memcpy(copy, text, len);
    copy[len] = '\0';
    free(w->clip);
    w->clip = copy;
    w->clip_len = len;
    // Earlier sources are cancelled by this one, and destroyed then.
    if (w->source_count == (int) (sizeof(w->sources) / sizeof(w->sources[0])))
        return;
    uint32_t source = w->next_id++;
    w->sources[w->source_count++] = source;
    wp_send_u32(w, w->dc_mgr, DC_MGR_CREATE_SOURCE, 1, source);
    for (int i = 0; i < TEXT_MIMES; i++) {
        struct msg m = {.n = 0};
        msg_str(&m, text_mimes[i]);
        wp_send(w, source, DC_SOURCE_OFFER, &m, -1);
    }
    wp_send_u32(w, w->dc_device, DC_DEVICE_SET_SELECTION, 1, source);
}

static int source_find(struct wp *w, uint32_t id) {
    for (int i = 0; i < w->source_count; i++)
        if (w->sources[i] == id)
            return i;
    return -1;
}

// Someone pasting what we hold.
static void clip_send(struct wp *w, int fd) {
    if (fd < 0)
        return;
    size_t at = 0;
    while (w->clip != NULL && at < w->clip_len) {
        ssize_t n = write(fd, w->clip + at, w->clip_len - at);
        if (n <= 0 && errno != EINTR)
            break;
        if (n > 0)
            at += (size_t) n;
    }
    close(fd);
}

static void clip_in_stop(struct wp *w) {
    if (w->clip_in >= 0)
        close(w->clip_in);
    w->clip_in = -1;
    free(w->clip_in_buf);
    w->clip_in_buf = NULL;
    w->clip_in_len = 0;
}

// The desktop's selection changed: read it, to give the app. Only once
// wayvnc is detached -- until then it hands the app the clipboard itself.
static void clip_receive(struct wp *w, uint32_t offer) {
    if (!w->detached || offer == 0 || offer != w->offer_new || w->offer_mime < 0)
        return;
    int fds[2];
    if (pipe(fds) != 0)
        return;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    struct msg m = {.n = 0};
    msg_str(&m, text_mimes[w->offer_mime]);
    wp_send(w, offer, DC_OFFER_RECEIVE, &m, fds[1]);
    close(fds[1]);
    clip_in_stop(w);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    w->clip_in = fds[0];
}

static void clip_in_read(struct wp *w) {
    char chunk[16384];
    ssize_t n = read(w->clip_in, chunk, sizeof(chunk));
    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    if (n > 0) {
        if (w->clip_in_len + (size_t) n > CLIP_MAX) {
            clip_in_stop(w);
            return;
        }
        char *grown = realloc(w->clip_in_buf, w->clip_in_len + (size_t) n);
        if (grown == NULL) {
            clip_in_stop(w);
            return;
        }
        memcpy(grown + w->clip_in_len, chunk, (size_t) n);
        w->clip_in_buf = grown;
        w->clip_in_len += (size_t) n;
        return;
    }
    // The end: hand it over, unless it is what the app just gave us.
    if (!(w->clip != NULL && w->clip_len == w->clip_in_len &&
          memcmp(w->clip, w->clip_in_buf != NULL ? w->clip_in_buf : "", w->clip_len) == 0))
        virtgpu_presenter_clipboard(w->display, w->clip_in_buf != NULL ? w->clip_in_buf : "", w->clip_in_len);
    clip_in_stop(w);
}

// ---- pointer and keyboard ----------------------------------------------------

// The us keymap wayvnc also uses; the compositor's xkbcommon expands it.
static const char keymap_text[] =
    "xkb_keymap {\n"
    "  xkb_keycodes { include \"evdev+aliases(qwerty)\" };\n"
    "  xkb_types { include \"complete\" };\n"
    "  xkb_compat { include \"complete\" };\n"
    "  xkb_symbols { include \"pc+us+inet(evdev)\" };\n"
    "};\n";

enum { MOD_SHIFT = 1, MOD_LOCK = 2, MOD_CONTROL = 4, MOD_ALT = 8, MOD_SUPER = 64 };
enum { KEY_LEFTSHIFT = 42, KEY_CAPSLOCK = 58 };

// An X keysym as a Linux key code on the us layout, and whether it takes
// Shift. What the layout cannot type is dropped, as wayvnc drops it.
static bool keysym_key(uint32_t sym, uint32_t *code, bool *shift) {
    static const char plain[] = "1234567890-=qwertyuiop[]asdfghjkl;'`\\zxcvbnm,./";
    static const char shifted[] = "!@#$%^&*()_+QWERTYUIOP{}ASDFGHJKL:\"~|ZXCVBNM<>?";
    static const uint8_t codes[] = {
        2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
        30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 43,
        44, 45, 46, 47, 48, 49, 50, 51, 52, 53,
    };
    if (sym >= 0x1000020 && sym < 0x100007f)
        sym -= 0x1000000;       // Unicode keysyms for ASCII
    *shift = false;
    if (sym == ' ') {
        *code = 57;
        return true;
    }
    if (sym > ' ' && sym < 0x7f) {
        const char *p = strchr(plain, (int) sym);
        if (p != NULL) {
            *code = codes[p - plain];
            return true;
        }
        p = strchr(shifted, (int) sym);
        if (p != NULL) {
            *code = codes[p - shifted];
            *shift = true;
            return true;
        }
        return false;
    }
    if (sym >= 0xffbe && sym <= 0xffc7) {   // F1-F10
        *code = 59 + (sym - 0xffbe);
        return true;
    }
    switch (sym) {
        case 0xffc8: *code = 87; return true;   // F11
        case 0xffc9: *code = 88; return true;   // F12
        case 0xff08: *code = 14; return true;   // BackSpace
        case 0xff09: *code = 15; return true;   // Tab
        case 0xfe20: *code = 15; *shift = true; return true;   // ISO_Left_Tab
        case 0xff0d: *code = 28; return true;   // Return
        case 0xff8d: *code = 96; return true;   // KP_Enter
        case 0xff1b: *code = 1; return true;    // Escape
        case 0xffff: *code = 111; return true;  // Delete
        case 0xff63: *code = 110; return true;  // Insert
        case 0xff50: *code = 102; return true;  // Home
        case 0xff57: *code = 107; return true;  // End
        case 0xff55: *code = 104; return true;  // Prior
        case 0xff56: *code = 109; return true;  // Next
        case 0xff51: *code = 105; return true;  // Left
        case 0xff52: *code = 103; return true;  // Up
        case 0xff53: *code = 106; return true;  // Right
        case 0xff54: *code = 108; return true;  // Down
        case 0xff61: *code = 99; return true;   // Print
        case 0xff13: *code = 119; return true;  // Pause
        case 0xff14: *code = 70; return true;   // Scroll_Lock
        case 0xff7f: *code = 69; return true;   // Num_Lock
        case 0xff67: *code = 127; return true;  // Menu
        case 0xffe1: *code = 42; return true;   // Shift_L
        case 0xffe2: *code = 54; return true;   // Shift_R
        case 0xffe3: *code = 29; return true;   // Control_L
        case 0xffe4: *code = 97; return true;   // Control_R
        case 0xffe5: *code = 58; return true;   // Caps_Lock
        case 0xffe9: *code = 56; return true;   // Alt_L
        case 0xffea: case 0xfe03: *code = 100; return true;    // Alt_R, ISO_Level3_Shift
        case 0xffe7: case 0xffeb: *code = 125; return true;    // Meta_L, Super_L
        case 0xffe8: case 0xffec: *code = 126; return true;    // Meta_R, Super_R
    }
    return false;
}

static uint32_t key_modifier(uint32_t code) {
    switch (code) {
        case 42: case 54: return MOD_SHIFT;
        case 29: case 97: return MOD_CONTROL;
        case 56: case 100: return MOD_ALT;
        case 125: case 126: return MOD_SUPER;
    }
    return 0;
}

// wlroots' virtual keyboard takes the modifier state only as told, so it is
// worked out from the keys held and sent whenever it changes.
static void mods_update(struct wp *w) {
    uint32_t depressed = 0;
    for (int i = 0; i < w->key_count; i++) {
        depressed |= key_modifier(w->keys[i].code);
        if (w->keys[i].shifted)
            depressed |= MOD_SHIFT;
    }
    w->mods_depressed = depressed;
    if (depressed == w->mods_sent_depressed && w->mods_locked == w->mods_sent_locked)
        return;
    wp_send_u32(w, w->vkbd, VKBD_MODIFIERS, 4, depressed, 0, w->mods_locked, 0);
    w->mods_sent_depressed = depressed;
    w->mods_sent_locked = w->mods_locked;
}

static void key_event(struct wp *w, uint32_t keysym, bool down) {
    if (w->vkbd == 0)
        return;
    uint32_t t = now_ms();
    if (!down) {
        for (int i = 0; i < w->key_count; i++) {
            if (w->keys[i].keysym != keysym)
                continue;
            struct wp_key k = w->keys[i];
            memmove(&w->keys[i], &w->keys[i + 1], (size_t) (w->key_count - i - 1) * sizeof(k));
            w->key_count--;
            wp_send_u32(w, w->vkbd, VKBD_KEY, 3, t, k.code, 0);
            if (k.shifted)
                wp_send_u32(w, w->vkbd, VKBD_KEY, 3, t, KEY_LEFTSHIFT, 0);
            mods_update(w);
            return;
        }
        return;
    }
    // A key held down again is the app's repeat; the compositor repeats.
    for (int i = 0; i < w->key_count; i++)
        if (w->keys[i].keysym == keysym)
            return;
    uint32_t code;
    bool shift;
    if (!keysym_key(keysym, &code, &shift) || w->key_count == (int) (sizeof(w->keys) / sizeof(w->keys[0])))
        return;
    // Shift is added for a symbol that needs it, unless it is already held.
    bool add_shift = shift && !(w->mods_depressed & MOD_SHIFT);
    w->keys[w->key_count++] = (struct wp_key) {.keysym = keysym, .code = code, .shifted = add_shift};
    if (code == KEY_CAPSLOCK)
        w->mods_locked ^= MOD_LOCK;
    if (add_shift)
        wp_send_u32(w, w->vkbd, VKBD_KEY, 3, t, KEY_LEFTSHIFT, 1);
    mods_update(w);
    wp_send_u32(w, w->vkbd, VKBD_KEY, 3, t, code, 1);
}

static void keys_release_all(struct wp *w) {
    while (w->key_count > 0)
        key_event(w, w->keys[w->key_count - 1].keysym, false);
}

// An RFB pointer event: position, then what the buttons did, then a frame.
static void pointer_event(struct wp *w, uint32_t x, uint32_t y, uint32_t mask) {
    if (w->vptr == 0 || w->width == 0 || w->height == 0)
        return;
    uint32_t t = now_ms();
    wp_send_u32(w, w->vptr, VPTR_MOTION_ABSOLUTE, 5, t, min_u32(x, w->width - 1), min_u32(y, w->height - 1),
                w->width, w->height);
    static const uint32_t button_codes[3] = {0x110, 0x112, 0x111};  // left, middle, right
    uint32_t changed = mask ^ w->buttons;
    for (int i = 0; i < 3; i++)
        if (changed & (1u << i))
            wp_send_u32(w, w->vptr, VPTR_BUTTON, 3, t, button_codes[i], (mask >> i) & 1);
    // Bits 3-6 are the wheel: up, down, left, right, one step per press.
    for (int i = 3; i < 7; i++) {
        if (!((changed & mask) & (1u << i)))
            continue;
        uint32_t axis = i < 5 ? 0 : 1;
        int32_t step = (i == 3 || i == 5) ? -1 : 1;
        wp_send_u32(w, w->vptr, VPTR_AXIS_SOURCE, 1, 0);
        wp_send_u32(w, w->vptr, VPTR_AXIS_DISCRETE, 4, t, axis, (uint32_t) (step * 15 * 256), (uint32_t) step);
    }
    w->buttons = mask;
    wp_send(w, w->vptr, VPTR_FRAME, NULL, -1);
}

static void input_read(struct wp *w) {
    if (w->icap - w->ilen < 65536) {
        size_t cap = w->icap != 0 ? w->icap * 2 : 131072;
        uint8_t *grown = realloc(w->ibuf, cap);
        if (grown == NULL)
            return;
        w->ibuf = grown;
        w->icap = cap;
    }
    ssize_t n = read(w->input, w->ibuf + w->ilen, w->icap - w->ilen);
    if (n <= 0)
        return;
    w->ilen += (size_t) n;
    size_t at = 0;
    while (w->ilen - at >= sizeof(struct virtgpu_input_event)) {
        struct virtgpu_input_event ev;
        memcpy(&ev, w->ibuf + at, sizeof(ev));
        if (w->ilen - at - sizeof(ev) < ev.len)
            break;
        const uint8_t *data = w->ibuf + at + sizeof(ev);
        switch (ev.type) {
            case VIRTGPU_INPUT_POINTER:
                pointer_event(w, ev.a & 0xffff, ev.a >> 16, ev.b);
                break;
            case VIRTGPU_INPUT_KEY:
                key_event(w, ev.a, ev.b != 0);
                break;
            case VIRTGPU_INPUT_CLIPBOARD:
                clip_set(w, data, ev.len);
                break;
            case VIRTGPU_INPUT_RESIZE:
                if (ev.a >= 64 && ev.b >= 64 && ev.a <= 16384 && ev.b <= 16384) {
                    w->want_width = ev.a;
                    w->want_height = ev.b;
                    resize_try(w);
                }
                break;
        }
        at += sizeof(ev) + ev.len;
    }
    memmove(w->ibuf, w->ibuf + at, w->ilen - at);
    w->ilen -= at;
    // A clipboard bigger than the buffer: make room for the rest of it.
    if (w->ilen >= sizeof(struct virtgpu_input_event)) {
        struct virtgpu_input_event ev;
        memcpy(&ev, w->ibuf, sizeof(ev));
        size_t need = sizeof(ev) + ev.len + 65536;
        if (need > w->icap && ev.len <= (8u << 20)) {
            uint8_t *grown = realloc(w->ibuf, need);
            if (grown != NULL) {
                w->ibuf = grown;
                w->icap = need;
            }
        }
    }
}

// Pointer, keyboard, clipboard and size, as far as the compositor offers
// them; without pointer and keyboard the app keeps sending its input to VNC.
static void input_start(struct wp *w) {
    if (w->seat_name == 0 || w->vptr_mgr_name == 0 || w->vkbd_mgr_name == 0) {
        wp_log(w, "the compositor has no virtual pointer or keyboard: input stays with VNC");
        return;
    }
    w->seat = wp_bind(w, w->seat_name, "wl_seat", 1);
    uint32_t vptr_mgr = wp_bind(w, w->vptr_mgr_name, "zwlr_virtual_pointer_manager_v1", min_u32(w->vptr_mgr_version, 2));
    w->vptr = w->next_id++;
    if (w->vptr_mgr_version >= 2)
        wp_send_u32(w, vptr_mgr, VPTR_MGR_CREATE_WITH_OUTPUT, 3, w->seat, w->output, w->vptr);
    else
        wp_send_u32(w, vptr_mgr, VPTR_MGR_CREATE, 2, w->seat, w->vptr);

    uint32_t vkbd_mgr = wp_bind(w, w->vkbd_mgr_name, "zwp_virtual_keyboard_manager_v1", 1);
    w->vkbd = w->next_id++;
    wp_send_u32(w, vkbd_mgr, VKBD_MGR_CREATE, 2, w->seat, w->vkbd);
    // The keymap goes over as a file the compositor maps.
    char path[256];
    const char *dir = getenv("XDG_RUNTIME_DIR");
    snprintf(path, sizeof(path), "%s/wl-present-keymap-%d", dir != NULL ? dir : "/tmp", (int) getpid());
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd >= 0) {
        unlink(path);
        ssize_t n = write(fd, keymap_text, sizeof(keymap_text));
        if (n == (ssize_t) sizeof(keymap_text)) {
            // format XKB_V1, the descriptor (out of band), the size with its NUL
            struct msg m = {.n = 0};
            msg_u32(&m, 1);
            msg_u32(&m, (uint32_t) sizeof(keymap_text));
            wp_send(w, w->vkbd, VKBD_KEYMAP, &m, fd);
        }
        close(fd);
    }

    if (w->dc_mgr_name != 0) {
        w->dc_mgr = wp_bind(w, w->dc_mgr_name, "zwlr_data_control_manager_v1", 1);
        w->dc_device = w->next_id++;
        wp_send_u32(w, w->dc_mgr, DC_MGR_GET_DEVICE, 2, w->dc_device, w->seat);
    }
    if (w->om_name != 0)
        w->om = wp_bind(w, w->om_name, "zwlr_output_manager_v1", min_u32(w->om_version, 2));

    w->input = virtgpu_presenter_input(w->display);
    if (w->input < 0) {
        wp_log(w, "cannot take the app's input");
        w->input = -1;
    }
}

// ---- events ------------------------------------------------------------------

static void on_event(struct wp *w, uint32_t object, uint16_t opcode, const uint32_t *args, size_t words) {
    if (object == ID_DISPLAY && opcode == DISPLAY_EV_ERROR && words >= 2) {
        size_t at = 2;
        wp_log(w, "protocol error on object %u, code %u: %s", args[0], args[1], arg_str(args, words, &at));
        w->fatal = true;
        return;
    }
    if (object == ID_REGISTRY && opcode == REGISTRY_EV_GLOBAL && words >= 3) {
        size_t at = 1;
        uint32_t name = args[0];
        const char *iface = arg_str(args, words, &at);
        uint32_t version = at < words ? args[at] : 0;
        if (!strcmp(iface, "wl_output") && w->output_name == 0) {
            w->output_name = name;
            w->output_version = version;
        } else if (!strcmp(iface, "zwlr_screencopy_manager_v1") && version >= 3) {
            w->screencopy_name = name;
            w->screencopy_version = 3;
        } else if (!strcmp(iface, "zwp_linux_dmabuf_v1") && version >= 3) {
            w->dmabuf_name = name;
        } else if (!strcmp(iface, "wl_seat") && w->seat_name == 0) {
            w->seat_name = name;
        } else if (!strcmp(iface, "zwlr_virtual_pointer_manager_v1")) {
            w->vptr_mgr_name = name;
            w->vptr_mgr_version = version;
        } else if (!strcmp(iface, "zwp_virtual_keyboard_manager_v1")) {
            w->vkbd_mgr_name = name;
        } else if (!strcmp(iface, "zwlr_data_control_manager_v1")) {
            w->dc_mgr_name = name;
        } else if (!strcmp(iface, "zwlr_output_manager_v1")) {
            w->om_name = name;
            w->om_version = version;
        }
        return;
    }
    if (object == ID_SYNC) {
        w->synced = true;
        return;
    }
    if (object != 0 && object == w->output) {
        if (opcode == OUTPUT_EV_NAME) {
            size_t at = 0;
            snprintf(w->output_label, sizeof(w->output_label), "%s", arg_str(args, words, &at));
        }
        return;
    }
    if (object != 0 && object == w->frame) {
        switch (opcode) {
            case FRAME_EV_LINUX_DMABUF:
                if (words >= 3) {
                    w->format = args[0];
                    w->width = args[1];
                    w->height = args[2];
                    w->have_dmabuf = true;
                }
                break;
            case FRAME_EV_BUFFER_DONE:
                w->buffer_done = true;
                break;
            case FRAME_EV_DAMAGE:
                if (words >= 4) {
                    int32_t x0 = (int32_t) args[0], y0 = (int32_t) args[1];
                    int32_t x1 = x0 + (int32_t) args[2], y1 = y0 + (int32_t) args[3];
                    if (w->dx1 <= w->dx0) {
                        w->dx0 = x0; w->dy0 = y0; w->dx1 = x1; w->dy1 = y1;
                    } else {
                        if (x0 < w->dx0) w->dx0 = x0;
                        if (y0 < w->dy0) w->dy0 = y0;
                        if (x1 > w->dx1) w->dx1 = x1;
                        if (y1 > w->dy1) w->dy1 = y1;
                    }
                }
                break;
            case FRAME_EV_READY:
                w->ready = true;
                break;
            case FRAME_EV_FAILED:
                w->failed = true;
                break;
        }
        return;
    }
    // The clipboard.
    if (object != 0 && object == w->dc_device) {
        if (opcode == DC_DEVICE_EV_DATA_OFFER && words >= 1) {
            if (w->offer_new != 0 && w->offer_new != w->offer_sel)
                wp_send(w, w->offer_new, DC_OFFER_DESTROY, NULL, -1);
            w->offer_new = args[0];
            w->offer_mime = -1;
        } else if (opcode == DC_DEVICE_EV_SELECTION && words >= 1) {
            if (w->offer_sel != 0 && w->offer_sel != args[0])
                wp_send(w, w->offer_sel, DC_OFFER_DESTROY, NULL, -1);
            w->offer_sel = args[0];
            clip_receive(w, args[0]);
        } else if (opcode == DC_DEVICE_EV_FINISHED) {
            w->dc_device = 0;
        }
        return;
    }
    if (object != 0 && object == w->offer_new && opcode == DC_OFFER_EV_OFFER) {
        size_t at = 0;
        const char *mime = arg_str(args, words, &at);
        for (int i = 0; i < TEXT_MIMES; i++)
            if (!strcmp(mime, text_mimes[i]) && (w->offer_mime < 0 || i < w->offer_mime))
                w->offer_mime = i;
        return;
    }
    int source = object != 0 ? source_find(w, object) : -1;
    if (source >= 0) {
        if (opcode == DC_SOURCE_EV_SEND) {
            clip_send(w, take_fd(w));
        } else if (opcode == DC_SOURCE_EV_CANCELLED) {
            wp_send(w, object, DC_SOURCE_DESTROY, NULL, -1);
            w->sources[source] = w->sources[--w->source_count];
        }
        return;
    }
    // The desktop's size.
    if (object != 0 && object == w->om) {
        if (opcode == OM_EV_HEAD && words >= 1 && w->head_count < (int) (sizeof(w->heads) / sizeof(w->heads[0])))
            w->heads[w->head_count++] = (struct wp_head) {.id = args[0]};
        else if (opcode == OM_EV_DONE && words >= 1) {
            w->om_serial = args[0];
            resize_try(w);
        }
        return;
    }
    struct wp_head *h = object >= 0xff000000u ? head_find(w, object) : NULL;
    if (h != NULL) {
        if (opcode == HEAD_EV_NAME) {
            size_t at = 0;
            snprintf(h->name, sizeof(h->name), "%s", arg_str(args, words, &at));
        } else if (opcode == HEAD_EV_ENABLED && words >= 1) {
            h->enabled = args[0] != 0;
        } else if (opcode == HEAD_EV_FINISHED) {
            *h = w->heads[--w->head_count];
        }
        return;
    }
    if (object != 0 && object == w->cfg) {
        resize_done(w, opcode);
        return;
    }
    // Anything else (wl_output's geometry and modes, the dmabuf formats, a
    // buffer's release, delete_id, the outputs' modes) needs nothing done.
}

// Read what has arrived -- from the compositor, the app, or a selection being
// pasted to the app -- waiting up to timeout_ms. 0 on progress or timeout,
// -1 when the connection is gone.
static int wp_dispatch(struct wp *w, int timeout_ms) {
    struct pollfd p[3] = {
        {.fd = w->sock, .events = POLLIN},
        {.fd = w->input, .events = POLLIN},
        {.fd = w->clip_in, .events = POLLIN},
    };
    int r = poll(p, 3, timeout_ms);
    if (r < 0)
        return errno == EINTR ? 0 : -1;
    if (r == 0)
        return 0;
    if (w->input >= 0 && (p[1].revents & POLLIN))
        input_read(w);
    if (w->clip_in >= 0 && (p[2].revents & (POLLIN | POLLHUP | POLLERR)))
        clip_in_read(w);
    if (!(p[0].revents & (POLLIN | POLLHUP | POLLERR)))
        return 0;
    union {
        struct cmsghdr h;
        char space[CMSG_SPACE(sizeof(int) * 28)];
    } cmsg;
    struct iovec iov = {.iov_base = w->in + w->in_len, .iov_len = sizeof(w->in) - w->in_len};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = &cmsg,
                        .msg_controllen = sizeof(cmsg.space)};
    ssize_t got = recvmsg(w->sock, &mh, 0);
    if (got <= 0)
        return -1;
    // Descriptors queue up for the events that carry them (only a clipboard
    // source's send, of those this client gets).
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c != NULL; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count; i++) {
                int fd;
                memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
                if (w->in_fd_count < (int) (sizeof(w->in_fds) / sizeof(w->in_fds[0])))
                    w->in_fds[w->in_fd_count++] = fd;
                else
                    close(fd);
            }
        }
    }
    w->in_len += (size_t) got;
    size_t at = 0;
    while (w->in_len - at >= 8) {
        uint32_t hdr[2];
        memcpy(hdr, w->in + at, 8);
        size_t size = hdr[1] >> 16;
        if (size < 8 || (size & 3))
            return -1;
        if (w->in_len - at < size)
            break;
        uint32_t args[(sizeof(w->in) / 4)];
        memcpy(args, w->in + at + 8, size - 8);
        on_event(w, hdr[0], (uint16_t) (hdr[1] & 0xffff), args, (size - 8) / 4);
        at += size;
    }
    memmove(w->in, w->in + at, w->in_len - at);
    w->in_len -= at;
    // With every whole message handled, a descriptor nothing took is stray.
    if (w->in_len == 0)
        while (w->in_fd_count > 0)
            close(take_fd(w));
    return 0;
}

// ---- buffers -----------------------------------------------------------------

static void buffers_free(struct wp *w) {
    for (int i = 0; i < 2; i++) {
        struct wp_buffer *b = &w->buf[i];
        if (b->id != 0)
            wp_send(w, b->id, BUFFER_DESTROY, NULL, -1);
        if (b->fd >= 0)
            close(b->fd);
        *b = (struct wp_buffer) {.fd = -1};
    }
    w->buf_width = w->buf_height = 0;
}

static int buffers_make(struct wp *w) {
    buffers_free(w);
    for (int i = 0; i < 2; i++) {
        struct wp_buffer *b = &w->buf[i];
        b->fd = virtgpu_presenter_buffer(w->width, w->height, &b->stride);
        if (b->fd < 0)
            return -1;

        uint32_t params = w->next_id++;
        struct msg m = {.n = 0};
        msg_u32(&m, params);
        wp_send(w, w->dmabuf, DMABUF_CREATE_PARAMS, &m, -1);
        m.n = 0;
        msg_u32(&m, 0);             // plane
        msg_u32(&m, 0);             // offset
        msg_u32(&m, b->stride);
        msg_u32(&m, 0);             // modifier: DRM_FORMAT_MOD_LINEAR
        msg_u32(&m, 0);
        wp_send(w, params, PARAMS_ADD, &m, b->fd);
        b->id = w->next_id++;
        m.n = 0;
        msg_u32(&m, b->id);
        msg_u32(&m, w->width);
        msg_u32(&m, w->height);
        msg_u32(&m, w->format);
        msg_u32(&m, 0);             // flags
        wp_send(w, params, PARAMS_CREATE_IMMED, &m, -1);
        wp_send(w, params, PARAMS_DESTROY, NULL, -1);
    }
    w->buf_width = w->width;
    w->buf_height = w->height;
    w->buf_format = w->format;
    return 0;
}

// ---- the program -------------------------------------------------------------

// A line for start-wayland.sh, which passes it to wayvncctl.
static void wayvnc_control(struct wp *w, const char *what) {
    printf("%s\n", what);
    fflush(stdout);
    wp_log(w, "wayvnc: %s", what);
}

static int wp_connect(struct wp *w) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    const char *name = getenv("WAYLAND_DISPLAY");
    if (name == NULL || name[0] == '\0')
        name = "wayland-0";
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (name[0] == '/')
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", name);
    else if (dir != NULL)
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s", dir, name);
    else
        return -1;
    w->sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (w->sock < 0)
        return -1;
    fcntl(w->sock, F_SETFD, FD_CLOEXEC);
    return connect(w->sock, (struct sockaddr *) &addr, sizeof(addr));
}

static int wp_run(struct wp *w) {
    if (wp_connect(w) != 0) {
        wp_log(w, "cannot connect to the compositor: %s", strerror(errno));
        return 1;
    }
    w->drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (w->drm < 0 || virtgpu_presenter_attach(w->drm, w->display) != 0) {
        wp_log(w, "no GPU render node to present from");
        return 1;
    }

    struct msg m = {.n = 0};
    msg_u32(&m, ID_REGISTRY);
    wp_send(w, ID_DISPLAY, DISPLAY_GET_REGISTRY, &m, -1);
    m.n = 0;
    msg_u32(&m, ID_SYNC);
    wp_send(w, ID_DISPLAY, DISPLAY_SYNC, &m, -1);
    while (!w->synced && !w->fatal)
        if (wp_dispatch(w, 5000) != 0)
            return 1;
    if (w->output_name == 0 || w->screencopy_name == 0 || w->dmabuf_name == 0) {
        wp_log(w, "the compositor lacks screencopy v3 or linux-dmabuf v3");
        return 1;
    }
    w->output = wp_bind(w, w->output_name, "wl_output", min_u32(w->output_version, 4));
    w->screencopy = wp_bind(w, w->screencopy_name, "zwlr_screencopy_manager_v1", w->screencopy_version);
    w->dmabuf = wp_bind(w, w->dmabuf_name, "zwp_linux_dmabuf_v1", 3);
    input_start(w);

    int next = 0;
    bool full = true;           // the next frame shown must carry everything
    while (!w->fatal) {
        w->failed = false;
        w->frame = w->next_id++;
        w->have_dmabuf = w->buffer_done = w->ready = false;
        w->dx0 = w->dy0 = w->dx1 = w->dy1 = 0;
        m.n = 0;
        msg_u32(&m, w->frame);
        msg_u32(&m, 1);         // draw the cursor into the frame
        msg_u32(&m, w->output);
        wp_send(w, w->screencopy, SCREENCOPY_CAPTURE_OUTPUT, &m, -1);
        while (!w->buffer_done && !w->failed && !w->fatal)
            if (wp_dispatch(w, 5000) != 0)
                return 0;
        if (w->fatal)
            break;
        if (w->failed || !w->have_dmabuf) {
            wp_log(w, "the compositor offers no dma-buf for its output");
            return 1;
        }
        if (w->width != w->buf_width || w->height != w->buf_height || w->format != w->buf_format) {
            if (buffers_make(w) != 0) {
                wp_log(w, "cannot allocate %ux%u buffers", w->width, w->height);
                return 1;
            }
            full = true;
        }
        struct wp_buffer *b = &w->buf[next];
        m.n = 0;
        msg_u32(&m, b->id);
        // After a full frame, copy on the next change only; that waiting is
        // the frame pacing -- nothing is copied while the desktop is still.
        wp_send(w, w->frame, full ? FRAME_COPY : FRAME_COPY_WITH_DAMAGE, &m, -1);
        while (!w->ready && !w->failed && !w->fatal)
            if (wp_dispatch(w, -1) != 0)
                return 0;
        wp_send(w, w->frame, FRAME_DESTROY, NULL, -1);
        if (w->fatal)
            break;
        if (w->failed) {
            // The output changed under the copy (a resize, say): start over
            // with new buffers.
            w->buf_width = 0;
            continue;
        }
        int32_t dx = 0, dy = 0, dw = (int32_t) w->width, dh = (int32_t) w->height;
        if (!full && w->dx1 > w->dx0) {
            dx = w->dx0 < 0 ? 0 : w->dx0;
            dy = w->dy0 < 0 ? 0 : w->dy0;
            dw = (w->dx1 > (int32_t) w->width ? (int32_t) w->width : w->dx1) - dx;
            dh = (w->dy1 > (int32_t) w->height ? (int32_t) w->height : w->dy1) - dy;
        }
        int shown = virtgpu_present_fd(b->fd, w->display, w->width, w->height, b->stride, w->format,
                                       dx, dy, dw, dh);
        next ^= 1;
        if ((shown == 0) != w->watched) {
            w->watched = shown == 0;
            wp_log(w, w->watched ? "the app is showing the desktop" : "nothing is showing the desktop");
        }
        // The app has the pixels and sends its input here: wayvnc, whose
        // screen-copying nobody needs now, lets go of the compositor. It stays
        // detached while the app is away (in the background, say); the app's
        // VNC connection has no use for its frames either way.
        if (w->watched && !w->detached && w->input >= 0) {
            w->detached = true;
            wayvnc_control(w, "detach");
        }
        if (shown != 0) {
            // Nobody is watching: look again in a while, and give the next
            // viewer a whole frame.
            full = true;
            if (wp_dispatch(w, 250) != 0)
                return 0;
        } else {
            full = false;
        }
    }
    return 0;
}

int native_wlpresent_main(int argc, char *const argv[], char *const envp[]) {
    (void) envp;
    struct wp w;
    memset(&w, 0, sizeof(w));
    w.sock = w.drm = w.input = w.clip_in = -1;
    w.next_id = NEXT_ID;
    w.buf[0].fd = w.buf[1].fd = -1;
    w.offer_mime = -1;
    w.quiet = argc > 1 && !strcmp(argv[1], "-q");
    // The desktop's VNC port names it to the app (fs/virtgpu.h).
    const char *port = getenv("WAYVNC_PORT");
    w.display = port != NULL ? (uint32_t) strtoul(port, NULL, 10) : 5901;
    int rc = wp_run(&w);
    if (w.sock >= 0 && w.vkbd != 0)
        keys_release_all(&w);
    // Input goes back to VNC as this descriptor closes; wayvnc must be there.
    if (w.input >= 0)
        close(w.input);
    if (w.detached)
        wayvnc_control(&w, "attach");
    clip_in_stop(&w);
    while (w.in_fd_count > 0)
        close(take_fd(&w));
    free(w.ibuf);
    free(w.clip);
    if (w.drm >= 0)
        buffers_free(&w);
    if (w.sock >= 0)
        close(w.sock);
    if (w.drm >= 0)
        close(w.drm);
    return rc;
}
