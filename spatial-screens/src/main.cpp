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
//        Gestures (if the sidecar connects): open palm = arm that hand;
//        with a hand armed, pinch-drag vertical = distance (anchor stays
//        fixed), fist held ~0.5s = recenter — either hand works. With BOTH
//        hands armed and pinching: spread = resize, midpoint = reposition.
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
#include <execinfo.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <unistd.h>
#include <vector>

#include "viture_glasses_provider.h"
#include "viture_device_carina.h"
#include "vk_renderer.h"
#include "pose_math.h"
#include "predict_math.h"
#include "vk_surface.h"
#include "gesture_client.h"
#include "gesture_manip.h"
#include "capture.h"
#include "cursor_overlay.h"
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
static std::atomic<double> g_vsync_last{0.0};      // steady_clock secs of last vsync
static std::atomic<double> g_vsync_interval{0.0};  // EMA of inter-vsync seconds
static float g_last_predict_ms = 0.f;              // live prediction horizon, gated; written each frame below
static float g_last_svel_deg = 0.f;                // smoothvel: live lead angle applied (deg), 0 in other modes
static constexpr float PINCH_DISTANCE_SENSITIVITY = 4.0f; // tune after hands-on test; higher = faster response to hand motion
static constexpr double FIST_HOLD_SECONDS = 0.5;          // how long a fist must be held before it triggers recenter
static constexpr float GRAB_REPOSITION_GAIN = 1.5f; // image-fraction -> world-fraction; tune on hardware
static constexpr float GRAB_DIAG_MIN = 20.f;        // inches
static constexpr float GRAB_DIAG_MAX = 200.f;       // inches
static constexpr float SELECT_CONE_DEG = 40.f;  // gaze cone half-angle for pick
static constexpr float SELECT_BORDER_M = 0.003f; // green border thickness (m) — thin frame (hardware-tuned 2026-07-06)

static void on_imu_noop(float*, double) {}
static void on_pose_noop(float*, double) {}

static double mono_now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static void on_vsync(double /*sdk_ts*/) {
    // Stamp with OUR monotonic clock, not the SDK timestamp: the render loop
    // compares against mono_now_s(), so both must share one timebase.
    double now = mono_now_s();
    double prev = g_vsync_last.load(std::memory_order_relaxed);
    if (prev > 0.0) {
        float iv = vsync_interval_update(g_vsync_interval.load(std::memory_order_relaxed),
                                         float(now - prev));
        g_vsync_interval.store(iv, std::memory_order_relaxed);
    }
    g_vsync_last.store(now, std::memory_order_relaxed);
}

