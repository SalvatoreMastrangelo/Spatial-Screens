#include "gesture_client.h"
#include "gesture_parse.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

constexpr double GESTURE_INFER_HZ = 15.0;

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

bool GestureClient::start(const std::string& socket_path, const std::string& script_path,
                           double connect_timeout_s) {
    std::lock_guard<std::mutex> lock(mutex_);
    unlink(socket_path.c_str());

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        fprintf(stderr, "gestures: socket() failed: %s\n", strerror(errno));
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(listen_fd_, 1) != 0) {
        fprintf(stderr, "gestures: bind/listen failed: %s\n", strerror(errno));
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    char* argv[] = {
        const_cast<char*>("python3"),
        const_cast<char*>(script_path.c_str()),
        const_cast<char*>("--socket"),
        const_cast<char*>(socket_path.c_str()),
        nullptr,
    };
    int rc = posix_spawnp(&child_pid_, "python3", nullptr, nullptr, argv, environ);
    if (rc != 0) {
        fprintf(stderr, "gestures: failed to spawn sidecar (%s) — gesture control disabled\n",
                strerror(rc));
        child_pid_ = -1;
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    pollfd pfd{ listen_fd_, POLLIN, 0 };
    int pres = ::poll(&pfd, 1, int(connect_timeout_s * 1000));
    if (pres <= 0) {
        fprintf(stderr, "gestures: sidecar did not connect within %.1fs — gesture control disabled\n",
                connect_timeout_s);
        stop_locked();
        return false;
    }
    conn_fd_ = accept(listen_fd_, nullptr, nullptr);
    if (conn_fd_ < 0) {
        fprintf(stderr, "gestures: accept() failed: %s\n", strerror(errno));
        stop_locked();
        return false;
    }
    fcntl(conn_fd_, F_SETFL, O_NONBLOCK);

    enabled_ = true;
    printf("gestures: sidecar connected (%s)\n", script_path.c_str());
    return true;
}

void GestureClient::maybe_send_frame(const uint8_t* gray8, int width, int height, double timestamp) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return;
    double t = now_s();
    if (t - last_send_s_ < 1.0 / GESTURE_INFER_HZ) return;
    last_send_s_ = t;

    const size_t data_len = size_t(width) * size_t(height);
    const uint32_t payload_len = uint32_t(8 + 4 + 4 + 1 + data_len);

    std::string msg;
    msg.resize(4 + payload_len);
    uint8_t* p = reinterpret_cast<uint8_t*>(&msg[0]);
    memcpy(p, &payload_len, 4);             p += 4;
    memcpy(p, &timestamp, 8);               p += 8;
    int32_t w = width, h = height;
    memcpy(p, &w, 4);                       p += 4;
    memcpy(p, &h, 4);                       p += 4;
    *p = 0; /* format: GRAY8, per Task 1 */ p += 1;
    memcpy(p, gray8, data_len);

    // A 640x480 GRAY8 frame (~307KB) exceeds the default Unix-domain-socket
    // send buffer (~208KB usable on this system) — confirmed on hardware:
    // send() reliably accepts only a partial write (~219KB) on the very
    // first frame. A single non-retrying send() call (as this used to be)
    // silently drops the unsent tail, which desyncs the sidecar's
    // length-prefixed frame parsing from the very next frame onward — it
    // ends up trying to read a bogus multi-gigabyte "length" reconstructed
    // from misaligned pixel bytes and hangs forever. Loop until the whole
    // message is sent; on EAGAIN (buffer momentarily full), wait briefly
    // for room via poll() rather than treating it as fatal.
    //
    // send_deadline_s is a single aggregate 200ms deadline for this entire
    // call, not a per-iteration timeout: this function runs under mutex_,
    // which poll()/stop() on the main render thread also take, so a
    // persistently-slow-draining peer must not be able to extend our hold
    // on the lock indefinitely by trickling out partial writes (each of
    // which would otherwise reset a fresh 200ms clock).
    const double send_deadline_s = now_s() + 0.2;
    size_t sent_total = 0;
    while (sent_total < msg.size()) {
        double t_now = now_s();
        if (t_now >= send_deadline_s) {
            fprintf(stderr, "gestures: send() timed out waiting for sidecar to drain — disabling gesture control\n");
            enabled_ = false;
            return;
        }
        ssize_t sent = send(conn_fd_, msg.data() + sent_total, msg.size() - sent_total, MSG_NOSIGNAL);
        if (sent > 0) {
            sent_total += size_t(sent);
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int timeout_ms = int((send_deadline_s - t_now) * 1000.0);
            if (timeout_ms < 0) timeout_ms = 0;
            pollfd pfd{ conn_fd_, POLLOUT, 0 };
            if (::poll(&pfd, 1, timeout_ms) > 0) continue;
            fprintf(stderr, "gestures: send() timed out waiting for sidecar to drain — disabling gesture control\n");
            enabled_ = false;
            return;
        }
        fprintf(stderr, "gestures: send() failed (%s) — disabling gesture control\n", strerror(errno));
        enabled_ = false;
        return;
    }
}

GestureEvent GestureClient::poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_) return last_event_;

    char buf[8192];
    for (;;) {
        ssize_t n = recv(conn_fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            recv_buf_.append(buf, size_t(n));
            continue;
        }
        if (n == 0) {
            fprintf(stderr, "gestures: sidecar closed the connection — disabling gesture control\n");
            enabled_ = false;
            break;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "gestures: recv() failed (%s) — disabling gesture control\n", strerror(errno));
            enabled_ = false;
        }
        break;
    }

    size_t nl;
    while ((nl = recv_buf_.find('\n')) != std::string::npos) {
        last_event_ = parse_event(recv_buf_.substr(0, nl));
        recv_buf_.erase(0, nl + 1);
    }
    return last_event_;
}

void GestureClient::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_locked();
}

// Actual teardown logic. Callers must already hold mutex_ — factored out
// so start()'s internal failure-cleanup paths can reuse it without
// recursively locking (std::mutex is not recursive).
void GestureClient::stop_locked() {
    if (child_pid_ > 0) {
        kill(child_pid_, SIGTERM);
        int status;
        for (int i = 0; i < 10; i++) {
            if (waitpid(child_pid_, &status, WNOHANG) != 0) { child_pid_ = -1; break; }
            usleep(100 * 1000);
        }
        if (child_pid_ > 0) { kill(child_pid_, SIGKILL); waitpid(child_pid_, &status, 0); }
        child_pid_ = -1;
    }
    if (conn_fd_ >= 0) { close(conn_fd_); conn_fd_ = -1; }
    if (listen_fd_ >= 0) { close(listen_fd_); listen_fd_ = -1; }
    enabled_ = false;
}
