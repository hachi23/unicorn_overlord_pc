// SPDX-License-Identifier: GPL-2.0-or-later
// vw: SMAA pass 3, neighbourhood blending of the colour by the weights. Blends the colour as it
// samples (linear light through an sRGB view), as SMAA asks.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "smaa/smaa.glsl"

layout(set = 0, binding = 0) uniform sampler2D color_tex;
layout(set = 0, binding = 1) uniform sampler2D blend_tex;
layout(location = 0) out vec4 color;

void main() {
    vec2 texcoord = smaa_texcoord();
    vec4 offset;
    SMAANeighborhoodBlendingVS(texcoord, offset);
    color = SMAANeighborhoodBlendingPS(texcoord, offset, color_tex, blend_tex);
}
