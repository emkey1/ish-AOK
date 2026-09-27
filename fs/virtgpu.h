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

// Called with a frame, synchronously: when it returns the buffer may be
// reused, so it must have finished reading it. Returns 0 when the frame was
// shown, nonzero when nothing is showing the desktop (the presenter then
// captures less often). Called with NULL when the presenter goes away, so the
// viewer can go back to asking VNC for pixels.
typedef int (*virtgpu_present_fn)(const struct virtgpu_frame *frame, void *ctx);

// The app's hook; NULL removes it. Waits for a frame in progress.
void virtgpu_set_present_hook(virtgpu_present_fn fn, void *ctx);

// For wl-present, on its own task: `render_fd` is its render node, whose
// closing (exit, crash or kill) tells the viewer it is gone.
int virtgpu_presenter_attach(int render_fd);
// A 32-bit linear buffer in host shared memory, as a new dma-buf descriptor
// (or a negative errno); *stride gets its row pitch.
int virtgpu_presenter_buffer(uint32_t width, uint32_t height, uint32_t *stride);
// Show the dma-buf `buf_fd`. 0 when shown, 1 when nobody is watching, or a
// negative errno.
int virtgpu_present_fd(int buf_fd, uint32_t width, uint32_t height, uint32_t stride,
                       uint32_t format, int32_t dx, int32_t dy, int32_t dw, int32_t dh);

#endif
