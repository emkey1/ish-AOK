// Offscreen Vulkan throughput probe for #484, run in a guest: draws T blended
// triangles per frame into a WxH image, F frames, two in flight; -r copies each
// frame back to host-visible memory (what a shared-memory present costs).
// Prints fps and a checksum of the last frame, so two drivers can be compared
// for correctness as well as speed. The driver is the loader's choice:
//
//   VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/virtio_icd.aarch64.json ./vkbench
//   VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.aarch64.json ./vkbench
//
// Built on the Mac, needing only the Vulkan headers (it dlopens the loader):
//
//   zig cc -target aarch64-linux-musl -dynamic -O2 -I/opt/homebrew/include \
//       -o vkbench tools/vkbench.c
//
// CPU per frame is the host's: `time` the whole ish process, less a -f 0 run.
// The shaders are embedded SPIR-V, from glslangValidator -V of:
//
//   #version 450
//   layout(push_constant) uniform PC { float t; } pc;
//   layout(location = 0) out vec3 col;
//   void main() {
//       int tri = gl_VertexIndex / 3;
//       int v = gl_VertexIndex % 3;
//       float a = float(tri) * 0.618 + pc.t;
//       vec2 c = vec2(sin(a * 1.3), cos(a * 0.7)) * 0.8;
//       vec2 o[3] = vec2[](vec2(0.0, -0.1), vec2(0.1, 0.1), vec2(-0.1, 0.1));
//       gl_Position = vec4(c + o[v], fract(float(tri) * 0.0001), 1.0);
//       col = vec3(fract(a), fract(a * 1.7), fract(a * 2.3));
//   }
//
//   #version 450
//   layout(location = 0) in vec3 col;
//   layout(location = 0) out vec4 o;
//   void main() {
//       vec3 c = col;
//       for (int i = 0; i < 16; i++)
//           c = fract(c * 1.37 + 0.11);
//       o = vec4(c, 1.0);
//   }
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define G(n) static PFN_##n n;
#define GLOBALS X(vkCreateInstance)
#define INSTANCE X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define DEVICE X(vkGetDeviceQueue) X(vkCreateImage) X(vkGetImageMemoryRequirements) \
    X(vkAllocateMemory) X(vkBindImageMemory) X(vkCreateImageView) X(vkCreateRenderPass) \
    X(vkCreateFramebuffer) X(vkCreateShaderModule) X(vkCreatePipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdPushConstants) \
    X(vkCmdDraw) X(vkCmdCopyImageToBuffer) X(vkCreateFence) X(vkWaitForFences) \
    X(vkResetFences) X(vkQueueSubmit) X(vkDeviceWaitIdle)
#define X(n) G(n)
GLOBALS INSTANCE DEVICE
#undef X
static PFN_vkGetInstanceProcAddr gipa;

