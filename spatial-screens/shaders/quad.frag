#version 450

layout(push_constant) uniform PC {
    mat4 mvp;
    vec4 color;
    vec4 rect;
    vec4 flags;
} pc;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    // flags.y = 1: clip the quad to an inscribed circle (status dot).
    if (pc.flags.y > 0.5 && length(v_uv - vec2(0.5)) > 0.5) discard;
    // Textured (the captured screen) is always opaque: the capture buffer's
    // alpha channel is undefined (BGRX), so force a=1. Only solid quads (the
    // overlay dots) use the push-constant alpha, enabling per-dot transparency.
    o_color = pc.flags.x > 0.5
        ? vec4((texture(u_tex, v_uv) * pc.color).rgb, 1.0)
        : pc.color;
}
