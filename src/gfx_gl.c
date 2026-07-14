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
#include <math.h>

#include "common.h"
#include "config.h"
#include "log.h"
#include "gfx.h"
#include "glx.h"
#include "camera.h"
#include "matrix.h"
#include "map.h"
#include "texture.h"

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

void gfx_color3f(float r, float g, float b) {
	glColor3f(r, g, b);
}

void gfx_color3ub(unsigned char r, unsigned char g, unsigned char b) {
	glColor3ub(r, g, b);
}

void gfx_multisample(int enabled) {
	if(enabled)
		glEnable(GL_MULTISAMPLE);
	else
		glDisable(GL_MULTISAMPLE);
}

void gfx_model_light(const float ambient4[4], const float diffuse4[4]) {
	glLightfv(GL_LIGHT0, GL_AMBIENT, ambient4);
	glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse4);
}

void gfx_model_mesh_begin(gfx_texture_t dummy) {
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glEnable(GL_COLOR_MATERIAL);
#ifndef OPENGL_ES
	glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
#endif
	glEnable(GL_NORMALIZE);

	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
	glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
	glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_CONSTANT);
	glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_CONSTANT);
	glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB, GL_PREVIOUS);
	glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_ALPHA, GL_PREVIOUS);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
	glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_ALPHA, GL_SRC_ALPHA);
	glBindTexture(GL_TEXTURE_2D, (GLuint)dummy);
}

void gfx_model_texenv_color(float r, float g, float b) {
	float c[4] = {r, g, b, 1.0F};
	glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, c);
}

void gfx_model_mesh_end(void) {
	glBindTexture(GL_TEXTURE_2D, 0);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glDisable(GL_TEXTURE_2D);

	glDisable(GL_NORMALIZE);
	glDisable(GL_COLOR_MATERIAL);
	glDisable(GL_LIGHT0);
	glDisable(GL_LIGHTING);
}

void gfx_model_points_begin_fixed(float point_size) {
	glPointParameterfv(GL_POINT_DISTANCE_ATTENUATION, (float[]) {0.0F, 0.0F, 1.0F});
	glPointSize(point_size);
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glEnable(GL_COLOR_MATERIAL);
#ifndef OPENGL_ES
	glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
#endif
	glEnable(GL_NORMALIZE);
}

void gfx_model_points_end_fixed(void) {
	glDisable(GL_NORMALIZE);
	glDisable(GL_COLOR_MATERIAL);
	glDisable(GL_LIGHT0);
	glDisable(GL_LIGHTING);
}

#ifndef OPENGL_ES
static int gfx_kv6_program = -1;

static int gfx_compile_shader(const char* vertex, const char* fragment) {
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
}

static void gfx_kv6_ensure_program(void) {
	if(gfx_kv6_program >= 0)
		return;
	/* Byte-identical to former model.c kv6 point-sprite shader source. */
	gfx_kv6_program
		= gfx_compile_shader("uniform float size;\n"
							 "uniform vec3 fog;\n"
							 "uniform vec3 camera;\n"
							 "uniform mat4 model;\n"
							 "uniform float dist_factor;\n"
							 "void main(void) {\n"
							 "	gl_Position = gl_ModelViewProjectionMatrix*gl_Vertex;\n"
							 "	float dist = length((model*gl_Vertex).xz-camera.xz)*dist_factor;\n"
							 "	vec3 N = normalize(model*vec4(gl_Normal,0)).xyz;\n"
							 "	vec3 L = normalize(vec3(0,-1,1));\n"
							 "	float d = clamp(dot(N,L),0.0,1.0)*0.5+0.5;\n"
							 "	gl_FrontColor = mix(vec4(d,d,d,1.0)*gl_Color,vec4(fog,1.0),min(dist,1.0));\n"
							 "	gl_PointSize = size/gl_Position.w;\n"
							 "}\n",
							 "void main(void) {\n"
							 "	gl_FragColor = gl_Color;\n"
							 "}\n");
}
#endif

void gfx_model_points_begin_shader(float point_size, float dist_factor, const float fog_rgb[3], const float camera[3],
								   const float model16[16]) {
#ifndef OPENGL_ES
	gfx_kv6_ensure_program();
	glEnable(GL_PROGRAM_POINT_SIZE);
	glUseProgram(gfx_kv6_program);
	glUniform1f(glGetUniformLocation(gfx_kv6_program, "dist_factor"), dist_factor);
	glUniform1f(glGetUniformLocation(gfx_kv6_program, "size"), point_size);
	glUniform3f(glGetUniformLocation(gfx_kv6_program, "fog"), fog_rgb[0], fog_rgb[1], fog_rgb[2]);
	glUniform3f(glGetUniformLocation(gfx_kv6_program, "camera"), camera[0], camera[1], camera[2]);
	glUniformMatrix4fv(glGetUniformLocation(gfx_kv6_program, "model"), 1, 0, model16);
#else
	(void)point_size;
	(void)dist_factor;
	(void)fog_rgb;
	(void)camera;
	(void)model16;
#endif
}

void gfx_model_points_end_shader(void) {
#ifndef OPENGL_ES
	glUseProgram(0);
	glDisable(GL_PROGRAM_POINT_SIZE);
#endif
}

void gfx_fog_enable_exp2(const float color4[4], float density) {
#ifdef OPENGL_ES
	glFogx(GL_FOG_MODE, GL_EXP2);
#else
	glFogi(GL_FOG_MODE, GL_EXP2);
#endif
	glFogf(GL_FOG_DENSITY, density);
	glFogfv(GL_FOG_COLOR, color4);
	glEnable(GL_FOG);
}

