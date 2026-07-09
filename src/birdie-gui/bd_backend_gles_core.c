/*
 * bd_backend_gles_core.c — GPU half of a bd_backend on raw OpenGL ES 3.
 *
 * Shaders, a streaming quad batch, textures, viewport/clear/scissor: the code
 * every GLES host shares. The SDL3 backend and the raw X11/EGL/GLES gallery
 * backend both bind their vtable GPU rows here so the implementation lives in
 * one place; each host adds only its own windowing, input, clipboard, and IME.
 *
 * The caller owns the GL context and must make it current before calling in.
 * See bd_backend_gles_core.h for the contract.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_backend_gles_core.h"

#include <GLES3/gl3.h>

#include <stddef.h>
#include <stdio.h>

/* stb_image decodes PNGs for the backend's load_texture / load_texture_mem
 * (app-supplied textures; the toolkit's own fonts and pushpins are baked from
 * embedded bitmaps, not PNGs). It is bundled with birdie-gui; this core owns the
 * single implementation so any GLES backend built against it resolves stbi_load
 * without its own copy. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

/* GL streaming state for the toolkit's 2D quads. One context is current at a
 * time, so a single instance serves whichever backend is linked. */
static struct {
	GLuint vao, vbo;
	int    vbo_cap;          /* current VBO capacity in vertices */
	GLuint cur_program;      /* program bound by use_shader */
	int    ready;
} gl;

static void
gl_lazy_init(void)
{
	if (gl.ready)
		return;
	gl.ready = 1;

	glGenVertexArrays(1, &gl.vao);
	glGenBuffers(1, &gl.vbo);
	glBindVertexArray(gl.vao);
	glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(bd_vertex),
	    (void *)offsetof(bd_vertex, x));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(bd_vertex),
	    (void *)offsetof(bd_vertex, u));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(bd_vertex),
	    (void *)offsetof(bd_vertex, r));
	glBindVertexArray(0);
}

void
bd_gles_viewport(int x, int y, int w, int h)
{
	glViewport(x, y, w, h);
}

void
bd_gles_clear(float r, float g, float b, float a)
{
	gl_lazy_init();
	glClearColor(r, g, b, a);
	glClear(GL_COLOR_BUFFER_BIT);
}

/* ------------------------------------------------------------------ */
/* shaders                                                            */
/* ------------------------------------------------------------------ */

static GLuint
compile(GLenum type, const char *src)
{
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		fprintf(stderr, "bd_gles: shader compile failed: %s\n", log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

bd_shader
bd_gles_make_shader(const char *vert, const char *frag)
{
	GLuint vs = compile(GL_VERTEX_SHADER, vert);
	GLuint fs = compile(GL_FRAGMENT_SHADER, frag);
	if (!vs || !fs) {
		if (vs) glDeleteShader(vs);
		if (fs) glDeleteShader(fs);
		return (bd_shader){0};
	}

	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	/* Bind the shared bd_vertex attribute names so shaders that omit the
	 * layout(location=) qualifier still land at 0/1/2. Shaders that use an
	 * explicit layout override these. */
	glBindAttribLocation(prog, 0, "a_pos");
	glBindAttribLocation(prog, 1, "a_uv");
	glBindAttribLocation(prog, 2, "a_col");
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[512];
		glGetProgramInfoLog(prog, sizeof log, NULL, log);
		fprintf(stderr, "bd_gles: program link failed: %s\n", log);
		glDeleteProgram(prog);
		return (bd_shader){0};
	}
	return (bd_shader){prog};
}

void
bd_gles_destroy_shader(bd_shader sh)
{
	if (sh.id)
		glDeleteProgram(sh.id);
}

void
bd_gles_use_shader(bd_shader sh)
{
	gl.cur_program = sh.id;
	glUseProgram(sh.id);
}

/* glUniform* operate on the active program in GLES3, so make sure the target
 * program is current before setting any uniform. */
static GLint
uloc(bd_shader sh, const char *name)
{
	if (gl.cur_program != sh.id) {
		gl.cur_program = sh.id;
		glUseProgram(sh.id);
	}
	return glGetUniformLocation(sh.id, name);
}

void bd_gles_uniform_int  (bd_shader s, const char *n, int v)
{ glUniform1i(uloc(s, n), v); }
void bd_gles_uniform_float(bd_shader s, const char *n, float v)
{ glUniform1f(uloc(s, n), v); }
void bd_gles_uniform_vec2 (bd_shader s, const char *n, float x, float y)
{ glUniform2f(uloc(s, n), x, y); }
void bd_gles_uniform_vec3 (bd_shader s, const char *n, float x, float y, float z)
{ glUniform3f(uloc(s, n), x, y, z); }
void bd_gles_uniform_vec4 (bd_shader s, const char *n, float x, float y, float z, float w)
{ glUniform4f(uloc(s, n), x, y, z, w); }
void bd_gles_uniform_mat4 (bd_shader s, const char *n, const float m[16])
{ glUniformMatrix4fv(uloc(s, n), 1, GL_FALSE, m); }

/* ------------------------------------------------------------------ */
/* draw                                                               */
/* ------------------------------------------------------------------ */

void
bd_gles_draw_verts(const bd_vertex *verts, int count)
{
	if (count <= 0)
		return;
	gl_lazy_init();
	/* establish 2D UI render state for the draw: alpha blend, no depth or
	 * face culling (UI quads come in either winding). Done here rather than in
	 * clear so the toolkit renders correctly even when clear is NULL (a
	 * compositing host owning the frame). */
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glBindVertexArray(gl.vao);
	glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
	if (count > gl.vbo_cap) {
		/* grow (and orphan) the streaming buffer */
		glBufferData(GL_ARRAY_BUFFER,
		    (GLsizeiptr)count * (GLsizeiptr)sizeof(bd_vertex),
		    verts, GL_STREAM_DRAW);
		gl.vbo_cap = count;
	} else {
		/* orphan then upload to avoid a GPU stall on reuse */
		glBufferData(GL_ARRAY_BUFFER,
		    (GLsizeiptr)gl.vbo_cap * (GLsizeiptr)sizeof(bd_vertex),
		    NULL, GL_STREAM_DRAW);
		glBufferSubData(GL_ARRAY_BUFFER, 0,
		    (GLsizeiptr)count * (GLsizeiptr)sizeof(bd_vertex), verts);
	}
	glDrawArrays(GL_TRIANGLES, 0, count);
}

/* ------------------------------------------------------------------ */
/* textures                                                           */
/* ------------------------------------------------------------------ */

static GLuint
make_gl_texture(int w, int h, const void *rgba, GLint filter)
{
	GLuint id;
	glGenTextures(1, &id);
	glBindTexture(GL_TEXTURE_2D, id);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
	    GL_UNSIGNED_BYTE, rgba);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return id;
}

