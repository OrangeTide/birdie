#include "bd_backend_ludica.h"
#include "ludica.h"
#include "ludica_gfx.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#if defined(__linux__)
#include <unistd.h>
#endif

/* stb_image lives in the ludica library (non-static), so we borrow its
 * from-memory decoder for embedded PNGs rather than owning a second copy. */
extern unsigned char *stbi_load_from_memory(const unsigned char *buffer,
    int len, int *x, int *y, int *comp, int req_comp);
extern void stbi_image_free(void *retval_from_stbi_load);

/*
 * ludica binding for the widget toolkit's bd_backend GPU interface. The draw
 * primitives map almost 1:1 onto ludica's shader/mesh/uniform/texture/scissor
 * API (lud_make_shader, lud_make_mesh, lud_uniform_*, lud_bind_texture,
 * lud_draw, lud_scissor). Shaders bind the bd_vertex attributes a_pos/a_uv/
 * a_col to locations 0/1/2.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

static int    be_width(void)  { return lud_width(); }
static int    be_height(void) { return lud_height(); }
static double be_time(void)   { return lud_time(); }

static void be_viewport(int x, int y, int w, int h) { lud_viewport(x, y, w, h); }

static void
be_clear(float r, float g, float b, float a)
{
	lud_clear(r, g, b, a);
}

/* ---- shaders ---- */

static bd_shader
be_make_shader(const char *vert, const char *frag)
{
	lud_shader_desc_t desc = {
		.vert_src = vert,
		.frag_src = frag,
		.attrs = { "a_pos", "a_uv", "a_col" },
		.num_attrs = 3,
	};
	lud_shader_t sh = lud_make_shader(&desc);
	return (bd_shader){sh.id};
}

static void be_destroy_shader(bd_shader sh) { lud_destroy_shader((lud_shader_t){sh.id}); }
static void be_use_shader(bd_shader sh)     { lud_apply_shader((lud_shader_t){sh.id}); }

static void be_uni_int  (bd_shader s, const char *n, int v)
{ lud_uniform_int((lud_shader_t){s.id}, n, v); }
static void be_uni_float(bd_shader s, const char *n, float v)
{ lud_uniform_float((lud_shader_t){s.id}, n, v); }
static void be_uni_vec2 (bd_shader s, const char *n, float x, float y)
{ lud_uniform_vec2((lud_shader_t){s.id}, n, x, y); }
static void be_uni_vec3 (bd_shader s, const char *n, float x, float y, float z)
{ lud_uniform_vec3((lud_shader_t){s.id}, n, x, y, z); }
static void be_uni_vec4 (bd_shader s, const char *n, float x, float y, float z, float w)
{ lud_uniform_vec4((lud_shader_t){s.id}, n, x, y, z, w); }
static void be_uni_mat4 (bd_shader s, const char *n, const float m[16])
{ lud_uniform_mat4((lud_shader_t){s.id}, n, m); }

/* ---- draw ----
 * One persistent DYNAMIC mesh, updated in place each batch (v26.06.1
 * lud_update_mesh grows it as needed) and drawn with lud_draw_range so only
 * the current `count` vertices render. lud_draw uses the applied shader. */
static lud_mesh_t batch_mesh;

static void
be_draw_verts(const bd_vertex *verts, int count)
{
	if (count <= 0)
		return;
	/* establish 2D UI render state for the draw: alpha blend, no depth or
	 * face culling (UI quads are drawn in either winding). Done here rather
	 * than in clear so the toolkit renders correctly even when clear is NULL
	 * (a compositing host owning the frame). */
	lud_blend(LUD_BLEND_ALPHA);
	lud_depth_test(0);
	lud_cull(LUD_CULL_NONE);
	if (batch_mesh.id == 0) {
		lud_mesh_desc_t desc = {
			.vertices = verts,
			.vertex_count = count,
			.vertex_stride = (int)sizeof(bd_vertex),
			.layout = {
				{ 2, (int)offsetof(bd_vertex, x) },
				{ 2, (int)offsetof(bd_vertex, u) },
				{ 4, (int)offsetof(bd_vertex, r) },
			},
			.num_attrs = 3,
			.usage = LUD_USAGE_DYNAMIC,
			.primitive = LUD_PRIM_TRIANGLES,
		};
		batch_mesh = lud_make_mesh(&desc);
	} else {
		lud_update_mesh(batch_mesh, 0, count, verts);
	}
	lud_draw_range(batch_mesh, 0, count);
}

