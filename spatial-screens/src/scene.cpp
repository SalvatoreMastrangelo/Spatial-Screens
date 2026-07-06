#include "scene.h"

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
    // yaw(-azimuth): +azimuth = user's right = -y rotation in EUS.
    Quat rot = qmul(quat_axis_angle(0, 1, 0, -s.cfg.azimuth),
                    quat_axis_angle(1, 0, 0, s.cfg.elevation));
    out_q = qmul(rack_q, rot);
    Vec3 fwd = qrot(out_q, {0, 0, -1});
    float d = s.cfg.distance * dist_scale;
    out_p = { rack_p.x + fwd.x * d, rack_p.y + fwd.y * d, rack_p.z + fwd.z * d };
}
