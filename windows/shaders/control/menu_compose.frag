#version 450
// NR Control: the menu (drawn into an 8-bit image as for an ordinary SDR screen) laid over the game's image in the swap
// chain's own encoding. mode 0: as it is (UNORM, SDR); 1: an _SRGB format (the hardware encodes, so linear here);
// 2: scRGB (linear, 1.0 = 80 nits; scale = white in those units); 3: HDR10 (BT.2020 primaries, PQ; scale = white / 10000
// nits). Blended over the game's colour with the menu's coverage; the game's alpha is kept.
layout(set = 0, binding = 0) uniform sampler2D menu;
layout(push_constant) uniform Push { uint mode; float scale; } pc;
layout(location = 0) out vec4 colour;

vec3 srgb_to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}
vec3 pq(vec3 n) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    const vec3 p = pow(max(n, vec3(0.0)), vec3(m1));
    return pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2));
}

void main() {
    const vec4 m = texelFetch(menu, ivec2(gl_FragCoord.xy), 0);
    if (m.a <= 0.0) discard;
    vec3 c = clamp(m.rgb / m.a, 0.0, 1.0);   // the menu's pixels are drawn over transparent black: premultiplied
    if (pc.mode == 1u) {
        c = srgb_to_linear(c);
    } else if (pc.mode == 2u) {
        c = srgb_to_linear(c) * pc.scale;
    } else if (pc.mode == 3u) {
        const mat3 bt709_to_bt2020 = mat3(0.627404, 0.069097, 0.016391,
                                          0.329283, 0.919540, 0.088013,
                                          0.043313, 0.011362, 0.895595);
        c = pq(bt709_to_bt2020 * srgb_to_linear(c) * pc.scale);
    }
    colour = vec4(c * m.a, m.a);
}
