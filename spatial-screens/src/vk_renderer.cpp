#include "vk_renderer.h"

#include <cstdio>
#include <cstring>

#include "../shaders/quad.vert.spv.h"
#include "../shaders/quad.frag.spv.h"

#define VK_CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "vulkan: %s failed (%d) at %s:%d\n", #x, _r, __FILE__, __LINE__); \
    return false; } } while (0)

// Matches the push_constant block in shaders/quad.{vert,frag}.
struct PushBlock {
    float mvp[16];
    float color[4];
    float rect[4];
    float flags[4];
};
static_assert(sizeof(PushBlock) == 112, "push block layout drifted from shaders");

bool vkr_create_instance(VkRend& r, bool want_direct) {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "spatial-screens";
    app.apiVersion = VK_API_VERSION_1_1;
    std::vector<const char*> exts = {"VK_KHR_surface", "VK_KHR_xlib_surface"};
    if (want_direct) {
        exts.push_back("VK_KHR_display");
        exts.push_back("VK_EXT_direct_mode_display");
        exts.push_back("VK_EXT_acquire_xlib_display");
    }
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    VkResult res = vkCreateInstance(&ci, nullptr, &r.instance);
    if (res == VK_ERROR_EXTENSION_NOT_PRESENT && want_direct) {
        fprintf(stderr, "vulkan: direct-display extensions unavailable\n");
        ci.enabledExtensionCount = 2;  // retry with just the window path
        res = vkCreateInstance(&ci, nullptr, &r.instance);
    } else if (res == VK_SUCCESS && want_direct) {
        r.has_display_ext = true;
    }
    if (res != VK_SUCCESS) {
        fprintf(stderr, "vulkan: vkCreateInstance failed (%d)\n", res);
        return false;
    }
    return true;
}

bool vkr_init_device(VkRend& r) {
    // One queue family with graphics + present-to-surface.
    uint32_t nfam = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(r.phys, &nfam, nullptr);
    std::vector<VkQueueFamilyProperties> fams(nfam);
    vkGetPhysicalDeviceQueueFamilyProperties(r.phys, &nfam, fams.data());
    bool found = false;
    for (uint32_t i = 0; i < nfam; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(r.phys, i, r.surface, &present);
        if ((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            r.qfam = i; found = true; break;
        }
    }
    if (!found) { fprintf(stderr, "vulkan: no graphics+present queue\n"); return false; }

    float prio = 1.f;
    VkDeviceQueueCreateInfo qi{};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = r.qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    const char* dev_exts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = dev_exts;
    VK_CHECK(vkCreateDevice(r.phys, &di, nullptr, &r.device));
    vkGetDeviceQueue(r.device, r.qfam, 0, &r.queue);

    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = r.qfam;
    VK_CHECK(vkCreateCommandPool(r.device, &pi, nullptr, &r.pool));
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = r.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = VkRend::FRAMES;
    VK_CHECK(vkAllocateCommandBuffers(r.device, &ai, r.cmd));
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < VkRend::FRAMES; i++) {
        VK_CHECK(vkCreateSemaphore(r.device, &si, nullptr, &r.sem_acquire[i]));
        VK_CHECK(vkCreateFence(r.device, &fi, nullptr, &r.fence[i]));
    }
    return true;
}

static void destroy_swapchain_objects(VkRend& r) {
    for (auto fb : r.fbs) vkDestroyFramebuffer(r.device, fb, nullptr);
    for (auto v : r.views) vkDestroyImageView(r.device, v, nullptr);
    for (auto s : r.sem_render) vkDestroySemaphore(r.device, s, nullptr);
    r.fbs.clear(); r.views.clear(); r.sem_render.clear(); r.images.clear();
    if (r.swapchain) vkDestroySwapchainKHR(r.device, r.swapchain, nullptr);
    r.swapchain = VK_NULL_HANDLE;
}

