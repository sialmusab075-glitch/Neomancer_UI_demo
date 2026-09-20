#version 330 core
// Atmosphere shell: a slightly larger sphere drawn additively. The glow is
// brightest where the shell is seen face-on (mostly hidden behind the planet)
// and falls to zero at its own silhouette, which leaves a soft halo beyond the
// Earth's limb.

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUv;

uniform vec3  uCameraPos;
uniform vec3  uSunDir;
uniform vec3  uAtmoColor;
uniform float uIntensity;

out vec4 fragColor;

void main() {
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCameraPos - vWorldPos);
    float facing = max(dot(N, V), 0.0);
    float glow = pow(facing, 3.2);
    // Brighter on the day side, faint but present on the night side.
    float sun = 0.22 + 0.78 * smoothstep(-0.35, 0.55, dot(N, uSunDir));
    fragColor = vec4(uAtmoColor * glow * sun * uIntensity, 1.0);
}
