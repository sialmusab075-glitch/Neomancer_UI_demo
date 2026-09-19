#version 330 core
// Separable 9-tap Gaussian using 5 bilinear fetches (linear-sampling trick).
// uDir = (1/width, 0) for the horizontal pass, (0, 1/height) for the vertical.

in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uTex;
uniform vec2 uDir;

void main() {
    vec3 c = texture(uTex, vUv).rgb * 0.2270270270;
    c += (texture(uTex, vUv + uDir * 1.3846153846).rgb + texture(uTex, vUv - uDir * 1.3846153846).rgb) * 0.3162162162;
    c += (texture(uTex, vUv + uDir * 3.2307692308).rgb + texture(uTex, vUv - uDir * 3.2307692308).rgb) * 0.0702702703;
    fragColor = vec4(c, 1.0);
}
