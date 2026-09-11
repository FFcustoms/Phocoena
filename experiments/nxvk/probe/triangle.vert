#version 450
// SPDX-License-Identifier: GPL-2.0-or-later
layout(location = 0) out vec3 color;
const vec2 positions[3] = vec2[3](vec2(0.0, -0.7), vec2(0.7, 0.7), vec2(-0.7, 0.7));
const vec3 colors[3] = vec3[3](vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1));
void main()
{
  gl_Position = vec4(positions[gl_VertexIndex], 0, 1);
  color = colors[gl_VertexIndex];
}
