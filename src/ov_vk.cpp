/* ov_vk.cpp: the overlay's way into the port's frames (any port version).
 *
 * The port gets every Vulkan function through vkGetInstanceProcAddr of vulkan-1.dll (vulkan-hpp's
 * DynamicLoader: GetProcAddress). ov_hook_vulkan (DllMain, before the game's code runs) points the export
 * table entries of vkGetInstanceProcAddr / vkGetDeviceProcAddr at our versions; they hand out wrappers for
 * device creation, queues, swapchains and vkQueuePresentKHR and the real functions for everything else.
 *
 * vkQueuePresentKHR (the port calls it with its Scheduler::submit_mutex held, so our queue submits are safe):
 *   - nothing to draw and no thumbnail wanted: straight to the driver (the closed overlay costs a lookup);
 *   - thumbnail: the swapchain image is blitted to 2x the thumbnail size, copied to a host buffer and averaged 2x2 there (own submit + fence);
 *   - overlay: one render pass (load, present layout in and out) with ImGui's draw data; our submit waits on
 *     the port's semaphores and the present waits on ours.
 * Swapchains get COLOR_ATTACHMENT and TRANSFER_SRC added to their usage. Input: the game window's WndProc
 * is subclassed; while the panel is open its messages also go to ImGui (and on to SDL as before). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include "ov.h"
#include "ov_int.h"
#include "meta.h"

OvLogFn ov_log;
static void nolog(const char *, ...) {}

/* ---- real entry points ---- */
static PFN_vkGetInstanceProcAddr g_gipa;
static PFN_vkGetDeviceProcAddr g_gdpa;
static PFN_vkCreateInstance g_create_instance;
static PFN_vkCreateDevice g_create_device;
static PFN_vkQueuePresentKHR g_tramp_present;      /* loader trampoline (queues we did not see created) */
static VkInstance g_inst;
static uint32_t g_api = VK_API_VERSION_1_1;

#define DEV_FNS(X) X(QueuePresentKHR) X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR) X(GetDeviceQueue) \
    X(GetDeviceQueue2) X(DestroyDevice) X(CreateImageView) X(DestroyImageView) X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateRenderPass) \
    X(DestroyRenderPass) X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) \
    X(EndCommandBuffer) X(ResetCommandBuffer) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdPipelineBarrier) X(CmdBlitImage) \
    X(CmdCopyImageToBuffer) X(CmdCopyBufferToImage) X(QueueSubmit) X(CreateFence) X(DestroyFence) X(WaitForFences) X(ResetFences) \
    X(GetFenceStatus) X(CreateSemaphore) X(DestroySemaphore) X(CreateImage) X(DestroyImage) X(CreateBuffer) X(DestroyBuffer) \
    X(GetImageMemoryRequirements) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindImageMemory) X(BindBufferMemory) \
    X(MapMemory) X(UnmapMemory) X(DeviceWaitIdle) X(QueueWaitIdle)
struct Dev {
    VkDevice dev; VkPhysicalDevice phys;
#define F(n) PFN_vk##n n;
    DEV_FNS(F)
#undef F
};
struct QueueRec { VkQueue q; Dev *d; uint32_t family; int graphics; };
static Dev g_devs[8]; static int g_ndev;
static QueueRec g_queues[64]; static int g_nq;
static SRWLOCK g_reg_lock = SRWLOCK_INIT;
static PFN_vkGetPhysicalDeviceMemoryProperties g_mem_props;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties g_qf_props;
static PFN_vkGetPhysicalDeviceFormatProperties g_fmt_props;

static Dev *dev_find(VkDevice d) {
    AcquireSRWLockShared(&g_reg_lock);
    Dev *r = NULL; for (int i = 0; i < g_ndev; ++i) if (g_devs[i].dev == d) r = &g_devs[i];
    ReleaseSRWLockShared(&g_reg_lock);
    return r;
}
static QueueRec *queue_find(VkQueue q) {
    AcquireSRWLockShared(&g_reg_lock);
    QueueRec *r = NULL; for (int i = 0; i < g_nq; ++i) if (g_queues[i].q == q) r = &g_queues[i];
    ReleaseSRWLockShared(&g_reg_lock);
    return r;
}
static void queue_add(Dev *d, uint32_t family, VkQueue q) {
    if (!q || queue_find(q)) return;
    int gfx = 0;
    if (g_qf_props) {
        VkQueueFamilyProperties p[32]; uint32_t n = 32; g_qf_props(d->phys, &n, p);
        gfx = family < n && (p[family].queueFlags & VK_QUEUE_GRAPHICS_BIT);
    }
    AcquireSRWLockExclusive(&g_reg_lock);
    if (g_nq < 64) { g_queues[g_nq].q = q; g_queues[g_nq].d = d; g_queues[g_nq].family = family; g_queues[g_nq].graphics = gfx; ++g_nq; }
    ReleaseSRWLockExclusive(&g_reg_lock);
}

/* ---- swapchains and the overlay's per-image resources ---- */
#define MAX_IMG 16
struct Swap {
    VkSwapchainKHR sc; Dev *d; VkFormat fmt; VkExtent2D ext; VkImageUsageFlags usage; int srgb;
    uint32_t n; VkImage img[MAX_IMG]; VkImageView view[MAX_IMG]; VkFramebuffer fb[MAX_IMG];
    VkCommandBuffer cb[MAX_IMG]; VkFence fence[MAX_IMG]; VkSemaphore done[MAX_IMG]; int built;
};
static Swap g_swaps[8];
static CRITICAL_SECTION g_cs;   /* swap list, the draw path, textures */

struct Gfx {   /* one device draws the overlay: the one that presents */
    Dev *d; uint32_t family; VkQueue queue; VkCommandPool pool;
    VkRenderPass rp; VkFormat rp_fmt; int imgui; int pipeline_ok;
    /* thumbnail */
    VkImage timg; VkDeviceMemory tmem; VkFormat tfmt; VkBuffer tbuf; VkDeviceMemory tbmem; void *tmap;
    VkCommandBuffer tcb; VkFence tfence; VkSemaphore tsem;
};
static Gfx G;
static volatile LONG g_thumb_req, g_thumb_state;
/* full frame with the overlay (command "ovshot FILE": screenshots for the report) */
static volatile LONG g_shot_req; static char g_shot_path[MAX_PATH]; static HANDLE g_shot_ev;
static VkBuffer g_sbuf; static VkDeviceMemory g_smem; static void *g_smap; static VkDeviceSize g_ssize; static VkFence g_shot_fence;
static uint32_t g_shot_w, g_shot_h; static VkFormat g_shot_fmt;   /* state: 0 idle, 1 recorded+submitted, 2 failed */
static HANDLE g_thumb_ev;
static int g_ext_w, g_ext_h;
static volatile LONG64 g_presents;
static double g_fps; static LARGE_INTEGER g_qpf, g_fps_t0; static LONG64 g_fps_n0;

