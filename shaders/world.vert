#version 450

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;

layout(push_constant) uniform PushConstants {
	mat4 mvp;
} pc;

layout(set = 0, binding = 0) uniform WorldUBO {
	mat4 model;
	vec4 fog_color;
	vec4 camera_fog_end; /* xyz = camera world pos, w = spherical fog end */
	vec4 fog_params;     /* x = density, y = flags (bit0=spherical, bit1=exp2) */
} ubo;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec3 v_world_pos;

void main() {
	vec4 world = ubo.model * vec4(in_pos, 1.0);
	v_world_pos = world.xyz;
	v_color = in_color;
	gl_Position = pc.mvp * vec4(in_pos, 1.0);
}
