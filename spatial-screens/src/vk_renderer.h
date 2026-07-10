// Minimal Vulkan renderer for spatial-screens: one pipeline, no vertex
// buffers, push-constant-driven quads (see shaders/quad.vert for the layout).
// Lifecycle: create_instance -> [vk_surface sets phys+surface] -> init_device
// -> init_swapchain -> init_pipeline -> init_texture -> {upload,draw}* -> destroy.
#pragma once
#include <vulkan/vulkan.h>
#include <vector>

#include "source_slots.h"

struct QuadDraw {
    float mvp[16];   // column-major, Vulkan clip conventions (y-down, z 0..1)
    float color[4];
    float rect[4];   // cx, cy, half_w, half_h (quad-local units)
    float uv[4] = {0, 0, 1, 1};  // u0,v0,u1,v1 sub-rect of the shared texture
    bool textured;
    bool circle = false;  // clip to inscribed circle (status dot)
    int source_index = 0;  // index into VkRend::src[]; which texture this quad samples
};

// Per-source texture/staging/descriptor state (image + upload buffer + the
// dset that binds this slot's view for sampling). One per source slot.
struct RSource {
    VkImage tex = VK_NULL_HANDLE;
    VkDeviceMemory tex_mem = VK_NULL_HANDLE;
    VkImageView tex_view = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0, pitch = 0;  // pitch in bytes
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void* staging_ptr = nullptr;
    bool dirty = false;
    VkDescriptorSet dset = VK_NULL_HANDLE;
};

struct VkRend {
    VkInstance instance = VK_NULL_HANDLE;
    bool has_display_ext = false;  // instance carries the direct-display trio
    VkPhysicalDevice phys = VK_NULL_HANDLE;   // set by vk_surface backend
    VkSurfaceKHR surface = VK_NULL_HANDLE;    // set by vk_surface backend
    VkDevice device = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    VkQueue queue = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> fbs;
    std::vector<VkSemaphore> sem_render;  // one per swapchain image

    VkRenderPass pass = VK_NULL_HANDLE;
    VkDescriptorSetLayout dlayout = VK_NULL_HANDLE;
    VkPipelineLayout playout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;

    // index 0 = monitor · 1..kSourceSlots-1 = window · kLabelSource = label
    RSource src[kSourceSlots + 1];

    // Back-compat aliases for main.cpp's pre-Task-7 direct scalar access to
    // the monitor texture (slot 0) — true references into src[0], not copies,
    // so reads/writes through either name stay in sync (main.cpp writes
    // tex_dirty directly for the cursor-overlay path). Task 7 migrates
    // main.cpp onto vkr_set_source_size/vkr_upload_source; these aliases can
    // be deleted once no call site references them.
    uint32_t& tex_w = src[0].w;
    uint32_t& tex_h = src[0].h;
    uint32_t& tex_pitch = src[0].pitch;      // pitch in bytes
    void*& staging_ptr = src[0].staging_ptr;
    bool& tex_dirty = src[0].dirty;

    static const int FRAMES = 2;  // frames in flight
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd[FRAMES] = {};
    VkSemaphore sem_acquire[FRAMES] = {};
    VkFence fence[FRAMES] = {};
    int frame = 0;
    uint32_t cur_img = 0;      // image acquired by vkr_begin_frame, consumed by vkr_submit_stereo
    bool frame_open = false;   // a begin_frame awaits its submit (late-latch reprojection)
};

bool vkr_create_instance(VkRend& r, bool want_direct);
bool vkr_init_device(VkRend& r);
bool vkr_init_swapchain(VkRend& r);
bool vkr_init_pipeline(VkRend& r);
bool vkr_init_texture(VkRend& r, uint32_t w, uint32_t h, uint32_t pitch_bytes);
void vkr_destroy_texture(VkRend& r);
void vkr_upload(VkRend& r, const void* pixels, size_t bytes);
// Per-source twins: idx selects VkRend::src[idx] (0 = monitor, 1..kSourceSlots-1
// = window, kLabelSource = label). vkr_set_source_size lazily (re)creates the
// slot's image/staging only when dims differ or it isn't created yet.
// vkr_init_texture/vkr_upload are thin idx==0 wrappers over these.
void vkr_set_source_size(VkRend& r, int idx, uint32_t w, uint32_t h, uint32_t pitch);
void vkr_upload_source(VkRend& r, int idx, const void* pixels, size_t bytes);
// Wait for all in-flight frames before mutating the staging buffer — a
// prior frame's buffer->image copy may still be reading it (visible as a
// flickering cursor overlay otherwise).
void vkr_wait_uploads(VkRend& r);
bool vkr_draw(VkRend& r, const QuadDraw* draws, int n);
// SBS stereo: draw `left` into the left half viewport, `right` into the
// right half (extent.width/2 each). Same contract as vkr_draw otherwise.
// Caller MUST pass a non-null `right` even when nright == 0; a null right
// list silently downgrades to the single-viewport mono path.
bool vkr_draw_stereo(VkRend& r, const QuadDraw* left, int nleft,
                     const QuadDraw* right, int nright);

// Late-latch reprojection: split of the draw into acquire and submit so the
// caller can sample the head pose AFTER the (vblank-paced) acquire returns.
// vkr_begin_frame does the fence wait + vkAcquireNextImageKHR (blocks); on
// success frame_open is set and the caller MUST follow with exactly one
// vkr_submit_stereo (same frame index) — do not `continue`/bail between them
// or the acquired image wedges. vkr_draw/vkr_draw_stereo remain begin+submit
// wrappers for the non-reprojection path. right may be null (mono).
bool vkr_begin_frame(VkRend& r);
bool vkr_submit_stereo(VkRend& r, const QuadDraw* left, int nleft,
                       const QuadDraw* right, int nright);
void vkr_destroy(VkRend& r);

// Device-level teardown only (waits idle; destroys swapchain, pipeline,
// texture, device, surface — leaves the instance alive). For the direct
// backend: call this, then direct_release(), then vkr_destroy().
void vkr_destroy_device(VkRend& r);
