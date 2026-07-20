#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(location = 0) out vec4 out_color;

void main() {
	vec4 c = texture(u_tex, v_uv) * v_color;
	/* GL_ALPHA_TEST GL_GREATER 0.5 */
	if(c.a <= 0.5)
		discard;
	out_color = c;
}
