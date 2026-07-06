#include "config.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool mkdir_recursive(const std::string& path) {
    // Walk the path component by component from the first '/', creating missing levels.
    // Ignore EEXIST; on any other failure, warn and return false.
    for (size_t i = 1; i <= path.length(); ++i) {
        if (i < path.length() && path[i] != '/') continue;
        std::string component = path.substr(0, i);
        if (mkdir(component.c_str(), 0755) != 0) {
            if (errno != EEXIST) {
                fprintf(stderr, "state: cannot create %s: %s\n", component.c_str(), strerror(errno));
                return false;
            }
        }
    }
    return true;
}

std::string xdg_dir(const char* env, const char* home_suffix) {
    const char* v = getenv(env);
    if (v && *v) return v;
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + home_suffix;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool parse_float(const std::string& key, const std::string& v, float& out) {
    char* end = nullptr;
    float f = strtof(v.c_str(), &end);
    if (end == v.c_str() || *end != '\0') {
        fprintf(stderr, "config: %s: not a number: '%s' (ignored)\n", key.c_str(), v.c_str());
        return false;
    }
    out = f;
    return true;
}

bool parse_bool(const std::string& v) { return v == "true" || v == "1" || v == "yes"; }

// Shared line-format parser for config and state files. std::getline grows
// the line buffer as needed, so an over-long value (e.g. a restore-token)
// is never split across lines the way a fixed fgets() buffer would split it.
void parse_kv_file(const std::string& path, bool warn_missing,
                   bool (*apply)(void*, const std::string&, const std::string&), void* ctx) {
    std::ifstream f(path);
    if (!f) {
        if (warn_missing) fprintf(stderr, "config: cannot open %s\n", path.c_str());
        return;
    }
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        lineno++;
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) {
            fprintf(stderr, "config: %s:%d: expected 'key = value' (skipped)\n",
                    path.c_str(), lineno);
            continue;
        }
        std::string key = trim(s.substr(0, eq)), val = trim(s.substr(eq + 1));
        if (!apply(ctx, key, val))
            fprintf(stderr, "config: %s:%d: unknown key '%s' (skipped)\n",
                    path.c_str(), lineno, key.c_str());
    }
}

}  // namespace

std::string config_default_path() {
    return xdg_dir("XDG_CONFIG_HOME", "/.config") + "/spatial-screens.conf";
}

std::string state_file_path() {
    return xdg_dir("XDG_STATE_HOME", "/.local/state") + "/spatial-screens/state";
}

// "screen.N.field" (N 1-based, <= 16). Grows o.screens to N; unknown fields
// and malformed indices report unknown-key (caller warns with line info).
static bool set_screen_option(Options& o, const std::string& k, const std::string& v) {
    size_t dot2 = k.find('.', 7);          // after "screen."
    if (dot2 == std::string::npos || dot2 == 7) return false;
    int n = atoi(k.substr(7, dot2 - 7).c_str());
    if (n < 1 || n > 16) return false;
    if (size_t(n) > o.screens.size()) o.screens.resize(size_t(n));
    ScreenCfg& s = o.screens[size_t(n) - 1];
    std::string f = k.substr(dot2 + 1);
    if (f == "monitor") s.monitor = v;
    else if (f == "azimuth") parse_float(k, v, s.azimuth);
    else if (f == "elevation") parse_float(k, v, s.elevation);
    else if (f == "distance") parse_float(k, v, s.distance);
    else if (f == "size") parse_float(k, v, s.size);
    else return false;
    return true;
}

bool set_option(Options& o, const std::string& k, const std::string& v) {
    if (k == "monitor") o.monitor = v;
    else if (k == "capture") o.capture = v;
    else if (k == "capture-backend") o.capture_backend = v;
    else if (k == "capture-hz") {
        float f;
        if (parse_float(k, v, f)) o.capture_hz = f < 1 ? 1 : (f > 240 ? 240 : int(f));
    }
    else if (k == "distance") parse_float(k, v, o.distance);
    else if (k == "size") parse_float(k, v, o.size);
    else if (k == "pitch-trim") parse_float(k, v, o.pitch_trim);
    else if (k == "predict-ms") parse_float(k, v, o.predict_ms);
    else if (k == "smooth-pos") parse_float(k, v, o.smooth_pos);
    else if (k == "smooth-ori") parse_float(k, v, o.smooth_ori);
    else if (k == "window") o.window = parse_bool(v);
    else if (k == "ws-port") { float f; if (parse_float(k, v, f)) o.ws_port = int(f); }
    else if (k == "stereo") o.stereo = parse_bool(v);
    else if (k == "ipd-mm") parse_float(k, v, o.ipd_mm);
    else if (k == "workspace") o.workspace = v;
    else if (k.rfind("screen.", 0) == 0) return set_screen_option(o, k, v);
    else return false;
    return true;
}

void load_options_file(const std::string& path, Options& o, bool warn_missing) {
    parse_kv_file(path, warn_missing,
                  [](void* ctx, const std::string& k, const std::string& v) {
                      return set_option(*static_cast<Options*>(ctx), k, v);
                  }, &o);
}

void load_state(AppState& s) {
    parse_kv_file(state_file_path(), false,
                  [](void* ctx, const std::string& k, const std::string& v) {
                      AppState& st = *static_cast<AppState*>(ctx);
                      if (k == "distance") parse_float(k, v, st.distance);
                      else if (k == "size") parse_float(k, v, st.size);
                      else if (k == "restore-token") st.restore_token = v;
                      else if (k == "rack-distance-scale") parse_float(k, v, st.rack_distance_scale);
                      else if (k == "rack-size-scale") parse_float(k, v, st.rack_size_scale);
                      else return false;
                      return true;
                  }, &s);
}

bool save_state(const AppState& s) {
    std::string path = state_file_path();
    std::string dir = path.substr(0, path.find_last_of('/'));
    if (!mkdir_recursive(dir)) return false;
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) {
        fprintf(stderr, "state: cannot write %s\n", tmp.c_str());
        return false;
    }
    fprintf(f, "# written by spatial-screens — do not edit while it runs\n");
    if (s.distance > 0) fprintf(f, "distance = %.3f\n", s.distance);
    if (s.size > 0) fprintf(f, "size = %.1f\n", s.size);
    if (s.rack_distance_scale > 0) fprintf(f, "rack-distance-scale = %.3f\n", s.rack_distance_scale);
    if (s.rack_size_scale > 0) fprintf(f, "rack-size-scale = %.3f\n", s.rack_size_scale);
    if (!s.restore_token.empty()) fprintf(f, "restore-token = %s\n", s.restore_token.c_str());
    fclose(f);
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        fprintf(stderr, "state: rename to %s failed\n", path.c_str());
        return false;
    }
    return true;
}
