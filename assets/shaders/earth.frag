#version 330 core
// Earth surface: Blue Marble texture (or a procedural lat/long grid when the
// texture is missing), a soft day/night terminator from a fixed Sun direction,
// a twilight band, and a fresnel glow on the limb. No colours are hard-coded:
// everything the theme decides arrives as a uniform.

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUv;

uniform sampler2D uTex;
uniform float uHasTex;      // 1 = sample the texture, 0 = procedural grid
uniform vec3  uSunDir;      // unit, fixed
uniform vec3  uCameraPos;   // eye relative to the target
uniform vec3  uBaseColor;   // the Earth's body colour (grid fallback)
uniform vec3  uGridColor;   // grid lines (theme structure colour)
uniform vec3  uAccent;      // equator and prime meridian (theme accent)
uniform vec3  uNightColor;  // ambient floor on the dark side (theme background)
uniform vec3  uAtmoColor;   // limb glow and twilight tint

out vec4 fragColor;

// Antialiased grid line: 1 on the line, 0 away from it. `cells` lines per unit of uv.
float gridLine(vec2 uv, vec2 cells, float widthPx) {
    vec2 g = uv * cells;
    vec2 d = abs(fract(g - 0.5) - 0.5) / max(fwidth(g), vec2(1e-5));
    return 1.0 - clamp(min(d.x, d.y) - widthPx + 1.0, 0.0, 1.0);
}

void main() {
    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCameraPos - vWorldPos);
    float ndl = dot(N, uSunDir);

    vec3 albedo;
    if (uHasTex > 0.5) {
        albedo = texture(uTex, vUv).rgb;
    } else {
        // Procedural surface: dim body colour, a line every 15 degrees (brighter
        // every 30), and the equator and prime meridian in the accent colour.
        float minor = gridLine(vUv, vec2(24.0, 12.0), 1.0);
        float major = gridLine(vUv, vec2(12.0, 6.0), 1.6);
        float equator = 1.0 - clamp(abs(vUv.y - 0.5) * 2.0 * 220.0 - 0.6, 0.0, 1.0);
        float meridian = 1.0 - clamp(min(vUv.x, 1.0 - vUv.x) * 2.0 * 440.0 - 0.6, 0.0, 1.0);
        albedo = uBaseColor * 0.55;
        albedo = mix(albedo, uGridColor * 0.75, minor);
        albedo = mix(albedo, uGridColor * 1.15, major);
        albedo = mix(albedo, uAccent, clamp(equator + meridian, 0.0, 1.0));
    }

    // Day/night: a soft terminator (the atmosphere scatters light past the geometric edge).
    float day = smoothstep(-0.10, 0.22, ndl);
    vec3 lit = albedo * mix(0.05, 1.0, day);
    lit = mix(lit, max(lit, albedo * uNightColor * 3.0), 1.0 - day);

    // Twilight: a thin tinted band right at the terminator.
    float twilight = exp(-pow(ndl / 0.11, 2.0));
    lit += uAtmoColor * twilight * 0.16;

    // Limb glow, stronger on the lit side.
    float fresnel = pow(1.0 - max(dot(N, V), 0.0), 3.0);
    lit += uAtmoColor * fresnel * (0.20 + 0.80 * day) * 0.85;

    fragColor = vec4(lit, 1.0);
}
