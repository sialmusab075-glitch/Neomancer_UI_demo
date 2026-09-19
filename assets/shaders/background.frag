#version 330 core
// Scene background: a subtle radial lift at the centre of the view, the base
// colour in between, and darker corners. Colours come from the theme.

in vec2 vUv;
out vec4 fragColor;

uniform vec3 uCenter;
uniform vec3 uMid;
uniform vec3 uEdge;
uniform vec2 uResolution;

void main() {
    vec2 q = (vUv - 0.5) * vec2(uResolution.x / uResolution.y, 1.0);
    float d = length(q);
    vec3 c = mix(uCenter, uMid, smoothstep(0.0, 0.55, d));
    c = mix(c, uEdge, smoothstep(0.55, 1.05, d));
    fragColor = vec4(c, 1.0);
}
