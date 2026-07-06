// spatial-screens — phase 2 renderer spike (M2 + first cut of M3).
//
// Places one virtual screen in 3D space using the Luma Ultra's 6DoF pose and
// renders it fullscreen on the glasses' display. The screen is textured with
// a live capture from a pluggable backend (--capture-backend): a source
// monitor via XShm, or a test pattern.
//
//   ./run.sh [--monitor NAME] [--capture NAME|test]
//            [--capture-backend auto|portal|xshm|test] [--distance M]
//            [--size INCHES] [--pitch-trim DEG] [--predict-ms MS]
//
// Keys:  R recenter (re-place screen in front of you)
//        Shift+R also reset the VIO origin
//        [ / ]  screen closer / farther      - / =  smaller / larger
//        Q/Esc  quit
//        Gestures (if the sidecar connects): pinch-drag vertical =
//        distance (anchor stays fixed); fist held ~0.5s = recenter.
//
// NOTE: stop viture-bridge before running — the SDK supports one client.
// Presentation: Vulkan direct display by default (RandR non-desktop=1 +
// VK_EXT_acquire_xlib_display leases the glasses output straight from
// Mesa). --window forces the EWMH-fullscreen windowed fallback instead.
//
// Coordinates: get_gl_pose_carina returns Twb in OpenGL/EUS (x right, y up,
// z backward), position in meters. World-locked content = render with
// view = inverse(head pose).

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "viture_glasses_provider.h"
#include "viture_device_carina.h"
#include "vk_renderer.h"
#include "pose_math.h"
#include "vk_surface.h"
#include "gesture_client.h"
#include "capture.h"
#include "capture_portal.h"
#include "config.h"
#include "telemetry.h"
#include "sbs_mode.h"
#include "scene.h"
#include "stereo.h"

// -------------------------------------------------------- cursor overlay ----
// Neither capture path delivers the pointer (mutter on X11 ignores the
// portal's embedded cursor_mode; XShm never grabs it), so blend it in
// ourselves from XFixes after each frame upload.

// Root-space origin of the captured region: the source rect XShm was built
// on (capture_name), else the first non-glasses output matching the frame's
// exact size (portal doesn't say which monitor the user picked). Same-size
// twin monitors can mismatch under portal; scaled streams get no cursor.
static bool cursor_source_origin(const std::vector<OutputRect>& outs,
                                 const std::string& capture_name,
                                 const std::string& glasses_name,
                                 int w, int h, int& x, int& y) {
    for (auto& o : outs)
        if (!capture_name.empty() && o.name == capture_name && o.w == w && o.h == h) {
            x = o.x; y = o.y; return true;
        }
    for (auto& o : outs)
        if (o.name != glasses_name && o.w == w && o.h == h) {
            x = o.x; y = o.y; return true;
        }
    return false;
}

// Saved patch of staging pixels beneath the last cursor stamp, so the
// pointer can move at the tick rate between (possibly slower) content
// frames: restore the patch, re-blend at the new position, re-upload.
struct CursorUnder {
    std::vector<uint8_t> px;
    int x = 0, y = 0, w = 0, h = 0;  // rect in frame coords
    bool valid = false;
};

static void cursor_restore(CursorUnder& u, uint8_t* dst, uint32_t pitch) {
    if (!u.valid) return;
    for (int row = 0; row < u.h; row++)
        memcpy(dst + size_t(u.y + row) * pitch + size_t(u.x) * 4,
               u.px.data() + size_t(row) * u.w * 4, size_t(u.w) * 4);
    u.valid = false;
}

// Alpha-blend the XFixes cursor (premultiplied ARGB; rows of unsigned long,
// low 32 bits per pixel) over BGRX pixels, saving the covered rect into `u`
// first. sx/sy = captured region origin in root space.
static void composite_cursor(Display* dpy, uint8_t* dst, int w, int h,
                             uint32_t pitch, int sx, int sy, CursorUnder& u) {
    XFixesCursorImage* ci = XFixesGetCursorImage(dpy);
    if (!ci) return;
    int cx = ci->x - ci->xhot - sx, cy = ci->y - ci->yhot - sy;
    int x0 = std::max(cx, 0), y0 = std::max(cy, 0);
    int x1 = std::min(cx + ci->width, w), y1 = std::min(cy + ci->height, h);
    if (x0 >= x1 || y0 >= y1) { XFree(ci); return; }
    u.x = x0; u.y = y0; u.w = x1 - x0; u.h = y1 - y0;
    u.px.resize(size_t(u.w) * u.h * 4);
    for (int row = 0; row < u.h; row++)
        memcpy(u.px.data() + size_t(row) * u.w * 4,
               dst + size_t(u.y + row) * pitch + size_t(u.x) * 4, size_t(u.w) * 4);
    u.valid = true;
    for (int dy = y0; dy < y1; dy++) {
        uint32_t* out = reinterpret_cast<uint32_t*>(dst + size_t(dy) * pitch);
        const unsigned long* src = ci->pixels + size_t(dy - cy) * ci->width - cx;
        for (int dx = x0; dx < x1; dx++) {
            uint32_t s = uint32_t(src[dx]);
            uint32_t a = s >> 24;
            if (!a) continue;
            uint32_t d = out[dx];
            uint32_t rb = ((d & 0x00ff00ffu) * (255 - a) / 255) & 0x00ff00ffu;
            uint32_t g = ((d & 0x0000ff00u) * (255 - a) / 255) & 0x0000ff00u;
            out[dx] = (s & 0x00ffffffu) + rb + g;  // premul src + dst*(1-a)
        }
    }
    XFree(ci);
}

// ------------------------------------------------------------- SDK glue ----

static XRDeviceProviderHandle g_provider = nullptr;
static int g_pid = 0;
static int g_sbs_orig = -1;
static std::atomic<bool> g_running{true};
static bool g_probe_camera = false;
static int g_probe_frames_remaining = 0;
static GestureClient g_gestures;
static std::atomic<int> g_cam_w{0};  // tracking-camera frame size, for hand-overlay aspect
static std::atomic<int> g_cam_h{0};
static constexpr float PINCH_DISTANCE_SENSITIVITY = 4.0f; // tune after hands-on test; higher = faster response to hand motion
static constexpr double FIST_HOLD_SECONDS = 0.5;          // how long a fist must be held before it triggers recenter

static void on_imu_noop(float*, double) {}
static void on_pose_noop(float*, double) {}

