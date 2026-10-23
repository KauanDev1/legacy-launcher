// SPDX-License-Identifier: MIT
// Fills the colour target with one colour (EDRAM clears, Task 18). Draw it with blit.vert.
#version 450

layout(push_constant) uniform Clear {
  vec4 color;
} u_clear;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
  o_color = u_clear.color;
}
