// spatial-screens — phase 2 renderer spike (M2 + first cut of M3).
//
// Places one virtual screen in 3D space using the Luma Ultra's 6DoF pose and
// renders it fullscreen on the glasses' display. The screen is textured with
// a live X11 capture of a source monitor (XShm) or a test pattern.
//
//   ./run.sh [--monitor NAME] [--capture NAME|test] [--distance M]
//            [--size INCHES] [--pitch-trim DEG] [--predict-ms MS]
//
// Keys:  R recenter (re-place screen in front of you)
//        Shift+R also reset the VIO origin
//        [ / ]  screen closer / farther      - / =  smaller / larger
//        Q/Esc  quit
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
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>
#include <sys/ipc.h>
#include <sys/shm.h>

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
#include "vk_surface.h"

// ---------------------------------------------------------------- math ----

struct Quat { float w = 1, x = 0, y = 0, z = 0; };
struct Vec3 { float x = 0, y = 0, z = 0; };

static Quat qmul(const Quat& a, const Quat& b) {
    return { a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
             a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
}
static Quat qconj(const Quat& q) { return { q.w, -q.x, -q.y, -q.z }; }

static Quat yaw_twist(const Quat& q) {
    float m = std::sqrt(q.w * q.w + q.y * q.y);
    if (m < 1e-6f) return {};
    return { q.w / m, 0, q.y / m, 0 };
}

static Quat quat_axis_angle(float ax, float ay, float az, float deg) {
    float r = deg * float(M_PI) / 180.f * 0.5f;
    float s = std::sin(r);
    return { std::cos(r), ax * s, ay * s, az * s };
}

static Vec3 qrot(const Quat& q, const Vec3& v) {
    float tx = 2 * (q.y * v.z - q.z * v.y);
    float ty = 2 * (q.z * v.x - q.x * v.z);
    float tz = 2 * (q.x * v.y - q.y * v.x);
    return { v.x + q.w * tx + (q.y * tz - q.z * ty),
             v.y + q.w * ty + (q.z * tx - q.x * tz),
             v.z + q.w * tz + (q.x * ty - q.y * tx) };
}

// Column-major 4x4 from rotation quat + translation (OpenGL convention).
static void mat_from_pose(const Quat& q, const Vec3& t, float* m) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0] = 1 - 2 * (yy + zz); m[4] = 2 * (xy - wz);     m[8] = 2 * (xz + wy);      m[12] = t.x;
    m[1] = 2 * (xy + wz);     m[5] = 1 - 2 * (xx + zz); m[9] = 2 * (yz - wx);      m[13] = t.y;
    m[2] = 2 * (xz - wy);     m[6] = 2 * (yz + wx);     m[10] = 1 - 2 * (xx + yy); m[14] = t.z;
    m[3] = 0;                 m[7] = 0;                 m[11] = 0;                 m[15] = 1;
}

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

// ------------------------------------------------------------- SDK glue ----

static XRDeviceProviderHandle g_provider = nullptr;
static std::atomic<bool> g_running{true};

