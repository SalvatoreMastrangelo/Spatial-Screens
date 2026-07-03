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
//
// Coordinates: get_gl_pose_carina returns Twb in OpenGL/EUS (x right, y up,
// z backward), position in meters. World-locked content = render with
// view = inverse(head pose).

#include <GL/glx.h>
#include <GL/gl.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>
#include <X11/keysym.h>
#include <sys/ipc.h>
#include <sys/shm.h>

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

// ------------------------------------------------------------- SDK glue ----

static XRDeviceProviderHandle g_provider = nullptr;
static std::atomic<bool> g_running{true};

static void on_imu_noop(float*, double) {}

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
    if (register_callbacks_carina(g_provider, nullptr, nullptr, on_imu_noop, nullptr) != 0 ||
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

// ------------------------------------------------------------- X11/RandR ----

struct OutputRect { std::string name; int x, y, w, h; };

static std::vector<OutputRect> list_outputs(Display* dpy) {
    std::vector<OutputRect> out;
    Window root = DefaultRootWindow(dpy);
    XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
    for (int i = 0; i < res->noutput; i++) {
        XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (oi->connection == RR_Connected && oi->crtc) {
            XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
            out.push_back({ oi->name, ci->x, ci->y, int(ci->width), int(ci->height) });
            XRRFreeCrtcInfo(ci);
        }
        XRRFreeOutputInfo(oi);
    }
    XRRFreeScreenResources(res);
    return out;
}

// ---------------------------------------------------------------- main ----

static void on_signal(int) { g_running = false; }

int main(int argc, char** argv) {
    std::string monitor_name, capture_name;
    float distance = 1.75f, diag_in = 120.f, pitch_trim = 0.f, predict_ms = 8.f;
    for (int i = 1; i < argc; i++) {
        auto next = [&](float& v) { if (i + 1 < argc) v = atof(argv[++i]); };
        if (!strcmp(argv[i], "--monitor") && i + 1 < argc) monitor_name = argv[++i];
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) capture_name = argv[++i];
        else if (!strcmp(argv[i], "--distance")) next(distance);
        else if (!strcmp(argv[i], "--size")) next(diag_in);
        else if (!strcmp(argv[i], "--pitch-trim")) next(pitch_trim);
        else if (!strcmp(argv[i], "--predict-ms")) next(predict_ms);
        else {
            printf("usage: %s [--monitor NAME] [--capture NAME|test] [--distance M] "
                   "[--size IN] [--pitch-trim DEG] [--predict-ms MS]\n", argv[0]);
            return 0;
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) { fprintf(stderr, "cannot open X display\n"); return 1; }
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

    // -- GLX window on the glasses output
    GLint visAttrs[] = { GLX_RGBA, GLX_DEPTH_SIZE, 16, GLX_DOUBLEBUFFER, None };
    XVisualInfo* vi = glXChooseVisual(dpy, DefaultScreen(dpy), visAttrs);
    if (!vi) { fprintf(stderr, "no GLX visual\n"); return 1; }
    XSetWindowAttributes swa{};
    swa.colormap = XCreateColormap(dpy, root, vi->visual, AllocNone);
    swa.override_redirect = True;
    swa.event_mask = KeyPressMask | StructureNotifyMask;
    Window win = XCreateWindow(dpy, root, glasses.x, glasses.y, glasses.w, glasses.h, 0,
                               vi->depth, InputOutput, vi->visual,
                               CWColormap | CWOverrideRedirect | CWEventMask, &swa);
    XMapRaised(dpy, win);
    XMoveResizeWindow(dpy, win, glasses.x, glasses.y, glasses.w, glasses.h);
    XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
    GLXContext ctx = glXCreateContext(dpy, vi, nullptr, True);
    glXMakeCurrent(dpy, win, ctx);

    // vsync on if available
    typedef void (*SwapIntervalProc)(Display*, GLXDrawable, int);
    if (auto p = (SwapIntervalProc)glXGetProcAddress((const GLubyte*)"glXSwapIntervalEXT"))
        p(dpy, win, 1);

    // -- capture setup (XShm full-source-monitor grabs)
    XShmSegmentInfo shm{};
    XImage* ximg = nullptr;
    if (!test_pattern) {
        ximg = XShmCreateImage(dpy, vi->visual, 24, ZPixmap, nullptr, &shm, source.w, source.h);
        shm.shmid = shmget(IPC_PRIVATE, size_t(ximg->bytes_per_line) * ximg->height, IPC_CREAT | 0600);
        shm.shmaddr = ximg->data = (char*)shmat(shm.shmid, nullptr, 0);
        shm.readOnly = False;
        XShmAttach(dpy, &shm);
        shmctl(shm.shmid, IPC_RMID, nullptr); // auto-free on detach
    }

    // -- texture
    int tex_w = test_pattern ? 1024 : source.w;
    int tex_h = test_pattern ? 576 : source.h;
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, tex_w, tex_h, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    std::vector<uint32_t> pattern(size_t(tex_w) * tex_h);

    // -- SDK last (it takes a second; window is already up)
    if (!sdk_init()) return 1;

    // -- projection from the glasses' 52-degree diagonal FOV (16:10 panel)
    const float DIAG_FOV = 52.f;
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

    printf("running — R recenter, Shift+R reset VIO, [ ] distance, - = size, Q quit\n");

    while (g_running) {
        // ---- input
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
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

        // ---- pose (predicted)
        float pose[7] = {0};
        if (get_gl_pose_carina(g_provider, pose, double(predict_ms) * 1e6) == 0) {
            head_p = { pose[0], pose[1], pose[2] };
            head_q = { pose[3], pose[4], pose[5], pose[6] };
            if (!have_pose) { have_pose = true; ori_offset = yaw_twist(head_q); place_screen(); }
        }

        // ---- capture (30 Hz is plenty; pose stays at panel rate)
        double tnow = now_s();
        if (tnow - last_cap_t > 1.0 / 30.0) {
            last_cap_t = tnow;
            glBindTexture(GL_TEXTURE_2D, tex);
            if (test_pattern) {
                int shift_px = int(tnow * 40) % 64;
                for (int y = 0; y < tex_h; y++)
                    for (int x = 0; x < tex_w; x++)
                        pattern[size_t(y) * tex_w + x] =
                            (((x + shift_px) / 64 + y / 64) & 1) ? 0xff2d3646 : 0xff59c2ff;
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_w, tex_h, GL_BGRA, GL_UNSIGNED_BYTE, pattern.data());
            } else if (XShmGetImage(dpy, root, ximg, source.x, source.y, AllPlanes)) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, tex_w, tex_h, GL_BGRA, GL_UNSIGNED_BYTE, ximg->data);
            }
        }

        // ---- render
        glViewport(0, 0, glasses.w, glasses.h);
        glClearColor(0.03f, 0.04f, 0.07f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glFrustum(-r, r, -t, t, near_z, far_z);

        if (have_pose && anchored) {
            // view = trim ⊗ inverse(recentered head pose)
            Quat head_rc = qmul(qconj(ori_offset), head_q);
            Vec3 hp = qrot(qconj(ori_offset), head_p);
            Quat view_q = qconj(qmul(head_rc, trim));
            Vec3 hp_neg = qrot(view_q, { -hp.x, -hp.y, -hp.z });
            float view[16];
            mat_from_pose(view_q, hp_neg, view);

            float model[16];
            mat_from_pose(anchor_q, anchor_p, model);

            glMatrixMode(GL_MODELVIEW);
            glLoadMatrixf(view);
            glMultMatrixf(model);

            // quad dimensions from diagonal size + capture aspect
            float diag_m = diag_in * 0.0254f;
            float w2 = diag_m * cap_aspect / std::sqrt(1 + cap_aspect * cap_aspect) * 0.5f;
            float h2 = diag_m / std::sqrt(1 + cap_aspect * cap_aspect) * 0.5f;

            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, tex);
            glColor3f(1, 1, 1);
            glBegin(GL_QUADS);
            glTexCoord2f(0, 1); glVertex3f(-w2, -h2, 0);
            glTexCoord2f(1, 1); glVertex3f(w2, -h2, 0);
            glTexCoord2f(1, 0); glVertex3f(w2, h2, 0);
            glTexCoord2f(0, 0); glVertex3f(-w2, h2, 0);
            glEnd();
            glDisable(GL_TEXTURE_2D);

            // thin frame for depth perception
            glColor3f(0.35f, 0.76f, 1.f);
            glLineWidth(2);
            glBegin(GL_LINE_LOOP);
            glVertex3f(-w2, -h2, 0); glVertex3f(w2, -h2, 0);
            glVertex3f(w2, h2, 0); glVertex3f(-w2, h2, 0);
            glEnd();
        }

        glXSwapBuffers(dpy, win);
        frames++;
        if (tnow - last_fps_t >= 2.0) {
            printf("fps %.0f  pose %s  dist %.2fm  size %.0f\"\n",
                   frames / (tnow - last_fps_t), have_pose ? "ok" : "waiting", distance, diag_in);
            frames = 0;
            last_fps_t = tnow;
        }
    }

    printf("shutting down…\n");
    xr_device_provider_stop(g_provider);
    xr_device_provider_shutdown(g_provider);
    xr_device_provider_destroy(g_provider);
    if (ximg) { XShmDetach(dpy, &shm); XDestroyImage(ximg); shmdt(shm.shmaddr); }
    glXMakeCurrent(dpy, None, nullptr);
    glXDestroyContext(dpy, ctx);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
