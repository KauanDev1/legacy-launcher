// SPDX-License-Identifier: MIT
// Fullscreen triangle from gl_VertexIndex (3 vertices, no vertex input); v_uv is (0,0) at the top-left of the target
// and (1,1) at the bottom-right. Shared by blit.frag and clear.frag.
#version 450

layout(location = 0) out vec2 v_uv;

void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  v_uv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
