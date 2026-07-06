// Multi-screen scene for spatial-screens: resolves configured screens (or a
// default rack) against logical monitors into renderable instances — UV
// sub-rect of the shared capture texture + a world pose per screen. Pure
// logic (no X11/Vulkan) so stereo-math-test covers it.
// See docs/specs/2026-07-05-stereo-3d-design.md §3.
#pragma once
#include <vector>
#include "config.h"
#include "pose_math.h"

struct MonRect { std::string name; int x = 0, y = 0, w = 0, h = 0; };

struct ScreenInst {
    ScreenCfg cfg;
    float uv[4] = {0, 0, 1, 1};  // u0,v0,u1,v1 into the shared texture
    float aspect = 16.f / 10.f;  // monitor pixel aspect
};

// cfg empty -> default rack over `monitors` in order (up to 4): center
// 0°@0.75m, sides ∓35°@1.2m, top +25°el@1.5m. Configured screens resolve
// monitors by name; misses warn to stderr and are dropped. fb is the
// captured framebuffer rect that UVs are relative to.
std::vector<ScreenInst> scene_build(const std::vector<ScreenCfg>& cfg,
                                    const std::vector<MonRect>& monitors,
                                    const MonRect& fb);

// World pose: rotate (yaw then pitch) the rack forward axis by
// azimuth/elevation, walk distance*dist_scale along it from the rack origin;
// the screen faces back along that axis.
void scene_screen_pose(const ScreenInst& s, const Quat& rack_q, const Vec3& rack_p,
                       float dist_scale, Quat& out_q, Vec3& out_p);

// Inverse of scene_screen_pose's override branch: express a world pose as a
// rack-relative (pose_ori, pose_pos) so scene_screen_pose reproduces it.
void world_to_rack_frame(const Quat& rack_q, const Vec3& rack_p,
                         const Quat& world_q, const Vec3& world_p,
                         Quat& out_ori, Vec3& out_pos);

// Gaze-center pick: index of the screen whose direction from head_p is most
// aligned with head-forward (head_q · -z) and within cone_deg; -1 if none.
// A screen coincident with the head (zero direction) is skipped.
int pick_gaze_screen(const std::vector<Vec3>& screen_pos,
                     const Vec3& head_p, const Quat& head_q, float cone_deg);
