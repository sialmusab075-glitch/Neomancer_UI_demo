#version 330 core
// Orbit rings. Widened into screen-space quads by orbit.geom.

layout(location = 0) in vec3  aPos; // world render units
layout(location = 1) in float aNu;  // true anomaly of this ring vertex (rad)

uniform mat4  uViewProj;
uniform vec3  uTarget;   // camera target (world render units)
uniform vec3  uEye;      // eye position relative to the target

// Depth fade (see line.vert).
uniform float uFadeNear;
uniform float uFadeFar;
uniform float uFadeMin;

// Trail: bright at the body's current true anomaly, dimming to uTrailMin
// ~300 degrees behind it. uDir = +1 when time runs forward, -1 in reverse,
// so "behind" always means "where the body has just been".
uniform float uNu;
uniform float uDir;
uniform float uTrail;    // 1 = trail on, 0 = uniform ring
uniform float uTrailMin;

out float vFade;

void main() {
    vec3 rel = aPos - uTarget;
    float d = length(rel - uEye);
    float depthFade = mix(1.0, uFadeMin, smoothstep(uFadeNear, uFadeFar, d));

    float behind = mod((uNu - aNu) * uDir, 6.28318531); // 0 at the body, grows behind it
    float trail = mix(1.0, uTrailMin, smoothstep(0.0, radians(300.0), behind));

    vFade = depthFade * mix(1.0, trail, uTrail);
    gl_Position = uViewProj * vec4(rel, 1.0);
}
