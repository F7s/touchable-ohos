// nx2android HarmonyOS NEXT: Vulkan presentation on the XComponent surface (G0b first frame).
// See gfx_surface_ohos.h for the contract. Self-contained: OHOS Vulkan + the XComponent window, no
// dependency on the game core, so the P0 host can show a first frame before dm/android/gfx_vk_* is
// linked. The render pass clears the swapchain image (a slowly changing colour, so a screenshot shows
// a live picture) and presents with FIFO.
#include "gfx_surface_ohos.h"

#define VK_USE_PLATFORM_OHOS 1
#include <vulkan/vulkan.h>

#include <native_window/external_window.h>
#include <android/native_window.h>

#include <hilog/log.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define GFX_DOMAIN 0xD2E1
#define GFX_TAG "nx2a-gfx"
#define LOGI(...) OH_LOG_Print(LOG_APP, LOG_INFO, GFX_DOMAIN, GFX_TAG, "%{public}s", log_buf(__VA_ARGS__))
#define LOGW(...) OH_LOG_Print(LOG_APP, LOG_WARN, GFX_DOMAIN, GFX_TAG, "%{public}s", log_buf(__VA_ARGS__))
#define LOGE(...) OH_LOG_Print(LOG_APP, LOG_ERROR, GFX_DOMAIN, GFX_TAG, "%{public}s", log_buf(__VA_ARGS__))

// One formatting buffer per call site would need macros with storage; a per-thread buffer is enough
// because every log line is printed immediately.
static const char *log_buf(const char *fmt, ...) {
    static __thread char b[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    return b;
}

enum { MAX_IMAGES = 8 };

typedef struct {
    VkInstance inst;
    VkSurfaceKHR surf;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue q;
    uint32_t qf;
    char devname[256];

    VkSwapchainKHR swap;
    VkFormat fmt;
    VkColorSpaceKHR cs;
    VkExtent2D ext;
    VkSurfaceTransformFlagBitsKHR transform;
    VkPresentModeKHR present_mode;
    uint32_t nimg;
    VkImage img[MAX_IMAGES];
    VkImageView view[MAX_IMAGES];
    VkFramebuffer fb[MAX_IMAGES];
    VkRenderPass rp;

    VkCommandPool cpool;
    VkCommandBuffer cb;
    VkSemaphore sem_acquire, sem_done;
    VkFence fence;
} gfx_t;

static gfx_t G;   // only the render thread touches this (set()/stop() only take the state lock)

// ---- renderer state, shared with the UI thread -------------------------------------------------
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static OHNativeWindow *g_win;      // held with a native-window reference

// G0b's own clear-colour presenter is a test fixture. When the game core is staged it must get the window through
// dm_front_window() instead, otherwise two swapchains race on one OHNativeWindow (the game's picture stays behind
// the clear colour). DM_OHOS_GFX_TEST=1 keeps the test presenter for G0b/G1 selftest runs.
static int test_presenter_wanted(void) {
    const char *e = getenv("DM_OHOS_GFX_TEST");
    return e && *e && strcmp(e, "0") != 0;
}
static int g_w, g_h;               // component size in pixels
static unsigned long g_gen;        // bumped on every set() so the render thread notices a resize
static pthread_t g_thread;
static int g_running;
static int g_stop;
static _Atomic int g_frames;       // presented frames

static const char *vkr(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        default: {
            static __thread char b[32];
            snprintf(b, sizeof b, "VkResult(%d)", (int)r);
            return b;
        }
    }
}

static int have_instance_ext(const char *name) {
    uint32_t n = 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &n, NULL) != VK_SUCCESS || !n) return 0;
    VkExtensionProperties *e = calloc(n, sizeof *e);
    if (!e) return 0;
    int found = 0;
    if (vkEnumerateInstanceExtensionProperties(NULL, &n, e) == VK_SUCCESS)
        for (uint32_t i = 0; i < n; i++)
            if (!strcmp(e[i].extensionName, name)) { found = 1; break; }
    free(e);
    return found;
}

static int have_device_ext(VkPhysicalDevice pd, const char *name) {
    uint32_t n = 0;
    if (vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) != VK_SUCCESS || !n) return 0;
    VkExtensionProperties *e = calloc(n, sizeof *e);
    if (!e) return 0;
    int found = 0;
    if (vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e) == VK_SUCCESS)
        for (uint32_t i = 0; i < n; i++)
            if (!strcmp(e[i].extensionName, name)) { found = 1; break; }
    free(e);
    return found;
}

