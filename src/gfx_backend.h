/*
	Internal backend dispatch table. Not part of the public gfx.h API.
*/

#ifndef GFX_BACKEND_H
#define GFX_BACKEND_H

#include "gfx.h"

typedef struct gfx_ops {
	void (*apply_context_hints)(void);
	void (*init)(void* window);
	void (*shutdown)(void);
	void (*resize)(int w, int h);
	void (*swap_buffers)(void);
	void (*set_vsync)(int interval);

	void (*matrix_projection)(const float* m16);
	void (*matrix_modelview)(const float* view16, const float* model16);
	void (*matrix_texture)(float sx, float sy);

	void (*pass_begin)(gfx_pass_t pass);
	void (*pass_end)(gfx_pass_t pass);

	gfx_texture_t (*texture_create_rgba)(int w, int h, const void* pixels);
	void (*texture_upload_rgba)(gfx_texture_t tex, int w, int h, const void* pixels);
	gfx_texture_t (*texture_create_alpha)(int w, int h, const void* pixels);
	void (*texture_update_sub_rgba)(gfx_texture_t tex, int x, int y, int w, int h, const void* pixels);
	void (*texture_set_filter)(gfx_texture_t tex, gfx_filter_t filter);
	void (*texture_set_filter_wrap_bound)(gfx_filter_t filter, gfx_wrap_t wrap);
	void (*texture_bind)(gfx_texture_t tex);
	void (*texture_destroy)(gfx_texture_t tex);

	int (*max_texture_size)(void);
	int (*supports_npot)(void);

	void (*texture_2d)(int enabled);
	void (*blend)(int enabled);

	void (*draw_quads_2d)(const float* xy, const float* uv, int vertex_count);
	void (*draw_quads_2d_short)(const short* xy, const short* uv, int vertex_count);

	void (*mesh_create)(gfx_mesh_t* m, int has_color, int has_normal);
	void (*mesh_destroy)(gfx_mesh_t* m);
	void (*mesh_update)(gfx_mesh_t* m, size_t count, gfx_mesh_type_t type, const void* color, const void* vertex,
						const void* normal);
	void (*mesh_draw)(gfx_mesh_t* m, gfx_mesh_type_t type);

	void (*draw_arrays)(gfx_mesh_type_t type, size_t count, const void* vertex, const void* color,
						const void* normal);

	void (*color_mask)(int r, int g, int b, int a);

	void (*color3f)(float r, float g, float b);
	void (*color3ub)(unsigned char r, unsigned char g, unsigned char b);
	void (*color4f)(float r, float g, float b, float a);
	void (*color4ub)(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
	void (*get_color4f)(float out[4]);
	void (*multisample)(int enabled);
	void (*line_width)(float w);

	void (*draw_lines_2f)(const float* xy_pairs, int vertex_count);
	void (*draw_lines_3s)(const short* xyz, int vertex_count);

	void (*depth_range_weapon)(void);
	void (*depth_range_reset)(void);

	void (*depth_test)(int enabled);
	void (*depth_func_notequal)(void);
	void (*depth_func_lequal)(void);

	void (*scissor)(int x, int y, int w, int h);
	void (*scissor_off)(void);

	void (*viewport)(int x, int y, int w, int h);

	void (*clear_color)(float r, float g, float b, float a);
	void (*clear)(void);
	void (*clear_color_only)(void);

	void (*shade_smooth)(void);
	void (*shade_flat)(void);
	void (*light0_position)(const float pos4[4]);

	void (*capture_framebuffer)(int x, int y, int w, int h, void* out_rgba);

	int (*gl2)(void);

	void (*model_light)(const float ambient4[4], const float diffuse4[4]);
	void (*model_mesh_begin)(gfx_texture_t dummy);
	void (*model_texenv_color)(float r, float g, float b);
	void (*model_mesh_end)(void);

	void (*model_points_begin_fixed)(float point_size);
	void (*model_points_end_fixed)(void);
	void (*model_points_begin_shader)(float point_size, float dist_factor, const float fog_rgb[3],
									  const float camera[3], const float model16[16]);
	void (*model_points_end_shader)(void);

	void (*fog_enable_exp2)(const float color4[4], float density);
	void (*fog_disable)(void);
	void (*fog_enable_spherical)(void);
	void (*fog_disable_spherical)(void);
	int (*fog_active)(void);
} gfx_ops_t;

extern const gfx_ops_t gfx_gl_ops;
extern const gfx_ops_t gfx_vk_ops;

#endif
