// viture-bridge — streams every sensor the official VITURE SDK v2.0 exposes
// to the sensor-viz web app over a localhost WebSocket (JSON text frames).
//
// Luma Ultra ("Carina" device type): 6DoF pose (position + quaternion, EUS/GL
// coords), raw IMU, VSync, device state. Older/other models fall back to the
// 3DoF pose + raw IMU path (open_imu).
//
// Protocol (server → client):
//   {"type":"hello", model, market_name, pid, firmware, device_type}
//   {"type":"pose",  t, px,py,pz, qw,qx,qy,qz}          6DoF (carina)
//   {"type":"euler", t, roll,pitch,yaw, qw,qx,qy,qz}    3DoF pose mode
//   {"type":"imu",   t, a:[..], g:[..], values:[..]}       carina (no mag/temp)
//   {"type":"imu",   t, g:[..], a:[..], m:[..], temp}      legacy raw mode
//   {"type":"vsync", t, hz}
//   {"type":"state", id, name, value}
//   {"type":"log",   level, text}
// client → server: {"type":"reset_pose"}

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <dirent.h>
#include <string>
#include <vector>

#include "viture_glasses_provider.h"
#include "viture_device.h"
#include "viture_device_carina.h"
#include "ws_server.hpp"

namespace {

wsrv::Server g_ws;
XRDeviceProviderHandle g_provider = nullptr;
std::atomic<bool> g_running{true};
std::atomic<int> g_device_type{-1};

// send throttles (wall-clock ms of last send)
std::atomic<long> g_last_pose_ms{0}, g_last_imu_ms{0}, g_last_vsync_ms{0};
std::atomic<long> g_vsync_count{0};

constexpr int POSE_SEND_HZ = 60;
constexpr int IMU_SEND_HZ = 30;

long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void sendf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_ws.broadcast(buf);
}

void log_msg(const char* level, const char* text) {
    fprintf(stderr, "[%s] %s\n", level, text);
    sendf(R"({"type":"log","level":"%s","text":"%s"})", level, text);
}

// ---- SDK callbacks -----------------------------------------------------------

void on_vsync_carina(double ts) {
    g_vsync_count++;
    long t = now_ms();
    if (t - g_last_vsync_ms.load() < 500) return;
    g_last_vsync_ms = t;
    sendf(R"({"type":"vsync","t":%.6f,"count":%ld})", ts, g_vsync_count.load());
}

// Carina IMU callback layout (observed on real Luma Ultra hardware): first
// triplet is the accelerometer (~9.8 magnitude at rest), second is the
// gyroscope; remaining floats are device-defined and shipped raw in values[].
void on_imu_carina(float* imu, double ts) {
    // Single pose source: poll the predicted GL pose at IMU rate. Do NOT also
    // register the SDK pose callback — it reports in a different frame /
    // without prediction, and interleaving the two makes the view jump
    // (XRLinuxDriver polls exclusively for the same reason).
    if (g_provider) {
        long t = now_ms();
        if (t - g_last_pose_ms.load() >= 1000 / POSE_SEND_HZ) {
            float pose[7] = {0};
            if (get_gl_pose_carina(g_provider, pose, 0.0) == 0) {
                g_last_pose_ms = t;
                sendf(R"({"type":"pose","t":%.6f,"px":%.5f,"py":%.5f,"pz":%.5f,"qw":%.6f,"qx":%.6f,"qy":%.6f,"qz":%.6f})",
                      ts, pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6]);
            }
        }
    }

    long t = now_ms();
    if (t - g_last_imu_ms.load() < 1000 / IMU_SEND_HZ) return;
    g_last_imu_ms = t;
    // Only accel (0-2) and gyro (3-5) are trustworthy on this callback.
    // Slot 6 behaves like an inter-sample dt and 7+ are zero/garbage — no
    // magnetometer or temperature here (observed on real hardware), so don't
    // emit them as such. values[] carries the first 7 floats for inspection.
    sendf(R"({"type":"imu","t":%.6f,"a":[%.5f,%.5f,%.5f],"g":[%.5f,%.5f,%.5f],"values":[%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f]})",
          ts,
          imu[0], imu[1], imu[2],
          imu[3], imu[4], imu[5],
          imu[0], imu[1], imu[2], imu[3], imu[4], imu[5], imu[6]);
}

// Non-carina devices: pose mode euler+quat, raw mode gyro/accel/mag/temp.
void on_pose_legacy(float* d, uint64_t ts) {
    long t = now_ms();
    if (t - g_last_pose_ms.load() < 1000 / POSE_SEND_HZ) return;
    g_last_pose_ms = t;
    sendf(R"({"type":"euler","t":%llu,"roll":%.4f,"pitch":%.4f,"yaw":%.4f,"qw":%.6f,"qx":%.6f,"qy":%.6f,"qz":%.6f})",
          (unsigned long long)ts, d[0], d[1], d[2], d[3], d[4], d[5], d[6]);
}

