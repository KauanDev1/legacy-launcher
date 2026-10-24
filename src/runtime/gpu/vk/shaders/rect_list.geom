// SPDX-License-Identifier: MIT
#version 450
// Xenos rectangle lists (VGT prim_type 8): every three vertices are three corners of a rectangle (in practice an
// axis-aligned screen-space quad). The draw path draws them as a triangle list and this geometry shader emits each
// one as a two-triangle strip whose fourth corner is v_a + v_b - v_r, r being the corner opposite the longest edge
// (the right angle) and a, b the other two in the original cyclic order, so both triangles keep the winding of the
// guest triangle. The position and the 16 interpolators of the translated vertex shader (locations 0..15, the plan B
// contract) are extrapolated the same way. Semantics per Xenia's rectangle-list expansion (reference only).

layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;

in gl_PerVertex {
  vec4 gl_Position;
} gl_in[];

out gl_PerVertex {
  vec4 gl_Position;
};

layout(location = 0) in vec4 i0[];
layout(location = 1) in vec4 i1[];
layout(location = 2) in vec4 i2[];
layout(location = 3) in vec4 i3[];
layout(location = 4) in vec4 i4[];
layout(location = 5) in vec4 i5[];
layout(location = 6) in vec4 i6[];
layout(location = 7) in vec4 i7[];
layout(location = 8) in vec4 i8[];
layout(location = 9) in vec4 i9[];
layout(location = 10) in vec4 i10[];
layout(location = 11) in vec4 i11[];
layout(location = 12) in vec4 i12[];
layout(location = 13) in vec4 i13[];
layout(location = 14) in vec4 i14[];
layout(location = 15) in vec4 i15[];

layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
layout(location = 4) out vec4 o4;
layout(location = 5) out vec4 o5;
layout(location = 6) out vec4 o6;
layout(location = 7) out vec4 o7;
layout(location = 8) out vec4 o8;
layout(location = 9) out vec4 o9;
layout(location = 10) out vec4 o10;
layout(location = 11) out vec4 o11;
layout(location = 12) out vec4 o12;
layout(location = 13) out vec4 o13;
layout(location = 14) out vec4 o14;
layout(location = 15) out vec4 o15;

void emit_corner(int v) {
  gl_Position = gl_in[v].gl_Position;
  o0 = i0[v];
  o1 = i1[v];
  o2 = i2[v];
  o3 = i3[v];
  o4 = i4[v];
  o5 = i5[v];
  o6 = i6[v];
  o7 = i7[v];
  o8 = i8[v];
  o9 = i9[v];
  o10 = i10[v];
  o11 = i11[v];
  o12 = i12[v];
  o13 = i13[v];
  o14 = i14[v];
  o15 = i15[v];
  EmitVertex();
}

// The corner opposite r: a + b - r.
void emit_fourth(int a, int b, int r) {
  gl_Position = gl_in[a].gl_Position + gl_in[b].gl_Position - gl_in[r].gl_Position;
  o0 = i0[a] + i0[b] - i0[r];
  o1 = i1[a] + i1[b] - i1[r];
  o2 = i2[a] + i2[b] - i2[r];
  o3 = i3[a] + i3[b] - i3[r];
  o4 = i4[a] + i4[b] - i4[r];
  o5 = i5[a] + i5[b] - i5[r];
  o6 = i6[a] + i6[b] - i6[r];
  o7 = i7[a] + i7[b] - i7[r];
  o8 = i8[a] + i8[b] - i8[r];
  o9 = i9[a] + i9[b] - i9[r];
  o10 = i10[a] + i10[b] - i10[r];
  o11 = i11[a] + i11[b] - i11[r];
  o12 = i12[a] + i12[b] - i12[r];
  o13 = i13[a] + i13[b] - i13[r];
  o14 = i14[a] + i14[b] - i14[r];
  o15 = i15[a] + i15[b] - i15[r];
  EmitVertex();
}

vec2 screen(int v) { return gl_in[v].gl_Position.xy / gl_in[v].gl_Position.w; }

float length2(vec2 d) { return dot(d, d); }

void main() {
  vec2 p0 = screen(0), p1 = screen(1), p2 = screen(2);
  float e12 = length2(p2 - p1), e20 = length2(p0 - p2), e01 = length2(p1 - p0);
  // The right angle sits opposite the longest edge (corner 0 unless edge 2-0 or 0-1 is longer).
  int r = 0;
  if (e20 > e12 && e20 >= e01) {
    r = 1;
  } else if (e01 > e12 && e01 > e20) {
    r = 2;
  }
  int a = (r + 1) % 3, b = (r + 2) % 3;
  emit_corner(r);
  emit_corner(a);
  emit_corner(b);
  emit_fourth(a, b, r);
  EndPrimitive();
}