// ---- device + surface ---------------------------------------------------------------------------
static int create_instance(gfx_t *g) {
    if (!have_instance_ext(VK_KHR_SURFACE_EXTENSION_NAME)) { LOGE("surface: VK_KHR_surface is not advertised"); return 0; }
    if (!have_instance_ext(VK_OHOS_SURFACE_EXTENSION_NAME)) { LOGE("surface: VK_OHOS_surface is not advertised"); return 0; }
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "nx2android";
    ai.applicationVersion = 1;
    ai.pEngineName = "nx2a-ohos";
    ai.engineVersion = 1;
    ai.apiVersion = VK_API_VERSION_1_1;
    const char *ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_OHOS_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = ext;
    VkResult r = vkCreateInstance(&ci, NULL, &g->inst);
    if (r != VK_SUCCESS) { LOGE("surface: vkCreateInstance: %s", vkr(r)); g->inst = VK_NULL_HANDLE; return 0; }
    LOGI("surface: instance with %s + %s", VK_KHR_SURFACE_EXTENSION_NAME, VK_OHOS_SURFACE_EXTENSION_NAME);
    return 1;
}

static int create_surface(gfx_t *g, OHNativeWindow *win) {
    VkSurfaceCreateInfoOHOS ci = {VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS};
    ci.window = win;
    VkResult r = vkCreateSurfaceOHOS(g->inst, &ci, NULL, &g->surf);
    if (r != VK_SUCCESS) { LOGE("surface: vkCreateSurfaceOHOS: %s", vkr(r)); g->surf = VK_NULL_HANDLE; return 0; }
    LOGI("surface: vkCreateSurfaceOHOS ok (window %p)", (void *)win);
    return 1;
}

static int pick_device(gfx_t *g) {
    uint32_t n = 0;
    if (vkEnumeratePhysicalDevices(g->inst, &n, NULL) != VK_SUCCESS || !n) { LOGE("surface: no Vulkan physical device"); return 0; }
    VkPhysicalDevice *pds = calloc(n, sizeof *pds);
    if (!pds) return 0;
    if (vkEnumeratePhysicalDevices(g->inst, &n, pds) != VK_SUCCESS) { free(pds); return 0; }
    for (uint32_t i = 0; i < n; i++) {
        if (!have_device_ext(pds[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, NULL);
        if (!nq) continue;
        VkQueueFamilyProperties *q = calloc(nq, sizeof *q);
        if (!q) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, q);
        int chosen = -1;
        for (uint32_t f = 0; f < nq && chosen < 0; f++) {
            if (!(q[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !q[f].queueCount) continue;
            VkBool32 present = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], f, g->surf, &present) == VK_SUCCESS && present) chosen = (int)f;
        }
        free(q);
        if (chosen < 0) continue;
        g->pd = pds[i];
        g->qf = (uint32_t)chosen;
        break;
    }
    free(pds);
    if (g->pd == VK_NULL_HANDLE) { LOGE("surface: no device with a graphics+present queue and VK_KHR_swapchain"); return 0; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g->pd, &props);
    snprintf(g->devname, sizeof g->devname, "%s", props.deviceName);
    LOGI("surface: device %s (Vulkan %u.%u.%u, queue family %u)",
         g->devname, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion), g->qf);
    return 1;
}

static int create_device(gfx_t *g) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g->qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char *ext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = 1;
    ci.ppEnabledExtensionNames = ext;
    VkResult r = vkCreateDevice(g->pd, &ci, NULL, &g->dev);
    if (r != VK_SUCCESS) { LOGE("surface: vkCreateDevice: %s", vkr(r)); g->dev = VK_NULL_HANDLE; return 0; }
    vkGetDeviceQueue(g->dev, g->qf, 0, &g->q);
    LOGI("surface: vkCreateDevice ok (%s)", VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    return 1;
}