static void on_imu_noop(float*, double) {}
static void on_pose_noop(float*, double) {}

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
    if (register_callbacks_carina(g_provider, on_pose_noop, nullptr, on_imu_noop, nullptr) != 0 ||
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
    std::string monitor_name, capture_name;
    // Defaults sized to FIT the Ultra's 52-degree FOV with margin: at 2 m the
    // panel shows ~85 inches full-width — a 60-inch screen leaves the frame
    // and the world visible around it, which is what makes 6DoF perceivable.
    // predict 0 by default: XRLinuxDriver's known-good usage passes 0, and
    // extrapolation visibly amplifies rotation jitter during head turns.
    // 24" at 0.75 m = desk-monitor ergonomics that fit the 52-degree FOV.
    // Prediction stays 0: extrapolation noise feeds the filter's speed
    // estimate and reads as shake (verified across builds).
    float distance = 0.75f, diag_in = 24.f, pitch_trim = 0.f, predict_ms = 0.f;
    // Pose smoothing (EMA blend factor per frame, 1 = off). Position gets a
    // heavy filter — VIO translation is where the jitter lives; orientation
    // stays light so head tracking doesn't feel laggy.
    float smooth_pos = 0.10f, smooth_ori = 0.40f;
    bool force_window = false;
    for (int i = 1; i < argc; i++) {
        auto next = [&](float& v) { if (i + 1 < argc) v = atof(argv[++i]); };
        if (!strcmp(argv[i], "--monitor") && i + 1 < argc) monitor_name = argv[++i];
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) capture_name = argv[++i];
        else if (!strcmp(argv[i], "--distance")) next(distance);
        else if (!strcmp(argv[i], "--size")) next(diag_in);
        else if (!strcmp(argv[i], "--pitch-trim")) next(pitch_trim);
        else if (!strcmp(argv[i], "--predict-ms")) next(predict_ms);
        else if (!strcmp(argv[i], "--smooth-pos")) next(smooth_pos);
        else if (!strcmp(argv[i], "--smooth-ori")) next(smooth_ori);
        else if (!strcmp(argv[i], "--window")) force_window = true;
        else {
            printf("usage: %s [--monitor NAME] [--capture NAME|test] [--distance M] "
                   "[--size IN] [--pitch-trim DEG] [--predict-ms MS] "
                   "[--smooth-pos 0..1] [--smooth-ori 0..1] [--window]\n", argv[0]);
            return 0;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { fprintf(stderr, "cannot open X display\n"); return 1; }
    XSetErrorHandler(on_x_error);
    Window root = DefaultRootWindow(dpy);

    // -- pick outputs: glasses = 1920x1200-ish (or --monitor), capture = another
    auto outputs = list_outputs(dpy);
    OutputRect glasses{}, source{};
    bool have_glasses = false, have_source = false;
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
    bool test_pattern = capture_name == "test";
    if (!test_pattern) {
        for (auto& o : outputs) {
            if (!capture_name.empty() ? o.name == capture_name : o.name != glasses.name) {
                source = o; have_source = true; break;
            }
        }
        if (!have_source) {
            printf("no capture source — falling back to test pattern\n");
            test_pattern = true;
        }
    }
    printf("glasses: %s (%dx%d)  capture: %s\n", glasses.name.c_str(), glasses.w, glasses.h,
           test_pattern ? "test pattern" : source.name.c_str());

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
        vkr_destroy_device(vk);
        if (sout.direct) direct_release(vk.instance);
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

    // -- capture setup (XShm full-source-monitor grabs)
    XShmSegmentInfo shm{};
    XImage* ximg = nullptr;
    if (!test_pattern) {
        ximg = XShmCreateImage(dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 24,
                               ZPixmap, nullptr, &shm, source.w, source.h);
        shm.shmid = shmget(IPC_PRIVATE, size_t(ximg->bytes_per_line) * ximg->height, IPC_CREAT | 0600);
        shm.shmaddr = ximg->data = (char*)shmat(shm.shmid, nullptr, 0);
        shm.readOnly = False;
        XShmAttach(dpy, &shm);
        shmctl(shm.shmid, IPC_RMID, nullptr); // auto-free on detach
    }

    // -- capture texture
    int tex_w = test_pattern ? 1024 : source.w;
    int tex_h = test_pattern ? 576 : source.h;
    uint32_t tex_pitch = test_pattern ? uint32_t(tex_w) * 4
                                      : uint32_t(ximg->bytes_per_line);
    if (!vkr_init_texture(vk, tex_w, tex_h, tex_pitch)) {
        vkr_destroy_device(vk);
        if (sout.direct) direct_release(vk.instance);
        vkr_destroy(vk);
        if (sout.direct) direct_restore(dpy);
        return 1;
    }
    std::vector<uint32_t> pattern(size_t(tex_w) * tex_h);

    // -- SDK last (it takes a second; window is already up)
    if (!sdk_init()) {
        vkr_destroy_device(vk);
        if (sout.direct) direct_release(vk.instance);
        vkr_destroy(vk);
        if (sout.direct) direct_restore(dpy);
        return 1;
    }

    // -- projection from the glasses' 52-degree diagonal FOV (16:10 panel)
    const float DIAG_FOV = 52.f;
    // In direct mode the swapchain extent is authoritative (mode may differ
    // from the desktop rect we detected).
    if (sout.direct) { glasses.w = int(vk.extent.width); glasses.h = int(vk.extent.height); }
    float diag_px = std::sqrt(float(glasses.w * glasses.w + glasses.h * glasses.h));
    float half = std::tan(DIAG_FOV * float(M_PI) / 360.f);
    float near_z = 0.1f, far_z = 100.f;
    float r = half * glasses.w / diag_px * near_z;
    float t = half * glasses.h / diag_px * near_z;

    // -- screen anchor state
    Quat head_q; Vec3 head_p;
    Quat ori_offset;              // yaw recenter
    Quat trim = quat_axis_angle(1, 0, 0, pitch_trim);
    Quat anchor_q; Vec3 anchor_p; // virtual screen pose
    bool anchored = false;
    float cap_aspect = test_pattern ? 16.f / 9.f : float(source.w) / float(source.h);

    auto place_screen = [&]() {
        Quat basis = yaw_twist(qmul(qconj(ori_offset), head_q));
        anchor_q = basis;
        Vec3 fwd = qrot(basis, { 0, 0, -1 });
        Vec3 hp = qrot(qconj(ori_offset), head_p);
        anchor_p = { hp.x + fwd.x * distance, hp.y + fwd.y * distance, hp.z + fwd.z * distance };
        anchored = true;
    };

    auto now_s = [] {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    };
    double last_fps_t = now_s(), last_cap_t = 0;
    int frames = 0;
    bool have_pose = false;

    // 6DoF liveness heuristic: if the head clearly rotates but reported
    // position stays frozen, the VIO is running orientation-only.
    bool sixdof_live = false;
    Vec3 win_min, win_max;
    Quat win_q0;
    float win_max_ang = 0;
    int win_n = 0;

    printf("running — hotkeys work globally with Ctrl+Alt: R recenter (Shift adds "
           "VIO reset), [ ] distance, - = size, Q quit\n");

    while (g_running) {
        // ---- input
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
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
                            if (!ximg) {
                                fprintf(stderr, "capture rebuild failed (XShmCreateImage)\n");
                                g_running = false;
                                break;
                            }
                            shm.shmid = shmget(IPC_PRIVATE,
                                               size_t(ximg->bytes_per_line) * ximg->height,
                                               IPC_CREAT | 0600);
                            shm.shmaddr = ximg->data = (char*)shmat(shm.shmid, nullptr, 0);
                            shm.readOnly = False;
                            XShmAttach(dpy, &shm);
                            shmctl(shm.shmid, IPC_RMID, nullptr);
                            vkr_destroy_texture(vk);
                            if (!vkr_init_texture(vk, source.w, source.h,
                                                  uint32_t(ximg->bytes_per_line))) {
                                fprintf(stderr, "capture rebuild failed (texture)\n");
                                g_running = false;
                                break;
                            }
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
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            bool shift = ev.xkey.state & ShiftMask;
            if (ks == XK_q || ks == XK_Escape) g_running = false;
            else if (ks == XK_r) {
                if (shift) reset_pose_carina(g_provider);
                ori_offset = yaw_twist(head_q);
                place_screen();
                printf("recentered%s\n", shift ? " + VIO reset" : "");
            }
            else if (ks == XK_bracketleft)  { distance = std::max(0.5f, distance - 0.25f); place_screen(); }
            else if (ks == XK_bracketright) { distance = std::min(10.f, distance + 0.25f); place_screen(); }
            else if (ks == XK_minus) diag_in = std::max(40.f, diag_in - 10.f);
            else if (ks == XK_equal) diag_in = std::min(400.f, diag_in + 10.f);
        }
        if (!g_running) break;

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
                place_screen();
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

        // ---- capture (30 Hz is plenty; pose stays at panel rate)
        double tnow = now_s();
        if (tnow - last_cap_t > 1.0 / 30.0) {
            last_cap_t = tnow;
            if (test_pattern) {
                int shift_px = int(tnow * 40) % 64;
                for (int y = 0; y < tex_h; y++)
                    for (int x = 0; x < tex_w; x++)
                        pattern[size_t(y) * tex_w + x] =
                            (((x + shift_px) / 64 + y / 64) & 1) ? 0xff2d3646 : 0xff59c2ff;
                vkr_upload(vk, pattern.data(), pattern.size() * 4);
            } else if (XShmGetImage(dpy, root, ximg, source.x, source.y, AllPlanes)) {
                vkr_upload(vk, ximg->data, size_t(ximg->bytes_per_line) * ximg->height);
            }
        }

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
        frames++;
        if (tnow - last_fps_t >= 2.0) {
            printf("fps %.0f  pose %s  6dof %s  head [%+.3f %+.3f %+.3f]m  dist %.2fm  size %.0f\"\n",
                   frames / (tnow - last_fps_t), have_pose ? "ok" : "waiting",
                   sixdof_live ? "LIVE" : "frozen",
                   head_p.x, head_p.y, head_p.z, distance, diag_in);
            frames = 0;
            last_fps_t = tnow;
        }
    }

    printf("shutting down…\n");
    xr_device_provider_stop(g_provider);
    xr_device_provider_shutdown(g_provider);
    xr_device_provider_destroy(g_provider);
    if (ximg) { XShmDetach(dpy, &shm); XDestroyImage(ximg); shmdt(shm.shmaddr); }
    vkr_destroy_device(vk);                        // swapchain/surface/device first
    if (sout.direct) direct_release(vk.instance);  // drop the lease (instance teardown never does)
    vkr_destroy(vk);
    if (sout.direct) direct_restore(dpy);          // now the server can re-enable the output
    else if (sout.window) XDestroyWindow(dpy, sout.window);
    XCloseDisplay(dpy);
    return 0;
}
