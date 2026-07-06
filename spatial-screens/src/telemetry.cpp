#include "telemetry.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace {
long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

bool Telemetry::start(uint16_t port) {
    if (port == 0) {
        printf("telemetry: disabled (--ws-port 0)\n");
        return false;
    }
    enabled_ = ws_.start(port, [this](const std::string& msg) {
        if (msg.find("\"reset_pose\"") != std::string::npos) reset_req_ = true;
    });
    if (enabled_)
        printf("telemetry: ws://127.0.0.1:%u (sensor-viz dashboard)\n", port);
    else
        fprintf(stderr, "telemetry: bind failed on port %u — continuing without\n", port);
    return enabled_;
}

void Telemetry::stop() {
    if (enabled_) ws_.stop();
    enabled_ = false;
}

void Telemetry::send_hello(const char* market, int pid, const char* fw, int device_type) {
    if (!enabled_) return;
    long t = now_ms();
    if (t - last_hello_ms_ < 2000) return;
    last_hello_ms_ = t;
    char buf[512];
    snprintf(buf, sizeof(buf),
             R"({"type":"hello","model":"%s","market_name":"%s","pid":%d,"firmware":"%s","device_type":%d,"app":"spatial-screens"})",
             market[0] ? market : "VITURE", market, pid, fw, device_type);
    ws_.broadcast(buf);
}

void Telemetry::send_pose(const float p[3], const float q[4], double t_s) {
    if (!enabled_) return;
    long t = now_ms();
    if (t - last_pose_ms_ < 1000 / 60) return;
    last_pose_ms_ = t;
    char buf[512];
    snprintf(buf, sizeof(buf),
             R"({"type":"pose","t":%.6f,"px":%.5f,"py":%.5f,"pz":%.5f,"qw":%.6f,"qx":%.6f,"qy":%.6f,"qz":%.6f})",
             t_s, p[0], p[1], p[2], q[0], q[1], q[2], q[3]);
    ws_.broadcast(buf);
}

void Telemetry::send_app(float fps, bool sixdof, bool anchored, float distance,
                         float size_in, const char* backend, bool direct, int rss_mb,
                         bool stereo, int screens) {
    if (!enabled_) return;
    long t = now_ms();
    if (t - last_app_ms_ < 500) return;
    last_app_ms_ = t;
    char buf[512];
    snprintf(buf, sizeof(buf),
             R"({"type":"app","fps":%.1f,"sixdof":%s,"anchored":%s,"distance":%.2f,"size":%.2f,"backend":"%s","direct":%s,"rss":%d,"stereo":%s,"screens":%d})",
             fps, sixdof ? "true" : "false", anchored ? "true" : "false",
             distance, size_in, backend, direct ? "true" : "false", rss_mb,
             stereo ? "true" : "false", screens);
    ws_.broadcast(buf);
}

void Telemetry::send_hands(bool lp, bool l_has, float ld, bool rp, bool r_has, float rd) {
    if (!enabled_) return;
    long t = now_ms();
    if (t - last_hands_ms_ < 100) return;   // <= 10 Hz
    last_hands_ms_ = t;
    char buf[256];
    snprintf(buf, sizeof(buf),
             R"({"type":"hands","left_present":%s,"left_depth":%.3f,"right_present":%s,"right_depth":%.3f})",
             lp ? "true" : "false", l_has ? ld : -1.0f,
             rp ? "true" : "false", r_has ? rd : -1.0f);
    ws_.broadcast(buf);
}

void Telemetry::log(const char* level, const char* text) {
    if (!enabled_) return;
    char buf[512];
    snprintf(buf, sizeof(buf), R"({"type":"log","level":"%s","text":"%s"})", level, text);
    ws_.broadcast(buf);
}
