/*
 * bd_backend_sdl3.c — SDL3 + OpenGL ES 3 binding for the bd_backend interface.
 *
 * Window and context management, the bd_backend vtable (shaders, a dynamic
 * quad batch, textures, scissor, clipboard, IME), and SDL-event translation.
 * A reference backend alongside bd_backend_ludica.c and the raw X11/EGL/GLES
 * gallery (src/guitest/); the toolkit and its renderer are untouched.
 *
 * The clear hook is left NULL: the host owns the frame clear so the UI can
 * composite over whatever it drew. draw_verts establishes the 2D UI render
 * state (alpha blend, no depth or cull) each batch, so the UI renders correctly
 * whether or not the host cleared or drew 3D first.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_backend_sdl3.h"

#include <GLES3/gl3.h>

#include <stddef.h>
#include <stdio.h>

/* stb_image decodes the toolkit's PNG assets (terminal atlas, pushpins). It is
 * bundled with birdie-gui; the backend owns the single implementation so a host
 * that links only this backend still resolves stbi_load. */
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

/* No clear hook: the host clears the framebuffer and draws its background
 * itself each frame, so the toolkit's optional clear is left NULL and the UI
 * composites on top. draw_verts establishes the 2D render state, so the UI
 * still renders correctly. */

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
	/* establish 2D UI render state for the draw: alpha blend, no depth or
	 * cull. A host that left depth testing on for a 3D scene relies on this
	 * (rather than on clear) to let the UI draw on top. */
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
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

const bd_backend bd_backend_sdl3 = {
	.width             = be_width,
	.height            = be_height,
	.time              = be_time,
	.viewport          = be_viewport,
	.clear             = NULL,       /* host owns the frame clear */
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

/* ------------------------------------------------------------------ */
/* window + context lifecycle                                         */
/* ------------------------------------------------------------------ */

SDL_Window *
bd_backend_sdl3_open(const char *title, int w, int h)
{
	if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "sdl3: SDL_Init failed: %s\n", SDL_GetError());
		return NULL;
	}

	/* Request an OpenGL ES 3.0 context: the toolkit's shaders are
	 * "#version 300 es". Depth is for hosts that draw a 3D background. */
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
	    SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

	S.win = SDL_CreateWindow(title ? title : "birdie-gui", w, h,
	    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
	if (!S.win) {
		fprintf(stderr, "sdl3: SDL_CreateWindow failed: %s\n",
		    SDL_GetError());
		return NULL;
	}

	S.ctx = SDL_GL_CreateContext(S.win);
	if (!S.ctx) {
		fprintf(stderr, "sdl3: SDL_GL_CreateContext failed: %s\n",
		    SDL_GetError());
		SDL_DestroyWindow(S.win);
		S.win = NULL;
		return NULL;
	}
	SDL_GL_MakeCurrent(S.win, S.ctx);
	SDL_GL_SetSwapInterval(1);   /* vsync */
	return S.win;
}

void
bd_backend_sdl3_close(void)
{
	if (S.clip) {
		SDL_free(S.clip);
		S.clip = NULL;
	}
	if (S.ctx) {
		SDL_GL_DestroyContext(S.ctx);
		S.ctx = NULL;
	}
	if (S.win) {
		SDL_DestroyWindow(S.win);
		S.win = NULL;
	}
	SDL_Quit();
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

int
bd_event_from_sdl(const SDL_Event *ev, bd_event *out)
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
