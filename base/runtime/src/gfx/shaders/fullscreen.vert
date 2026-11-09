#version 450
// Full-screen triangle. v_dst is the destination position in [0,1]^2 (0,0 top left, as the image rows run).
layout(location = 0) out vec2 v_dst;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_dst = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
