// SPDX-License-Identifier: GPL-2.0-or-later
// One triangle over the whole target; uv runs 0..1 across it.
#version 450
layout(location = 0) out vec2 uv;
void main() {
    const vec2 corner = vec2(float(gl_VertexIndex == 1) * 2.0, float(gl_VertexIndex == 2) * 2.0);
    uv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
