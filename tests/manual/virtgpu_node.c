// /dev/dri/renderD128, the virtio-gpu render node (fs/virtgpu.c, #484), as
// Mesa's Venus driver and libdrm see it -- without Mesa: the sysfs identity
// drmGetDevices2 reads, and the ioctls, blob mapping, out-fences and PRIME
// that vn_renderer_virtgpu.c uses.
//
// The node exists only in a build with the renderer (-Dgpu, on Darwin hosts);
// elsewhere this is a SKIP. Compared against the uAPI (include/uapi/drm) and
// libdrm's xf86drm.c rather than a Linux run: no Linux host here has a
// virtio-gpu with a Venus renderer behind it.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "test_common.h"

#define NODE "/dev/dri/renderD128"

struct drm_version_ { int major, minor, patch; size_t name_len; char *name;
                      size_t date_len; char *date; size_t desc_len; char *desc; };
struct drm_gem_close_ { uint32_t handle, pad; };
struct drm_prime_handle_ { uint32_t handle, flags; int32_t fd; };
struct virtgpu_getparam_ { uint64_t param, value; };
struct virtgpu_get_caps_ { uint32_t id, ver; uint64_t addr; uint32_t size, pad; };
struct virtgpu_ctx_param_ { uint64_t param, value; };
struct virtgpu_ctx_init_ { uint32_t num_params, pad; uint64_t params; };
struct virtgpu_blob_ { uint32_t blob_mem, blob_flags, bo_handle, res_handle;
                       uint64_t size; uint32_t pad, cmd_size; uint64_t cmd, blob_id; };
struct virtgpu_map_ { uint64_t offset; uint32_t handle, pad; };
struct virtgpu_info_ { uint32_t bo_handle, res_handle, size, blob_mem; };
struct virtgpu_execbuffer_ { uint32_t flags, size; uint64_t command, bo_handles;
                             uint32_t num_bo_handles; int32_t fence_fd; uint32_t ring_idx,
                             syncobj_stride, num_in, num_out; uint64_t in, out; };

#define DRM_IOCTL_VERSION_ _IOWR('d', 0x00, struct drm_version_)
#define DRM_IOCTL_GEM_CLOSE_ _IOW('d', 0x09, struct drm_gem_close_)
#define DRM_IOCTL_PRIME_TO_FD_ _IOWR('d', 0x2d, struct drm_prime_handle_)
#define DRM_IOCTL_PRIME_TO_HANDLE_ _IOWR('d', 0x2e, struct drm_prime_handle_)
#define VIRTGPU_MAP_ _IOWR('d', 0x41, struct virtgpu_map_)
#define VIRTGPU_EXECBUFFER_ _IOWR('d', 0x42, struct virtgpu_execbuffer_)
#define VIRTGPU_GETPARAM_ _IOWR('d', 0x43, struct virtgpu_getparam_)
#define VIRTGPU_RESOURCE_INFO_ _IOWR('d', 0x45, struct virtgpu_info_)
#define VIRTGPU_GET_CAPS_ _IOWR('d', 0x49, struct virtgpu_get_caps_)
#define VIRTGPU_CREATE_BLOB_ _IOWR('d', 0x4a, struct virtgpu_blob_)
#define VIRTGPU_CONTEXT_INIT_ _IOWR('d', 0x4b, struct virtgpu_ctx_init_)

static void ck(const char *label, long got, long want) {
    if (got != want)
        failf(label, (uint64_t) got, 0, 0, (uint64_t) want, 0, 0);
    test_logf("  %-58s got=%ld want=%ld\n", label, got, want);
}

// 0, or -errno.
static long io(int fd, unsigned long req, void *arg) {
    return ioctl(fd, req, arg) < 0 ? -errno : 0;
}

static void check_link(const char *path, const char *want) {
    char buf[256];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    buf[n > 0 ? n : 0] = '\0';
    if (n < 0 || strcmp(buf, want) != 0)
        printf("FAIL readlink %s: \"%s\", want \"%s\"\n", path, n < 0 ? strerror(errno) : buf, want),
        failures_total++;
}

