#version 330 core
// Expands each line segment into a quad `uWidth` pixels wide (plus 1 px of
// feather on each side for antialiasing). Works for GL_LINES, GL_LINE_STRIP
// and GL_LINE_LOOP draws. Carries the per-vertex fade through.

layout(lines) in;
layout(triangle_strip, max_vertices = 4) out;

uniform vec2  uViewport; // render-target size in pixels
uniform float uWidth;    // line width in pixels

in float vFade[];

noperspective out float gEdge; // -1..1 across the line, in units of the half-width incl. feather
out float gFade;

void main() {
    vec4 p0 = gl_in[0].gl_Position;
    vec4 p1 = gl_in[1].gl_Position;

    // Simple near-plane handling: drop segments with an endpoint behind the
    // camera. Only affects the few segments right next to the eye.
    if (p0.w <= 0.0 || p1.w <= 0.0) {
        return;
    }

    vec2 halfVp = 0.5 * uViewport;
    vec2 s0 = p0.xy / p0.w * halfVp;
    vec2 s1 = p1.xy / p1.w * halfVp;
    vec2 d = s1 - s0;
    float len = length(d);
    d = len > 1e-5 ? d / len : vec2(1.0, 0.0);

    float halfW = 0.5 * uWidth + 1.0;
    vec2 n = vec2(-d.y, d.x) * halfW / halfVp; // NDC offset

    gFade = vFade[0];
    gEdge = 1.0;  gl_Position = vec4(p0.xy + n * p0.w, p0.zw); EmitVertex();
    gEdge = -1.0; gl_Position = vec4(p0.xy - n * p0.w, p0.zw); EmitVertex();
    gFade = vFade[1];
    gEdge = 1.0;  gl_Position = vec4(p1.xy + n * p1.w, p1.zw); EmitVertex();
    gEdge = -1.0; gl_Position = vec4(p1.xy - n * p1.w, p1.zw); EmitVertex();
    EndPrimitive();
}
