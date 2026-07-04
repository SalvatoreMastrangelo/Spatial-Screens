#include "capture_portal.h"
#include "capture.h"

#include <dbus/dbus.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>
#include <vector>

// ---------------------------------------------------- D-Bus plumbing ----

namespace {

constexpr const char* PORTAL_DEST = "org.freedesktop.portal.Desktop";
constexpr const char* PORTAL_PATH = "/org/freedesktop/portal/desktop";
constexpr const char* SCREENCAST_IFACE = "org.freedesktop.portal.ScreenCast";
constexpr const char* REQUEST_IFACE = "org.freedesktop.portal.Request";
constexpr const char* SESSION_IFACE = "org.freedesktop.portal.Session";

std::string unique_name_component(DBusConnection* conn) {
    std::string s = dbus_bus_get_unique_name(conn);  // e.g. ":1.42" -> "1_42"
    if (!s.empty() && s[0] == ':') s.erase(0, 1);
    for (char& c : s) if (c == '.') c = '_';
    return s;
}

// ---- a{sv} builders

void dict_open(DBusMessageIter* it, DBusMessageIter* arr) {
    dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "{sv}", arr);
}
void dict_add(DBusMessageIter* arr, const char* key, int type, const char* sig, const void* val) {
    DBusMessageIter e, v;
    dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, nullptr, &e);
    dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, sig, &v);
    dbus_message_iter_append_basic(&v, type, val);
    dbus_message_iter_close_container(&e, &v);
    dbus_message_iter_close_container(arr, &e);
}
void dict_add_str(DBusMessageIter* a, const char* k, const char* s) { dict_add(a, k, DBUS_TYPE_STRING, "s", &s); }
void dict_add_u32(DBusMessageIter* a, const char* k, uint32_t u) { dict_add(a, k, DBUS_TYPE_UINT32, "u", &u); }
void dict_add_bool(DBusMessageIter* a, const char* k, bool b) { dbus_bool_t v = b; dict_add(a, k, DBUS_TYPE_BOOLEAN, "b", &v); }

// ---- Response signal handling

DBusMessage* wait_response(DBusConnection* conn, const std::string& path_a,
                           const std::string& path_b, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!dbus_connection_read_write(conn, 100)) return nullptr;  // bus died
        while (DBusMessage* m = dbus_connection_pop_message(conn)) {
            if (dbus_message_is_signal(m, REQUEST_IFACE, "Response")) {
                const char* p = dbus_message_get_path(m);
                if (p && (path_a == p || path_b == p)) return m;
            }
            dbus_message_unref(m);
        }
    }
    return nullptr;
}

// Response signature: (u code, a{sv} results). Leaves *results at the array.
bool parse_response(DBusMessage* msg, uint32_t& code, DBusMessageIter* results) {
    DBusMessageIter it;
    if (!dbus_message_iter_init(msg, &it) ||
        dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) return false;
    dbus_message_iter_get_basic(&it, &code);
    if (!dbus_message_iter_next(&it) ||
        dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return false;
    *results = it;
    return true;
}

// Positions *value_out inside the variant of results[key].
bool results_find(DBusMessageIter results_arr, const char* key, DBusMessageIter* value_out) {
    DBusMessageIter arr;
    dbus_message_iter_recurse(&results_arr, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter e;
        dbus_message_iter_recurse(&arr, &e);
        const char* k = nullptr;
        dbus_message_iter_get_basic(&e, &k);
        if (k && !strcmp(k, key)) {
            dbus_message_iter_next(&e);
            dbus_message_iter_recurse(&e, value_out);
            return true;
        }
        dbus_message_iter_next(&arr);
    }
    return false;
}

// One ScreenCast request-style call: AddMatch on the predicted request path,
// send, then wait for Request.Response (portals may return a different
// request handle — accept either path).
DBusMessage* screencast_call(DBusConnection* conn, const char* method,
                             const std::string& token, int timeout_ms,
                             const std::function<void(DBusMessageIter*)>& build_args) {
    std::string sender = unique_name_component(conn);
    std::string predicted = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
    std::string match = std::string("type='signal',interface='") + REQUEST_IFACE +
                        "',member='Response',path='" + predicted + "'";
    dbus_bus_add_match(conn, match.c_str(), nullptr);
    dbus_connection_flush(conn);

    DBusMessage* call = dbus_message_new_method_call(PORTAL_DEST, PORTAL_PATH,
                                                     SCREENCAST_IFACE, method);
    DBusMessageIter it;
    dbus_message_iter_init_append(call, &it);
    build_args(&it);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, call, 5000, &err);
    dbus_message_unref(call);
    if (!reply) {
        fprintf(stderr, "portal: %s failed: %s\n", method, err.message ? err.message : "?");
        dbus_error_free(&err);
        return nullptr;
    }
    std::string actual = predicted;
    DBusMessageIter rit;
    if (dbus_message_iter_init(reply, &rit) &&
        dbus_message_iter_get_arg_type(&rit) == DBUS_TYPE_OBJECT_PATH) {
        const char* rp = nullptr;
        dbus_message_iter_get_basic(&rit, &rp);
        if (rp) actual = rp;
    }
    dbus_message_unref(reply);
    if (actual != predicted) {
        std::string m2 = std::string("type='signal',interface='") + REQUEST_IFACE +
                         "',member='Response',path='" + actual + "'";
        dbus_bus_add_match(conn, m2.c_str(), nullptr);
        dbus_connection_flush(conn);
    }
    DBusMessage* resp = wait_response(conn, predicted, actual, timeout_ms);
    if (!resp) fprintf(stderr, "portal: no Response for %s within %d ms\n", method, timeout_ms);
    return resp;
}

