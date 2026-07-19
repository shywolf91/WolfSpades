/*
	Copyright (c) 2017-2020 ByteBit

	This file is part of BetterSpades.

	BetterSpades is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	BetterSpades is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with BetterSpades.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "gfx.h"
#include "gfx_backend.h"
#include "log.h"

static const gfx_ops_t* gfx_ops = &gfx_gl_ops;
static gfx_backend_t gfx_backend = GFX_BACKEND_GL;

void gfx_select_backend(gfx_backend_t backend) {
	gfx_backend = backend;
	switch(backend) {
#ifndef __EMSCRIPTEN__
		case GFX_BACKEND_VULKAN: gfx_ops = &gfx_vk_ops; break;
#endif
		case GFX_BACKEND_GL:
		default: gfx_ops = &gfx_gl_ops; gfx_backend = GFX_BACKEND_GL; break;
	}
	log_info("gfx backend: %s", gfx_backend == GFX_BACKEND_VULKAN ? "vulkan" : "opengl");
}

gfx_backend_t gfx_selected_backend(void) {
	return gfx_backend;
}

void gfx_apply_context_hints(void) {
	gfx_ops->apply_context_hints();
}

void gfx_init(void* window) {
	gfx_ops->init(window);
}

void gfx_shutdown(void) {
	gfx_ops->shutdown();
}

void gfx_resize(int w, int h) {
	gfx_ops->resize(w, h);
}

void gfx_swap_buffers(void) {
	gfx_ops->swap_buffers();
}

void gfx_set_vsync(int interval) {
	gfx_ops->set_vsync(interval);
}

void gfx_matrix_projection(const float* m16) {
	gfx_ops->matrix_projection(m16);
}

void gfx_matrix_modelview(const float* view16, const float* model16) {
	gfx_ops->matrix_modelview(view16, model16);
}

void gfx_matrix_texture(float sx, float sy) {
	gfx_ops->matrix_texture(sx, sy);
}

void gfx_pass_begin(gfx_pass_t pass) {
	gfx_ops->pass_begin(pass);
}

void gfx_pass_end(gfx_pass_t pass) {
	gfx_ops->pass_end(pass);
}

gfx_texture_t gfx_texture_create_rgba(int w, int h, const void* pixels) {
	return gfx_ops->texture_create_rgba(w, h, pixels);
}

void gfx_texture_upload_rgba(gfx_texture_t tex, int w, int h, const void* pixels) {
	gfx_ops->texture_upload_rgba(tex, w, h, pixels);
}

gfx_texture_t gfx_texture_create_alpha(int w, int h, const void* pixels) {
	return gfx_ops->texture_create_alpha(w, h, pixels);
}

void gfx_texture_update_sub_rgba(gfx_texture_t tex, int x, int y, int w, int h, const void* pixels) {
	gfx_ops->texture_update_sub_rgba(tex, x, y, w, h, pixels);
}

void gfx_texture_set_filter(gfx_texture_t tex, gfx_filter_t filter) {
	gfx_ops->texture_set_filter(tex, filter);
}

void gfx_texture_set_filter_wrap_bound(gfx_filter_t filter, gfx_wrap_t wrap) {
	gfx_ops->texture_set_filter_wrap_bound(filter, wrap);
}

void gfx_texture_bind(gfx_texture_t tex) {
	gfx_ops->texture_bind(tex);
}

void gfx_texture_destroy(gfx_texture_t tex) {
	gfx_ops->texture_destroy(tex);
}

int gfx_max_texture_size(void) {
	return gfx_ops->max_texture_size();
}

int gfx_supports_npot(void) {
	return gfx_ops->supports_npot();
}

void gfx_texture_2d(int enabled) {
	gfx_ops->texture_2d(enabled);
}

void gfx_blend(int enabled) {
	gfx_ops->blend(enabled);
}

void gfx_draw_quads_2d(const float* xy, const float* uv, int vertex_count) {
	gfx_ops->draw_quads_2d(xy, uv, vertex_count);
}

void gfx_draw_quads_2d_short(const short* xy, const short* uv, int vertex_count) {
	gfx_ops->draw_quads_2d_short(xy, uv, vertex_count);
}

void gfx_mesh_create(gfx_mesh_t* m, int has_color, int has_normal) {
	gfx_ops->mesh_create(m, has_color, has_normal);
}

void gfx_mesh_destroy(gfx_mesh_t* m) {
	gfx_ops->mesh_destroy(m);
}

void gfx_mesh_update(gfx_mesh_t* m, size_t count, gfx_mesh_type_t type, const void* color, const void* vertex,
					 const void* normal) {
	gfx_ops->mesh_update(m, count, type, color, vertex, normal);
}

void gfx_mesh_draw(gfx_mesh_t* m, gfx_mesh_type_t type) {
	gfx_ops->mesh_draw(m, type);
}

void gfx_draw_arrays(gfx_mesh_type_t type, size_t count, const void* vertex, const void* color, const void* normal) {
	gfx_ops->draw_arrays(type, count, vertex, color, normal);
}

void gfx_color_mask(int r, int g, int b, int a) {
	gfx_ops->color_mask(r, g, b, a);
}

void gfx_color3f(float r, float g, float b) {
	gfx_ops->color3f(r, g, b);
}

void gfx_color3ub(unsigned char r, unsigned char g, unsigned char b) {
	gfx_ops->color3ub(r, g, b);
}

void gfx_color4f(float r, float g, float b, float a) {
	gfx_ops->color4f(r, g, b, a);
}

void gfx_color4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
	gfx_ops->color4ub(r, g, b, a);
}

void gfx_get_color4f(float out[4]) {
	gfx_ops->get_color4f(out);
}

void gfx_multisample(int enabled) {
	gfx_ops->multisample(enabled);
}

void gfx_line_width(float w) {
	gfx_ops->line_width(w);
}

void gfx_draw_lines_2f(const float* xy_pairs, int vertex_count) {
	gfx_ops->draw_lines_2f(xy_pairs, vertex_count);
}

void gfx_draw_lines_3s(const short* xyz, int vertex_count) {
	gfx_ops->draw_lines_3s(xyz, vertex_count);
}

void gfx_depth_range_weapon(void) {
	gfx_ops->depth_range_weapon();
}

void gfx_depth_range_reset(void) {
	gfx_ops->depth_range_reset();
}

void gfx_depth_test(int enabled) {
	gfx_ops->depth_test(enabled);
}

void gfx_depth_func_notequal(void) {
	gfx_ops->depth_func_notequal();
}

void gfx_depth_func_lequal(void) {
	gfx_ops->depth_func_lequal();
}

void gfx_scissor(int x, int y, int w, int h) {
	gfx_ops->scissor(x, y, w, h);
}

void gfx_scissor_off(void) {
	gfx_ops->scissor_off();
}

void gfx_viewport(int x, int y, int w, int h) {
	gfx_ops->viewport(x, y, w, h);
}

void gfx_clear_color(float r, float g, float b, float a) {
	gfx_ops->clear_color(r, g, b, a);
}

void gfx_clear(void) {
	gfx_ops->clear();
}

void gfx_clear_color_only(void) {
	gfx_ops->clear_color_only();
}

void gfx_shade_smooth(void) {
	gfx_ops->shade_smooth();
}

void gfx_shade_flat(void) {
	gfx_ops->shade_flat();
}

void gfx_light0_position(const float pos4[4]) {
	gfx_ops->light0_position(pos4);
}

void gfx_capture_framebuffer(int x, int y, int w, int h, void* out_rgba) {
	gfx_ops->capture_framebuffer(x, y, w, h, out_rgba);
}

int gfx_gl2(void) {
	return gfx_ops->gl2();
}

void gfx_model_light(const float ambient4[4], const float diffuse4[4]) {
	gfx_ops->model_light(ambient4, diffuse4);
}

void gfx_model_mesh_begin(gfx_texture_t dummy) {
	gfx_ops->model_mesh_begin(dummy);
}

void gfx_model_texenv_color(float r, float g, float b) {
	gfx_ops->model_texenv_color(r, g, b);
}

void gfx_model_mesh_end(void) {
	gfx_ops->model_mesh_end();
}

void gfx_model_points_begin_fixed(float point_size) {
	gfx_ops->model_points_begin_fixed(point_size);
}

void gfx_model_points_end_fixed(void) {
	gfx_ops->model_points_end_fixed();
}

void gfx_model_points_begin_shader(float point_size, float dist_factor, const float fog_rgb[3], const float camera[3],
								   const float model16[16]) {
	gfx_ops->model_points_begin_shader(point_size, dist_factor, fog_rgb, camera, model16);
}

void gfx_model_points_end_shader(void) {
	gfx_ops->model_points_end_shader();
}

void gfx_fog_enable_exp2(const float color4[4], float density) {
	gfx_ops->fog_enable_exp2(color4, density);
}

void gfx_fog_disable(void) {
	gfx_ops->fog_disable();
}

void gfx_fog_enable_spherical(void) {
	gfx_ops->fog_enable_spherical();
}

void gfx_fog_disable_spherical(void) {
	gfx_ops->fog_disable_spherical();
}

int gfx_fog_active(void) {
	return gfx_ops->fog_active();
}