#define CK(e) do { VkResult r_ = (e); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s: %d\n", #e, r_); exit(1); } } while (0)

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static const uint32_t vert_spv[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x0000005e, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0008000f, 0x00000000,
    0x00000004, 0x6e69616d, 0x00000000, 0x0000000a, 0x0000003f, 0x00000052,
    0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00030005, 0x00000008, 0x00697274, 0x00060005, 0x0000000a,
    0x565f6c67, 0x65747265, 0x646e4978, 0x00007865, 0x00030005, 0x0000000e,
    0x00000076, 0x00030005, 0x00000013, 0x00000061, 0x00030005, 0x00000018,
    0x00004350, 0x00040006, 0x00000018, 0x00000000, 0x00000074, 0x00030005,
    0x0000001a, 0x00006370, 0x00030005, 0x00000022, 0x00000063, 0x00030005,
    0x00000032, 0x0000006f, 0x00060005, 0x0000003d, 0x505f6c67, 0x65567265,
    0x78657472, 0x00000000, 0x00060006, 0x0000003d, 0x00000000, 0x505f6c67,
    0x7469736f, 0x006e6f69, 0x00070006, 0x0000003d, 0x00000001, 0x505f6c67,
    0x746e696f, 0x657a6953, 0x00000000, 0x00070006, 0x0000003d, 0x00000002,
    0x435f6c67, 0x4470696c, 0x61747369, 0x0065636e, 0x00070006, 0x0000003d,
    0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369, 0x0065636e, 0x00030005,
    0x0000003f, 0x00000000, 0x00030005, 0x00000052, 0x006c6f63, 0x00040047,
    0x0000000a, 0x0000000b, 0x0000002a, 0x00030047, 0x00000018, 0x00000002,
    0x00050048, 0x00000018, 0x00000000, 0x00000023, 0x00000000, 0x00030047,
    0x0000003d, 0x00000002, 0x00050048, 0x0000003d, 0x00000000, 0x0000000b,
    0x00000000, 0x00050048, 0x0000003d, 0x00000001, 0x0000000b, 0x00000001,
    0x00050048, 0x0000003d, 0x00000002, 0x0000000b, 0x00000003, 0x00050048,
    0x0000003d, 0x00000003, 0x0000000b, 0x00000004, 0x00040047, 0x00000052,
    0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000001, 0x00040020,
    0x00000007, 0x00000007, 0x00000006, 0x00040020, 0x00000009, 0x00000001,
    0x00000006, 0x0004003b, 0x00000009, 0x0000000a, 0x00000001, 0x0004002b,
    0x00000006, 0x0000000c, 0x00000003, 0x00030016, 0x00000011, 0x00000020,
    0x00040020, 0x00000012, 0x00000007, 0x00000011, 0x0004002b, 0x00000011,
    0x00000016, 0x3f1e353f, 0x0003001e, 0x00000018, 0x00000011, 0x00040020,
    0x00000019, 0x00000009, 0x00000018, 0x0004003b, 0x00000019, 0x0000001a,
    0x00000009, 0x0004002b, 0x00000006, 0x0000001b, 0x00000000, 0x00040020,
    0x0000001c, 0x00000009, 0x00000011, 0x00040017, 0x00000020, 0x00000011,
    0x00000002, 0x00040020, 0x00000021, 0x00000007, 0x00000020, 0x0004002b,
    0x00000011, 0x00000024, 0x3fa66666, 0x0004002b, 0x00000011, 0x00000028,
    0x3f333333, 0x0004002b, 0x00000011, 0x0000002c, 0x3f4ccccd, 0x00040015,
    0x0000002e, 0x00000020, 0x00000000, 0x0004002b, 0x0000002e, 0x0000002f,
    0x00000003, 0x0004001c, 0x00000030, 0x00000020, 0x0000002f, 0x00040020,
    0x00000031, 0x00000007, 0x00000030, 0x0004002b, 0x00000011, 0x00000033,
    0x00000000, 0x0004002b, 0x00000011, 0x00000034, 0xbdcccccd, 0x0005002c,
    0x00000020, 0x00000035, 0x00000033, 0x00000034, 0x0004002b, 0x00000011,
    0x00000036, 0x3dcccccd, 0x0005002c, 0x00000020, 0x00000037, 0x00000036,
    0x00000036, 0x0005002c, 0x00000020, 0x00000038, 0x00000034, 0x00000036,
    0x0006002c, 0x00000030, 0x00000039, 0x00000035, 0x00000037, 0x00000038,
    0x00040017, 0x0000003a, 0x00000011, 0x00000004, 0x0004002b, 0x0000002e,
    0x0000003b, 0x00000001, 0x0004001c, 0x0000003c, 0x00000011, 0x0000003b,
    0x0006001e, 0x0000003d, 0x0000003a, 0x00000011, 0x0000003c, 0x0000003c,
    0x00040020, 0x0000003e, 0x00000003, 0x0000003d, 0x0004003b, 0x0000003e,
    0x0000003f, 0x00000003, 0x0004002b, 0x00000011, 0x00000047, 0x38d1b717,
    0x0004002b, 0x00000011, 0x0000004a, 0x3f800000, 0x00040020, 0x0000004e,
    0x00000003, 0x0000003a, 0x00040017, 0x00000050, 0x00000011, 0x00000003,
    0x00040020, 0x00000051, 0x00000003, 0x00000050, 0x0004003b, 0x00000051,
    0x00000052, 0x00000003, 0x0004002b, 0x00000011, 0x00000056, 0x3fd9999a,
    0x0004002b, 0x00000011, 0x0000005a, 0x40133333, 0x00050036, 0x00000002,
    0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b,
    0x00000007, 0x00000008, 0x00000007, 0x0004003b, 0x00000007, 0x0000000e,
    0x00000007, 0x0004003b, 0x00000012, 0x00000013, 0x00000007, 0x0004003b,
    0x00000021, 0x00000022, 0x00000007, 0x0004003b, 0x00000031, 0x00000032,
    0x00000007, 0x0004003d, 0x00000006, 0x0000000b, 0x0000000a, 0x00050087,
    0x00000006, 0x0000000d, 0x0000000b, 0x0000000c, 0x0003003e, 0x00000008,
    0x0000000d, 0x0004003d, 0x00000006, 0x0000000f, 0x0000000a, 0x0005008b,
    0x00000006, 0x00000010, 0x0000000f, 0x0000000c, 0x0003003e, 0x0000000e,
    0x00000010, 0x0004003d, 0x00000006, 0x00000014, 0x00000008, 0x0004006f,
    0x00000011, 0x00000015, 0x00000014, 0x00050085, 0x00000011, 0x00000017,
    0x00000015, 0x00000016, 0x00050041, 0x0000001c, 0x0000001d, 0x0000001a,
    0x0000001b, 0x0004003d, 0x00000011, 0x0000001e, 0x0000001d, 0x00050081,
    0x00000011, 0x0000001f, 0x00000017, 0x0000001e, 0x0003003e, 0x00000013,
    0x0000001f, 0x0004003d, 0x00000011, 0x00000023, 0x00000013, 0x00050085,
    0x00000011, 0x00000025, 0x00000023, 0x00000024, 0x0006000c, 0x00000011,
    0x00000026, 0x00000001, 0x0000000d, 0x00000025, 0x0004003d, 0x00000011,
    0x00000027, 0x00000013, 0x00050085, 0x00000011, 0x00000029, 0x00000027,
    0x00000028, 0x0006000c, 0x00000011, 0x0000002a, 0x00000001, 0x0000000e,
    0x00000029, 0x00050050, 0x00000020, 0x0000002b, 0x00000026, 0x0000002a,
    0x0005008e, 0x00000020, 0x0000002d, 0x0000002b, 0x0000002c, 0x0003003e,
    0x00000022, 0x0000002d, 0x0003003e, 0x00000032, 0x00000039, 0x0004003d,
    0x00000020, 0x00000040, 0x00000022, 0x0004003d, 0x00000006, 0x00000041,
    0x0000000e, 0x00050041, 0x00000021, 0x00000042, 0x00000032, 0x00000041,
    0x0004003d, 0x00000020, 0x00000043, 0x00000042, 0x00050081, 0x00000020,
    0x00000044, 0x00000040, 0x00000043, 0x0004003d, 0x00000006, 0x00000045,
    0x00000008, 0x0004006f, 0x00000011, 0x00000046, 0x00000045, 0x00050085,
    0x00000011, 0x00000048, 0x00000046, 0x00000047, 0x0006000c, 0x00000011,
    0x00000049, 0x00000001, 0x0000000a, 0x00000048, 0x00050051, 0x00000011,
    0x0000004b, 0x00000044, 0x00000000, 0x00050051, 0x00000011, 0x0000004c,
    0x00000044, 0x00000001, 0x00070050, 0x0000003a, 0x0000004d, 0x0000004b,
    0x0000004c, 0x00000049, 0x0000004a, 0x00050041, 0x0000004e, 0x0000004f,
    0x0000003f, 0x0000001b, 0x0003003e, 0x0000004f, 0x0000004d, 0x0004003d,
    0x00000011, 0x00000053, 0x00000013, 0x0006000c, 0x00000011, 0x00000054,
    0x00000001, 0x0000000a, 0x00000053, 0x0004003d, 0x00000011, 0x00000055,
    0x00000013, 0x00050085, 0x00000011, 0x00000057, 0x00000055, 0x00000056,
    0x0006000c, 0x00000011, 0x00000058, 0x00000001, 0x0000000a, 0x00000057,
    0x0004003d, 0x00000011, 0x00000059, 0x00000013, 0x00050085, 0x00000011,
    0x0000005b, 0x00000059, 0x0000005a, 0x0006000c, 0x00000011, 0x0000005c,
    0x00000001, 0x0000000a, 0x0000005b, 0x00060050, 0x00000050, 0x0000005d,
    0x00000054, 0x00000058, 0x0000005c, 0x0003003e, 0x00000052, 0x0000005d,
    0x000100fd, 0x00010038,
};

