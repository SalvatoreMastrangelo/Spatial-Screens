#include "capture_portal.h"
#include "capture.h"

#include <dbus/dbus.h>

#include <chrono>
#include <cstdio>
#include <cstring>

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
