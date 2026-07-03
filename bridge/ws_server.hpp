// Minimal single-header WebSocket server (RFC 6455, text frames, no TLS).
// Localhost-only by design: it exists to stream sensor JSON from the VITURE
// SDK to the sensor-viz web app. Runs its own thread; broadcast() and the
// on-message callback are thread-safe.

#pragma once

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wsrv {

// ---- SHA-1 (needed for the WebSocket handshake accept key) -----------------

struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint64_t total = 0;
    uint8_t buf[64];
    size_t buflen = 0;

    static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                   (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void update(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        total += len;
        while (len) {
            size_t n = std::min(len, sizeof(buf) - buflen);
            std::memcpy(buf + buflen, p, n);
            buflen += n; p += n; len -= n;
            if (buflen == 64) { block(buf); buflen = 0; }
        }
    }

    void final(uint8_t out[20]) {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t zero = 0;
        while (buflen != 56) update(&zero, 1);
        uint8_t lenb[8];
        for (int i = 0; i < 8; i++) lenb[i] = uint8_t(bits >> (56 - i * 8));
        update(lenb, 8);
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 4; j++) out[i * 4 + j] = uint8_t(h[i] >> (24 - j * 8));
    }
};

inline std::string base64(const uint8_t* data, size_t len) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < len) v |= uint32_t(data[i + 1]) << 8;
        if (i + 2 < len) v |= uint32_t(data[i + 2]);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out += i + 2 < len ? tbl[v & 63] : '=';
    }
    return out;
}

// ---- server -----------------------------------------------------------------

class Server {
public:
    using MessageHandler = std::function<void(const std::string&)>;

    bool start(uint16_t port, MessageHandler on_message) {
        on_message_ = std::move(on_message);
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;
        int one = 1;
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::perror("ws bind");
            return false;
        }
        if (listen(listen_fd_, 8) < 0) return false;
        fcntl(listen_fd_, F_SETFL, O_NONBLOCK);
        running_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        for (auto& c : clients_) ::close(c.fd);
        clients_.clear();
        if (listen_fd_ >= 0) ::close(listen_fd_);
    }

    void broadcast(const std::string& text) {
        std::lock_guard<std::mutex> lk(mu_);
        outbox_.push_back(text);
        // Bound memory if no client is draining.
        while (outbox_.size() > 2048) outbox_.pop_front();
    }

    size_t client_count() {
        std::lock_guard<std::mutex> lk(mu_);
        return client_count_;
    }

