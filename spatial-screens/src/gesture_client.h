#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <sys/types.h>

// Hand-gesture events from the Python/MediaPipe sidecar. See
// spatial-screens/gestures/ and
// docs/specs/2026-07-03-hand-gesture-control-design.md.
//
// Optional feature: if the sidecar can't be started, GestureClient reports
// enabled() == false and poll() always returns a default (not-present)
// event. Callers must not treat gestures as a dependency.
// One hand's classified state from the Python/MediaPipe sidecar.
struct HandState {
    bool present = false;
    bool pinching = false;   // pinch_norm < PINCH_THRESHOLD
    float pinch_x = 0.f, pinch_y = 0.f; // normalized [0,1] pinch midpoint
    std::string pose;        // "open_palm" | "fist" | "point" | "none" | ""
    float landmarks[21][2] = {}; // MediaPipe hand, normalized [0,1] image coords (x-right, y-down); thumb tip = [4], index tip = [8]
    bool has_landmarks = false;  // true iff a full 21-point array parsed
    bool has_depth = false;      // true iff the sidecar sent a fused stereo depth
    float depth = 0.f;           // meters, rough-scaled (see camera-fusion design)
};

// Both hands from one event. See gestures/protocol.py encode_event.
struct GestureEvent {
    HandState left, right;
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
    // `left` and `right` must each be width*height bytes (GRAY8), one byte per
    // pixel — the two tracking-camera planes, sent together as one frame.
    void maybe_send_frame(const uint8_t* left, const uint8_t* right,
                          int width, int height, double timestamp);

    // Non-blocking. Drains all buffered events and returns the newest;
    // returns the last-known event (or a default one) if nothing new
    // arrived since the last poll(), or if gestures are disabled.
    GestureEvent poll();

    // Terminates the sidecar and closes the socket. Safe to call even if
    // start() failed or was never called.
    void stop();

    bool enabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return enabled_;
    }

private:
    // Actual teardown logic, factored out of stop() so start()'s internal
    // failure-cleanup paths can call it while already holding mutex_
    // (std::mutex isn't recursive, so start() can't call the public,
    // lock-taking stop() without deadlocking itself).
    void stop_locked();

    // Guards all fields below. maybe_send_frame() may be called from an
    // SDK-internal capture thread (on_camera_carina) while poll()/stop()
    // run on the main render-loop thread.
    mutable std::mutex mutex_;
    bool enabled_ = false;
    int listen_fd_ = -1;
    int conn_fd_ = -1;
    pid_t child_pid_ = -1;
    double last_send_s_ = 0.0;
    GestureEvent last_event_;
    std::string recv_buf_;
};
