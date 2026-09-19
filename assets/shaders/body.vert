#version 330 core
// Planets and the Sun. Positions are camera-target-relative (see Camera.h).

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

uniform mat4 uModel;     // translate(body - target) * scale(radius)
uniform mat4 uViewProj;

out vec3 vWorldPos;
out vec3 vNormal;

void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = mat3(uModel) * aNormal; // uniform scale only, so no inverse-transpose needed
    gl_Position = uViewProj * world;
}
