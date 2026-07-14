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

#include <stdlib.h>

#include "common.h"
#include "glx.h"

// for future opengl-es abstraction layer

int glx_version = 0;

int glx_fog = 0;

static int glx_major_ver() {
#ifdef OPENGL_ES
	return 2;
#else
	return atoi(glGetString(GL_VERSION));
#endif
}

void glx_init() {
#ifndef OPENGL_ES
	glx_version = glx_major_ver() >= 2;
#else
	glx_version = 0;
#endif
}

int glx_shader(const char* vertex, const char* fragment) {
#ifndef OPENGL_ES
	int v, f;
	if(vertex) {
		v = glCreateShader(GL_VERTEX_SHADER);
		glShaderSource(v, 1, (const GLchar* const*)&vertex, NULL);
		glCompileShader(v);
	}

	if(fragment) {
		f = glCreateShader(GL_FRAGMENT_SHADER);
		glShaderSource(f, 1, (const GLchar* const*)&fragment, NULL);
		glCompileShader(f);
	}

	int program = glCreateProgram();
	if(vertex)
		glAttachShader(program, v);
	if(vertex)
		glAttachShader(program, f);
	glLinkProgram(program);
	return program;
#else
	(void)vertex;
	(void)fragment;
	return 0;
#endif
}

void glx_displaylist_create(glx_displaylist* x, bool has_color, bool has_normal) {
	gfx_mesh_create(x, has_color, has_normal);
}

void glx_displaylist_destroy(glx_displaylist* x) {
	gfx_mesh_destroy(x);
}

void glx_displaylist_update(glx_displaylist* x, size_t size, int type, void* color, void* vertex, void* normal) {
	gfx_mesh_update(x, size, (gfx_mesh_type_t)type, color, vertex, normal);
}

void glx_displaylist_draw(glx_displaylist* x, int type) {
	gfx_mesh_draw(x, (gfx_mesh_type_t)type);
}

void glx_enable_sphericalfog() {
	gfx_fog_enable_spherical();
}

void glx_disable_sphericalfog() {
	gfx_fog_disable_spherical();
}
