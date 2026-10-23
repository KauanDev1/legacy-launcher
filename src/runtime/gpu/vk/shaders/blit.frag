// SPDX-License-Identifier: MIT
// Copies (and, when the sizes differ, scales with the sampler's filter) a 2D texture to the colour target. The
// presenter (Task 17) draws the frame with it.
#version 450

layout(set = 0, binding = 0) uniform sampler2D u_source;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
  o_color = texture(u_source, v_uv);
}
