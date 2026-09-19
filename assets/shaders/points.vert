#version 330 core
// Point sprites: the starfield (directions at infinity), the Sun's particle
// volume and the structure's dot markers. Colours come from the theme:
//   uMode 0 (stars):   aColor.r = temperature 0..1 -> cool / mid / warm ramp
//   uMode 1 (Sun):     aColor.r = heat 0..1        -> falloff / mid / hot ramp
//   uMode 2 (markers): flat uTint
// aColor.a is always the per-point brightness.

layout(location = 0) in vec3  aPos;
layout(location = 1) in float aSize;   // pixels at DPI 1.0
layout(location = 2) in vec4  aColor;

uniform mat4  uView;
uniform mat4  uProj;
uniform mat4  uModel;      // used when uInfinite == 0
uniform float uInfinite;   // 1 = aPos is a direction at infinity (starfield)
uniform float uPixelScale; // DPI scale
uniform float uTime;       // seconds, for flicker
uniform vec3  uEye;        // eye position relative to the camera target
uniform float uFacing;     // 1 = dim particles on the far side of their sphere (Sun)
uniform float uFlicker;    // flicker amplitude (0 = steady)
uniform float uMode;
uniform vec3  uRampLow;    // stars: cool   | Sun: falloff
uniform vec3  uRampMid;    // stars: mid    | Sun: mid
uniform vec3  uRampHigh;   // stars: warm   | Sun: hot
uniform vec3  uTint;       // markers

out vec4 vColor;

float hash(float n) { return fract(sin(n) * 43758.5453); }

vec3 ramp3(float t) {
    return t < 0.5 ? mix(uRampLow, uRampMid, t * 2.0) : mix(uRampMid, uRampHigh, (t - 0.5) * 2.0);
}

void main() {
    float h = hash(float(gl_VertexID) * 12.9898 + 1.0);
    float b = aColor.a;

    if (uInfinite > 0.5) {
        // Rotation only (w = 0): stars never move with camera translation and
        // are pushed to the far plane so they are never clipped by it.
        gl_Position = uProj * vec4(mat3(uView) * aPos, 0.0);
        gl_Position.z = gl_Position.w * 0.99999;
    } else {
        vec4 world = uModel * vec4(aPos, 1.0);
        gl_Position = uProj * uView * world;
        // Far-side particles are dimmer, so the sphere reads as a volume.
        vec3 centre = (uModel * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
        vec3 radial = world.xyz - centre;
        float rl = length(radial);
        float facing = rl > 1e-6 ? dot(radial / rl, normalize(uEye - centre)) * 0.5 + 0.5 : 0.5;
        b *= mix(1.0, mix(0.22, 1.0, facing), uFacing);
    }

    // Slow per-particle flicker: each point has its own rate and phase.
    b *= 1.0 + uFlicker * sin(uTime * (0.6 + 2.2 * h) + h * 6.2831853);

    vec3 rgb = uMode < 1.5 ? ramp3(clamp(aColor.r, 0.0, 1.0)) : uTint;
    gl_PointSize = aSize * uPixelScale;
    vColor = vec4(rgb, b);
}
