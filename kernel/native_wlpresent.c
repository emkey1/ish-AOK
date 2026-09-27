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
// through emulated code, and wayvnc, left with nothing to send while the app
// stops asking it for pixels, carries only input.
//
// The Wayland client is written out here rather than linked: the handful of
// requests this needs is less code than a libwayland built for the host, and
// the protocol is stable. Everything is in one struct on the stack -- a native
// program is a function the app may call again, so no state may outlive a run
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
    SCREENCOPY_CAPTURE_OUTPUT = 0,
    FRAME_COPY = 0, FRAME_DESTROY = 1, FRAME_COPY_WITH_DAMAGE = 2,
    FRAME_EV_BUFFER = 0, FRAME_EV_FLAGS = 1, FRAME_EV_READY = 2, FRAME_EV_FAILED = 3,
    FRAME_EV_DAMAGE = 4, FRAME_EV_LINUX_DMABUF = 5, FRAME_EV_BUFFER_DONE = 6,
    DMABUF_CREATE_PARAMS = 1,
    PARAMS_DESTROY = 0, PARAMS_ADD = 1, PARAMS_CREATE_IMMED = 3,
    BUFFER_DESTROY = 0,
};

// The buffers come straight from the kernel (virtgpu_presenter_buffer), not
// through DRM ioctls: the shim passes only terminal ioctls, and this program
// is kernel-resident code anyway.
struct wp_buffer {
    int fd;                     // a dma-buf of host shared memory
    uint32_t stride;
    uint32_t id;                // wl_buffer
};

struct wp {
    int sock, drm;
    uint32_t next_id;
    // what the compositor offers
    uint32_t output_name, screencopy_name, dmabuf_name;
    uint32_t screencopy_version;
    uint32_t output, screencopy, dmabuf;
    // the frame in flight
    uint32_t frame;
    uint32_t format, width, height;
    bool have_dmabuf, buffer_done, ready, failed;
    bool fatal;                 // a protocol error: the connection is over
    int32_t dx0, dy0, dx1, dy1;     // damage, as a bounding box
    // two buffers, alternated
    struct wp_buffer buf[2];
    uint32_t buf_width, buf_height, buf_format;
    // incoming bytes
    uint8_t in[16384];
    size_t in_len;
    bool synced, quiet;
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
        if (!strcmp(iface, "wl_output") && w->output_name == 0)
            w->output_name = name;
        else if (!strcmp(iface, "zwlr_screencopy_manager_v1") && version >= 3) {
            w->screencopy_name = name;
            w->screencopy_version = 3;
        } else if (!strcmp(iface, "zwp_linux_dmabuf_v1") && version >= 3)
            w->dmabuf_name = name;
        return;
    }
    if (object == ID_SYNC) {
        w->synced = true;
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
    }
    // Anything else (wl_output's description, the dmabuf formats, a buffer's
    // release, delete_id) needs nothing done.
}

// Read what has arrived, waiting up to timeout_ms. 0 on progress or timeout,
// -1 when the connection is gone.
static int wp_dispatch(struct wp *w, int timeout_ms) {
    struct pollfd p = {.fd = w->sock, .events = POLLIN};
    int r = poll(&p, 1, timeout_ms);
    if (r < 0)
        return errno == EINTR ? 0 : -1;
    if (r == 0)
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
    // No event this client asks for carries a descriptor; close any that came.
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c != NULL; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < count; i++) {
                int fd;
                memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
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
    return 0;
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
    if (w->drm < 0 || virtgpu_presenter_attach(w->drm) != 0) {
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
    w->output = wp_bind(w, w->output_name, "wl_output", 1);
    w->screencopy = wp_bind(w, w->screencopy_name, "zwlr_screencopy_manager_v1", w->screencopy_version);
    w->dmabuf = wp_bind(w, w->dmabuf_name, "zwp_linux_dmabuf_v1", 3);

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
        int shown = virtgpu_present_fd(b->fd, w->width, w->height, b->stride, w->format,
                                       dx, dy, dw, dh);
        next ^= 1;
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
    w.sock = w.drm = -1;
    w.next_id = NEXT_ID;
    w.buf[0].fd = w.buf[1].fd = -1;
    w.quiet = argc > 1 && !strcmp(argv[1], "-q");
    int rc = wp_run(&w);
    if (w.drm >= 0)
        buffers_free(&w);
    if (w.sock >= 0)
        close(w.sock);
    if (w.drm >= 0)
        close(w.drm);
    return rc;
}
