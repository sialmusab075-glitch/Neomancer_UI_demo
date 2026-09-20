#version 330 core
// Solid disc for a measured diameter, ring for an estimated one. Drawn with
// blending (GL_SRC_ALPHA, GL_ONE); rgb above 1 feeds the bloom.

in vec4  vColor;
in float vHollow;

uniform float uIntensity;

out vec4 fragColor;

void main() {
    float d = length(gl_PointCoord - vec2(0.5)) * 2.0; // 0 centre .. 1 edge
    float edge = 1.0 - smoothstep(0.80, 1.0, d);
    float ring = smoothstep(0.52, 0.68, d);
    float coverage = mix(edge * (0.55 + 0.45 * (1.0 - d)), edge * (ring + 0.10), vHollow);
    if (coverage <= 0.003) {
        discard;
    }
    fragColor = vec4(vColor.rgb * uIntensity, vColor.a * coverage);
}
