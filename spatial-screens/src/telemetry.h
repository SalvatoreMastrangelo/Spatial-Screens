// WebSocket telemetry speaking the viture-bridge protocol (documented in
// bridge/main.cpp) so the phase-1 sensor-viz dashboard monitors
// spatial-screens unmodified, plus one new message type:
//   {"type":"app", fps, sixdof, anchored, distance, size, backend, direct, rss}
// The bridge and spatial-screens never run together (single-client SDK), so
// both default to port 8765. All send_* methods rate-limit internally and
// no-op when disabled — safe to call every frame.
#pragma once
#include <atomic>
#include <cstdint>

#include "ws_server.hpp"

class Telemetry {
public:
    // port 0 = disabled by request. Bind failure warns and stays disabled —
    // telemetry is never fatal.
    bool start(uint16_t port);
    void stop();
    bool enabled() const { return enabled_; }

    void send_hello(const char* market, int pid, const char* firmware, int device_type); // <= 1/2s
    void send_pose(const float p[3], const float q[4], double t);                        // <= 60 Hz
    void send_app(float fps, bool sixdof, bool anchored, float distance,
                  float size_in, const char* backend, bool direct, int rss_mb);          // <= 2 Hz
    // Unthrottled. text must not contain quotes or backslashes (no escaping,
    // same contract as the bridge's log messages).
    void log(const char* level, const char* text);

    // True once per dashboard {"type":"reset_pose"} request (consume-on-read;
    // set on the WS thread, read on the render loop).
    bool reset_requested() { return reset_req_.exchange(false); }

private:
    wsrv::Server ws_;
    std::atomic<bool> reset_req_{false};
    bool enabled_ = false;
    long last_hello_ms_ = 0, last_pose_ms_ = 0, last_app_ms_ = 0;
};
