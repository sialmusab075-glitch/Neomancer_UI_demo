#version 330 core
// Flyby markers: one point sprite per visible asteroid. Positions are computed on
// the CPU each frame from the simulation clock (they are also what picking uses).

layout(location = 0) in vec3  aPos;    // render units, relative to the Earth
layout(location = 1) in float aSize;   // marker RADIUS in pixels at DPI 1
layout(location = 2) in vec4  aColor;  // rgb + alpha (path fade-in / fade-out)
layout(location = 3) in float aHollow; // 1 = estimated diameter: drawn as a ring

uniform mat4  uViewProj;
uniform float uPixelScale;

out vec4  vColor;
out float vHollow;

void main() {
    gl_Position = uViewProj * vec4(aPos, 1.0);
    gl_PointSize = 2.0 * aSize * uPixelScale;
    vColor = aColor;
    vHollow = aHollow;
}
