#version 330 core

uniform vec4 uColor;

in float vAlpha;

out vec4 fragColor;

void main() {
    fragColor = vec4(uColor.rgb, uColor.a * vAlpha);
}