/* ---- textures ---- */

static bd_texture
be_load_texture(const char *path)
{
	lud_texture_t t = lud_load_texture(path,
	    LUD_FILTER_NEAREST, LUD_FILTER_NEAREST);   /* pixel-art PNGs */
	return (bd_texture){t.id};
}

/* Decode a PNG from memory and upload it, matching be_load_texture's NEAREST
 * filtering. lud_load_texture is path-only, so embedded blobs decode here. */
static bd_texture
be_load_texture_mem(const unsigned char *data, int len)
{
	int w, h, comp;
	unsigned char *pixels = stbi_load_from_memory(data, len, &w, &h, &comp, 4);
	if (!pixels) {
		fprintf(stderr, "ludica: cannot decode embedded texture\n");
		return (bd_texture){0};
	}
	lud_texture_desc_t desc = {
		.width = w, .height = h,
		.format = LUD_PIXFMT_RGBA8,
		.min_filter = LUD_FILTER_NEAREST,   /* pixel-art PNGs */
		.mag_filter = LUD_FILTER_NEAREST,
		.data = pixels,
	};
	lud_texture_t t = lud_make_texture(&desc);
	stbi_image_free(pixels);
	return (bd_texture){t.id};
}

static bd_texture
be_make_texture(int w, int h, const void *rgba)
{
	lud_texture_desc_t desc = {
		.width = w, .height = h,
		.format = LUD_PIXFMT_RGBA8,
		.min_filter = LUD_FILTER_LINEAR,
		.mag_filter = LUD_FILTER_LINEAR,
		.data = rgba,
	};
	lud_texture_t t = lud_make_texture(&desc);
	return (bd_texture){t.id};
}

static void
be_update_texture(bd_texture t, int x, int y, int w, int h, const void *rgba)
{
	lud_update_texture((lud_texture_t){t.id}, x, y, w, h, rgba);
}

static void be_bind_texture(bd_texture t, int unit)
{ lud_bind_texture((lud_texture_t){t.id}, unit); }
static void be_destroy_texture(bd_texture t)
{ lud_destroy_texture((lud_texture_t){t.id}); }

/* ---- scissor ---- */

static void be_scissor(int x, int y, int w, int h) { lud_scissor(x, y, w, h); }
static void be_scissor_off(void) { lud_scissor_off(); }

/* ---- asset resolution ---- */

#if defined(__linux__)
/* Locate an asset for an installed Linux app: the executable's own directory,
 * a sibling ../share/birdie data dir (FHS install layout), then
 * $HOME/.local/share/birdie. Writes the first hit into the caller's `buf` and
 * returns it; NULL if none exist (the toolkit then uses the cwd-relative dev
 * path). ludica has no cross-platform base-path query, so this is done here. */
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
#endif /* __linux__ */

/* ------------------------------------------------------------------ */
/* clipboard (ludica)                                                 */
/* ------------------------------------------------------------------ */

/* Last string handed out by be_clipboard_get. ludica returns a fresh malloc'd
 * copy each call; the backend contract wants a pointer it owns until the next
 * clipboard call, so we hold this one and free it on the following get. */
static char *clip_owned;

static void
be_clipboard_set(const char *utf8)
{
	lud_clipboard_set_text(utf8 ? utf8 : "");
}

static const char *
be_clipboard_get(void)
{
	free(clip_owned);                       /* release the previous handout */
	clip_owned = lud_clipboard_get_text();  /* malloc'd, or NULL on empty */
	return (clip_owned && clip_owned[0]) ? clip_owned : NULL;
}

const bd_backend bd_backend_ludica = {
	.width            = be_width,
	.height           = be_height,
	.time             = be_time,
	.viewport         = be_viewport,
	.clear            = be_clear,
	.make_shader      = be_make_shader,
	.destroy_shader   = be_destroy_shader,
	.use_shader       = be_use_shader,
	.set_uniform_int   = be_uni_int,
	.set_uniform_float = be_uni_float,
	.set_uniform_vec2  = be_uni_vec2,
	.set_uniform_vec3  = be_uni_vec3,
	.set_uniform_vec4  = be_uni_vec4,
	.set_uniform_mat4  = be_uni_mat4,
	.draw_verts       = be_draw_verts,
	.load_texture     = be_load_texture,
	.load_texture_mem = be_load_texture_mem,
	.make_texture     = be_make_texture,
	.update_texture   = be_update_texture,
	.bind_texture     = be_bind_texture,
	.destroy_texture  = be_destroy_texture,
	.scissor          = be_scissor,
	.scissor_off      = be_scissor_off,
	.clipboard_set    = be_clipboard_set,
	.clipboard_get    = be_clipboard_get,
#if defined(__linux__)
	.resolve_asset    = be_resolve_asset,
#endif
};

