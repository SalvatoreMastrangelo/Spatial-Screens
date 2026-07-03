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
// Release the leased display back to the X server. Call after
// vkr_destroy_device() (swapchain/surface must be gone) and before
// vkr_destroy() (needs the live instance). Then call direct_restore().
void direct_release(VkInstance inst);
bool window_create(Display* dpy, VkInstance inst, int x, int y, int w, int h, SurfaceOut& out);