static void check_sysfs(void) {
    struct stat st;
    // What drmNodeIsDRM, drmParseSubsystemType and the platform bus-info
    // parsers read, in that order.
    check_link("/sys/dev/char/226:128", "../../devices/platform/aok-gpu/drm/renderD128");
    ck("/sys/dev/char/226:128/device/drm is a directory",
       stat("/sys/dev/char/226:128/device/drm", &st) == 0 && S_ISDIR(st.st_mode), 1);
    check_link("/sys/dev/char/226:128/device/subsystem", "../../../bus/platform");
    char buf[256] = "";
    FILE *f = fopen("/sys/dev/char/226:128/device/uevent", "r");
    size_t n = f != NULL ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    buf[n] = '\0';
    if (f != NULL)
        fclose(f);
    ck("the device's uevent carries MODALIAS=platform:aok-gpu",
       strstr(buf, "MODALIAS=platform:aok-gpu\n") != NULL, 1);
    f = fopen("/sys/dev/char/226:128/dev", "r");
    n = f != NULL ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    buf[n] = '\0';
    if (f != NULL)
        fclose(f);
    ck("renderD128/dev is 226:128", strcmp(buf, "226:128\n") == 0, 1);
    check_link("/sys/class/drm/renderD128", "../../devices/platform/aok-gpu/drm/renderD128");
}

