#version 330 core
// Soft round point sprite. Used with additive blending.

in vec4 vColor;

uniform float uIntensity;

out vec4 fragColor;

void main() {
    float d = length(gl_PointCoord - vec2(0.5)) * 2.0; // 0 centre .. 1 edge
    float a = 1.0 - smoothstep(0.35, 1.0, d);
    if (a <= 0.0) {
        discard;
    }
    fragColor = vec4(vColor.rgb * vColor.a * a * uIntensity, 1.0);
}
