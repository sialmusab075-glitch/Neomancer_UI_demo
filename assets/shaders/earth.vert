#version 330 core
// The Earth and its atmosphere shell. The view is centred on the Earth, so
// positions are already camera-target-relative (see Camera.h).

layout(location = 0) in vec3 aPos; // unit sphere, object space (it turns with the planet)
layout(location = 1) in vec2 aUv;  // equirectangular: u = longitude, v = 0 at the north pole

uniform mat4 uModel;    // tilt * spin * scale(radius)
uniform mat4 uViewProj;

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUv;

void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    // The world normal of a sphere point is its rotated position. Lighting uses
    // this, so it does not depend on the spin; only the texture coordinates do.
    vNormal = mat3(uModel) * aPos;
    vUv = aUv;
    gl_Position = uViewProj * world;
}
