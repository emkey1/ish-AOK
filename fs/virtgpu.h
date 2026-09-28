#ifndef FS_VIRTGPU_H
#define FS_VIRTGPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Whether /dev/dri/renderD128 exists: the renderer was built in and
// ISH_VIRTGPU=0 has not turned it off. Its sysfs identity follows this.
bool virtgpu_available(void);

#ifdef ISH_VIRTGPU
extern struct dev_ops virtgpu_dev;
#endif

// ---- presenting the Wayland desktop straight to the app (#484) ----
//
// wl-present (kernel/native_wlpresent.c) has the compositor copy each frame,
// on the GPU, into one of its dma-bufs, then hands it over here; the app,
// which registered a hook, draws it with Metal. VNC then carries only input.
//
// Each desktop is named by its VNC port (`display`), so the hook for one
// desktop's window never gets another's frames; a hook for display 0 takes
// any desktop's that has no hook of its own.

// A frame: the whole buffer (host shared memory, mapped for the life of the
// buffer), its layout, and the rectangle that changed since the last frame.
// `pixels` is page-aligned and `size` a whole number of host pages, so it can
// be wrapped as an MTLBuffer without copying.
struct virtgpu_frame {
    void *pixels;
    size_t size;
    uint32_t width, height, stride;
    uint32_t format;            // DRM fourcc; XRGB8888 is B,G,R,X in memory
    int32_t damage_x, damage_y, damage_width, damage_height;
};

// The app's side, per display. frame is called with a frame, synchronously:
// when it returns the buffer may be reused, so it must have finished reading
// it. It returns 0 when the frame was shown, nonzero when nothing is showing
// the desktop (the presenter then captures less often). It is called with
// NULL when the presenter goes away, so the viewer can go back to VNC.
// clipboard, which may be NULL, gets the desktop's clipboard as UTF-8 text
// (not NUL-terminated) whenever it changes, once the presenter carries the
// clipboard.
struct virtgpu_present_ops {
    int (*frame)(const struct virtgpu_frame *frame, void *ctx);
    void (*clipboard)(const char *text, size_t len, void *ctx);
};

// The app's hook for a display, replacing any it had. Waits for a call in
// progress, as does clearing.
void virtgpu_set_present_hook(uint32_t display, const struct virtgpu_present_ops *ops, void *ctx);
// Removes the display's hook only if it is still ctx's, for a viewer going
// away while another may have taken over.
void virtgpu_clear_present_hook(uint32_t display, void *ctx);

// Input from the app to the presenter, which then drives the compositor's
// virtual pointer and keyboard itself -- so wayvnc need not be attached to
// the compositor at all. The events are RFB's, as the app already makes
// them.
enum {
    VIRTGPU_INPUT_POINTER = 1,  // a = x | y << 16 in desktop pixels, b = RFB button mask
    VIRTGPU_INPUT_KEY = 2,      // a = X keysym, b = 1 down / 0 up
    VIRTGPU_INPUT_CLIPBOARD = 3,    // data = UTF-8 text for the desktop's clipboard
    VIRTGPU_INPUT_RESIZE = 4,   // a = width, b = height
};
// On the presenter's descriptor, each event reads as this header and then
// `len` bytes of data.
struct virtgpu_input_event {
    uint32_t type, a, b, len;
};
// Queue an event for the display's presenter. 0 when queued; _ENOENT when no
// presenter takes input for it (send the event over VNC instead); _EAGAIN
// when its queue is full.
int virtgpu_present_input(uint32_t display, uint32_t type, uint32_t a, uint32_t b,
                          const void *data, uint32_t len);

// For wl-present, on its own task: `render_fd` is its render node, whose
// closing (exit, crash or kill) tells the display's viewer it is gone.
int virtgpu_presenter_attach(int render_fd, uint32_t display);
// A 32-bit linear buffer in host shared memory, as a new dma-buf descriptor
// (or a negative errno); *stride gets its row pitch.
int virtgpu_presenter_buffer(uint32_t width, uint32_t height, uint32_t *stride);
// Show the dma-buf `buf_fd` on the display. 0 when shown, 1 when nobody is
// watching, or a negative errno.
int virtgpu_present_fd(int buf_fd, uint32_t display, uint32_t width, uint32_t height, uint32_t stride,
                       uint32_t format, int32_t dx, int32_t dy, int32_t dw, int32_t dh);
// A new non-blocking descriptor on which the app's input for the display
// arrives (struct virtgpu_input_event), readable while any is queued; from
// then until it closes, the app sends its input here.
int virtgpu_presenter_input(uint32_t display);
// Give the app the desktop's clipboard (UTF-8).
void virtgpu_presenter_clipboard(uint32_t display, const char *text, size_t len);

#endif