void append_session_and_dict(DBusMessageIter* it, const char* session,
                             const std::function<void(DBusMessageIter*)>& fill) {
    dbus_message_iter_append_basic(it, DBUS_TYPE_OBJECT_PATH, &session);
    DBusMessageIter arr;
    dict_open(it, &arr);
    fill(&arr);
    dbus_message_iter_close_container(it, &arr);
}

}  // namespace

// ------------------------------------------------- portal session API ----

bool portal_open_screencast(const std::string& old_token, PortalSession& out) {
    DBusError err;
    dbus_error_init(&err);
    // Private connection: we pump it ourselves; sharing the process-wide one
    // would steal signals from any other in-process D-Bus user.
    DBusConnection* conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!conn) {
        fprintf(stderr, "portal: session bus unavailable: %s\n", err.message ? err.message : "?");
        dbus_error_free(&err);
        return false;
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    out.conn = conn;

    // 1. CreateSession
    DBusMessage* resp = screencast_call(conn, "CreateSession", "ss_req1", 5000,
        [&](DBusMessageIter* it) {
            DBusMessageIter arr;
            dict_open(it, &arr);
            dict_add_str(&arr, "handle_token", "ss_req1");
            dict_add_str(&arr, "session_handle_token", "ss_session");
            dbus_message_iter_close_container(it, &arr);
        });
    uint32_t code = 1;
    DBusMessageIter results, v;
    if (!resp || !parse_response(resp, code, &results) || code != 0 ||
        !results_find(results, "session_handle", &v)) {
        fprintf(stderr, "portal: CreateSession failed (code %u)\n", code);
        if (resp) dbus_message_unref(resp);
        portal_close_session(out);
        return false;
    }
    const char* sh = nullptr;
    dbus_message_iter_get_basic(&v, &sh);
    out.session_handle = sh ? sh : "";
    dbus_message_unref(resp);

    // 2. SelectSources — monitors only, cursor embedded (XShm never captured
    // the cursor; for a virtual monitor you look at, seeing it is the point),
    // persist_mode 2 (until revoked) + previous restore_token if we have one.
    auto select_args = [&](const char* token, uint32_t cursor_mode) {
        return [&, token, cursor_mode](DBusMessageIter* it) {
            append_session_and_dict(it, out.session_handle.c_str(), [&](DBusMessageIter* arr) {
                dict_add_str(arr, "handle_token", token);
                dict_add_u32(arr, "types", 1);        // 1 = MONITOR
                dict_add_bool(arr, "multiple", false);
                dict_add_u32(arr, "cursor_mode", cursor_mode);  // 2 = EMBEDDED
                dict_add_u32(arr, "persist_mode", 2);
                if (!old_token.empty()) dict_add_str(arr, "restore_token", old_token.c_str());
            });
        };
    };
    resp = screencast_call(conn, "SelectSources", "ss_req2", 5000, select_args("ss_req2", 2));
    bool ok = resp && parse_response(resp, code, &results) && code == 0;
    if (resp) dbus_message_unref(resp);
    if (!ok) {
        // Retry once with a plain hidden cursor in case EMBEDDED is unsupported.
        resp = screencast_call(conn, "SelectSources", "ss_req2b", 5000, select_args("ss_req2b", 1));
        ok = resp && parse_response(resp, code, &results) && code == 0;
        if (resp) dbus_message_unref(resp);
        if (!ok) {
            fprintf(stderr, "portal: SelectSources failed (code %u)\n", code);
            portal_close_session(out);
            return false;
        }
    }

    // 3. Start — long timeout: the picker dialog may be up.
    // Signature: (o session, s parent_window, a{sv} options).
    resp = screencast_call(conn, "Start", "ss_req3", 120000, [&](DBusMessageIter* it) {
        const char* session = out.session_handle.c_str();
        const char* parent = "";
        dbus_message_iter_append_basic(it, DBUS_TYPE_OBJECT_PATH, &session);
        dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &parent);
        DBusMessageIter arr;
        dict_open(it, &arr);
        dict_add_str(&arr, "handle_token", "ss_req3");
        dbus_message_iter_close_container(it, &arr);
    });
    if (!resp || !parse_response(resp, code, &results) || code != 0) {
        fprintf(stderr, "portal: Start failed (code %u)%s\n", code,
                code == 1 ? " — cancelled by user" : "");
        if (resp) dbus_message_unref(resp);
        portal_close_session(out);
        return false;
    }
    if (results_find(results, "restore_token", &v)) {
        const char* tok = nullptr;
        dbus_message_iter_get_basic(&v, &tok);
        if (tok) out.restore_token = tok;
    }
    if (!results_find(results, "streams", &v)) {
        fprintf(stderr, "portal: Start returned no streams\n");
        dbus_message_unref(resp);
        portal_close_session(out);
        return false;
    }
    {
        DBusMessageIter sa;
        dbus_message_iter_recurse(&v, &sa);  // a(ua{sv})
        if (dbus_message_iter_get_arg_type(&sa) != DBUS_TYPE_STRUCT) {
            fprintf(stderr, "portal: empty streams array\n");
            dbus_message_unref(resp);
            portal_close_session(out);
            return false;
        }
        DBusMessageIter st;
        dbus_message_iter_recurse(&sa, &st);
        dbus_message_iter_get_basic(&st, &out.node_id);
    }
    dbus_message_unref(resp);

    // 4. OpenPipeWireRemote — a plain method call (fd in the reply, no Request).
    DBusMessage* call = dbus_message_new_method_call(PORTAL_DEST, PORTAL_PATH,
                                                     SCREENCAST_IFACE, "OpenPipeWireRemote");
    DBusMessageIter it;
    dbus_message_iter_init_append(call, &it);
    append_session_and_dict(&it, out.session_handle.c_str(), [](DBusMessageIter*) {});
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, call, 5000, &err);
    dbus_message_unref(call);
    if (!reply || !dbus_message_get_args(reply, &err, DBUS_TYPE_UNIX_FD, &out.pw_fd,
                                         DBUS_TYPE_INVALID)) {
        fprintf(stderr, "portal: OpenPipeWireRemote failed: %s\n",
                err.message ? err.message : "?");
        dbus_error_free(&err);
        if (reply) dbus_message_unref(reply);
        portal_close_session(out);
        return false;
    }
    dbus_message_unref(reply);
    printf("portal: screencast ready (node %u%s)\n", out.node_id,
           out.restore_token.empty() ? "" : ", restore token received");
    return true;
}

