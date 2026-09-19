#version 330 core
// Bright pass + 2x downsample. Four bilinear taps (each averaging 2x2 texels)
// around the half-resolution texel centre, then a soft-knee threshold so
// only emissive/HDR parts of the scene feed the bloom.

in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uScene;
uniform vec2  uTexel;     // 1 / full-resolution size
uniform float uThreshold; // brightness where bloom starts
uniform float uKnee;      // softness of the threshold

void main() {
    vec3 c = texture(uScene, vUv + uTexel * vec2(-1.0, -1.0)).rgb
           + texture(uScene, vUv + uTexel * vec2( 1.0, -1.0)).rgb
           + texture(uScene, vUv + uTexel * vec2(-1.0,  1.0)).rgb
           + texture(uScene, vUv + uTexel * vec2( 1.0,  1.0)).rgb;
    c *= 0.25;

    float br = max(c.r, max(c.g, c.b));
    float soft = clamp(br - uThreshold + uKnee, 0.0, 2.0 * uKnee);
    soft = soft * soft / (4.0 * uKnee + 1e-4);
    float contrib = max(soft, br - uThreshold) / max(br, 1e-4);
    fragColor = vec4(c * contrib, 1.0);
}
