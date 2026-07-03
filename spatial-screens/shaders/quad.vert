#version 450

// Push-constant layout shared with quad.frag and QuadDraw in vk_renderer.h.
layout(push_constant) uniform PC {
    mat4 mvp;     // column-major, Vulkan clip space (y-down, z in [0,1])
    vec4 color;
    vec4 rect;    // cx, cy, half_w, half_h in quad-local meters
    vec4 flags;   // x: 1 = sample texture, 0 = solid color
} pc;

layout(location = 0) out vec2 v_uv;

// Two CCW triangles from gl_VertexIndex — no vertex buffers anywhere.
void main() {
    const int idx[6] = int[6](0, 1, 2, 0, 2, 3);
    const vec2 c[4] = vec2[4](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, 1));
    vec2 p = c[idx[gl_VertexIndex]];
    // Row 0 of the capture buffer is the top of the source screen; top of
    // the quad (p.y = +1) must sample v = 0.
    v_uv = vec2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    vec2 local = pc.rect.xy + p * pc.rect.zw;
    gl_Position = pc.mvp * vec4(local, 0.0, 1.0);
}