static void destroy_swapchain(gfx_t *g) {
    if (!g->dev) return;
    vkDeviceWaitIdle(g->dev);
    for (uint32_t i = 0; i < g->nimg; i++) {
        if (g->fb[i]) { vkDestroyFramebuffer(g->dev, g->fb[i], NULL); g->fb[i] = VK_NULL_HANDLE; }
        if (g->view[i]) { vkDestroyImageView(g->dev, g->view[i], NULL); g->view[i] = VK_NULL_HANDLE; }
    }
    g->nimg = 0;
    if (g->rp) { vkDestroyRenderPass(g->dev, g->rp, NULL); g->rp = VK_NULL_HANDLE; }
    if (g->swap) { vkDestroySwapchainKHR(g->dev, g->swap, NULL); g->swap = VK_NULL_HANDLE; }
}

static void destroy_all(gfx_t *g) {
    if (g->dev) {
        destroy_swapchain(g);
        if (g->cpool) { vkDestroyCommandPool(g->dev, g->cpool, NULL); g->cpool = VK_NULL_HANDLE; }
        if (g->sem_acquire) { vkDestroySemaphore(g->dev, g->sem_acquire, NULL); g->sem_acquire = VK_NULL_HANDLE; }
        if (g->sem_done) { vkDestroySemaphore(g->dev, g->sem_done, NULL); g->sem_done = VK_NULL_HANDLE; }
        if (g->fence) { vkDestroyFence(g->dev, g->fence, NULL); g->fence = VK_NULL_HANDLE; }
        vkDestroyDevice(g->dev, NULL);
        g->dev = VK_NULL_HANDLE;
    }
    if (g->surf) { vkDestroySurfaceKHR(g->inst, g->surf, NULL); g->surf = VK_NULL_HANDLE; }
    if (g->inst) { vkDestroyInstance(g->inst, NULL); g->inst = VK_NULL_HANDLE; }
    memset(g, 0, sizeof *g);
}