static void on_camera_carina(char* image_left0, char* /*image_right0*/,
                              char* /*image_left1*/, char* /*image_right1*/,
                              double timestamp, int width, int height) {
    if (g_probe_camera && g_probe_frames_remaining > 0) {
        static int frame_idx = 0;
        char path[256];
        snprintf(path, sizeof(path), "/tmp/spatial-screens-probe-%03d.pgm", frame_idx++);
        FILE* f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P5\n%d %d\n255\n", width, height);
            fwrite(image_left0, 1, size_t(width) * size_t(height), f);
            fclose(f);
            printf("gestures: probe frame -> %s (%dx%d, t=%.3f)\n", path, width, height, timestamp);
        }
        g_probe_frames_remaining--;
    }
    g_cam_w.store(width, std::memory_order_relaxed);
    g_cam_h.store(height, std::memory_order_relaxed);
    g_gestures.maybe_send_frame(reinterpret_cast<uint8_t*>(image_left0), width, height, timestamp);
}

static int scan_viture_pid() {
    DIR* dir = opendir("/sys/bus/usb/devices");
    if (!dir) return 0;
    dirent* e;
    int found = 0;
    while ((e = readdir(dir)) && !found) {
        std::string base = std::string("/sys/bus/usb/devices/") + e->d_name;
        FILE* fv = fopen((base + "/idVendor").c_str(), "r");
        if (!fv) continue;
        char vendor[8] = {0};
        if (fgets(vendor, sizeof(vendor), fv) && strncmp(vendor, "35ca", 4) == 0) {
            FILE* fp = fopen((base + "/idProduct").c_str(), "r");
            if (fp) {
                unsigned pid = 0;
                if (fscanf(fp, "%x", &pid) == 1 &&
                    xr_device_provider_is_product_id_valid(int(pid)))
                    found = int(pid);
                fclose(fp);
            }
        }
        fclose(fv);
    }
    closedir(dir);
    return found;
}

static bool sdk_init() {
    int pid = scan_viture_pid();
    if (!pid) {
        fprintf(stderr, "No supported VITURE glasses found.\n");
        return false;
    }
    g_pid = pid;
    xr_device_provider_set_log_level(1);
    g_provider = xr_device_provider_create(pid);
    if (!g_provider) {
        fprintf(stderr, "provider create failed — is viture-bridge still running? Stop it first.\n");
        return false;
    }
    if (xr_device_provider_get_device_type(g_provider) != XR_DEVICE_TYPE_VITURE_CARINA) {
        fprintf(stderr, "This spike needs a Luma Ultra (6DoF/Carina) device.\n");
        return false;
    }
    // Register a no-op pose callback too: we poll get_gl_pose_carina for
    // rendering, but the first session of the day (pose callback registered)
    // showed live VIO translation while later imu-only sessions did not —
    // cheap insurance in case registration gates the camera pipeline.
    if (register_callbacks_carina(g_provider, on_pose_noop, nullptr, on_imu_noop, on_camera_carina) != 0 ||
        xr_device_provider_initialize(g_provider, nullptr) != 0) {
        fprintf(stderr, "SDK init failed\n");
        return false;
    }
    sleep(1);
    if (xr_device_provider_start(g_provider) != 0) {
        fprintf(stderr, "SDK start failed (permissions? another client?)\n");
        return false;
    }
    printf("SDK started (pid 0x%04x, Carina 6DoF)\n", pid);
    return true;
}

// Every exit path after sdk_init() must restore the panel mode BEFORE the
// provider goes away, then tear the SDK down. Safe to call once only.
static void sdk_shutdown() {
    if (!g_provider) return;
    sbs_exit(g_provider, g_sbs_orig);
    g_sbs_orig = -1;
    xr_device_provider_stop(g_provider);
    xr_device_provider_shutdown(g_provider);
    xr_device_provider_destroy(g_provider);
    g_provider = nullptr;
}

static std::string executable_dir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    std::string path(buf);
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

// Cheap RSS sample for the leak watchdog: 2nd field of /proc/self/statm is
// resident pages. Not for per-frame use — call it at the ~2s fps cadence.
static int sample_rss_mb() {
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    long pages = 0, resident = 0;
    int got = fscanf(f, "%ld %ld", &pages, &resident);
    fclose(f);
    if (got != 2) return 0;
    long page_size = sysconf(_SC_PAGESIZE);
    return int(resident * page_size / (1024 * 1024));
}

// ---------------------------------------------------------------- main ----

static void on_signal(int) { g_running = false; }

// X errors must never kill us: between display acquisition and teardown a
// fatal default handler would strand the leased output (seen live: BadAccess
// from XGrabKey when another client held the combo). Log and carry on.
static int on_x_error(Display*, XErrorEvent* e) {
    fprintf(stderr, "x11: non-fatal error (request %d, error %d)\n",
            e->request_code, e->error_code);
    return 0;
}

