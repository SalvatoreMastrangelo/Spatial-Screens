# spatial-screens Direct Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the GLX windowed presentation in `spatial-screens/` with a Vulkan renderer that drives the glasses' display directly via a RandR lease (`VK_EXT_acquire_xlib_display` + `VK_KHR_display` swapchain), with a windowed Vulkan fallback.

**Architecture:** One Vulkan renderer (`vk_renderer`), two surface backends (`vk_surface`): direct display (default) and EWMH-fullscreen X11 window (fallback / `--window`). The app sets RandR `non-desktop=1` on the glasses output so Mutter releases it, Mesa performs the lease inside `vkAcquireXlibDisplayEXT`, and FIFO presents flip on the glasses' own 120 Hz vblank. All GLX/`SGI_video_sync` machinery is deleted. Spec: `docs/specs/2026-07-03-direct-mode-design.md`.

**Tech Stack:** C++17, raw Vulkan C API (`libvulkan`), Xlib/XRandR/XShm (existing), GLSL→SPIR-V via `glslangValidator` (generated headers checked in), GNU Make.

## Global Constraints

- Linux x86_64 only; never modify anything under `sdk/` (vendored, closed source).
- `./run.sh` is the only correct way to launch the SDK-linked binary (`LD_LIBRARY_PATH` for the SDK's bundled OpenCV). `make vk-test` does not link the SDK and runs directly.
- Stop `viture-bridge` before running anything SDK-linked — the SDK supports one client at a time.
- No C++ test rig in this repo (per CLAUDE.md): verification steps are `make` compiles plus manual on-hardware checkpoints written into Tasks 5–8. Do not invent a unit-test framework.
- C++ style: snake_case functions, `g_` prefix for atomic globals, 2-space… no — C++ files in this repo use 4-space indent (see `src/main.cpp`); match it.
- Target machine facts (verified): glasses = RandR output `DP-1`, 1920×1200@120, Intel iGPU (`modesetting`, Provider 0); Mesa 25.1.5 ANV; X.Org 21.1.4 RandR 1.6; WM is Mutter; laptop panel `eDP-1` must never be auto-picked (existing rule).
- Push-constant block layout (112 bytes) is shared by shaders and C++: `mat4 mvp` (0–63), `vec4 color` (64–79), `vec4 rect` = cx,cy,half_w,half_h (80–95), `vec4 flags` = textured,0,0,0 (96–111). Any change must touch `shaders/quad.vert`, `shaders/quad.frag`, and `QuadDraw`/`vkr_draw` together.
- Every commit message follows the repo's existing style: `spatial-screens: <what changed>` (see `git log`).

---

### Task 1: Toolchain install + ANV go/no-go gate

**Files:** none (system packages only).

**Interfaces:**
- Produces: a working `vulkaninfo`, `glslangValidator`, and `/usr/include/vulkan/vulkan.h` for every later task. **This task is the go/no-go gate from the spec — if the three extensions are missing, STOP and report; do not proceed to Task 2.**

- [ ] **Step 1: Install packages** (needs sudo — if running as an agent without sudo, ask the user to run it):

```bash
sudo apt install -y libvulkan-dev vulkan-tools glslang-tools
```

- [ ] **Step 2: Verify the Intel ANV device exposes the direct-display trio**

Run:
```bash
vulkaninfo 2>/dev/null | grep -E "GPU id|deviceName" | head -8
vulkaninfo 2>/dev/null | grep -cE "VK_EXT_acquire_xlib_display|VK_EXT_direct_mode_display" 
vulkaninfo 2>/dev/null | grep -c "VK_KHR_display"
```
Expected: an Intel device is listed (e.g. `deviceName = Intel(R) Arc(tm) Graphics`); the second command prints ≥ 2 (both instance extensions present); the third prints ≥ 1. If any extension is absent, STOP — the design's step-zero gate failed; report to the user instead of implementing.

- [ ] **Step 3: Verify the shader compiler**

Run: `glslangValidator --version`
Expected: version banner (any recent version).

---

### Task 2: Shaders + Makefile build scaffolding

**Files:**
- Create: `spatial-screens/shaders/quad.vert`
- Create: `spatial-screens/shaders/quad.frag`
- Create (generated, checked in): `spatial-screens/shaders/quad.vert.spv.h`, `spatial-screens/shaders/quad.frag.spv.h`
- Modify: `spatial-screens/Makefile` (full replacement shown below)

**Interfaces:**
- Produces: C headers defining `const uint32_t quad_vert_spv[]` / `quad_frag_spv[]` (+ matching `*_spv_len`-style sizing via `sizeof`), included by `vk_renderer.cpp` (Task 3) as `#include "../shaders/quad.vert.spv.h"`. Make targets `spatial-screens` (SDK-linked, from Task 7 on) and `vk-test` (Task 5).
- Build-state note: between this task and Task 4, the default `make` target cannot link (it lists `vk_renderer.o`/`vk_surface.o`, whose sources land in Tasks 3–4). That's expected — Tasks 2–4 each build only their named target. The full `spatial-screens` link works again from Task 4 on (old GLX `main.cpp` still compiles and links via the `LEGACY_GL` line, which Task 7 removes).

- [ ] **Step 1: Write `spatial-screens/shaders/quad.vert`**

```glsl
#version 450

// Push-constant layout shared with quad.frag and QuadDraw in vk_renderer.h.
layout(push_constant) uniform PC {
    mat4 mvp;     // column-major, Vulkan clip space (y-down, z in [0,1])
    vec4 color;
    vec4 rect;    // cx, cy, half_w, half_h in quad-local meters
    vec4 flags;   // x: 1 = sample texture, 0 = solid color
} pc;

layout(location = 0) out vec2 v_uv;

// Two CCW triangles from gl_VertexIndex — no vertex buffers anywhere.
void main() {
    const int idx[6] = int[6](0, 1, 2, 0, 2, 3);
    const vec2 c[4] = vec2[4](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 p = c[idx[gl_VertexIndex]];
    // Row 0 of the capture buffer is the top of the source screen; top of
    // the quad (p.y = +1) must sample v = 0.
    v_uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    vec2 local = pc.rect.xy + p * pc.rect.zw;
    gl_Position = pc.mvp * vec4(local, 0.0, 1.0);
}
```

- [ ] **Step 2: Write `spatial-screens/shaders/quad.frag`**

```glsl
#version 450

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 color;
    vec4 rect;
    vec4 flags;
} pc;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    o_color = pc.flags.x > 0.5 ? texture(u_tex, v_uv) * pc.color : pc.color;
}
```

- [ ] **Step 3: Replace `spatial-screens/Makefile` with:**

```make
SDK_DIR := ../sdk
SDK_LIB := $(SDK_DIR)/lib/x86_64

CXX ?= g++
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -I$(SDK_DIR)/include
# LEGACY_GL: removed in the main.cpp rewrite task (direct-mode plan Task 7).
LEGACY_GL := -lGL
VK_LIBS := -lvulkan -lX11 -lXext -lXrandr
LDFLAGS := -L$(SDK_LIB) -lglasses $(LEGACY_GL) $(VK_LIBS) -lpthread \
	-Wl,--disable-new-dtags,-rpath,'$$ORIGIN/$(SDK_LIB)' \
	-Wl,--allow-shlib-undefined

SHADER_HDRS := shaders/quad.vert.spv.h shaders/quad.frag.spv.h
OBJS := src/main.o src/vk_renderer.o src/vk_surface.o

spatial-screens: $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS)

src/%.o: src/%.cpp $(SHADER_HDRS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Generated SPIR-V headers are checked in so builds work without glslang.
shaders/%.vert.spv.h: shaders/%.vert
	glslangValidator -V --vn $(subst .,_,$(notdir $<))_spv -o $@ $<
shaders/%.frag.spv.h: shaders/%.frag
	glslangValidator -V --vn $(subst .,_,$(notdir $<))_spv -o $@ $<

vk-test: src/vk_test.o src/vk_renderer.o src/vk_surface.o
	$(CXX) $^ -o $@ $(VK_LIBS)

run: spatial-screens
	./run.sh
clean:
	rm -f spatial-screens vk-test src/*.o

.PHONY: run clean
```

Note: `src/vk_renderer.o`, `src/vk_surface.o`, `src/vk_test.o` don't exist until Tasks 3–5; that's fine — this task only verifies shader-header generation.

- [ ] **Step 4: Generate the SPIR-V headers and eyeball them**

Run:
```bash
cd spatial-screens && make shaders/quad.vert.spv.h shaders/quad.frag.spv.h && head -3 shaders/quad.vert.spv.h
```
Expected: glslangValidator prints the source filenames with no ERROR lines; the header starts with a comment + `const uint32_t quad_vert_spv[] = {` (exact variable name matters — Task 3 includes it).

- [ ] **Step 5: Commit**

```bash
git add spatial-screens/shaders spatial-screens/Makefile
git commit -m "spatial-screens: Vulkan shader pair + build scaffolding for direct mode"
```

---

### Task 3: `vk_renderer` — instance, device, swapchain, pipeline, frame loop

**Files:**
- Create: `spatial-screens/src/vk_renderer.h`
- Create: `spatial-screens/src/vk_renderer.cpp`

**Interfaces:**
- Consumes: `quad_vert_spv[]` / `quad_frag_spv[]` from Task 2's generated headers.
- Produces (used by Tasks 5 and 7 exactly as declared):
  - `struct QuadDraw { float mvp[16]; float color[4]; float rect[4]; bool textured; }`
  - `bool vkr_create_instance(VkRend&, bool want_direct)` — sets `r.has_display_ext`
  - `bool vkr_init_device(VkRend&)` — requires `r.phys` and `r.surface` already set by a `vk_surface` backend (Task 4)
  - `bool vkr_init_swapchain(VkRend&)` (idempotent; also used for recreate)
  - `bool vkr_init_pipeline(VkRend&)`
  - `bool vkr_init_texture(VkRend&, uint32_t w, uint32_t h, uint32_t pitch_bytes)`
  - `void vkr_destroy_texture(VkRend&)` (for capture-source resize, Task 7)
  - `void vkr_upload(VkRend&, const void* pixels, size_t bytes)`
  - `bool vkr_draw(VkRend&, const QuadDraw* draws, int n)` — returns false only when the swapchain was recreated and the frame skipped
  - `void vkr_destroy(VkRend&)`

No runtime verification is possible yet (no surface provider) — this task's gate is a clean compile of the object file. Runtime behavior is exercised in Task 5.

- [ ] **Step 1: Write `spatial-screens/src/vk_renderer.h`**

```cpp
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
```

- [ ] **Step 2: Write `spatial-screens/src/vk_renderer.cpp`**

```cpp
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
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "spatial-screens";
    app.apiVersion = VK_API_VERSION_1_1;
    std::vector<const char*> exts = {"VK_KHR_surface", "VK_KHR_xlib_surface"};
    if (want_direct) {
        exts.push_back("VK_KHR_display");
        exts.push_back("VK_EXT_direct_mode_display");
        exts.push_back("VK_EXT_acquire_xlib_display");
    }
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
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
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = r.qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    const char* dev_exts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = dev_exts;
    VK_CHECK(vkCreateDevice(r.phys, &di, nullptr, &r.device));
    vkGetDeviceQueue(r.device, r.qfam, 0, &r.queue);

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = r.qfam;
    VK_CHECK(vkCreateCommandPool(r.device, &pi, nullptr, &r.pool));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = r.pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = VkRend::FRAMES;
    VK_CHECK(vkAllocateCommandBuffers(r.device, &ai, r.cmd));
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
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
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
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
        VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
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
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < nimg; i++) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = r.images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = r.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(r.device, &vi, nullptr, &r.views[i]));
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
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
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
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
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 1;
    dli.pBindings = &bind;
    VK_CHECK(vkCreateDescriptorSetLayout(r.device, &dli, nullptr, &r.dlayout));

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(PushBlock)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
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

    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;  // projection Y-flip flips winding; skip culling
    rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo bl{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bl.attachmentCount = 1;
    bl.pAttachments = &ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dsi.dynamicStateCount = 2;
    dsi.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
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

    VkSamplerCreateInfo smi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    smi.magFilter = VK_FILTER_LINEAR;
    smi.minFilter = VK_FILTER_LINEAR;
    smi.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smi.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(r.device, &smi, nullptr, &r.sampler));

    VkDescriptorPoolSize psz{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &psz;
    VK_CHECK(vkCreateDescriptorPool(r.device, &dpi, nullptr, &r.dpool));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
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

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
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
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = mem_type(r.phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(r.device, &ma, nullptr, &r.tex_mem));
    VK_CHECK(vkBindImageMemory(r.device, r.tex, r.tex_mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = r.tex;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(r.device, &vi, nullptr, &r.tex_view));

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
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
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = r.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VK_CHECK(vkAllocateCommandBuffers(r.device, &cai, &cb));
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbi);
    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
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
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cb;
    vkQueueSubmit(r.queue, 1, &sub, VK_NULL_HANDLE);
    vkQueueWaitIdle(r.queue);
    vkFreeCommandBuffers(r.device, r.pool, 1, &cb);

    VkDescriptorImageInfo dii{r.sampler, r.tex_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
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
    vkWaitForFences(r.device, 1, &r.fence[r.frame], VK_TRUE, UINT64_MAX);
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
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbi);

    if (r.tex_dirty) {
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
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
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
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
        vkCmdPushConstants(cb, r.playout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pb), &pb);
        vkCmdDraw(cb, 6, 1, 0, 0);
    }
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sub.waitSemaphoreCount = 1;
    sub.pWaitSemaphores = &r.sem_acquire[r.frame];
    sub.pWaitDstStageMask = &wait_stage;
    sub.commandBufferCount = 1;
    sub.pCommandBuffers = &cb;
    sub.signalSemaphoreCount = 1;
    sub.pSignalSemaphores = &r.sem_render[img];
    vkQueueSubmit(r.queue, 1, &sub, r.fence[r.frame]);

    VkPresentInfoKHR pri{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
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

void vkr_destroy(VkRend& r) {
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
    // Destroying the instance also releases a directly-acquired display
    // (drops the RandR lease fd) — direct_restore() must run AFTER this.
    if (r.instance) vkDestroyInstance(r.instance, nullptr);
    r.instance = VK_NULL_HANDLE;
}
```

- [ ] **Step 3: Compile just the new object**

Run: `cd spatial-screens && make src/vk_renderer.o`
Expected: compiles with no warnings (`-Wall -Wextra`).

- [ ] **Step 4: Commit**

```bash
git add spatial-screens/src/vk_renderer.h spatial-screens/src/vk_renderer.cpp
git commit -m "spatial-screens: Vulkan renderer core (swapchain, pipeline, push-constant quads)"
```

---

### Task 4: `vk_surface` — direct-display acquisition, window fallback, restore guard

**Files:**
- Create: `spatial-screens/src/vk_surface.h`
- Create: `spatial-screens/src/vk_surface.cpp`

**Interfaces:**
- Consumes: a `VkInstance` created by `vkr_create_instance` with `has_display_ext == true` (for `direct_acquire`).
- Produces (used by Tasks 5 and 7 exactly as declared):
  - `struct OutputRect { std::string name; RROutput id; int x, y, w, h; }` — **moves here from `main.cpp`**, gaining the `id` field
  - `std::vector<OutputRect> list_outputs(Display* dpy)` — moves here from `main.cpp`
  - `struct SurfaceOut { VkSurfaceKHR surface; VkPhysicalDevice phys; uint32_t width, height; bool direct; Window window; }`
  - `bool direct_acquire(Display*, VkInstance, RROutput, SurfaceOut&)` — on failure it logs why, restores any RandR state it already changed, and returns false
  - `void direct_restore(Display*)` — RandR-only restore; call AFTER `vkr_destroy` (instance destruction drops the lease)
  - `bool window_create(Display*, VkInstance, int x, int y, int w, int h, SurfaceOut&)` — EWMH-fullscreen window + xlib surface + integrated-GPU device pick

- [ ] **Step 1: Write `spatial-screens/src/vk_surface.h`**

```cpp
// Surface backends for the spatial-screens Vulkan renderer.
//
// Direct (default): set RandR non-desktop=1 on the glasses output so Mutter
// releases it, then acquire it with VK_EXT_acquire_xlib_display (Mesa issues
// the RandR lease internally) and create a VK_KHR_display plane surface.
//
// Window (fallback): the EWMH-fullscreen + bypass-compositor window from the
// GLX era, presenting via VK_KHR_xlib_surface.
#pragma once
#include <vulkan/vulkan.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <string>
#include <vector>

struct OutputRect { std::string name; RROutput id = 0; int x = 0, y = 0, w = 0, h = 0; };

std::vector<OutputRect> list_outputs(Display* dpy);

struct SurfaceOut {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;
    bool direct = false;
    Window window = 0;  // window backend only
};

bool direct_acquire(Display* dpy, VkInstance inst, RROutput out_id, SurfaceOut& out);
void direct_restore(Display* dpy);
bool window_create(Display* dpy, VkInstance inst, int x, int y, int w, int h, SurfaceOut& out);
```

- [ ] **Step 2: Write `spatial-screens/src/vk_surface.cpp`**

```cpp
#include "vk_surface.h"

#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_XLIB_XRANDR_EXT
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xlib.h>
#include <vulkan/vulkan_xlib_xrandr.h>

#include <X11/Xatom.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>

std::vector<OutputRect> list_outputs(Display* dpy) {
    std::vector<OutputRect> out;
    Window root = DefaultRootWindow(dpy);
    XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
    for (int i = 0; i < res->noutput; i++) {
        XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (oi->connection == RR_Connected && oi->crtc) {
            XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
            out.push_back({ oi->name, res->outputs[i], ci->x, ci->y,
                            int(ci->width), int(ci->height) });
            XRRFreeCrtcInfo(ci);
        }
        XRRFreeOutputInfo(oi);
    }
    XRRFreeScreenResources(res);
    return out;
}

// ------------------------------------------------------------- direct ----

// Saved RandR state for restore; single-output app, so a file-static is fine.
static struct {
    bool prop_set = false;       // we changed non-desktop
    RROutput output = 0;
    RRCrtc crtc = 0;             // CRTC the output was on (0 = was off)
    RRMode mode = 0;
    int x = 0, y = 0;
    Rotation rot = RR_Rotate_0;
    std::vector<RROutput> crtc_outputs;
    bool was_primary = false;
} g_saved;

static void set_non_desktop(Display* dpy, RROutput out_id, long value) {
    Atom prop = XInternAtom(dpy, "non-desktop", True);
    if (prop == None) return;
    XRRChangeOutputProperty(dpy, out_id, prop, XA_INTEGER, 32,
                            PropModeReplace, (unsigned char*)&value, 1);
    XSync(dpy, False);
}

void direct_restore(Display* dpy) {
    if (!g_saved.prop_set) return;
    set_non_desktop(dpy, g_saved.output, 0);
    // Belt and suspenders: Mutter usually re-adopts the output on its own
    // once non-desktop drops; explicitly restore the old CRTC config too.
    if (g_saved.crtc && g_saved.mode) {
        Window root = DefaultRootWindow(dpy);
        XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
        XRRSetCrtcConfig(dpy, res, g_saved.crtc, CurrentTime,
                         g_saved.x, g_saved.y, g_saved.mode, g_saved.rot,
                         g_saved.crtc_outputs.data(), (int)g_saved.crtc_outputs.size());
        if (g_saved.was_primary) XRRSetOutputPrimary(dpy, root, g_saved.output);
        XRRFreeScreenResources(res);
    }
    XSync(dpy, False);
    g_saved.prop_set = false;
    printf("direct mode: output returned to the desktop\n");
}

bool direct_acquire(Display* dpy, VkInstance inst, RROutput out_id, SurfaceOut& out) {
    Window root = DefaultRootWindow(dpy);

    if (XInternAtom(dpy, "non-desktop", True) == None) {
        fprintf(stderr, "direct mode: server has no non-desktop property\n");
        return false;
    }

    // Save current config for restore.
    XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
    XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, out_id);
    g_saved.output = out_id;
    g_saved.crtc = oi->crtc;
    g_saved.was_primary = (XRRGetOutputPrimary(dpy, root) == out_id);
    if (oi->crtc) {
        XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
        g_saved.mode = ci->mode;
        g_saved.x = ci->x;
        g_saved.y = ci->y;
        g_saved.rot = ci->rotation;
        g_saved.crtc_outputs.assign(ci->outputs, ci->outputs + ci->noutput);
        XRRFreeCrtcInfo(ci);
    }
    XRRFreeOutputInfo(oi);

    // Hand the output to lease-land: Mutter sees non-desktop=1 and drops it.
    set_non_desktop(dpy, out_id, 1);
    g_saved.prop_set = true;
    bool crtc_off = false;
    for (int i = 0; i < 20 && !crtc_off; i++) {  // up to ~2 s
        usleep(100 * 1000);
        XRROutputInfo* poll = XRRGetOutputInfo(dpy, res, out_id);
        crtc_off = (poll->crtc == 0);
        XRRFreeOutputInfo(poll);
    }
    if (!crtc_off && g_saved.crtc) {
        // Mutter didn't release it; disable the CRTC ourselves.
        XRRSetCrtcConfig(dpy, res, g_saved.crtc, CurrentTime, 0, 0, None,
                         RR_Rotate_0, nullptr, 0);
        XSync(dpy, False);
    }
    XRRFreeScreenResources(res);

    auto p_get = (PFN_vkGetRandROutputDisplayEXT)
        vkGetInstanceProcAddr(inst, "vkGetRandROutputDisplayEXT");
    auto p_acq = (PFN_vkAcquireXlibDisplayEXT)
        vkGetInstanceProcAddr(inst, "vkAcquireXlibDisplayEXT");
    if (!p_get || !p_acq) {
        fprintf(stderr, "direct mode: acquire entry points missing\n");
        direct_restore(dpy);
        return false;
    }

    // The physical device that maps this RandR output to a display owns the
    // connector (Intel succeeds for DP-1; NVIDIA fails — natural selection).
    uint32_t npd = 0;
    vkEnumeratePhysicalDevices(inst, &npd, nullptr);
    std::vector<VkPhysicalDevice> pds(npd);
    vkEnumeratePhysicalDevices(inst, &npd, pds.data());
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDisplayKHR display = VK_NULL_HANDLE;
    for (auto pd : pds) {
        VkDisplayKHR d = VK_NULL_HANDLE;
        if (p_get(pd, dpy, out_id, &d) == VK_SUCCESS && d != VK_NULL_HANDLE) {
            phys = pd;
            display = d;
            break;
        }
    }
    if (!display) {
        fprintf(stderr, "direct mode: no Vulkan device owns this output\n");
        direct_restore(dpy);
        return false;
    }
    VkResult ar = p_acq(phys, dpy, display);
    if (ar != VK_SUCCESS) {
        fprintf(stderr, "direct mode: vkAcquireXlibDisplayEXT failed (%d)\n", ar);
        direct_restore(dpy);
        return false;
    }

    // Mode: native resolution at the highest refresh (spec: 1920x1200@120).
    uint32_t nmode = 0;
    vkGetDisplayModePropertiesKHR(phys, display, &nmode, nullptr);
    std::vector<VkDisplayModePropertiesKHR> modes(nmode);
    vkGetDisplayModePropertiesKHR(phys, display, &nmode, modes.data());
    if (!nmode) {
        fprintf(stderr, "direct mode: display exposes no modes\n");
        direct_restore(dpy);
        return false;
    }
    VkDisplayModePropertiesKHR best = modes[0];
    for (auto& m : modes) {
        uint32_t a = m.parameters.visibleRegion.width * m.parameters.visibleRegion.height;
        uint32_t b = best.parameters.visibleRegion.width * best.parameters.visibleRegion.height;
        if (a > b || (a == b && m.parameters.refreshRate > best.parameters.refreshRate))
            best = m;
    }

    // Plane: first one that supports this display and isn't tied to another.
    uint32_t nplane = 0;
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR(phys, &nplane, nullptr);
    std::vector<VkDisplayPlanePropertiesKHR> planes(nplane);
    vkGetPhysicalDeviceDisplayPlanePropertiesKHR(phys, &nplane, planes.data());
    uint32_t plane = UINT32_MAX;
    for (uint32_t i = 0; i < nplane; i++) {
        if (planes[i].currentDisplay != VK_NULL_HANDLE &&
            planes[i].currentDisplay != display) continue;
        uint32_t nsup = 0;
        vkGetDisplayPlaneSupportedDisplaysKHR(phys, i, &nsup, nullptr);
        std::vector<VkDisplayKHR> sup(nsup);
        vkGetDisplayPlaneSupportedDisplaysKHR(phys, i, &nsup, sup.data());
        for (auto d : sup)
            if (d == display) { plane = i; break; }
        if (plane != UINT32_MAX) break;
    }
    if (plane == UINT32_MAX) {
        fprintf(stderr, "direct mode: no display plane supports this output\n");
        direct_restore(dpy);
        return false;
    }
    VkDisplayPlaneCapabilitiesKHR pcaps;
    vkGetDisplayPlaneCapabilitiesKHR(phys, best.displayMode, plane, &pcaps);
    VkDisplayPlaneAlphaFlagBitsKHR alpha =
        (pcaps.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR)
            ? VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR
            : (VkDisplayPlaneAlphaFlagBitsKHR)(pcaps.supportedAlpha & -pcaps.supportedAlpha);

    VkDisplaySurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};
    sci.displayMode = best.displayMode;
    sci.planeIndex = plane;
    sci.planeStackIndex = planes[plane].currentStackIndex;
    sci.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sci.globalAlpha = 1.f;
    sci.alphaMode = alpha;
    sci.imageExtent = best.parameters.visibleRegion;
    VkResult sr = vkCreateDisplayPlaneSurfaceKHR(inst, &sci, nullptr, &out.surface);
    if (sr != VK_SUCCESS) {
        fprintf(stderr, "direct mode: display surface creation failed (%d)\n", sr);
        direct_restore(dpy);
        return false;
    }
    out.phys = phys;
    out.width = best.parameters.visibleRegion.width;
    out.height = best.parameters.visibleRegion.height;
    out.direct = true;
    printf("direct mode: %ux%u@%.0f Hz on plane %u\n", out.width, out.height,
           best.parameters.refreshRate / 1000.0, plane);
    return true;
}

// ------------------------------------------------------------- window ----

bool window_create(Display* dpy, VkInstance inst, int x, int y, int w, int h,
                   SurfaceOut& out) {
    Window root = DefaultRootWindow(dpy);
    XSetWindowAttributes swa{};
    swa.event_mask = KeyPressMask | StructureNotifyMask;
    Window win = XCreateWindow(dpy, root, x, y, w, h, 0, CopyFromParent,
                               InputOutput, CopyFromParent, CWEventMask, &swa);
    XStoreName(dpy, win, "spatial-screens");
    // Managed EWMH-fullscreen (not override-redirect): Mutter's unredirect
    // fast path only engages for proper fullscreen windows.
    Atom wm_state = XInternAtom(dpy, "_NET_WM_STATE", False);
    Atom wm_fs = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);
    XChangeProperty(dpy, win, wm_state, XA_ATOM, 32, PropModeReplace,
                    (unsigned char*)&wm_fs, 1);
    Atom bypass = XInternAtom(dpy, "_NET_WM_BYPASS_COMPOSITOR", False);
    long one = 1;
    XChangeProperty(dpy, win, bypass, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char*)&one, 1);
    XMapRaised(dpy, win);
    XMoveResizeWindow(dpy, win, x, y, w, h);
    XSync(dpy, False);

    VkXlibSurfaceCreateInfoKHR xci{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
    xci.dpy = dpy;
    xci.window = win;
    if (vkCreateXlibSurfaceKHR(inst, &xci, nullptr, &out.surface) != VK_SUCCESS) {
        fprintf(stderr, "vulkan: xlib surface creation failed\n");
        XDestroyWindow(dpy, win);
        return false;
    }

    // Prefer the integrated GPU that can present here (avoids a PRIME copy).
    uint32_t npd = 0;
    vkEnumeratePhysicalDevices(inst, &npd, nullptr);
    std::vector<VkPhysicalDevice> pds(npd);
    vkEnumeratePhysicalDevices(inst, &npd, pds.data());
    VkPhysicalDevice pick = VK_NULL_HANDLE;
    for (auto pd : pds) {
        uint32_t nfam = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nfam, nullptr);
        bool can_present = false;
        for (uint32_t i = 0; i < nfam && !can_present; i++) {
            VkBool32 s = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, out.surface, &s);
            can_present = s;
        }
        if (!can_present) continue;
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) { pick = pd; break; }
        if (!pick) pick = pd;
    }
    if (!pick) {
        fprintf(stderr, "vulkan: no device can present to the window\n");
        vkDestroySurfaceKHR(inst, out.surface, nullptr);
        out.surface = VK_NULL_HANDLE;
        XDestroyWindow(dpy, win);
        return false;
    }
    out.phys = pick;
    out.width = (uint32_t)w;
    out.height = (uint32_t)h;
    out.direct = false;
    out.window = win;
    printf("window fallback: %dx%d+%d+%d\n", w, h, x, y);
    return true;
}
```

- [ ] **Step 3: Compile the object**

Run: `cd spatial-screens && make src/vk_surface.o`
Expected: clean compile, no warnings.

- [ ] **Step 4: Commit**

```bash
git add spatial-screens/src/vk_surface.h spatial-screens/src/vk_surface.cpp
git commit -m "spatial-screens: direct-display acquisition + window fallback backends"
```

---

### Task 5: `vk-test` harness + window-backend runtime checkpoint

**Files:**
- Create: `spatial-screens/src/vk_test.cpp`

**Interfaces:**
- Consumes: everything Tasks 3–4 produced. Links WITHOUT the SDK (`make vk-test`) so it runs with no glasses IMU, no `viture-bridge` conflict, and no `run.sh`.
- Produces: the permanent debug tool for the presentation stack, kept in-tree.

- [ ] **Step 1: Write `spatial-screens/src/vk_test.cpp`**

```cpp
// vk-test — exercises the Vulkan presentation stack without the VITURE SDK.
//
//   ./vk-test [--direct] [--monitor NAME] [--seconds N]
//
// Renders an animated checkerboard quad + border. Default: EWMH-fullscreen
// window on the auto-detected glasses output (use --monitor eDP-1 to test on
// the laptop panel with no glasses). --direct takes the output away from the
// desktop via non-desktop=1 + VK_EXT_acquire_xlib_display and restores it on
// exit (Ctrl+C included).
#include "vk_renderer.h"
#include "vk_surface.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running = false; }

int main(int argc, char** argv) {
    bool direct = false;
    std::string monitor;
    double seconds = 8.0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--direct")) direct = true;
        else if (!strcmp(argv[i], "--monitor") && i + 1 < argc) monitor = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else { printf("usage: %s [--direct] [--monitor NAME] [--seconds N]\n", argv[0]); return 0; }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { fprintf(stderr, "cannot open X display\n"); return 1; }

    auto outputs = list_outputs(dpy);
    OutputRect target{};
    bool found = false;
    for (auto& o : outputs) {
        bool is_glasses_mode = (o.h == 1200 && (o.w == 1920 || o.w == 3840));
        bool is_laptop = o.name.rfind("eDP", 0) == 0 || o.name.rfind("LVDS", 0) == 0;
        if (!monitor.empty() ? o.name == monitor : (is_glasses_mode && !is_laptop)) {
            target = o; found = true; break;
        }
    }
    if (!found) { fprintf(stderr, "target output not found\n"); return 1; }
    printf("target: %s %dx%d+%d+%d\n", target.name.c_str(), target.w, target.h,
           target.x, target.y);

    VkRend vk{};
    if (!vkr_create_instance(vk, direct)) return 1;
    SurfaceOut sout{};
    bool ok = direct && vk.has_display_ext &&
              direct_acquire(dpy, vk.instance, target.id, sout);
    if (!ok) {
        if (direct) fprintf(stderr, "direct failed — window fallback\n");
        if (!window_create(dpy, vk.instance, target.x, target.y, target.w, target.h, sout))
            return 1;
    }
    vk.phys = sout.phys;
    vk.surface = sout.surface;
    if (!vkr_init_device(vk) || !vkr_init_swapchain(vk) || !vkr_init_pipeline(vk))
        return 1;

    const uint32_t TW = 512, TH = 512;
    if (!vkr_init_texture(vk, TW, TH, TW * 4)) return 1;
    std::vector<uint32_t> px(TW * TH);

    auto t0 = std::chrono::steady_clock::now();
    int frames = 0;
    while (g_running) {
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (t > seconds) break;
        int shift = int(t * 60) % 64;
        for (uint32_t y = 0; y < TH; y++)
            for (uint32_t x = 0; x < TW; x++)
                px[y * TW + x] = (((x + shift) / 32 + y / 32) & 1) ? 0xff2d3646 : 0xff59c2ff;
        vkr_upload(vk, px.data(), px.size() * 4);

        QuadDraw d[5] = {};
        float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        memcpy(d[0].mvp, ident, sizeof(ident));
        d[0].color[0] = d[0].color[1] = d[0].color[2] = d[0].color[3] = 1.f;
        d[0].rect[2] = 0.7f;  // half extents in clip space (MVP = identity)
        d[0].rect[3] = 0.7f;
        d[0].textured = true;
        for (int i = 1; i < 5; i++) {
            memcpy(d[i].mvp, ident, sizeof(ident));
            d[i].color[0] = 0.35f; d[i].color[1] = 0.76f; d[i].color[2] = 1.f;
            d[i].color[3] = 1.f;
            d[i].textured = false;
        }
        const float bt = 0.02f, hw = 0.7f;
        d[1].rect[1] = -hw; d[1].rect[2] = hw + bt; d[1].rect[3] = bt;  // top (y-down)
        d[2].rect[1] =  hw; d[2].rect[2] = hw + bt; d[2].rect[3] = bt;  // bottom
        d[3].rect[0] = -hw; d[3].rect[2] = bt; d[3].rect[3] = hw + bt;  // left
        d[4].rect[0] =  hw; d[4].rect[2] = bt; d[4].rect[3] = hw + bt;  // right
        if (vkr_draw(vk, d, 5)) frames++;

        while (XPending(dpy)) { XEvent ev; XNextEvent(dpy, &ev); }  // drain
    }
    double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("%d frames in %.1f s = %.1f fps\n", frames, el, frames / el);

    vkr_destroy(vk);
    if (sout.direct) direct_restore(dpy);
    else if (sout.window) XDestroyWindow(dpy, sout.window);
    XCloseDisplay(dpy);
    return 0;
}
```

- [ ] **Step 2: Build it**

Run: `cd spatial-screens && make vk-test`
Expected: links clean (no SDK libs involved).

- [ ] **Step 3: RUNTIME CHECKPOINT — window backend on the laptop panel (no glasses needed)**

Run: `cd spatial-screens && ./vk-test --monitor eDP-1 --seconds 5`
Expected: fullscreen animated blue/grey checkerboard with a light-blue border on the laptop panel for 5 s; console prints `window fallback: ...`, `vulkan: swapchain 2560x1600, N images, FIFO`, then a frame count at roughly the panel's refresh rate. No validation errors, no crash, desktop intact afterwards.

- [ ] **Step 4: Commit**

```bash
git add spatial-screens/src/vk_test.cpp
git commit -m "spatial-screens: vk-test harness (presentation stack without the SDK)"
```

---

### Task 6: RUNTIME CHECKPOINT — direct backend on the glasses

This is the spec's go/no-go moment for the ANV display path. Glasses must be plugged in. No code is written in this task unless the checkpoint fails; budget debugging time here.

**Files:** none (or fixes to `vk_surface.cpp`/`vk_renderer.cpp` if the checkpoint fails).

- [ ] **Step 1: Baseline — desktop shows DP-1**

Run: `xrandr | grep " connected" `
Expected: `DP-1 connected primary 1920x1200+...` (plus eDP-1).

- [ ] **Step 2: Direct-mode run**

Run: `cd spatial-screens && ./vk-test --direct --seconds 10`
Expected, in order:
1. `target: DP-1 1920x1200+...`
2. Within ~2 s the desktop reflows (DP-1 released; panels move to eDP-1).
3. `direct mode: 1920x1200@120 Hz on plane N`
4. `vulkan: swapchain 1920x1200, N images, FIFO`
5. The glasses show the checkerboard + border, full screen, no tearing.
6. Frame count ≈ 120 fps (FIFO-locked to the glasses' vblank — this number is the whole point of the project phase; record it).
7. On exit: `direct mode: output returned to the desktop`, and `xrandr` shows DP-1 connected/active again with its old position (primary restored).

- [ ] **Step 3: Interrupt path**

Run: `./vk-test --direct --seconds 60` then Ctrl+C after ~5 s.
Expected: same restore behavior — DP-1 back on the desktop.

- [ ] **Step 4: Fallback-and-restore path on the laptop panel**

Run: `./vk-test --direct --monitor eDP-1 --seconds 5`
Two outcomes are acceptable — Intel owns the eDP connector too, so direct
acquisition may legitimately succeed there:
- **Direct succeeds:** laptop panel leaves the desktop, shows the checkerboard,
  and MUST return to the desktop afterwards (bonus: proves the path on a
  second panel).
- **Direct fails** (Mutter declines to drop eDP, or acquire errors): the
  harness must print `direct failed — window fallback`, render in a window,
  and eDP-1 must still be a live desktop output afterwards — proving
  `direct_acquire` restored RandR state before returning false.

In BOTH cases the hard requirement is: after the run, `xrandr` shows eDP-1
connected and active. If it's left dark/off, the restore path in
`direct_acquire`/`direct_restore` is broken — fix before proceeding.

- [ ] **Step 5: Commit any fixes**

```bash
git add -A spatial-screens/src
git commit -m "spatial-screens: direct-mode checkpoint fixes"
```
(Skip the commit if no changes were needed.)

---

### Task 7: `main.cpp` rewrite — Vulkan integration, GLX deletion

**Files:**
- Modify: `spatial-screens/src/main.cpp` (surgical edits below; anchors are exact current content)
- Modify: `spatial-screens/Makefile` (drop `LEGACY_GL`)
- Modify: `spatial-screens/run.sh` (drop GL vsync env vars)

**Interfaces:**
- Consumes: every function listed in Task 3/Task 4 "Produces" blocks, exactly as declared there.
- Produces: the final `spatial-screens` binary. New flag `--window`; `--sgi-sync` deleted.

- [ ] **Step 1: Header comment + includes.** Replace the GLX/GL/keysym include block

```cpp
#include <GL/glx.h>
#include <GL/gl.h>
#include <X11/Xatom.h>
```
with
```cpp
#include <X11/Xatom.h>
```
and add, after the `viture_device_carina.h` include:
```cpp
#include "vk_renderer.h"
#include "vk_surface.h"
```
In the file-top comment, change the NOTE paragraph to mention: presentation is Vulkan direct display by default (RandR non-desktop + `VK_EXT_acquire_xlib_display`), `--window` forces the fullscreen-window fallback; delete the mention of `--sgi-sync` if present. Keep the "stop viture-bridge" note.

- [ ] **Step 2: Delete the SGI global.** Remove the line `static bool g_want_sgi = false;`

- [ ] **Step 3: Math additions.** After `mat_from_pose(...)` (ends `m[15] = 1; }`), add:

```cpp
// out = a * b (column-major 4x4)
static void mat_mul(const float* a, const float* b, float* out) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            out[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] +
                             a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
}

// Symmetric perspective for Vulkan clip space (y-down, z in [0,1]);
// rr/tt are frustum half-extents at the near plane, as for glFrustum.
static void mat_projection_vk(float rr, float tt, float n, float f, float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = n / rr;
    m[5] = -n / tt;
    m[10] = f / (n - f);
    m[11] = -1.f;
    m[14] = n * f / (n - f);
}
```

- [ ] **Step 4: Delete the local RandR helpers.** Remove the whole `// X11/RandR` section (`struct OutputRect ... }` through the end of `list_outputs`) — both now come from `vk_surface.h`. Keep the `#include <X11/extensions/Xrandr.h>` (still used for change notifications).

- [ ] **Step 5: Flags.** In the arg loop, replace
```cpp
        else if (!strcmp(argv[i], "--sgi-sync")) g_want_sgi = true;
```
with
```cpp
        else if (!strcmp(argv[i], "--window")) force_window = true;
```
add `bool force_window = false;` next to the other option defaults, and update the usage `printf` to list `[--window]` instead of the removed flag.

- [ ] **Step 6: Replace the presentation setup.** Delete everything from the comment `// -- GLX window on the glasses output` through the `printf("vblank sync: ...")` line — EXCEPT keep the global-hotkeys block (`// Global hotkeys (Ctrl+Alt+key) ...` through its closing brace) — and insert in its place (hotkeys block stays after it):

```cpp
    // -- Vulkan: direct display (default) or EWMH-fullscreen window fallback
    VkRend vk{};
    if (!vkr_create_instance(vk, !force_window)) return 1;
    SurfaceOut sout{};
    bool direct_ok = !force_window && vk.has_display_ext &&
                     direct_acquire(dpy, vk.instance, glasses.id, sout);
    if (!direct_ok) {
        if (!force_window) fprintf(stderr, "direct mode unavailable — window fallback\n");
        // direct_acquire restores the desktop on failure, but the layout may
        // have shifted meanwhile — re-resolve the glasses rect first.
        outputs = list_outputs(dpy);
        for (auto& o : outputs) if (o.name == glasses.name) { glasses = o; break; }
        if (!window_create(dpy, vk.instance, glasses.x, glasses.y, glasses.w, glasses.h, sout))
            return 1;
    }
    vk.phys = sout.phys;
    vk.surface = sout.surface;
    if (!vkr_init_device(vk) || !vkr_init_swapchain(vk) || !vkr_init_pipeline(vk)) {
        vkr_destroy(vk);
        if (sout.direct) direct_restore(dpy);
        return 1;
    }
    // Direct mode reflowed the desktop: the capture source rect moved.
    if (sout.direct && !test_pattern) {
        outputs = list_outputs(dpy);
        bool still_there = false;
        for (auto& o : outputs)
            if (o.name == source.name) { source = o; still_there = true; break; }
        if (!still_there) {
            printf("capture source disappeared — falling back to test pattern\n");
            test_pattern = true;
        }
    }
    int rr_event_base = 0, rr_error_base = 0;
    XRRQueryExtension(dpy, &rr_event_base, &rr_error_base);
    XRRSelectInput(dpy, root, RRScreenChangeNotifyMask);
```

- [ ] **Step 7: Capture setup.** In the XShm block, `XShmCreateImage` currently takes `vi->visual` (the deleted GLX visual). Change that call to:

```cpp
        ximg = XShmCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 24,
                               ZPixmap, nullptr, &shm, source.w, source.h);
```

- [ ] **Step 8: Texture setup.** Replace the whole `// -- texture` block (GL gen/bind/params/`glTexImage2D`) with:

```cpp
    // -- capture texture
    int tex_w = test_pattern ? 1024 : source.w;
    int tex_h = test_pattern ? 576 : source.h;
    uint32_t tex_pitch = test_pattern ? uint32_t(tex_w) * 4
                                      : uint32_t(ximg->bytes_per_line);
    if (!vkr_init_texture(vk, tex_w, tex_h, tex_pitch)) return 1;
    std::vector<uint32_t> pattern(size_t(tex_w) * tex_h);
```

- [ ] **Step 9: Projection block.** In the `// -- projection ...` section, keep the `DIAG_FOV`/`diag_px`/`half`/`near_z`/`far_z`/`r`/`t` computations but change `glasses.w`/`glasses.h` to the actual swapchain in direct mode — insert immediately before `float diag_px = ...`:

```cpp
    // In direct mode the swapchain extent is authoritative (mode may differ
    // from the desktop rect we detected).
    if (sout.direct) { glasses.w = int(vk.extent.width); glasses.h = int(vk.extent.height); }
```

- [ ] **Step 10: RandR change events.** In the event loop, the current code starts with `if (ev.type != KeyPress) continue;`. Replace that line with:

```cpp
            if (ev.type == rr_event_base + RRScreenChangeNotify) {
                XRRUpdateConfiguration(&ev);
                if (!test_pattern) {
                    auto outs = list_outputs(dpy);
                    for (auto& o : outs) {
                        if (o.name != source.name) continue;
                        if (o.w != source.w || o.h != source.h) {
                            // Source resized: rebuild the XShm segment + texture.
                            XShmDetach(dpy, &shm);
                            XDestroyImage(ximg);
                            shmdt(shm.shmaddr);
                            source = o;
                            ximg = XShmCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)),
                                                   24, ZPixmap, nullptr, &shm, source.w, source.h);
                            shm.shmid = shmget(IPC_PRIVATE,
                                               size_t(ximg->bytes_per_line) * ximg->height,
                                               IPC_CREAT | 0600);
                            shm.shmaddr = ximg->data = (char*)shmat(shm.shmid, nullptr, 0);
                            shm.readOnly = False;
                            XShmAttach(dpy, &shm);
                            shmctl(shm.shmid, IPC_RMID, nullptr);
                            vkr_destroy_texture(vk);
                            vkr_init_texture(vk, source.w, source.h,
                                             uint32_t(ximg->bytes_per_line));
                            cap_aspect = float(source.w) / float(source.h);
                        } else {
                            source = o;  // moved only: new grab origin
                        }
                        break;
                    }
                }
                continue;
            }
            if (ev.type != KeyPress) continue;
```

- [ ] **Step 11: Capture tick.** Replace the GL upload calls in the 30 Hz capture block: drop `glBindTexture(...)`, replace the test-pattern `glTexSubImage2D(...)` with `vkr_upload(vk, pattern.data(), pattern.size() * 4);` and the XShm branch with:

```cpp
            } else if (XShmGetImage(dpy, root, ximg, source.x, source.y, AllPlanes)) {
                vkr_upload(vk, ximg->data, size_t(ximg->bytes_per_line) * ximg->height);
            }
```

- [ ] **Step 12: Render block.** Replace everything from `// ---- render` through the `glXSwapBuffers(dpy, win);` line (inclusive — this removes all `gl*` calls and the `use_sgi` wait) with:

```cpp
        // ---- render
        QuadDraw draws[5];
        int ndraw = 0;
        if (have_pose && anchored) {
            // view = trim ⊗ inverse(recentered head pose)
            Quat head_rc = qmul(qconj(ori_offset), head_q);
            Vec3 hp = qrot(qconj(ori_offset), head_p);
            Quat view_q = qconj(qmul(head_rc, trim));
            Vec3 hp_neg = qrot(view_q, { -hp.x, -hp.y, -hp.z });
            float view[16], model[16], vm[16], proj[16], mvp[16];
            mat_from_pose(view_q, hp_neg, view);
            mat_from_pose(anchor_q, anchor_p, model);
            mat_mul(view, model, vm);
            mat_projection_vk(r, t, near_z, far_z, proj);
            mat_mul(proj, vm, mvp);

            // quad dimensions from diagonal size + capture aspect
            float diag_m = diag_in * 0.0254f;
            float w2 = diag_m * cap_aspect / std::sqrt(1 + cap_aspect * cap_aspect) * 0.5f;
            float h2 = diag_m / std::sqrt(1 + cap_aspect * cap_aspect) * 0.5f;

            auto quad = [&](float cx, float cy, float hw, float hh,
                            const float* col, bool textured) {
                QuadDraw& d = draws[ndraw++];
                memcpy(d.mvp, mvp, sizeof(mvp));
                memcpy(d.color, col, 4 * sizeof(float));
                d.rect[0] = cx; d.rect[1] = cy; d.rect[2] = hw; d.rect[3] = hh;
                d.textured = textured;
            };
            // thin frame for depth perception; orange warns that positional
            // tracking is frozen (orientation-only mode)
            const float white[4] = { 1, 1, 1, 1 };
            const float live[4] = { 0.35f, 0.76f, 1.f, 1.f };
            const float frozen[4] = { 1.f, 0.55f, 0.2f, 1.f };
            const float* bc = sixdof_live ? live : frozen;
            float bt = 0.004f * diag_m;  // border half-thickness
            quad(0, 0, w2, h2, white, true);
            quad(0, h2, w2 + bt, bt, bc, false);
            quad(0, -h2, w2 + bt, bt, bc, false);
            quad(-w2, 0, bt, h2 + bt, bc, false);
            quad(w2, 0, bt, h2 + bt, bc, false);
        }
        vkr_draw(vk, draws, ndraw);
```
(The `frames++;` and fps print that followed stay unchanged.)

- [ ] **Step 13: Teardown.** Replace the teardown block after `printf("shutting down…\n");` — the three `xr_device_provider_*` calls stay, then replace the GLX/window cleanup with:

```cpp
    if (ximg) { XShmDetach(dpy, &shm); XDestroyImage(ximg); shmdt(shm.shmaddr); }
    vkr_destroy(vk);                       // instance destruction drops the lease
    if (sout.direct) direct_restore(dpy);  // then hand the output back to the desktop
    else if (sout.window) XDestroyWindow(dpy, sout.window);
    XCloseDisplay(dpy);
    return 0;
```

- [ ] **Step 14: Makefile + run.sh cleanup.** In `spatial-screens/Makefile` delete the `LEGACY_GL := -lGL` line and remove `$(LEGACY_GL)` from `LDFLAGS`. In `spatial-screens/run.sh` delete the two GL vsync exports and their comment (`# Force vsync on both driver stacks...`, `export vblank_mode=3`, `export __GL_SYNC_TO_VBLANK=1`) — Vulkan FIFO owns pacing now.

- [ ] **Step 15: Build**

Run: `cd spatial-screens && make`
Expected: clean build of `spatial-screens` with no warnings; `ldd spatial-screens | grep -i gl` shows no libGL.

- [ ] **Step 16: RUNTIME CHECKPOINT — full app on hardware** (glasses plugged in, `viture-bridge` stopped)

Run: `cd spatial-screens && ./run.sh --pitch-trim 16`
Expected: console prints `direct mode: 1920x1200@120 Hz...`; desktop reflows off DP-1; glasses show the captured monitor world-anchored as before; fps line reads ~120; Ctrl+Alt+R recenters; Ctrl+Alt+Q quits and DP-1 returns to the desktop. Then run `./run.sh --window --capture test` and confirm the old windowed behavior still works.

- [ ] **Step 17: Commit**

```bash
git add spatial-screens/src/main.cpp spatial-screens/Makefile spatial-screens/run.sh
git commit -m "spatial-screens: direct mode by default — Vulkan display lease, GLX path removed"
```

---

### Task 8: Restore-path verification, README + docs

**Files:**
- Modify: `spatial-screens/README.md`
- Modify: `docs/plan/roadmap.md` (only if it lists presentation/tearing work — check and update the relevant line)

- [ ] **Step 1: Run the spec's remaining verification checklist** (spec §Verification, items 3–5):

1. `./run.sh` then SIGTERM from another terminal (`pkill spatial-screens`): DP-1 restored.
2. `./run.sh --monitor DP-9` (bogus): exits with the existing "glasses display not found" error, desktop untouched.
3. While running in direct mode, change the laptop panel's resolution (`xrandr --output eDP-1 --mode 1920x1200` then back): capture follows without a crash (RRScreenChangeNotify path).
Record pass/fail for each in the final report to the user.

- [ ] **Step 2: Update `spatial-screens/README.md`:**
- Run section: mention direct mode is the default, `--window` forces the fallback, and stopping `viture-bridge` first still applies.
- Options list: add `--window`, remove `--sgi-sync`.
- Scope section: mark the presentation milestone as direct-mode Vulkan (M2 upgraded), test harness `make vk-test && ./vk-test [--direct]`.
- Notes: add crash recovery — if the app is SIGKILLed in direct mode the output stays non-desktop; recover with `xrandr --output DP-1 --set non-desktop 0`.

- [ ] **Step 3: Final commit**

```bash
git add spatial-screens/README.md docs/plan/roadmap.md
git commit -m "spatial-screens: document direct mode (README, roadmap)"
```

---

## Plan Self-Review Notes

- **Spec coverage:** acquisition dance incl. save/restore + primary flag (Task 4), fallback-restores-first (Task 4 `direct_acquire` failure paths + Task 6 Step 4 verifies), one-pipeline/no-vertex-buffer renderer with 112-byte push block (Tasks 2–3), FIFO/minImageCount-2/two-frames-in-flight (Task 3), capture reflow + resize handling (Task 7 Steps 6/10), GLX+SGI deletion (Task 7), deps + go/no-go gate (Task 1), manual verification items (Tasks 5–8).
- **Known simplification vs spec:** spec's "RAII guard" is realized as `direct_restore()` called on every exit path (failure paths inside `direct_acquire`, error path after renderer init, normal teardown) — equivalent behavior, simpler in this procedural codebase. Signals already funnel through `g_running` → normal teardown.
- **Type consistency check done:** `QuadDraw.rect` = {cx, cy, half_w, half_h} everywhere; `vkr_init_texture(w, h, pitch_bytes)` callers pass bytes; `list_outputs`/`OutputRect.id` shared via `vk_surface.h`.

