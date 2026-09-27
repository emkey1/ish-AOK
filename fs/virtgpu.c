// /dev/dri/renderD128: a virtio-gpu render node, for 3D in the guest (#484).
//
// The guest runs its distribution's own Mesa, whose Venus Vulkan driver
// (libvulkan_virtio) speaks to a virtio-gpu device through the virtio_gpu DRM
// uAPI -- the same driver a QEMU or crosvm guest uses. There is no virtio
// transport here: the ioctls this node answers call virglrenderer's Venus
// renderer in the same process, which replays the guest's Vulkan calls on the
// host's Vulkan (MoltenVK over Metal on Apple hosts). zink turns that Vulkan
// into OpenGL for the guest, so one path serves both.
//
// What Mesa's Venus DRM backend (src/virtio/vulkan/vn_renderer_virtgpu.c)
// needs, and so what is here:
//
//  - DRM_IOCTL_VERSION naming the driver "virtio_gpu", version 0.x;
//  - VIRTGPU GETPARAM, GET_CAPS, CONTEXT_INIT, RESOURCE_CREATE_BLOB,
//    RESOURCE_INFO, MAP and EXECBUFFER, and GEM_CLOSE;
//  - mmap of a blob at the offset MAP handed out, which maps the host's shared
//    memory for it -- the same pages the host GPU reads, so nothing is copied;
//  - an out-fence from EXECBUFFER: a descriptor that polls readable once the
//    host has retired the submission. Mesa builds its sync objects on these
//    itself (SIMULATE_SYNCOBJ), so no DRM_IOCTL_SYNCOBJ_* is needed;
//  - PRIME export and import, for the window-system paths.
//
// libdrm finds the node through sysfs (drmGetDevices2): /sys/dev/char/226:128
// and a "platform" bus device, fs/proc/root.c.
//
// The renderer is built only when meson is given -Dvirglrenderer=<build dir>;
// without it the node does not exist (virtgpu_available()).

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "debug.h"
#include "kernel/abi.h"
#include "kernel/calls.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "kernel/task.h"
#include "fs/dev.h"
#include "fs/devices.h"
#include "fs/fd.h"
#include "fs/poll.h"
#include "fs/real.h"
#include "fs/virtgpu.h"
#include "util/list.h"
#include "util/sync.h"

#ifdef ISH_VIRTGPU

// ---- virglrenderer's in-process Venus renderer ----------------------------
//
// src/venus/vkr_renderer.h, declared here rather than included: the header
// pulls in virglrenderer's private config.h. These are the calls its render
// server makes (server/render_state.c); calling them directly is what keeps
// the renderer in this process, which iOS requires.

struct vkr_renderer_callbacks {
    void (*debug_logger)(int level, const char *message, void *user_data);
    void (*retire_fence)(uint32_t ctx_id, uint32_t ring_idx, uint64_t fence_id);
};
#define VKR_RENDERER_THREAD_SYNC (1u << 0)
#define VKR_RENDERER_ASYNC_FENCE_CB (1u << 1)
#define VIRGL_RENDERER_FENCE_FLAG_MERGEABLE (1u << 0)
enum { VIRGL_RESOURCE_FD_SHM_ = 2 };

size_t vkr_get_capset(void *capset, uint32_t flags);
bool vkr_renderer_init(uint32_t flags, const struct vkr_renderer_callbacks *cbs);
bool vkr_renderer_create_context(uint32_t ctx_id, uint32_t ctx_flags, uint32_t nlen, const char *name);
void vkr_renderer_destroy_context(uint32_t ctx_id);
bool vkr_renderer_submit_cmd(uint32_t ctx_id, void *cmd, uint32_t size);
bool vkr_renderer_submit_fence(uint32_t ctx_id, uint32_t flags, uint64_t ring_idx, uint64_t fence_id);
bool vkr_renderer_create_resource(uint32_t ctx_id, uint32_t res_id, uint64_t blob_id, uint64_t blob_size,
        uint32_t blob_flags, int *out_fd_type, int *out_res_fd, uint32_t *out_map_info, void *out_vulkan_info);
bool vkr_renderer_import_resource(uint32_t ctx_id, uint32_t res_id, int fd_type, int fd, uint64_t size);
void vkr_renderer_destroy_resource(uint32_t ctx_id, uint32_t res_id);

// ---- the uAPI (include/uapi/drm/drm.h, virtgpu_drm.h) ---------------------

#define DRM_IOCTL_TYPE 'd'
#define DRM_NR_VERSION 0x00
#define DRM_NR_GEM_CLOSE 0x09
#define DRM_NR_GET_CAP 0x0c
#define DRM_NR_PRIME_HANDLE_TO_FD 0x2d
#define DRM_NR_PRIME_FD_TO_HANDLE 0x2e
#define DRM_COMMAND_BASE 0x40
#define VIRTGPU_NR_MAP (DRM_COMMAND_BASE + 0x01)
#define VIRTGPU_NR_EXECBUFFER (DRM_COMMAND_BASE + 0x02)
#define VIRTGPU_NR_GETPARAM (DRM_COMMAND_BASE + 0x03)
#define VIRTGPU_NR_RESOURCE_INFO (DRM_COMMAND_BASE + 0x05)
#define VIRTGPU_NR_WAIT (DRM_COMMAND_BASE + 0x08)
#define VIRTGPU_NR_GET_CAPS (DRM_COMMAND_BASE + 0x09)
#define VIRTGPU_NR_RESOURCE_CREATE_BLOB (DRM_COMMAND_BASE + 0x0a)
#define VIRTGPU_NR_CONTEXT_INIT (DRM_COMMAND_BASE + 0x0b)

