#include "bd_backend_ludica.h"
#include "ludica.h"
#include "ludica_gfx.h"
#include "ludica_vfont.h"

/*
 * ludica binding for the widget toolkit's bd_backend interface. Every wrapper
 * is a thin pass-through; the only adaptation is bridging the {unsigned id;}
 * handle structs and choosing nearest-neighbor filtering for the GUI's
 * pixel-art chrome atlas and sprites.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

static int    be_width(void)  { return lud_width(); }
static int    be_height(void) { return lud_height(); }
static double be_time(void)   { return lud_time(); }

static void
be_viewport(int x, int y, int w, int h)
{
	lud_viewport(x, y, w, h);
}

static void
be_clear(float r, float g, float b, float a)
{
	lud_clear(r, g, b, a);
}

static void
be_sprite_begin(float x, float y, float w, float h)
{
	lud_sprite_begin(x, y, w, h);
}

static void
be_sprite_end(void)
{
	lud_sprite_end();
}

static void
be_fill_rect(float x, float y, float w, float h,
    float r, float g, float b, float a)
{
	lud_sprite_rect(x, y, w, h, r, g, b, a);
}

static void
be_stroke_rect(float x, float y, float w, float h,
    float r, float g, float b, float a)
{
	lud_sprite_rect_lines(x, y, w, h, r, g, b, a);
}

static void
be_draw_tinted(bd_texture tex, float dx, float dy, float dw, float dh,
    float sx, float sy, float sw, float sh,
    float r, float g, float b, float a)
{
	lud_sprite_draw_tinted((lud_texture_t){tex.id}, dx, dy, dw, dh,
	    sx, sy, sw, sh, r, g, b, a);
}

static void
be_vfont_begin(float vx, float vy, float vw, float vh)
{
	lud_vfont_begin(vx, vy, vw, vh);
}

static void
be_vfont_end(void)
{
	lud_vfont_end();
}

static void
be_vfont_draw(bd_font font, float x, float y, float size,
    float r, float g, float b, float a, const char *text)
{
	lud_vfont_draw((lud_vfont_t){font.id}, x, y, size, r, g, b, a, text);
}

static float
be_vfont_text_width(bd_font font, float size, const char *text)
{
	return lud_vfont_text_width((lud_vfont_t){font.id}, size, text);
}

static bd_texture
be_load_texture(const char *path)
{
	lud_texture_t t = lud_load_texture(path,
	    LUD_FILTER_NEAREST, LUD_FILTER_NEAREST);
	return (bd_texture){t.id};
}

static void
be_destroy_texture(bd_texture tex)
{
	lud_destroy_texture((lud_texture_t){tex.id});
}

static bd_font
be_load_font(const char *path)
{
	lud_vfont_t f = lud_load_vfont(path);
	return (bd_font){f.id};
}

static void
be_destroy_font(bd_font font)
{
	lud_destroy_vfont((lud_vfont_t){font.id});
}

const bd_backend bd_backend_ludica = {
	.width            = be_width,
	.height           = be_height,
	.time             = be_time,
	.viewport         = be_viewport,
	.clear            = be_clear,
	.sprite_begin     = be_sprite_begin,
	.sprite_end       = be_sprite_end,
	.fill_rect        = be_fill_rect,
	.stroke_rect      = be_stroke_rect,
	.draw_tinted      = be_draw_tinted,
	.vfont_begin      = be_vfont_begin,
	.vfont_end        = be_vfont_end,
	.vfont_draw       = be_vfont_draw,
	.vfont_text_width = be_vfont_text_width,
	.load_texture     = be_load_texture,
	.destroy_texture  = be_destroy_texture,
	.load_font        = be_load_font,
	.destroy_font     = be_destroy_font,
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
		break;
	case LUD_EV_CHAR:
		e.type = BD_EV_CHAR;
		e.codepoint = ev->ch.codepoint;
		break;
	default:
		return 0;
	}

	*out = e;
	return 1;
}
