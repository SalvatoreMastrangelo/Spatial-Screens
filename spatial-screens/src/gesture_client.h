#pragma once

#include <cstdint>
#include <string>
#include <sys/types.h>

// Hand-gesture events from the Python/MediaPipe sidecar. See
// spatial-screens/gestures/ and
// docs/specs/2026-07-03-hand-gesture-control-design.md.
//
// Optional feature: if the sidecar can't be started, GestureClient reports
// enabled() == false and poll() always returns a default (not-present)
// event. Callers must not treat gestures as a dependency.
struct GestureEvent {
    bool present = false;
    bool pinching = false;   // pinch_norm < PINCH_THRESHOLD
    float pinch_x = 0.f, pinch_y = 0.f; // normalized [0,1] pinch midpoint
    std::string pose;        // "open_palm" | "fist" | "point" | "none" | ""
};

class GestureClient {
public:
    // Starts listening on socket_path, spawns `python3 script_path --socket
    // socket_path`, and waits up to connect_timeout_s for it to connect.
    // Returns false (and logs why) on any failure; the object stays safely
    // usable in the disabled state either way.
    bool start(const std::string& socket_path, const std::string& script_path,
               double connect_timeout_s = 5.0);

    // Rate-limited to GESTURE_INFER_HZ; safe to call every render frame.
    // gray8 must be width*height bytes, one byte per pixel.
    void maybe_send_frame(const uint8_t* gray8, int width, int height, double timestamp);

    // Non-blocking. Drains all buffered events and returns the newest;
    // returns the last-known event (or a default one) if nothing new
    // arrived since the last poll(), or if gestures are disabled.
    GestureEvent poll();

    // Terminates the sidecar and closes the socket. Safe to call even if
    // start() failed or was never called.
    void stop();

    bool enabled() const { return enabled_; }

private:
    bool enabled_ = false;
    int listen_fd_ = -1;
    int conn_fd_ = -1;
    pid_t child_pid_ = -1;
    double last_send_s_ = 0.0;
    GestureEvent last_event_;
    std::string recv_buf_;
};