bool vkr_init_swapchain(VkRend& r) {
    vkDeviceWaitIdle(r.device);
    destroy_swapchain_objects(r);

    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(r.phys, r.surface, &caps));
    uint32_t nfmt = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(r.phys, r.surface, &nfmt, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nfmt);
    vkGetPhysicalDeviceSurfaceFormatsKHR(r.phys, r.surface, &nfmt, fmts.data());
    VkSurfaceFormatKHR pick = fmts[0];
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { pick = f; break; }
    r.format = pick.format;
    r.extent = caps.currentExtent;

    uint32_t count = caps.minImageCount > 2 ? caps.minImageCount : 2;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = r.surface;
    sci.minImageCount = count;
    sci.imageFormat = pick.format;
    sci.imageColorSpace = pick.colorSpace;
    sci.imageExtent = r.extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // tear-free vblank pacing
    sci.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(r.device, &sci, nullptr, &r.swapchain));

    if (!r.pass) {
        VkAttachmentDescription at{};
        at.format = r.format;
        at.samples = VK_SAMPLE_COUNT_1_BIT;
        at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        at.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rci{};
        rci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rci.attachmentCount = 1;
        rci.pAttachments = &at;
        rci.subpassCount = 1;
        rci.pSubpasses = &sub;
        rci.dependencyCount = 1;
        rci.pDependencies = &dep;
        VK_CHECK(vkCreateRenderPass(r.device, &rci, nullptr, &r.pass));
    }

    uint32_t nimg = 0;
    vkGetSwapchainImagesKHR(r.device, r.swapchain, &nimg, nullptr);
    r.images.resize(nimg);
    vkGetSwapchainImagesKHR(r.device, r.swapchain, &nimg, r.images.data());
    r.views.resize(nimg);
    r.fbs.resize(nimg);
    r.sem_render.resize(nimg);
    VkSemaphoreCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < nimg; i++) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = r.images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = r.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(r.device, &vi, nullptr, &r.views[i]));
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = r.pass;
        fci.attachmentCount = 1;
        fci.pAttachments = &r.views[i];
        fci.width = r.extent.width;
        fci.height = r.extent.height;
        fci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(r.device, &fci, nullptr, &r.fbs[i]));
        VK_CHECK(vkCreateSemaphore(r.device, &si, nullptr, &r.sem_render[i]));
    }
    printf("vulkan: swapchain %ux%u, %u images, FIFO\n",
           r.extent.width, r.extent.height, nimg);
    return true;
}

static VkShaderModule make_module(VkDevice dev, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}

bool vkr_init_pipeline(VkRend& r) {
    VkDescriptorSetLayoutBinding bind{};
    bind.binding = 0;
    bind.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bind.descriptorCount = 1;
    bind.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dli{};
    dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dli.bindingCount = 1;
    dli.pBindings = &bind;
    VK_CHECK(vkCreateDescriptorSetLayout(r.device, &dli, nullptr, &r.dlayout));

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(PushBlock)};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &r.dlayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VK_CHECK(vkCreatePipelineLayout(r.device, &pli, nullptr, &r.playout));

    VkShaderModule vs = make_module(r.device, quad_vert_spv, sizeof(quad_vert_spv));
    VkShaderModule fs = make_module(r.device, quad_frag_spv, sizeof(quad_frag_spv));
    if (!vs || !fs) { fprintf(stderr, "vulkan: shader module creation failed\n"); return false; }
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;

    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;  // projection Y-flip flips winding; skip culling
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo bl{};
    bl.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    bl.attachmentCount = 1;
    bl.pAttachments = &ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi{};
    dsi.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dsi.dynamicStateCount = 2;
    dsi.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gci{};
    gci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gci.stageCount = 2;
    gci.pStages = stages;
    gci.pVertexInputState = &vin;
    gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vp;
    gci.pRasterizationState = &rs;
    gci.pMultisampleState = &ms;
    gci.pColorBlendState = &bl;
    gci.pDynamicState = &dsi;
    gci.layout = r.playout;
    gci.renderPass = r.pass;
    VkResult pres = vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &gci, nullptr, &r.pipeline);
    vkDestroyShaderModule(r.device, vs, nullptr);
    vkDestroyShaderModule(r.device, fs, nullptr);
    if (pres != VK_SUCCESS) { fprintf(stderr, "vulkan: pipeline creation failed\n"); return false; }

    VkSamplerCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    smi.magFilter = VK_FILTER_LINEAR;
    smi.minFilter = VK_FILTER_LINEAR;
    smi.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smi.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(r.device, &smi, nullptr, &r.sampler));

    VkDescriptorPoolSize psz{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &psz;
    VK_CHECK(vkCreateDescriptorPool(r.device, &dpi, nullptr, &r.dpool));
    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = r.dpool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &r.dlayout;
    VK_CHECK(vkAllocateDescriptorSets(r.device, &dai, &r.dset));
    return true;
}

