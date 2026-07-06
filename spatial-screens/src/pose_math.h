// Shared quaternion / 4x4 (column-major, OpenGL convention) math for
// spatial-screens. Extracted from main.cpp so the scene/stereo modules and
// their unit tests use the same code that renders.
#pragma once
#include <cmath>
#include <cstring>

struct Quat { float w = 1, x = 0, y = 0, z = 0; };
struct Vec3 { float x = 0, y = 0, z = 0; };

inline Quat qmul(const Quat& a, const Quat& b) {
    return { a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
             a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
}
inline Quat qconj(const Quat& q) { return { q.w, -q.x, -q.y, -q.z }; }

inline Quat yaw_twist(const Quat& q) {
    float m = std::sqrt(q.w * q.w + q.y * q.y);
    if (m < 1e-6f) return {};
    return { q.w / m, 0, q.y / m, 0 };
}

inline Quat quat_axis_angle(float ax, float ay, float az, float deg) {
    float r = deg * float(M_PI) / 180.f * 0.5f;
    float s = std::sin(r);
    return { std::cos(r), ax * s, ay * s, az * s };
}

inline Vec3 qrot(const Quat& q, const Vec3& v) {
    float tx = 2 * (q.y * v.z - q.z * v.y);
    float ty = 2 * (q.z * v.x - q.x * v.z);
    float tz = 2 * (q.x * v.y - q.y * v.x);
    return { v.x + q.w * tx + (q.y * tz - q.z * ty),
             v.y + q.w * ty + (q.z * tx - q.x * tz),
             v.z + q.w * tz + (q.x * ty - q.y * tx) };
}

// Column-major 4x4 from rotation quat + translation (OpenGL convention).
inline void mat_from_pose(const Quat& q, const Vec3& t, float* m) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0] = 1 - 2 * (yy + zz); m[4] = 2 * (xy - wz);     m[8] = 2 * (xz + wy);      m[12] = t.x;
    m[1] = 2 * (xy + wz);     m[5] = 1 - 2 * (xx + zz); m[9] = 2 * (yz - wx);      m[13] = t.y;
    m[2] = 2 * (xz - wy);     m[6] = 2 * (yz + wx);     m[10] = 1 - 2 * (xx + yy); m[14] = t.z;
    m[3] = 0;                 m[7] = 0;                 m[11] = 0;                 m[15] = 1;
}

// out = a * b (column-major 4x4)
inline void mat_mul(const float* a, const float* b, float* out) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            out[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] + a[1 * 4 + r] * b[c * 4 + 1] +
                             a[2 * 4 + r] * b[c * 4 + 2] + a[3 * 4 + r] * b[c * 4 + 3];
}

// Symmetric perspective for Vulkan clip space (y-down, z in [0,1]);
// rr/tt are frustum half-extents at the near plane, as for glFrustum.
inline void mat_projection_vk(float rr, float tt, float n, float f, float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = n / rr;
    m[5] = -n / tt;
    m[10] = f / (n - f);
    m[11] = -1.f;
    m[14] = n * f / (n - f);
}
