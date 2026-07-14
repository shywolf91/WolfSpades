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

#include <string.h>

#include "common.h"
#include "config.h"
#include "log.h"
#include "gfx.h"
#include "glx.h"

static void* gfx_window;

void gfx_apply_context_hints(void) {
#ifdef USE_GLFW
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 1);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
#ifdef OPENGL_ES
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
	glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
#endif
	glfwWindowHint(GLFW_SAMPLES, settings.multisamples);

	/*
	#FIXME: This is intended to fix the issue #145.
	This is dirty because it disables the application-level Hi-DPI support for every installation
	instead of being applied only to those who needs it.
	*/
	glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_FALSE);
#endif

#ifdef USE_SDL
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
#ifdef OPENGL_ES
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#endif
#endif
}

void gfx_init(void* window) {
	gfx_window = window;

#ifdef USE_GLFW
	glfwMakeContextCurrent(window);
#endif

#ifdef USE_SDL
	SDL_GL_CreateContext(window);
#endif

#ifndef OPENGL_ES
	if(glewInit())
		log_error("Could not load extended OpenGL functions!");
#endif

	log_info("Vendor: %s", glGetString(GL_VENDOR));
	log_info("Renderer: %s", glGetString(GL_RENDERER));
	log_info("Version: %s", glGetString(GL_VERSION));

	if(settings.multisamples > 0) {
		glEnable(GL_MULTISAMPLE);
		log_info("MSAAx%i on", settings.multisamples);
	}

	while(glGetError() != GL_NO_ERROR)
		;

	glEnable(GL_DEPTH_TEST);
	glEnable(GL_CULL_FACE);
	glCullFace(GL_BACK);
	glFrontFace(GL_CCW);
#ifdef OPENGL_ES
	glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
#else
	glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
#endif
	glClearDepth(1.0F);
	glDepthFunc(GL_LEQUAL);
	glShadeModel(GL_SMOOTH);
	glDisable(GL_FOG);
}

void gfx_shutdown(void) {
	gfx_window = NULL;
}

void gfx_resize(int w, int h) {
	glViewport(0, 0, w, h);
}

void gfx_swap_buffers(void) {
#ifdef USE_GLFW
	glfwSwapBuffers(gfx_window);
#endif

#ifdef USE_SDL
	SDL_GL_SwapWindow(gfx_window);
#endif
}

void gfx_set_vsync(int interval) {
#ifdef USE_GLFW
	glfwSwapInterval(interval);
#endif

#ifdef USE_SDL
	SDL_GL_SetSwapInterval(interval);
#endif
}

void gfx_matrix_projection(const float* m16) {
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(m16);
}

void gfx_matrix_modelview(const float* view16, const float* model16) {
	glMatrixMode(GL_MODELVIEW);
	glLoadMatrixf(view16);
	glMultMatrixf(model16);
}

void gfx_matrix_texture(float sx, float sy) {
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glScalef(sx, sy, 1.0F);
	glMatrixMode(GL_MODELVIEW);
}

static GLenum gfx_filter_to_gl(gfx_filter_t filter) {
	return (filter == GFX_FILTER_LINEAR) ? GL_LINEAR : GL_NEAREST;
}

static GLenum gfx_wrap_to_gl(gfx_wrap_t wrap) {
	return (wrap == GFX_WRAP_CLAMP) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
}

gfx_texture_t gfx_texture_create_rgba(int w, int h, const void* pixels) {
	GLuint id = 0;
	glGenTextures(1, &id);
	glBindTexture(GL_TEXTURE_2D, id);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(GL_TEXTURE_2D, 0);
	return (gfx_texture_t)id;
}

void gfx_texture_upload_rgba(gfx_texture_t tex, int w, int h, const void* pixels) {
	glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glBindTexture(GL_TEXTURE_2D, 0);
}

gfx_texture_t gfx_texture_create_alpha(int w, int h, const void* pixels) {
	GLuint id = 0;
	glGenTextures(1, &id);
	glBindTexture(GL_TEXTURE_2D, id);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, w, h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, pixels);
	/* Font bake historically sets MIN only - leave MAG at driver default. */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, 0);
	return (gfx_texture_t)id;
}

void gfx_texture_update_sub_rgba(gfx_texture_t tex, int x, int y, int w, int h, const void* pixels) {
	glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
	glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void gfx_texture_set_filter(gfx_texture_t tex, gfx_filter_t filter) {
	GLenum mode = gfx_filter_to_gl(filter);
	glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mode);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mode);
	glBindTexture(GL_TEXTURE_2D, 0);
}

void gfx_texture_set_filter_wrap_bound(gfx_filter_t filter, gfx_wrap_t wrap) {
	GLenum f = gfx_filter_to_gl(filter);
	GLenum w = gfx_wrap_to_gl(wrap);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, w);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, w);
}

void gfx_texture_bind(gfx_texture_t tex) {
	glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
}

void gfx_texture_destroy(gfx_texture_t tex) {
	GLuint id = (GLuint)tex;
	glDeleteTextures(1, &id);
}

int gfx_max_texture_size(void) {
	int max_size = 0;
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
	return max_size;
}

