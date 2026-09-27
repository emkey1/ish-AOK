// Cross-process dma-buf sharing through the Vulkan driver, for #484: what a
// compositor does with a client's buffer. The parent creates a LINEAR
// DRM-format-modifier image in exportable dma-buf memory, clears it to a known
// colour, and exports the memory as a dma-buf; the child (a separate Vulkan
// context) imports that fd into an image with the same explicit layout, copies
// it to host memory, and checks every pixel. Prints PASS or FAIL.
//
// Built on the Mac, needing only the Vulkan headers (it dlopens the loader):
//
//   zig cc -target aarch64-linux-gnu.2.17 -O2 -I/opt/homebrew/include \
//       -o vkdmabuf tools/vkdmabuf.c -ldl
//
// and run with VK_ICD_FILENAMES pointing at the driver under test.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define W 64
#define H 48
#define FMT VK_FORMAT_R8G8B8A8_UNORM

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfi;

#define CK(e) do { VkResult r_ = (e); if (r_ != VK_SUCCESS) { \
    printf("vkdmabuf: FAIL %s = %d (%s)\n", #e, r_, who); exit(1); } } while (0)
#define I(n) PFN_##n n = (PFN_##n) gipa(inst, #n)
#define D(n) PFN_##n n = (PFN_##n) gdpa(dev, #n)
static const char *who = "parent";

static void init(void) {
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("vkdmabuf: FAIL %s\n", dlerror()); exit(1); }
    gipa = (PFN_vkGetInstanceProcAddr) dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance) gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    CK(vkCreateInstance(&ici, NULL, &inst));
    I(vkEnumeratePhysicalDevices); I(vkCreateDevice); I(vkGetDeviceProcAddr);
    uint32_t n = 1;
    VkResult er = vkEnumeratePhysicalDevices(inst, &n, &pd);
    if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || n == 0) { printf("vkdmabuf: FAIL no device\n"); exit(1); }
    gdpa = vkGetDeviceProcAddr;
    qfi = 0;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio};
    const char *exts[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
                          "VK_EXT_image_drm_format_modifier", "VK_KHR_image_format_list"};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci, .enabledExtensionCount = 4, .ppEnabledExtensionNames = exts};
    CK(vkCreateDevice(pd, &dci, NULL, &dev));
    D(vkGetDeviceQueue);
    vkGetDeviceQueue(dev, qfi, 0, &queue);
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    I(vkGetPhysicalDeviceMemoryProperties);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    printf("vkdmabuf: FAIL no memory type in %#x (%s)\n", bits, who);
    exit(1);
}

// An image with modifier LINEAR in dma-buf memory, either exportable (fd < 0)
// or imported from fd with the given row pitch.
static VkImage make_image(int fd, VkDeviceSize pitch, VkDeviceMemory *mem_out, VkDeviceSize *pitch_out) {
    D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkAllocateMemory); D(vkBindImageMemory);
    D(vkGetImageSubresourceLayout); D(vkGetImageDrmFormatModifierPropertiesEXT);
    uint64_t linear = 0;   // DRM_FORMAT_MOD_LINEAR
    VkSubresourceLayout plane = {.offset = 0, .rowPitch = pitch};
    VkImageDrmFormatModifierListCreateInfoEXT list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .drmFormatModifierCount = 1, .pDrmFormatModifiers = &linear};
    VkImageDrmFormatModifierExplicitCreateInfoEXT expl = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .drmFormatModifier = linear, .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &plane};
    VkExternalMemoryImageCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = fd < 0 ? (void *) &list : (void *) &expl,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &ext,
        .imageType = VK_IMAGE_TYPE_2D, .format = FMT, .extent = {W, H, 1}, .mipLevels = 1,
        .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VkImage img;
    CK(vkCreateImage(dev, &ci, NULL, &img));
    VkImageDrmFormatModifierPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
    CK(vkGetImageDrmFormatModifierPropertiesEXT(dev, img, &mp));
    if (mp.drmFormatModifier != 0) { printf("vkdmabuf: FAIL modifier %#llx\n", (unsigned long long) mp.drmFormatModifier); exit(1); }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, img, &mr);
    VkExportMemoryAllocateInfo exp = {.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = fd};
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = fd < 0 ? (void *) &exp : (void *) &imp, .image = img};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &ded,
        .allocationSize = mr.size, .memoryTypeIndex = mem_type(mr.memoryTypeBits, 0)};
    CK(vkAllocateMemory(dev, &ai, NULL, mem_out));
    CK(vkBindImageMemory(dev, img, *mem_out, 0));
    VkImageSubresource sub = {.aspectMask = VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT};
    VkSubresourceLayout layout;
    vkGetImageSubresourceLayout(dev, img, &sub, &layout);
    if (pitch_out)
        *pitch_out = layout.rowPitch;
    return img;
}

static VkCommandBuffer begin(void) {
    D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi};
    VkCommandPool pool;
    CK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cb;
    CK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

static void submit(VkCommandBuffer cb) {
    D(vkEndCommandBuffer); D(vkQueueSubmit); D(vkQueueWaitIdle);
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CK(vkQueueWaitIdle(queue));
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    D(vkCmdPipelineBarrier);
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

int main(void) {
    init();
    D(vkCmdClearColorImage); D(vkGetMemoryFdKHR);
    VkDeviceMemory mem;
    VkDeviceSize pitch;
    VkImage img = make_image(-1, 0, &mem, &pitch);
    VkCommandBuffer cb = begin();
    barrier(cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue c = {.float32 = {1.0f, 0.5f, 0.25f, 1.0f}};
    VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
    barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    submit(cb);
    VkMemoryGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = mem,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int fd;
    CK(vkGetMemoryFdKHR(dev, &gi, &fd));
    off_t size = lseek(fd, 0, SEEK_END);
    printf("exported: rowPitch %llu, dma-buf %lld bytes\n", (unsigned long long) pitch, (long long) size);

    fflush(stdout);
    pid_t child = fork();
    if (child == 0) {
        who = "child";
        init();
        D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkAllocateMemory); D(vkBindBufferMemory);
        D(vkMapMemory); D(vkCmdCopyImageToBuffer);
        VkDeviceMemory imem;
        VkImage iimg = make_image(fd, pitch, &imem, NULL);
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = W * H * 4,
            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VkBuffer buf;
        CK(vkCreateBuffer(dev, &bci, NULL, &buf));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev, buf, &mr);
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
            .memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VkDeviceMemory bmem;
        CK(vkAllocateMemory(dev, &ai, NULL, &bmem));
        CK(vkBindBufferMemory(dev, buf, bmem, 0));
        VkCommandBuffer cb2 = begin();
        VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
        vkCmdCopyImageToBuffer(cb2, iimg, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &region);
        submit(cb2);
        unsigned char *p;
        CK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **) &p));
        int bad = 0;
        for (int i = 0; i < W * H; i++)
            if (p[i * 4] != 255 || abs(p[i * 4 + 1] - 128) > 1 || abs(p[i * 4 + 2] - 64) > 1 || p[i * 4 + 3] != 255)
                bad++;
        printf("child: imported %dx%d, first pixel %d,%d,%d,%d, %d wrong pixels\n",
               W, H, p[0], p[1], p[2], p[3], bad);
        fflush(stdout);
        _exit(bad != 0);
    }
    int status;
    waitpid(child, &status, 0);
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("vkdmabuf: %s\n", ok ? "PASS" : "FAIL");
    return !ok;
}
