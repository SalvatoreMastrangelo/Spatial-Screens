// Minimal Vulkan renderer for spatial-screens: one pipeline, no vertex
// buffers, push-constant-driven quads (see shaders/quad.vert for the layout).
// Lifecycle: create_instance -> [vk_surface sets phys+surface] -> init_device
// -> init_swapchain -> init_pipeline -> init_texture -> {upload,draw}* -> destroy.
#pragma once
#include <vulkan/vulkan.h>
#include <vector>

struct QuadDraw {
    float mvp[16];   // column-major, Vulkan clip conventions (y-down, z 0..1)
    float color[4];
    float rect[4];   // cx, cy, half_w, half_h (quad-local units)
    bool textured;
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
    VkDescriptorSet dset = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;

    VkImage tex = VK_NULL_HANDLE;
    VkDeviceMemory tex_mem = VK_NULL_HANDLE;
    VkImageView tex_view = VK_NULL_HANDLE;
    uint32_t tex_w = 0, tex_h = 0, tex_pitch = 0;  // pitch in bytes
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void* staging_ptr = nullptr;
    bool tex_dirty = false;

    static const int FRAMES = 2;  // frames in flight
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd[FRAMES] = {};
    VkSemaphore sem_acquire[FRAMES] = {};
    VkFence fence[FRAMES] = {};
    int frame = 0;
};

bool vkr_create_instance(VkRend& r, bool want_direct);
bool vkr_init_device(VkRend& r);
bool vkr_init_swapchain(VkRend& r);
bool vkr_init_pipeline(VkRend& r);
bool vkr_init_texture(VkRend& r, uint32_t w, uint32_t h, uint32_t pitch_bytes);
void vkr_destroy_texture(VkRend& r);
void vkr_upload(VkRend& r, const void* pixels, size_t bytes);
bool vkr_draw(VkRend& r, const QuadDraw* draws, int n);
void vkr_destroy(VkRend& r);

// Device-level teardown only (waits idle; destroys swapchain, pipeline,
// texture, device, surface — leaves the instance alive). For the direct
// backend: call this, then direct_release(), then vkr_destroy().
void vkr_destroy_device(VkRend& r);
