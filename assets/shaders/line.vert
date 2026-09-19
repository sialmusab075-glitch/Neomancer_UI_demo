#version 330 core
// Ecliptic grid. Vertices arrive flat (y = 0) in world render units; the
// gravity well and Jupiter's dip are applied here so the dip can move.

layout(location = 0) in vec3 aPos;
layout(location = 1) in float aAlpha; // line weight incl. rim fade

uniform mat4  uViewProj;
uniform vec3  uTarget;     // camera target (world render units)
uniform vec3  uEye;        // eye position relative to the target

// Sun well: y = yOffset - K * (1/sqrt(r^2+s^2) - 1/sqrt(R^2+s^2))  (see EclipticGrid.h)
uniform float uWellK;
uniform float uWellSoft;
uniform float uWellRim;
uniform float uYOffset;
// Jupiter: small Gaussian dip that follows the planet.
uniform vec3  uDipPos;
uniform float uDipK;
uniform float uDipWidth;

// Depth fade: full strength up to uFadeNear from the eye, down to uFadeMin at uFadeFar.
uniform float uFadeNear;
uniform float uFadeFar;
uniform float uFadeMin;

out float vAlpha;

void main() {
    vec3 p = aPos;
    float r = length(p.xz);
    float s2 = uWellSoft * uWellSoft;
    p.y = uYOffset - uWellK * (inversesqrt(r * r + s2) - inversesqrt(uWellRim * uWellRim + s2));
    vec2 dj = p.xz - uDipPos.xz;
    p.y -= uDipK * exp(-dot(dj, dj) / (2.0 * uDipWidth * uDipWidth));

    vec3 rel = p - uTarget;
    float d = length(rel - uEye);
    float depthFade = mix(1.0, uFadeMin, smoothstep(uFadeNear, uFadeFar, d));

    vAlpha = aAlpha * depthFade;
    gl_Position = uViewProj * vec4(rel, 1.0);
}