#define IOC_NR(cmd) ((unsigned) (cmd) & 0xff)
#define IOC_TYPE(cmd) (((unsigned) (cmd) >> 8) & 0xff)
#define IOC_SIZE(cmd) (((unsigned) (cmd) >> 16) & 0x3fff)

#define DRM_CAP_PRIME 0x5
#define DRM_CAP_TIMESTAMP_MONOTONIC 0x6
#define DRM_CAP_SYNCOBJ 0x13
#define DRM_CAP_SYNCOBJ_TIMELINE 0x14
#define DRM_PRIME_CAP_IMPORT 0x1
#define DRM_PRIME_CAP_EXPORT 0x2
#define DRM_CLOEXEC O_CLOEXEC_

#define VIRTGPU_PARAM_3D_FEATURES 1
#define VIRTGPU_PARAM_CAPSET_QUERY_FIX 2
#define VIRTGPU_PARAM_RESOURCE_BLOB 3
#define VIRTGPU_PARAM_HOST_VISIBLE 4
#define VIRTGPU_PARAM_CROSS_DEVICE 5
#define VIRTGPU_PARAM_CONTEXT_INIT 6
#define VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs 7

#define VIRTGPU_EXECBUF_FENCE_FD_IN 0x01
#define VIRTGPU_EXECBUF_FENCE_FD_OUT 0x02
#define VIRTGPU_EXECBUF_RING_IDX 0x04

#define VIRTGPU_DRM_CAPSET_VENUS 4
#define VIRTGPU_BLOB_MEM_HOST3D 0x0002
#define VIRTGPU_BLOB_FLAG_USE_MAPPABLE 0x0001

#define VIRTGPU_CONTEXT_PARAM_CAPSET_ID 0x0001
#define VIRTGPU_CONTEXT_PARAM_NUM_RINGS 0x0002
#define VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK 0x0003
#define VIRTGPU_CONTEXT_PARAM_DEBUG_NAME 0x0004

// Every struct below has the same layout on every guest ABI (fixed-width
// fields, pointers carried in a u64) except drm_version, handled on its own.
struct drm_gem_close_ { uint32_t handle, pad; };
struct drm_get_cap_ { uint64_t capability, value; };
struct drm_prime_handle_ { uint32_t handle, flags; int32_t fd; };
struct drm_virtgpu_map_ { uint64_t offset; uint32_t handle, pad; };
struct drm_virtgpu_execbuffer_ {
    uint32_t flags, size;
    uint64_t command, bo_handles;
    uint32_t num_bo_handles;
    int32_t fence_fd;
    uint32_t ring_idx, syncobj_stride, num_in_syncobjs, num_out_syncobjs;
    uint64_t in_syncobjs, out_syncobjs;
};
struct drm_virtgpu_getparam_ { uint64_t param, value; };
struct drm_virtgpu_resource_info_ { uint32_t bo_handle, res_handle, size, blob_mem; };
struct drm_virtgpu_get_caps_ { uint32_t cap_set_id, cap_set_ver; uint64_t addr; uint32_t size, pad; };
struct drm_virtgpu_resource_create_blob_ {
    uint32_t blob_mem, blob_flags, bo_handle, res_handle;
    uint64_t size;
    uint32_t pad, cmd_size;
    uint64_t cmd, blob_id;
};
struct drm_virtgpu_context_init_ { uint32_t num_params, pad; uint64_t ctx_set_params; };
struct drm_virtgpu_context_set_param_ { uint64_t param, value; };

#define VIRTGPU_MAX_RINGS 64
// A Venus command stream is a few KiB; a shader upload can be a few MiB.
#define VIRTGPU_MAX_CMD_SIZE (64u << 20)

// ---- state -----------------------------------------------------------------

// A host resource: one blob, shared by every handle (in any open file) and
// every PRIME descriptor that names it.
struct vgpu_res {
    atomic_uint refcount;
    uint32_t res_id;
    uint32_t ctx_id;        // the context that created it
    uint32_t blob_mem;
    uint64_t size;
    int host_fd;            // shared memory, -1 when the blob is not mappable
};

// A Venus context, one per open file. Fence descriptors keep it alive after
// the file is closed, since a fence may be polled long after.
struct vgpu_ctx {
    atomic_uint refcount;
    uint32_t ctx_id;
    uint32_t num_rings;
    bool destroyed;
    uint64_t next_fence[VIRTGPU_MAX_RINGS];
    _Atomic uint64_t retired[VIRTGPU_MAX_RINGS];
    struct list link;       // vgpu_contexts
    struct list fences;     // pending struct vgpu_fence
};

