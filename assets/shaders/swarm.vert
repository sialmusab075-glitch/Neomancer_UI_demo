#version 330 core
// NEOS layer: one point per asteroid, all in a single draw call.
//
// The CPU hands over heliocentric ecliptic positions in AU (interpolated or exact,
// see neo/sim/SwarmField); the scale mapping the planets use is applied HERE, so 42,666
// points cost the CPU nothing per frame beyond the upload:
//
//   compressed:  render = axes(dir) * k * log10(1 + d * c)        (ScaleMapper::toRender)
//   true:        render = axes(p) * unitsPerAU
//   axes:        ecliptic (x, y, z) -> render (x, z, -y)
//
// Colour and brightness come from the legend (neo/sim/SwarmLegend.h, same ranges).

layout(location = 0) in vec3 aEcl;  // AU, heliocentric ecliptic
layout(location = 1) in vec3 aAttr; // log10 diameter (km), PHA (0/1), days to the next approach (< 0: none)

uniform mat4  uViewProj;
uniform vec3  uTarget;      // camera target in render units
uniform float uTrueScale;   // 0 compressed, 1 true
uniform float uK;
uniform float uC;
uniform float uUnitsPerAU;
uniform vec3  uEarth;       // Earth, heliocentric ecliptic AU
uniform int   uLegend;      // 0 distance from Earth, 1 PHA, 2 diameter, 3 time to approach
uniform vec3  uColNear;     // the bright end of the ramp
uniform vec3  uColMid;
uniform vec3  uColFar;      // the dim end
uniform float uPixelScale;
uniform float uIntensity;

out vec3  vColor;
out float vAlpha;

vec3 toRender(vec3 ecl) {
    vec3 axes = vec3(ecl.x, ecl.z, -ecl.y);
    if (uTrueScale > 0.5) {
        return axes * uUnitsPerAU;
    }
    float d = length(ecl);
    if (d <= 0.0) {
        return vec3(0.0);
    }
    return axes / d * (uK * log(1.0 + d * uC) * 0.4342944819);
}

void main() {
    gl_Position = uViewProj * vec4(toRender(aEcl) - uTarget, 1.0);

    float t = 0.0; // 0 = the dim end of the legend, 1 = the bright end
    if (uLegend == 0) {
        float d = length(aEcl - uEarth);
        t = 1.0 - clamp(log(max(d, 1e-5) / 0.01) / log(500.0), 0.0, 1.0); // 0.01 .. 5 AU
    } else if (uLegend == 1) {
        t = aAttr.y;
    } else if (uLegend == 2) {
        t = aAttr.x < -8.0 ? 0.0 : clamp((aAttr.x + 2.0) / 3.0, 0.0, 1.0); // 10 m .. 10 km
    } else {
        t = aAttr.z < 0.0 ? 0.0 : 1.0 - clamp(aAttr.z / 365.0, 0.0, 1.0);  // now .. a year
    }
    vColor = t < 0.5 ? mix(uColFar, uColMid, t * 2.0) : mix(uColMid, uColNear, (t - 0.5) * 2.0);
    vAlpha = 0.30 + 0.70 * t;
    gl_PointSize = (1.7 + 2.3 * t) * uPixelScale;
}
