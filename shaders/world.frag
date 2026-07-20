#version 450

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec3 v_world_pos;

layout(set = 0, binding = 0) uniform WorldUBO {
	mat4 model;
	vec4 fog_color;
	vec4 camera_fog_end; /* xyz = camera, w = fog end */
	vec4 fog_params;     /* x = density, y = flags */
} ubo;

layout(location = 0) out vec4 out_color;

void main() {
	vec3 cam = ubo.camera_fog_end.xyz;
	float fog_end = ubo.camera_fog_end.w;
	uint flags = uint(ubo.fog_params.y);
	float density = ubo.fog_params.x;

	vec3 col = v_color.rgb;
	float alpha = v_color.a;

	/* Spherical: XZ distance / render_distance → mix toward fog */
	if((flags & 1u) != 0u && fog_end > 0.0) {
		float d = length(vec2(v_world_pos.x - cam.x, v_world_pos.z - cam.z));
		float f_sph = clamp(d / fog_end, 0.0, 1.0);
		col = mix(col, ubo.fog_color.rgb, f_sph);
	}

	/* EXP2: C = f * Cin + (1-f) * Cfog, f = exp(-(density * eyeDist)^2) */
	if((flags & 2u) != 0u) {
		float eye = length(v_world_pos - cam);
		float f_exp = exp(-pow(density * eye, 2.0));
		col = f_exp * col + (1.0 - f_exp) * ubo.fog_color.rgb;
	}

	out_color = vec4(col, alpha);
}