static uint32_t mem_type(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return 0;
}

bool vkr_init_texture(VkRend& r, uint32_t w, uint32_t h, uint32_t pitch_bytes) {
    r.tex_w = w; r.tex_h = h; r.tex_pitch = pitch_bytes;

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;  // XShm ZPixmap 32bpp is BGRX
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(r.device, &ii, nullptr, &r.tex));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(r.device, r.tex, &mr);
    VkMemoryAllocateInfo ma{};
    ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = mem_type(r.phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(r.device, &ma, nullptr, &r.tex_mem));
    VK_CHECK(vkBindImageMemory(r.device, r.tex, r.tex_mem, 0));
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = r.tex;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(r.device, &vi, nullptr, &r.tex_view));

    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = (VkDeviceSize)pitch_bytes * h;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VK_CHECK(vkCreateBuffer(r.device, &bi, nullptr, &r.staging));
    vkGetBufferMemoryRequirements(r.device, r.staging, &mr);
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = mem_type(r.phys, mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(r.device, &ma, nullptr, &r.staging_mem));
    VK_CHECK(vkBindBufferMemory(r.device, r.staging, r.staging_mem, 0));
    VK_CHECK(vkMapMemory(r.device, r.staging_mem, 0, VK_WHOLE_SIZE, 0, &r.staging_ptr));

    // One-shot transition UNDEFINED -> SHADER_READ_ONLY so the descriptor is
    // valid before the first capture lands.
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = r.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VK_CHECK(vkAllocateCommandBuffers(r.device, &cai, &cb));
    VkCommandBufferBeginInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbi);
    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bar.image = r.tex;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    vkEndCommandBuffer(cb);
    VkSubmitInfo sub{};
    sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cb;
    VkResult subres = vkQueueSubmit(r.queue, 1, &sub, VK_NULL_HANDLE);
    if (subres != VK_SUCCESS) {
        fprintf(stderr, "vulkan: submit failed (%d)\n", subres);
        vkFreeCommandBuffers(r.device, r.pool, 1, &cb);
        return false;
    }
    vkQueueWaitIdle(r.queue);
    vkFreeCommandBuffers(r.device, r.pool, 1, &cb);

    VkDescriptorImageInfo dii{r.sampler, r.tex_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = r.dset;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &dii;
    vkUpdateDescriptorSets(r.device, 1, &wr, 0, nullptr);
    r.tex_dirty = false;
    return true;
}

void vkr_destroy_texture(VkRend& r) {
    if (!r.device) return;
    vkDeviceWaitIdle(r.device);
    if (r.staging_ptr) vkUnmapMemory(r.device, r.staging_mem);
    if (r.staging) vkDestroyBuffer(r.device, r.staging, nullptr);
    if (r.staging_mem) vkFreeMemory(r.device, r.staging_mem, nullptr);
    if (r.tex_view) vkDestroyImageView(r.device, r.tex_view, nullptr);
    if (r.tex) vkDestroyImage(r.device, r.tex, nullptr);
    if (r.tex_mem) vkFreeMemory(r.device, r.tex_mem, nullptr);
    r.staging = VK_NULL_HANDLE; r.staging_mem = VK_NULL_HANDLE; r.staging_ptr = nullptr;
    r.tex = VK_NULL_HANDLE; r.tex_mem = VK_NULL_HANDLE; r.tex_view = VK_NULL_HANDLE;
}

void vkr_upload(VkRend& r, const void* pixels, size_t bytes) {
    size_t cap = (size_t)r.tex_pitch * r.tex_h;
    if (bytes > cap) bytes = cap;
    memcpy(r.staging_ptr, pixels, bytes);
    r.tex_dirty = true;
}