static int is_srgb(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}
static uint32_t mem_type(Dev *d, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties p; g_mem_props(d->phys, &p);
    for (uint32_t i = 0; i < p.memoryTypeCount; ++i) if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
static void swap_release(Swap *s) {
    if (!s->built) return;
    Dev *d = s->d;
    for (uint32_t i = 0; i < s->n; ++i) {
        if (s->fence[i]) d->WaitForFences(d->dev, 1, &s->fence[i], VK_TRUE, 2000000000ull);
        if (s->fb[i]) d->DestroyFramebuffer(d->dev, s->fb[i], NULL);
        if (s->view[i]) d->DestroyImageView(d->dev, s->view[i], NULL);
        if (s->fence[i]) d->DestroyFence(d->dev, s->fence[i], NULL);
        if (s->done[i]) d->DestroySemaphore(d->dev, s->done[i], NULL);
        if (s->cb[i] && G.pool) d->FreeCommandBuffers(d->dev, G.pool, 1, &s->cb[i]);
        s->fb[i] = 0; s->view[i] = 0; s->fence[i] = 0; s->done[i] = 0; s->cb[i] = 0;
    }
    s->built = 0;
}
static Swap *swap_find(VkSwapchainKHR sc) { for (auto &s : g_swaps) if (s.sc == sc && sc) return &s; return NULL; }

static int make_render_pass(Dev *d, VkFormat fmt) {
    if (G.rp && G.rp_fmt == fmt) return 1;
    if (G.rp) { d->QueueWaitIdle(G.queue); d->DestroyRenderPass(d->dev, G.rp, NULL); G.rp = 0; G.pipeline_ok = 0; }
    VkAttachmentDescription a = {};
    a.format = fmt; a.samples = VK_SAMPLE_COUNT_1_BIT; a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; a.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {}; sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS; sp.colorAttachmentCount = 1; sp.pColorAttachments = &ref;
    VkSubpassDependency dep = {}; dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo ci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1; ci.pAttachments = &a; ci.subpassCount = 1; ci.pSubpasses = &sp; ci.dependencyCount = 1; ci.pDependencies = &dep;
    if (d->CreateRenderPass(d->dev, &ci, NULL, &G.rp) != VK_SUCCESS) { G.rp = 0; return 0; }
    G.rp_fmt = fmt;
    return 1;
}
static int gfx_init(Dev *d, QueueRec *qr) {
    if (G.d == d && G.family == qr->family) { G.queue = qr->q; return 1; }
    if (G.d) return 0;   /* another device already draws (never seen: the port has one) */
    G.d = d; G.family = qr->family; G.queue = qr->q;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = qr->family;
    if (d->CreateCommandPool(d->dev, &pci, NULL, &G.pool) != VK_SUCCESS) { G.d = NULL; return 0; }
    ov_log("overlay: drawing on device %p, queue family %u", (void *)d->dev, qr->family);
    return 1;
}
static int swap_build(Swap *s) {
    if (s->built) return 1;
    Dev *d = s->d;
    uint32_t n = MAX_IMG;
    if (d->GetSwapchainImagesKHR(d->dev, s->sc, &n, s->img) < 0 || !n) return 0;
    s->n = n;
    if (!make_render_pass(d, s->fmt)) return 0;
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = s->img[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = s->fmt;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = G.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VkFenceCreateInfo fc = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkSemaphoreCreateInfo sc = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (d->CreateImageView(d->dev, &vi, NULL, &s->view[i]) != VK_SUCCESS) goto fail;
        fi.renderPass = G.rp; fi.attachmentCount = 1; fi.pAttachments = &s->view[i]; fi.width = s->ext.width; fi.height = s->ext.height; fi.layers = 1;
        if (d->CreateFramebuffer(d->dev, &fi, NULL, &s->fb[i]) != VK_SUCCESS) goto fail;
        if (d->AllocateCommandBuffers(d->dev, &ai, &s->cb[i]) != VK_SUCCESS) goto fail;
        if (d->CreateFence(d->dev, &fc, NULL, &s->fence[i]) != VK_SUCCESS) goto fail;
        if (d->CreateSemaphore(d->dev, &sc, NULL, &s->done[i]) != VK_SUCCESS) goto fail;
    }
    s->built = 1;
    ov_log("overlay: swapchain %p %ux%u format %d (%s), %u images, usage 0x%x", (void *)s->sc, s->ext.width, s->ext.height, (int)s->fmt,
           s->srgb ? "sRGB" : "UNORM", n, (unsigned)s->usage);
    return 1;
fail:
    s->built = 1; swap_release(s);
    ov_log("overlay: swapchain resources not created");
    return 0;
}

/* ---- textures ---- */
struct Tex {
    int w, h; VkFormat fmt; VkImage img; VkDeviceMemory mem; VkImageView view; VkDescriptorSet ds;
    VkBuffer stg; VkDeviceMemory smem; void *smap; VkFence last;   /* fence of the frame that last copied from stg */
    uint8_t *pending; int pw, ph; int dead;
};
static Tex g_tex[OVTEX_COUNT];
static SRWLOCK g_tex_lock = SRWLOCK_INIT;
void ovtex_set(int id, const uint8_t *rgba, int w, int h) {
    if (id < 0 || id >= OVTEX_COUNT) return;
    uint8_t *copy = rgba ? (uint8_t *)malloc((size_t)w * h * 4) : NULL;
    if (copy) memcpy(copy, rgba, (size_t)w * h * 4);
    AcquireSRWLockExclusive(&g_tex_lock);
    free(g_tex[id].pending); g_tex[id].pending = copy; g_tex[id].pw = w; g_tex[id].ph = h; g_tex[id].dead = !rgba;
    ReleaseSRWLockExclusive(&g_tex_lock);
}
ImTextureID ovtex_id(int id, int *w, int *h) {
    if (id < 0 || id >= OVTEX_COUNT || !g_tex[id].ds || g_tex[id].dead) return (ImTextureID)0;
    if (w) *w = g_tex[id].w; if (h) *h = g_tex[id].h;
    return (ImTextureID)g_tex[id].ds;
}
static void tex_destroy(Dev *d, Tex *t) {
    if (t->ds) ImGui_ImplVulkan_RemoveTexture(t->ds);
    if (t->view) d->DestroyImageView(d->dev, t->view, NULL);
    if (t->img) d->DestroyImage(d->dev, t->img, NULL);
    if (t->mem) d->FreeMemory(d->dev, t->mem, NULL);
    if (t->stg) d->DestroyBuffer(d->dev, t->stg, NULL);
    if (t->smem) d->FreeMemory(d->dev, t->smem, NULL);
    t->ds = 0; t->view = 0; t->img = 0; t->mem = 0; t->stg = 0; t->smem = 0; t->smap = NULL; t->w = t->h = 0;
}
static int tex_create(Dev *d, Tex *t, int w, int h, VkFormat fmt) {
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt; ii.extent = {(uint32_t)w, (uint32_t)h, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (d->CreateImage(d->dev, &ii, NULL, &t->img) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr; d->GetImageMemoryRequirements(d->dev, t->img, &mr);
    VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.allocationSize = mr.size; ma.memoryTypeIndex = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ma.memoryTypeIndex == UINT32_MAX || d->AllocateMemory(d->dev, &ma, NULL, &t->mem) != VK_SUCCESS || d->BindImageMemory(d->dev, t->img, t->mem, 0) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    if (d->CreateImageView(d->dev, &vi, NULL, &t->view) != VK_SUCCESS) return 0;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = (VkDeviceSize)w * h * 4; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (d->CreateBuffer(d->dev, &bi, NULL, &t->stg) != VK_SUCCESS) return 0;
    d->GetBufferMemoryRequirements(d->dev, t->stg, &mr);
    ma.allocationSize = mr.size; ma.memoryTypeIndex = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ma.memoryTypeIndex == UINT32_MAX || d->AllocateMemory(d->dev, &ma, NULL, &t->smem) != VK_SUCCESS || d->BindBufferMemory(d->dev, t->stg, t->smem, 0) != VK_SUCCESS) return 0;
    if (d->MapMemory(d->dev, t->smem, 0, VK_WHOLE_SIZE, 0, &t->smap) != VK_SUCCESS) return 0;
    t->ds = ImGui_ImplVulkan_AddTexture(t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t->w = w; t->h = h; t->fmt = fmt;
    return t->ds != 0;
}
static void barrier_img(Dev *d, VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
                        VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from; b.newLayout = to; b.srcAccessMask = sa; b.dstAccessMask = da;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
    d->CmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}
/* in the frame's command buffer, before the render pass */
static void tex_uploads(Dev *d, VkCommandBuffer cb, VkFence frame_fence, int srgb) {
    AcquireSRWLockExclusive(&g_tex_lock);
    for (int i = 0; i < OVTEX_COUNT; ++i) {
        Tex *t = &g_tex[i];
        if (!t->pending) continue;
        if (t->last && d->GetFenceStatus(d->dev, t->last) != VK_SUCCESS) continue;   /* staging still read by a frame in flight */
        VkFormat fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        int fresh = 0;
        if (t->w != t->pw || t->h != t->ph || t->fmt != fmt || !t->img) {
            if (t->img) { d->QueueWaitIdle(G.queue); tex_destroy(d, t); }
            if (!tex_create(d, t, t->pw, t->ph, fmt)) { tex_destroy(d, t); free(t->pending); t->pending = NULL; continue; }
            fresh = 1;
        }
        memcpy(t->smap, t->pending, (size_t)t->w * t->h * 4);
        free(t->pending); t->pending = NULL;
        barrier_img(d, cb, t->img, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy r = {}; r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; r.imageSubresource.layerCount = 1;
        r.imageExtent = {(uint32_t)t->w, (uint32_t)t->h, 1};
        d->CmdCopyBufferToImage(cb, t->stg, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        barrier_img(d, cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        t->last = frame_fence; t->dead = 0;
    }
    ReleaseSRWLockExclusive(&g_tex_lock);
}

/* ---- thumbnail ---- */
/* one linear blit from 1080p+ straight to the thumbnail skips pixels (blocky); 2x then a 2x2 average does not */
#define TW (META_THUMB_W * 2)
#define TH (META_THUMB_H * 2)
static int thumb_init(Dev *d, int srgb) {
    VkFormat fmt = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (G.timg && G.tfmt == fmt) return 1;
    if (G.timg) { d->QueueWaitIdle(G.queue); d->DestroyImage(d->dev, G.timg, NULL); d->FreeMemory(d->dev, G.tmem, NULL); G.timg = 0; G.tmem = 0; }
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt; ii.extent = {TW, TH, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (d->CreateImage(d->dev, &ii, NULL, &G.timg) != VK_SUCCESS) { G.timg = 0; return 0; }
    VkMemoryRequirements mr; d->GetImageMemoryRequirements(d->dev, G.timg, &mr);
    VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ma.allocationSize = mr.size; ma.memoryTypeIndex = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ma.memoryTypeIndex == UINT32_MAX || d->AllocateMemory(d->dev, &ma, NULL, &G.tmem) != VK_SUCCESS) return 0;
    d->BindImageMemory(d->dev, G.timg, G.tmem, 0);
    G.tfmt = fmt;
    if (!G.tbuf) {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = TW * TH * 4; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (d->CreateBuffer(d->dev, &bi, NULL, &G.tbuf) != VK_SUCCESS) return 0;
        d->GetBufferMemoryRequirements(d->dev, G.tbuf, &mr);
        ma.allocationSize = mr.size; ma.memoryTypeIndex = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ma.memoryTypeIndex == UINT32_MAX || d->AllocateMemory(d->dev, &ma, NULL, &G.tbmem) != VK_SUCCESS) return 0;
        d->BindBufferMemory(d->dev, G.tbuf, G.tbmem, 0);
        d->MapMemory(d->dev, G.tbmem, 0, VK_WHOLE_SIZE, 0, &G.tmap);
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool = G.pool; ai.commandBufferCount = 1;
        d->AllocateCommandBuffers(d->dev, &ai, &G.tcb);
        VkFenceCreateInfo fc = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        d->CreateFence(d->dev, &fc, NULL, &G.tfence);
        VkSemaphoreCreateInfo sc = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        d->CreateSemaphore(d->dev, &sc, NULL, &G.tsem);
    }
    return G.tmap && G.tcb && G.tfence && G.tsem;
}
/* records and submits the copy of image i; returns 1 when submitted (G.tsem signaled after the port's waits) */
static int thumb_submit(Dev *d, Swap *s, uint32_t i, const VkPresentInfoKHR *pi) {
    if (!(s->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !thumb_init(d, s->srgb)) return 0;
    d->WaitForFences(d->dev, 1, &G.tfence, VK_TRUE, 1000000000ull);
    d->ResetFences(d->dev, 1, &G.tfence);
    VkCommandBuffer cb = G.tcb;
    d->ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->BeginCommandBuffer(cb, &bi);
    barrier_img(d, cb, s->img[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier_img(d, cb, G.timg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    /* centre 16:9 crop */
    int32_t W = (int32_t)s->ext.width, H = (int32_t)s->ext.height, cw = W, ch = H;
    if ((int64_t)W * 9 > (int64_t)H * 16) cw = H * 16 / 9; else ch = W * 9 / 16;
    VkImageBlit r = {};
    r.srcSubresource.aspectMask = r.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; r.srcSubresource.layerCount = r.dstSubresource.layerCount = 1;
    r.srcOffsets[0] = {(W - cw) / 2, (H - ch) / 2, 0}; r.srcOffsets[1] = {(W - cw) / 2 + cw, (H - ch) / 2 + ch, 1};
    r.dstOffsets[1] = {TW, TH, 1};
    d->CmdBlitImage(cb, s->img[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, G.timg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_LINEAR);
    barrier_img(d, cb, G.timg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c = {}; c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1; c.imageExtent = {TW, TH, 1};
    d->CmdCopyImageToBuffer(cb, G.timg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, G.tbuf, 1, &c);
    barrier_img(d, cb, s->img[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    VkBufferMemoryBarrier hb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER}; hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; hb.buffer = G.tbuf; hb.size = VK_WHOLE_SIZE;
    d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &hb, 0, NULL);
    d->EndCommandBuffer(cb);
    VkPipelineStageFlags st[16]; for (int k = 0; k < 16; ++k) st[k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = pi->waitSemaphoreCount < 16 ? pi->waitSemaphoreCount : 16; si.pWaitSemaphores = pi->pWaitSemaphores; si.pWaitDstStageMask = st;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb; si.signalSemaphoreCount = 1; si.pSignalSemaphores = &G.tsem;
    return d->QueueSubmit(G.queue, 1, &si, G.tfence) == VK_SUCCESS;
}
int ov_thumbnail(uint8_t *bgr, int timeout_ms) {
    if (!G.d || !g_thumb_ev) return 0;
    ResetEvent(g_thumb_ev);
    InterlockedExchange(&g_thumb_state, 0);
    InterlockedExchange(&g_thumb_req, 1);
    if (WaitForSingleObject(g_thumb_ev, timeout_ms) != WAIT_OBJECT_0) { InterlockedExchange(&g_thumb_req, 0); return 0; }
    if (g_thumb_state != 1) return 0;
    Dev *d = G.d;
    if (d->WaitForFences(d->dev, 1, &G.tfence, VK_TRUE, 500000000ull) != VK_SUCCESS) return 0;
    const uint8_t *src = (const uint8_t *)G.tmap;
    long sum = 0;
    for (int y = 0; y < META_THUMB_H; ++y) {   /* RGBA top-down 2x -> BGR bottom-up, 2x2 average */
        const uint8_t *s = src + (size_t)(2 * y) * TW * 4, *s2 = s + TW * 4;
        uint8_t *o = bgr + (size_t)(META_THUMB_H - 1 - y) * META_THUMB_W * 3;
        for (int x = 0; x < META_THUMB_W; ++x, s += 8, s2 += 8)
            for (int c = 0; c < 3; ++c) { int v = (s[2 - c] + s[6 - c] + s2[2 - c] + s2[6 - c] + 2) >> 2; o[3 * x + c] = (uint8_t)v; sum += v; }
    }
    return sum > 0;
}

/* ---- input (game window subclass) ---- */
struct Ev { int type; int a, b; };   /* 1 mouse pos, 2 button, 3 wheel, 4 key, 5 char */
static Ev g_ev[256]; static int g_nev; static SRWLOCK g_ev_lock = SRWLOCK_INIT;
static HWND g_hwnd; static WNDPROC g_prev_proc;
static void ev_push(int type, int a, int b) {
    AcquireSRWLockExclusive(&g_ev_lock);
    if (g_nev < 256) { g_ev[g_nev].type = type; g_ev[g_nev].a = a; g_ev[g_nev].b = b; ++g_nev; }
    ReleaseSRWLockExclusive(&g_ev_lock);
}
static LRESULT CALLBACK ov_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ui_wants_input()) {
        switch (m) {
        case WM_MOUSEMOVE: ev_push(1, (short)LOWORD(l), (short)HIWORD(l)); break;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: ev_push(2, 0, 1); break;
        case WM_LBUTTONUP: ev_push(2, 0, 0); break;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: ev_push(2, 1, 1); break;
        case WM_RBUTTONUP: ev_push(2, 1, 0); break;
        case WM_MBUTTONDOWN: ev_push(2, 2, 1); break;
        case WM_MBUTTONUP: ev_push(2, 2, 0); break;
        case WM_MOUSEWHEEL: ev_push(3, GET_WHEEL_DELTA_WPARAM(w), 0); break;
        case WM_KEYDOWN: case WM_SYSKEYDOWN: {
            int vk = (int)w;
            ev_push(4, vk, 1);
            BYTE ks[256]; WCHAR ch[4];
            if (GetKeyboardState(ks)) {
                int n = ToUnicodeEx((UINT)vk, (UINT)((l >> 16) & 0xff), ks, ch, 4, 4, GetKeyboardLayout(0));
                for (int i = 0; i < n; ++i) if (ch[i] >= 32 && ch[i] != 127) ev_push(5, ch[i], 0);
            }
            break;
        }
        case WM_KEYUP: case WM_SYSKEYUP: ev_push(4, (int)w, 0); break;
        }
    }
    return CallWindowProcW(g_prev_proc, h, m, w, l);
}
static BOOL CALLBACK find_window(HWND h, LPARAM out) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    char cls[64]; GetClassNameA(h, cls, sizeof cls);
    if (!strcmp(cls, "SDL_app")) { *(HWND *)out = h; return FALSE; }
    return TRUE;
}
static void subclass_window(void) {
    if (g_hwnd) return;
    static DWORD last_try;
    if (GetTickCount() - last_try < 1000) return;
    last_try = GetTickCount();
    HWND h = NULL; EnumWindows(find_window, (LPARAM)&h);
    if (!h) return;
    g_prev_proc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)ov_wndproc);
    if (g_prev_proc) { g_hwnd = h; ov_log("overlay: input from window %p", (void *)h); }
}
HWND ov_game_window(void) { return g_hwnd; }
static ImGuiKey vk_key(int vk) {
    if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + vk - '0');
    if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + vk - 'A');
    if (vk >= VK_F1 && vk <= VK_F12) return (ImGuiKey)(ImGuiKey_F1 + vk - VK_F1);
    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab; case VK_LEFT: return ImGuiKey_LeftArrow; case VK_RIGHT: return ImGuiKey_RightArrow;
    case VK_UP: return ImGuiKey_UpArrow; case VK_DOWN: return ImGuiKey_DownArrow; case VK_PRIOR: return ImGuiKey_PageUp;
    case VK_NEXT: return ImGuiKey_PageDown; case VK_HOME: return ImGuiKey_Home; case VK_END: return ImGuiKey_End;
    case VK_INSERT: return ImGuiKey_Insert; case VK_DELETE: return ImGuiKey_Delete; case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space; case VK_RETURN: return ImGuiKey_Enter; case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_SHIFT: return ImGuiKey_LeftShift; case VK_CONTROL: return ImGuiKey_LeftCtrl; case VK_MENU: return ImGuiKey_LeftAlt;
    }
    return ImGuiKey_None;
}
static void apply_input(ImGuiIO &io, float sx, float sy) {
    Ev ev[256]; int n;
    AcquireSRWLockExclusive(&g_ev_lock);
    n = g_nev; memcpy(ev, g_ev, n * sizeof *ev); g_nev = 0;
    ReleaseSRWLockExclusive(&g_ev_lock);
    for (int i = 0; i < n; ++i) {
        const Ev &e = ev[i];
        if (e.type == 1) io.AddMousePosEvent(e.a * sx, e.b * sy);
        else if (e.type == 2) io.AddMouseButtonEvent(e.a, e.b != 0);
        else if (e.type == 3) io.AddMouseWheelEvent(0, e.a / 120.0f);
        else if (e.type == 4) {
            if (e.b) ui_on_key(e.a);
            ImGuiKey k = vk_key(e.a);
            if (e.a == VK_SHIFT) io.AddKeyEvent(ImGuiMod_Shift, e.b != 0);
            if (e.a == VK_CONTROL) io.AddKeyEvent(ImGuiMod_Ctrl, e.b != 0);
            if (e.a == VK_MENU) io.AddKeyEvent(ImGuiMod_Alt, e.b != 0);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, e.b != 0);
        } else if (e.type == 5) io.AddInputCharacterUTF16((ImWchar16)e.a);
    }
}

/* ---- ImGui ---- */
static PFN_vkVoidFunction imgui_loader(const char *name, void *) {
    PFN_vkVoidFunction f = G.d ? g_gdpa(G.d->dev, name) : NULL;
    return f ? f : g_gipa(g_inst, name);
}
static void check_vk(VkResult r) { if (r < 0) ov_log("overlay: Vulkan error %d in ImGui", (int)r); }
static int imgui_init(Swap *s) {
    if (!G.imgui) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = NULL; io.LogFilename = NULL;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        io.BackendPlatformName = "bbstate";
        ui_setup_fonts(io);
        if (!ImGui_ImplVulkan_LoadFunctions(g_api, imgui_loader)) { ov_log("overlay: ImGui could not load Vulkan functions"); return 0; }
        ImGui_ImplVulkan_InitInfo ii = {};
        ii.ApiVersion = g_api; ii.Instance = g_inst; ii.PhysicalDevice = G.d->phys; ii.Device = G.d->dev;
        ii.QueueFamily = G.family; ii.Queue = G.queue; ii.DescriptorPoolSize = 64;
        ii.MinImageCount = 2; ii.ImageCount = s->n < 2 ? 2 : s->n;
        ii.PipelineInfoMain.RenderPass = G.rp; ii.PipelineInfoMain.Subpass = 0; ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        ii.CheckVkResultFn = check_vk;
        if (!ImGui_ImplVulkan_Init(&ii)) { ov_log("overlay: ImGui_ImplVulkan_Init failed"); return 0; }
        G.imgui = 1; G.pipeline_ok = 1;
        QueryPerformanceFrequency(&g_qpf);
        ov_log("overlay: ImGui %s ready", IMGUI_VERSION);
    }
    if (!G.pipeline_ok) {   /* render pass made again (other swapchain format) */
        ImGui_ImplVulkan_PipelineInfo pi = {}; pi.RenderPass = G.rp; pi.Subpass = 0; pi.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        ImGui_ImplVulkan_CreateMainPipeline(&pi);
        G.pipeline_ok = 1;
    }
    return 1;
}
static float g_lin[256];
static void to_linear(ImDrawData *dd) {   /* sRGB swapchain: ImGui's colours are sRGB values */
    if (!g_lin[255]) for (int i = 0; i < 256; ++i) { float c = i / 255.0f; g_lin[i] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }
    for (int l = 0; l < dd->CmdListsCount; ++l) {
        ImDrawList *dl = dd->CmdLists[l];
        for (int v = 0; v < dl->VtxBuffer.Size; ++v) {
            ImU32 c = dl->VtxBuffer[v].col;
            ImU32 r = (ImU32)(g_lin[c & 0xff] * 255.0f + 0.5f), g = (ImU32)(g_lin[(c >> 8) & 0xff] * 255.0f + 0.5f), b = (ImU32)(g_lin[(c >> 16) & 0xff] * 255.0f + 0.5f);
            dl->VtxBuffer[v].col = (c & 0xff000000u) | (b << 16) | (g << 8) | r;
        }
    }
}
static LARGE_INTEGER g_last_frame;
/* cost of our own code per present (until the driver's vkQueuePresentKHR is called): closed / drawing */
static __declspec(thread) LONGLONG t_enter;
static volatile LONG64 g_st_n[2], g_st_t[2];
static VkResult present_real(PFN_vkQueuePresentKHR f, VkQueue q, const VkPresentInfoKHR *pi, int drawing) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    InterlockedIncrement64(&g_st_n[drawing]); InterlockedAdd64(&g_st_t[drawing], t.QuadPart - t_enter);
    return f(q, pi);
}
extern "C" void ov_stats(char *out, int n) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    double a = g_st_n[0] ? g_st_t[0] * 1e6 / f.QuadPart / g_st_n[0] : 0, b = g_st_n[1] ? g_st_t[1] * 1e6 / f.QuadPart / g_st_n[1] : 0;
    _snprintf_s(out, n, _TRUNCATE, "overlay cost per present: closed %.1f us (%lld presents), drawing %.1f us (%lld presents); presents/s now %.1f",
                a, g_st_n[0], b, g_st_n[1], g_fps);
    g_st_n[0] = g_st_n[1] = g_st_t[0] = g_st_t[1] = 0;
}
static VkResult draw_and_present(QueueRec *qr, VkQueue q, const VkPresentInfoKHR *pi, int thumb, int draw) {
    Dev *d = qr->d;
    Swap *s = swap_find(pi->pSwapchains[0]);
    if (!s || !gfx_init(d, qr) || !swap_build(s)) {
        if (thumb) { InterlockedExchange(&g_thumb_req, 0); InterlockedExchange(&g_thumb_state, 2); SetEvent(g_thumb_ev); }
        return present_real(d->QueuePresentKHR, q, pi, 0);
    }
    uint32_t i = pi->pImageIndices[0];
    if (i >= s->n) return present_real(d->QueuePresentKHR, q, pi, 0);
    int thumbed = 0;
    if (thumb) {
        InterlockedExchange(&g_thumb_req, 0);
        thumbed = thumb_submit(d, s, i, pi);
        InterlockedExchange(&g_thumb_state, thumbed ? 1 : 2);
        SetEvent(g_thumb_ev);
    }
    if (!draw) {
        if (!thumbed) return present_real(d->QueuePresentKHR, q, pi, 0);
        VkPresentInfoKHR p2 = *pi; p2.waitSemaphoreCount = 1; p2.pWaitSemaphores = &G.tsem;
        return present_real(d->QueuePresentKHR, q, &p2, 0);
    }
    if (!imgui_init(s)) { /* the overlay cannot draw: keep the game going */
        VkPresentInfoKHR p2 = *pi; if (thumbed) { p2.waitSemaphoreCount = 1; p2.pWaitSemaphores = &G.tsem; }
        return present_real(d->QueuePresentKHR, q, &p2, 0);
    }
    subclass_window();
    d->WaitForFences(d->dev, 1, &s->fence[i], VK_TRUE, 1000000000ull);
    d->ResetFences(d->dev, 1, &s->fence[i]);
    VkCommandBuffer cb = s->cb[i];
    d->ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->BeginCommandBuffer(cb, &bi);
    tex_uploads(d, cb, s->fence[i], s->srgb);

    /* the ImGui frame */
    ImGuiIO &io = ImGui::GetIO();
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    float dt = g_last_frame.QuadPart ? (float)((now.QuadPart - g_last_frame.QuadPart) / (double)g_qpf.QuadPart) : 1 / 60.0f;
    g_last_frame = now;
    io.DeltaTime = dt > 0 && dt < 1 ? dt : 1 / 60.0f;
    io.DisplaySize = ImVec2((float)s->ext.width, (float)s->ext.height);
    g_ext_w = (int)s->ext.width; g_ext_h = (int)s->ext.height;
    float sx = 1, sy = 1;
    RECT rc; if (g_hwnd && GetClientRect(g_hwnd, &rc) && rc.right > 0 && rc.bottom > 0) { sx = s->ext.width / (float)rc.right; sy = s->ext.height / (float)rc.bottom; }
    apply_input(io, sx, sy);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    ui_build((float)s->ext.width, (float)s->ext.height);
    ImGui::Render();
    ImDrawData *dd = ImGui::GetDrawData();
    if (s->srgb) to_linear(dd);
    VkRenderPassBeginInfo rb = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = G.rp; rb.framebuffer = s->fb[i]; rb.renderArea.extent = s->ext;
    d->CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(dd, cb);
    d->CmdEndRenderPass(cb);
    int shot = 0;
    if (g_shot_req && (s->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        VkDeviceSize need = (VkDeviceSize)s->ext.width * s->ext.height * 4;
        if (g_ssize < need) {
            if (g_sbuf) { d->QueueWaitIdle(G.queue); d->DestroyBuffer(d->dev, g_sbuf, NULL); d->FreeMemory(d->dev, g_smem, NULL); g_sbuf = 0; g_ssize = 0; }
            VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = need; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            VkMemoryRequirements mr; VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            if (d->CreateBuffer(d->dev, &bci, NULL, &g_sbuf) == VK_SUCCESS) {
                d->GetBufferMemoryRequirements(d->dev, g_sbuf, &mr); ma.allocationSize = mr.size;
                ma.memoryTypeIndex = mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                if (ma.memoryTypeIndex != UINT32_MAX && d->AllocateMemory(d->dev, &ma, NULL, &g_smem) == VK_SUCCESS) {
                    d->BindBufferMemory(d->dev, g_sbuf, g_smem, 0); d->MapMemory(d->dev, g_smem, 0, VK_WHOLE_SIZE, 0, &g_smap); g_ssize = need;
                }
            }
        }
        if (g_ssize >= need) {
            barrier_img(d, cb, s->img[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy c = {}; c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1; c.imageExtent = {s->ext.width, s->ext.height, 1};
            d->CmdCopyImageToBuffer(cb, s->img[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_sbuf, 1, &c);
            barrier_img(d, cb, s->img[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            g_shot_w = s->ext.width; g_shot_h = s->ext.height; g_shot_fmt = s->fmt; g_shot_fence = s->fence[i];
            shot = 1;
        }
    }
    d->EndCommandBuffer(cb);

    VkPipelineStageFlags st[16]; for (int k = 0; k < 16; ++k) st[k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (thumbed) { si.waitSemaphoreCount = 1; si.pWaitSemaphores = &G.tsem; }
    else { si.waitSemaphoreCount = pi->waitSemaphoreCount < 16 ? pi->waitSemaphoreCount : 16; si.pWaitSemaphores = pi->pWaitSemaphores; }
    si.pWaitDstStageMask = st; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &s->done[i];
    if (d->QueueSubmit(G.queue, 1, &si, s->fence[i]) != VK_SUCCESS) {
        ov_log("overlay: submit failed");
        VkPresentInfoKHR p2 = *pi; if (thumbed) { p2.waitSemaphoreCount = 1; p2.pWaitSemaphores = &G.tsem; }
        return present_real(d->QueuePresentKHR, q, &p2, 0);
    }
    if (shot) { InterlockedExchange(&g_shot_req, 0); SetEvent(g_shot_ev); }
    VkPresentInfoKHR p2 = *pi; p2.waitSemaphoreCount = 1; p2.pWaitSemaphores = &s->done[i];
    return present_real(d->QueuePresentKHR, q, &p2, 1);
}

/* ---- wrappers ---- */
static VkResult VKAPI_CALL hk_QueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi) {
    { LARGE_INTEGER t; QueryPerformanceCounter(&t); t_enter = t.QuadPart; }
    LONG64 n = InterlockedIncrement64(&g_presents);
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    if (!g_fps_t0.QuadPart) { g_fps_t0 = t; g_fps_n0 = n; }
    else if (t.QuadPart - g_fps_t0.QuadPart >= g_qpf.QuadPart) { g_fps = (n - g_fps_n0) * (double)g_qpf.QuadPart / (t.QuadPart - g_fps_t0.QuadPart); g_fps_t0 = t; g_fps_n0 = n; }
    QueueRec *qr = queue_find(q);
    if (!qr) return g_tramp_present(q, pi);
    int thumb = g_thumb_req != 0, draw = ui_wants_frame();
    if ((!thumb && !draw) || pi->swapchainCount != 1 || !qr->graphics) {
        if (thumb && (pi->swapchainCount != 1 || !qr->graphics)) { InterlockedExchange(&g_thumb_req, 0); InterlockedExchange(&g_thumb_state, 2); SetEvent(g_thumb_ev); }
        return present_real(qr->d->QueuePresentKHR, q, pi, 0);
    }
    EnterCriticalSection(&g_cs);
    VkResult r = draw_and_present(qr, q, pi, thumb, draw);
    LeaveCriticalSection(&g_cs);
    return r;
}
double ov_fps(void) { return g_fps; }
/* the next frame with the overlay drawn, as a 24-bit BMP (only while the overlay draws) */
extern "C" int ov_screenshot(const char *path) {
    if (!G.d || !g_shot_ev) return 0;
    ResetEvent(g_shot_ev);
    InterlockedExchange(&g_shot_req, 1);
    if (WaitForSingleObject(g_shot_ev, 3000) != WAIT_OBJECT_0) { InterlockedExchange(&g_shot_req, 0); return 0; }
    Dev *d = G.d;
    if (d->WaitForFences(d->dev, 1, &g_shot_fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) return 0;
    uint32_t w = g_shot_w, h = g_shot_h, row = (w * 3 + 3) & ~3u;
    int rgba = g_shot_fmt == VK_FORMAT_R8G8B8A8_UNORM || g_shot_fmt == VK_FORMAT_R8G8B8A8_SRGB || g_shot_fmt == VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    uint8_t *out = (uint8_t *)malloc((size_t)row * h);
    if (!out) return 0;
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *src = (const uint8_t *)g_smap + (size_t)y * w * 4; uint8_t *o = out + (size_t)(h - 1 - y) * row;
        for (uint32_t x = 0; x < w; ++x) { o[3 * x] = src[4 * x + (rgba ? 2 : 0)]; o[3 * x + 1] = src[4 * x + 1]; o[3 * x + 2] = src[4 * x + (rgba ? 0 : 2)]; }
    }
    BITMAPFILEHEADER fh = {0x4d42, (DWORD)(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + row * h), 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
    BITMAPINFOHEADER ih = {sizeof ih, (LONG)w, (LONG)h, 1, 24, BI_RGB, row * h, 2835, 2835, 0, 0};
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    int ok = f != INVALID_HANDLE_VALUE;
    if (ok) { DWORD wr; WriteFile(f, &fh, sizeof fh, &wr, NULL); WriteFile(f, &ih, sizeof ih, &wr, NULL); WriteFile(f, out, row * h, &wr, NULL); CloseHandle(f); }
    free(out);
    ov_log("overlay: screenshot %ux%u -> %s", w, h, path);
    return ok;
}
void ov_window_size(int *w, int *h) { *w = g_ext_w; *h = g_ext_h; }

static VkResult VKAPI_CALL hk_CreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *out) {
    Dev *d = dev_find(dev);
    if (!d) return VK_ERROR_INITIALIZATION_FAILED;
    VkSwapchainCreateInfoKHR c = *ci;
    c.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult r = d->CreateSwapchainKHR(dev, &c, a, out);
    if (r != VK_SUCCESS) { c.imageUsage = ci->imageUsage; r = d->CreateSwapchainKHR(dev, ci, a, out); }
    if (r == VK_SUCCESS) {
        EnterCriticalSection(&g_cs);
        Swap *s = NULL; for (auto &x : g_swaps) if (!x.sc) { s = &x; break; }
        if (s) { memset(s, 0, sizeof *s); s->sc = *out; s->d = d; s->fmt = c.imageFormat; s->ext = c.imageExtent; s->usage = c.imageUsage; s->srgb = is_srgb(c.imageFormat); }
        LeaveCriticalSection(&g_cs);
        ov_log("overlay: swapchain created %ux%u format %d colour space %d usage 0x%x", c.imageExtent.width, c.imageExtent.height, (int)c.imageFormat, (int)c.imageColorSpace, (unsigned)c.imageUsage);
    }
    return r;
}
static void VKAPI_CALL hk_DestroySwapchainKHR(VkDevice dev, VkSwapchainKHR sc, const VkAllocationCallbacks *a) {
    Dev *d = dev_find(dev);
    EnterCriticalSection(&g_cs);
    Swap *s = swap_find(sc);
    if (s) { swap_release(s); s->sc = 0; }
    LeaveCriticalSection(&g_cs);
    if (d) d->DestroySwapchainKHR(dev, sc, a);
}
static void VKAPI_CALL hk_GetDeviceQueue(VkDevice dev, uint32_t fam, uint32_t idx, VkQueue *q) {
    Dev *d = dev_find(dev);
    if (!d) return;
    d->GetDeviceQueue(dev, fam, idx, q);
    queue_add(d, fam, *q);
}
static void VKAPI_CALL hk_GetDeviceQueue2(VkDevice dev, const VkDeviceQueueInfo2 *qi, VkQueue *q) {
    Dev *d = dev_find(dev);
    if (!d || !d->GetDeviceQueue2) return;
    d->GetDeviceQueue2(dev, qi, q);
    queue_add(d, qi->queueFamilyIndex, *q);
}
static void VKAPI_CALL hk_DestroyDevice(VkDevice dev, const VkAllocationCallbacks *a) {
    Dev *d = dev_find(dev);
    if (!d) return;
    EnterCriticalSection(&g_cs);
    if (G.d == d) {
        d->DeviceWaitIdle(dev);
        for (auto &s : g_swaps) if (s.d == d) { swap_release(&s); s.sc = 0; }
        if (G.imgui) {
            for (auto &t : g_tex) tex_destroy(d, &t);
            ImGui_ImplVulkan_Shutdown(); ImGui::DestroyContext(); G.imgui = 0;
        }
        if (G.rp) d->DestroyRenderPass(dev, G.rp, NULL);
        if (G.timg) { d->DestroyImage(dev, G.timg, NULL); d->FreeMemory(dev, G.tmem, NULL); }
        if (G.tbuf) { d->DestroyBuffer(dev, G.tbuf, NULL); d->FreeMemory(dev, G.tbmem, NULL); }
        if (G.tfence) d->DestroyFence(dev, G.tfence, NULL);
        if (G.tsem) d->DestroySemaphore(dev, G.tsem, NULL);
        if (G.pool) d->DestroyCommandPool(dev, G.pool, NULL);
        memset(&G, 0, sizeof G);
    }
    LeaveCriticalSection(&g_cs);
    PFN_vkDestroyDevice destroy = d->DestroyDevice;
    AcquireSRWLockExclusive(&g_reg_lock);
    for (int i = 0; i < g_nq; ++i) if (g_queues[i].d == d) g_queues[i].q = NULL;
    d->dev = NULL;
    ReleaseSRWLockExclusive(&g_reg_lock);
    destroy(dev, a);
}
static PFN_vkVoidFunction VKAPI_CALL hk_GetDeviceProcAddr(VkDevice dev, const char *name);
static VkResult VKAPI_CALL hk_CreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    VkResult r = g_create_device(phys, ci, a, out);
    if (r != VK_SUCCESS) return r;
    AcquireSRWLockExclusive(&g_reg_lock);
    Dev *d = NULL; for (int i = 0; i < g_ndev; ++i) if (!g_devs[i].dev) d = &g_devs[i];
    if (!d && g_ndev < 8) d = &g_devs[g_ndev++];
    if (d) {
        memset(d, 0, sizeof *d); d->dev = *out; d->phys = phys;
#define F(n) d->n = (PFN_vk##n)g_gdpa(*out, "vk" #n);
        DEV_FNS(F)
#undef F
    }
    ReleaseSRWLockExclusive(&g_reg_lock);
    if (!g_mem_props && g_inst) {
        g_mem_props = (PFN_vkGetPhysicalDeviceMemoryProperties)g_gipa(g_inst, "vkGetPhysicalDeviceMemoryProperties");
        g_qf_props = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)g_gipa(g_inst, "vkGetPhysicalDeviceQueueFamilyProperties");
        g_fmt_props = (PFN_vkGetPhysicalDeviceFormatProperties)g_gipa(g_inst, "vkGetPhysicalDeviceFormatProperties");
    }
    ov_log("overlay: device %p created (%u queue families requested)", (void *)*out, ci->queueCreateInfoCount);
    return r;
}
static VkResult VKAPI_CALL hk_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    VkResult r = g_create_instance(ci, a, out);
    if (r == VK_SUCCESS) {
        g_inst = *out;
        if (ci->pApplicationInfo && ci->pApplicationInfo->apiVersion) g_api = ci->pApplicationInfo->apiVersion;
        ov_log("overlay: Vulkan instance %p, API %u.%u", (void *)g_inst, VK_API_VERSION_MAJOR(g_api), VK_API_VERSION_MINOR(g_api));
    }
    return r;
}
static PFN_vkVoidFunction wrap_device(const char *name, PFN_vkVoidFunction real) {
    if (!real) return NULL;
    if (!strcmp(name, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)hk_QueuePresentKHR;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)hk_CreateSwapchainKHR;
    if (!strcmp(name, "vkDestroySwapchainKHR")) return (PFN_vkVoidFunction)hk_DestroySwapchainKHR;
    if (!strcmp(name, "vkGetDeviceQueue")) return (PFN_vkVoidFunction)hk_GetDeviceQueue;
    if (!strcmp(name, "vkGetDeviceQueue2")) return (PFN_vkVoidFunction)hk_GetDeviceQueue2;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)hk_DestroyDevice;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)hk_GetDeviceProcAddr;
    return real;
}
static PFN_vkVoidFunction VKAPI_CALL hk_GetDeviceProcAddr(VkDevice dev, const char *name) {
    PFN_vkVoidFunction f = g_gdpa(dev, name);
    if (!name || !dev_find(dev)) return f;
    return wrap_device(name, f);
}
static PFN_vkVoidFunction VKAPI_CALL hk_GetInstanceProcAddr(VkInstance inst, const char *name) {
    PFN_vkVoidFunction f = g_gipa(inst, name);
    if (!f || !name) return f;
    if (inst && !g_inst) g_inst = inst;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)hk_GetInstanceProcAddr;
    if (!strcmp(name, "vkCreateInstance")) { g_create_instance = (PFN_vkCreateInstance)f; return (PFN_vkVoidFunction)hk_CreateInstance; }
    if (!strcmp(name, "vkCreateDevice")) { g_create_device = (PFN_vkCreateDevice)f; return (PFN_vkVoidFunction)hk_CreateDevice; }
    if (!strcmp(name, "vkQueuePresentKHR")) { g_tramp_present = (PFN_vkQueuePresentKHR)f; return (PFN_vkVoidFunction)hk_QueuePresentKHR; }
    /* device functions through the instance (loader trampolines): our wrappers find the device themselves */
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)hk_GetDeviceProcAddr;
    if (!strcmp(name, "vkCreateSwapchainKHR") || !strcmp(name, "vkDestroySwapchainKHR") || !strcmp(name, "vkGetDeviceQueue") ||
        !strcmp(name, "vkGetDeviceQueue2") || !strcmp(name, "vkDestroyDevice")) return wrap_device(name, f);
    return f;
}