int gfx_supports_npot(void) {
	const char* ext = (const char*)glGetString(GL_EXTENSIONS);
	return ext && strstr(ext, "ARB_texture_non_power_of_two") != NULL;
}

void gfx_texture_2d(int enabled) {
	if(enabled)
		glEnable(GL_TEXTURE_2D);
	else
		glDisable(GL_TEXTURE_2D);
}

void gfx_blend(int enabled) {
	if(enabled) {
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	} else {
		glDisable(GL_BLEND);
	}
}

void gfx_draw_quads_2d(const float* xy, const float* uv, int vertex_count) {
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_VERTEX_ARRAY);
	glTexCoordPointer(2, GL_FLOAT, 0, uv);
	glVertexPointer(2, GL_FLOAT, 0, xy);
	glDrawArrays(GL_TRIANGLES, 0, vertex_count);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
}

void gfx_draw_quads_2d_short(const short* xy, const short* uv, int vertex_count) {
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(2, GL_SHORT, 0, xy);
	glTexCoordPointer(2, GL_SHORT, 0, uv);
	glDrawArrays(GL_TRIANGLES, 0, vertex_count);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_VERTEX_ARRAY);
}

void gfx_mesh_create(gfx_mesh_t* m, int has_color, int has_normal) {
	m->has_color = has_color;
	m->has_normal = has_normal;

#ifndef OPENGL_ES
	if(!glx_version || settings.force_displaylist) {
		m->legacy = glGenLists(1);
	} else {
		glGenBuffers(1, &m->modern);
	}
#else
	glGenBuffers(1, &m->modern);
#endif
	m->buffer_size = 0;
}

void gfx_mesh_destroy(gfx_mesh_t* m) {
#ifndef OPENGL_ES
	if(!glx_version || settings.force_displaylist) {
		glDeleteLists(m->legacy, 1);
	} else {
		glDeleteBuffers(1, &m->modern);
	}
#else
	glDeleteBuffers(1, &m->modern);
#endif
}

void gfx_mesh_update(gfx_mesh_t* m, size_t count, gfx_mesh_type_t type, const void* color, const void* vertex,
					 const void* normal) {
	int grow_buffer = count > m->buffer_size;
	m->buffer_size = max(m->buffer_size, count);
	m->size = count;

#ifndef OPENGL_ES
	if(!glx_version || settings.force_displaylist) {
		glEnableClientState(GL_VERTEX_ARRAY);
		if(m->has_color)
			glEnableClientState(GL_COLOR_ARRAY);
		if(m->has_normal)
			glEnableClientState(GL_NORMAL_ARRAY);

		glNewList(m->legacy, GL_COMPILE);
		if(count > 0) {
			if(m->has_color)
				glColorPointer(4, GL_UNSIGNED_BYTE, 0, color);

			switch(type) {
				case GFX_MESH_SHORT: glVertexPointer(3, GL_SHORT, 0, vertex); break;
				case GFX_MESH_POINTS:
				case GFX_MESH_FLOAT: glVertexPointer(3, GL_FLOAT, 0, vertex); break;
			}

			if(m->has_normal)
				glNormalPointer(GL_BYTE, 0, normal);
			glDrawArrays((type == GFX_MESH_POINTS) ? GL_POINTS : GL_QUADS, 0, m->size);
		}
		glEndList();

		glDisableClientState(GL_VERTEX_ARRAY);
		if(m->has_color)
			glDisableClientState(GL_COLOR_ARRAY);
		if(m->has_normal)
			glDisableClientState(GL_NORMAL_ARRAY);
	} else {
#endif
		size_t len_vertex = ((type == GFX_MESH_SHORT) ? sizeof(GLshort) : sizeof(GLfloat)) * 3;
		size_t len_color = m->has_color ? (sizeof(GLubyte) * 4) : 0;
		size_t len_normal = m->has_normal ? (sizeof(GLbyte) * 3) : 0;

		glBindBuffer(GL_ARRAY_BUFFER, m->modern);

		if(grow_buffer) {
			glBufferData(GL_ARRAY_BUFFER, m->size * (len_vertex + len_color + len_normal), NULL, GL_STATIC_DRAW);
		}

		glBufferSubData(GL_ARRAY_BUFFER, 0, m->size * len_vertex, vertex);

		if(m->has_color) {
			glBufferSubData(GL_ARRAY_BUFFER, m->size * len_vertex, m->size * len_color, color);
		}

		if(m->has_normal) {
			glBufferSubData(GL_ARRAY_BUFFER, m->size * (len_vertex + len_color), m->size * len_normal, normal);
		}

		glBindBuffer(GL_ARRAY_BUFFER, 0);
#ifndef OPENGL_ES
	}
#endif
}

