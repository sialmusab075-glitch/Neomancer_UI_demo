#version 330 core
// The ordinary flyby paths, drawn as native one-pixel lines (no geometry shader) with
// additive blending (GL_SRC_ALPHA, GL_ONE). A thousand widened quads across the HDR
// multisampled target is a fill-rate bill an integrated GPU cannot pay at 60 fps; a
// one-pixel line is the same picture for a fraction of the fragments. Only the
// selected and hovered paths are widened (flypath.vert + orbit.geom + orbit.frag).

uniform vec4 uColor;

in float vFade; // 1 at closest approach, fading toward the path ends

out vec4 fragColor;

void main() {
    fragColor = vec4(uColor.rgb, uColor.a * vFade);
}
