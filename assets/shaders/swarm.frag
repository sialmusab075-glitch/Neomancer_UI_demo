#version 330 core
// A small soft disc per asteroid. Additive blending (GL_ONE, GL_ONE): a crowd of
// points brightens where it is dense and feeds the bloom pass, a lone one stays faint.

in vec3  vColor;
in float vAlpha;

uniform float uIntensity;

out vec4 fragColor;

void main() {
    float d = length(gl_PointCoord - vec2(0.5)) * 2.0; // 0 centre .. 1 edge
    float cover = 1.0 - smoothstep(0.55, 1.0, d);
    if (cover <= 0.004) {
        discard;
    }
    fragColor = vec4(vColor * (uIntensity * vAlpha * cover), 1.0);
}
