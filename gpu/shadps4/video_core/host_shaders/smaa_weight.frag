// SPDX-License-Identifier: GPL-2.0-or-later
// vw: SMAA pass 2, blending weights from the edges and the area and search lookup textures.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "smaa/smaa.glsl"

layout(set = 0, binding = 0) uniform sampler2D edges_tex;
layout(set = 0, binding = 1) uniform sampler2D area_tex;   // bilinear
layout(set = 0, binding = 2) uniform sampler2D search_tex; // nearest
layout(location = 0) out vec4 weights;

void main() {
    vec2 texcoord = smaa_texcoord();
    vec2 pixcoord;
    vec4 offset[3];
    SMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);
    weights = SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, edges_tex, area_tex,
                                              search_tex, vec4(0.0));
}