static int create_swapchain(gfx_t *g, int want_w, int want_h) {
    VkSurfaceCapabilitiesKHR caps;
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g->pd, g->surf, &caps);
    if (r != VK_SUCCESS) { LOGE("swapchain: capabilities: %s", vkr(r)); return 0; }

    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g->pd, g->surf, &nf, NULL);
    if (!nf) { LOGE("swapchain: the surface offers no format"); return 0; }
    VkSurfaceFormatKHR f[32];
    if (nf > 32) nf = 32;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(g->pd, g->surf, &nf, f) != VK_SUCCESS) return 0;
    VkSurfaceFormatKHR sf = f[0];
    for (uint32_t i = 0; i < nf; i++)
        if (f[i].format == VK_FORMAT_R8G8B8A8_UNORM && f[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = f[i]; break; }
    if (sf.format != VK_FORMAT_R8G8B8A8_UNORM)
        for (uint32_t i = 0; i < nf; i++)
            if (f[i].format == VK_FORMAT_B8G8R8A8_UNORM && f[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = f[i]; break; }

    uint32_t npm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g->pd, g->surf, &npm, NULL);
    VkPresentModeKHR pm[8];
    if (npm > 8) npm = 8;
    VkPresentModeKHR want_pm = VK_PRESENT_MODE_FIFO_KHR;
    if (npm && vkGetPhysicalDeviceSurfacePresentModesKHR(g->pd, g->surf, &npm, pm) == VK_SUCCESS) {
        want_pm = pm[0];
        for (uint32_t i = 0; i < npm; i++) if (pm[i] == VK_PRESENT_MODE_FIFO_KHR) { want_pm = pm[i]; break; }
    }

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX || ext.height == UINT32_MAX) {
        uint32_t w = want_w > 0 ? (uint32_t)want_w : caps.minImageExtent.width;
        uint32_t h = want_h > 0 ? (uint32_t)want_h : caps.minImageExtent.height;
        if (w < caps.minImageExtent.width) w = caps.minImageExtent.width;
        if (w > caps.maxImageExtent.width) w = caps.maxImageExtent.width;
        if (h < caps.minImageExtent.height) h = caps.minImageExtent.height;
        if (h > caps.maxImageExtent.height) h = caps.maxImageExtent.height;
        ext.width = w;
        ext.height = h;
    }
    if (!ext.width || !ext.height) { LOGE("swapchain: zero extent"); return 0; }

    uint32_t n = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        for (uint32_t m = 1; m && m <= (uint32_t)VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; m <<= 1)
            if (caps.supportedCompositeAlpha & m) { alpha = (VkCompositeAlphaFlagBitsKHR)m; break; }
    }

    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g->surf;
    ci.minImageCount = n;
    ci.imageFormat = sf.format;
    ci.imageColorSpace = sf.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = want_pm;
    ci.clipped = VK_TRUE;
    r = vkCreateSwapchainKHR(g->dev, &ci, NULL, &g->swap);
    if (r != VK_SUCCESS) { LOGE("swapchain: vkCreateSwapchainKHR: %s", vkr(r)); g->swap = VK_NULL_HANDLE; return 0; }
    g->fmt = sf.format;
    g->cs = sf.colorSpace;
    g->ext = ext;
    g->transform = caps.currentTransform;
    g->present_mode = want_pm;

    uint32_t ni = 0;
    vkGetSwapchainImagesKHR(g->dev, g->swap, &ni, NULL);
    if (!ni) { LOGE("swapchain: no images"); destroy_swapchain(g); return 0; }
    if (ni > MAX_IMAGES) ni = MAX_IMAGES;
    g->nimg = ni;
    if (vkGetSwapchainImagesKHR(g->dev, g->swap, &ni, g->img) != VK_SUCCESS) { destroy_swapchain(g); return 0; }

    VkAttachmentDescription att = {0};
    att.format = g->fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {0};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;
    VkSubpassDependency dep = {0};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &att;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sp;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    r = vkCreateRenderPass(g->dev, &rpci, NULL, &g->rp);
    if (r != VK_SUCCESS) { LOGE("swapchain: vkCreateRenderPass: %s", vkr(r)); destroy_swapchain(g); return 0; }

    for (uint32_t i = 0; i < g->nimg; i++) {
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = g->img[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = g->fmt;
        vci.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        vci.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        vci.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        vci.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.baseMipLevel = 0;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.baseArrayLayer = 0;
        vci.subresourceRange.layerCount = 1;
        r = vkCreateImageView(g->dev, &vci, NULL, &g->view[i]);
        if (r != VK_SUCCESS) { LOGE("swapchain: vkCreateImageView %u: %s", i, vkr(r)); destroy_swapchain(g); return 0; }
        VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = g->rp;
        fci.attachmentCount = 1;
        fci.pAttachments = &g->view[i];
        fci.width = g->ext.width;
        fci.height = g->ext.height;
        fci.layers = 1;
        r = vkCreateFramebuffer(g->dev, &fci, NULL, &g->fb[i]);
        if (r != VK_SUCCESS) { LOGE("swapchain: vkCreateFramebuffer %u: %s", i, vkr(r)); destroy_swapchain(g); return 0; }
    }
    LOGI("swapchain: %ux%u format %d colorSpace %d, %u images, present mode %d, transform %#x",
         g->ext.width, g->ext.height, (int)g->fmt, (int)g->cs, g->nimg, (int)g->present_mode, (unsigned)g->transform);
    return 1;
}

static int create_device_objects(gfx_t *g) {
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g->qf;
    VkResult r = vkCreateCommandPool(g->dev, &pci, NULL, &g->cpool);
    if (r != VK_SUCCESS) { LOGE("device: vkCreateCommandPool: %s", vkr(r)); return 0; }
    VkCommandBufferAllocateInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = g->cpool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    r = vkAllocateCommandBuffers(g->dev, &cbi, &g->cb);
    if (r != VK_SUCCESS) { LOGE("device: vkAllocateCommandBuffers: %s", vkr(r)); return 0; }
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if ((r = vkCreateSemaphore(g->dev, &sci, NULL, &g->sem_acquire)) != VK_SUCCESS) { LOGE("device: semaphore: %s", vkr(r)); return 0; }
    if ((r = vkCreateSemaphore(g->dev, &sci, NULL, &g->sem_done)) != VK_SUCCESS) { LOGE("device: semaphore: %s", vkr(r)); return 0; }
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // the first vkWaitForFences must pass before the first submit
    if ((r = vkCreateFence(g->dev, &fci, NULL, &g->fence)) != VK_SUCCESS) { LOGE("device: fence: %s", vkr(r)); return 0; }
    return 1;
}

static int create_all(gfx_t *g, OHNativeWindow *win, int w, int h) {
    if (!create_instance(g)) return 0;
    if (!create_surface(g, win)) { destroy_all(g); return 0; }
    if (!pick_device(g)) { destroy_all(g); return 0; }
    if (!create_device(g)) { destroy_all(g); return 0; }
    if (!create_device_objects(g)) { destroy_all(g); return 0; }
    if (!create_swapchain(g, w, h)) { destroy_all(g); return 0; }
    LOGI("surface: presenter ready on %dx%d (%s)", w, h, g->devname);
    return 1;
}