void portal_close_session(PortalSession& s) {
    DBusConnection* conn = static_cast<DBusConnection*>(s.conn);
    if (!conn) return;
    if (!s.session_handle.empty()) {
        DBusMessage* call = dbus_message_new_method_call(
            PORTAL_DEST, s.session_handle.c_str(), SESSION_IFACE, "Close");
        dbus_connection_send(conn, call, nullptr);
        dbus_connection_flush(conn);
        dbus_message_unref(call);
    }
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    s.conn = nullptr;
    s.session_handle.clear();
}

// ------------------------------------------------- PipeWire backend ----

namespace {

class PortalBackend : public CaptureBackend {
public:
    PortalBackend(std::string old_token, std::function<void(const std::string&)> on_new_token)
        : old_token_(std::move(old_token)), on_new_token_(std::move(on_new_token)) {}
    ~PortalBackend() override { stop(); }

    bool start() override {
        if (!portal_open_screencast(old_token_, session_)) return false;
        if (!session_.restore_token.empty() && on_new_token_)
            on_new_token_(session_.restore_token);

        pw_init(nullptr, nullptr);
        loop_ = pw_thread_loop_new("ss-capture", nullptr);
        if (!loop_) {
            fprintf(stderr, "capture(portal): thread loop alloc failed\n");
            return false;
        }
        ctx_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
        if (!loop_ || !ctx_ || pw_thread_loop_start(loop_) != 0) {
            fprintf(stderr, "capture(portal): pipewire loop setup failed\n");
            return false;
        }
        pw_thread_loop_lock(loop_);
        core_ = pw_context_connect_fd(ctx_, fcntl(session_.pw_fd, F_DUPFD_CLOEXEC, 5),
                                      nullptr, 0);
        if (!core_) {
            pw_thread_loop_unlock(loop_);
            fprintf(stderr, "capture(portal): connect_fd failed\n");
            return false;
        }
        stream_ = pw_stream_new(core_, "spatial-screens",
                                pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                                                  PW_KEY_MEDIA_CATEGORY, "Capture",
                                                  PW_KEY_MEDIA_ROLE, "Screen", nullptr));
        if (!stream_) {
            pw_thread_loop_unlock(loop_);
            fprintf(stderr, "capture(portal): stream alloc failed\n");
            return false;
        }
        static const pw_stream_events EVENTS = {
            PW_VERSION_STREAM_EVENTS,
            /*.destroy =*/ nullptr,
            /*.state_changed =*/ &PortalBackend::on_state_changed,
            /*.control_info =*/ nullptr,
            /*.io_changed =*/ nullptr,
            /*.param_changed =*/ &PortalBackend::on_param_changed,
            /*.add_buffer =*/ nullptr,
            /*.remove_buffer =*/ nullptr,
            /*.process =*/ &PortalBackend::on_process,
            /*.drained =*/ nullptr,
            /*.command =*/ nullptr,
            /*.trigger_done =*/ nullptr,
        };
        pw_stream_add_listener(stream_, &listener_, &EVENTS, this);

