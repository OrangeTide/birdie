/*
 * sdl3_example.c — birdie-gui on an SDL3 window + OpenGL ES 3 context, mixing
 * the toolkit's 2D UI with a hand-written 3D scene.
 *
 * The window shows a rotatable 3D tetrahedron drawn with raw GLES3 as the
 * background, and a birdie-gui UI composited on top: a floating "terminal"
 * subwindow you can drag by its title bar and minimize, plus a hint line.
 * Drag anywhere over the 3D background to rotate the tetrahedron; the mouse
 * wheel zooms.
 *
 * It is a third reference backend alongside ludica (bd_backend_ludica.c) and
 * the raw X11/EGL/GLES gallery (src/guitest/): the toolkit and renderer are
 * untouched, only the bd_backend vtable and the SDL event translation change.
 *
 * Layering trick: the toolkit renders the whole UI in one pass that normally
 * begins by clearing the framebuffer. Here the *host* owns the clear and draws
 * the 3D scene first, so this backend's clear() is a deliberate no-op and the
 * UI composites over the 3D. The toolkit's root frame is transparent (bg alpha
 * 0), so only the opaque subwindow and text land on top of the tetrahedron.
 *
 * The examples are a separate modular-make project (examples/ has its own copy
 * of GNUmakefile) so the main birdie build never depends on SDL3. Build and run
 * (needs SDL3 via pkg-config; libvt for the terminal widget is built from the
 * parent tree):
 *
 *   cd examples && make
 *   cd .. && examples/_out/<triplet>/bin/sdl3_example
 *
 * Run it from the repo root so the compiled-in BD_ASSET_* font/pushpin paths
 * resolve against src/birdie/assets/.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "widget.h"
#include "widget_ext.h"     /* bd_widget_rect */
#include "bd_backend.h"
#include "bd_widget_vt.h"

#include <SDL3/SDL.h>
#include <GLES3/gl3.h>

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* stb_image decodes the toolkit's PNG assets (terminal atlas, pushpins). It is
 * bundled with birdie-gui; we own the single implementation here since this
 * example does not link the gallery backend that normally provides it. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

/* ------------------------------------------------------------------ */
/* SDL window + GL context                                            */
/* ------------------------------------------------------------------ */

static struct {
	SDL_Window   *win;
	SDL_GLContext ctx;
	char         *clip;      /* last clipboard string handed out (SDL-owned) */

	/* GL streaming state for the toolkit's 2D quads. */
	GLuint vao, vbo;
	int    vbo_cap;          /* current VBO capacity in vertices */
	GLuint cur_program;      /* program bound by use_shader */
	int    ready;
} S;