static const uint32_t frag_spv[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x0000002d, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000004,
    0x00000004, 0x6e69616d, 0x00000000, 0x0000000b, 0x00000026, 0x00030010,
    0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005,
    0x00000004, 0x6e69616d, 0x00000000, 0x00030005, 0x00000009, 0x00000063,
    0x00030005, 0x0000000b, 0x006c6f63, 0x00030005, 0x0000000f, 0x00000069,
    0x00030005, 0x00000026, 0x0000006f, 0x00040047, 0x0000000b, 0x0000001e,
    0x00000000, 0x00040047, 0x00000026, 0x0000001e, 0x00000000, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006,
    0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000003, 0x00040020,
    0x00000008, 0x00000007, 0x00000007, 0x00040020, 0x0000000a, 0x00000001,
    0x00000007, 0x0004003b, 0x0000000a, 0x0000000b, 0x00000001, 0x00040015,
    0x0000000d, 0x00000020, 0x00000001, 0x00040020, 0x0000000e, 0x00000007,
    0x0000000d, 0x0004002b, 0x0000000d, 0x00000010, 0x00000000, 0x0004002b,
    0x0000000d, 0x00000017, 0x00000010, 0x00020014, 0x00000018, 0x0004002b,
    0x00000006, 0x0000001b, 0x3faf5c29, 0x0004002b, 0x00000006, 0x0000001d,
    0x3de147ae, 0x0004002b, 0x0000000d, 0x00000022, 0x00000001, 0x00040017,
    0x00000024, 0x00000006, 0x00000004, 0x00040020, 0x00000025, 0x00000003,
    0x00000024, 0x0004003b, 0x00000025, 0x00000026, 0x00000003, 0x0004002b,
    0x00000006, 0x00000028, 0x3f800000, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000008,
    0x00000009, 0x00000007, 0x0004003b, 0x0000000e, 0x0000000f, 0x00000007,
    0x0004003d, 0x00000007, 0x0000000c, 0x0000000b, 0x0003003e, 0x00000009,
    0x0000000c, 0x0003003e, 0x0000000f, 0x00000010, 0x000200f9, 0x00000011,
    0x000200f8, 0x00000011, 0x000400f6, 0x00000013, 0x00000014, 0x00000000,
    0x000200f9, 0x00000015, 0x000200f8, 0x00000015, 0x0004003d, 0x0000000d,
    0x00000016, 0x0000000f, 0x000500b1, 0x00000018, 0x00000019, 0x00000016,
    0x00000017, 0x000400fa, 0x00000019, 0x00000012, 0x00000013, 0x000200f8,
    0x00000012, 0x0004003d, 0x00000007, 0x0000001a, 0x00000009, 0x0005008e,
    0x00000007, 0x0000001c, 0x0000001a, 0x0000001b, 0x00060050, 0x00000007,
    0x0000001e, 0x0000001d, 0x0000001d, 0x0000001d, 0x00050081, 0x00000007,
    0x0000001f, 0x0000001c, 0x0000001e, 0x0006000c, 0x00000007, 0x00000020,
    0x00000001, 0x0000000a, 0x0000001f, 0x0003003e, 0x00000009, 0x00000020,
    0x000200f9, 0x00000014, 0x000200f8, 0x00000014, 0x0004003d, 0x0000000d,
    0x00000021, 0x0000000f, 0x00050080, 0x0000000d, 0x00000023, 0x00000021,
    0x00000022, 0x0003003e, 0x0000000f, 0x00000023, 0x000200f9, 0x00000011,
    0x000200f8, 0x00000013, 0x0004003d, 0x00000007, 0x00000027, 0x00000009,
    0x00050051, 0x00000006, 0x00000029, 0x00000027, 0x00000000, 0x00050051,
    0x00000006, 0x0000002a, 0x00000027, 0x00000001, 0x00050051, 0x00000006,
    0x0000002b, 0x00000027, 0x00000002, 0x00070050, 0x00000024, 0x0000002c,
    0x00000029, 0x0000002a, 0x0000002b, 0x00000028, 0x0003003e, 0x00000026,
    0x0000002c, 0x000100fd, 0x00010038,
};

