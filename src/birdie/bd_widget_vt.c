#include "bd_widget_vt.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include "vt_state.h"
#include "vt_parse.h"
#include "vt_ops.h"
#include "vt_buf.h"
#include "vt_cell.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

/*
 * VT terminal widget, implemented as a toolkit extension. All terminal state,
 * the CP437 bitmap-font atlas, and the 256-color/truecolor resolver live here,
 * not in core widget.c.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#define GLYPH_W   8
#define GLYPH_H   16
#define FONT_COLS 32
#define ATLAS_W   (FONT_COLS * GLYPH_W)   /* CP437 atlas: 32*8  = 256 px wide */
#define ATLAS_H   128                     /* 256 glyphs / 32 cols = 8 rows * 16 */

#define TERM_SCROLLBACK 2000

#ifndef BD_ASSET_TERM_FONT
#define BD_ASSET_TERM_FONT "src/birdie/assets/font8x16.png"
#endif

struct vt_widget {
	struct vt_state  *vt;
	struct vt_parse  *vt_parser;
	int               cols, rows;
	int               scroll_back;
	bd_palette        pal;
};

static int        vt_type;      /* registered type id (0 = not yet registered) */
static bd_texture vt_font;      /* shared CP437 atlas */
static int        vt_live;      /* live terminal count, governs font lifetime */

/* ------------------------------------------------------------------ */
/* drawing helpers (via the backend)                                  */
/* ------------------------------------------------------------------ */

static void
fill(int x, int y, int w, int h, uint32_t color)
{
	if (color & 0xFF)
		bd_draw_rect((float)x, (float)y, (float)w, (float)h, color);
}

static void
glyph(int ch, float dx, float dy, uint32_t fg, uint32_t bg)
{
	int sx = (ch % FONT_COLS) * GLYPH_W;
	int sy = (ch / FONT_COLS) * GLYPH_H;

	if (bg & 0xFF)
		bd_draw_rect(dx, dy, GLYPH_W, GLYPH_H, bg);
	bd_draw_sprite(vt_font, dx, dy, GLYPH_W, GLYPH_H,
	    sx / (float)ATLAS_W, sy / (float)ATLAS_H,
	    (sx + GLYPH_W) / (float)ATLAS_W, (sy + GLYPH_H) / (float)ATLAS_H,
	    fg);
}

/* Resolve a vt_color (default / 256-indexed / truecolor) to RGBA8. */
static uint32_t
color_rgba(const struct vt_color *c, uint32_t def, const bd_palette *pal)
{
	switch (c->type) {
	case VT_COLOR_DEFAULT:
		return def;
	case VT_COLOR_INDEXED: {
		unsigned idx = c->index;
		if (idx < 16)
			return pal->ansi[idx];
		if (idx < 232) {
			idx -= 16;
			int b = (idx % 6) * 51;
			idx /= 6;
			int g = (idx % 6) * 51;
			int r = (idx / 6) * 51;
			return ((uint32_t)r << 24) | ((uint32_t)g << 16) |
			    ((uint32_t)b << 8) | 0xFFu;
		}
		int v = 8 + (idx - 232) * 10;
		return ((uint32_t)v << 24) | ((uint32_t)v << 16) |
		    ((uint32_t)v << 8) | 0xFFu;
	}
	case VT_COLOR_RGB:
		return ((uint32_t)c->rgb.r << 24) | ((uint32_t)c->rgb.g << 16) |
		    ((uint32_t)c->rgb.b << 8) | 0xFFu;
	}
	return def;
}

/* ------------------------------------------------------------------ */
/* widget-class hooks                                                 */
/* ------------------------------------------------------------------ */

static void
vt_init(bd_id id, void *state)
{
	struct vt_widget *t = state;

	if (vt_live++ == 0) {
		vt_font = bd_backend_get()->load_texture(BD_ASSET_TERM_FONT);
		if (vt_font.id == 0)
			fprintf(stderr, "bd: failed to load terminal font atlas\n");
	}

	t->cols = 80;
	t->rows = 24;
	t->vt = vt_state_new(t->rows, t->cols, TERM_SCROLLBACK);
	t->vt_parser = vt_parse_new(vt_ops_default(), t->vt);
	t->scroll_back = 0;
	t->pal = bd_palette_default();

	/* class visual defaults; caller attributes (applied after init) win */
	bd_set(id, BD_BG_C, t->pal.default_bg, BD_FG_C, t->pal.default_fg,
	    BD_PAD_I, 2, BD_END);
}

static void
vt_destroy(bd_id id, void *state)
{
	struct vt_widget *t = state;
	(void)id;

	if (t->vt_parser)
		vt_parse_free(t->vt_parser);
	if (t->vt)
		vt_state_free(t->vt);

	if (--vt_live == 0 && vt_font.id != 0) {
		bd_backend_get()->destroy_texture(vt_font);
		vt_font.id = 0;
	}
}