// 2 = presented, 1 = retry, 0 = the swapchain must be made again, -1 = fatal
static int draw_frame(gfx_t *g, uint64_t frame_no) {
    VkResult r = vkWaitForFences(g->dev, 1, &g->fence, VK_TRUE, 1000000000ull);
    if (r == VK_TIMEOUT) return 1;
    if (r != VK_SUCCESS) { LOGE("present: vkWaitForFences: %s", vkr(r)); return -1; }
    uint32_t idx = 0;
    r = vkAcquireNextImageKHR(g->dev, g->swap, UINT64_MAX, g->sem_acquire, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) return 0;
    if (r == VK_SUBOPTIMAL_KHR) return 0;
    if (r != VK_SUCCESS) { LOGE("present: vkAcquireNextImageKHR: %s", vkr(r)); return -1; }
    if (idx >= g->nimg) return 0;
    if (vkResetFences(g->dev, 1, &g->fence) != VK_SUCCESS) return -1;
    if (vkResetCommandBuffer(g->cb, 0) != VK_SUCCESS) return -1;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(g->cb, &bi) != VK_SUCCESS) return -1;
    // A slowly changing colour: each screenshot proves a live picture, not a stale window.
    float t = (float)(frame_no % 300) / 300.0f;
    VkClearValue clear;
    memset(&clear, 0, sizeof clear);
    clear.color.float32[0] = 0.05f + 0.45f * t;
    clear.color.float32[1] = 0.55f - 0.40f * t;
    clear.color.float32[2] = 0.30f + 0.35f * (1.0f - t);
    clear.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo rbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = g->rp;
    rbi.framebuffer = g->fb[idx];
    rbi.renderArea.extent = g->ext;
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(g->cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(g->cb);
    if (vkEndCommandBuffer(g->cb) != VK_SUCCESS) return -1;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g->sem_acquire;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g->cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g->sem_done;
    if (vkQueueSubmit(g->q, 1, &si, g->fence) != VK_SUCCESS) return -1;
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g->sem_done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g->swap;
    pi.pImageIndices = &idx;
    r = vkQueuePresentKHR(g->q, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) return 0;
    if (r != VK_SUCCESS) { LOGE("present: vkQueuePresentKHR: %s", vkr(r)); return -1; }
    return 2;
}

