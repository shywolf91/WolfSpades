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

#ifndef GFX_H
#define GFX_H

#include <stddef.h>
#include <stdint.h>

void gfx_apply_context_hints(void);
void gfx_init(void* window);
void gfx_shutdown(void);
void gfx_resize(int w, int h);
void gfx_swap_buffers(void);
void gfx_set_vsync(int interval);

/* CPU owns matrix math; gfx only uploads the final values. */
void gfx_matrix_projection(const float* m16);
void gfx_matrix_modelview(const float* view16, const float* model16);
/* Texture matrix: LoadIdentity then Scale(sx,sy,1); use 1,1 to reset. */
void gfx_matrix_texture(float sx, float sy);

typedef enum {
	GFX_PASS_WORLD_3D,		/* depth test on, depth range 0..1 */
	GFX_PASS_BLOCK_OUTLINE, /* depth off, depth mask off */
	GFX_PASS_DAMAGED,		/* depth EQUAL + blend */
	GFX_PASS_COLLAPSING,	/* blend only */
	GFX_PASS_NAMETAG,		/* alpha test + depth off */
	GFX_PASS_UI_2D,			/* depth off, multisample off */
} gfx_pass_t;

void gfx_pass_begin(gfx_pass_t pass);
void gfx_pass_end(gfx_pass_t pass);

/* Opaque texture handle (stores backend id; 0 = none). */
typedef uint32_t gfx_texture_t;

typedef enum {
	GFX_FILTER_NEAREST = 0,
	GFX_FILTER_LINEAR = 1,
} gfx_filter_t;

typedef enum {
	GFX_WRAP_REPEAT = 0,
	GFX_WRAP_CLAMP = 1,
} gfx_wrap_t;

/* Create RGBA with NEAREST+REPEAT (matches texture_create / create_buffer). */
gfx_texture_t gfx_texture_create_rgba(int w, int h, const void* pixels);
/* Re-upload RGBA into an existing handle; same NEAREST+REPEAT params. */
void gfx_texture_upload_rgba(gfx_texture_t tex, int w, int h, const void* pixels);
/* Create ALPHA atlas; sets MIN_FILTER=LINEAR only (matches font bake). */
gfx_texture_t gfx_texture_create_alpha(int w, int h, const void* pixels);
void gfx_texture_update_sub_rgba(gfx_texture_t tex, int x, int y, int w, int h, const void* pixels);
/* Bind, set min+mag filter, unbind (matches texture_filter). */
void gfx_texture_set_filter(gfx_texture_t tex, gfx_filter_t filter);
/* Set filter+wrap on the currently bound texture (matches font_render). */
void gfx_texture_set_filter_wrap_bound(gfx_filter_t filter, gfx_wrap_t wrap);
void gfx_texture_bind(gfx_texture_t tex);
void gfx_texture_destroy(gfx_texture_t tex);

int gfx_max_texture_size(void);
int gfx_supports_npot(void);

/* 2D textured-quad state on/off (call order preserved per site). */
void gfx_texture_2d(int enabled);
void gfx_blend(int enabled); /* SRC_ALPHA / ONE_MINUS_SRC_ALPHA when on */

/* Client-array textured tris; vertex_count is draw vertex count (e.g. 6). */
void gfx_draw_quads_2d(const float* xy, const float* uv, int vertex_count);
void gfx_draw_quads_2d_short(const short* xy, const short* uv, int vertex_count);

/*
 * Persistent mesh (former display-list wrapper). Embeddable; fields are backend-owned.
 * Storage is display-list or VBO exactly as before — no promotion.
 */
typedef struct gfx_mesh {
	uint32_t legacy;
	uint32_t modern;
	size_t size;
	size_t buffer_size;
	int has_color;
	int has_normal;
} gfx_mesh_t;

/* Vertex format + primitive selector (mirrors prior NORMAL / ENHANCED / POINTS). */
typedef enum {
	GFX_MESH_SHORT = 0, /* short3 positions; quads/tris */
	GFX_MESH_FLOAT = 1, /* float3 positions; quads/tris */
	GFX_MESH_POINTS = 2, /* float3 positions; points */
} gfx_mesh_type_t;

void gfx_mesh_create(gfx_mesh_t* m, int has_color, int has_normal);
void gfx_mesh_destroy(gfx_mesh_t* m);
void gfx_mesh_update(gfx_mesh_t* m, size_t count, gfx_mesh_type_t type, const void* color,
					 const void* vertex, const void* normal);
void gfx_mesh_draw(gfx_mesh_t* m, gfx_mesh_type_t type);

/*
 * Transient client-array draw (tesselator_draw). color/normal may be NULL to skip
 * those arrays. Primitive follows mesh type (POINTS vs quads/tris).
 */
void gfx_draw_arrays(gfx_mesh_type_t type, size_t count, const void* vertex, const void* color,
					 const void* normal);

/* Collapsing-structure depth pre-pass (color-mask around double mesh draw). */
void gfx_color_mask(int r, int g, int b, int a);

/* Current color (kv6 point tint). */
void gfx_color3f(float r, float g, float b);
void gfx_color3ub(unsigned char r, unsigned char g, unsigned char b);
void gfx_multisample(int enabled);

/* --- kv6 model lighting / mesh combine / point sprites --- */
void gfx_model_light(const float ambient4[4], const float diffuse4[4]);

void gfx_model_mesh_begin(gfx_texture_t dummy);
void gfx_model_texenv_color(float r, float g, float b); /* ENV_COLOR rgb, a=1 */
void gfx_model_mesh_end(void);

/* Fixed-function point path (legacy / ES when !gfx version flag). */
void gfx_model_points_begin_fixed(float point_size);
void gfx_model_points_end_fixed(void);

/* Point-sprite shader path: lazy-compiles kv6 program; uniforms match prior model.c. */
void gfx_model_points_begin_shader(float point_size, float dist_factor, const float fog_rgb[3],
								   const float camera[3], const float model16[16]);
void gfx_model_points_end_shader(void);

/* --- fog --- */
void gfx_fog_enable_exp2(const float color4[4], float density);
void gfx_fog_disable(void);
void gfx_fog_enable_spherical(void);
void gfx_fog_disable_spherical(void);
int gfx_fog_active(void); /* spherical fog on/off flag */

#endif
