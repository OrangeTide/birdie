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
#include "bd_backend_gles_core.h"

#include <stdio.h>

/* ------------------------------------------------------------------ */
/* SDL window + GL context                                            */
/* ------------------------------------------------------------------ */

static struct {
	SDL_Window   *win;
	SDL_GLContext ctx;
	char         *clip;      /* last clipboard string handed out (SDL-owned) */
} S;

/* The GPU vtable rows (shaders, draw, textures, viewport) come from
 * bd_backend_gles_core; only windowing, clipboard, and IME are SDL-specific.
 * No clear hook: the host owns the frame clear so the UI composites on top. */

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

/* Scissor needs the framebuffer height for the top-left/bottom-left flip; the
 * core takes it explicitly (a windowing concern), so wrap it with our height. */
static void be_scissor(int x, int y, int w, int h)
{ bd_gles_scissor(x, y, w, h, be_height()); }

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
	.viewport          = bd_gles_viewport,
	.clear             = NULL,       /* host owns the frame clear */
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
	case SDL_EVENT_WINDOW_FOCUS_GAINED:
		e.type = BD_EV_FOCUS_IN;
		break;
	case SDL_EVENT_WINDOW_FOCUS_LOST:
		e.type = BD_EV_FOCUS_OUT;
		break;
	default:
		return 0;
	}

	*out = e;
	return 1;
}
