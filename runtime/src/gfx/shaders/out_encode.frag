#version 450
layout(location = 0) in vec2 v_dst;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_src;
layout(push_constant) uniform PushConstants {
    vec4 pic;    // the picture rectangle in destination UV: origin xy, size zw (0,0,1,1 when the picture fills the target)
    vec4 crop;   // the source crop in texels: origin xy, size zw
    vec4 src;    // xy: the whole source size in texels, z: 1 = flip the picture vertically
    vec4 p;      // x: mode (0 copy, 1 linear to sRGB, 2 linear to HDR10, 3 linear to scRGB), y: paper white nits, z: peak nits
} pc;

// IEC 61966-2-1 sRGB OETF of clamp(x, 0, 1).
vec3 srgb_oetf(vec3 x) {
    x = clamp(x, 0.0, 1.0);
    vec3 lo = x * 12.92;
    vec3 hi = 1.055 * pow(x, vec3(1.0 / 2.4)) - 0.055;
    return vec3(x.r <= 0.0031308 ? lo.r : hi.r, x.g <= 0.0031308 ? lo.g : hi.g, x.b <= 0.0031308 ? lo.b : hi.b);
}

// SMPTE ST 2084 (PQ) inverse EOTF: nits in [0, 10000] to the 0..1 signal.
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
    vec2 local = (v_dst - pc.pic.xy) / pc.pic.zw;
    if (any(lessThan(local, vec2(0.0))) || any(greaterThan(local, vec2(1.0)))) {
        o_color = vec4(0.0, 0.0, 0.0, 1.0);   // letterbox bar (black in every mode)
        return;
    }
    if (pc.src.z > 0.5) local.y = 1.0 - local.y;   // flip: a window with the lower-left origin mode is presented upside down otherwise
    // texel-space position inside the crop; clamped to the crop's texel centres so a linear filter never reads outside it
    vec2 texel = pc.crop.xy + local * pc.crop.zw;
    texel = clamp(texel, pc.crop.xy + 0.5, pc.crop.xy + pc.crop.zw - 0.5);
    vec3 rgb = textureLod(u_src, texel / pc.src.xy, 0.0).rgb;

    int mode = int(pc.p.x + 0.5);
    vec3 outc;
    if (mode == 1) {
        outc = srgb_oetf(rgb);
    } else if (mode == 2) {
        // scene-linear BT.709 with 1.0 = paper white -> BT.2020 -> nits, limited to the peak, then PQ
        vec3 r2020 = vec3(dot(vec3(0.6274, 0.3293, 0.0433), rgb),
                          dot(vec3(0.0691, 0.9195, 0.0114), rgb),
                          dot(vec3(0.0164, 0.0880, 0.8956), rgb));
        vec3 nits = clamp(min(r2020 * pc.p.y, vec3(pc.p.z)), 0.0, 10000.0);
        outc = pq_encode(nits);
    } else if (mode == 3) {
        outc = min(rgb * pc.p.y, vec3(pc.p.z)) / 80.0;   // scRGB: 1.0 = 80 nits, limited to the peak like HDR10
    } else {
        outc = rgb;                  // mode 0 (and any other value): the texel unchanged
    }
    o_color = vec4(outc, 1.0);
}