private:
    struct Client {
        int fd;
        bool handshaken = false;
        std::string inbuf;
    };

    void loop() {
        while (running_) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listen_fd_, &rfds);
            int maxfd = listen_fd_;
            for (auto& c : clients_) { FD_SET(c.fd, &rfds); maxfd = std::max(maxfd, c.fd); }
            timeval tv{0, 5000}; // 5 ms tick keeps broadcast latency low
            select(maxfd + 1, &rfds, nullptr, nullptr, &tv);

            if (FD_ISSET(listen_fd_, &rfds)) accept_new();

            for (auto it = clients_.begin(); it != clients_.end();) {
                bool drop = false;
                if (FD_ISSET(it->fd, &rfds)) drop = !read_client(*it);
                if (drop) { ::close(it->fd); it = clients_.erase(it); }
                else ++it;
            }

            flush_outbox();
            std::lock_guard<std::mutex> lk(mu_);
            client_count_ = clients_.size();
        }
    }

    void accept_new() {
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) return;
        fcntl(fd, F_SETFL, O_NONBLOCK);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        clients_.push_back({fd});
    }

    bool read_client(Client& c) {
        char buf[4096];
        ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
        if (n <= 0) return n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK);
        c.inbuf.append(buf, size_t(n));

        if (!c.handshaken) return do_handshake(c);

        // parse complete frames
        while (true) {
            if (c.inbuf.size() < 2) return true;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(c.inbuf.data());
            uint8_t opcode = p[0] & 0x0F;
            bool masked = p[1] & 0x80;
            uint64_t len = p[1] & 0x7F;
            size_t hdr = 2;
            if (len == 126) {
                if (c.inbuf.size() < 4) return true;
                len = (uint64_t(p[2]) << 8) | p[3];
                hdr = 4;
            } else if (len == 127) {
                if (c.inbuf.size() < 10) return true;
                len = 0;
                for (int i = 0; i < 8; i++) len = (len << 8) | p[2 + i];
                hdr = 10;
            }
            size_t maskoff = hdr;
            if (masked) hdr += 4;
            if (len > 1 << 20) return false; // oversized frame — drop client
            if (c.inbuf.size() < hdr + len) return true;

            std::string payload = c.inbuf.substr(hdr, len);
            if (masked) {
                const uint8_t* mask = p + maskoff;
                for (size_t i = 0; i < payload.size(); i++) payload[i] ^= char(mask[i & 3]);
            }
            c.inbuf.erase(0, hdr + len);

            if (opcode == 0x8) return false;              // close
            if (opcode == 0x9) send_frame(c.fd, payload, 0xA); // ping → pong
            else if (opcode == 0x1 && on_message_) on_message_(payload);
        }
    }

    bool do_handshake(Client& c) {
        size_t end = c.inbuf.find("\r\n\r\n");
        if (end == std::string::npos) return c.inbuf.size() < 16384;
        std::string headers = c.inbuf.substr(0, end);
        c.inbuf.erase(0, end + 4);

        // Header names are case-insensitive (Chrome capitalizes, Node lowercases).
        std::string lower = headers;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return char(std::tolower(ch)); });
        std::string key;
        size_t k = lower.find("sec-websocket-key:");
        if (k != std::string::npos) {
            k += 18;
            while (k < headers.size() && headers[k] == ' ') k++;
            size_t e = headers.find("\r\n", k);
            key = headers.substr(k, (e == std::string::npos ? headers.size() : e) - k);
        }
        if (key.empty()) return false;

        key += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        Sha1 sha;
        sha.update(key.data(), key.size());
        uint8_t digest[20];
        sha.final(digest);
        std::string accept = base64(digest, 20);

        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
        if (::send(c.fd, resp.data(), resp.size(), MSG_NOSIGNAL) != ssize_t(resp.size())) return false;
        c.handshaken = true;
        return true;
    }

    static bool send_frame(int fd, const std::string& payload, uint8_t opcode = 0x1) {
        uint8_t hdr[10];
        size_t hlen;
        hdr[0] = 0x80 | opcode;
        if (payload.size() < 126) {
            hdr[1] = uint8_t(payload.size());
            hlen = 2;
        } else if (payload.size() < 65536) {
            hdr[1] = 126;
            hdr[2] = uint8_t(payload.size() >> 8);
            hdr[3] = uint8_t(payload.size());
            hlen = 4;
        } else {
            hdr[1] = 127;
            uint64_t l = payload.size();
            for (int i = 0; i < 8; i++) hdr[2 + i] = uint8_t(l >> (56 - i * 8));
            hlen = 10;
        }
        if (::send(fd, hdr, hlen, MSG_NOSIGNAL) != ssize_t(hlen)) return false;
        return ::send(fd, payload.data(), payload.size(), MSG_NOSIGNAL) == ssize_t(payload.size());
    }

    void flush_outbox() {
        std::deque<std::string> pending;
        {
            std::lock_guard<std::mutex> lk(mu_);
            pending.swap(outbox_);
        }
        if (pending.empty()) return;
        for (auto it = clients_.begin(); it != clients_.end();) {
            bool ok = true;
            if (it->handshaken) {
                for (const auto& msg : pending) {
                    if (!send_frame(it->fd, msg)) { ok = false; break; }
                }
            }
            if (!ok) { ::close(it->fd); it = clients_.erase(it); }
            else ++it;
        }
    }

    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::vector<Client> clients_;
    std::mutex mu_;
    std::deque<std::string> outbox_;
    size_t client_count_ = 0;
    MessageHandler on_message_;
};

} // namespace wsrv
