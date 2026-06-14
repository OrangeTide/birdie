/*
 * bd_backend_gles.c — birdie-gui backend on raw OpenGL ES 3.
 *
 * Implements the bd_backend GPU vtable (shaders, vertex draws, uniforms,
 * textures, scissor) directly against GLES3, paired with the window.h
 * windowing layer (x11_window.c on Linux). This is the non-ludica reference
 * backend: birdie the MUD client runs on ludica, the widget gallery runs on
 * this, so both backends stay exercised.
 *
 * Opaque handles carry GL names directly: bd_shader.id is a program object,
 * bd_texture.id is a texture object. The toolkit's bd_draw.c owns the default
 * UI shader and quad batching, so this backend only compiles shaders on
 * request, streams a vertex buffer for draw_verts, and manages textures.
 *
 * Seeded from the smoltrek X11/EGL/GLES backend; adopted into birdie-gui.
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_backend_gles.h"

#include <stddef.h>
#include <stdio.h>

#include <GLES3/gl3.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

/* ------------------------------------------------------------------ */
/* GL state                                                           */
/* ------------------------------------------------------------------ */

static struct {
	GLuint vao, vbo;
	int    vbo_cap;          /* current VBO capacity in vertices */
	GLuint cur_program;      /* program bound by use_shader */
	int    cur_window;       /* window made current by window_begin */
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

/* ------------------------------------------------------------------ */
/* frame / window                                                     */
/* ------------------------------------------------------------------ */

/* width()/height() report the window made current by window_begin (the one
 * being rendered), falling back to the primary before any begin. */
static int    be_width(void)  { return win_window_width(gl.cur_window ? gl.cur_window : 1); }
static int    be_height(void) { return win_window_height(gl.cur_window ? gl.cur_window : 1); }
static double be_time(void)   { return win_time(); }

static void be_viewport(int x, int y, int w, int h) { glViewport(x, y, w, h); }

static void
be_clear(float r, float g, float b, float a)
{
	gl_lazy_init();
	/* establish 2D UI render state each frame: alpha blend, no depth or
	 * face culling (UI quads come in either winding) */
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
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

static bd_shader
be_make_shader(const char *vert, const char *frag)
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
	 * layout(location=) qualifier still land at 0/1/2. */
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

static void
be_destroy_shader(bd_shader sh)
{
	if (sh.id)
		glDeleteProgram(sh.id);
}

static void
be_use_shader(bd_shader sh)
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

static void be_uni_int  (bd_shader s, const char *n, int v)
{ glUniform1i(uloc(s, n), v); }
static void be_uni_float(bd_shader s, const char *n, float v)
{ glUniform1f(uloc(s, n), v); }
static void be_uni_vec2 (bd_shader s, const char *n, float x, float y)
{ glUniform2f(uloc(s, n), x, y); }
static void be_uni_vec3 (bd_shader s, const char *n, float x, float y, float z)
{ glUniform3f(uloc(s, n), x, y, z); }
static void be_uni_vec4 (bd_shader s, const char *n, float x, float y, float z, float w)
{ glUniform4f(uloc(s, n), x, y, z, w); }
static void be_uni_mat4 (bd_shader s, const char *n, const float m[16])
{ glUniformMatrix4fv(uloc(s, n), 1, GL_FALSE, m); }

/* ------------------------------------------------------------------ */
/* draw                                                               */
/* ------------------------------------------------------------------ */

static void
be_draw_verts(const bd_vertex *verts, int count)
{
	if (count <= 0)
		return;
	gl_lazy_init();
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

static bd_texture
be_load_texture(const char *path)
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

static bd_texture
be_make_texture(int w, int h, const void *rgba)
{
	return (bd_texture){make_gl_texture(w, h, rgba, GL_LINEAR)};
}

static void
be_update_texture(bd_texture t, int x, int y, int w, int h, const void *rgba)
{
	if (!t.id)
		return;
	glBindTexture(GL_TEXTURE_2D, t.id);
	glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA,
	    GL_UNSIGNED_BYTE, rgba);
}

static void
be_bind_texture(bd_texture t, int unit)
{
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, t.id);
}

static void
be_destroy_texture(bd_texture t)
{
	if (t.id)
		glDeleteTextures(1, &t.id);
}

/* ------------------------------------------------------------------ */
/* scissor                                                            */
/* ------------------------------------------------------------------ */

static void
be_scissor(int x, int y, int w, int h)
{
	/* toolkit clip is top-left pixels; GL scissor origin is bottom-left */
	glEnable(GL_SCISSOR_TEST);
	glScissor(x, win_height() - (y + h), w, h);
}

static void be_scissor_off(void) { glDisable(GL_SCISSOR_TEST); }

/* ------------------------------------------------------------------ */
/* multiple windows                                                   */
/* ------------------------------------------------------------------ */

static int  be_window_open(const char *title, int w, int h)
{ return win_window_open(title, w, h); }
static void be_window_close(int id) { win_window_close(id); }

static void
be_window_begin(int id)
{
	gl.cur_window = id;
	win_window_begin(id);
}

static void be_window_swap(int id)              { win_window_swap(id); }
static int  be_window_width(int id)             { return win_window_width(id); }
static int  be_window_height(int id)            { return win_window_height(id); }
static void be_window_set_title(int id, const char *t) { win_window_set_title(id, t); }

/* ------------------------------------------------------------------ */
/* vtable                                                             */
/* ------------------------------------------------------------------ */

const bd_backend bd_backend_gles = {
	.width             = be_width,
	.height            = be_height,
	.time              = be_time,
	.viewport          = be_viewport,
	.clear             = be_clear,
	.make_shader       = be_make_shader,
	.destroy_shader    = be_destroy_shader,
	.use_shader        = be_use_shader,
	.set_uniform_int   = be_uni_int,
	.set_uniform_float = be_uni_float,
	.set_uniform_vec2  = be_uni_vec2,
	.set_uniform_vec3  = be_uni_vec3,
	.set_uniform_vec4  = be_uni_vec4,
	.set_uniform_mat4  = be_uni_mat4,
	.draw_verts        = be_draw_verts,
	.load_texture      = be_load_texture,
	.make_texture      = be_make_texture,
	.update_texture    = be_update_texture,
	.bind_texture      = be_bind_texture,
	.destroy_texture   = be_destroy_texture,
	.scissor           = be_scissor,
	.scissor_off       = be_scissor_off,
	.multi_window      = 1,
	.window_open       = be_window_open,
	.window_close      = be_window_close,
	.window_begin      = be_window_begin,
	.window_swap       = be_window_swap,
	.window_width      = be_window_width,
	.window_height     = be_window_height,
	.window_set_title  = be_window_set_title,
};

/* ------------------------------------------------------------------ */
/* event translation                                                  */
/* ------------------------------------------------------------------ */

int
bd_event_from_win(const win_event *ev, bd_event *out)
{
	bd_event e = {0};
	e.mods = ev->mods;
	e.window = ev->window;

	switch (ev->type) {
	case WIN_EV_MOUSE_MOVE:
		e.type = BD_EV_MOUSE_MOVE;
		e.x = ev->x;
		e.y = ev->y;
		break;
	case WIN_EV_MOUSE_DOWN:
		e.type = BD_EV_MOUSE_DOWN;
		e.x = ev->x;
		e.y = ev->y;
		e.button = ev->button;
		break;
	case WIN_EV_MOUSE_UP:
		e.type = BD_EV_MOUSE_UP;
		e.x = ev->x;
		e.y = ev->y;
		e.button = ev->button;
		break;
	case WIN_EV_MOUSE_SCROLL:
		e.type = BD_EV_MOUSE_SCROLL;
		e.scroll_dy = ev->scroll_dy;
		break;
	case WIN_EV_KEY_DOWN:
		e.type = BD_EV_KEY_DOWN;
		e.key = ev->key;
		break;
	case WIN_EV_CHAR:
		e.type = BD_EV_CHAR;
		e.codepoint = ev->codepoint;
		break;
	default:
		return 0;       /* close, resize, key-up: no bd_event yet */
	}

	*out = e;
	return 1;
}