struct vgpu_fence {
    struct vgpu_ctx *ctx;
    uint32_t ring;
    uint64_t id;
    struct fd *fd;
    struct list link;       // ctx->fences while pending
};

struct vgpu_handle {
    uint32_t handle;
    struct vgpu_res *res;
    uint64_t map_offset;    // 0 until MAP
    bool imported;          // imported into this file's context from another
    struct list link;
};

struct vgpu_file {
    lock_t lock;
    struct vgpu_ctx *ctx;   // NULL until CONTEXT_INIT
    uint32_t next_handle;
    uint64_t next_map_offset;
    struct list handles;
};

// vgpu_lock guards the context list and every context's fence list and
// counters. renderer_lock serialises calls into the renderer, as its server
// does. Order: renderer_lock may be held when vgpu_lock is taken (a fence can
// retire inside a submit), never the other way.
static lock_t vgpu_lock = LOCK_INITIALIZER;
static lock_t renderer_lock = LOCK_INITIALIZER;
static struct list vgpu_contexts = {&vgpu_contexts, &vgpu_contexts};
static atomic_uint next_ctx_id = 1;
static atomic_uint next_res_id = 1;
static int renderer_state;  // 0 untried, 1 ready, -1 failed
static unsigned char venus_capset[512];
static size_t venus_capset_size;

static const struct fd_ops vgpu_fence_ops;
static const struct fd_ops vgpu_prime_ops;

// ---- fences ----------------------------------------------------------------

static bool fence_signaled(struct vgpu_fence *fence) {
    return atomic_load(&fence->ctx->retired[fence->ring]) >= fence->id;
}

// A pending fence found by the retire callback, retained for waking once the
// list lock is dropped (poll_wakeup must not run under a lock the fence's own
// poll could want).
struct woken { struct fd *fds[32]; int count; };

static void wake_fences(struct woken *w) {
    for (int i = 0; i < w->count; i++) {
        struct fd *fd = w->fds[i];
        lock(&fd->lock, 0);
        notify(&fd->cond);
        unlock(&fd->lock);
        poll_wakeup(fd, POLL_READ);
        fd_close(fd);
    }
    w->count = 0;
}

// Take fences that are now signaled off `ctx`'s list, at most one batch.
// Returns true if more may remain. Caller holds vgpu_lock.
static bool collect_signaled(struct vgpu_ctx *ctx, struct woken *w) {
    struct vgpu_fence *fence, *tmp;
    list_for_each_entry_safe(&ctx->fences, fence, tmp, link) {
        if (!fence_signaled(fence))
            continue;
        if (w->count == (int) (sizeof(w->fds) / sizeof(w->fds[0])))
            return true;
        list_remove(&fence->link);
        struct fd *fd = fd_retain_if_live(fence->fd);
        if (fd != NULL)
            w->fds[w->count++] = fd;
    }
    return false;
}

static void retire_fence(uint32_t ctx_id, uint32_t ring_idx, uint64_t fence_id) {
    if (ring_idx >= VIRTGPU_MAX_RINGS)
        return;
    struct woken w = {.count = 0};
    bool more = true;
    while (more) {
        lock(&vgpu_lock, 0);
        struct vgpu_ctx *ctx = NULL, *c;
        list_for_each_entry(&vgpu_contexts, c, link) {
            if (c->ctx_id == ctx_id) {
                ctx = c;
                break;
            }
        }
        more = false;
        if (ctx != NULL) {
            // Mergeable fences retire in order, and one callback can stand
            // for every fence before it.
            if (atomic_load(&ctx->retired[ring_idx]) < fence_id)
                atomic_store(&ctx->retired[ring_idx], fence_id);
            more = collect_signaled(ctx, &w);
        }
        unlock(&vgpu_lock);
        wake_fences(&w);
    }
}

static void ctx_release(struct vgpu_ctx *ctx) {
    if (atomic_fetch_sub(&ctx->refcount, 1) != 1)
        return;
    lock(&vgpu_lock, 0);
    list_remove(&ctx->link);
    unlock(&vgpu_lock);
    free(ctx);
}

static int fence_poll(struct fd *fd) {
    return fence_signaled(fd->data) ? POLL_READ : 0;
}

static int fence_close(struct fd *fd) {
    struct vgpu_fence *fence = fd->data;
    lock(&vgpu_lock, 0);
    if (fence->link.next != NULL)
        list_remove_safe(&fence->link);
    unlock(&vgpu_lock);
    ctx_release(fence->ctx);
    free(fence);
    return 0;
}

static const struct fd_ops vgpu_fence_ops = {
    .name = "sync_file",
    .anon_inode_class = "sync_file",
    .poll = fence_poll,
    .close = fence_close,
};

