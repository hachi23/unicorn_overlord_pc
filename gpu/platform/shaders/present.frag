// SPDX-License-Identifier: GPL-2.0-or-later
// The game's image into the frame the window shows. The source is read as linear light (an
// sRGB view, or decoded here for 10-bit sRGB buffers, which have no sRGB view) and the result
// is written sRGB encoded. A source larger than the frame is averaged over each frame pixel's
// footprint (a grid of up to 4x4 bilinear taps) instead of one tap that skips most of its
// pixels and shimmers; with sharpening on, that average is sharpened against its four
// neighbours, less where the local contrast is already high (after AMD's CAS).
#version 450
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 0) uniform sampler2D source;
layout(push_constant) uniform Settings {
    uint srgb_input;
    float sharpen;   // 0 off .. 1 strongest
} settings;

vec3 encode(vec3 c) {
    return mix(1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, c * 12.92, lessThan(c, vec3(0.0031308)));
}
vec3 decode(vec3 c) {
    return mix(pow(max(c + 0.055, 0.0) / 1.055, vec3(2.4)), c / 12.92, lessThan(c, vec3(0.04045)));
}
vec4 tap(vec2 at) {
    vec4 c = texture(source, at);
    if (settings.srgb_input != 0u) c.rgb = decode(c.rgb);
    return c;
}
vec4 area(vec2 center, vec2 footprint, ivec2 taps) {
    vec4 sum = vec4(0.0);
    for (int y = 0; y < taps.y; ++y)
        for (int x = 0; x < taps.x; ++x)
            sum += tap(center + footprint * ((vec2(x, y) + 0.5) / vec2(taps) - 0.5));
    return sum / float(taps.x * taps.y);
}

void main() {
    const vec2 pixel = abs(vec2(dFdx(uv).x, dFdy(uv).y));          // one frame pixel in uv
    const vec2 texels = pixel * vec2(textureSize(source, 0));        // source texels under it
    vec4 c;
    if (texels.x > 1.01 || texels.y > 1.01) {
        const ivec2 taps = clamp(ivec2(ceil(texels)), ivec2(1), ivec2(4));
        c = area(uv, pixel, taps);
        if (settings.sharpen > 0.0) {
            const vec3 m = c.rgb;
            const vec3 n = area(uv - vec2(0.0, pixel.y), pixel, taps).rgb;
            const vec3 s = area(uv + vec2(0.0, pixel.y), pixel, taps).rgb;
            const vec3 w = area(uv - vec2(pixel.x, 0.0), pixel, taps).rgb;
            const vec3 e = area(uv + vec2(pixel.x, 0.0), pixel, taps).rgb;
            const vec3 lo = min(m, min(min(n, s), min(w, e)));
            const vec3 hi = max(m, max(max(n, s), max(w, e)));
            const vec3 amount = sqrt(clamp(min(lo, 1.0 - hi) / max(hi, 1e-4), 0.0, 1.0));
            const vec3 weight = amount * (-1.0 / mix(8.0, 5.0, settings.sharpen));
            c.rgb = clamp((m + (n + s + w + e) * weight) / (1.0 + 4.0 * weight), 0.0, 1.0);
        }
    } else {
        c = tap(uv);
    }
    color = vec4(encode(max(c.rgb, 0.0)), 1.0);
}