void on_raw_legacy(float* d, uint64_t ts, uint64_t /*vsync*/) {
    long t = now_ms();
    if (t - g_last_imu_ms.load() < 1000 / IMU_SEND_HZ) return;
    g_last_imu_ms = t;
    sendf(R"({"type":"imu","t":%llu,"g":[%.5f,%.5f,%.5f],"a":[%.5f,%.5f,%.5f],"m":[%.4f,%.4f,%.4f],"temp":%.2f})",
          (unsigned long long)ts, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9]);
}

std::atomic<int> g_baseline_dispmode{-1};
std::atomic<bool> g_lock_dispmode{false};

void on_state(int id, int value) {
    static const char* names[] = {"brightness", "volume", "dispmode", "film", "dof"};
    const char* name = (id >= 0 && id <= 4) ? names[id] : "unknown";
    sendf(R"({"type":"state","id":%d,"name":"%s","value":%d})", id, name, value);
    fprintf(stderr, "[state] %s = %d (0x%x)\n", name, value, value);

    // Optional guard against accidental 2D/3D toggles (double-tap on the
    // right temple switches to 3840-wide SBS and stretches the desktop).
    if (id == 2 && g_lock_dispmode && g_baseline_dispmode > 0 && value != g_baseline_dispmode) {
        fprintf(stderr, "[lock] display mode changed to 0x%x, restoring 0x%x\n",
                value, g_baseline_dispmode.load());
        xr_device_provider_set_display_mode(g_provider, g_baseline_dispmode);
        sendf(R"({"type":"log","level":"warn","text":"display mode change reverted [lock active]"})");
    }
    if (id == 2 && g_baseline_dispmode < 0) g_baseline_dispmode = value;
}

// ---- discovery -----------------------------------------------------------------

std::vector<int> scan_viture_pids() {
    std::vector<int> pids;
    DIR* dir = opendir("/sys/bus/usb/devices");
    if (!dir) return pids;
    dirent* e;
    while ((e = readdir(dir))) {
        std::string base = std::string("/sys/bus/usb/devices/") + e->d_name;
        FILE* fv = fopen((base + "/idVendor").c_str(), "r");
        if (!fv) continue;
        char vendor[8] = {0};
        if (fgets(vendor, sizeof(vendor), fv) && strncmp(vendor, "35ca", 4) == 0) {
            FILE* fp = fopen((base + "/idProduct").c_str(), "r");
            if (fp) {
                unsigned pid = 0;
                if (fscanf(fp, "%x", &pid) == 1) pids.push_back(int(pid));
                fclose(fp);
            }
        }
        fclose(fv);
    }
    closedir(dir);
    return pids;
}

void poll_device_state(bool force = false) {
    static int last[4] = {-1000, -1000, -1000, -1000};
    if (!g_provider) return;
    if (force) for (int i = 0; i < 4; i++) last[i] = -1000; // re-announce for late-joining clients
    int vals[4] = {
        xr_device_provider_get_brightness_level(g_provider),
        xr_device_provider_get_volume_level(g_provider),
        xr_device_provider_get_display_mode(g_provider),
        -1,
    };
    // Stop querying getters that repeatedly fail (e.g. film mode returns -4 on
    // Luma Ultra) so the SDK's error logging doesn't flood the console.
    static int film_failures = 0;
    if (film_failures < 3) {
        float film = 0;
        if (xr_device_provider_get_film_mode(g_provider, &film) == 0) {
            vals[3] = int(film * 100);
            film_failures = 0;
        } else {
            film_failures++;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (vals[i] >= 0 && vals[i] != last[i]) {
            last[i] = vals[i];
            on_state(i, vals[i]);
        }
    }
}

void handle_client_message(const std::string& msg) {
    if (msg.find("\"reset_pose\"") != std::string::npos && g_provider &&
        g_device_type == XR_DEVICE_TYPE_VITURE_CARINA) {
        int r = reset_pose_carina(g_provider);
        log_msg("info", r == 0 ? "pose reset" : "pose reset failed");
    }
}

void on_signal(int) { g_running = false; }

} // namespace

