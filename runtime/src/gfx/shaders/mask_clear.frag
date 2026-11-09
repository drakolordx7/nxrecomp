#version 450
// Masked colour clear (src/gfx/nvn/nvn_mask_clear.cpp): writes the push-constant colour to colour output LOC only. scripts/gen_shaders.py
// compiles one variant per (TYPE, LOC): TYPE vec4 (kind 0, floats), ivec4 (kind 1, int32), uvec4 (kind 2, uint32); LOC 0..7.
// The pass reserves 32 bytes of push constants; the colour is the first 16. A pipeline only enables the colour write mask of its own
// location, so the other channels of that attachment keep their values.
layout(location = LOC) out TYPE o_color;
layout(push_constant) uniform PushConstants {
    TYPE value;   // the first 16 bytes of the push constant range
} pc;
void main() {
    o_color = pc.value;
}
