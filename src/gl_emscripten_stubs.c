/*
 * Minimal stubs for fixed-function GL entry points that Emscripten
 * LEGACY_GL_EMULATION does not provide under WebGL2.
 */
#ifdef __EMSCRIPTEN__

#include <GL/gl.h>

GLuint glGenLists(GLsizei range) {
	(void)range;
	return 0;
}

void glDeleteLists(GLuint list, GLsizei range) {
	(void)list;
	(void)range;
}

void glNewList(GLuint list, GLenum mode) {
	(void)list;
	(void)mode;
}

void glEndList(void) { }

void glCallList(GLuint list) {
	(void)list;
}

void glColorMaterial(GLenum face, GLenum mode) {
	(void)face;
	(void)mode;
}

void glPointParameterfv(GLenum pname, const GLfloat* params) {
	(void)pname;
	(void)params;
}

void glLightf(GLenum light, GLenum pname, GLfloat param) {
	(void)light;
	(void)pname;
	(void)param;
}

#endif