int main(int argc, char **argv) {
    test_init(argc, argv);
    alarm(test_watchdog_secs(60));
    struct stat st;
    if (stat(NODE, &st) != 0) {
        printf("virtgpu_node: SKIP (no " NODE " in this build)\n");
        return 0;
    }
    ck("the node is char 226:128", S_ISCHR(st.st_mode) && major(st.st_rdev) == 226 &&
       minor(st.st_rdev) == 128, 1);
    check_sysfs();

    int fd = open(NODE, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        // The renderer could not start (no Metal device): a host limit, not a
        // conformance failure, and Mesa sees the same refusal.
        printf("virtgpu_node: SKIP (open: %s)\n", strerror(errno));
        return 0;
    }

    char name[32] = "", date[4] = "", desc[64] = "";
    struct drm_version_ v = {.name_len = sizeof(name) - 1, .name = name,
                             .date_len = sizeof(date) - 1, .date = date,
                             .desc_len = sizeof(desc) - 1, .desc = desc};
    ck("DRM_IOCTL_VERSION", io(fd, DRM_IOCTL_VERSION_, &v), 0);
    ck("  names the virtio_gpu driver", strcmp(name, "virtio_gpu") == 0, 1);
    ck("  major version 0 (Mesa requires it)", v.major, 0);
    ck("  name_len is the full length", (long) v.name_len, 10);
    char tiny[4] = "";
    struct drm_version_ v2 = {.name_len = 3, .name = tiny};
    ck("a short buffer takes a prefix", io(fd, DRM_IOCTL_VERSION_, &v2), 0);
    ck("  of that many bytes", memcmp(tiny, "vir", 3) == 0 && tiny[3] == '\0', 1);
    ck("  and still reports the full length", (long) v2.name_len, 10);

    uint64_t val;
    struct virtgpu_getparam_ gp = {.value = (uintptr_t) &val};
    static const uint64_t required[] = {1, 2, 3, 4, 6};   // 3D, capset fix, blob, host visible, ctx init
    for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        val = 0;
        gp.param = required[i];
        ck("GETPARAM of a parameter Venus requires", io(fd, VIRTGPU_GETPARAM_, &gp), 0);
        ck("  is 1", (long) val, 1);
    }
    val = 0;
    gp.param = 7;
    ck("GETPARAM SUPPORTED_CAPSET_IDs", io(fd, VIRTGPU_GETPARAM_, &gp), 0);
    ck("  names Venus (capset 4)", (long) (val & (1u << 4)), 1 << 4);
    gp.param = 999;
    ck("GETPARAM of an unknown parameter is EINVAL", io(fd, VIRTGPU_GETPARAM_, &gp), -EINVAL);

    uint32_t caps[64] = {0};
    struct virtgpu_get_caps_ gc = {.id = 4, .addr = (uintptr_t) caps, .size = sizeof(caps)};
    ck("GET_CAPS Venus", io(fd, VIRTGPU_GET_CAPS_, &gc), 0);
    ck("  has a wire format version", caps[0] != 0, 1);
    gc.id = 1;
    ck("GET_CAPS virgl (not built) is EINVAL", io(fd, VIRTGPU_GET_CAPS_, &gc), -EINVAL);

    struct virtgpu_blob_ blob = {.blob_mem = 2, .blob_flags = 1, .size = 4096};
    ck("CREATE_BLOB before CONTEXT_INIT is EINVAL", io(fd, VIRTGPU_CREATE_BLOB_, &blob), -EINVAL);
    struct virtgpu_ctx_param_ params[3] = {{1, 4}, {2, 64}, {3, 0}};
    struct virtgpu_ctx_init_ ci = {.num_params = 3, .params = (uintptr_t) params};
    ck("CONTEXT_INIT for Venus, 64 rings", io(fd, VIRTGPU_CONTEXT_INIT_, &ci), 0);
    ck("  a second one is EEXIST", io(fd, VIRTGPU_CONTEXT_INIT_, &ci), -EEXIST);

    // A shared-memory blob (blob_id 0, what Venus uses for its rings).
    ck("CREATE_BLOB, HOST3D and mappable", io(fd, VIRTGPU_CREATE_BLOB_, &blob), 0);
    ck("  returns a handle", blob.bo_handle != 0, 1);
    struct virtgpu_info_ info = {.bo_handle = blob.bo_handle};
    ck("RESOURCE_INFO", io(fd, VIRTGPU_RESOURCE_INFO_, &info), 0);
    ck("  size", info.size, 4096);
    ck("  resource id", info.res_handle, blob.res_handle);
    struct virtgpu_map_ map = {.handle = blob.bo_handle};
    ck("MAP", io(fd, VIRTGPU_MAP_, &map), 0);
    unsigned char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t) map.offset);
    ck("mmap at MAP's offset", p != MAP_FAILED, 1);
    ck("mmap at offset 0 maps nothing",
       mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0) == MAP_FAILED, 1);

    // PRIME: a dma-buf of the same memory, whose size lseek reports.
    struct drm_prime_handle_ ph = {.handle = blob.bo_handle, .flags = O_CLOEXEC | O_RDWR};
    ck("PRIME_HANDLE_TO_FD", io(fd, DRM_IOCTL_PRIME_TO_FD_, &ph), 0);
    ck("  lseek(SEEK_END) is the size", (long) lseek(ph.fd, 0, SEEK_END), 4096);
    if (p != MAP_FAILED) {
        unsigned char *q = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, ph.fd, 0);
        ck("  mmap of the dma-buf", q != MAP_FAILED, 1);
        if (q != MAP_FAILED) {
            p[100] = 0x5a;
            ck("  shares the blob's pages", q[100], 0x5a);
            munmap(q, 4096);
        }
        munmap(p, 4096);
    }
    struct drm_prime_handle_ back = {.fd = ph.fd};
    ck("PRIME_FD_TO_HANDLE in the same file", io(fd, DRM_IOCTL_PRIME_TO_HANDLE_, &back), 0);
    ck("  is the same handle", back.handle, blob.bo_handle);
    int devnull = open("/dev/null", O_RDONLY);
    struct drm_prime_handle_ bad = {.fd = devnull};
    ck("PRIME_FD_TO_HANDLE of another kind of fd is EINVAL",
       io(fd, DRM_IOCTL_PRIME_TO_HANDLE_, &bad), -EINVAL);
    close(devnull);
    close(ph.fd);

    // An empty submission with an out-fence on ring 0: the fence is a
    // descriptor that polls readable once the renderer retires it.
    struct virtgpu_execbuffer_ eb = {.flags = 0x02 | 0x04, .ring_idx = 0, .fence_fd = -1};
    ck("EXECBUFFER with FENCE_FD_OUT", io(fd, VIRTGPU_EXECBUFFER_, &eb), 0);
    ck("  returns a fence descriptor", eb.fence_fd >= 0, 1);
    if (eb.fence_fd >= 0) {
        struct pollfd pfd = {.fd = eb.fence_fd, .events = POLLIN};
        ck("  which polls readable", poll(&pfd, 1, 5000), 1);
        close(eb.fence_fd);
    }
    eb.ring_idx = 64;
    ck("EXECBUFFER on a ring past NUM_RINGS is EINVAL", io(fd, VIRTGPU_EXECBUFFER_, &eb), -EINVAL);
    struct virtgpu_execbuffer_ odd = {.size = 3, .command = (uintptr_t) caps};
    ck("EXECBUFFER of a stream not a whole number of words is EINVAL",
       io(fd, VIRTGPU_EXECBUFFER_, &odd), -EINVAL);

    struct drm_gem_close_ gcl = {.handle = blob.bo_handle};
    ck("GEM_CLOSE", io(fd, DRM_IOCTL_GEM_CLOSE_, &gcl), 0);
    ck("  a second time is EINVAL", io(fd, DRM_IOCTL_GEM_CLOSE_, &gcl), -EINVAL);
    ck("  and RESOURCE_INFO no longer knows it", io(fd, VIRTGPU_RESOURCE_INFO_, &info), -ENOENT);

    close(fd);
    return finish_suite("virtgpu_node");
}
