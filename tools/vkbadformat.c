// A guest must not be able to take the app down by naming a format the renderer
// has never heard of. Run in a guest: a child per format creates a render pass
// whose attachment format is missing from MoltenVK's table, then the parent
// creates a good one. MoltenVK used to build an error message from the missing
// entry's NULL name, which killed the whole emulator. Reaching "done" with the
// good format and the wait both 0 is the pass, with the emulator still up.
//
// A child may well die of SIGABRT, and that is fine. Venus creates render passes
// without waiting for an answer, so the driver hears of the failure only when
// the host declares that context fatal, and then its own "aborting on ring fatal
// error" ends the one process that did this.
//
//   VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/virtio_icd.aarch64.json ./vkbadformat
//
// Built on the Mac, needing only the Vulkan headers (it dlopens the loader):
//
//   zig cc -target aarch64-linux-musl -dynamic -O2 -I/opt/homebrew/include \
//       -o vkbadformat tools/vkbadformat.c
#include <dlfcn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "%s: %d\n", #x, r_); return 1; } } while (0)

static PFN_vkCreateRenderPass create_render_pass;
static PFN_vkDestroyRenderPass destroy_render_pass;
static PFN_vkDeviceWaitIdle wait_idle;

static VkResult try_format(VkDevice dev, VkFormat format) {
    VkAttachmentDescription att = {.format = format, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ref};
    VkRenderPassCreateInfo rpci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub};
    VkRenderPass rp = VK_NULL_HANDLE;
    VkResult r = create_render_pass(dev, &rpci, NULL, &rp);
    if (r == VK_SUCCESS)
        destroy_render_pass(dev, rp, NULL);
    return r;
}

static int open_device(VkDevice *dev) {
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr) dlsym(lib, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance create_instance = (PFN_vkCreateInstance) gipa(NULL, "vkCreateInstance");

    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    CK(create_instance(&ici, NULL, &inst));
    PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices) gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties get_props = (PFN_vkGetPhysicalDeviceProperties) gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_queues =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties) gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkCreateDevice create_device = (PFN_vkCreateDevice) gipa(inst, "vkCreateDevice");
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr) gipa(inst, "vkGetDeviceProcAddr");

    uint32_t n = 1;
    VkPhysicalDevice pd;
    VkResult er = enumerate(inst, &n, &pd);
    if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || n == 0) { fprintf(stderr, "no device\n"); return 1; }
    VkPhysicalDeviceProperties props;
    get_props(pd, &props);
    printf("device: %s\n", props.deviceName);

    uint32_t qfc = 16;
    VkQueueFamilyProperties qf[16];
    get_queues(pd, &qfc, qf);
    uint32_t qfi = 0;
    while (!(qf[qfi].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfi++;
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
    CK(create_device(pd, &dci, NULL, dev));
    create_render_pass = (PFN_vkCreateRenderPass) gdpa(*dev, "vkCreateRenderPass");
    destroy_render_pass = (PFN_vkDestroyRenderPass) gdpa(*dev, "vkDestroyRenderPass");
    wait_idle = (PFN_vkDeviceWaitIdle) gdpa(*dev, "vkDeviceWaitIdle");
    return 0;
}

int main(void) {

    // Past the table's linear range, near the enum's top, and negative.
    static const VkFormat bad[] = {(VkFormat) 1000999000, (VkFormat) 0x7ffffff0, (VkFormat) -5};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        pid_t pid = fork();
        if (pid == 0) {
            VkDevice dev;
            if (open_device(&dev) != 0)
                _exit(1);
            printf("format %d: %d\n", (int) bad[i], try_format(dev, bad[i]));
            fflush(stdout);
            // The host gets to an asynchronous create on its own time; this is
            // what makes it reach the bad one before the process exits.
            printf("format %d wait idle: %d\n", (int) bad[i], wait_idle(dev));
            fflush(stdout);
            _exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        printf("format %d child: %s %d\n", (int) bad[i], WIFSIGNALED(status) ? "signal" : "exit",
               WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status));
        fflush(stdout);
    }

    VkDevice dev;
    if (open_device(&dev) != 0)
        return 1;
    printf("good format: %d\n", try_format(dev, VK_FORMAT_R8G8B8A8_UNORM));
    printf("wait idle: %d\n", wait_idle(dev));
    printf("done\n");
    return 0;
}
