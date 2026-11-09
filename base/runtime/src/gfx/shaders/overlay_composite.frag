#version 450
layout(location = 0) in vec2 v_dst;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_ui;
layout(push_constant) uniform PushConstants {
    vec4 p;      // x: mode (2 HDR10, 3 scRGB), y: paper white nits, z: peak nits
} pc;

vec3 srgb_eotf(vec3 x) {
    x = clamp(x, 0.0, 1.0);
    vec3 lo = x / 12.92;
    vec3 hi = pow((x + 0.055) / 1.055, vec3(2.4));
    return vec3(x.r <= 0.04045 ? lo.r : hi.r, x.g <= 0.04045 ? lo.g : hi.g, x.b <= 0.04045 ? lo.b : hi.b);
}

const float PQ_M1 = 2610.0 / 16384.0;
const float PQ_M2 = 2523.0 / 4096.0 * 128.0;
const float PQ_C1 = 3424.0 / 4096.0;
const float PQ_C2 = 2413.0 / 4096.0 * 32.0;
const float PQ_C3 = 2392.0 / 4096.0 * 32.0;
vec3 pq_encode(vec3 nits) {
    vec3 y = clamp(nits / 10000.0, 0.0, 1.0);
    vec3 ym = pow(y, vec3(PQ_M1));
    return pow((vec3(PQ_C1) + PQ_C2 * ym) / (vec3(1.0) + PQ_C3 * ym), vec3(PQ_M2));
}

void main() {
    vec4 texel = texture(u_ui, v_dst);
    float a = texel.a;
    if (a <= 0.0) {
        o_color = vec4(0.0);   // nothing drawn here: the blend leaves the swapchain image unchanged
        return;
    }
    vec3 straight = clamp(texel.rgb / a, 0.0, 1.0);
    vec3 lin = srgb_eotf(straight);
    int mode = int(pc.p.x + 0.5);
    vec3 outc;
    if (mode == 2) {
        vec3 r2020 = vec3(dot(vec3(0.6274, 0.3293, 0.0433), lin),
                          dot(vec3(0.0691, 0.9195, 0.0114), lin),
                          dot(vec3(0.0164, 0.0880, 0.8956), lin));
        vec3 nits = clamp(min(r2020 * pc.p.y, vec3(pc.p.z)), 0.0, 10000.0);
        outc = pq_encode(nits);
    } else if (mode == 3) {
        outc = lin * pc.p.y / 80.0;   // scRGB: 1.0 = 80 nits, not clamped
    } else {
        outc = lin;
    }
    o_color = vec4(outc, a);
}
