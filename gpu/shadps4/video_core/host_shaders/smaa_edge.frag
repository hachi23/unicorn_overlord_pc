// SPDX-License-Identifier: GPL-2.0-or-later
// vw: SMAA pass 1, luma edges. SMAA finds edges in gamma space: a linear (sRGB view) colour is
// encoded first. No edge: discarded, the target stays cleared to 0.
#version 450
#extension GL_GOOGLE_include_directive : require

vec4 smaa_gamma(vec4 c);
#define SMAASamplePoint(tex, coord) smaa_gamma(texture(tex, coord))
#include "smaa/smaa.glsl"

layout(set = 0, binding = 0) uniform sampler2D color_tex;
layout(location = 0) out vec2 edges;

vec4 smaa_gamma(vec4 c) {
    if (pc.linear == 0u) return c;
    vec3 v = max(c.rgb, 0.0);
    return vec4(mix(v * 12.92, 1.055 * pow(v, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, v)), c.a);
}

void main() {
    vec2 texcoord = smaa_texcoord();
    vec4 offset[3];
    SMAAEdgeDetectionVS(texcoord, offset);
    edges = SMAALumaEdgeDetectionPS(texcoord, offset, color_tex);
}