static void on_camera_carina(char* image_left0, char* image_right0,
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
    g_gestures.maybe_send_frame(reinterpret_cast<uint8_t*>(image_left0),
                                reinterpret_cast<uint8_t*>(image_right0),
                                width, height, timestamp);
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
    if (register_callbacks_carina(g_provider, on_pose_noop, on_vsync, on_imu_noop, on_camera_carina) != 0 ||
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

// Crash backtrace: on a fatal fault, dump the C++ stack to stderr
// (async-signal-safe backtrace_symbols_fd) then re-raise the default handler so
// it still core-dumps / exits with the signal. Diagnostic aid for the two-hand
// hardware bring-up; build with -g -rdynamic for readable frames.
static void on_crash(int sig) {
    void* frames[32];
    int n = backtrace(frames, 32);
    const char msg[] = "\n*** spatial-screens crash — backtrace ***\n";
    ssize_t wr = write(STDERR_FILENO, msg, sizeof(msg) - 1);
    (void)wr;
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    signal(sig, SIG_DFL);
    raise(sig);
}

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
    bool fusion = true;
    bool both_cam = true;
    bool gestures_enabled = true;   // --no-gestures skips the sidecar (fps A/B: isolates iGPU contention)
    bool reproject = false;         // --reproject: late-latch (acquire before pose sample), cuts FIFO-wait latency
    std::string gesture_enhance = "gamma_clahe";
    float gesture_enhance_gamma = 0.45f, gesture_enhance_clahe_clip = 3.0f;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        bool ok = false;
        if (!strcmp(a, "--fusion")) { fusion = true; continue; }
        if (!strcmp(a, "--no-fusion")) { fusion = false; continue; }
        if (!strcmp(a, "--no-both-cam")) { both_cam = false; continue; }
        if (!strcmp(a, "--no-gestures")) { gestures_enabled = false; continue; }
        if (!strcmp(a, "--reproject")) { reproject = true; continue; }
        if (!strcmp(a, "--enhance") && i + 1 < argc) { gesture_enhance = argv[++i]; continue; }
        if (!strcmp(a, "--enhance-gamma") && i + 1 < argc) { gesture_enhance_gamma = atof(argv[++i]); continue; }
        if (!strcmp(a, "--enhance-clahe-clip") && i + 1 < argc) { gesture_enhance_clahe_clip = atof(argv[++i]); continue; }
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
                   "[--size IN] [--pitch-trim DEG] [--predict-ms MS] "
                   "[--predict-mode off|fixed|vsync|smoothvel] [--scanout-ms MS] [--predict-cap-ms MS] "
                   "[--predict-scale 0..1] [--ang-dead DEG] [--ang-ramp DEG] [--ori-motion-cap 0..1] [--vel-cutoff HZ] "
                   "[--smooth-pos 0..1] "
                   "[--smooth-ori 0..1] [--ws-port N] [--window] [--config PATH] "
                   "[--dump-config] [--probe-camera] [--no-fusion] [--fusion] "
                   "[--no-both-cam] [--enhance MODE] [--enhance-gamma F] [--enhance-clahe-clip F]\n"
                   "  --no-fusion   disable stereo depth fusion (fusion is ON by default)\n"
                   "  --fusion      force-enable stereo depth fusion (default)\n"
                   "  --no-both-cam disable both-camera tracking union (fusion feeds depth only)\n"
                   "  --no-gestures skip the MediaPipe gesture sidecar entirely (fps A/B)\n"
                   "  --reproject   late-latch: sample head pose AFTER the vblank-paced acquire (lower latency)\n"
                   "  --enhance     hand-frame brightening: none|gamma|clahe|gamma_clahe (default gamma_clahe)\n"
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
        printf("predict-mode = %s\nscanout-ms = %.2f\npredict-cap-ms = %.2f\n",
               o.predict_mode.c_str(), o.scanout_ms, o.predict_cap_ms);
        printf("predict-scale = %.2f\nang-dead = %.2f\nang-ramp = %.2f\nori-motion-cap = %.2f\n",
               o.predict_scale, o.ang_dead, o.ang_ramp, o.ori_motion_cap);
        printf("vel-cutoff = %.2f\n", o.vel_cutoff);
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
    std::string predict_mode = o.predict_mode;
    float scanout_ms = o.scanout_ms, predict_cap_ms = o.predict_cap_ms;
    float predict_scale = o.predict_scale, ang_dead = o.ang_dead, ang_ramp = o.ang_ramp;
    float ori_motion_cap = o.ori_motion_cap;
    float vel_cutoff = o.vel_cutoff;
    bool force_window = o.window;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGSEGV, on_crash);
    signal(SIGABRT, on_crash);

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
    //
    // run.sh applies the stereo workspace only AFTER the lease settles the
    // layout (mutter reapplies its stored config on both the SBS adopt and
    // the lease, clobbering anything set earlier), so the VS monitors and
    // the panel's scaled rect land a beat after this point — wait for the
    // configured grid, re-resolving the capture rect every tick.
    int want_tiles = 0;
    if (!force_window && o.workspace.size() == 3 && o.workspace[1] == 'x' &&
        o.workspace[0] >= '1' && o.workspace[0] <= '9' &&
        o.workspace[2] >= '1' && o.workspace[2] <= '9')
        want_tiles = (o.workspace[0] - '0') * (o.workspace[2] - '0');

    // Capture source rect = what the xshm backend grabs (the whole capture
    // output). UVs slice it; test/portal frames are sliced the same way.
    OutputRect cap_src{};
    std::vector<MonRect> vs_mons;
    for (int waited = 0;; waited += 250) {
        outputs = list_outputs(dpy);
        cap_src = {};
        for (auto& oo : outputs)
            if (!capture_name.empty() ? oo.name == capture_name
                                      : oo.name != glasses.name) { cap_src = oo; break; }
        vs_mons.clear();
        for (auto& m : list_monitors(dpy)) {
            bool inside = m.x >= cap_src.x && m.y >= cap_src.y &&
                          m.x + m.w <= cap_src.x + cap_src.w &&
                          m.y + m.h <= cap_src.y + cap_src.h;
            if (m.name.rfind("VS", 0) == 0 && inside)
                vs_mons.push_back({m.name, m.x, m.y, m.w, m.h});
        }
        if (want_tiles <= 1 || (int)vs_mons.size() >= want_tiles) break;
        if (waited >= 10000) {
            fprintf(stderr, "scene: workspace %s never appeared (%zu/%d tiles) — "
                            "continuing without it\n",
                    o.workspace.c_str(), vs_mons.size(), want_tiles);
            break;
        }
        if (!waited) printf("scene: waiting for workspace monitors (%s grid)…\n",
                            o.workspace.c_str());
        usleep(250 * 1000);
    }
    MonRect fb_rect{cap_src.name, cap_src.x, cap_src.y, cap_src.w, cap_src.h};
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
                if (found) b = capture_create_xshm(src, capture_hz);
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
    if (gestures_enabled) {
        g_gestures.start(gesture_socket, executable_dir() + "/gestures/hand_tracker.py",
                         5.0, fusion, both_cam, gesture_enhance,
                         gesture_enhance_gamma, gesture_enhance_clahe_clip);
        tele.log("info", g_gestures.enabled() ? "gesture sidecar connected"
                                              : "gesture sidecar unavailable");
    } else {
        tele.log("info", "gesture sidecar disabled (--no-gestures)");
    }

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
        // Direct: stereo iff the scanned-out mode is SBS-wide. Window: SBS
        // halves are only correct on an actually-SBS output — deliberate window
        // stereo (--window debug) or a succeeded panel switch (g_sbs_orig >= 0).
        // Otherwise the halves would squish into a 2D panel; spec §4 → mono.
        stereo_active = o.stereo &&
            (sout.direct ? vk.extent.width >= 2 * vk.extent.height
                         : (force_window || g_sbs_orig >= 0));
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

    int active_screen = -1;              // -1 = none (rack-global); else scene idx

    // Move the active screen toward/away along the line from the USER (head) to
    // the screen — "distance from me", not from the rack origin — clamped to a
    // comfortable range. Orientation is preserved. No-op if nothing is selected.
    auto scale_active_distance = [&](float f) {
        if (active_screen < 0) return;
        Quat sq; Vec3 sp;
        scene_screen_pose(scene[active_screen], rack_q, rack_p, rack_dist_scale, sq, sp);
        Vec3 hp = qrot(qconj(ori_offset), head_p);   // head in the rack/world frame
        Vec3 d = { sp.x - hp.x, sp.y - hp.y, sp.z - hp.z };
        float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len < 1e-6f) return;
        float nlen = std::clamp(len * f, 0.25f, 10.f);
        float s = nlen / len;
        Vec3 wp = { hp.x + d.x * s, hp.y + d.y * s, hp.z + d.z * s };
        world_to_rack_frame(rack_q, rack_p, sq, wp,
                            scene[active_screen].cfg.pose_ori,
                            scene[active_screen].cfg.pose_pos);
        scene[active_screen].cfg.has_pose_override = true;
    };

    auto now_s = [] {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    };
    double last_fps_t = now_s(), last_cap_t = 0;
    int frames = 0, cap_frames = 0, last_cap_fps = 0;
    float last_fps = 0;
    double last_mode_poll_t = 0; int last_polled_mode = -1;
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
    // Per-hand arming + single-hand drag state (index 0 = left, 1 = right).
    bool armed[2] = {false, false};
    bool was_pinching[2] = {false, false};
    float pinch_prev_y[2] = {0.f, 0.f};
    double fist_start_s[2] = {-1.0, -1.0};
    bool fist_triggered[2] = {false, false};
    bool was_two_up[2] = {false, false}; // rising-edge latch per hand for select
    double two_fist_start_s = -1.0;      // both-hands-fist global-recenter hold timer
    bool two_fist_triggered = false;
    // Two-hand grab (resize + reposition).
    GrabState grab;
    float grab_scale0 = 1.f;   // rack_size_scale snapshot at grab start (rack mode)
    Vec3 grab_rel0;            // grabbed anchor's offset in the head-local frame at grab start
    Quat grab_ori0;            // active screen's WORLD orientation at grab start
    Quat grab_head_q0;         // head orientation (recentered frame) at grab start — grab follows head pose

    printf("running — hotkeys work globally with Ctrl+Alt: R recenter (Shift adds "
           "VIO reset), [ ] distance, - = size, Q quit\n"
           "gestures (if sidecar connected): open palm=arm (either hand); "
           "index+middle (two-up) while looking at a screen=select it; armed hand "
           "pinch-drag vertical=distance, fist-hold(0.5s)=recenter selected screen "
           "(or rack if none); both hands armed+pinching: spread=resize, "
           "midpoint=reposition; both fists(0.5s)=global recenter\n");

    static float dt_ring[256]; int dt_ring_n = 0, dt_ring_pos = 0;
    float dt_mean_ema = 1.f / 90.f;
    float speed_hat = 0.f;       // smoothed linear speed (m/s), One-Euro
    float ang_speed_hat = 0.f;   // smoothed angular speed (deg/frame)
    Quat sv_prev_q;              // smoothvel: previous filtered head_q (angular-velocity estimate)
    Vec3 sv_omega{ 0, 0, 0 };    // smoothvel: smoothed world-frame angular velocity (rad/s)
    bool sv_have_prev = false;

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
                if (active_screen >= 0) scale_active_distance(0.9f);
                else if (multi) rack_dist_scale = std::max(0.25f, rack_dist_scale * 0.9f);
                else { distance = std::max(0.5f, distance - 0.25f); scene[0].cfg.distance = distance; }
                place_rack();
            }
            else if (ks == XK_bracketright) {
                if (active_screen >= 0) scale_active_distance(1.1f);
                else if (multi) rack_dist_scale = std::min(4.f, rack_dist_scale * 1.1f);
                else { distance = std::min(10.f, distance + 0.25f); scene[0].cfg.distance = distance; }
                place_rack();
            }
            else if (ks == XK_minus) {
                if (active_screen >= 0)
                    scene[active_screen].cfg.size = std::max(10.f, scene[active_screen].cfg.size - 10.f);
                else if (multi) rack_size_scale = std::max(0.4f, rack_size_scale * 0.9f);
                else { diag_in = std::max(10.f, diag_in - 10.f); scene[0].cfg.size = diag_in; }
            }
            else if (ks == XK_equal) {
                if (active_screen >= 0)
                    scene[active_screen].cfg.size = std::min(400.f, scene[active_screen].cfg.size + 10.f);
                else if (multi) rack_size_scale = std::min(3.f, rack_size_scale * 1.1f);
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

        // ---- informational display-mode poll (1 Hz). NEVER gates rendering:
        // get_display_mode lags a full command cycle (spike finding #1).
        if (sout.direct && tnow - last_mode_poll_t > 1.0) {
            last_mode_poll_t = tnow;
            int m = xr_device_provider_get_display_mode(g_provider);
            if (m >= 0 && m != last_polled_mode) {
                if (last_polled_mode >= 0) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "panel mode now 0x%02x (informational)", m);
                    tele.log("info", msg);
                }
                last_polled_mode = m;
            }
        }

        // ---- gestures (two hands)
        // Per-hand arming: each open palm arms its own hand. A single-hand
        // gesture needs that hand armed; the two-hand grab needs both. Each
        // completed gesture disarms the hand(s) it used. See
        // docs/specs/2026-07-06-two-hand-gestures-design.md.
        GestureEvent gev = g_gestures.poll();
        HandState* hands[2] = { &gev.left, &gev.right };
        for (int i = 0; i < 2; i++) {
            if (!hands[i]->present) { armed[i] = false; was_two_up[i] = false; }
            if (hands[i]->pose == "open_palm") armed[i] = true;
        }
        // Defensive: a screen count can shrink (future feature) — never index OOB.
        if (active_screen >= int(scene.size())) active_screen = -1;

        bool both_ready = gev.left.present && gev.right.present && armed[0] && armed[1];
        // Both fists (held) = GLOBAL recenter. A closed fist can also register as
        // pinching, so exclude the both-fist case from the pinch-grab and let the
        // two-fist gesture take priority over it.
        bool two_fist_now = both_ready && gev.left.pose == "fist" && gev.right.pose == "fist";
        bool grab_now = both_ready && gev.left.pinching && gev.right.pinching && !two_fist_now;
        if (!two_fist_now) { two_fist_start_s = -1; two_fist_triggered = false; }

        if (two_fist_now) {
            if (two_fist_start_s < 0) { two_fist_start_s = now_s(); two_fist_triggered = false; }
            else if (!two_fist_triggered && now_s() - two_fist_start_s > FIST_HOLD_SECONDS) {
                ori_offset = yaw_twist(head_q);
                place_rack();
                printf("gesture recenter (two-fist, global)\n");
                tele.log("info", "recentered (global)");
                two_fist_triggered = true;
                armed[0] = armed[1] = false;
            }
            grab.active = false;   // both fists supersede any pinch-grab in progress
            for (int i = 0; i < 2; i++) {
                was_pinching[i] = false; fist_start_s[i] = -1;
                fist_triggered[i] = false; was_two_up[i] = false;
            }
        } else if (grab_now) {
            if (!grab.active) {
                Vec3 rr = qrot(rack_q, { 1, 0, 0 });
                Vec3 uu = qrot(rack_q, { 0, 1, 0 });
                float size0 = diag_in;
                Vec3 anchor0 = { rack_p.x, rack_p.y, rack_p.z };
                if (active_screen >= 0) {
                    Quat sq; Vec3 sp;
                    scene_screen_pose(scene[active_screen], rack_q, rack_p,
                                      rack_dist_scale, sq, sp);
                    anchor0 = sp;
                    size0 = scene[active_screen].cfg.size;
                    grab_ori0 = sq;   // weld reference
                }
                grab = grab_begin(gev.left.pinch_x, gev.left.pinch_y,
                                  gev.right.pinch_x, gev.right.pinch_y,
                                  size0, { anchor0.x, anchor0.y, anchor0.z },
                                  { rr.x, rr.y, rr.z }, { uu.x, uu.y, uu.z });
                grab_scale0 = rack_size_scale;   // baseline for rack-mode resize
                // Head-anchored grab: store the anchor's offset in the HEAD-local
                // frame at grab start, so the grab follows the head pose (look/lean
                // and the screen comes with you); hand motion nudges it on top.
                Quat head_rc0 = qmul(qconj(ori_offset), head_q);
                grab_head_q0 = head_rc0;   // head-delta baseline
                Vec3 hp0 = qrot(qconj(ori_offset), head_p);
                Vec3 off0 = { anchor0.x - hp0.x, anchor0.y - hp0.y, anchor0.z - hp0.z };
                grab_rel0 = qrot(qconj(head_rc0), off0);
            } else {
                GrabResult gr = grab_update(grab, gev.left.pinch_x, gev.left.pinch_y,
                                            gev.right.pinch_x, gev.right.pinch_y,
                                            distance, GRAB_REPOSITION_GAIN,
                                            GRAB_DIAG_MIN, GRAB_DIAG_MAX);
                if (active_screen >= 0) {
                    // Reposition + resize the ONE active screen. HEAD-ANCHORED:
                    // rebuild the anchor from the CURRENT head pose + the stored
                    // head-local offset, nudged by the pinch-midpoint delta in the
                    // head's right/up plane, so the screen follows head motion
                    // (look/lean and it comes with you) while hands fine-tune it.
                    // gr.anchor is unused here; gr.diag (spread) drives size.
                    // Orientation is head-anchored too: the screen welds to the head's
                    // full rotation delta (yaw+pitch+roll) and world-locks on release —
                    // the orientation twin of the head-local position anchor above.
                    // Head-anchoring is gated to the active-screen branch because the
                    // rack origin sits ON the head (d0->0 there).
                    Quat head_rc = qmul(qconj(ori_offset), head_q);
                    Vec3 hp = qrot(qconj(ori_offset), head_p);
                    float d0 = std::sqrt(grab_rel0.x * grab_rel0.x +
                                         grab_rel0.y * grab_rel0.y + grab_rel0.z * grab_rel0.z);
                    float mid_x = 0.5f * (gev.left.pinch_x + gev.right.pinch_x);
                    float mid_y = 0.5f * (gev.left.pinch_y + gev.right.pinch_y);
                    float dmx = mid_x - grab.mid0x, dmy = mid_y - grab.mid0y;
                    Vec3 rel = { grab_rel0.x + dmx * GRAB_REPOSITION_GAIN * d0,
                                 grab_rel0.y - dmy * GRAB_REPOSITION_GAIN * d0,
                                 grab_rel0.z };
                    Vec3 rw = qrot(head_rc, rel);
                    Vec3 anchor = { hp.x + rw.x, hp.y + rw.y, hp.z + rw.z };
                    Quat world_ori = head_delta_orient(grab_ori0, grab_head_q0, head_rc);
                    world_to_rack_frame(rack_q, rack_p, world_ori, anchor,
                                        scene[active_screen].cfg.pose_ori,
                                        scene[active_screen].cfg.pose_pos);
                    scene[active_screen].cfg.has_pose_override = true;  // already true; explicit
                    scene[active_screen].cfg.size = gr.diag;
                } else {
                    // Rack-global (nothing selected): hand-driven reposition in the
                    // rack's right/up plane (world-anchored) — the pre-tweak behavior.
                    rack_p = { gr.anchor.x, gr.anchor.y, gr.anchor.z };
                    if (multi) {
                        float ratio = gr.diag / std::max(1e-3f, grab.size0);
                        rack_size_scale = std::clamp(grab_scale0 * ratio, 0.4f, 3.f);
                    } else {
                        diag_in = gr.diag;
                        scene[0].cfg.size = diag_in;
                    }
                }
            }
            // Suppress single-hand logic and reset its per-hand run-state so it
            // re-seeds cleanly if a hand later acts alone.
            for (int i = 0; i < 2; i++) {
                was_pinching[i] = false;
                fist_start_s[i] = -1;
                fist_triggered[i] = false;
            }
        } else {
            if (grab.active) {
                grab.active = false;   // grab ended -> both hands must re-arm
                armed[0] = armed[1] = false;
            }
            // Single-hand gestures on the first qualifying armed hand
            // (fist-hold recenter takes priority over pinch-drag distance).
            for (int i = 0; i < 2; i++) {
                HandState& h = *hands[i];
                if (armed[i] && h.pose == "two_up") {
                    if (!was_two_up[i]) {           // rising edge only
                        // Recentered head frame — matches the screens' world frame.
                        Quat head_rc = qmul(qconj(ori_offset), head_q);
                        Quat head_pick = qmul(head_rc, trim);  // match render camera (view applies trim); else pick is pitch_trim off
                        Vec3 hp = qrot(qconj(ori_offset), head_p);
                        std::vector<Vec3> spos(scene.size());
                        for (size_t k = 0; k < scene.size(); k++) {
                            Quat sq; Vec3 sp;
                            scene_screen_pose(scene[k], rack_q, rack_p,
                                              rack_dist_scale, sq, sp);
                            spos[k] = sp;
                        }
                        int pick = pick_gaze_screen(spos, hp, head_pick, SELECT_CONE_DEG);
                        active_screen = pick;        // -1 doubles as deselect
                        if (pick >= 0 && !scene[pick].cfg.has_pose_override) {
                            if (multi) scene[pick].cfg.size *= rack_size_scale;  // fold rack scale in -> no size jump when we detach it below
                            // Seed the override from the formula pose so retarget
                            // gestures have a well-defined pose and there's no jump.
                            Quat wq; Vec3 wp;
                            scene_screen_pose(scene[pick], rack_q, rack_p,
                                              rack_dist_scale, wq, wp);
                            world_to_rack_frame(rack_q, rack_p, wq, wp,
                                                scene[pick].cfg.pose_ori,
                                                scene[pick].cfg.pose_pos);
                            scene[pick].cfg.has_pose_override = true;
                        }
                        printf("gesture select -> screen %d\n", active_screen);
                        tele.log("info", pick >= 0 ? "screen selected" : "screen deselected");
                        armed[i] = false;            // one action per open-hand arm
                    }
                    was_two_up[i] = true;
                    was_pinching[i] = false;
                    fist_start_s[i] = -1;
                    break;                            // this hand owns the frame
                }
                was_two_up[i] = false;
                if (armed[i] && h.pose == "fist") {
                    if (fist_start_s[i] < 0) { fist_start_s[i] = now_s(); fist_triggered[i] = false; }
                    else if (!fist_triggered[i] && now_s() - fist_start_s[i] > FIST_HOLD_SECONDS) {
                        if (active_screen >= 0) {
                            // Per-screen recenter: bring the active screen directly in
                            // front of the user, facing them, keeping its current
                            // distance from the head. Writes the override (rack frame).
                            Quat head_rc = qmul(qconj(ori_offset), head_q);
                            Quat face = qmul(head_rc, trim);       // matches render forward
                            Vec3 hp = qrot(qconj(ori_offset), head_p);
                            Vec3 fw = qrot(face, { 0, 0, -1 });
                            Quat cq; Vec3 cp;
                            scene_screen_pose(scene[active_screen], rack_q, rack_p,
                                              rack_dist_scale, cq, cp);
                            float ddx = cp.x - hp.x, ddy = cp.y - hp.y, ddz = cp.z - hp.z;
                            float d = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
                            if (d < 0.3f) d = 0.75f;               // guard: don't collapse onto the head
                            Vec3 wp = { hp.x + fw.x * d, hp.y + fw.y * d, hp.z + fw.z * d };
                            world_to_rack_frame(rack_q, rack_p, face, wp,
                                                scene[active_screen].cfg.pose_ori,
                                                scene[active_screen].cfg.pose_pos);
                            scene[active_screen].cfg.has_pose_override = true;
                            printf("gesture recenter (fist-hold, screen %d)\n", active_screen);
                            tele.log("info", "screen recentered");
                        } else {
                            ori_offset = yaw_twist(head_q);
                            place_rack();
                            printf("gesture recenter (fist-hold, global)\n");
                            tele.log("info", "recentered");
                        }
                        fist_triggered[i] = true;
                        armed[i] = false;      // one gesture per open-hand arm
                    }
                    was_pinching[i] = false;
                    break;                     // this hand owns the gesture this frame
                } else if (armed[i] && h.pinching) {
                    fist_start_s[i] = -1;
                    fist_triggered[i] = false;
                    if (was_pinching[i]) {
                        float dy = h.pinch_y - pinch_prev_y[i]; // image space: +y down
                        if (active_screen >= 0) {
                            scale_active_distance(1.f - dy * PINCH_DISTANCE_SENSITIVITY * 0.5f);
                        } else if (multi) {
                            rack_dist_scale = std::clamp(
                                rack_dist_scale * (1.f - dy * PINCH_DISTANCE_SENSITIVITY * 0.5f),
                                0.25f, 4.f);
                        } else {
                            distance = std::clamp(distance - dy * PINCH_DISTANCE_SENSITIVITY, 0.5f, 10.f);
                            scene[0].cfg.distance = distance;
                        }
                    }
                    pinch_prev_y[i] = h.pinch_y;
                    was_pinching[i] = true;
                    break;
                } else {
                    fist_start_s[i] = -1;
                    fist_triggered[i] = false;
                    if (was_pinching[i]) armed[i] = false; // completed pinch-drag -> disarm
                    was_pinching[i] = false;
                }
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
                !cap->composites_cursor() && strcmp(cap->name(), "test") != 0) {
                int sx = 0, sy = 0;
                if (cursor_source_origin(outputs, capture_name, glasses.name,
                                         int(vk.tex_w), int(vk.tex_h), sx, sy)) {
                    uint8_t* st = static_cast<uint8_t*>(vk.staging_ptr);
                    vkr_wait_uploads(vk);
                    cursor_restore(cursor_under, st, vk.tex_pitch);
                    composite_cursor(dpy, st, int(vk.tex_w), int(vk.tex_h),
                                     vk.tex_pitch, sx, sy, &cursor_under);
                    vk.tex_dirty = true;
                }
            }
        }

        // Extent changed (swapchain rebuilt — e.g. a panel-mode toggle under
        // the lease) — re-derive the per-eye projection and render path.
        if (vk.extent.width != known_extent.width || vk.extent.height != known_extent.height)
            refresh_projection();

        // Late-latch reprojection: acquire the swapchain image (blocks ~to the
        // next vblank) BEFORE sampling the head pose, so the presented frame's
        // pose is sampled ACROSS the FIFO wait instead of predicted across it.
        // On success this MUST be followed by exactly one vkr_submit_stereo with
        // no continue/return between (verified none exists down to the draw).
        bool frame_began = false;
        if (reproject) frame_began = vkr_begin_frame(vk);

        // ---- pose (predicted, then smoothed)
        // Real per-frame sample period for the One-Euro filter (and, later,
        // prediction). Clamped so a stall or a burst can't destabilize alpha.
        static double last_pose_now = -1.0;
        double pose_now = mono_now_s();
        float dt = (last_pose_now < 0.0)
                 ? (1.f / 90.f)
                 : std::clamp(float(pose_now - last_pose_now), 1.f / 240.f, 1.f / 30.f);
        last_pose_now = pose_now;
        dt_ring[dt_ring_pos] = dt; dt_ring_pos = (dt_ring_pos + 1) & 255;
        if (dt_ring_n < 256) dt_ring_n++;
        dt_mean_ema += (dt - dt_mean_ema) * 0.02f;

        // Prediction horizon (seconds), speed-gated so at rest it decays to ~0
        // (the failure mode of the earlier fixed --predict-ms). vsync mode times
        // it to the next scanout; fixed uses the configured predict-ms; off = 0.
        float gate = predict_gate(speed_hat, ang_speed_hat,
                                  /*lin_dead*/0.03f, /*lin_ramp*/0.30f,
                                  ang_dead, ang_ramp);
        double predict_s = 0.0;
        if (predict_mode != "off") {
            double cap_s = predict_cap_ms * 1e-3;
            if (predict_mode == "vsync") {
                double p = compute_predict_s(pose_now,
                                             g_vsync_last.load(std::memory_order_relaxed),
                                             g_vsync_interval.load(std::memory_order_relaxed),
                                             scanout_ms * 1e-3, cap_s);
                predict_s = (p >= 0.0) ? p
                          : std::min(double(dt_mean_ema) + scanout_ms * 1e-3, cap_s); // fallback
            } else { // "fixed"
                predict_s = std::min(double(predict_ms) * 1e-3, cap_s);
            }
            predict_s *= gate * predict_scale;
        }
        g_last_predict_ms = float(predict_s * 1e3);

        float pose[7] = {0};
        // smoothvel does its own extrapolation from a de-noised velocity, so the SDK
        // must return the un-extrapolated pose (predict 0); other modes let it predict.
        double sdk_predict_ns = (predict_mode == "smoothvel") ? 0.0 : predict_s * 1e9;
        if (get_gl_pose_carina(g_provider, pose, sdk_predict_ns) == 0) {
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
                float dx = rp.x - head_p.x, dy = rp.y - head_p.y, dz = rp.z - head_p.z;
                float speed = std::sqrt(dx * dx + dy * dy + dz * dz) / dt; // m/s
                const float d_cutoff = 1.8f; // Hz — how fast the speed estimate reacts
                float ad = one_euro_alpha(d_cutoff, dt);
                speed_hat += (speed - speed_hat) * ad;
                float min_cutoff = smooth_pos * 4.f; // default 0.10 → 0.4 Hz at rest
                // Deadband removes the VIO noise floor from the speed signal,
                // allowing a strong motion gain without rest wiggle.
                float speed_eff = std::max(0.f, speed_hat - 0.03f);
                float cutoff = min_cutoff + 9.f * speed_eff;
                float ap = one_euro_alpha(cutoff, dt);
                head_p.x += dx * ap;
                head_p.y += dy * ap;
                head_p.z += dz * ap;
                // nlerp along the shortest arc, with speed-adaptive blending
                // (One-Euro style): near-still → heavy filtering kills the
                // shimmer; fast turns → near-raw so tracking stays snappy.
                float dot = head_q.w * rq.w + head_q.x * rq.x + head_q.y * rq.y + head_q.z * rq.z;
                float sign = dot < 0 ? -1.f : 1.f;
                float ang = 2.f * std::acos(std::min(1.f, std::fabs(dot))) * 180.f / float(M_PI);
                ang_speed_hat += (ang - ang_speed_hat) * 0.3f;   // deg/frame EMA
                // Velocity term dominates quickly: shimmer-damping at rest,
                // near-raw tracking as soon as the head actually turns.
                float a = std::min(ori_motion_cap, smooth_ori * 0.25f + ang * 1.2f);
                head_q.w += (rq.w * sign - head_q.w) * a;
                head_q.x += (rq.x * sign - head_q.x) * a;
                head_q.y += (rq.y * sign - head_q.y) * a;
                head_q.z += (rq.z * sign - head_q.z) * a;
                float m = std::sqrt(head_q.w * head_q.w + head_q.x * head_q.x +
                                    head_q.y * head_q.y + head_q.z * head_q.z);
                if (m > 1e-6f) { head_q.w /= m; head_q.x /= m; head_q.y /= m; head_q.z /= m; }
            }
        }
        // smoothvel: track a low-passed world-frame angular velocity from the filtered
        // head pose. Runs every frame (cheap); consumed only in smoothvel mode at the
        // view build. At rest Δq→0 so ω decays to ~0 (no shimmer), independent of the gate.
        if (have_pose) {
            if (sv_have_prev) {
                Vec3 rv = quat_delta_rotvec(sv_prev_q, head_q);   // world-frame delta this frame (rad)
                float a_om = one_euro_alpha(vel_cutoff, dt);
                sv_omega.x += (rv.x / dt - sv_omega.x) * a_om;
                sv_omega.y += (rv.y / dt - sv_omega.y) * a_om;
                sv_omega.z += (rv.z / dt - sv_omega.z) * a_om;
            }
            sv_prev_q = head_q;
            sv_have_prev = true;
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

        // ---- render
        // Draw-list caps tie together, per eye: config's 16-screen max
        // + 4 active-screen border bars + VO dot + 2 per-hand status dots
        // + 2 hands x 21 landmarks = 65 <= 72. Bump any cap and this grows too.
        QuadDraw draws[2][72];
        int ndraw[2] = {0, 0};
        if (have_pose && anchored) {
            // view = trim ⊗ inverse(recentered head pose)
            // smoothvel: extrapolate the DISPLAY orientation forward by the gated horizon
            // using our de-noised angular velocity (display-only — gestures/telemetry above
            // keep the un-predicted pose). Other modes render head_q directly.
            Quat disp_q = (predict_mode == "smoothvel")
                        ? quat_integrate(head_q, sv_omega, float(predict_s)) : head_q;
            g_last_svel_deg = (predict_mode == "smoothvel")
                        ? std::sqrt(sv_omega.x * sv_omega.x + sv_omega.y * sv_omega.y +
                                    sv_omega.z * sv_omega.z) * float(predict_s) * 180.f / float(M_PI)
                        : 0.f;
            Quat head_rc = qmul(qconj(ori_offset), disp_q);
            Vec3 hp = qrot(qconj(ori_offset), head_p);
            Quat view_q = qconj(qmul(head_rc, trim));
            Vec3 hp_neg = qrot(view_q, { -hp.x, -hp.y, -hp.z });

            // Painter's order, per frame: no depth buffer, and 6DoF lets the
            // user walk around the rack — order by live head distance,
            // farthest first, so the nearer screen wins where two overlap.
            // Poses are eye-independent; both eyes reuse them.
            struct SortedScreen { Quat q; Vec3 p; float d2; const ScreenInst* s; };
            SortedScreen order[40];
            int nscene = int(scene.size() < 40 ? scene.size() : 40);
            for (int i = 0; i < nscene; i++) {
                SortedScreen& e = order[i];
                e.s = &scene[i];
                scene_screen_pose(scene[i], rack_q, rack_p, rack_dist_scale, e.q, e.p);
                float dx = e.p.x - hp.x, dy = e.p.y - hp.y, dz = e.p.z - hp.z;
                e.d2 = dx * dx + dy * dy + dz * dz;
            }
            std::sort(order, order + nscene,
                      [](const SortedScreen& a, const SortedScreen& b) { return a.d2 > b.d2; });

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
                // Shared "selection/pinch active" green — the selection border,
                // the pinch-status dot, and the fingertip highlight must all use
                // the same green (design: the feedback channels agree).
                const float status_green[4] = { 0.20f, 0.90f, 0.30f, 1.f };
                for (int i = 0; i < nscene; i++) {
                    if (nd >= 72) break;
                    const ScreenInst& s = *order[i].s;
                    float model[16], vm[16], smvp[16];
                    mat_from_pose(order[i].q, order[i].p, model);
                    mat_mul(view, model, vm);
                    mat_mul(proj, vm, smvp);
                    float aspect = multi ? s.aspect : cap_aspect;
                    float diag_m = s.cfg.size * ((multi && !s.cfg.has_pose_override) ? rack_size_scale : 1.f) * 0.0254f;
                    float w2 = diag_m * aspect / std::sqrt(1 + aspect * aspect) * 0.5f;
                    float h2 = diag_m / std::sqrt(1 + aspect * aspect) * 0.5f;
                    QuadDraw& d = dl[nd++];
                    memcpy(d.mvp, smvp, sizeof(smvp));
                    memcpy(d.color, white, 4 * sizeof(float));
                    d.rect[0] = 0; d.rect[1] = 0; d.rect[2] = w2; d.rect[3] = h2;
                    memcpy(d.uv, s.uv, sizeof(s.uv));
                    d.textured = true;
                    // Selected-screen highlight: a green frame OUTSIDE the
                    // content rect [±w2,±h2], coplanar with the screen (shares
                    // smvp) so it gets correct per-eye stereo and never overlaps
                    // the content. order[] is the sorted draw order; match the
                    // active screen by identity, not index.
                    if (active_screen >= 0 && order[i].s == &scene[active_screen]) {
                        const float b = SELECT_BORDER_M;
                        // top, bottom, left, right (top/bottom widened by b to fill corners)
                        const float bars[4][4] = {
                            { 0.f,        h2 + b * 0.5f, w2 + b,      b * 0.5f },
                            { 0.f,      -(h2 + b * 0.5f), w2 + b,      b * 0.5f },
                            { -(w2 + b * 0.5f), 0.f,      b * 0.5f,    h2       },
                            {  (w2 + b * 0.5f), 0.f,      b * 0.5f,    h2       },
                        };
                        for (int e = 0; e < 4 && nd < 72; e++) {
                            QuadDraw& bd = dl[nd++];
                            memcpy(bd.mvp, smvp, sizeof(smvp));
                            memcpy(bd.color, status_green, 4 * sizeof(float));
                            bd.rect[0] = bars[e][0]; bd.rect[1] = bars[e][1];
                            bd.rect[2] = bars[e][2]; bd.rect[3] = bars[e][3];
                            bd.textured = false;
                            bd.circle = false;
                        }
                    }
                }
                // `mvp` below (HUD blocks) previously reused the screen's matrix
                // chain; the HUD only ever used proj-based matrices, which are
                // built inline there already — no change needed.

                // Head-locked 6DoF tracking-status dot: blue = 6DoF live,
                // orange = positional tracking frozen (orientation-only). Fixed
                // at bottom-CENTER of the view, flanked by the two per-hand
                // status dots. mvp is projection * translate only, no world xf.
                const float live[4] = { 0.35f, 0.76f, 1.f, 1.f };
                const float frozen[4] = { 1.f, 0.55f, 0.2f, 1.f };
                const float DOT_Z = 0.5f;          // meters in front of the eye
                float tan_r = r / near_z, tan_t = t / near_z;
                float eye_m[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,
                                    eye_off, -0.95f * tan_t * DOT_Z, -DOT_Z, 1 };
                QuadDraw& d = dl[nd++];
                mat_mul(proj, eye_m, d.mvp);
                memcpy(d.color, sixdof_live ? live : frozen, 4 * sizeof(float));
                float dot_r = 0.0045f * DOT_Z;     // ~0.5 degrees apparent size
                d.rect[0] = 0; d.rect[1] = 0; d.rect[2] = dot_r; d.rect[3] = dot_r;
                d.textured = false;
                d.circle = true;

                // Per-hand pinch/arm-status dots: one per hand at the bottom
                // corners (left hand -> bottom-left, right hand -> bottom-right),
                // flanking the centered VO dot. Only shown while the gesture
                // pipeline is live. Per-hand states: grey = hand not seen, amber
                // = seen but not armed, blue = armed (open palm), green =
                // armed + pinching.
                if (g_gestures.enabled()) {
                    const float grey[4]  = { 0.5f, 0.5f, 0.5f, 1.f };
                    const float amber[4] = { 1.f, 0.65f, 0.1f, 1.f };
                    const float blue[4]  = { 0.30f, 0.55f, 1.f, 1.f };
                    const HandState* hstate[2] = { &gev.left, &gev.right };
                    const float hxf[2] = { -0.95f, 0.95f };  // left dot left, right dot right
                    for (int i = 0; i < 2; i++) {
                        const HandState& h = *hstate[i];
                        const float* pcol = !h.present ? grey
                                          : !armed[i]  ? amber
                                          : (h.pinching ? status_green : blue);
                        float peye[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,
                                           hxf[i] * tan_r * DOT_Z + eye_off, -0.95f * tan_t * DOT_Z, -DOT_Z, 1 };
                        QuadDraw& pd = dl[nd++];
                        mat_mul(proj, peye, pd.mvp);
                        memcpy(pd.color, pcol, 4 * sizeof(float));
                        float pr = dot_r;
                        if (h.has_depth && h.depth > 0.05f) {
                            // Nearer hand -> bigger dot (cheap in-glasses depth
                            // cue; the HUD has no glyphs). 0.5 m reads nominal.
                            float s = 0.5f / h.depth;
                            if (s < 0.6f) s = 0.6f; else if (s > 1.8f) s = 1.8f;
                            pr = dot_r * s;
                        }
                        pd.rect[0] = 0; pd.rect[1] = 0; pd.rect[2] = pr; pd.rect[3] = pr;
                        pd.textured = false;
                        pd.circle = true;
                    }
                }

                // Hand-landmark overlay: each hand as 21 dots in its own
                // head-locked panel (left hand -> lower-left, right hand ->
                // lower-right), shown only while that hand is seen. One shared
                // panel mvp per hand; each landmark is a different rect center
                // (the quad shader builds the quad from rect.xy ± rect.zw), so no
                // per-dot matrix. Per-hand alpha follows that hand's armed flag;
                // thumb tip (4) and index tip (8) go green when that hand is
                // armed and pinching.
                if (g_gestures.enabled()) {
                    int cw = g_cam_w.load(std::memory_order_relaxed);
                    int ch = g_cam_h.load(std::memory_order_relaxed);
                    float aspect = (cw > 0 && ch > 0) ? float(cw) / float(ch) : 4.f / 3.f;
                    const float PANEL_H = 0.09f * DOT_Z;        // half-height (~10° tall)
                    const float PANEL_W = PANEL_H * aspect;     // aspect-preserved half-width
                    const float lm_col[4] = { 0.40f, 0.90f, 1.00f, 1.f }; // soft cyan
                    const float tip[4]    = { 1.00f, 0.85f, 0.10f, 1.f }; // yellow (thumb/index tip)
                    const float lm_r = 0.0033f * DOT_Z;  // ~0.35 degrees apparent size (very thin)
                    const HandState* ovh[2] = { &gev.left, &gev.right };
                    const float panel_x[2] = { -0.55f, 0.55f };  // left hand lower-left, right hand lower-right
                    for (int hnd = 0; hnd < 2; hnd++) {
                        const HandState& h = *ovh[hnd];
                        if (!h.present || !h.has_landmarks) continue;
                        float leye[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,
                                           panel_x[hnd] * tan_r * DOT_Z + eye_off, -0.45f * tan_t * DOT_Z, -DOT_Z, 1 };
                        float panel_mvp[16];
                        mat_mul(proj, leye, panel_mvp);
                        // Unarmed: the whole hand is drawn mildly transparent;
                        // arming (open palm) makes it opaque.
                        const float ov_alpha = armed[hnd] ? 1.f : 0.45f;
                        for (int i = 0; i < 21 && nd < 72; i++) {
                            float nx = h.landmarks[i][0];
                            float ny = h.landmarks[i][1];
                            // Normalized image coords -> panel-local. Image y is
                            // down, eye-space y up, so negate y. If the hand reads
                            // mirrored on hardware, change (nx-0.5f) to (0.5f-nx).
                            float lx =  (nx - 0.5f) * 2.f * PANEL_W;
                            float ly = -(ny - 0.5f) * 2.f * PANEL_H;
                            const float* base = (i == 4 || i == 8)
                                                  ? ((armed[hnd] && h.pinching) ? status_green : tip)
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
        // Consecutive rebuilds with no present in between — a rebuild whose
        // acquire+init succeed but whose presents never recover would loop
        // forever otherwise (the 120-frame latch is unreachable once draw_fail
        // resets on rebuild). Reset when a present lands.
        static int rebuilds_without_present = 0;
        // Stereo passes both stack lists (draws[1] is non-null even when
        // empty — a null right list would silently downgrade to mono).
        // reproject: submit the image acquired before the pose block (frame_began);
        // a failed acquire falls through as drew=false into the rebuild path below.
        // Non-reproject: acquire+submit together here, exactly as before.
        bool drew = reproject
            ? (frame_began && (stereo_active
                ? vkr_submit_stereo(vk, draws[0], ndraw[0], draws[1], ndraw[1])
                : vkr_submit_stereo(vk, draws[0], ndraw[0], nullptr, 0)))
            : (stereo_active
                ? vkr_draw_stereo(vk, draws[0], ndraw[0], draws[1], ndraw[1])
                : vkr_draw(vk, draws[0], ndraw[0]));
        if (drew) {
            draw_fail = 0;
            rebuilds_without_present = 0;
            frames++;
        } else {
            draw_fail++;
            if (sout.direct && draw_fail == 30 && ++rebuilds_without_present > 3) {
                fprintf(stderr, "display: %d rebuilds without a present — shutting down\n",
                        rebuilds_without_present);
                tele.log("error", "display rebuild livelock - shutting down");
                g_running = false;
            } else if (sout.direct && draw_fail == 30) {
                // Panel mode likely changed under the lease (hardware 2D/3D
                // toggle): rebuild the whole display chain against the current
                // mode and re-pick stereo/mono from the new extent.
                fprintf(stderr, "display: rebuilding after present failures (mode change?)\n");
                tele.log("warn", "display mode changed - rebuilding");
                uint32_t old_tex_w = vk.tex_w, old_tex_h = vk.tex_h, old_pitch = vk.tex_pitch;
                vkr_destroy_device(vk);
                direct_release(vk.instance);
                direct_restore(dpy);
                outputs = list_outputs(dpy);
                for (auto& oo : outputs) if (oo.name == glasses.name) { glasses = oo; break; }
                SurfaceOut nsout{};
                if (direct_acquire(dpy, vk.instance, glasses.id, nsout)) {
                    sout = nsout;
                    vk.phys = sout.phys;
                    vk.surface = sout.surface;
                    if (vkr_init_device(vk) && vkr_init_swapchain(vk) &&
                        vkr_init_pipeline(vk) && vkr_init_texture(vk, old_tex_w, old_tex_h, old_pitch)) {
                        refresh_projection();
                        draw_fail = 0;
                        cursor_under.valid = false;
                        printf("display: rebuilt, %s\n", stereo_active ? "stereo" : "mono");
                    } else {
                        fprintf(stderr, "display: rebuild failed — shutting down\n");
                        g_running = false;
                    }
                } else {
                    fprintf(stderr, "display: re-acquire failed — shutting down\n");
                    g_running = false;
                }
            } else if (draw_fail >= 120) {
                fprintf(stderr, "presentation failed repeatedly — shutting down\n");
                g_running = false;
            }
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

            // Prediction/latency diagnostics: frame-dt mean/p95/max, vsync clock,
            // and the live prediction horizon (0 until Task 6 enables it).
            float dmean = 0, dmax = 0;
            for (int i = 0; i < dt_ring_n; i++) { dmean += dt_ring[i]; if (dt_ring[i] > dmax) dmax = dt_ring[i]; }
            if (dt_ring_n) dmean /= dt_ring_n;
            float dsorted[256];
            for (int i = 0; i < dt_ring_n; i++) dsorted[i] = dt_ring[i];
            std::sort(dsorted, dsorted + dt_ring_n);
            float dp95 = dt_ring_n ? dsorted[(dt_ring_n * 95) / 100] : 0.f;
            double vs_int = g_vsync_interval.load(std::memory_order_relaxed);
            double vs_age = mono_now_s() - g_vsync_last.load(std::memory_order_relaxed);
            bool vs_ok = vs_int > 0.0 && vs_age < 8.0 * vs_int;
            printf("  predict %.1fms  svel %.2fdeg  dt %.1f/%.1f/%.1f ms(mean/p95/max)  vsync %s %.2fms\n",
                   g_last_predict_ms, g_last_svel_deg, dmean * 1e3f, dp95 * 1e3f, dmax * 1e3f,
                   vs_ok ? "ok" : "ABSENT", vs_int * 1e3);

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
        tele.send_hands(gev.left.present, gev.left.has_depth, gev.left.depth,
                        gev.right.present, gev.right.has_depth, gev.right.depth);
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
    // Panel back to 2D while the lease is still held: releasing the lease
    // first makes X re-modeset the output, and the resulting DP retrain
    // kills the restore's USB command (all 6 retries failed on hardware).
    // With the link quiet the command lands first try; the mode change then
    // drops our lease, which we were about to release anyway.
    sbs_exit(g_provider, g_sbs_orig);
    g_sbs_orig = -1;
    g_gestures.stop();
    tele.stop();
    if (cap) cap->stop();
    vkr_destroy_device(vk);                        // swapchain/surface/device first
    if (sout.direct) direct_release(vk.instance);  // drop the lease (instance teardown never does)
    vkr_destroy(vk);
    if (sout.direct) direct_restore(dpy);          // now the server can re-enable the output
    else if (sout.window) XDestroyWindow(dpy, sout.window);
    sdk_shutdown();                                // safety net for error paths; panel already 2D here
    XCloseDisplay(dpy);
    return 0;
}