int main(int argc, char** argv) {
    uint16_t port = 8765;
    int imu_freq = 2; // viture::protocol::Imu::Frequency::MEDIUM = 120 Hz
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = uint16_t(atoi(argv[++i]));
        else if (!strcmp(argv[i], "--imu-freq") && i + 1 < argc) imu_freq = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--verbose")) xr_device_provider_set_log_level(3);
        else if (!strcmp(argv[i], "--lock-display-mode")) g_lock_dispmode = true;
        else {
            printf("usage: %s [--port N] [--imu-freq 0..4] [--lock-display-mode] [--verbose]\n", argv[0]);
            return 0;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    // -- find the glasses
    auto pids = scan_viture_pids();
    int pid = 0;
    for (int p : pids) {
        if (xr_device_provider_is_product_id_valid(p)) { pid = p; break; }
    }
    if (pid == 0) {
        fprintf(stderr, "No supported VITURE glasses found (VID 35ca%s)\n",
                pids.empty() ? " not present" : ", but no supported PID");
        return 1;
    }
    printf("Found VITURE device pid=0x%04x\n", pid);

    char market[64] = {0};
    int mlen = sizeof(market);
    xr_device_provider_get_market_name(pid, market, &mlen);

    // -- provider lifecycle (mirrors XRLinuxDriver's sequence)
    xr_device_provider_set_log_level(1);
    g_provider = xr_device_provider_create(pid);
    if (!g_provider) {
        fprintf(stderr, "xr_device_provider_create failed (permissions? see 70-viture-xr.rules)\n");
        return 1;
    }

    int dtype = xr_device_provider_get_device_type(g_provider);
    g_device_type = dtype;
    printf("Device: %s (type=%d%s)\n", market[0] ? market : "VITURE",
           dtype, dtype == XR_DEVICE_TYPE_VITURE_CARINA ? ", Carina/6DoF" : "");

    int reg;
    if (dtype == XR_DEVICE_TYPE_VITURE_CARINA) {
        reg = register_callbacks_carina(g_provider, nullptr, on_vsync_carina, on_imu_carina, nullptr);
    } else {
        reg = register_pose_callback(g_provider, on_pose_legacy);
        register_raw_callback(g_provider, on_raw_legacy);
    }
    if (reg != 0) {
        fprintf(stderr, "SDK callback registration failed (%d)\n", reg);
        return 1;
    }

    if (xr_device_provider_initialize(g_provider, nullptr) != 0) {
        fprintf(stderr, "provider initialize failed\n");
        return 1;
    }

    if (!g_ws.start(port, handle_client_message)) {
        fprintf(stderr, "WebSocket server failed to bind 127.0.0.1:%u\n", port);
        return 1;
    }
    printf("WebSocket listening on ws://127.0.0.1:%u\n", port);

    sleep(1); // SDK needs a beat between initialize and start (per XRLinuxDriver)
    if (xr_device_provider_start(g_provider) != 0) {
        fprintf(stderr, "provider start failed\n");
        g_ws.stop();
        return 1;
    }

    // Instant device events (button presses, mode toggles) in addition to the
    // 2 s polling. XRLinuxDriver notes the SDK may not always deliver these;
    // polling remains the fallback.
    xr_device_provider_register_state_callback(g_provider, on_state);

    if (dtype != XR_DEVICE_TYPE_VITURE_CARINA) {
        // pose mode for orientation stream; raw mode also carries gyro/accel/mag
        if (open_imu(g_provider, 1 /*MODE_POSE*/, uint8_t(imu_freq)) != 0)
            log_msg("warn", "open_imu(POSE) failed");
        if (open_imu(g_provider, 0 /*MODE_RAW*/, uint8_t(imu_freq)) != 0)
            log_msg("warn", "open_imu(RAW) failed (raw gyro/accel unavailable)");
    }

    char fw[128] = {0};
    int fwlen = sizeof(fw);
    xr_device_provider_get_glasses_version(g_provider, fw, &fwlen);

    // hello broadcast for late joiners too, every 2s alongside state polling
    long last_hello = 0;
    while (g_running) {
        long t = now_ms();
        if (t - last_hello > 2000) {
            last_hello = t;
            static int cycle = 0;
            sendf(R"({"type":"hello","model":"%s","market_name":"%s","pid":%d,"firmware":"%s","device_type":%d})",
                  market[0] ? market : "VITURE", market, pid, fw, dtype);
            poll_device_state(++cycle % 5 == 0);
        }
        usleep(100000);
    }

    printf("\nshutting down…\n");
    if (dtype != XR_DEVICE_TYPE_VITURE_CARINA) {
        close_imu(g_provider, 1);
        close_imu(g_provider, 0);
    }
    xr_device_provider_stop(g_provider);
    xr_device_provider_shutdown(g_provider);
    xr_device_provider_destroy(g_provider);
    g_ws.stop();
    return 0;
}
