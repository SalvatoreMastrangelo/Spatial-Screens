// xdg-desktop-portal ScreenCast session plumbing (libdbus-1) + the PipeWire
// capture backend built on it. The D-Bus dance is blocking and startup-only;
// Start()'s wait is generous because the user may be interacting with the
// system monitor-picker dialog. Exposed separately from capture.h so
// capture-test can probe the portal without a backend.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class CaptureBackend;

struct PortalSession {
    void* conn = nullptr;          // DBusConnection*, opaque to callers
    std::string session_handle;
    uint32_t node_id = 0;
    int pw_fd = -1;                // caller owns; portal_close_session() does not close it
    std::string restore_token;    // new token from Start ("" if the portal sent none)
};

// old_token: previous restore token ("" for none) — skips the picker dialog
// when the portal accepts it. Returns false (with the reason on stderr) on
// any failure, including the user cancelling the dialog.
bool portal_open_screencast(const std::string& old_token, PortalSession& out);
void portal_close_session(PortalSession& s);

// Task 5: the PipeWire-backed CaptureBackend. max_hz seeds the negotiated
// framerate's default (the compositor delivers at most this on damage).
std::unique_ptr<CaptureBackend> capture_create_portal(
    const std::string& old_token,
    std::function<void(const std::string&)> on_new_token,
    int max_hz = 120);