int main(int argc, char** argv) {
    // Defaults sized to FIT the Ultra's 52-degree FOV with margin: at 2 m the
    // panel shows ~85 inches full-width — a 60-inch screen leaves the frame
    // and the world visible around it, which is what makes 6DoF perceivable.
    // predict 0 by default: XRLinuxDriver's known-good usage passes 0, and
    // extrapolation visibly amplifies rotation jitter during head turns.
    // 24" at 0.75 m = desk-monitor ergonomics that fit the 52-degree FOV.
    // Prediction stays 0: extrapolation noise feeds the filter's speed
    // estimate and reads as shake (verified across builds).
    // Pose smoothing (EMA blend factor per frame, 1 = off). Position gets a
    // heavy filter — VIO translation is where the jitter lives; orientation
    // stays light so head tracking doesn't feel laggy.
    Options o;
    AppState app_state;
    std::string config_path = config_default_path();
    bool config_explicit = false, dump_config = false;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--config") && i + 1 < argc) { config_path = argv[++i]; config_explicit = true; }
    load_options_file(config_path, o, config_explicit);
    load_state(app_state);
    if (app_state.distance > 0) o.distance = app_state.distance;
    if (app_state.size > 0) o.size = app_state.size;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        bool ok = false;
        if (!strncmp(a, "--", 2)) {
            a += 2;
            if (!strcmp(a, "config")) { i++; ok = true; }  // handled in the pre-pass
            else if (!strcmp(a, "window")) { o.window = true; ok = true; }
            else if (!strcmp(a, "dump-config")) { dump_config = true; ok = true; }
            else if (!strcmp(a, "probe-camera")) { g_probe_camera = true; g_probe_frames_remaining = 10; ok = true; }
            else if (i + 1 < argc) ok = set_option(o, a, argv[++i]);
        }
        if (!ok) {
            printf("usage: %s [--monitor NAME] [--capture NAME|test] "
                   "[--capture-backend auto|portal|xshm|test] [--capture-hz N] [--distance M] "
                   "[--size IN] [--pitch-trim DEG] [--predict-ms MS] [--smooth-pos 0..1] "
                   "[--smooth-ori 0..1] [--ws-port N] [--window] [--config PATH] "
                   "[--dump-config] [--probe-camera]\n"
                   "config: %s   state: %s\n",
                   argv[0], config_default_path().c_str(), state_file_path().c_str());
            return 0;
        }
    }
    if (o.capture == "test") { o.capture_backend = "test"; o.capture.clear(); }
    if (dump_config) {
        printf("# effective options (config %s, state %s)\n",
               config_path.c_str(), state_file_path().c_str());
        printf("monitor = %s\ncapture = %s\ncapture-backend = %s\ncapture-hz = %d\n",
               o.monitor.c_str(), o.capture.c_str(), o.capture_backend.c_str(), o.capture_hz);
        printf("distance = %.3f\nsize = %.1f\npitch-trim = %.2f\npredict-ms = %.2f\n",
               o.distance, o.size, o.pitch_trim, o.predict_ms);
        printf("smooth-pos = %.2f\nsmooth-ori = %.2f\nwindow = %s\nws-port = %d\n",
               o.smooth_pos, o.smooth_ori, o.window ? "true" : "false", o.ws_port);
        printf("stereo = %s\nipd-mm = %.1f\nworkspace = %s\nscreens = %zu configured\n",
               o.stereo ? "true" : "false", o.ipd_mm, o.workspace.c_str(), o.screens.size());
        return 0;
    }
    Telemetry tele;
    tele.start(uint16_t(o.ws_port));
    // Local aliases: the render loop mutates distance/size at runtime.
    std::string monitor_name = o.monitor, capture_name = o.capture;
    std::string capture_backend = o.capture_backend;
    int capture_hz = o.capture_hz;
    float distance = o.distance, diag_in = o.size, pitch_trim = o.pitch_trim;
    float predict_ms = o.predict_ms, smooth_pos = o.smooth_pos, smooth_ori = o.smooth_ori;
    bool force_window = o.window;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { fprintf(stderr, "cannot open X display\n"); return 1; }
    XSetErrorHandler(on_x_error);
    Window root = DefaultRootWindow(dpy);
    int xfixes_ev = 0, xfixes_err = 0;
    bool have_xfixes = XFixesQueryExtension(dpy, &xfixes_ev, &xfixes_err);

    // -- pick outputs: glasses = 1920x1200-ish (or --monitor), capture = another
    auto outputs = list_outputs(dpy);
    OutputRect glasses{};
    bool have_glasses = false;
    printf("outputs:\n");
    for (auto& o : outputs) printf("  %-10s %dx%d+%d+%d\n", o.name.c_str(), o.w, o.h, o.x, o.y);
    for (auto& o : outputs) {
        bool is_glasses_mode = (o.h == 1200 && (o.w == 1920 || o.w == 3840));
        // Laptop panels (eDP-*) can share the glasses' 1920x1200 mode — never
        // auto-pick them, or we fullscreen over the user's own screen.
        bool is_laptop_panel = o.name.rfind("eDP", 0) == 0 || o.name.rfind("LVDS", 0) == 0;
        if (!monitor_name.empty() ? o.name == monitor_name
                                  : (is_glasses_mode && !is_laptop_panel)) {
            glasses = o; have_glasses = true; break;
        }
    }
    if (!have_glasses) {
        fprintf(stderr, "glasses display not found (no 1920x1200 output; use --monitor NAME)\n");
        return 1;
    }
    printf("glasses: %s (%dx%d)\n", glasses.name.c_str(), glasses.w, glasses.h);

    // -- SDK first: the panel must be in its final mode before Vulkan
    // enumerates display modes (direct mode picks from what's exposed).
    if (!sdk_init()) return 1;

    // -- SBS 3D: switch the panel and wait for RandR to see 3840-wide.
    // Window mode skips the panel switch entirely (debug renders SBS halves
    // into a desktop window). Failure falls back to mono — never fatal.
    if (o.stereo && !force_window) {
        g_sbs_orig = sbs_enter(g_provider, dpy, glasses.name);
        if (g_sbs_orig >= 0) {
            // The mode switch reflowed the desktop — re-resolve the glasses
            // rect (position AND size changed to 3840x1200).
            outputs = list_outputs(dpy);
            for (auto& out2 : outputs)
                if (out2.name == glasses.name) { glasses = out2; break; }
        }
    }

    // -- Vulkan: direct display (default) or EWMH-fullscreen window fallback
    VkRend vk{};
    if (!vkr_create_instance(vk, !force_window)) { sdk_shutdown(); return 1; }
    SurfaceOut sout{};
    bool direct_ok = !force_window && vk.has_display_ext &&
                     direct_acquire(dpy, vk.instance, glasses.id, sout);
    if (!direct_ok) {
        if (!force_window) fprintf(stderr, "direct mode unavailable — window fallback\n");
        // direct_acquire restores the desktop on failure, but the layout may
        // have shifted meanwhile — re-resolve the glasses rect first.
        outputs = list_outputs(dpy);
        for (auto& o : outputs) if (o.name == glasses.name) { glasses = o; break; }
        if (!window_create(dpy, vk.instance, glasses.x, glasses.y, glasses.w, glasses.h, sout)) {
            sdk_shutdown();
            return 1;
        }
    }
    vk.phys = sout.phys;
    vk.surface = sout.surface;
    if (!vkr_init_device(vk) || !vkr_init_swapchain(vk) || !vkr_init_pipeline(vk)) {
        vkr_destroy_device(vk);
        if (sout.direct) direct_release(vk.instance);
        vkr_destroy(vk);
        if (sout.direct) direct_restore(dpy);
        sdk_shutdown();
        return 1;
    }
    int rr_event_base = 0, rr_error_base = 0;
    XRRQueryExtension(dpy, &rr_event_base, &rr_error_base);
    XRRSelectInput(dpy, root, RRScreenChangeNotifyMask);

    // Global hotkeys (Ctrl+Alt+key) — focus on an override-redirect window is
    // unreliable, and the MVP wants a global recenter shortcut anyway. Grab
    // each combo with NumLock/CapsLock variants so they fire regardless.
    {
        KeySym hot[] = { XK_r, XK_bracketleft, XK_bracketright, XK_minus, XK_equal, XK_q };
        unsigned base = ControlMask | Mod1Mask;
        unsigned locks[] = { 0, Mod2Mask, LockMask, Mod2Mask | LockMask };
        for (KeySym ks : hot) {
            KeyCode kc = XKeysymToKeycode(dpy, ks);
            if (!kc) continue;
            for (unsigned lk : locks) {
                XGrabKey(dpy, kc, base | lk, root, True, GrabModeAsync, GrabModeAsync);
                if (ks == XK_r)
                    XGrabKey(dpy, kc, base | ShiftMask | lk, root, True, GrabModeAsync, GrabModeAsync);
            }
        }
    }

    // The lease just reflowed the desktop and RandR events weren't selected
    // yet — re-snapshot so both the scene's monitor matching and cursor
    // mapping start from live geometry.
    outputs = list_outputs(dpy);

    // Capture source rect = what the xshm backend grabs (the whole capture
    // output). UVs slice it; test/portal frames are sliced the same way.
    OutputRect cap_src{};
    for (auto& oo : outputs)
        if (!capture_name.empty() ? oo.name == capture_name
                                  : oo.name != glasses.name) { cap_src = oo; break; }
    MonRect fb_rect{cap_src.name, cap_src.x, cap_src.y, cap_src.w, cap_src.h};

    std::vector<MonRect> vs_mons;
    for (auto& m : list_monitors(dpy)) {
        bool inside = m.x >= cap_src.x && m.y >= cap_src.y &&
                      m.x + m.w <= cap_src.x + cap_src.w &&
                      m.y + m.h <= cap_src.y + cap_src.h;
        if (m.name.rfind("VS", 0) == 0 && inside)
            vs_mons.push_back({m.name, m.x, m.y, m.w, m.h});
    }
    std::vector<ScreenInst> scene;
    if (!o.screens.empty() || vs_mons.size() > 1) {
        scene = scene_build(o.screens, vs_mons, fb_rect);
        if (scene.empty())
            fprintf(stderr, "scene: no configured screen matched a monitor — "
                            "falling back to single screen\n");
    }
    bool multi = scene.size() > 1;
    if (scene.empty()) {
        // Single screen, full frame: reproduces pre-multi behavior exactly.
        ScreenInst s;
        s.cfg.distance = distance;
        s.cfg.size = diag_in;
        scene.push_back(s);
    }
    printf("scene: %zu screen(s)%s\n", scene.size(), multi ? " (rack)" : "");
    float rack_dist_scale = multi && app_state.rack_distance_scale > 0
                                ? app_state.rack_distance_scale : 1.f;
    float rack_size_scale = multi && app_state.rack_size_scale > 0
                                ? app_state.rack_size_scale : 1.f;

    // -- capture backend chain: auto = portal -> xshm -> test; explicit
    // backend -> test. Multi-screen forces xshm — portal delivers one picked
    // stream and can't feed N uv rects.
    std::vector<std::string> chain;
    if (capture_backend == "auto") {
        if (multi) chain = { "xshm", "test" };  // portal can't feed N uv rects
        else chain = { "portal", "xshm", "test" };
    }
    else if (capture_backend != "test") chain = { capture_backend, "test" };
    else chain = { "test" };
    size_t chain_pos = 0;
    std::unique_ptr<CaptureBackend> cap;
    float cap_aspect = 16.f / 9.f;
    CursorUnder cursor_under;
    auto switch_backend = [&]() {
        while (chain_pos < chain.size()) {
            const std::string& kind = chain[chain_pos++];
            std::unique_ptr<CaptureBackend> b;
            if (kind == "portal") {
                if (!capture_name.empty())
                    printf("capture: --capture is ignored under portal "
                           "(the picker/restore token owns source selection)\n");
                b = capture_create_portal(app_state.restore_token,
                                          [&](const std::string& tok) {
                                              app_state.restore_token = tok;
                                              save_state(app_state);
                                          },
                                          capture_hz);
            } else if (kind == "xshm") {
                auto outs = list_outputs(dpy);
                OutputRect src{};
                bool found = false;
                for (auto& o : outs)
                    if (!capture_name.empty() ? o.name == capture_name
                                              : o.name != glasses.name) { src = o; found = true; break; }
                if (found) b = capture_create_xshm(dpy, src);
                else fprintf(stderr, "capture: no xshm source monitor\n");
            } else if (kind == "test") {
                b = capture_create_test();
            } else {
                fprintf(stderr, "capture: unknown backend %s\n", kind.c_str());
            }
            if (b && b->start()) {
                cap = std::move(b);
                printf("capture: %s\n", cap->name());
                char msg[128];
                snprintf(msg, sizeof(msg), "capture backend: %s", cap->name());
                tele.log("info", msg);
                return;
            }
            if (b) fprintf(stderr, "capture: %s failed to start — falling back\n", kind.c_str());
        }
    };
    switch_backend();

    // -- capture texture: prefer real dims from a first frame
    CaptureFrame first{};
    for (int i = 0; i < 200 && cap && !cap->latest_frame(first); i++)
        usleep(10 * 1000);
    uint32_t tex_w = first.data ? uint32_t(first.w) : 1024;
    uint32_t tex_h = first.data ? uint32_t(first.h) : 576;
    uint32_t tex_pitch = first.data ? first.pitch : 1024 * 4;
    if (!vkr_init_texture(vk, tex_w, tex_h, tex_pitch)) {
        if (cap) cap->stop();
        vkr_destroy_device(vk);
        if (sout.direct) direct_release(vk.instance);
        vkr_destroy(vk);
        if (sout.direct) direct_restore(dpy);
        sdk_shutdown();
        return 1;
    }
    if (first.data) {
        vkr_upload(vk, first.data, size_t(first.pitch) * first.h);
        cap_aspect = float(first.w) / float(first.h);
    }

    char market[64] = {0};
    int mlen = sizeof(market);
    xr_device_provider_get_market_name(g_pid, market, &mlen);
    char fw[128] = {0};
    int fwlen = sizeof(fw);
    xr_device_provider_get_glasses_version(g_provider, fw, &fwlen);

    std::string gesture_socket = "/tmp/spatial-screens-gestures-" + std::to_string(getpid()) + ".sock";
    g_gestures.start(gesture_socket, executable_dir() + "/gestures/hand_tracker.py");
    tele.log("info", g_gestures.enabled() ? "gesture sidecar connected"
                                          : "gesture sidecar unavailable");

    // -- projection from the glasses' 52-degree diagonal FOV (16:10 panel).
    // Stereo: each eye is a full-FOV 1920x1200 panel — frustum from HALF the
    // swapchain width. Derived values refresh whenever the extent changes
    // (window resize, panel mode change under the lease).
    const float DIAG_FOV = 52.f;
    float near_z = 0.1f, far_z = 100.f;
    float r = 0, t = 0;
    bool stereo_active = false;
    VkExtent2D known_extent{0, 0};
    auto refresh_projection = [&]() {
        if (sout.direct) { glasses.w = int(vk.extent.width); glasses.h = int(vk.extent.height); }
        // Direct: stereo iff the scanned-out mode is SBS-wide. Window: honor
        // the config (debug SBS halves in a desktop window).
        stereo_active = o.stereo &&
            (sout.direct ? vk.extent.width >= 2 * vk.extent.height : true);
        uint32_t eye_w = stereo_active ? vk.extent.width / 2 : vk.extent.width;
        stereo_eye_frustum(eye_w, vk.extent.height, DIAG_FOV, near_z, r, t);
        known_extent = vk.extent;
        printf("render: %s, eye %ux%u\n", stereo_active ? "stereo (SBS)" : "mono",
               eye_w, vk.extent.height);
    };
    refresh_projection();
    float ipd_m = o.ipd_mm * 0.001f;

    // -- screen anchor state
    Quat head_q; Vec3 head_p;
    Quat ori_offset;              // yaw recenter
    Quat trim = quat_axis_angle(1, 0, 0, pitch_trim);
    Quat rack_q; Vec3 rack_p;     // rack origin pose (all screens hang off it)
    bool anchored = false;

    auto place_rack = [&]() {
        Quat basis = yaw_twist(qmul(qconj(ori_offset), head_q));
        rack_q = basis;
        // Rack origin AT the head; each screen walks out by its own distance
        // in scene_screen_pose — for a single screen this reproduces the old
        // place_screen geometry (head + fwd * distance) exactly.
        rack_p = qrot(qconj(ori_offset), head_p);
        anchored = true;
    };

    auto now_s = [] {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    };
    double last_fps_t = now_s(), last_cap_t = 0;
    int frames = 0, cap_frames = 0, last_cap_fps = 0;
    float last_fps = 0;
    int rss_mb = 0;
    bool rss_warned = false, rss_critical = false;
    bool have_pose = false;
    double recenter_at = -1;  // >0: re-seed pose + re-place at this time

    // 6DoF liveness heuristic: if the head clearly rotates but reported
    // position stays frozen, the VIO is running orientation-only.
    bool sixdof_live = false;
    Vec3 win_min, win_max;
    Quat win_q0;
    float win_max_ang = 0;
    int win_n = 0;
    bool was_pinching = false;
    float pinch_prev_y = 0;
    double fist_start_s = -1; // -1 = not currently holding a fist
    bool fist_triggered = false;
    bool gestures_armed = false; // actions gated: armed by an open palm, disarmed after each gesture

    printf("running — hotkeys work globally with Ctrl+Alt: R recenter (Shift adds "
           "VIO reset), [ ] distance, - = size, Q quit\n"
           "gestures (if sidecar connected): pinch-drag vertical=distance, "
           "fist-hold(0.5s)=recenter\n");

    while (g_running) {
        // ---- input
        while (g_running && XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == rr_event_base + RRScreenChangeNotify) {
                XRRUpdateConfiguration(&ev);
                outputs = list_outputs(dpy);  // keep fresh for cursor mapping
                if (cap) cap->on_outputs_changed(outputs);
                continue;
            }
            if (ev.type != KeyPress) continue;
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            bool shift = ev.xkey.state & ShiftMask;
            if (ks == XK_q || ks == XK_Escape) g_running = false;
            else if (ks == XK_r) {
                if (shift) {
                    // VIO re-zeros asynchronously — placing now would use the
                    // stale pose. Defer: re-seed + re-place once it settles.
                    reset_pose_carina(g_provider);
                    recenter_at = now_s() + 0.5;
                } else {
                    ori_offset = yaw_twist(head_q);
                    place_rack();
                }
                printf("recentered%s\n", shift ? " + VIO reset" : "");
                tele.log("info", "recentered");
            }
            else if (ks == XK_bracketleft) {
                if (multi) rack_dist_scale = std::max(0.25f, rack_dist_scale * 0.9f);
                else { distance = std::max(0.5f, distance - 0.25f); scene[0].cfg.distance = distance; }
                place_rack();
            }
            else if (ks == XK_bracketright) {
                if (multi) rack_dist_scale = std::min(4.f, rack_dist_scale * 1.1f);
                else { distance = std::min(10.f, distance + 0.25f); scene[0].cfg.distance = distance; }
                place_rack();
            }
            else if (ks == XK_minus) {
                if (multi) rack_size_scale = std::max(0.4f, rack_size_scale * 0.9f);
                else { diag_in = std::max(10.f, diag_in - 10.f); scene[0].cfg.size = diag_in; }
            }
            else if (ks == XK_equal) {
                if (multi) rack_size_scale = std::min(3.f, rack_size_scale * 1.1f);
                else { diag_in = std::min(400.f, diag_in + 10.f); scene[0].cfg.size = diag_in; }
            }
        }
        if (!g_running) break;

        double tnow = now_s();

        // ---- dashboard recenter request
        if (tele.reset_requested()) {
            reset_pose_carina(g_provider);
            recenter_at = tnow + 0.5;  // re-place after the VIO settles
            printf("recentered + VIO reset (dashboard)\n");
            tele.log("info", "pose reset via dashboard");
        }
        // Deferred post-VIO-reset re-place: dropping have_pose re-enters the
        // first-pose path, which hard-seeds the filters from the fresh pose,
        // re-derives the yaw offset, and re-places the screen in front.
        if (recenter_at > 0 && tnow >= recenter_at) {
            have_pose = false;
            recenter_at = -1;
        }

        // ---- gestures
        // Fist-hold takes priority and is mutually exclusive with pinch-drag:
        // the two pose classifiers run independently in the Python sidecar off
        // the same landmarks, so a single frame could in principle satisfy both.
        // When a fist is held, this frame is treated as "not pinching" (rather
        // than skipping the else via an added && condition) so that pinch state
        // resets cleanly — if pinch-drag resumes later, its first frame back
        // only seeds pinch_prev_y instead of computing a delta against stale
        // pre-fist coordinates.
        GestureEvent gev = g_gestures.poll();

        // Deliberate-intent gate: gesture ACTIONS (pinch-drag distance,
        // fist-hold recenter) fire only while "armed", and arming requires a
        // fully open hand. Each completed gesture disarms again, so a hand
        // merely passing through the tracking camera — or a stray pinch — does
        // nothing until you re-open your hand. The pinch-status dot shows the
        // state: grey (no hand), amber (hand seen, not armed), blue (armed),
        // green (armed + pinching).
        if (!gev.present) gestures_armed = false;      // hand gone -> re-arm needed
        if (gev.pose == "open_palm") gestures_armed = true;

        if (gestures_armed && gev.pose == "fist") {
            if (fist_start_s < 0) { fist_start_s = now_s(); fist_triggered = false; }
            else if (!fist_triggered && now_s() - fist_start_s > FIST_HOLD_SECONDS) {
                ori_offset = yaw_twist(head_q);
                place_rack();
                printf("gesture recenter (fist-hold)\n");
                tele.log("info", "recentered");
                fist_triggered = true;
                gestures_armed = false;                // one gesture per open-hand arm
            }
            was_pinching = false;
        } else if (gestures_armed && gev.pinching) {
            fist_start_s = -1;
            fist_triggered = false;
            if (was_pinching) {
                float dy = gev.pinch_y - pinch_prev_y; // image space: +y down
                if (multi) {
                    rack_dist_scale = std::clamp(
                        rack_dist_scale * (1.f - dy * PINCH_DISTANCE_SENSITIVITY * 0.5f),
                        0.25f, 4.f);
                } else {
                    float old_distance = distance;
                    distance = std::clamp(distance - dy * PINCH_DISTANCE_SENSITIVITY, 0.5f, 10.f);
                    scene[0].cfg.distance = distance;
                    (void)old_distance;
                }
            }
            pinch_prev_y = gev.pinch_y;
            was_pinching = true;
        } else {
            fist_start_s = -1;
            fist_triggered = false;
            // A pinch-drag that just ended is one completed gesture -> disarm.
            if (was_pinching) gestures_armed = false;
            was_pinching = false;
        }

        // ---- pose (predicted, then smoothed)
        float pose[7] = {0};
        if (get_gl_pose_carina(g_provider, pose, double(predict_ms) * 1e6) == 0) {
            Vec3 rp = { pose[0], pose[1], pose[2] };
            Quat rq = { pose[3], pose[4], pose[5], pose[6] };
            if (!have_pose) {
                head_p = rp;
                head_q = rq;
                have_pose = true;
                ori_offset = yaw_twist(head_q);
                place_rack();
            } else {
                // One-Euro position filter: the cutoff follows a SMOOTHED
                // speed estimate, so mm-level VIO noise spikes cannot open
                // the filter (no wiggle at rest) while sustained real motion
                // opens it within ~100 ms (little perceived lag).
                static float speed_hat = 0;
                const float Te = 1.f / 120.f;
                float dx = rp.x - head_p.x, dy = rp.y - head_p.y, dz = rp.z - head_p.z;
                float speed = std::sqrt(dx * dx + dy * dy + dz * dz) / Te; // m/s
                const float d_cutoff = 1.8f; // Hz — how fast the speed estimate reacts
                float ad = 1.f / (1.f + 1.f / (2.f * float(M_PI) * d_cutoff * Te));
                speed_hat += (speed - speed_hat) * ad;
                float min_cutoff = smooth_pos * 4.f; // default 0.10 → 0.4 Hz at rest
                // Deadband removes the VIO noise floor from the speed signal,
                // allowing a strong motion gain without rest wiggle.
                float speed_eff = std::max(0.f, speed_hat - 0.03f);
                float cutoff = min_cutoff + 9.f * speed_eff;
                float ap = 1.f / (1.f + 1.f / (2.f * float(M_PI) * cutoff * Te));
                head_p.x += dx * ap;
                head_p.y += dy * ap;
                head_p.z += dz * ap;
                // nlerp along the shortest arc, with speed-adaptive blending
                // (One-Euro style): near-still → heavy filtering kills the
                // shimmer; fast turns → near-raw so tracking stays snappy.
                float dot = head_q.w * rq.w + head_q.x * rq.x + head_q.y * rq.y + head_q.z * rq.z;
                float sign = dot < 0 ? -1.f : 1.f;
                float ang = 2.f * std::acos(std::min(1.f, std::fabs(dot))) * 180.f / float(M_PI);
                // Velocity term dominates quickly: shimmer-damping at rest,
                // near-raw tracking as soon as the head actually turns.
                float a = std::min(0.95f, smooth_ori * 0.25f + ang * 1.2f);
                head_q.w += (rq.w * sign - head_q.w) * a;
                head_q.x += (rq.x * sign - head_q.x) * a;
                head_q.y += (rq.y * sign - head_q.y) * a;
                head_q.z += (rq.z * sign - head_q.z) * a;
                float m = std::sqrt(head_q.w * head_q.w + head_q.x * head_q.x +
                                    head_q.y * head_q.y + head_q.z * head_q.z);
                if (m > 1e-6f) { head_q.w /= m; head_q.x /= m; head_q.y /= m; head_q.z /= m; }
            }
        }
        if (have_pose) {
            float tp[3] = { head_p.x, head_p.y, head_p.z };
            float tq[4] = { head_q.w, head_q.x, head_q.y, head_q.z };
            tele.send_pose(tp, tq, tnow);
        }

        // ---- 6DoF liveness over a ~1 s window
        if (have_pose) {
            if (win_n == 0) { win_min = win_max = head_p; win_q0 = head_q; win_max_ang = 0; }
            win_min.x = std::min(win_min.x, head_p.x); win_max.x = std::max(win_max.x, head_p.x);
            win_min.y = std::min(win_min.y, head_p.y); win_max.y = std::max(win_max.y, head_p.y);
            win_min.z = std::min(win_min.z, head_p.z); win_max.z = std::max(win_max.z, head_p.z);
            float d = std::fabs(win_q0.w * head_q.w + win_q0.x * head_q.x +
                                win_q0.y * head_q.y + win_q0.z * head_q.z);
            win_max_ang = std::max(win_max_ang,
                                   2.f * std::acos(std::min(1.f, d)) * 180.f / float(M_PI));
            if (++win_n >= 120) {
                float pos_range = std::max({ win_max.x - win_min.x, win_max.y - win_min.y,
                                             win_max.z - win_min.z });
                if (pos_range > 0.02f) sixdof_live = true;            // clear translation
                else if (win_max_ang > 8.f) sixdof_live = false;      // moving head, dead position
                win_n = 0;
            }
        }

        // ---- capture (--capture-hz, default 30; pose stays at panel rate)
        if (tnow - last_cap_t > 1.0 / capture_hz) {
            last_cap_t = tnow;
            if (cap && !cap->alive()) {
                fprintf(stderr, "capture: %s died — switching\n", cap->name());
                cap->stop();
                switch_backend();
            }
            CaptureFrame f{};
            if (cap && cap->latest_frame(f)) {
                cap_frames++;
                if (uint32_t(f.w) != vk.tex_w || uint32_t(f.h) != vk.tex_h ||
                    f.pitch != vk.tex_pitch) {
                    vkr_destroy_texture(vk);
                    if (!vkr_init_texture(vk, f.w, f.h, f.pitch)) {
                        fprintf(stderr, "capture texture rebuild failed\n");
                        g_running = false;
                    } else {
                        cap_aspect = float(f.w) / float(f.h);
                        tele.log("info", "capture source resized - texture rebuilt");
                    }
                }
                if (g_running) {
                    vkr_wait_uploads(vk);
                    vkr_upload(vk, f.data, size_t(f.pitch) * f.h);
                    cursor_under.valid = false;  // stamp overwritten by the fresh frame
                }
            }
            // Pointer overlay at the tick rate — content frames may arrive
            // slower (portal only sends on damage), but the cursor should
            // still move smoothly: restore the pixels under the previous
            // stamp, re-blend at the current position, re-copy to the GPU.
            if (g_running && cap && have_xfixes && vk.staging_ptr &&
                strcmp(cap->name(), "test") != 0) {
                int sx = 0, sy = 0;
                if (cursor_source_origin(outputs, capture_name, glasses.name,
                                         int(vk.tex_w), int(vk.tex_h), sx, sy)) {
                    uint8_t* st = static_cast<uint8_t*>(vk.staging_ptr);
                    vkr_wait_uploads(vk);
                    cursor_restore(cursor_under, st, vk.tex_pitch);
                    composite_cursor(dpy, st, int(vk.tex_w), int(vk.tex_h),
                                     vk.tex_pitch, sx, sy, cursor_under);
                    vk.tex_dirty = true;
                }
            }
        }

        // Extent changed (swapchain rebuilt — e.g. a panel-mode toggle under
        // the lease) — re-derive the per-eye projection and render path.
        if (vk.extent.width != known_extent.width || vk.extent.height != known_extent.height)
            refresh_projection();

        // ---- render
        // Draw-list caps tie together: config's 16-screen max + 2 HUD dots +
        // 21 hand landmarks = 39 <= 40. Bump any cap and this must grow too.
        QuadDraw draws[2][40];
        int ndraw[2] = {0, 0};
        if (have_pose && anchored) {
            // view = trim ⊗ inverse(recentered head pose)
            Quat head_rc = qmul(qconj(ori_offset), head_q);
            Vec3 hp = qrot(qconj(ori_offset), head_p);
            Quat view_q = qconj(qmul(head_rc, trim));
            Vec3 hp_neg = qrot(view_q, { -hp.x, -hp.y, -hp.z });

            // Build one eye's draw list. eye_off = view-space x shift (±IPD/2
            // in stereo, 0 in mono); it slides the camera and each head-locked
            // HUD element so both eyes fuse at their design depths.
            auto build_eye = [&](int eye, float eye_off) {
                QuadDraw* dl = draws[eye];
                int& nd = ndraw[eye];
                float view[16], proj[16];
                mat_from_pose(view_q, hp_neg, view);
                view[12] += eye_off;                    // camera ±IPD/2, view space
                mat_projection_vk(r, t, near_z, far_z, proj);

                const float white[4] = { 1, 1, 1, 1 };
                for (auto& s : scene) {
                    if (nd >= 40) break;
                    Quat sq; Vec3 sp;
                    scene_screen_pose(s, rack_q, rack_p, rack_dist_scale, sq, sp);
                    float model[16], vm[16], smvp[16];
                    mat_from_pose(sq, sp, model);
                    mat_mul(view, model, vm);
                    mat_mul(proj, vm, smvp);
                    float aspect = multi ? s.aspect : cap_aspect;
                    float diag_m = s.cfg.size * (multi ? rack_size_scale : 1.f) * 0.0254f;
                    float w2 = diag_m * aspect / std::sqrt(1 + aspect * aspect) * 0.5f;
                    float h2 = diag_m / std::sqrt(1 + aspect * aspect) * 0.5f;
                    QuadDraw& d = dl[nd++];
                    memcpy(d.mvp, smvp, sizeof(smvp));
                    memcpy(d.color, white, 4 * sizeof(float));
                    d.rect[0] = 0; d.rect[1] = 0; d.rect[2] = w2; d.rect[3] = h2;
                    memcpy(d.uv, s.uv, sizeof(s.uv));
                    d.textured = true;
                }
                // `mvp` below (HUD blocks) previously reused the screen's matrix
                // chain; the HUD only ever used proj-based matrices, which are
                // built inline there already — no change needed.

                // Head-locked tracking-status dot (replaced the old border
                // frame): blue = 6DoF live, orange = positional tracking frozen
                // (orientation-only). Fixed in the lower-right of the view —
                // mvp is projection * translate only, no world transform.
                const float live[4] = { 0.35f, 0.76f, 1.f, 1.f };
                const float frozen[4] = { 1.f, 0.55f, 0.2f, 1.f };
                const float DOT_Z = 0.5f;          // meters in front of the eye
                float tan_r = r / near_z, tan_t = t / near_z;
                float eye_m[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,
                                    0.95f * tan_r * DOT_Z + eye_off, -0.95f * tan_t * DOT_Z, -DOT_Z, 1 };
                QuadDraw& d = dl[nd++];
                mat_mul(proj, eye_m, d.mvp);
                memcpy(d.color, sixdof_live ? live : frozen, 4 * sizeof(float));
                float dot_r = 0.0045f * DOT_Z;     // ~0.5 degrees apparent size
                d.rect[0] = 0; d.rect[1] = 0; d.rect[2] = dot_r; d.rect[3] = dot_r;
                d.textured = false;
                d.circle = true;

                // Shared "pinch active" green — the pinch-status dot and the
                // fingertip highlight must use the same green (design: the two
                // feedback channels agree).
                const float status_green[4] = { 0.20f, 0.90f, 0.30f, 1.f };

                // Pinch/arm-status dot, just left of the VO tracking-status dot (at
                // x-factor 0.95). Only shown while the gesture pipeline is live (a
                // grey dot with no sidecar would falsely imply "running, no hand
                // seen"). States: grey = no hand, amber = hand seen but not armed,
                // blue = armed (open palm seen), green = armed + pinching.
                if (g_gestures.enabled()) {
                    const float grey[4]  = { 0.5f, 0.5f, 0.5f, 1.f };
                    const float amber[4] = { 1.f, 0.65f, 0.1f, 1.f };
                    const float blue[4]  = { 0.30f, 0.55f, 1.f, 1.f };
                    const float* pcol = !gev.present   ? grey
                                      : !gestures_armed ? amber
                                      : (gev.pinching ? status_green : blue);
                    float peye[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,
                                       0.90f * tan_r * DOT_Z + eye_off, -0.95f * tan_t * DOT_Z, -DOT_Z, 1 };
                    QuadDraw& pd = dl[nd++];
                    mat_mul(proj, peye, pd.mvp);
                    memcpy(pd.color, pcol, 4 * sizeof(float));
                    pd.rect[0] = 0; pd.rect[1] = 0; pd.rect[2] = dot_r; pd.rect[3] = dot_r;
                    pd.textured = false;
                    pd.circle = true;
                }

                // Hand-landmark overlay: 21 dots in a head-locked lower-left panel,
                // shown only while a hand is actually seen. One shared panel mvp;
                // each landmark is a different rect center (the quad shader builds
                // the quad from rect.xy ± rect.zw), so no per-dot matrix. Thumb tip
                // (4) and index tip (8) — the pair the pinch is measured between —
                // are yellow, turning green while pinching.
                if (g_gestures.enabled() && gev.present && gev.has_landmarks) {
                    int cw = g_cam_w.load(std::memory_order_relaxed);
                    int ch = g_cam_h.load(std::memory_order_relaxed);
                    float aspect = (cw > 0 && ch > 0) ? float(cw) / float(ch) : 4.f / 3.f;
                    const float PANEL_H = 0.09f * DOT_Z;        // half-height (~10° tall)
                    const float PANEL_W = PANEL_H * aspect;     // aspect-preserved half-width
                    float leye[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,
                                       -0.55f * tan_r * DOT_Z + eye_off, -0.45f * tan_t * DOT_Z, -DOT_Z, 1 };
                    float panel_mvp[16];
                    mat_mul(proj, leye, panel_mvp);
                    const float lm_col[4] = { 0.40f, 0.90f, 1.00f, 1.f }; // soft cyan
                    const float tip[4]    = { 1.00f, 0.85f, 0.10f, 1.f }; // yellow (thumb/index tip)
                    const float lm_r = 0.0033f * DOT_Z;  // ~0.35 degrees apparent size (very thin)
                    // Unarmed: the whole hand is drawn mildly transparent; arming
                    // (open palm) makes it opaque. Fingertips go green only when the
                    // pinch is actually actionable (armed && pinching).
                    const float ov_alpha = gestures_armed ? 1.f : 0.45f;
                    for (int i = 0; i < 21 && nd < 40; i++) {
                        float nx = gev.landmarks[i][0];
                        float ny = gev.landmarks[i][1];
                        // Normalized image coords -> panel-local. Image y is down,
                        // eye-space y is up, so negate y. x is mapped directly; if
                        // the hand reads left-right mirrored on hardware, change
                        // (nx - 0.5f) to (0.5f - nx) here.
                        float lx =  (nx - 0.5f) * 2.f * PANEL_W;
                        float ly = -(ny - 0.5f) * 2.f * PANEL_H;
                        const float* base = (i == 4 || i == 8)
                                              ? ((gestures_armed && gev.pinching) ? status_green : tip)
                                              : lm_col;
                        float col[4] = { base[0], base[1], base[2], ov_alpha };
                        QuadDraw& ld = dl[nd++];
                        memcpy(ld.mvp, panel_mvp, sizeof(panel_mvp));
                        memcpy(ld.color, col, 4 * sizeof(float));
                        ld.rect[0] = lx; ld.rect[1] = ly; ld.rect[2] = lm_r; ld.rect[3] = lm_r;
                        ld.textured = false;
                        ld.circle = true;
                    }
                }
            };
            if (stereo_active) {
                build_eye(0, stereo_eye_offset(ipd_m, 0));
                build_eye(1, stereo_eye_offset(ipd_m, 1));
            } else {
                build_eye(0, 0.f);
            }
        }
        static int draw_fail = 0;
        // Stereo passes both stack lists (draws[1] is non-null even when
        // empty — a null right list would silently downgrade to mono).
        bool drew = stereo_active
            ? vkr_draw_stereo(vk, draws[0], ndraw[0], draws[1], ndraw[1])
            : vkr_draw(vk, draws[0], ndraw[0]);
        if (drew) {
            draw_fail = 0;
            frames++;
        } else if (++draw_fail >= 120) {
            fprintf(stderr, "presentation failed repeatedly — shutting down\n");
            g_running = false;
        }
        if (tnow - last_fps_t >= 2.0) {
            last_fps = float(frames / (tnow - last_fps_t));
            last_cap_fps = int(cap_frames / (tnow - last_fps_t) + 0.5);
            // Multi-screen has no single distance/size — report the rack scale
            // multipliers instead.
            printf(multi
                   ? "fps %.0f  cap %d/s  pose %s  6dof %s  head [%+.3f %+.3f %+.3f]m  rack-dist x%.2f  rack-size x%.2f\n"
                   : "fps %.0f  cap %d/s  pose %s  6dof %s  head [%+.3f %+.3f %+.3f]m  dist %.2fm  size %.0f\"\n",
                   last_fps, last_cap_fps, have_pose ? "ok" : "waiting",
                   sixdof_live ? "LIVE" : "frozen",
                   head_p.x, head_p.y, head_p.z,
                   multi ? rack_dist_scale : distance,
                   multi ? rack_size_scale : diag_in);
            frames = 0;
            cap_frames = 0;
            last_fps_t = tnow;

            // ---- RSS leak watchdog: sampled on the fps cadence, each
            // threshold latched to fire once.
            rss_mb = sample_rss_mb();
            if (!rss_warned && rss_mb > 2048) {
                rss_warned = true;
                fprintf(stderr, "spatial-screens: RSS %d MB — possible leak\n", rss_mb);
                char msg[128];
                snprintf(msg, sizeof(msg), "spatial-screens: RSS %d MB — possible leak", rss_mb);
                tele.log("warn", msg);
            }
            if (!rss_critical && rss_mb > 8192) {
                rss_critical = true;
                fprintf(stderr, "spatial-screens: RSS %d MB exceeds 8 GB — shutting down to avoid OOM kill\n",
                        rss_mb);
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "spatial-screens: RSS %d MB exceeds 8 GB — shutting down to avoid OOM kill", rss_mb);
                tele.log("error", msg);
                g_running = false;
            }
        }
        tele.send_hello(market, g_pid, fw, XR_DEVICE_TYPE_VITURE_CARINA);
        // Multi mode overloads distance/size with the rack scale multipliers
        // (see Telemetry::send_app); single-screen sends meters/inches.
        tele.send_app(last_fps, sixdof_live, anchored,
                      multi ? rack_dist_scale : distance,
                      multi ? rack_size_scale : diag_in,
                      cap ? cap->name() : "none", sout.direct, rss_mb,
                      stereo_active, int(scene.size()));
    }

    if (multi) {
        app_state.rack_distance_scale = rack_dist_scale;
        app_state.rack_size_scale = rack_size_scale;
    } else {
        app_state.distance = distance;
        app_state.size = diag_in;
    }
    save_state(app_state);
    printf("shutting down…\n");
    g_gestures.stop();
    tele.stop();
    if (cap) cap->stop();
    vkr_destroy_device(vk);                        // swapchain/surface/device first
    if (sout.direct) direct_release(vk.instance);  // drop the lease (instance teardown never does)
    vkr_destroy(vk);
    if (sout.direct) direct_restore(dpy);          // now the server can re-enable the output
    else if (sout.window) XDestroyWindow(dpy, sout.window);
    sdk_shutdown();                                // panel back to 2D, then SDK down
    XCloseDisplay(dpy);
    return 0;
}