bd_texture
bd_gles_load_texture(const char *path)
{
	int w, h, comp;
	unsigned char *pixels = stbi_load(path, &w, &h, &comp, 4);
	if (!pixels) {
		fprintf(stderr, "bd_gles: cannot load texture '%s'\n", path);
		return (bd_texture){0};
	}
	GLuint id = make_gl_texture(w, h, pixels, GL_NEAREST); /* pixel-art PNGs */
	stbi_image_free(pixels);
	return (bd_texture){id};
}

/* Decode a PNG from memory, matching bd_gles_load_texture's NEAREST filtering. */
bd_texture
bd_gles_load_texture_mem(const unsigned char *data, int len)
{
	int w, h, comp;
	unsigned char *pixels = stbi_load_from_memory(data, len, &w, &h, &comp, 4);
	if (!pixels) {
		fprintf(stderr, "bd_gles: cannot decode embedded texture\n");
		return (bd_texture){0};
	}
	GLuint id = make_gl_texture(w, h, pixels, GL_NEAREST); /* pixel-art PNGs */
	stbi_image_free(pixels);
	return (bd_texture){id};
}

bd_texture
bd_gles_make_texture(int w, int h, const void *rgba)
{
	return (bd_texture){make_gl_texture(w, h, rgba, GL_LINEAR)};
}

void
bd_gles_update_texture(bd_texture t, int x, int y, int w, int h, const void *rgba)
{
	if (!t.id)
		return;
	glBindTexture(GL_TEXTURE_2D, t.id);
	glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA,
	    GL_UNSIGNED_BYTE, rgba);
}

void
bd_gles_bind_texture(bd_texture t, int unit)
{
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, t.id);
}

void
bd_gles_destroy_texture(bd_texture t)
{
	if (t.id)
		glDeleteTextures(1, &t.id);
}

/* ------------------------------------------------------------------ */
/* scissor                                                            */
/* ------------------------------------------------------------------ */

void
bd_gles_scissor(int x, int y, int w, int h, int fb_height)
{
	/* toolkit clip is top-left pixels; GL scissor origin is bottom-left.
	 * Flip against the framebuffer height the caller passes (the window
	 * being rendered). */
	glEnable(GL_SCISSOR_TEST);
	glScissor(x, fb_height - (y + h), w, h);
}

void
bd_gles_scissor_off(void)
{
	glDisable(GL_SCISSOR_TEST);
}