static void
gl_lazy_init(void)
{
	if (S.ready)
		return;
	S.ready = 1;

	glGenVertexArrays(1, &S.vao);
	glGenBuffers(1, &S.vbo);
	glBindVertexArray(S.vao);
	glBindBuffer(GL_ARRAY_BUFFER, S.vbo);
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

static int
be_width(void)
{
	int w = 0, h = 0;
	SDL_GetWindowSizeInPixels(S.win, &w, &h);
	return w;
}

static int
be_height(void)
{
	int w = 0, h = 0;
	SDL_GetWindowSizeInPixels(S.win, &w, &h);
	return h;
}

static double be_time(void) { return SDL_GetTicks() / 1000.0; }

static void be_viewport(int x, int y, int w, int h) { glViewport(x, y, w, h); }

/* The host clears the framebuffer and draws the 3D background itself (see
 * main()), so the toolkit's per-frame clear only re-establishes the 2D UI
 * render state and leaves the color/depth buffers intact. That is what lets the
 * UI composite on top of the tetrahedron. The r/g/b/a here are intentionally
 * ignored. */
static void
be_clear(float r, float g, float b, float a)
{
	(void)r; (void)g; (void)b; (void)a;
	gl_lazy_init();
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
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
		fprintf(stderr, "sdl3: shader compile failed: %s\n", log);
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
	 * layout(location=) qualifier still land at 0/1/2. Shaders that use an
	 * explicit layout (like the 3D scene below) override these. */
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
		fprintf(stderr, "sdl3: program link failed: %s\n", log);
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
	S.cur_program = sh.id;
	glUseProgram(sh.id);
}

/* glUniform* target the active program in GLES3, so bind first. */
static GLint
uloc(bd_shader sh, const char *name)
{
	if (S.cur_program != sh.id) {
		S.cur_program = sh.id;
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
	glBindVertexArray(S.vao);
	glBindBuffer(GL_ARRAY_BUFFER, S.vbo);
	if (count > S.vbo_cap) {
		glBufferData(GL_ARRAY_BUFFER,
		    (GLsizeiptr)count * (GLsizeiptr)sizeof(bd_vertex),
		    verts, GL_STREAM_DRAW);
		S.vbo_cap = count;
	} else {
		glBufferData(GL_ARRAY_BUFFER,
		    (GLsizeiptr)S.vbo_cap * (GLsizeiptr)sizeof(bd_vertex),
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
		fprintf(stderr, "sdl3: cannot load texture '%s'\n", path);
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
	/* toolkit clip is top-left pixels; GL scissor origin is bottom-left. */
	glEnable(GL_SCISSOR_TEST);
	glScissor(x, be_height() - (y + h), w, h);
}

static void be_scissor_off(void) { glDisable(GL_SCISSOR_TEST); }

/* ------------------------------------------------------------------ */
/* clipboard + IME (SDL)                                              */
/* ------------------------------------------------------------------ */

static void
be_clipboard_set(const char *utf8)
{
	SDL_SetClipboardText(utf8 ? utf8 : "");
}

static const char *
be_clipboard_get(void)
{
	if (S.clip)
		SDL_free(S.clip);
	S.clip = SDL_GetClipboardText();
	return (S.clip && S.clip[0]) ? S.clip : NULL;
}

static void
be_ime_set_enabled(int on)
{
	if (on)
		SDL_StartTextInput(S.win);
	else
		SDL_StopTextInput(S.win);
}

static void
be_ime_set_cursor_rect(int x, int y, int w, int h)
{
	SDL_Rect r = { x, y, w, h };
	SDL_SetTextInputArea(S.win, &r, 0);
}

/* ------------------------------------------------------------------ */
/* vtable                                                             */
/* ------------------------------------------------------------------ */

static const bd_backend backend = {
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
	.multi_window      = 0,      /* one window; the host swaps it itself */
	.clipboard_set     = be_clipboard_set,
	.clipboard_get     = be_clipboard_get,
	.ime_set_enabled     = be_ime_set_enabled,
	.ime_set_cursor_rect = be_ime_set_cursor_rect,
};

/* ================================================================== */
/* 3D tetrahedron (raw GLES3, drawn as the background)                */
/* ================================================================== */

/* Minimal column-major 4x4 matrix math. M(m,row,col) indexes an element. */
#define M(m, r, c) ((m)[(c) * 4 + (r)])

static void
mat4_identity(float *m)
{
	memset(m, 0, 16 * sizeof(float));
	M(m, 0, 0) = M(m, 1, 1) = M(m, 2, 2) = M(m, 3, 3) = 1.0f;
}

/* out = a * b (out may alias neither a nor b). */
static void
mat4_mul(float *out, const float *a, const float *b)
{
	float r[16];
	for (int c = 0; c < 4; c++)
		for (int row = 0; row < 4; row++) {
			float s = 0.0f;
			for (int k = 0; k < 4; k++)
				s += M(a, row, k) * M(b, k, c);
			r[c * 4 + row] = s;
		}
	memcpy(out, r, sizeof r);
}

static void
mat4_perspective(float *m, float fovy, float aspect, float n, float f)
{
	float t = tanf(fovy * 0.5f);
	memset(m, 0, 16 * sizeof(float));
	M(m, 0, 0) = 1.0f / (aspect * t);
	M(m, 1, 1) = 1.0f / t;
	M(m, 2, 2) = (f + n) / (n - f);
	M(m, 3, 2) = -1.0f;
	M(m, 2, 3) = (2.0f * f * n) / (n - f);
}

static void
mat4_rot_x(float *m, float a)
{
	mat4_identity(m);
	float c = cosf(a), s = sinf(a);
	M(m, 1, 1) = c; M(m, 1, 2) = -s;
	M(m, 2, 1) = s; M(m, 2, 2) = c;
}

static void
mat4_rot_y(float *m, float a)
{
	mat4_identity(m);
	float c = cosf(a), s = sinf(a);
	M(m, 0, 0) = c; M(m, 0, 2) = s;
	M(m, 2, 0) = -s; M(m, 2, 2) = c;
}

static const char *TET_VERT =
	"#version 300 es\n"
	"layout(location=0) in vec3 a_pos;\n"
	"layout(location=1) in vec3 a_col;\n"
	"layout(location=2) in vec3 a_norm;\n"
	"uniform mat4 u_mvp;\n"
	"uniform mat4 u_model;\n"
	"out vec3 v_col;\n"
	"out vec3 v_norm;\n"
	"void main(){\n"
	"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"    v_norm = mat3(u_model) * a_norm;\n"
	"    v_col = a_col;\n"
	"}\n";

static const char *TET_FRAG =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec3 v_col;\n"
	"in vec3 v_norm;\n"
	"out vec4 frag;\n"
	"void main(){\n"
	"    vec3 L = normalize(vec3(0.4, 0.7, 0.6));\n"
	"    float d = abs(dot(normalize(v_norm), L));\n"  /* two-sided */
	"    frag = vec4(v_col * (d * 0.75 + 0.25), 1.0);\n"
	"}\n";

static struct {
	bd_shader prog;
	GLuint    vao, vbo;
	int       verts;
} tet;

/* Append one triangular face (positions + a flat color + face normal). */
static void
tet_face(float *buf, int *n, const float a[3], const float b[3],
    const float c[3], const float col[3])
{
	/* face normal = normalize((b-a) x (c-a)) */
	float u[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
	float v[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
	float nrm[3] = {
		u[1]*v[2] - u[2]*v[1],
		u[2]*v[0] - u[0]*v[2],
		u[0]*v[1] - u[1]*v[0],
	};
	float len = sqrtf(nrm[0]*nrm[0] + nrm[1]*nrm[1] + nrm[2]*nrm[2]);
	if (len > 0.0f) { nrm[0]/=len; nrm[1]/=len; nrm[2]/=len; }

	const float *tri[3] = { a, b, c };
	for (int i = 0; i < 3; i++) {
		float *o = buf + (*n) * 9;
		o[0]=tri[i][0]; o[1]=tri[i][1]; o[2]=tri[i][2];
		o[3]=col[0];    o[4]=col[1];    o[5]=col[2];
		o[6]=nrm[0];    o[7]=nrm[1];    o[8]=nrm[2];
		(*n)++;
	}
}

static void
tet_init(void)
{
	/* Regular tetrahedron, scaled to fit the view. */
	const float k = 0.85f;
	float p0[3] = {  k,  k,  k };
	float p1[3] = {  k, -k, -k };
	float p2[3] = { -k,  k, -k };
	float p3[3] = { -k, -k,  k };

	float red[3]    = { 0.90f, 0.28f, 0.30f };
	float green[3]  = { 0.35f, 0.78f, 0.42f };
	float blue[3]   = { 0.32f, 0.55f, 0.95f };
	float yellow[3] = { 0.95f, 0.80f, 0.30f };

	float buf[12 * 9];
	int n = 0;
	tet_face(buf, &n, p0, p1, p2, red);
	tet_face(buf, &n, p0, p3, p1, green);
	tet_face(buf, &n, p0, p2, p3, blue);
	tet_face(buf, &n, p1, p3, p2, yellow);
	tet.verts = n;

	tet.prog = be_make_shader(TET_VERT, TET_FRAG);

	glGenVertexArrays(1, &tet.vao);
	glGenBuffers(1, &tet.vbo);
	glBindVertexArray(tet.vao);
	glBindBuffer(GL_ARRAY_BUFFER, tet.vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 9 * n, buf, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float),
	    (void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float),
	    (void *)(3 * sizeof(float)));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float),
	    (void *)(6 * sizeof(float)));
	glBindVertexArray(0);
}

/* Draw the tetrahedron into the full window at the given rotation and camera
 * distance. Leaves depth testing on; the toolkit's clear() turns it back off
 * before the UI draws. */
static void
tet_draw(float rot_x, float rot_y, float cam_dist, int w, int h)
{
	glViewport(0, 0, w, h);
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);   /* two-sided lighting; winding-agnostic */

	float rx[16], ry[16], model[16], view[16], proj[16], vm[16], mvp[16];
	mat4_rot_x(rx, rot_x);
	mat4_rot_y(ry, rot_y);
	mat4_mul(model, ry, rx);                 /* model = Ry * Rx (rotation only) */

	mat4_identity(view);
	M(view, 2, 3) = -cam_dist;               /* translate along -Z */

	float aspect = h > 0 ? (float)w / (float)h : 1.0f;
	mat4_perspective(proj, 0.8f, aspect, 0.1f, 100.0f);

	mat4_mul(vm, view, model);
	mat4_mul(mvp, proj, vm);

	be_use_shader(tet.prog);
	be_uni_mat4(tet.prog, "u_mvp", mvp);
	be_uni_mat4(tet.prog, "u_model", model);

	glBindVertexArray(tet.vao);
	glDrawArrays(GL_TRIANGLES, 0, tet.verts);
	glBindVertexArray(0);
}

/* ------------------------------------------------------------------ */
/* SDL event -> bd_event translation                                  */
/* ------------------------------------------------------------------ */

static int
sdl_mods(void)
{
	SDL_Keymod m = SDL_GetModState();
	int out = 0;
	if (m & SDL_KMOD_SHIFT) out |= BD_MOD_SHIFT;
	if (m & SDL_KMOD_CTRL)  out |= BD_MOD_CTRL;
	if (m & SDL_KMOD_ALT)   out |= BD_MOD_ALT;
	return out;
}

/* Map an SDL keycode to a BD_KEY_*. Letters fold to their uppercase ASCII so
 * Ctrl-C/X/V reach the toolkit's clipboard shortcuts; typed text arrives
 * separately as SDL_EVENT_TEXT_INPUT. Returns BD_KEY_UNKNOWN if unmapped. */
static int
map_key(SDL_Keycode kc)
{
	if (kc >= 'a' && kc <= 'z')
		return BD_KEY_A + (kc - 'a');
	switch (kc) {
	case SDLK_LEFT:      return BD_KEY_LEFT;
	case SDLK_RIGHT:     return BD_KEY_RIGHT;
	case SDLK_UP:        return BD_KEY_UP;
	case SDLK_DOWN:      return BD_KEY_DOWN;
	case SDLK_HOME:      return BD_KEY_HOME;
	case SDLK_END:       return BD_KEY_END;
	case SDLK_BACKSPACE: return BD_KEY_BACKSPACE;
	case SDLK_DELETE:    return BD_KEY_DELETE;
	case SDLK_RETURN:    return BD_KEY_ENTER;
	case SDLK_KP_ENTER:  return BD_KEY_ENTER;
	case SDLK_ESCAPE:    return BD_KEY_ESCAPE;
	case SDLK_TAB:       return BD_KEY_TAB;
	case SDLK_F2:        return BD_KEY_F2;
	default:             return BD_KEY_UNKNOWN;
	}
}

/* Translate one SDL event into a bd_event. Returns 1 if *out was written. */
static int
translate(const SDL_Event *ev, bd_event *out)
{
	bd_event e = {0};
	e.mods = sdl_mods();

	switch (ev->type) {
	case SDL_EVENT_MOUSE_MOTION:
		e.type = BD_EV_MOUSE_MOVE;
		e.x = (int)ev->motion.x;
		e.y = (int)ev->motion.y;
		break;
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
	case SDL_EVENT_MOUSE_BUTTON_UP:
		e.type = ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN
		    ? BD_EV_MOUSE_DOWN : BD_EV_MOUSE_UP;
		e.x = (int)ev->button.x;
		e.y = (int)ev->button.y;
		e.button = ev->button.button == SDL_BUTTON_LEFT   ? BD_MOUSE_LEFT
		    : ev->button.button == SDL_BUTTON_RIGHT       ? BD_MOUSE_RIGHT
		    : ev->button.button == SDL_BUTTON_MIDDLE      ? BD_MOUSE_MIDDLE
		    : 0;
		break;
	case SDL_EVENT_MOUSE_WHEEL:
		e.type = BD_EV_MOUSE_SCROLL;
		e.scroll_dy = ev->wheel.y;
		e.x = (int)ev->wheel.mouse_x;
		e.y = (int)ev->wheel.mouse_y;
		break;
	case SDL_EVENT_KEY_DOWN:
		e.type = BD_EV_KEY_DOWN;
		e.key = map_key(ev->key.key);
		e.repeat = ev->key.repeat ? 1 : 0;
		if (e.key == BD_KEY_UNKNOWN)
			return 0;
		break;
	case SDL_EVENT_KEY_UP:
		e.type = BD_EV_KEY_UP;
		e.key = map_key(ev->key.key);
		if (e.key == BD_KEY_UNKNOWN)
			return 0;
		break;
	case SDL_EVENT_TEXT_INPUT:
		e.type = BD_EV_TEXT_COMMIT;
		e.text = ev->text.text;
		break;
	default:
		return 0;
	}

	*out = e;
	return 1;
}

/* ================================================================== */
/* demo UI: a floating, minimizable terminal subwindow                */
/* ================================================================== */

enum {
	SUBWIN_W      = 560,
	SUBWIN_FULL_H = 340,
	TITLE_H       = 26,
};

static bd_id root;
static bd_id subwin;     /* the floating window panel */
static bd_id titlebar;   /* its drag handle */
static bd_id min_btn;    /* minimize / restore button */
static bd_id term;       /* terminal body */
static bd_id term_input; /* command line */
static int   minimized;

static void
report(const char *msg)
{
	if (term) {
		bd_terminal_write(term, msg, -1);
		bd_terminal_write(term, "\r\n", 2);
	}
}

static void
on_minimize(bd_id id, void *arg)
{
	(void)id; (void)arg;
	minimized = !minimized;
	bd_set(term,       BD_VISIBLE_B, !minimized, BD_END);
	bd_set(term_input, BD_VISIBLE_B, !minimized, BD_END);
	bd_set(subwin, BD_PREF_H_I, minimized ? TITLE_H : SUBWIN_FULL_H, BD_END);
	bd_set(min_btn, BD_LABEL_S, minimized ? "+" : "_", BD_END);
}

/* The input line fires its click handler on Enter, before it clears, so the
 * submitted text is still readable here. Echo it to the terminal. */
static void
on_submit(bd_id id, void *arg)
{
	(void)arg;
	const char *cmd = bd_get_s(id, BD_LABEL_S);
	if (!cmd || !cmd[0])
		return;
	char line[1024];
	snprintf(line, sizeof line, "> %s", cmd);
	report(line);
}

static void
build_ui(void)
{
	/* Transparent, fixed-layout root: children float at absolute X/Y and the
	 * 3D background shows through everywhere they don't paint. */
	root = bd_create(BD_NONE, BD_FRAME,
	    BD_LAYOUT_I, BD_LAYOUT_FIXED,
	    BD_PAD_I,    0,
	    BD_BG_C,     0x00000000u,     /* alpha 0 => nothing drawn */
	    BD_END);

	bd_create(root, BD_LABEL,
	    BD_LABEL_S, "Drag the tetrahedron to rotate  -  wheel to zoom  -  "
	                "drag the title bar to move the terminal, _ to minimize",
	    BD_X_I, 12, BD_Y_I, 10, BD_PREF_W_I, 900, BD_PREF_H_I, 18,
	    BD_BG_C, 0x00000000u, BD_FG_C, 0xE8ECF0FFu,
	    BD_END);

	/* The floating terminal subwindow: an opaque panel laid out as a column
	 * of title bar / terminal / command line. */
	subwin = bd_create(root, BD_PANEL,
	    BD_LAYOUT_I, BD_LAYOUT_COL,
	    BD_X_I, 90, BD_Y_I, 70,
	    BD_PREF_W_I, SUBWIN_W, BD_PREF_H_I, SUBWIN_FULL_H,
	    BD_PAD_I, 0, BD_GAP_I, 0,
	    BD_END);

	titlebar = bd_create(subwin, BD_PANEL,
	    BD_LAYOUT_I, BD_LAYOUT_ROW,
	    BD_PREF_H_I, TITLE_H, BD_PAD_I, 3, BD_GAP_I, 4,
	    BD_BG_C, 0x2A3340FFu,
	    BD_END);
	bd_create(titlebar, BD_LABEL,
	    BD_LABEL_S, "Terminal", BD_GROW_I, 1,
	    BD_BG_C, 0x00000000u, BD_FG_C, 0xDCE3EAFFu,
	    BD_END);
	min_btn = bd_create(titlebar, BD_BUTTON,
	    BD_LABEL_S, "_", BD_PREF_W_I, 34,
	    BD_ON_CLICK_F, on_minimize,
	    BD_END);

	term = bd_terminal_create(subwin, BD_GROW_I, 1, BD_END);
	bd_terminal_write(term,
	    "birdie-gui + SDL3, 2D UI over a 3D scene.\r\n"
	    "This terminal is a draggable, minimizable subwindow.\r\n"
	    "Type below and press Enter.\r\n", -1);

	term_input = bd_create(subwin, BD_INPUT_LINE,
	    BD_PREF_H_I, 26, BD_ON_CLICK_F, on_submit,
	    BD_END);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

static int
in_rect(int px, int py, int x, int y, int w, int h)
{
	return px >= x && px < x + w && py >= y && py < y + h;
}

int
main(void)
{
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "sdl3: SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	/* Request an OpenGL ES 3.0 context: the toolkit's shaders (and the 3D
	 * scene) are "#version 300 es". */
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
	    SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

	S.win = SDL_CreateWindow("birdie-gui on SDL3", 1024, 720,
	    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
	if (!S.win) {
		fprintf(stderr, "sdl3: SDL_CreateWindow failed: %s\n",
		    SDL_GetError());
		SDL_Quit();
		return 1;
	}

	S.ctx = SDL_GL_CreateContext(S.win);
	if (!S.ctx) {
		fprintf(stderr, "sdl3: SDL_GL_CreateContext failed: %s\n",
		    SDL_GetError());
		SDL_DestroyWindow(S.win);
		SDL_Quit();
		return 1;
	}
	SDL_GL_MakeCurrent(S.win, S.ctx);
	SDL_GL_SetSwapInterval(1);   /* vsync */

	bd_gui_init(&backend, NULL);   /* NULL theme = defaults */
	build_ui();
	tet_init();

	/* Host-side drag state: the toolkit consumes events over its widgets; an
	 * unconsumed left-drag either moves the subwindow (started on the title
	 * bar) or rotates the tetrahedron (started over the 3D background). */
	enum { DRAG_NONE, DRAG_ROTATE, DRAG_MOVE } drag = DRAG_NONE;
	int drag_off_x = 0, drag_off_y = 0, last_x = 0, last_y = 0;
	float rot_x = 0.5f, rot_y = 0.7f, cam_dist = 4.0f;
	double last_time = be_time();

	int running = 1;
	while (running) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_EVENT_QUIT) {
				running = 0;
				continue;
			}

			bd_event bev;
			if (!translate(&ev, &bev))
				continue;

			if (bev.type == BD_EV_MOUSE_DOWN &&
			    bev.button == BD_MOUSE_LEFT) {
				if (bd_gui_event(&bev))
					continue;   /* a widget took it */
				int tx, ty, tw, th;
				bd_widget_rect(titlebar, &tx, &ty, &tw, &th);
				if (in_rect(bev.x, bev.y, tx, ty, tw, th)) {
					int sx, sy;
					bd_widget_rect(subwin, &sx, &sy, NULL, NULL);
					drag = DRAG_MOVE;
					drag_off_x = bev.x - sx;
					drag_off_y = bev.y - sy;
				} else {
					drag = DRAG_ROTATE;
					last_x = bev.x;
					last_y = bev.y;
				}
			} else if (bev.type == BD_EV_MOUSE_MOVE && drag == DRAG_ROTATE) {
				rot_y += (float)(bev.x - last_x) * 0.01f;
				rot_x += (float)(bev.y - last_y) * 0.01f;
				if (rot_x >  1.55f) rot_x =  1.55f;
				if (rot_x < -1.55f) rot_x = -1.55f;
				last_x = bev.x;
				last_y = bev.y;
			} else if (bev.type == BD_EV_MOUSE_MOVE && drag == DRAG_MOVE) {
				int nx = bev.x - drag_off_x;
				int ny = bev.y - drag_off_y;
				int ww = be_width(), wh = be_height();
				if (nx < 0) nx = 0;
				if (ny < 0) ny = 0;
				if (nx > ww - 40)     nx = ww - 40;
				if (ny > wh - TITLE_H) ny = wh - TITLE_H;
				bd_set(subwin, BD_X_I, nx, BD_Y_I, ny, BD_END);
			} else if (bev.type == BD_EV_MOUSE_UP &&
			    bev.button == BD_MOUSE_LEFT) {
				drag = DRAG_NONE;
				bd_gui_event(&bev);
			} else if (bev.type == BD_EV_MOUSE_SCROLL) {
				if (!bd_gui_event(&bev)) {   /* not over the terminal */
					cam_dist -= bev.scroll_dy * 0.4f;
					if (cam_dist < 2.5f)  cam_dist = 2.5f;
					if (cam_dist > 12.0f) cam_dist = 12.0f;
				}
			} else {
				bd_gui_event(&bev);
			}
		}

		/* gentle idle spin unless the user is rotating by hand */
		double now = be_time();
		float dt = (float)(now - last_time);
		last_time = now;
		if (drag != DRAG_ROTATE)
			rot_y += dt * 0.3f;

		int w = be_width(), h = be_height();

		/* The host owns the frame: clear, draw the 3D background, then let
		 * the toolkit composite the UI on top (its clear() is a no-op). */
		glDisable(GL_SCISSOR_TEST);
		glClearColor(0.05f, 0.06f, 0.08f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		tet_draw(rot_x, rot_y, cam_dist, w, h);

		bd_gui_layout(w, h);
		bd_gui_render();
		SDL_GL_SwapWindow(S.win);
	}

	bd_gui_cleanup();
	if (S.clip)
		SDL_free(S.clip);
	SDL_GL_DestroyContext(S.ctx);
	SDL_DestroyWindow(S.win);
	SDL_Quit();
	return 0;
}
