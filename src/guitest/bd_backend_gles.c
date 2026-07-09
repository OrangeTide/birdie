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
#include "bd_backend_gles_core.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* The GPU vtable rows (shaders, draw, textures, viewport, clear) come from
 * bd_backend_gles_core; this file supplies only the windowing (multiple native
 * windows via window.h), clipboard, and IME. */

/* ------------------------------------------------------------------ */
/* frame / window                                                     */
/* ------------------------------------------------------------------ */

static int cur_window;       /* window made current by window_begin */

/* width()/height() report the window made current by window_begin (the one
 * being rendered), falling back to the primary before any begin. */
static int    be_width(void)  { return win_window_width(cur_window ? cur_window : 1); }
static int    be_height(void) { return win_window_height(cur_window ? cur_window : 1); }

/* Scissor needs the framebuffer height for the top-left/bottom-left flip; the
 * core takes it explicitly, so wrap it with the current window's height. */
static void be_scissor(int x, int y, int w, int h)
{ bd_gles_scissor(x, y, w, h, be_height()); }

/* ------------------------------------------------------------------ */
/* multiple windows                                                   */
/* ------------------------------------------------------------------ */

static int  be_window_open(const char *title, int w, int h)
{ return win_window_open(title, w, h); }
static void be_window_close(int id) { win_window_close(id); }

static void
be_window_begin(int id)
{
	cur_window = id;
	win_window_begin(id);
}

static void be_window_swap(int id)              { win_window_swap(id); }
static int  be_window_width(int id)             { return win_window_width(id); }
static int  be_window_height(int id)            { return win_window_height(id); }
static void be_window_set_title(int id, const char *t) { win_window_set_title(id, t); }
static void be_window_minimize(int id)          { win_window_minimize(id); }
static void be_window_restore(int id)           { win_window_restore(id); }

static void        be_clipboard_set(const char *s) { win_clipboard_set(s); }
static const char *be_clipboard_get(void)          { return win_clipboard_get(); }

static void be_ime_set_enabled(int on) { win_ime_set_enabled(on); }
static void be_ime_set_cursor_rect(int x, int y, int w, int h)
{ win_ime_set_cursor_rect(x, y, w, h); }

/* Locate an asset for an installed Linux app. Searches, in order: the
 * executable's own directory, a sibling ../share/birdie data dir (the usual
 * FHS install layout), and $HOME/.local/share/birdie. On the first hit writes
 * the absolute path into the caller's `buf` and returns it; NULL if none exist
 * (the toolkit then uses the plain relative name, read from the cwd). */
static const char *
be_resolve_asset(const char *rel, char *buf, size_t bufsz)
{
	char exe[4096];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (n > 0) {
		exe[n] = '\0';
		char *slash = strrchr(exe, '/');
		if (slash) {
			int dir = (int)(slash - exe);
			snprintf(buf, bufsz, "%.*s/%s", dir, exe, rel);
			if (access(buf, R_OK) == 0)
				return buf;
			snprintf(buf, bufsz, "%.*s/../share/birdie/%s",
			    dir, exe, rel);
			if (access(buf, R_OK) == 0)
				return buf;
		}
	}
	const char *home = getenv("HOME");
	if (home) {
		snprintf(buf, bufsz, "%s/.local/share/birdie/%s", home, rel);
		if (access(buf, R_OK) == 0)
			return buf;
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* vtable                                                             */
/* ------------------------------------------------------------------ */

const bd_backend bd_backend_gles = {
	.width             = be_width,
	.height            = be_height,
	.viewport          = bd_gles_viewport,
	.clear             = bd_gles_clear,
	.make_shader       = bd_gles_make_shader,
	.destroy_shader    = bd_gles_destroy_shader,
	.use_shader        = bd_gles_use_shader,
	.set_uniform_int   = bd_gles_uniform_int,
	.set_uniform_float = bd_gles_uniform_float,
	.set_uniform_vec2  = bd_gles_uniform_vec2,
	.set_uniform_vec3  = bd_gles_uniform_vec3,
	.set_uniform_vec4  = bd_gles_uniform_vec4,
	.set_uniform_mat4  = bd_gles_uniform_mat4,
	.draw_verts        = bd_gles_draw_verts,
	.load_texture      = bd_gles_load_texture,
	.load_texture_mem  = bd_gles_load_texture_mem,
	.make_texture      = bd_gles_make_texture,
	.update_texture    = bd_gles_update_texture,
	.bind_texture      = bd_gles_bind_texture,
	.destroy_texture   = bd_gles_destroy_texture,
	.scissor           = be_scissor,
	.scissor_off       = bd_gles_scissor_off,
	.multi_window      = 1,
	.window_open       = be_window_open,
	.window_close      = be_window_close,
	.window_begin      = be_window_begin,
	.window_swap       = be_window_swap,
	.window_width      = be_window_width,
	.window_height     = be_window_height,
	.window_set_title  = be_window_set_title,
	.window_minimize   = be_window_minimize,
	.window_restore    = be_window_restore,
	.clipboard_set     = be_clipboard_set,
	.clipboard_get     = be_clipboard_get,
	.ime_set_enabled     = be_ime_set_enabled,
	.ime_set_cursor_rect = be_ime_set_cursor_rect,
	.resolve_asset       = be_resolve_asset,
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
		e.repeat = ev->repeat;
		break;
	case WIN_EV_KEY_UP:
		e.type = BD_EV_KEY_UP;
		e.key = ev->key;
		break;
	case WIN_EV_CHAR:
		e.type = BD_EV_CHAR;
		e.codepoint = ev->codepoint;
		e.repeat = ev->repeat;
		break;
	case WIN_EV_TEXT_COMMIT:
		e.type = BD_EV_TEXT_COMMIT;
		e.text = ev->text;
		break;
	case WIN_EV_TOUCH_DOWN:
	case WIN_EV_TOUCH_MOVE:
	case WIN_EV_TOUCH_UP:
		e.type = ev->type == WIN_EV_TOUCH_DOWN ? BD_EV_TOUCH_DOWN
		    : ev->type == WIN_EV_TOUCH_MOVE ? BD_EV_TOUCH_MOVE
		    : BD_EV_TOUCH_UP;
		e.x = ev->x;
		e.y = ev->y;
		e.touch = ev->touch;
		break;
	case WIN_EV_PEN_HOVER:
	case WIN_EV_PEN_DOWN:
	case WIN_EV_PEN_MOVE:
	case WIN_EV_PEN_UP:
		e.type = ev->type == WIN_EV_PEN_HOVER ? BD_EV_PEN_HOVER
		    : ev->type == WIN_EV_PEN_DOWN ? BD_EV_PEN_DOWN
		    : ev->type == WIN_EV_PEN_MOVE ? BD_EV_PEN_MOVE
		    : BD_EV_PEN_UP;
		e.x = ev->x;
		e.y = ev->y;
		e.pressure = ev->pressure;
		e.tilt_x = ev->tilt_x;
		e.tilt_y = ev->tilt_y;
		e.pen_flags = ev->pen_flags;
		break;
	case WIN_EV_FOCUS_IN:
		e.type = BD_EV_FOCUS_IN;
		break;
	case WIN_EV_FOCUS_OUT:
		e.type = BD_EV_FOCUS_OUT;
		break;
	default:
		return 0;       /* close, resize: no bd_event */
	}

	*out = e;
	return 1;
}
