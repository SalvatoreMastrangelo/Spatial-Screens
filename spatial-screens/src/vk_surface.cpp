#include "vk_surface.h"

#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_XLIB_XRANDR_EXT
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xlib.h>
#include <vulkan/vulkan_xlib_xrandr.h>

#include <X11/Xatom.h>
#include <cstdio>
#include <unistd.h>

// Restore races the server's async lease teardown; RandR calls during the
// window raise BadAccess, which must not kill the process mid-restore.
static int ignore_x_error(Display*, XErrorEvent*) { return 0; }

// Scoped swallow-all X error handler: direct_acquire/direct_restore must
// survive RandR errors even in binaries without a global handler (vk-test).
struct x_error_guard {
    XErrorHandler old;
    x_error_guard() : old(XSetErrorHandler(ignore_x_error)) {}
    ~x_error_guard() { XSetErrorHandler(old); }
};

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

std::vector<OutputRect> list_monitors(Display* dpy) {
    std::vector<OutputRect> out;
    Window root = DefaultRootWindow(dpy);
    int n = 0;
    XRRMonitorInfo* mi = XRRGetMonitors(dpy, root, True, &n);
    if (!mi) return out;
    for (int i = 0; i < n; i++) {
        char* name = XGetAtomName(dpy, mi[i].name);
        out.push_back({ name ? name : "", 0, mi[i].x, mi[i].y,
                        mi[i].width, mi[i].height });
        if (name) XFree(name);
    }
    XRRFreeMonitors(mi);
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
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDisplayKHR display = VK_NULL_HANDLE;
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
    XErrorHandler old_handler = XSetErrorHandler(ignore_x_error);
    set_non_desktop(dpy, g_saved.output, 0);
    if (g_saved.crtc && g_saved.mode) {
        Window root = DefaultRootWindow(dpy);
        // The server tears the lease down asynchronously after the client
        // drops the fd (instance destruction); until then the CRTC/output
        // stay guarded (BadAccess / RRSetConfigFailed). Swallow X errors and
        // retry the re-enable until the lease is gone (~3 s worst case).
        Status st = RRSetConfigFailed;
        for (int i = 0; i < 30 && st != RRSetConfigSuccess; i++) {
            XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
            st = XRRSetCrtcConfig(dpy, res, g_saved.crtc, CurrentTime,
                                  g_saved.x, g_saved.y, g_saved.mode, g_saved.rot,
                                  g_saved.crtc_outputs.data(),
                                  (int)g_saved.crtc_outputs.size());
            XRRFreeScreenResources(res);
            if (st != RRSetConfigSuccess) usleep(100 * 1000);
        }
        if (st == RRSetConfigSuccess && g_saved.was_primary)
            XRRSetOutputPrimary(dpy, root, g_saved.output);
        XSync(dpy, False);
        if (st == RRSetConfigSuccess)
            printf("direct mode: output returned to the desktop\n");
        else
            fprintf(stderr, "direct mode: could not re-enable the output — "
                            "run: xrandr --output <name> --auto\n");
    } else {
        XSync(dpy, False);
    }
    XSetErrorHandler(old_handler);
    g_saved.prop_set = false;
}

void direct_release(VkInstance inst) {
    if (!g_saved.display) return;
    auto p_rel = (PFN_vkReleaseDisplayEXT)
        vkGetInstanceProcAddr(inst, "vkReleaseDisplayEXT");
    if (p_rel) {
        p_rel(g_saved.phys, g_saved.display);
    } else {
        fprintf(stderr, "direct mode: vkReleaseDisplayEXT missing — lease held until exit\n");
    }
    g_saved.display = VK_NULL_HANDLE;
}

bool direct_acquire(Display* dpy, VkInstance inst, RROutput out_id, SurfaceOut& out) {
    Window root = DefaultRootWindow(dpy);

    if (XInternAtom(dpy, "non-desktop", True) == None) {
        fprintf(stderr, "direct mode: server has no non-desktop property\n");
        return false;
    }
    x_error_guard xguard;

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
    g_saved.phys = phys;
    g_saved.display = display;

    // Mode: native resolution at the highest refresh (spec: 1920x1200@120).
    uint32_t nmode = 0;
    vkGetDisplayModePropertiesKHR(phys, display, &nmode, nullptr);
    std::vector<VkDisplayModePropertiesKHR> modes(nmode);
    vkGetDisplayModePropertiesKHR(phys, display, &nmode, modes.data());
    if (!nmode) {
        fprintf(stderr, "direct mode: display exposes no modes\n");
        direct_release(inst);
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
        direct_release(inst);
        direct_restore(dpy);
        return false;
    }
    VkDisplayPlaneCapabilitiesKHR pcaps;
    vkGetDisplayPlaneCapabilitiesKHR(phys, best.displayMode, plane, &pcaps);
    VkDisplayPlaneAlphaFlagBitsKHR alpha =
        (pcaps.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR)
            ? VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR
            : (VkDisplayPlaneAlphaFlagBitsKHR)(pcaps.supportedAlpha & -pcaps.supportedAlpha);

    VkDisplaySurfaceCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR;
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
        direct_release(inst);
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

    VkXlibSurfaceCreateInfoKHR xci{};
    xci.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
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
