// Options resolution for spatial-screens: compiled defaults < config file
// (~/.config/spatial-screens.conf, user-authored, read-only) < state file
// (~/.local/state/spatial-screens/state, app-written) < CLI flags.
// See docs/specs/2026-07-04-m3-remainder-design.md §2.
#pragma once
#include <string>
#include <vector>
#include "pose_math.h"   // Vec3, Quat for the per-screen pose override

// One virtual screen in the rack (multi-screen stereo). Azimuth: + = to the
// user's right; elevation: + = up; distance in meters from the rack origin;
// size = diagonal inches. monitor names a logical monitor (VS1..VSn).
struct ScreenCfg {
    std::string monitor;
    float azimuth = 0.f, elevation = 0.f;
    float distance = 0.75f;
    float size = 24.f;
    // Free-placement override (set once a gesture selects/moves this screen).
    // Stored relative to the rack origin so recenter moves it with the rack.
    // When set, scene_screen_pose ignores azimuth/elevation/distance/dist_scale.
    bool has_pose_override = false;
    Vec3 pose_pos;   // world position in the rack frame
    Quat pose_ori;   // world orientation in the rack frame
};

struct Options {
    std::string monitor;                  // glasses output ("" = autodetect)
    std::string capture;                  // capture monitor ("" = first non-glasses)
    std::string capture_backend = "auto"; // auto|portal|xshm|test
    int capture_hz = 30;                  // mirrored-content update rate (1..240)
    float distance = 0.75f;               // meters
    float size = 24.f;                    // diagonal inches
    float pitch_trim = 0.f;               // degrees
    float predict_ms = 0.f;
    std::string predict_mode = "vsync";   // off|fixed|vsync (head-pose prediction)
    float scanout_ms = 5.f;               // extra sample-to-photon term (vsync mode)
    float predict_cap_ms = 35.f;          // clamp on the prediction horizon
    float predict_scale = 1.f;            // [0,1] fraction of the gated horizon actually applied
    float ang_dead = 2.f;                 // predict-gate angular deadband (deg/frame)
    float ang_ramp = 20.f;                // predict-gate angular ramp (deg/frame)
    float ori_motion_cap = 0.95f;         // orientation filter transparency ceiling during motion
    float smooth_pos = 0.10f;
    float smooth_ori = 0.40f;
    bool window = false;
    int ws_port = 8765;                   // 0 = telemetry disabled
    bool stereo = true;                   // SBS 3D on the glasses; false = legacy mono
    float ipd_mm = 63.f;                  // interpupillary distance (no SDK API for it)
    std::string workspace = "2x2";        // run.sh logical-monitor grid ("off" = don't touch)
    std::vector<ScreenCfg> screens;       // empty = default rack from monitor count
};

// Live-tuned values + portal restore token. Whole file rewritten atomically
// on clean exit and whenever a new restore token arrives.
struct AppState {
    float distance = -1.f;                // < 0 = unset
    float size = -1.f;
    std::string restore_token;
    float rack_distance_scale = -1.f;     // < 0 = unset (multi-screen rack multipliers)
    float rack_size_scale = -1.f;
};

std::string config_default_path();  // $XDG_CONFIG_HOME|~/.config + /spatial-screens.conf
std::string state_file_path();      // $XDG_STATE_HOME|~/.local/state + /spatial-screens/state

// Applies one "key value" pair using the config-key spelling (e.g.
// "capture-backend"). Returns false for unknown keys; warns to stderr (and
// still returns true) for unparsable numeric values.
bool set_option(Options& o, const std::string& key, const std::string& value);

// INI-style "key = value", '#' comments, no sections. Missing file is fine
// (warn only if warn_missing); bad lines warn and are skipped.
void load_options_file(const std::string& path, Options& o, bool warn_missing);

void load_state(AppState& s);
bool save_state(const AppState& s);
