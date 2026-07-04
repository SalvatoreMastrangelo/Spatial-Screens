#include "gesture_client.h"

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
constexpr float PINCH_THRESHOLD = 0.5f; // tune after Task 9's hands-on test

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Minimal key-based scanners for the fixed, known event schema (see
// protocol.py's encode_event). Not a general JSON parser — deliberately
// so, to avoid adding a JSON library dependency for a 5-field schema we
// control on both ends.
bool json_find_bool(const std::string& s, const char* key, bool& out) {
    std::string pat = std::string("\"") + key + "\":";
    auto pos = s.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    out = s.compare(pos, 4, "true") == 0;
    return true;
}

bool json_find_number(const std::string& s, const char* key, float& out) {
    std::string pat = std::string("\"") + key + "\":";
    auto pos = s.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    out = strtof(s.c_str() + pos, nullptr);
    return true;
}

bool json_find_string(const std::string& s, const char* key, std::string& out) {
    std::string pat = std::string("\"") + key + "\":\"";
    auto pos = s.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    auto end = s.find('"', pos);
    if (end == std::string::npos) return false;
    out = s.substr(pos, end - pos);
    return true;
}

// pinch_pos is emitted as "pinch_pos":[x,y] — json_find_number would stop
// at the array's leading '[', so it needs its own extractor.
bool json_find_pair(const std::string& s, const char* key, float& x, float& y) {
    std::string pat = std::string("\"") + key + "\":[";
    auto pos = s.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    x = strtof(s.c_str() + pos, nullptr);
    auto comma = s.find(',', pos);
    if (comma == std::string::npos) return false;
    y = strtof(s.c_str() + comma + 1, nullptr);
    return true;
}

GestureEvent parse_event(const std::string& line) {
    GestureEvent ev;
    json_find_bool(line, "present", ev.present);
    float pinch_norm = 999.f;
    json_find_number(line, "pinch_norm", pinch_norm);
    ev.pinching = ev.present && pinch_norm < PINCH_THRESHOLD;
    json_find_pair(line, "pinch_pos", ev.pinch_x, ev.pinch_y);
    json_find_string(line, "pose", ev.pose);
    return ev;
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

    ssize_t sent = send(conn_fd_, msg.data(), msg.size(), MSG_NOSIGNAL);
    if (sent < 0) {
        fprintf(stderr, "gestures: send() failed (%s) — disabling gesture control\n", strerror(errno));
        enabled_ = false;
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