static void
vt_render(bd_id id, void *state)
{
	struct vt_widget *t = state;
	if (!t->vt)
		return;

	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	int pad = bd_get_i(id, BD_PAD_I);
	int ox = x + pad;
	int oy = y + pad;

	struct vt_buf *buf = t->vt->buf;
	int cols = vt_buf_cols(buf);
	int rows = vt_buf_rows(buf);

	fill(x, y, w, h, t->pal.default_bg);

	int sb_count = vt_buf_scrollback_lines(buf);
	int off = t->scroll_back;
	if (off > sb_count)
		off = sb_count;

	for (int r = 0; r < rows; r++) {
		int vline = sb_count - off + r;
		struct vt_row *row;
		if (vline < sb_count)
			row = vt_buf_scrollback_row(buf, -(sb_count - vline));
		else
			row = vt_buf_row(buf, vline - sb_count);
		if (!row)
			continue;
		float dy = (float)(oy + r * GLYPH_H);
		for (int c = 0; c < cols; c++) {
			struct vt_cell *cell = &row->cells[c];
			if (cell->width == 0)
				continue;
			uint32_t fg = color_rgba(&cell->fg, t->pal.default_fg,
			    &t->pal);
			uint32_t bg = color_rgba(&cell->bg, t->pal.default_bg,
			    &t->pal);
			if (cell->attrs & VT_ATTR_BOLD &&
			    fg == t->pal.default_fg)
				fg = t->pal.bold_fg;
			if (cell->attrs & VT_ATTR_REVERSE) {
				uint32_t tmp = fg; fg = bg; bg = tmp;
			}
			int ch = cell->codepoint < 256
			    ? (int)cell->codepoint : '?';
			if (ch == 0)
				ch = ' ';
			float dx = (float)(ox + c * GLYPH_W);
			glyph(ch, dx, dy, fg, bg);
		}
	}

	if (off == 0 && (t->vt->modes & VT_MODE_CURSOR_VIS)) {
		int cr = t->vt->cursor_row;
		int cc = t->vt->cursor_col;
		if (cr >= 0 && cr < rows && cc >= 0 && cc < cols) {
			int cx = ox + cc * GLYPH_W;
			int cy = oy + cr * GLYPH_H;
			fill(cx, cy, GLYPH_W, GLYPH_H, t->pal.default_fg);
		}
	}
}

static void
vt_layout(bd_id id, void *state, int w, int h)
{
	struct vt_widget *t = state;
	if (!t->vt)
		return;
	int pad = bd_get_i(id, BD_PAD_I);
	int nc = (w - 2 * pad) / GLYPH_W;
	int nr = (h - 2 * pad) / GLYPH_H;
	if (nc < 1) nc = 1;
	if (nr < 1) nr = 1;
	if (nc != t->cols || nr != t->rows) {
		t->cols = nc;
		t->rows = nr;
		vt_state_resize(t->vt, nr, nc);
	}
}

static int
vt_event(bd_id id, void *state, const bd_event *ev)
{
	struct vt_widget *t = state;
	(void)id;
	if (ev->type != BD_EV_MOUSE_SCROLL || !t->vt)
		return 0;
	int lines = (int)(-ev->scroll_dy * 3.0f);
	int sb_max = vt_buf_scrollback_lines(t->vt->buf);
	t->scroll_back += lines;
	if (t->scroll_back < 0)
		t->scroll_back = 0;
	if (t->scroll_back > sb_max)
		t->scroll_back = sb_max;
	return 1;
}

static const bd_widget_class vt_class = {
	.name              = "terminal",
	.state_size        = sizeof(struct vt_widget),
	.init              = vt_init,
	.destroy           = vt_destroy,
	.render            = vt_render,
	.layout            = vt_layout,
	.event             = vt_event,
	.contains_children = 0,
};

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

void
bd_terminal_register(void)
{
	if (vt_type == 0)
		vt_type = bd_register_widget_class(&vt_class);
}

bd_id
bd_terminal_create(bd_id parent, ...)
{
	bd_terminal_register();

	va_list ap;
	va_start(ap, parent);
	bd_id id = bd_create_va(parent, vt_type, ap);
	va_end(ap);
	return id;
}

void
bd_terminal_write(bd_id id, const char *data, int len)
{
	if (!data || bd_widget_type(id) != vt_type)
		return;
	struct vt_widget *t = bd_widget_state(id);
	if (!t || !t->vt_parser)
		return;
	if (len < 0)
		len = (int)strlen(data);
	vt_parse_feed(t->vt_parser, data, (size_t)len);
	if (t->scroll_back > 0)
		t->scroll_back = 0;
}

void
bd_terminal_set_palette(bd_id id, const bd_palette *pal)
{
	if (!pal || bd_widget_type(id) != vt_type)
		return;
	struct vt_widget *t = bd_widget_state(id);
	if (!t)
		return;
	t->pal = *pal;
	/* keep the widget's bg/fg attributes in sync with the new defaults */
	bd_set(id, BD_BG_C, t->pal.default_bg, BD_FG_C, t->pal.default_fg,
	    BD_END);
}