/* ------------------------------------------------------------------ */
/* event translation                                                  */
/* ------------------------------------------------------------------ */

static int
map_mods(unsigned m)
{
	int r = 0;
	if (m & LUD_MOD_SHIFT) r |= BD_MOD_SHIFT;
	if (m & LUD_MOD_CTRL)  r |= BD_MOD_CTRL;
	if (m & LUD_MOD_ALT)   r |= BD_MOD_ALT;
	return r;
}

static int
map_button(enum lud_mouse_button b)
{
	switch (b) {
	case LUD_MOUSE_LEFT:   return BD_MOUSE_LEFT;
	case LUD_MOUSE_RIGHT:  return BD_MOUSE_RIGHT;
	case LUD_MOUSE_MIDDLE: return BD_MOUSE_MIDDLE;
	default:               return 0;
	}
}

static int
map_key(enum lud_keycode k)
{
	if (k >= LUD_KEY_A && k <= LUD_KEY_A + 25)
		return BD_KEY_A + (k - LUD_KEY_A);
	switch (k) {
	case LUD_KEY_LEFT:      return BD_KEY_LEFT;
	case LUD_KEY_RIGHT:     return BD_KEY_RIGHT;
	case LUD_KEY_UP:        return BD_KEY_UP;
	case LUD_KEY_DOWN:      return BD_KEY_DOWN;
	case LUD_KEY_HOME:      return BD_KEY_HOME;
	case LUD_KEY_END:       return BD_KEY_END;
	case LUD_KEY_BACKSPACE: return BD_KEY_BACKSPACE;
	case LUD_KEY_DELETE:    return BD_KEY_DELETE;
	case LUD_KEY_ENTER:     return BD_KEY_ENTER;
	case LUD_KEY_ESCAPE:    return BD_KEY_ESCAPE;
	case LUD_KEY_TAB:       return BD_KEY_TAB;
	default:                return BD_KEY_UNKNOWN;
	}
}

int
bd_event_from_lud(const lud_event_t *ev, bd_event *out)
{
	bd_event e = {0};
	e.mods = map_mods(ev->modifiers);

	switch (ev->type) {
	case LUD_EV_MOUSE_MOVE:
		e.type = BD_EV_MOUSE_MOVE;
		e.x = ev->mouse_move.x;
		e.y = ev->mouse_move.y;
		break;
	case LUD_EV_MOUSE_DOWN:
		e.type = BD_EV_MOUSE_DOWN;
		e.x = ev->mouse_button.x;
		e.y = ev->mouse_button.y;
		e.button = map_button(ev->mouse_button.button);
		break;
	case LUD_EV_MOUSE_UP:
		e.type = BD_EV_MOUSE_UP;
		e.x = ev->mouse_button.x;
		e.y = ev->mouse_button.y;
		e.button = map_button(ev->mouse_button.button);
		break;
	case LUD_EV_MOUSE_SCROLL:
		e.type = BD_EV_MOUSE_SCROLL;
		e.scroll_dy = ev->scroll.dy;
		break;
	case LUD_EV_KEY_DOWN:
		e.type = BD_EV_KEY_DOWN;
		e.key = map_key(ev->key.keycode);
		e.repeat = ev->key.repeat;
		break;
	case LUD_EV_KEY_UP:
		e.type = BD_EV_KEY_UP;
		e.key = map_key(ev->key.keycode);
		break;
	case LUD_EV_CHAR:
		e.type = BD_EV_CHAR;
		e.codepoint = ev->ch.codepoint;
		break;
	case LUD_EV_FOCUS:
		e.type = BD_EV_FOCUS_IN;
		break;
	case LUD_EV_UNFOCUS:
		e.type = BD_EV_FOCUS_OUT;
		break;
	default:
		return 0;
	}

	*out = e;
	return 1;
}
