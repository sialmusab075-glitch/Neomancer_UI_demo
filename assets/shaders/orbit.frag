#version 330 core
// Drawn with additive blending (GL_SRC_ALPHA, GL_ONE). uColor.rgb may exceed 1
// so bright lines feed the bloom pass.

uniform vec4  uColor;
uniform float uWidth; // pixels
uniform float uSoft;  // 0 = crisp line, 1 = soft halo (falls off across the width)

noperspective in float gEdge;
in float gFade;

out vec4 fragColor;

void main() {
    float halfW = 0.5 * uWidth + 1.0;
    float distPx = abs(gEdge) * halfW;
    // Crisp: full coverage inside the nominal width, 1 px linear falloff outside it.
    float crisp = clamp(0.5 * uWidth + 0.5 - distPx, 0.0, 1.0);
    // Halo: smooth quadratic falloff from the centre to the edge.
    float t = 1.0 - clamp(abs(gEdge), 0.0, 1.0);
    float soft = t * t;
    float coverage = mix(crisp, soft, uSoft);
    fragColor = vec4(uColor.rgb, uColor.a * coverage * gFade);
}