void gfx_fog_disable(void) {
	glDisable(GL_FOG);
}

void gfx_fog_enable_spherical(void) {
#ifndef OPENGL_ES
	if(!settings.smooth_fog) {
		glActiveTexture(GL_TEXTURE1);
		glEnable(GL_TEXTURE_2D);
		glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, (float[]) {fog_color[0], fog_color[1], fog_color[2], 1.0F});
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_BLEND);
		glBindTexture(GL_TEXTURE_2D, texture_gradient.texture_id);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexGeni(GL_T, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
		glTexGeni(GL_S, GL_TEXTURE_GEN_MODE, GL_EYE_LINEAR);
		glTexGenfv(GL_T, GL_EYE_PLANE,
				   (float[]) {1.0F / settings.render_distance / 2.0F, 0.0F, 0.0F,
							  -camera_x / settings.render_distance / 2.0F + 0.5F});
		glTexGenfv(GL_S, GL_EYE_PLANE,
				   (float[]) {0.0F, 0.0F, 1.0F / settings.render_distance / 2.0F,
							  -camera_z / settings.render_distance / 2.0F + 0.5F});
		glEnable(GL_TEXTURE_GEN_T);
		glEnable(GL_TEXTURE_GEN_S);
		glActiveTexture(GL_TEXTURE0);
	} else {
		matrix_push(matrix_model);
		matrix_identity(matrix_model);
		matrix_upload();
		matrix_pop(matrix_model);

		glEnable(GL_LIGHTING);
		glEnable(GL_LIGHT1);
		glEnable(GL_COLOR_MATERIAL);
		glColorMaterial(GL_FRONT, GL_DIFFUSE);
		glLightModelfv(GL_LIGHT_MODEL_AMBIENT, (float[]) {fog_color[0], fog_color[1], fog_color[2], 1.0F});

		glLightfv(GL_LIGHT1, GL_POSITION,
				  (float[]) {camera_x, (settings.render_distance * map_size_y) / 16.0F, camera_z, 1.0F});
		glLightfv(GL_LIGHT1, GL_SPOT_DIRECTION, (float[]) {0.0F, -1.0F, 0.0F});
		glLightfv(GL_LIGHT1, GL_DIFFUSE, (float[]) {1.0F, 1.0F, 1.0F, 1.0F});
		glLightfv(GL_LIGHT1, GL_AMBIENT, (float[]) {-fog_color[0], -fog_color[1], -fog_color[2], 1.0F});
		glLightf(GL_LIGHT1, GL_SPOT_CUTOFF, tan(16.0F / map_size_y) / PI * 180.0F);
		glLightf(GL_LIGHT1, GL_SPOT_EXPONENT, 128.0F);
		glNormal3f(0.0F, 1.0F, 0.0F);
	}
#else
	matrix_push(matrix_model);
	matrix_identity(matrix_model);
	matrix_upload();
	matrix_pop(matrix_model);

	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT1);
	glEnable(GL_COLOR_MATERIAL);
	float amb[4] = {0.0F, 0.0F, 0.0F, 1.0F};
	glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);

	float lpos[4] = {camera_x, (settings.render_distance * map_size_y) / 16.0F, camera_z, 1.0F};
	glLightfv(GL_LIGHT1, GL_POSITION, lpos);
	float dir[3] = {0.0F, -1.0F, 0.0F};
	glLightfv(GL_LIGHT1, GL_SPOT_DIRECTION, dir);
	float dif[4] = {0.0F, 0.0F, 0.0F, 1.0F};
	glLightfv(GL_LIGHT1, GL_DIFFUSE, dif);
	float amb2[4] = {1.0F, 1.0F, 1.0F, 1.0F};
	glLightfv(GL_LIGHT1, GL_AMBIENT, amb2);
	glLightf(GL_LIGHT1, GL_SPOT_CUTOFF, tan(16.0F / map_size_y) / PI * 180.0F);
	glLightf(GL_LIGHT1, GL_SPOT_EXPONENT, 128.0F);
	glNormal3f(0.0F, 1.0F, 0.0F);
	glEnable(GL_FOG);
	glFogf(GL_FOG_MODE, GL_LINEAR);
	glFogf(GL_FOG_START, 0.0F);
	glFogf(GL_FOG_END, settings.render_distance);
	glFogfv(GL_FOG_COLOR, fog_color);
#endif
	glx_fog = 1;
}

void gfx_fog_disable_spherical(void) {
#ifndef OPENGL_ES
	if(!settings.smooth_fog) {
		glActiveTexture(GL_TEXTURE1);
		glDisable(GL_TEXTURE_GEN_T);
		glDisable(GL_TEXTURE_GEN_S);
		glBindTexture(GL_TEXTURE_2D, 0);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
		glDisable(GL_TEXTURE_2D);
		glActiveTexture(GL_TEXTURE0);
	} else {
		glDisable(GL_COLOR_MATERIAL);
		glDisable(GL_LIGHT1);
		glDisable(GL_LIGHTING);
		float a[4] = {0.2F, 0.2F, 0.2F, 1.0F};
		glLightModelfv(GL_LIGHT_MODEL_AMBIENT, a);
	}
#else
	glDisable(GL_FOG);
	glDisable(GL_COLOR_MATERIAL);
	glDisable(GL_LIGHT1);
	glDisable(GL_LIGHTING);
	float a[4] = {0.2F, 0.2F, 0.2F, 1.0F};
	glLightModelfv(GL_LIGHT_MODEL_AMBIENT, a);
#endif
	glx_fog = 0;
}

int gfx_fog_active(void) {
	return glx_fog;
}