static uint32_t mem_type(VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want)
            return i;
    fprintf(stderr, "no memory type\n");
    exit(1);
}

int main(int argc, char **argv) {
    int frames = 600, w = 1280, h = 720, tris = 20000, readback = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t")) tris = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-w")) w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h")) h = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-r")) readback = 1;
    }
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr) dlsym(lib, "vkGetInstanceProcAddr");
#define X(n) n = (PFN_##n) gipa(NULL, #n);
    GLOBALS
#undef X
    double t0 = now();
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    CK(vkCreateInstance(&ici, NULL, &inst));
#define X(n) n = (PFN_##n) gipa(inst, #n);
    INSTANCE
#undef X
    uint32_t n = 1;
    VkPhysicalDevice pd;
    VkResult er = vkEnumeratePhysicalDevices(inst, &n, &pd);
    if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || n == 0) { fprintf(stderr, "no device\n"); return 1; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t qfc = 16;
    VkQueueFamilyProperties qf[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, qf);
    uint32_t qfi = 0;
    while (!(qf[qfi].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfi++;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    VkDevice dev;
    CK(vkCreateDevice(pd, &dci, NULL, &dev));
#define X(n) n = (PFN_##n) vkGetDeviceProcAddr(dev, #n);
    DEVICE
#undef X
    VkQueue q;
    vkGetDeviceQueue(dev, qfi, 0, &q);

    VkImageCreateInfo imci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage img;
    CK(vkCreateImage(dev, &imci, NULL, &img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
        .memoryTypeIndex = mem_type(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory imem;
    CK(vkAllocateMemory(dev, &mai, NULL, &imem));
    CK(vkBindImageMemory(dev, img, imem, 0));
    VkImageViewCreateInfo ivci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = imci.format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView view;
    CK(vkCreateImageView(dev, &ivci, NULL, &view));

    VkAttachmentDescription att = {.format = imci.format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ref};
    VkSubpassDependency dep = {.srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
        .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &dep};
    VkRenderPass rp;
    CK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
    VkFramebufferCreateInfo fbci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
        .attachmentCount = 1, .pAttachments = &view, .width = w, .height = h, .layers = 1};
    VkFramebuffer fb;
    CK(vkCreateFramebuffer(dev, &fbci, NULL, &fb));

    VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(vert_spv), .pCode = vert_spv};
    VkShaderModule vs, fs;
    CK(vkCreateShaderModule(dev, &smci, NULL, &vs));
    smci.codeSize = sizeof(frag_spv);
    smci.pCode = frag_spv;
    CK(vkCreateShaderModule(dev, &smci, NULL, &fs));
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 4};
    VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
    VkPipelineLayout pl;
    CK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkViewport vp = {0, 0, w, h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    VkPipelineViewportStateCreateInfo vps = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba};
    VkGraphicsPipelineCreateInfo gpci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .layout = pl, .renderPass = rp};
    VkPipeline pipe;
    CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

    VkBuffer buf = VK_NULL_HANDLE;
    unsigned char *pixels = NULL;
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize) w * h * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    CK(vkCreateBuffer(dev, &bci, NULL, &buf));
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mem_type(&mp, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory bmem;
    CK(vkAllocateMemory(dev, &mai, NULL, &bmem));
    CK(vkBindBufferMemory(dev, buf, bmem, 0));
    CK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **) &pixels));

    VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qfi};
    VkCommandPool pool;
    CK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
    VkCommandBuffer cmd[2];
    VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
    CK(vkAllocateCommandBuffers(dev, &cbai, cmd));
    VkFence fence[2];
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    CK(vkCreateFence(dev, &fci, NULL, &fence[0]));
    CK(vkCreateFence(dev, &fci, NULL, &fence[1]));
    double t_setup = now() - t0;

    double t1 = now();
    for (int f = 0; f < frames; f++) {
        int k = f & 1;
        CK(vkWaitForFences(dev, 1, &fence[k], VK_TRUE, UINT64_MAX));
        CK(vkResetFences(dev, 1, &fence[k]));
        vkResetCommandBuffer(cmd[k], 0);
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        CK(vkBeginCommandBuffer(cmd[k], &bi));
        VkClearValue clear = {.color = {{0, 0, 0, 1}}};
        VkRenderPassBeginInfo rpbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
            .framebuffer = fb, .renderArea = sc, .clearValueCount = 1, .pClearValues = &clear};
        vkCmdBeginRenderPass(cmd[k], &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd[k], VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        float t = f * 0.01f;
        vkCmdPushConstants(cmd[k], pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 4, &t);
        vkCmdDraw(cmd[k], tris * 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd[k]);
        if (readback || f == frames - 1) {
            VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                .imageExtent = {w, h, 1}};
            vkCmdCopyImageToBuffer(cmd[k], img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
        }
        CK(vkEndCommandBuffer(cmd[k]));
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd[k]};
        CK(vkQueueSubmit(q, 1, &si, fence[k]));
    }
    vkDeviceWaitIdle(dev);
    double t_run = now() - t1;
    uint64_t sum = 0;
    for (size_t i = 0; i < (size_t) w * h * 4; i += 4)
        sum += pixels[i] + 3 * pixels[i + 1] + 7 * pixels[i + 2];
    printf("%s: %dx%d, %d tris, %d frames%s: setup %.2f s, %.1f fps (%.2f ms/frame), checksum %llu\n",
           props.deviceName, w, h, tris, frames, readback ? " +readback" : "", t_setup,
           frames / t_run, t_run * 1000 / (frames ? frames : 1), (unsigned long long) sum);
    return 0;
}
