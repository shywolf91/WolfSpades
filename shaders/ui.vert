#version 450

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(push_constant) uniform PushConstants {
	mat4 mvp;
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
	gl_Position = pc.mvp * vec4(in_pos, 0.0, 1.0);
	v_uv = in_uv;
	v_color = in_color;
}
