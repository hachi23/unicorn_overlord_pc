// SPDX-License-Identifier: GPL-2.0-or-later
// vw: SMAA 1x (gpu/third_party/smaa, MIT) for host_passes/smaa_pass.cpp. Each pass is a fragment
// shader on the full-screen triangle; the vertex functions run per pixel from gl_FragCoord.

layout(push_constant) uniform Constants {
    vec4 metrics; // 1 / width, 1 / height, width, height
    uint linear;  // the colour samples as linear light (an sRGB view)
} pc;

#define SMAA_RT_METRICS pc.metrics
#define SMAA_PRESET_HIGH
#define SMAA_CUSTOM_SL
#define SMAATexture2D(tex) sampler2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) textureLod(tex, coord, 0.0)
#define SMAASampleLevelZeroPoint(tex, coord) textureLod(tex, coord, 0.0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) textureLodOffset(tex, coord, 0.0, offset)
#define SMAASample(tex, coord) texture(tex, coord)
#ifndef SMAASamplePoint
#define SMAASamplePoint(tex, coord) texture(tex, coord)
#endif
#define SMAASampleOffset(tex, coord, offset) texture(tex, coord, offset)
#define SMAA_FLATTEN
#define SMAA_BRANCH
#define lerp(a, b, t) mix(a, b, t)
#define saturate(a) clamp(a, 0.0, 1.0)
#define mad(a, b, c) fma(a, b, c)
#define SMAAGather(tex, coord) textureGather(tex, coord)
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define bool2 bvec2
#define bool3 bvec3
#define bool4 bvec4

#include "../../../../third_party/smaa/SMAA.hlsl"

vec2 smaa_texcoord() {
    return gl_FragCoord.xy * SMAA_RT_METRICS.xy;
}
