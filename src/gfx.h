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

void gfx_apply_context_hints(void);
void gfx_init(void* window);
void gfx_shutdown(void);
void gfx_resize(int w, int h);
void gfx_swap_buffers(void);
void gfx_set_vsync(int interval);

/* CPU owns matrix math; gfx only uploads the final values. */
void gfx_matrix_projection(const float* m16);
void gfx_matrix_modelview(const float* view16, const float* model16);

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

#endif