        // Locals, not &SPA_RECTANGLE(...) temporaries — C++ has no compound literals.
        spa_rectangle sz_def = SPA_RECTANGLE(1920, 1080), sz_min = SPA_RECTANGLE(1, 1),
                      sz_max = SPA_RECTANGLE(8192, 8192);
        spa_fraction fr_def = SPA_FRACTION(30, 1), fr_min = SPA_FRACTION(0, 1),
                     fr_max = SPA_FRACTION(240, 1);
        uint8_t podbuf[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(podbuf, sizeof(podbuf));
        const spa_pod* params[1] = { (const spa_pod*)spa_pod_builder_add_object(&b,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3,
                SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&sz_def, &sz_min, &sz_max),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&fr_def, &fr_min, &fr_max)) };
        alive_ = true;  // BEFORE the wait loop: errors during negotiation clear it
        if (pw_stream_connect(stream_, PW_DIRECTION_INPUT, session_.node_id,
                              (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT |
                                                PW_STREAM_FLAG_MAP_BUFFERS),
                              params, 1) != 0) {
            pw_thread_loop_unlock(loop_);
            fprintf(stderr, "capture(portal): stream connect failed\n");
            return false;
        }
        // Wait for format negotiation so callers can size the texture.
        struct timespec abst;
        pw_thread_loop_get_time(loop_, &abst, 5 * SPA_NSEC_PER_SEC);
        while (!have_format_ && alive_)
            if (pw_thread_loop_timed_wait_full(loop_, &abst) != 0) break;
        pw_thread_loop_unlock(loop_);
        if (!have_format_) {
            fprintf(stderr, "capture(portal): no video format within 5 s\n");
            return false;
        }
        return true;
    }

    // Swap-out under the lock: read_buf_ is owned by the consumer side, so
    // the PipeWire thread can never touch (or reallocate) the bytes behind
    // the pointer we hand out. Returns false when no NEW frame arrived
    // since the last call — the caller just keeps its current texture.
    bool latest_frame(CaptureFrame& out) override {
        std::lock_guard<std::mutex> lk(mtx_);
        if (front_ < 0) return false;
        read_buf_.swap(buf_[front_]);
        front_ = -1;
        out.data = read_buf_.data();
        out.w = w_; out.h = h_; out.pitch = pitch_;
        return true;
    }

    bool alive() const override { return alive_; }
    const char* name() const override { return "portal"; }

    void stop() override {
        if (loop_) {
            pw_thread_loop_lock(loop_);
            if (stream_) { pw_stream_destroy(stream_); stream_ = nullptr; }
            if (core_) { pw_core_disconnect(core_); core_ = nullptr; }
            pw_thread_loop_unlock(loop_);
            pw_thread_loop_stop(loop_);
            if (ctx_) { pw_context_destroy(ctx_); ctx_ = nullptr; }
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
        }
        if (session_.pw_fd >= 0) { close(session_.pw_fd); session_.pw_fd = -1; }
        portal_close_session(session_);
        alive_ = false;
    }

