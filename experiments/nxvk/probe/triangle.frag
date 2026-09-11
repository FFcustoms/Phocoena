#version 450
// SPDX-License-Identifier: GPL-2.0-or-later
layout(location = 0) in vec3 color;
layout(location = 0) out vec4 output_color;
void main() { output_color = vec4(color, 1); }