/* ---- export table patch ---- */
static uint8_t *alloc_above(uint8_t *base, SIZE_T size) {
    MEMORY_BASIC_INFORMATION mbi;
    for (uint8_t *p = base + 0x10000; p < base + 0x7fff0000ull; p += 0x10000) {
        if (!VirtualQuery(p, &mbi, sizeof mbi)) break;
        if (mbi.State != MEM_FREE) { p = (uint8_t *)(((uintptr_t)mbi.BaseAddress + mbi.RegionSize + 0xffff) & ~(uintptr_t)0xffff) - 0x10000; continue; }
        void *r = VirtualAlloc(p, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (r) return (uint8_t *)r;
    }
    return NULL;
}
static int eat_patch(HMODULE m, const char *name, void *to, void **orig, uint8_t *stub) {
    uint8_t *base = (uint8_t *)m;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return 0;
    IMAGE_EXPORT_DIRECTORY *ex = (IMAGE_EXPORT_DIRECTORY *)(base + dir.VirtualAddress);
    DWORD *names = (DWORD *)(base + ex->AddressOfNames), *funcs = (DWORD *)(base + ex->AddressOfFunctions);
    WORD *ords = (WORD *)(base + ex->AddressOfNameOrdinals);
    for (DWORD i = 0; i < ex->NumberOfNames; ++i) {
        if (strcmp((const char *)base + names[i], name)) continue;
        DWORD *slot = &funcs[ords[i]];
        *orig = base + *slot;
        stub[0] = 0xff; stub[1] = 0x25; memset(stub + 2, 0, 4); memcpy(stub + 6, &to, 8);   /* jmp [rip+0] */
        DWORD old;
        if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return 0;
        *slot = (DWORD)(stub - base);
        VirtualProtect(slot, 4, old, &old);
        return 1;
    }
    return 0;
}
void ov_hook_vulkan(void *self, OvLogFn log) {
    (void)self;
    ov_log = log ? log : nolog;
    InitializeCriticalSection(&g_cs);
    g_thumb_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_shot_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    HMODULE vk = GetModuleHandleA("vulkan-1.dll");
    if (!vk) { ov_log("overlay: vulkan-1.dll not loaded: no overlay"); return; }
    uint8_t *stubs = alloc_above((uint8_t *)vk, 4096);
    if (!stubs) { ov_log("overlay: no memory near vulkan-1.dll: no overlay"); return; }
    int a = eat_patch(vk, "vkGetInstanceProcAddr", (void *)hk_GetInstanceProcAddr, (void **)&g_gipa, stubs);
    int b = eat_patch(vk, "vkGetDeviceProcAddr", (void *)hk_GetDeviceProcAddr, (void **)&g_gdpa, stubs + 16);
    FlushInstructionCache(GetCurrentProcess(), stubs, 4096);
    if (!a || !b) ov_log("overlay: vulkan-1.dll exports not patched (%d %d): no overlay", a, b);
    else ov_log("overlay: vulkan-1.dll exports patched (stubs %p)", stubs);
}
