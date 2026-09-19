#version 330 core
// Final composite of the 3D view: scene + bloom, a soft shoulder so HDR cores
// don't clip flat, a gentle colour grade (scaled by WARMTH), and the optional
// "finish" (vignette + faint animated noise).

in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform float uBloomIntensity; // 0 = bloom off
uniform float uFinish;         // 1 = vignette + noise
uniform float uTime;
uniform vec2  uResolution;     // pixels

// Grade (theme tokens).
uniform float uWarmth;         // 0 = no grade .. 1 = full grade
uniform vec3  uShadowLift;     // added to the darkest tones
uniform vec3  uHighlight;      // near-white the brightest tones are pulled toward
uniform float uRedLimit;       // saturation above this is removed from red-dominant pixels

const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);

vec3 grade(vec3 c) {
    float lum = dot(c, kLuma);
    // Lift the shadows slightly toward the theme's shadow colour.
    vec3 g = c + uShadowLift * (1.0 - smoothstep(0.0, 0.25, lum));
    // Keep highlights near-white so bloom reads white-hot rather than saturated.
    float peak = max(g.r, max(g.g, g.b));
    float hl = smoothstep(0.55, 0.95, peak);
    g = mix(g, uHighlight * peak, hl * 0.45);
    // Clamp the saturation of red-dominant colours.
    float mn = min(g.r, min(g.g, g.b));
    peak = max(g.r, max(g.g, g.b));
    float sat = peak > 1e-4 ? (peak - mn) / peak : 0.0;
    float redDominant = step(max(g.g, g.b), g.r);
    float excess = max(sat - uRedLimit, 0.0) * redDominant;
    g = mix(g, vec3(dot(g, kLuma)), clamp(excess, 0.0, 1.0));
    return g;
}

void main() {
    vec3 c = texture(uScene, vUv).rgb + texture(uBloom, vUv).rgb * uBloomIntensity;

    // Linear up to 0.8, then an exponential shoulder approaching 1.0.
    vec3 over = max(c - 0.8, 0.0);
    c = min(c, vec3(0.8)) + 0.2 * (1.0 - exp(-over / 0.2));

    c = mix(c, grade(c), clamp(uWarmth, 0.0, 1.0));

    if (uFinish > 0.5) {
        // Soft vignette: aspect-corrected distance from the centre.
        vec2 q = (vUv - 0.5) * vec2(uResolution.x / uResolution.y, 1.0);
        float v = smoothstep(0.95, 0.30, length(q));
        c *= mix(0.70, 1.0, v);
        // ~1.5% animated noise to break up gradient banding.
        float n = fract(sin(dot(gl_FragCoord.xy + fract(uTime * 7.0) * vec2(113.0, 71.0), vec2(12.9898, 78.233))) * 43758.5453);
        c += (n - 0.5) * 0.015;
    }
    fragColor = vec4(c, 1.0);
}
