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

#endif