void gfx_mesh_draw(gfx_mesh_t* m, gfx_mesh_type_t type) {
#ifndef OPENGL_ES
	if(!glx_version || settings.force_displaylist) {
		glCallList(m->legacy);
	} else {
#endif
		glEnableClientState(GL_VERTEX_ARRAY);
		glBindBuffer(GL_ARRAY_BUFFER, m->modern);

		size_t len_vertex = ((type == GFX_MESH_SHORT) ? sizeof(GLshort) : sizeof(GLfloat)) * 3;
		size_t len_color = m->has_color ? (sizeof(GLubyte) * 4) : 0;
		size_t len_normal = m->has_normal ? (sizeof(GLbyte) * 3) : 0;

		switch(type) {
			case GFX_MESH_SHORT: glVertexPointer(3, GL_SHORT, 0, NULL); break;
			case GFX_MESH_POINTS:
			case GFX_MESH_FLOAT: glVertexPointer(3, GL_FLOAT, 0, NULL); break;
		}

		if(m->has_color) {
			glEnableClientState(GL_COLOR_ARRAY);
			glColorPointer(4, GL_UNSIGNED_BYTE, 0, (const void*)(m->size * len_vertex));
		}

		if(m->has_normal) {
			glEnableClientState(GL_NORMAL_ARRAY);
			glNormalPointer(GL_BYTE, 0, (const void*)(m->size * (len_vertex + len_color)));
		}

		glBindBuffer(GL_ARRAY_BUFFER, 0);

		if(type == GFX_MESH_POINTS) {
			glDrawArrays(GL_POINTS, 0, m->size);
		} else {
#ifdef OPENGL_ES
			glDrawArrays(GL_TRIANGLES, 0, m->size);
#else
			glDrawArrays(GL_QUADS, 0, m->size);
#endif
		}

		if(m->has_normal)
			glDisableClientState(GL_NORMAL_ARRAY);
		if(m->has_color)
			glDisableClientState(GL_COLOR_ARRAY);
		glDisableClientState(GL_VERTEX_ARRAY);
#ifndef OPENGL_ES
	}
#endif
}

void gfx_draw_arrays(gfx_mesh_type_t type, size_t count, const void* vertex, const void* color, const void* normal) {
	glEnableClientState(GL_VERTEX_ARRAY);

	if(normal) {
		glEnableClientState(GL_NORMAL_ARRAY);
		glNormalPointer(GL_BYTE, 0, normal);
	}

	switch(type) {
		case GFX_MESH_SHORT: glVertexPointer(3, GL_SHORT, 0, vertex); break;
		case GFX_MESH_POINTS:
		case GFX_MESH_FLOAT: glVertexPointer(3, GL_FLOAT, 0, vertex); break;
	}

	if(color) {
		glEnableClientState(GL_COLOR_ARRAY);
		glColorPointer(4, GL_UNSIGNED_BYTE, 0, color);
	}

	if(type == GFX_MESH_POINTS) {
		glDrawArrays(GL_POINTS, 0, count);
	} else {
#ifdef OPENGL_ES
		glDrawArrays(GL_TRIANGLES, 0, count);
#else
		glDrawArrays(GL_QUADS, 0, count);
#endif
	}

	if(color)
		glDisableClientState(GL_COLOR_ARRAY);

	glDisableClientState(GL_VERTEX_ARRAY);

	if(normal)
		glDisableClientState(GL_NORMAL_ARRAY);
}

void gfx_color_mask(int r, int g, int b, int a) {
	glColorMask(r ? GL_TRUE : GL_FALSE, g ? GL_TRUE : GL_FALSE, b ? GL_TRUE : GL_FALSE, a ? GL_TRUE : GL_FALSE);
}

void gfx_pass_begin(gfx_pass_t pass) {
	switch(pass) {
		case GFX_PASS_WORLD_3D:
			glEnable(GL_DEPTH_TEST);
			glDepthRange(0.0F, 1.0F);
			break;
		case GFX_PASS_BLOCK_OUTLINE:
			glDisable(GL_DEPTH_TEST);
			glDepthMask(GL_FALSE);
			break;
		case GFX_PASS_DAMAGED:
			glDepthFunc(GL_EQUAL);
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			break;
		case GFX_PASS_COLLAPSING:
			glEnable(GL_BLEND);
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
			break;
		case GFX_PASS_NAMETAG:
			glEnable(GL_ALPHA_TEST);
			glAlphaFunc(GL_GREATER, 0.5F);
			glDisable(GL_DEPTH_TEST);
			break;
		case GFX_PASS_UI_2D:
			glDisable(GL_DEPTH_TEST);
			glDisable(GL_MULTISAMPLE);
			break;
	}
}

void gfx_pass_end(gfx_pass_t pass) {
	switch(pass) {
		case GFX_PASS_WORLD_3D:
			break;
		case GFX_PASS_BLOCK_OUTLINE:
			glEnable(GL_DEPTH_TEST);
			glDepthMask(GL_TRUE);
			break;
		case GFX_PASS_DAMAGED:
			glDepthFunc(GL_LEQUAL);
			glDisable(GL_BLEND);
			break;
		case GFX_PASS_COLLAPSING:
			glDisable(GL_BLEND);
			break;
		case GFX_PASS_NAMETAG:
			glEnable(GL_DEPTH_TEST);
			glDisable(GL_ALPHA_TEST);
			break;
		case GFX_PASS_UI_2D:
			if(settings.multisamples > 0)
				glEnable(GL_MULTISAMPLE);
			break;
	}
}