// Wait for one of our fences, interruptibly. Any other descriptor is EINVAL:
// the only fences a guest can hold here are ones this device made.
static int fence_wait(fd_t f) {
    struct fd *fd = f_get(f);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &vgpu_fence_ops)
        return _EINVAL;
    struct vgpu_fence *fence = fd->data;
    int err = 0;
    lock(&fd->lock, 0);
    while (!fence_signaled(fence) && err == 0)
        err = wait_for(&fd->cond, &fd->lock, NULL);
    unlock(&fd->lock);
    return err == _EINTR ? _ERESTART : err;
}

// ---- resources -------------------------------------------------------------

static void res_release(struct vgpu_res *res) {
    if (atomic_fetch_sub(&res->refcount, 1) != 1)
        return;
    lock(&renderer_lock, 0);
    vkr_renderer_destroy_resource(res->ctx_id, res->res_id);
    unlock(&renderer_lock);
    if (res->host_fd >= 0)
        close(res->host_fd);
    free(res);
}

// Caller holds file->lock.
static struct vgpu_handle *handle_find(struct vgpu_file *file, uint32_t handle) {
    struct vgpu_handle *h;
    list_for_each_entry(&file->handles, h, link) {
        if (h->handle == handle)
            return h;
    }
    return NULL;
}

// Caller holds file->lock. Takes over the caller's reference to `res`.
static struct vgpu_handle *handle_add(struct vgpu_file *file, struct vgpu_res *res, bool imported) {
    struct vgpu_handle *h = calloc(1, sizeof(*h));
    if (h == NULL)
        return NULL;
    h->handle = file->next_handle++;
    h->res = res;
    h->imported = imported;
    list_add_tail(&file->handles, &h->link);
    return h;
}

// Caller holds file->lock.
static void handle_drop(struct vgpu_file *file, struct vgpu_handle *h) {
    list_remove(&h->link);
    if (h->imported) {
        lock(&renderer_lock, 0);
        vkr_renderer_destroy_resource(file->ctx->ctx_id, h->res->res_id);
        unlock(&renderer_lock);
    }
    res_release(h->res);
    free(h);
}

// ---- the renderer ----------------------------------------------------------

static void renderer_log(int level, const char *message, void *user_data) {
    (void) level; (void) user_data;
    printk("virtgpu: %s", message);
}

static bool renderer_ready(void) {
    lock(&renderer_lock, 0);
    if (renderer_state == 0) {
        static const struct vkr_renderer_callbacks cbs = {
            .debug_logger = renderer_log,
            .retire_fence = retire_fence,
        };
        renderer_state = -1;
        // The flags its render server always uses: a sync thread per queue,
        // and fences reported from that thread.
        if (vkr_renderer_init(VKR_RENDERER_THREAD_SYNC | VKR_RENDERER_ASYNC_FENCE_CB, &cbs)) {
            venus_capset_size = vkr_get_capset(NULL, 0);
            if (venus_capset_size <= sizeof(venus_capset)) {
                vkr_get_capset(venus_capset, 0);
                renderer_state = 1;
            }
        }
        if (renderer_state < 0)
            printk("virtgpu: the Venus renderer did not start (no host Vulkan?)\n");
    }
    bool ready = renderer_state > 0;
    unlock(&renderer_lock);
    return ready;
}

bool virtgpu_available(void) {
    return getenv("ISH_VIRTGPU") == NULL || strcmp(getenv("ISH_VIRTGPU"), "0") != 0;
}

// ---- ioctls ----------------------------------------------------------------

static int copy_field(const char *value, guest_addr_t addr, uint64_t *len) {
    size_t n = strlen(value);
    size_t copy = *len < n ? (size_t) *len : n;
    if (copy > 0 && addr != 0 && user_write(addr, value, copy))
        return _EFAULT;
    *len = n;
    return 0;
}

static int ioctl_version(void *arg, unsigned size) {
    static const char name[] = "virtio_gpu", date[] = "0", desc[] = "virtio GPU and KMS";
    int32_t *v = arg;
    v[0] = 0; v[1] = 1; v[2] = 0;   // major, minor, patchlevel
    uint64_t len[3];
    guest_addr_t addr[3];
    bool wide = size >= 64;
    for (int i = 0; i < 3; i++) {
        if (wide) {
            uint64_t *w = (uint64_t *) ((char *) arg + 16);
            len[i] = w[i * 2];
            addr[i] = (guest_addr_t) w[i * 2 + 1];
        } else {
            uint32_t *w = (uint32_t *) ((char *) arg + 12);
            len[i] = w[i * 2];
            addr[i] = w[i * 2 + 1];
        }
    }
    const char *fields[3] = {name, date, desc};
    for (int i = 0; i < 3; i++) {
        int err = copy_field(fields[i], addr[i], &len[i]);
        if (err < 0)
            return err;
        if (wide)
            ((uint64_t *) ((char *) arg + 16))[i * 2] = len[i];
        else
            ((uint32_t *) ((char *) arg + 12))[i * 2] = (uint32_t) len[i];
    }
    return 0;
}

