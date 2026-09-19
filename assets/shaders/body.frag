#version 330 core
// Lambert lighting from the Sun plus a thin fresnel rim for the holographic look.
// The Sun itself is emissive (uEmissive = 1), with limb darkening so it reads as a sphere.

in vec3 vWorldPos;
in vec3 vNormal;

uniform vec3  uColor;
uniform vec3  uLightPos;    // Sun position (target-relative)
uniform vec3  uCameraPos;   // eye position (target-relative)
uniform vec3  uRimColor;
uniform float uEmissive;    // 0 = lit by the Sun, 1 = self-luminous
uniform float uRimStrength; // 1 = normal, >1 = selected

out vec4 fragColor;

void main() {
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCameraPos - vWorldPos);
    vec3 L = normalize(uLightPos - vWorldPos);
    float NdotV = max(dot(N, V), 0.0);

    float lambert = max(dot(N, L), 0.0);
    vec3 lit = uColor * (0.14 + 0.86 * lambert);
    vec3 emissive = uColor * (0.55 + 0.45 * NdotV);
    vec3 base = mix(lit, emissive, uEmissive);

    float fresnel = pow(1.0 - NdotV, 4.0);
    vec3 rim = uRimColor * fresnel * mix(0.8, 0.35, uEmissive) * uRimStrength;

    fragColor = vec4(base + rim, 1.0);
}
