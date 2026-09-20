#version 330 core
// Flyby path: a polyline widened into screen-space quads by orbit.geom (which
// carries the fade through) and shaded by orbit.frag.

layout(location = 0) in vec3  aPos;  // render units, relative to the Earth
layout(location = 1) in float aFade; // 1 at closest approach, fading to the path ends

uniform mat4 uViewProj;

out float vFade;

void main() {
    vFade = aFade;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