static int ioctl_getparam(struct drm_virtgpu_getparam_ *p) {
    int32_t value;
    switch (p->param) {
        case VIRTGPU_PARAM_3D_FEATURES:
        case VIRTGPU_PARAM_CAPSET_QUERY_FIX:
        case VIRTGPU_PARAM_RESOURCE_BLOB:
        case VIRTGPU_PARAM_HOST_VISIBLE:
        case VIRTGPU_PARAM_CONTEXT_INIT:
            value = 1;
            break;
        case VIRTGPU_PARAM_CROSS_DEVICE:
            value = 0;
            break;
        case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs:
            value = 1 << VIRTGPU_DRM_CAPSET_VENUS;
            break;
        default:
            return _EINVAL;
    }
    // The kernel writes an int, not the u64 the field could hold.
    if (user_write((guest_addr_t) p->value, &value, sizeof(value)))
        return _EFAULT;
    return 0;
}

static int ioctl_get_caps(struct drm_virtgpu_get_caps_ *c) {
    if (c->cap_set_id != VIRTGPU_DRM_CAPSET_VENUS || c->cap_set_ver != 0)
        return _EINVAL;
    size_t n = c->size < venus_capset_size ? c->size : venus_capset_size;
    if (n > 0 && user_write((guest_addr_t) c->addr, venus_capset, n))
        return _EFAULT;
    return 0;
}

static int ioctl_context_init(struct vgpu_file *file, struct drm_virtgpu_context_init_ *init) {
    uint32_t capset = 0, rings = 1;
    char name[64] = "";
    if (init->num_params > 16)
        return _EINVAL;
    for (uint32_t i = 0; i < init->num_params; i++) {
        struct drm_virtgpu_context_set_param_ p;
        if (user_read((guest_addr_t) (init->ctx_set_params + i * sizeof(p)), &p, sizeof(p)))
            return _EFAULT;
        switch (p.param) {
            case VIRTGPU_CONTEXT_PARAM_CAPSET_ID:
                capset = (uint32_t) p.value;
                break;
            case VIRTGPU_CONTEXT_PARAM_NUM_RINGS:
                if (p.value == 0 || p.value > VIRTGPU_MAX_RINGS)
                    return _EINVAL;
                rings = (uint32_t) p.value;
                break;
            case VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK:
                // Fence events on read(2): nothing here delivers them, and
                // Mesa asks for none.
                if (p.value != 0)
                    return _EINVAL;
                break;
            case VIRTGPU_CONTEXT_PARAM_DEBUG_NAME:
                if (user_read_string((guest_addr_t) p.value, name, sizeof(name)))
                    return _EFAULT;
                break;
            default:
                return _EINVAL;
        }
    }
    if (capset != VIRTGPU_DRM_CAPSET_VENUS)
        return _EINVAL;

    lock(&file->lock, 0);
    if (file->ctx != NULL) {
        unlock(&file->lock);
        return _EEXIST;
    }
    struct vgpu_ctx *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        unlock(&file->lock);
        return _ENOMEM;
    }
    ctx->refcount = 1;
    ctx->ctx_id = atomic_fetch_add(&next_ctx_id, 1);
    ctx->num_rings = rings;
    list_init(&ctx->fences);
    if (name[0] == '\0' && current != NULL)
        snprintf(name, sizeof(name), "%s", current->comm);
    lock(&renderer_lock, 0);
    bool ok = vkr_renderer_create_context(ctx->ctx_id, capset, (uint32_t) strlen(name), name);
    unlock(&renderer_lock);
    if (!ok) {
        unlock(&file->lock);
        free(ctx);
        return _ENOMEM;
    }
    lock(&vgpu_lock, 0);
    list_add(&vgpu_contexts, &ctx->link);
    unlock(&vgpu_lock);
    file->ctx = ctx;
    unlock(&file->lock);
    return 0;
}

// Read a command stream from the guest and hand it to the renderer.
static int submit_cmd(struct vgpu_ctx *ctx, uint64_t addr, uint32_t size) {
    if (size == 0)
        return 0;
    if (size > VIRTGPU_MAX_CMD_SIZE || (size & 3) != 0)
        return _EINVAL;
    void *buf = malloc(size);
    if (buf == NULL)
        return _ENOMEM;
    if (user_read((guest_addr_t) addr, buf, size)) {
        free(buf);
        return _EFAULT;
    }
    lock(&renderer_lock, 0);
    bool ok = vkr_renderer_submit_cmd(ctx->ctx_id, buf, size);
    unlock(&renderer_lock);
    free(buf);
    return ok ? 0 : _EINVAL;
}