bool vkr_draw(VkRend& r, const QuadDraw* draws, int n) {
    if (!r.swapchain) return false;
    if (vkWaitForFences(r.device, 1, &r.fence[r.frame], VK_TRUE, 2000000000ull) != VK_SUCCESS)
        return false;  // wedged or lost device: let the caller bail to teardown
    uint32_t img = 0;
    VkResult res = vkAcquireNextImageKHR(r.device, r.swapchain, UINT64_MAX,
                                         r.sem_acquire[r.frame], VK_NULL_HANDLE, &img);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) { vkr_init_swapchain(r); return false; }
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "vulkan: acquire failed (%d)\n", res);
        return false;
    }
    vkResetFences(r.device, 1, &r.fence[r.frame]);

    VkCommandBuffer cb = r.cmd[r.frame];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbi);

    if (r.tex_dirty) {
        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.image = r.tex;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        VkBufferImageCopy cp{};
        cp.bufferRowLength = r.tex_pitch / 4;  // pitch in pixels (32bpp)
        cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.imageExtent = {r.tex_w, r.tex_h, 1};
        vkCmdCopyBufferToImage(cb, r.staging, r.tex,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
        r.tex_dirty = false;
    }

    VkClearValue clear{};  // true black: OLED pixels off = transparent glasses
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = r.pass;
    rbi.framebuffer = r.fbs[img];
    rbi.renderArea = {{0, 0}, r.extent};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vpt{0, 0, (float)r.extent.width, (float)r.extent.height, 0, 1};
    VkRect2D sc{{0, 0}, r.extent};
    vkCmdSetViewport(cb, 0, 1, &vpt);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r.playout,
                            0, 1, &r.dset, 0, nullptr);
    for (int i = 0; i < n; i++) {
        PushBlock pb{};
        memcpy(pb.mvp, draws[i].mvp, sizeof(pb.mvp));
        memcpy(pb.color, draws[i].color, sizeof(pb.color));
        memcpy(pb.rect, draws[i].rect, sizeof(pb.rect));
        pb.flags[0] = draws[i].textured ? 1.f : 0.f;
        pb.flags[1] = draws[i].circle ? 1.f : 0.f;
        vkCmdPushConstants(cb, r.playout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pb), &pb);
        vkCmdDraw(cb, 6, 1, 0, 0);
    }
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo sub{};
    sub.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sub.waitSemaphoreCount = 1;
    sub.pWaitSemaphores = &r.sem_acquire[r.frame];
    sub.pWaitDstStageMask = &wait_stage;
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cb;
    sub.signalSemaphoreCount = 1;
    sub.pSignalSemaphores = &r.sem_render[img];
    res = vkQueueSubmit(r.queue, 1, &sub, r.fence[r.frame]);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "vulkan: submit failed (%d)\n", res);
        return false;  // bounded fence wait above makes the next call safe
    }

    VkPresentInfoKHR pri{};
    pri.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pri.waitSemaphoreCount = 1;
    pri.pWaitSemaphores = &r.sem_render[img];
    pri.swapchainCount = 1;
    pri.pSwapchains = &r.swapchain;
    pri.pImageIndices = &img;
    res = vkQueuePresentKHR(r.queue, &pri);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
        vkr_init_swapchain(r);
    r.frame = (r.frame + 1) % VkRend::FRAMES;
    return true;
}

void vkr_destroy_device(VkRend& r) {
    if (r.device) {
        vkDeviceWaitIdle(r.device);
        vkr_destroy_texture(r);
        destroy_swapchain_objects(r);
        if (r.sampler) vkDestroySampler(r.device, r.sampler, nullptr);
        if (r.dpool) vkDestroyDescriptorPool(r.device, r.dpool, nullptr);
        if (r.pipeline) vkDestroyPipeline(r.device, r.pipeline, nullptr);
        if (r.playout) vkDestroyPipelineLayout(r.device, r.playout, nullptr);
        if (r.dlayout) vkDestroyDescriptorSetLayout(r.device, r.dlayout, nullptr);
        if (r.pass) vkDestroyRenderPass(r.device, r.pass, nullptr);
        for (int i = 0; i < VkRend::FRAMES; i++) {
            if (r.sem_acquire[i]) vkDestroySemaphore(r.device, r.sem_acquire[i], nullptr);
            if (r.fence[i]) vkDestroyFence(r.device, r.fence[i], nullptr);
        }
        if (r.pool) vkDestroyCommandPool(r.device, r.pool, nullptr);
        vkDestroyDevice(r.device, nullptr);
        r.device = VK_NULL_HANDLE;
    }
    if (r.surface) vkDestroySurfaceKHR(r.instance, r.surface, nullptr);
    r.surface = VK_NULL_HANDLE;
}

void vkr_destroy(VkRend& r) {
    vkr_destroy_device(r);
    // NOTE: instance destruction alone does NOT drop a direct-display lease
    // (Mesa keeps the lease fd for the process lifetime) — the direct
    // backend must call direct_release() before this.
    if (r.instance) vkDestroyInstance(r.instance, nullptr);
    r.instance = VK_NULL_HANDLE;
}
