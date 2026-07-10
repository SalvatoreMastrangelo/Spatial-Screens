#include "scene.h"

#include <cmath>
#include <cstdio>

namespace {

const struct { float az, el, dist; } RACK_DEFAULT[4] = {
    {0, 0, 0.75f},    // center, near
    {-35, 0, 1.2f},   // left
    {35, 0, 1.2f},    // right
    {0, 25, 1.5f},    // above center, far
};

const MonRect* find_monitor(const std::vector<MonRect>& mons, const std::string& name) {
    for (auto& m : mons)
        if (m.name == name) return &m;
    return nullptr;
}

ScreenInst make_inst(const ScreenCfg& cfg, const MonRect& m, const MonRect& fb) {
    ScreenInst s;
    s.cfg = cfg;
    s.uv[0] = float(m.x - fb.x) / float(fb.w);
    s.uv[1] = float(m.y - fb.y) / float(fb.h);
    s.uv[2] = float(m.x + m.w - fb.x) / float(fb.w);
    s.uv[3] = float(m.y + m.h - fb.y) / float(fb.h);
    s.aspect = m.h > 0 ? float(m.w) / float(m.h) : 16.f / 10.f;
    return s;
}

}  // namespace

std::vector<ScreenInst> scene_build(const std::vector<ScreenCfg>& cfg,
                                    const std::vector<MonRect>& monitors,
                                    const MonRect& fb) {
    std::vector<ScreenInst> out;
    if (cfg.empty()) {
        size_t n = monitors.size() < 4 ? monitors.size() : 4;
        for (size_t i = 0; i < n; i++) {
            ScreenCfg c;
            c.monitor = monitors[i].name;
            c.azimuth = RACK_DEFAULT[i].az;
            c.elevation = RACK_DEFAULT[i].el;
            c.distance = RACK_DEFAULT[i].dist;
            out.push_back(make_inst(c, monitors[i], fb));
        }
        return out;
    }
    for (auto& c : cfg) {
        if (c.source == "window") {
            ScreenInst s;
            s.cfg = c;
            s.uv[0] = 0; s.uv[1] = 0; s.uv[2] = 1; s.uv[3] = 1;
            s.source_index = -1;   // unresolved until a window is bound
            out.push_back(s);
            continue;
        }
        const MonRect* m = find_monitor(monitors, c.monitor);
        if (!m) {
            fprintf(stderr, "scene: monitor '%s' not found — screen skipped\n",
                    c.monitor.c_str());
            continue;
        }
        out.push_back(make_inst(c, *m, fb));
    }
    return out;
}

void scene_screen_pose(const ScreenInst& s, const Quat& rack_q, const Vec3& rack_p,
                       float dist_scale, Quat& out_q, Vec3& out_p) {
    if (s.cfg.has_pose_override) {
        out_q = qmul(rack_q, s.cfg.pose_ori);
        Vec3 w = qrot(rack_q, s.cfg.pose_pos);
        out_p = { rack_p.x + w.x, rack_p.y + w.y, rack_p.z + w.z };
        return;
    }
    // yaw(-azimuth): +azimuth = user's right = -y rotation in EUS.
    Quat rot = qmul(quat_axis_angle(0, 1, 0, -s.cfg.azimuth),
                    quat_axis_angle(1, 0, 0, s.cfg.elevation));
    out_q = qmul(rack_q, rot);
    Vec3 fwd = qrot(out_q, {0, 0, -1});
    float d = s.cfg.distance * dist_scale;
    out_p = { rack_p.x + fwd.x * d, rack_p.y + fwd.y * d, rack_p.z + fwd.z * d };
}

void scene_window_resize(ScreenInst& s, int new_w, int new_h) {
    if (new_w <= 0 || new_h <= 0) return;
    if (s.src_w > 0 && s.src_h > 0) {
        float old_d = std::sqrt(float(s.src_w) * s.src_w + float(s.src_h) * s.src_h);
        float new_d = std::sqrt(float(new_w) * new_w + float(new_h) * new_h);
        if (old_d > 0) s.cfg.size *= new_d / old_d;
    }
    s.src_w = new_w; s.src_h = new_h;
    s.aspect = float(new_w) / float(new_h);
}

void world_to_rack_frame(const Quat& rack_q, const Vec3& rack_p,
                         const Quat& world_q, const Vec3& world_p,
                         Quat& out_ori, Vec3& out_pos) {
    Quat inv = qconj(rack_q);
    Vec3 d = { world_p.x - rack_p.x, world_p.y - rack_p.y, world_p.z - rack_p.z };
    out_pos = qrot(inv, d);
    out_ori = qmul(inv, world_q);
}

int pick_gaze_screen(const std::vector<Vec3>& screen_pos,
                     const Vec3& head_p, const Quat& head_q, float cone_deg) {
    Vec3 fwd = qrot(head_q, {0, 0, -1});
    float cos_cone = std::cos(cone_deg * float(M_PI) / 180.f);
    int best = -1;
    float best_dot = cos_cone;   // must beat the cone edge to qualify
    for (size_t i = 0; i < screen_pos.size(); i++) {
        Vec3 d = { screen_pos[i].x - head_p.x,
                   screen_pos[i].y - head_p.y,
                   screen_pos[i].z - head_p.z };
        float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len < 1e-6f) continue;                 // screen at the head — skip
        float dot = (d.x * fwd.x + d.y * fwd.y + d.z * fwd.z) / len;
        if (dot > best_dot) { best_dot = dot; best = int(i); }
    }
    return best;
}