static int ioctl_create_blob(struct vgpu_file *file, struct drm_virtgpu_resource_create_blob_ *b) {
    struct vgpu_ctx *ctx = file->ctx;
    if (ctx == NULL)
        return _EINVAL;
    if (b->blob_mem != VIRTGPU_BLOB_MEM_HOST3D || b->size == 0 || (b->size & 4095) != 0)
        return _EINVAL;
    // The command that creates the memory the blob exports comes first, as
    // the kernel sends it ahead of the resource.
    int err = submit_cmd(ctx, b->cmd, b->cmd_size);
    if (err < 0)
        return err;

    struct vgpu_res *res = calloc(1, sizeof(*res));
    if (res == NULL)
        return _ENOMEM;
    res->refcount = 1;
    res->res_id = atomic_fetch_add(&next_res_id, 1);
    res->ctx_id = ctx->ctx_id;
    res->blob_mem = b->blob_mem;
    res->size = b->size;
    res->host_fd = -1;
    int fd_type = -1, host_fd = -1;
    uint32_t map_info = 0;
    unsigned char vulkan_info[64];
    lock(&renderer_lock, 0);
    bool ok = vkr_renderer_create_resource(ctx->ctx_id, res->res_id, b->blob_id, b->size,
            b->blob_flags, &fd_type, &host_fd, &map_info, vulkan_info);
    unlock(&renderer_lock);
    if (!ok) {
        free(res);
        return _EINVAL;
    }
    if (fd_type == VIRGL_RESOURCE_FD_SHM_)
        res->host_fd = host_fd;
    else if (host_fd >= 0)
        close(host_fd);
    if ((b->blob_flags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE) && res->host_fd < 0) {
        res_release(res);
        return _EINVAL;
    }

    lock(&file->lock, 0);
    struct vgpu_handle *h = handle_add(file, res, false);
    unlock(&file->lock);
    if (h == NULL) {
        res_release(res);
        return _ENOMEM;
    }
    b->bo_handle = h->handle;
    b->res_handle = res->res_id;
    return 0;
}

static int ioctl_map(struct vgpu_file *file, struct drm_virtgpu_map_ *m) {
    lock(&file->lock, 0);
    struct vgpu_handle *h = handle_find(file, m->handle);
    int err = 0;
    if (h == NULL) {
        err = _ENOENT;
    } else if (h->res->host_fd < 0) {
        err = _EINVAL;
    } else {
        if (h->map_offset == 0) {
            h->map_offset = file->next_map_offset;
            file->next_map_offset += (h->res->size + 0xffff) & ~(uint64_t) 0xffff;
        }
        m->offset = h->map_offset;
    }
    unlock(&file->lock);
    return err;
}

static int ioctl_resource_info(struct vgpu_file *file, struct drm_virtgpu_resource_info_ *info) {
    lock(&file->lock, 0);
    struct vgpu_handle *h = handle_find(file, info->bo_handle);
    if (h != NULL) {
        info->res_handle = h->res->res_id;
        info->size = (uint32_t) h->res->size;
        info->blob_mem = h->res->blob_mem;
    }
    unlock(&file->lock);
    return h != NULL ? 0 : _ENOENT;
}

static int ioctl_gem_close(struct vgpu_file *file, struct drm_gem_close_ *c) {
    lock(&file->lock, 0);
    struct vgpu_handle *h = handle_find(file, c->handle);
    if (h != NULL)
        handle_drop(file, h);
    unlock(&file->lock);
    return h != NULL ? 0 : _EINVAL;
}

static int ioctl_execbuffer(struct vgpu_file *file, struct drm_virtgpu_execbuffer_ *e, unsigned size) {
    struct vgpu_ctx *ctx = file->ctx;
    if (ctx == NULL)
        return _EINVAL;
    if (e->flags & ~(VIRTGPU_EXECBUF_FENCE_FD_IN | VIRTGPU_EXECBUF_FENCE_FD_OUT | VIRTGPU_EXECBUF_RING_IDX))
        return _EINVAL;
    // An older struct (without the syncobj fields) arrives zero-extended.
    if (size >= sizeof(*e) && (e->num_in_syncobjs != 0 || e->num_out_syncobjs != 0))
        return _EINVAL;
    uint32_t ring = (e->flags & VIRTGPU_EXECBUF_RING_IDX) ? e->ring_idx : 0;
    if (ring >= ctx->num_rings)
        return _EINVAL;

    if (e->flags & VIRTGPU_EXECBUF_FENCE_FD_IN) {
        int err = fence_wait(e->fence_fd);
        if (err < 0)
            return err;
    }
    int err = submit_cmd(ctx, e->command, e->size);
    if (err < 0)
        return err;
    if (!(e->flags & VIRTGPU_EXECBUF_FENCE_FD_OUT))
        return 0;

    struct vgpu_fence *fence = calloc(1, sizeof(*fence));
    struct fd *fd = fence != NULL ? adhoc_fd_create(&vgpu_fence_ops) : NULL;
    if (fd == NULL) {
        free(fence);
        return _ENOMEM;
    }
    atomic_fetch_add(&ctx->refcount, 1);
    fence->ctx = ctx;
    fence->ring = ring;
    fence->fd = fd;
    fd->data = fence;
    lock(&vgpu_lock, 0);
    fence->id = ++ctx->next_fence[ring];
    list_add_tail(&ctx->fences, &fence->link);
    unlock(&vgpu_lock);

    lock(&renderer_lock, 0);
    bool ok = vkr_renderer_submit_fence(ctx->ctx_id, VIRGL_RENDERER_FENCE_FLAG_MERGEABLE, ring, fence->id);
    unlock(&renderer_lock);
    if (!ok) {
        fd_close(fd);
        return _EINVAL;
    }
    fd_t f = f_install(fd, O_CLOEXEC_);
    if (f < 0)
        return f;
    e->fence_fd = f;
    return 0;
}