static void *render_main(void *arg) {
    (void)arg;
    memset(&G, 0, sizeof G);
    OHNativeWindow *cur_win = NULL;
    int cur_w = 0, cur_h = 0;
    unsigned long seen_gen = 0;
    uint64_t frames = 0, last_frames = 0;
    struct timespec last_ts;
    clock_gettime(CLOCK_MONOTONIC, &last_ts);
    int init_fails = 0;
    LOGI("surface: renderer thread started");

    for (;;) {
        pthread_mutex_lock(&g_mu);
        while (!g_stop && !g_win) pthread_cond_wait(&g_cv, &g_mu);
        int stop = g_stop;
        OHNativeWindow *win = g_win;
        int w = g_w, h = g_h;
        unsigned long gen = g_gen;
        pthread_mutex_unlock(&g_mu);
        if (stop || !win) break;

        if (win != cur_win || w != cur_w || h != cur_h) {
            int same_win = win == cur_win && cur_win != NULL;
            if (!same_win) {
                destroy_all(&G);
                if (cur_win) { OH_NativeWindow_NativeObjectUnreference(cur_win); cur_win = NULL; }
                OH_NativeWindow_NativeObjectReference(win);   // the render thread's own reference
                cur_win = win;
            } else {
                destroy_swapchain(&G);
            }
            cur_w = w;
            cur_h = h;
            seen_gen = gen;
            int ok = same_win ? create_swapchain(&G, w, h) : create_all(&G, win, w, h);
            if (!ok) {
                if (++init_fails <= 3 || init_fails % 30 == 0) LOGE("surface: presenter init failed (attempt %d), retrying", init_fails);
                struct timespec ts = {0, 500 * 1000 * 1000};
                nanosleep(&ts, NULL);
                continue;
            }
            init_fails = 0;
        }
        if (!G.swap && !create_swapchain(&G, w, h)) {
            struct timespec ts = {0, 300 * 1000 * 1000};
            nanosleep(&ts, NULL);
            continue;
        }

        int rc = draw_frame(&G, frames);
        if (rc == 2) {
            frames++;
            atomic_store(&g_frames, (int)frames);
            if (frames == 1) LOGI("present: FIRST FRAME presented (%ux%u, format %d, %u images, present mode %d)",
                                  G.ext.width, G.ext.height, (int)G.fmt, G.nimg, (int)G.present_mode);
            if (frames % 60 == 0) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                double dt = (double)(now.tv_sec - last_ts.tv_sec) + (double)(now.tv_nsec - last_ts.tv_nsec) / 1e9;
                double fps = dt > 0 ? (double)(frames - last_frames) / dt : 0;
                LOGI("present: %llu frames, %.1f fps, %ux%u", (unsigned long long)frames, fps, G.ext.width, G.ext.height);
                last_frames = frames;
                last_ts = now;
            }
        } else if (rc == 0) {
            destroy_swapchain(&G);
        } else if (rc < 0) {
            destroy_swapchain(&G);
            struct timespec ts = {0, 200 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }

        pthread_mutex_lock(&g_mu);
        int now_stop = g_stop;
        unsigned long now_gen = g_gen;
        int nw = g_w, nh = g_h;
        pthread_mutex_unlock(&g_mu);
        if (now_stop) break;
        if (now_gen != seen_gen) {
            seen_gen = now_gen;
            if (nw != cur_w || nh != cur_h) {   // resize of the same window: only the swapchain changes
                destroy_swapchain(&G);
                cur_w = nw;
                cur_h = nh;
            }
        }
    }
    destroy_all(&G);
    if (cur_win) { OH_NativeWindow_NativeObjectUnreference(cur_win); cur_win = NULL; }
    LOGI("surface: renderer stopped after %llu frames", (unsigned long long)frames);
    return NULL;
}

// ---- public API --------------------------------------------------------------------------------
// The game's gfx_vk_present.c uses this weak symbol (android/front_android.c defines the Android one). A strong
// definition here means the OHOS host hands the XComponent window to the Vulkan game renderer.
ANativeWindow *dm_front_window(void) {
    pthread_mutex_lock(&g_mu);
    ANativeWindow *w = (ANativeWindow *)g_win;
    pthread_mutex_unlock(&g_mu);
    return w;
}

__attribute__((visibility("default"))) ANativeWindow *dm_ohos_front_window(void) { return dm_front_window(); }

void dm_gfx_surface_set(void *window, int width, int height) {
    OHNativeWindow *w = (OHNativeWindow *)window;
    int attach = w != NULL && width > 0 && height > 0;
    pthread_mutex_lock(&g_mu);
    OHNativeWindow *old = g_win;
    if (attach) OH_NativeWindow_NativeObjectReference(w);
    g_win = attach ? w : NULL;
    if (attach) { g_w = width; g_h = height; }
    g_gen++;
    if (attach && !test_presenter_wanted()) {
        pthread_mutex_unlock(&g_mu);
        if (old && old != g_win) OH_NativeWindow_NativeObjectUnreference(old);
        LOGI("surface: window handed to the game core (test presenter off)");
        return;
    }
    if (attach) {
        if (!g_running) {
            g_stop = 0;
            if (pthread_create(&g_thread, NULL, render_main, NULL) == 0) g_running = 1;
            else LOGE("surface: cannot create the render thread");
        }
        pthread_cond_broadcast(&g_cv);
    } else {
        g_stop = 1;
        pthread_cond_broadcast(&g_cv);
        if (g_running) {
            pthread_mutex_unlock(&g_mu);
            pthread_join(g_thread, NULL);
            pthread_mutex_lock(&g_mu);
            g_running = 0;
            atomic_store(&g_frames, 0);
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (old && old != g_win) OH_NativeWindow_NativeObjectUnreference(old);
    if (attach) LOGI("surface: attached %dx%d", width, height);
    else LOGI("surface: detached");
}

int dm_gfx_surface_frames(void) { return atomic_load(&g_frames); }

void dm_gfx_surface_stop(void) { dm_gfx_surface_set(NULL, 0, 0); }
