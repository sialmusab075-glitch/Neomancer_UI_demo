#version 330 core
// Reference structure: range rings, ticks, spokes, fragments, and the Sun's
// billboarded halo/crown. Plain lines with per-vertex alpha and the same depth
// fade as the grid and orbits. Used with line.frag.

layout(location = 0) in vec3  aPos;
layout(location = 1) in float aAlpha;

uniform mat4  uViewProj;
uniform mat4  uModel;    // places the geometry relative to the camera target
uniform vec3  uEye;      // eye position relative to the camera target
uniform float uFadeNear;
uniform float uFadeFar;
uniform float uFadeMin;

out float vAlpha;

void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    float d = length(world.xyz - uEye);
    vAlpha = aAlpha * mix(1.0, uFadeMin, smoothstep(uFadeNear, uFadeFar, d));
    gl_Position = uViewProj * world;
}