// ---- PRIME -----------------------------------------------------------------

static int prime_close(struct fd *fd) {
    res_release(fd->data);
    return 0;
}

static int prime_mmap(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags) {
    struct vgpu_res *res = fd->data;
    if (res->host_fd < 0)
        return _ENODEV;
    if (offset < 0 || (uint64_t) offset + (uint64_t) pages * PAGE_SIZE > res->size)
        return _EINVAL;
    return host_fd_mmap(res->host_fd, mem, start, pages, offset, prot, flags);
}

// A dma-buf's size is what lseek(SEEK_END) reports; that is how a consumer
// that did not make one learns it.
static off_t_ prime_lseek(struct fd *fd, off_t_ off, int whence) {
    struct vgpu_res *res = fd->data;
    if (off != 0)
        return _EINVAL;
    if (whence == LSEEK_END)
        return (off_t_) res->size;
    if (whence == LSEEK_SET || whence == LSEEK_CUR)
        return 0;
    return _EINVAL;
}

static const struct fd_ops vgpu_prime_ops = {
    .name = "dmabuf",
    .anon_inode_class = "dmabuf",
    .mmap = prime_mmap,
    .lseek = prime_lseek,
    .close = prime_close,
};

static int ioctl_prime_to_fd(struct vgpu_file *file, struct drm_prime_handle_ *p) {
    lock(&file->lock, 0);
    struct vgpu_handle *h = handle_find(file, p->handle);
    struct vgpu_res *res = h != NULL ? h->res : NULL;
    if (res != NULL)
        atomic_fetch_add(&res->refcount, 1);
    unlock(&file->lock);
    if (res == NULL)
        return _ENOENT;
    struct fd *fd = adhoc_fd_create(&vgpu_prime_ops);
    if (fd == NULL) {
        res_release(res);
        return _ENOMEM;
    }
    fd->data = res;
    fd->flags = O_RDWR_;
    fd_t f = f_install(fd, p->flags & DRM_CLOEXEC);
    if (f < 0)
        return f;
    p->fd = f;
    return 0;
}

static int ioctl_prime_to_handle(struct vgpu_file *file, struct drm_prime_handle_ *p) {
    struct fd *fd = f_get(p->fd);
    if (fd == NULL)
        return _EBADF;
    if (fd->ops != &vgpu_prime_ops)
        return _EINVAL;
    struct vgpu_res *res = fd->data;
    lock(&file->lock, 0);
    int err = 0;
    struct vgpu_handle *h;
    list_for_each_entry(&file->handles, h, link) {
        if (h->res == res) {
            p->handle = h->handle;
            unlock(&file->lock);
            return 0;
        }
    }
    // Another file's resource: its context must be told of it, with the
    // memory it lives in.
    bool imported = false;
    if (file->ctx == NULL) {
        err = _EINVAL;
    } else if (res->ctx_id != file->ctx->ctx_id) {
        int dup_fd = res->host_fd >= 0 ? dup(res->host_fd) : -1;
        lock(&renderer_lock, 0);
        imported = dup_fd >= 0 && vkr_renderer_import_resource(file->ctx->ctx_id, res->res_id,
                VIRGL_RESOURCE_FD_SHM_, dup_fd, res->size);
        unlock(&renderer_lock);
        if (!imported) {
            if (dup_fd >= 0)
                close(dup_fd);
            err = _EINVAL;
        }
    }
    if (err == 0) {
        atomic_fetch_add(&res->refcount, 1);
        h = handle_add(file, res, imported);
        if (h == NULL) {
            res_release(res);
            err = _ENOMEM;
        } else {
            p->handle = h->handle;
        }
    }
    unlock(&file->lock);
    return err;
}

// ---- the device ------------------------------------------------------------

static ssize_t vgpu_ioctl_size(int cmd) {
    if (IOC_TYPE(cmd) != DRM_IOCTL_TYPE)
        return -1;
    return IOC_SIZE(cmd);
}