private:
    static void on_param_changed(void* ud, uint32_t id, const spa_pod* param) {
        auto* self = static_cast<PortalBackend*>(ud);
        if (id != SPA_PARAM_Format || !param) return;
        uint32_t mt, mst;
        if (spa_format_parse(param, &mt, &mst) < 0 ||
            mt != SPA_MEDIA_TYPE_video || mst != SPA_MEDIA_SUBTYPE_raw) return;
        spa_video_info_raw info{};
        if (spa_format_video_raw_parse(param, &info) < 0) return;
        {
            std::lock_guard<std::mutex> lk(self->mtx_);
            self->w_ = int(info.size.width);
            self->h_ = int(info.size.height);
            self->pitch_ = info.size.width * 4;  // real stride comes per-buffer
            // Discard any pending frame: it was captured at the OLD size and
            // front_ would otherwise be handed out paired with the NEW w_/h_,
            // an over-read past the end of the (shorter) buffer.
            self->front_ = -1;
        }
        self->have_format_ = true;
        pw_thread_loop_signal(self->loop_, false);
    }

    static void on_state_changed(void* ud, pw_stream_state /*old*/, pw_stream_state st,
                                 const char* error) {
        auto* self = static_cast<PortalBackend*>(ud);
        if (st == PW_STREAM_STATE_ERROR ||
            (st == PW_STREAM_STATE_UNCONNECTED && self->saw_streaming_)) {
            fprintf(stderr, "capture(portal): stream %s%s%s\n",
                    st == PW_STREAM_STATE_ERROR ? "error" : "disconnected",
                    error ? ": " : "", error ? error : "");
            self->alive_ = false;
            pw_thread_loop_signal(self->loop_, false);
        }
        if (st == PW_STREAM_STATE_STREAMING) self->saw_streaming_ = true;
    }

    static void on_process(void* ud) {
        auto* self = static_cast<PortalBackend*>(ud);
        pw_buffer* pb = pw_stream_dequeue_buffer(self->stream_);
        if (!pb) return;
        spa_buffer* sb = pb->buffer;
        if (sb->datas[0].data && self->have_format_) {
            uint32_t stride = sb->datas[0].chunk->stride;
            if (!stride) stride = uint32_t(self->w_) * 4;
            std::lock_guard<std::mutex> lk(self->mtx_);
            int back = self->front_ == 0 ? 1 : 0;
            self->buf_[back].assign(
                static_cast<const uint8_t*>(sb->datas[0].data),
                static_cast<const uint8_t*>(sb->datas[0].data) + size_t(stride) * self->h_);
            self->pitch_ = stride;
            self->front_ = back;
        }
        pw_stream_queue_buffer(self->stream_, pb);
    }

    std::string old_token_;
    std::function<void(const std::string&)> on_new_token_;
    PortalSession session_{};
    pw_thread_loop* loop_ = nullptr;
    pw_context* ctx_ = nullptr;
    pw_core* core_ = nullptr;
    pw_stream* stream_ = nullptr;
    spa_hook listener_{};
    std::atomic<bool> have_format_{false};
    std::atomic<bool> alive_{false};
    bool saw_streaming_ = false;
    std::mutex mtx_;
    std::vector<uint8_t> buf_[2];   // written by the PipeWire thread
    std::vector<uint8_t> read_buf_; // owned by the consumer after swap-out
    int front_ = -1;                // -1 = no unconsumed frame
    int w_ = 0, h_ = 0;
    uint32_t pitch_ = 0;
};

}  // namespace

std::unique_ptr<CaptureBackend> capture_create_portal(
    const std::string& old_token,
    std::function<void(const std::string&)> on_new_token) {
    return std::make_unique<PortalBackend>(std::move(old_token), std::move(on_new_token));
}