static int vgpu_ioctl(struct fd *fd, int cmd, void *arg) {
    struct vgpu_file *file = fd->data;
    unsigned size = IOC_SIZE(cmd);
#define NEED(type) do { if (size < sizeof(type)) return _EINVAL; } while (0)
    switch (IOC_NR(cmd)) {
        case DRM_NR_VERSION:
            if (size != 36 && size != 64)
                return _EINVAL;
            return ioctl_version(arg, size);
        case DRM_NR_GET_CAP: {
            NEED(struct drm_get_cap_);
            struct drm_get_cap_ *c = arg;
            switch (c->capability) {
                case DRM_CAP_PRIME: c->value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT; return 0;
                case DRM_CAP_TIMESTAMP_MONOTONIC: c->value = 1; return 0;
                case DRM_CAP_SYNCOBJ: case DRM_CAP_SYNCOBJ_TIMELINE: c->value = 0; return 0;
                default: return _EINVAL;
            }
        }
        case DRM_NR_GEM_CLOSE:
            NEED(struct drm_gem_close_);
            return ioctl_gem_close(file, arg);
        case DRM_NR_PRIME_HANDLE_TO_FD:
            NEED(struct drm_prime_handle_);
            return ioctl_prime_to_fd(file, arg);
        case DRM_NR_PRIME_FD_TO_HANDLE:
            NEED(struct drm_prime_handle_);
            return ioctl_prime_to_handle(file, arg);
        case VIRTGPU_NR_GETPARAM:
            NEED(struct drm_virtgpu_getparam_);
            return ioctl_getparam(arg);
        case VIRTGPU_NR_GET_CAPS:
            NEED(struct drm_virtgpu_get_caps_);
            return ioctl_get_caps(arg);
        case VIRTGPU_NR_CONTEXT_INIT:
            NEED(struct drm_virtgpu_context_init_);
            return ioctl_context_init(file, arg);
        case VIRTGPU_NR_RESOURCE_CREATE_BLOB:
            NEED(struct drm_virtgpu_resource_create_blob_);
            return ioctl_create_blob(file, arg);
        case VIRTGPU_NR_MAP:
            NEED(struct drm_virtgpu_map_);
            return ioctl_map(file, arg);
        case VIRTGPU_NR_RESOURCE_INFO:
            NEED(struct drm_virtgpu_resource_info_);
            return ioctl_resource_info(file, arg);
        case VIRTGPU_NR_EXECBUFFER: {
            // The syncobj fields were added later; an older struct is 32 or 40
            // bytes and the rest is zero.
            if (size < 32 || size > sizeof(struct drm_virtgpu_execbuffer_))
                return _EINVAL;
            struct drm_virtgpu_execbuffer_ e = {0};
            memcpy(&e, arg, size);
            int err = ioctl_execbuffer(file, &e, size);
            memcpy(arg, &e, size);
            return err;
        }
        default: {
            static atomic_int reported;
            if (atomic_fetch_add(&reported, 1) < 8)
                printk("virtgpu: unhandled ioctl %#x (nr %#x, %u bytes)\n", cmd, IOC_NR(cmd), size);
            return _EINVAL;
        }
    }
#undef NEED
}

static int vgpu_mmap(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags) {
    struct vgpu_file *file = fd->data;
    uint64_t len = (uint64_t) pages * PAGE_SIZE;
    lock(&file->lock, 0);
    struct vgpu_handle *h, *found = NULL;
    list_for_each_entry(&file->handles, h, link) {
        if (h->map_offset != 0 && (uint64_t) offset >= h->map_offset &&
                (uint64_t) offset + len <= h->map_offset + h->res->size) {
            found = h;
            break;
        }
    }
    int err = found != NULL
        ? host_fd_mmap(found->res->host_fd, mem, start, pages, (off_t) (offset - found->map_offset), prot, flags)
        : _EINVAL;
    unlock(&file->lock);
    return err;
}

static int vgpu_close(struct fd *fd) {
    struct vgpu_file *file = fd->data;
    lock(&file->lock, 0);
    struct vgpu_handle *h, *tmp;
    list_for_each_entry_safe(&file->handles, h, tmp, link)
        handle_drop(file, h);
    struct vgpu_ctx *ctx = file->ctx;
    unlock(&file->lock);
    if (ctx != NULL) {
        lock(&renderer_lock, 0);
        vkr_renderer_destroy_context(ctx->ctx_id);
        unlock(&renderer_lock);
        // A fence the renderer will now never retire must not hang whoever
        // polls it.
        struct woken w = {.count = 0};
        bool more = true;
        while (more) {
            lock(&vgpu_lock, 0);
            for (uint32_t r = 0; r < VIRTGPU_MAX_RINGS; r++)
                atomic_store(&ctx->retired[r], UINT64_MAX);
            ctx->destroyed = true;
            more = collect_signaled(ctx, &w);
            unlock(&vgpu_lock);
            wake_fences(&w);
        }
        ctx_release(ctx);
    }
    free(file);
    return 0;
}

static int vgpu_open(int major, int minor, struct fd *fd) {
    (void) major;
    if (minor != DEV_VIRTGPU_RENDER_MINOR || !virtgpu_available())
        return _ENXIO;
    if (!renderer_ready())
        return _ENODEV;
    struct vgpu_file *file = calloc(1, sizeof(*file));
    if (file == NULL)
        return _ENOMEM;
    lock_init(&file->lock, "virtgpu_file\0");
    list_init(&file->handles);
    file->next_handle = 1;
    // Fake offsets for mmap, as the DRM core hands out: far from 0 so a stray
    // mmap of the node at offset 0 maps nothing.
    file->next_map_offset = (uint64_t) 1 << 32;
    fd->data = file;
    return 0;
}

struct dev_ops virtgpu_dev = {
    .open = vgpu_open,
    .fd = {
        .name = "virtgpu",
        .ioctl_size = vgpu_ioctl_size,
        .ioctl = vgpu_ioctl,
        .mmap = vgpu_mmap,
        .close = vgpu_close,
    },
};

#else

bool virtgpu_available(void) {
    return false;
}

#endif
