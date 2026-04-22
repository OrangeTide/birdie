#include "widget.h"
#include "ludica.h"
#include "ludica_gfx.h"
#include "ludica_vfont.h"
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* constants                                                          */
/* ------------------------------------------------------------------ */

#define MAX_WIDGETS 256
#define TYPEMASK    0xF
#define TYPE_I      1
#define TYPE_S      2
#define TYPE_P      3
#define TYPE_F      4
#define TYPE_C      5
#define TYPE_B      6

#define FONT_COLS   32
#define GLYPH_W     8
#define GLYPH_H     16

#define CHROME_FONT_SZ  14.0f
#define CHROME_BASELINE 0.75f

/* flat dark theme — RGBA8 */
#define TH_BG       0x2B2B2BFFu
#define TH_PANEL    0x313335FFu
#define TH_WIDGET   0x3C3F41FFu
#define TH_HOVER    0x4C5052FFu
#define TH_PRESS    0x27292AFFu
#define TH_TEXT     0xBBBBBBFFu
#define TH_TEXT_HI  0xFFFFFFFFu
#define TH_BORDER   0x555555FFu
#define TH_FOCUS    0x5599DDFFu
#define TH_SELECT   0x264F78FFu

/* ------------------------------------------------------------------ */
/* internal widget structure                                          */
/* ------------------------------------------------------------------ */

struct widget {
	int alive;
	int type;
	bd_id parent;
	bd_id first_child, last_child;
	bd_id next_sib, prev_sib;

	int pref_w, pref_h, grow;
	int layout, pad, gap;
	int user_x, user_y;

	int x, y, w, h;

	const char *label;
	uint32_t fg, bg;
	int visible, enabled;
	int role;
	const char *acc_name;

	bd_callback_fn on_click;
	void *on_click_data;
	bd_callback_fn on_close;
	void *on_close_data;

	int hover, pressed;

	int menu_open, menu_pinned;
	int popup_x, popup_y, popup_w, popup_h;

	char text_buf[1024];
	int text_len;
	int cursor;
	int sel_anchor;
	float scroll_x;
};

/* ------------------------------------------------------------------ */
/* module state                                                       */
/* ------------------------------------------------------------------ */

static struct widget pool[MAX_WIDGETS];
static int pool_next = 1;
static bd_id root = BD_NONE;
static bd_id active_press = BD_NONE;
static bd_id active_menu = BD_NONE;
static bd_id drag_menu = BD_NONE;
static int drag_off_x, drag_off_y;
static int mouse_x, mouse_y;
static lud_texture_t font_tex;
static lud_vfont_t gui_font;
static lud_texture_t pin_out_tex;
static lud_texture_t pin_in_tex;
static bd_id focus_id = BD_NONE;
static float cursor_blink;

/* ------------------------------------------------------------------ */
/* color / drawing helpers                                            */
/* ------------------------------------------------------------------ */

static inline void
rgba(uint32_t c, float *r, float *g, float *b, float *a)
{
	*r = ((c >> 24) & 0xFF) / 255.0f;
	*g = ((c >> 16) & 0xFF) / 255.0f;
	*b = ((c >>  8) & 0xFF) / 255.0f;
	*a = ( c        & 0xFF) / 255.0f;
}

static void
draw_chr(int ch, float dx, float dy, uint32_t fg, uint32_t bg)
{
	float r, g, b, a;
	int sx = (ch % FONT_COLS) * GLYPH_W;
	int sy = (ch / FONT_COLS) * GLYPH_H;

	if (bg & 0xFF) {
		rgba(bg, &r, &g, &b, &a);
		lud_sprite_rect(dx, dy, GLYPH_W, GLYPH_H, r, g, b, a);
	}
	rgba(fg, &r, &g, &b, &a);
	lud_sprite_draw_tinted(font_tex, dx, dy, GLYPH_W, GLYPH_H,
	    (float)sx, (float)sy, GLYPH_W, GLYPH_H, r, g, b, a);
}

static void
draw_str(const char *s, float x, float y, uint32_t fg, uint32_t bg)
{
	for (; *s; s++, x += GLYPH_W)
		draw_chr((unsigned char)*s, x, y, fg, bg);
}

static int
str_px(const char *s)
{
	int n = 0;
	if (s)
		while (*s++) n++;
	return n * GLYPH_W;
}

static void
fill_rect(int x, int y, int w, int h, uint32_t color)
{
	float r, g, b, a;
	rgba(color, &r, &g, &b, &a);
	if (a > 0.0f)
		lud_sprite_rect((float)x, (float)y, (float)w, (float)h,
		    r, g, b, a);
}

static void
stroke_rect(int x, int y, int w, int h, uint32_t color)
{
	float r, g, b, a;
	rgba(color, &r, &g, &b, &a);
	lud_sprite_rect_lines((float)x, (float)y, (float)w, (float)h,
	    r, g, b, a);
}

static inline int
in_rect(int px, int py, int rx, int ry, int rw, int rh)
{
	return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

/* ------------------------------------------------------------------ */
/* proportional chrome text (vfont)                                   */
/* ------------------------------------------------------------------ */

static float
chrome_text_w(const char *s)
{
	return s ? lud_vfont_text_width(gui_font, CHROME_FONT_SZ, s) : 0.0f;
}

static float
chrome_baseline_y(int wy, int wh)
{
	return (float)wy + (float)wh * CHROME_BASELINE;
}

#define MAX_TEXT_DRAWS 256
struct text_draw {
	float x, y;
	uint32_t color;
	const char *text;
};
static struct text_draw text_buf[MAX_TEXT_DRAWS];
static int text_count;

static float
queue_text(const char *text, float x, float y, uint32_t color)
{
	if (!text)
		return 0.0f;
	if (text_count < MAX_TEXT_DRAWS)
		text_buf[text_count++] = (struct text_draw){x, y, color, text};
	return chrome_text_w(text);
}

static float
queue_sprite(lud_texture_t tex, float x, float y,
    float w, float h, float sw, float sh, uint32_t color)
{
	float r, g, b, a;
	rgba(color, &r, &g, &b, &a);
	lud_sprite_draw_tinted(tex, x, y, w, h,
	    0.0f, 0.0f, sw, sh, r, g, b, a);
	return w;
}

static void
flush_text(int vw, int vh)
{
	if (text_count == 0)
		return;
	lud_vfont_begin(0, 0, (float)vw, (float)vh);
	for (int i = 0; i < text_count; i++) {
		struct text_draw *td = &text_buf[i];
		float r, g, b, a;
		rgba(td->color, &r, &g, &b, &a);
		lud_vfont_draw(gui_font, td->x, td->y, CHROME_FONT_SZ,
		    r, g, b, a, td->text);
	}
	lud_vfont_end();
	text_count = 0;
}

/* ------------------------------------------------------------------ */
/* UTF-8 helpers                                                      */
/* ------------------------------------------------------------------ */

static int
utf8_encode(unsigned cp, char *out)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

static int
utf8_prev(const char *s, int pos)
{
	if (pos <= 0)
		return 0;
	pos--;
	while (pos > 0 && (s[pos] & 0xC0) == 0x80)
		pos--;
	return pos;
}

static int
utf8_next(const char *s, int pos, int len)
{
	if (pos >= len)
		return len;
	pos++;
	while (pos < len && (s[pos] & 0xC0) == 0x80)
		pos++;
	return pos;
}

static int
word_left(const char *s, int pos)
{
	while (pos > 0 && s[pos - 1] == ' ')
		pos--;
	while (pos > 0 && s[pos - 1] != ' ')
		pos--;
	return pos;
}

static int
word_right(const char *s, int pos, int len)
{
	while (pos < len && s[pos] != ' ')
		pos++;
	while (pos < len && s[pos] == ' ')
		pos++;
	return pos;
}

static float
input_text_px(const char *buf, int end)
{
	if (end <= 0)
		return 0.0f;
	char tmp[1024];
	if (end >= (int)sizeof(tmp))
		end = (int)sizeof(tmp) - 1;
	memcpy(tmp, buf, end);
	tmp[end] = '\0';
	return chrome_text_w(tmp);
}

/* ------------------------------------------------------------------ */
/* attribute application                                              */
/* ------------------------------------------------------------------ */

static void
apply_i(struct widget *w, int attr, int val)
{
	switch (attr) {
	case BD_WIDTH_I:  /* fall through */
	case BD_PREF_W_I: w->pref_w = val; break;
	case BD_HEIGHT_I: /* fall through */
	case BD_PREF_H_I: w->pref_h = val; break;
	case BD_GROW_I:   w->grow = val; break;
	case BD_X_I:      w->user_x = val; break;
	case BD_Y_I:      w->user_y = val; break;
	case BD_LAYOUT_I: w->layout = val; break;
	case BD_ROLE_I:   w->role = val; break;
	case BD_PAD_I:    w->pad = val; break;
	case BD_GAP_I:    w->gap = val; break;
	}
}

static void
apply_s(struct widget *w, int attr, const char *val)
{
	switch (attr) {
	case BD_LABEL_S:
		w->label = val;
		if (w->type == BD_INPUT_LINE && val) {
			int n = (int)strlen(val);
			if (n >= (int)sizeof(w->text_buf))
				n = (int)sizeof(w->text_buf) - 1;
			memcpy(w->text_buf, val, n);
			w->text_buf[n] = '\0';
			w->text_len = n;
			w->cursor = n;
			w->sel_anchor = -1;
		}
		break;
	case BD_NAME_S:  w->acc_name = val; break;
	}
}

static void
apply_p(struct widget *w, int attr, void *val)
{
	switch (attr) {
	case BD_ON_CLICK_P: w->on_click_data = val; break;
	case BD_ON_CLOSE_P: w->on_close_data = val; break;
	}
}

static void
apply_f(struct widget *w, int attr, bd_callback_fn val)
{
	switch (attr) {
	case BD_ON_CLICK_F: w->on_click = val; break;
	case BD_ON_CLOSE_F: w->on_close = val; break;
	}
}

static void
apply_c(struct widget *w, int attr, uint32_t val)
{
	switch (attr) {
	case BD_FG_C: w->fg = val; break;
	case BD_BG_C: w->bg = val; break;
	}
}

static void
apply_b(struct widget *w, int attr, int val)
{
	switch (attr) {
	case BD_VISIBLE_B:  w->visible = val; break;
	case BD_ENABLED_B:  w->enabled = val; break;
	case BD_MENU_PIN_B: w->menu_pinned = val; break;
	}
}

/* ------------------------------------------------------------------ */
/* varargs / array attribute parsing                                  */
/* ------------------------------------------------------------------ */

static void
parse_va(struct widget *w, va_list ap)
{
	for (;;) {
		int attr = va_arg(ap, int);
		if (attr == BD_END)
			break;
		switch (attr & TYPEMASK) {
		case TYPE_I: apply_i(w, attr, va_arg(ap, int)); break;
		case TYPE_S: apply_s(w, attr, va_arg(ap, const char *)); break;
		case TYPE_P: apply_p(w, attr, va_arg(ap, void *)); break;
		case TYPE_F: apply_f(w, attr, va_arg(ap, bd_callback_fn)); break;
		case TYPE_C: apply_c(w, attr, va_arg(ap, unsigned int)); break;
		case TYPE_B: apply_b(w, attr, va_arg(ap, int)); break;
		default:
			fprintf(stderr, "bd: unknown attr type %d\n",
			    attr & TYPEMASK);
			return;
		}
	}
}

static void
parse_arr(struct widget *w, const bd_attr *a)
{
	for (; a->id != BD_END; a++) {
		switch (a->id & TYPEMASK) {
		case TYPE_I: apply_i(w, a->id, a->i); break;
		case TYPE_S: apply_s(w, a->id, a->s); break;
		case TYPE_P: apply_p(w, a->id, a->p); break;
		case TYPE_F: apply_f(w, a->id, a->f); break;
		case TYPE_C: apply_c(w, a->id, a->c); break;
		case TYPE_B: apply_b(w, a->id, a->i); break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* tree operations                                                    */
/* ------------------------------------------------------------------ */

static void
link_child(bd_id pid, bd_id cid)
{
	struct widget *p = &pool[pid];
	struct widget *c = &pool[cid];

	c->parent = pid;
	c->prev_sib = p->last_child;
	c->next_sib = BD_NONE;
	if (p->last_child != BD_NONE)
		pool[p->last_child].next_sib = cid;
	else
		p->first_child = cid;
	p->last_child = cid;
}

static void
defaults(struct widget *w, int type)
{
	w->type = type;
	w->visible = 1;
	w->enabled = 1;
	w->fg = TH_TEXT;

	switch (type) {
	case BD_FRAME:
		w->bg = TH_BG;
		w->layout = BD_LAYOUT_COL;
		break;
	case BD_PANEL:
		w->layout = BD_LAYOUT_COL;
		break;
	case BD_BUTTON:
		w->bg = TH_WIDGET;
		w->fg = TH_TEXT_HI;
		w->pref_h = (int)CHROME_FONT_SZ + 8;
		break;
	case BD_LABEL:
		w->pref_h = (int)CHROME_FONT_SZ + 4;
		break;
	case BD_MENU:
		w->pref_h = (int)CHROME_FONT_SZ + 4;
		break;
	case BD_INPUT_LINE:
		w->bg = TH_PRESS;
		w->fg = TH_TEXT_HI;
		w->pref_h = (int)CHROME_FONT_SZ + 8;
		w->pad = 4;
		w->sel_anchor = -1;
		break;
	}
}

/* ------------------------------------------------------------------ */
/* public: create / destroy                                           */
/* ------------------------------------------------------------------ */

bd_id
bd_create(bd_id parent, int type, ...)
{
	if (pool_next >= MAX_WIDGETS)
		return BD_NONE;

	bd_id id = (bd_id)pool_next++;
	struct widget *w = &pool[id];

	memset(w, 0, sizeof *w);
	w->alive = 1;
	defaults(w, type);

	if (parent != BD_NONE)
		link_child(parent, id);
	else if (root == BD_NONE)
		root = id;

	va_list ap;
	va_start(ap, type);
	parse_va(w, ap);
	va_end(ap);

	if (type == BD_MENU && w->pref_w == 0 && w->label)
		w->pref_w = (int)(chrome_text_w(w->label) + CHROME_FONT_SZ);

	return id;
}

bd_id
bd_create_v(bd_id parent, int type, const bd_attr *attrs)
{
	if (pool_next >= MAX_WIDGETS)
		return BD_NONE;

	bd_id id = (bd_id)pool_next++;
	struct widget *w = &pool[id];

	memset(w, 0, sizeof *w);
	w->alive = 1;
	defaults(w, type);

	if (parent != BD_NONE)
		link_child(parent, id);
	else if (root == BD_NONE)
		root = id;

	if (attrs)
		parse_arr(w, attrs);

	if (type == BD_MENU && w->pref_w == 0 && w->label)
		w->pref_w = (int)(chrome_text_w(w->label) + CHROME_FONT_SZ);

	return id;
}

void
bd_destroy(bd_id id)
{
	if (id == BD_NONE || !pool[id].alive)
		return;

	while (pool[id].first_child != BD_NONE)
		bd_destroy(pool[id].first_child);

	struct widget *w = &pool[id];
	if (w->parent != BD_NONE) {
		struct widget *p = &pool[w->parent];
		if (w->prev_sib != BD_NONE)
			pool[w->prev_sib].next_sib = w->next_sib;
		else
			p->first_child = w->next_sib;
		if (w->next_sib != BD_NONE)
			pool[w->next_sib].prev_sib = w->prev_sib;
		else
			p->last_child = w->prev_sib;
	}
	if (root == id)
		root = BD_NONE;
	w->alive = 0;
}

/* ------------------------------------------------------------------ */
/* public: set / get                                                  */
/* ------------------------------------------------------------------ */

void
bd_set(bd_id id, ...)
{
	if (id == BD_NONE || !pool[id].alive)
		return;
	va_list ap;
	va_start(ap, id);
	parse_va(&pool[id], ap);
	va_end(ap);
}

void
bd_set_v(bd_id id, const bd_attr *attrs)
{
	if (id == BD_NONE || !pool[id].alive)
		return;
	parse_arr(&pool[id], attrs);
}

int
bd_get_i(bd_id id, int attr)
{
	if (id == BD_NONE || !pool[id].alive)
		return 0;
	struct widget *w = &pool[id];
	switch (attr) {
	case BD_WIDTH_I:  case BD_PREF_W_I: return w->pref_w;
	case BD_HEIGHT_I: case BD_PREF_H_I: return w->pref_h;
	case BD_GROW_I:   return w->grow;
	case BD_X_I:      return w->user_x;
	case BD_Y_I:      return w->user_y;
	case BD_LAYOUT_I: return w->layout;
	case BD_ROLE_I:   return w->role;
	case BD_PAD_I:    return w->pad;
	case BD_GAP_I:    return w->gap;
	case BD_VISIBLE_B:  return w->visible;
	case BD_ENABLED_B:  return w->enabled;
	case BD_MENU_PIN_B: return w->menu_pinned;
	}
	return 0;
}

const char *
bd_get_s(bd_id id, int attr)
{
	if (id == BD_NONE || !pool[id].alive)
		return NULL;
	switch (attr) {
	case BD_LABEL_S:
		if (pool[id].type == BD_INPUT_LINE)
			return pool[id].text_buf;
		return pool[id].label;
	case BD_NAME_S:  return pool[id].acc_name;
	}
	return NULL;
}

bd_id
bd_parent(bd_id id)
{
	return (id != BD_NONE && pool[id].alive) ? pool[id].parent : BD_NONE;
}

bd_id
bd_first_child(bd_id id)
{
	return (id != BD_NONE && pool[id].alive) ? pool[id].first_child : BD_NONE;
}

bd_id
bd_next_sibling(bd_id id)
{
	return (id != BD_NONE && pool[id].alive) ? pool[id].next_sib : BD_NONE;
}

/* ------------------------------------------------------------------ */
/* layout engine                                                      */
/* ------------------------------------------------------------------ */

static void
layout_children(bd_id id)
{
	struct widget *w = &pool[id];
	if (w->first_child == BD_NONE)
		return;

	int ax = w->x + w->pad;
	int ay = w->y + w->pad;
	int aw = w->w - 2 * w->pad;
	int ah = w->h - 2 * w->pad;
	if (aw < 0) aw = 0;
	if (ah < 0) ah = 0;

	int mode = w->layout ? w->layout : BD_LAYOUT_COL;

	if (mode == BD_LAYOUT_FIXED) {
		bd_id c;
		for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib) {
			struct widget *ch = &pool[c];
			ch->x = ax + ch->user_x;
			ch->y = ay + ch->user_y;
			ch->w = ch->pref_w > 0 ? ch->pref_w : aw;
			ch->h = ch->pref_h > 0 ? ch->pref_h : ah;
			if (ch->type != BD_MENU)
				layout_children(c);
		}
		return;
	}

	/* flexbox: row or col */
	int is_row = (mode == BD_LAYOUT_ROW);
	int total = is_row ? aw : ah;
	int cross = is_row ? ah : aw;
	int sum_pref = 0, sum_grow = 0, n = 0;
	bd_id c;

	for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib) {
		int pref = is_row ? pool[c].pref_w : pool[c].pref_h;
		if (pref <= 0)
			pref = is_row ? GLYPH_W * 8 : GLYPH_H;
		sum_pref += pref;
		sum_grow += pool[c].grow;
		n++;
	}

	int gaps = n > 1 ? (n - 1) * w->gap : 0;
	int remaining = total - sum_pref - gaps;
	if (remaining < 0) remaining = 0;

	int pos = is_row ? ax : ay;
	for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib) {
		struct widget *ch = &pool[c];
		int pref = is_row ? ch->pref_w : ch->pref_h;
		if (pref <= 0)
			pref = is_row ? GLYPH_W * 8 : GLYPH_H;

		int extent = pref;
		if (sum_grow > 0 && ch->grow > 0)
			extent += remaining * ch->grow / sum_grow;

		if (is_row) {
			ch->x = pos; ch->y = ay;
			ch->w = extent; ch->h = cross;
		} else {
			ch->x = ax; ch->y = pos;
			ch->w = cross; ch->h = extent;
		}

		pos += extent + w->gap;
		if (ch->type != BD_MENU)
			layout_children(c);
	}
}

/* ------------------------------------------------------------------ */
/* rendering                                                          */
/* ------------------------------------------------------------------ */

static void
render_widget(bd_id id)
{
	struct widget *w = &pool[id];
	if (!w->alive || !w->visible)
		return;

	switch (w->type) {
	case BD_FRAME:
		fill_rect(w->x, w->y, w->w, w->h, w->bg);
		break;

	case BD_PANEL:
		if (w->bg & 0xFF)
			fill_rect(w->x, w->y, w->w, w->h, w->bg);
		break;

	case BD_LABEL:
		if (w->bg & 0xFF)
			fill_rect(w->x, w->y, w->w, w->h, w->bg);
		if (w->label) {
			float tx = (float)(w->x + w->pad);
			float ty = chrome_baseline_y(w->y, w->h);
			queue_text(w->label, tx, ty, w->fg);
		}
		break;

	case BD_BUTTON: {
		uint32_t bg = w->bg;
		if (w->pressed && w->hover)
			bg = TH_PRESS;
		else if (w->hover)
			bg = TH_HOVER;
		fill_rect(w->x, w->y, w->w, w->h, bg);
		stroke_rect(w->x, w->y, w->w, w->h, TH_BORDER);
		if (w->label) {
			float tw = chrome_text_w(w->label);
			float tx = (float)w->x + ((float)w->w - tw) * 0.5f;
			float ty = chrome_baseline_y(w->y, w->h);
			queue_text(w->label, tx, ty, w->fg);
		}
		break;
	}

	case BD_MENU: {
		uint32_t bg = 0;
		if (w->menu_open || w->hover)
			bg = TH_HOVER;
		if (bg)
			fill_rect(w->x, w->y, w->w, w->h, bg);
		if (w->label) {
			float tx = (float)(w->x + w->pad) + CHROME_FONT_SZ * 0.25f;
			float ty = chrome_baseline_y(w->y, w->h);
			queue_text(w->label, tx, ty, w->fg);
		}
		break;
	}

	case BD_INPUT_LINE: {
		int focused = (focus_id == id);
		uint32_t border = focused ? TH_FOCUS : TH_BORDER;
		fill_rect(w->x, w->y, w->w, w->h, w->bg);
		stroke_rect(w->x, w->y, w->w, w->h, border);

		float vis_x = (float)(w->x + w->pad);
		float vis_w = (float)(w->w - 2 * w->pad);

		/* keep cursor in view */
		float cursor_px = input_text_px(w->text_buf, w->cursor);
		if (cursor_px - w->scroll_x > vis_w)
			w->scroll_x = cursor_px - vis_w;
		if (cursor_px - w->scroll_x < 0.0f)
			w->scroll_x = cursor_px;

		float base_y = chrome_baseline_y(w->y, w->h);

		/* selection highlight */
		if (focused && w->sel_anchor >= 0 &&
		    w->sel_anchor != w->cursor) {
			int s0 = w->sel_anchor < w->cursor
			    ? w->sel_anchor : w->cursor;
			int s1 = w->sel_anchor < w->cursor
			    ? w->cursor : w->sel_anchor;
			float sx0 = input_text_px(w->text_buf, s0) - w->scroll_x;
			float sx1 = input_text_px(w->text_buf, s1) - w->scroll_x;
			fill_rect((int)(vis_x + sx0), w->y + 2,
			    (int)(sx1 - sx0), w->h - 4, TH_SELECT);
		}

		/* text */
		if (w->text_len > 0)
			queue_text(w->text_buf,
			    vis_x - w->scroll_x, base_y, w->fg);

		/* blinking cursor */
		if (focused) {
			double t = lud_time() - (double)cursor_blink;
			if (((int)(t * 2.0)) % 2 == 0) {
				float cx = vis_x + cursor_px - w->scroll_x;
				fill_rect((int)cx, w->y + 2, 2, w->h - 4, w->fg);
			}
		}
		break;
	}

	default:
		if (w->bg & 0xFF)
			fill_rect(w->x, w->y, w->w, w->h, w->bg);
		break;
	}

	if (w->type != BD_MENU && w->type != BD_INPUT_LINE) {
		bd_id c;
		for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib)
			render_widget(c);
	}
}

/* ------------------------------------------------------------------ */
/* hit testing                                                        */
/* ------------------------------------------------------------------ */

static void
update_hover(bd_id id, int mx, int my)
{
	struct widget *w = &pool[id];
	if (!w->alive || !w->visible)
		return;
	w->hover = in_rect(mx, my, w->x, w->y, w->w, w->h);
	if (w->type == BD_MENU && !w->menu_open)
		return;
	bd_id c;
	for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib)
		update_hover(c, mx, my);
}

static bd_id
hit_interactive(bd_id id, int mx, int my)
{
	struct widget *w = &pool[id];
	if (!w->alive || !w->visible)
		return BD_NONE;
	if (!in_rect(mx, my, w->x, w->y, w->w, w->h))
		return BD_NONE;

	bd_id found = BD_NONE;
	bd_id c;
	for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib) {
		bd_id r = hit_interactive(c, mx, my);
		if (r != BD_NONE)
			found = r;
	}
	if (found != BD_NONE)
		return found;
	if ((w->on_click || w->type == BD_INPUT_LINE) && w->enabled)
		return id;
	return BD_NONE;
}

/* ------------------------------------------------------------------ */
/* popup menus                                                        */
/* ------------------------------------------------------------------ */

#define POPUP_PAD     4
#define PIN_ROW_H     ((int)CHROME_FONT_SZ + 6)
#define MENU_ITEM_H   ((int)CHROME_FONT_SZ + 6)
#define POPUP_MIN_W   100
#define PIN_CLICK_W   (PIN_OUT_W + 2 * POPUP_PAD)
#define PIN_OUT_W     29
#define PIN_OUT_H     14
#define PIN_IN_W      15
#define PIN_IN_H      15

static void
menu_open_at(bd_id id)
{
	pool[id].menu_open = 1;
	if (!pool[id].menu_pinned)
		active_menu = id;
}

static void
menu_close(bd_id id)
{
	struct widget *w = &pool[id];
	w->menu_open = 0;
	w->menu_pinned = 0;
	if (active_menu == id)
		active_menu = BD_NONE;
	bd_id c;
	for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib)
		pool[c].hover = 0;
}

static void
position_popup_items(struct widget *w)
{
	int iy = w->popup_y + PIN_ROW_H + 1;
	bd_id c;
	for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib) {
		struct widget *ch = &pool[c];
		ch->x = w->popup_x;
		ch->y = iy;
		ch->w = w->popup_w;
		ch->h = MENU_ITEM_H;
		iy += MENU_ITEM_H;
	}
}

static void
layout_popup(bd_id id)
{
	struct widget *w = &pool[id];
	if (!w->menu_open)
		return;

	int n = 0;
	float max_label = 0;
	bd_id c;
	for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib) {
		float lw = chrome_text_w(pool[c].label);
		if (lw > max_label)
			max_label = lw;
		n++;
	}

	int pw = (int)(max_label + 0.5f) + 2 * POPUP_PAD + (int)CHROME_FONT_SZ;
	if (pw < POPUP_MIN_W)
		pw = POPUP_MIN_W;
	int ph = PIN_ROW_H + 1 + n * MENU_ITEM_H + POPUP_PAD;

	w->popup_w = pw;
	w->popup_h = ph;

	if (!w->menu_pinned) {
		int px = w->x;
		int py = w->y + w->h;

		int ww = lud_width(), wh = lud_height();
		if (px + pw > ww) px = ww - pw;
		if (px < 0) px = 0;
		if (py + ph > wh) py = w->y - ph;
		if (py < 0) py = 0;

		w->popup_x = px;
		w->popup_y = py;
	}

	position_popup_items(w);
}

static void
render_popup(bd_id id)
{
	struct widget *w = &pool[id];
	if (!w->menu_open)
		return;

	int px = w->popup_x, py = w->popup_y;
	int pw = w->popup_w, ph = w->popup_h;

	fill_rect(px, py, pw, ph, TH_PANEL);
	stroke_rect(px, py, pw, ph, TH_BORDER);

	/* pushpin row — pen advances through inline sprite + text */
	int pin_hover = in_rect(mouse_x, mouse_y,
	    px, py, pw, PIN_ROW_H);
	if (pin_hover)
		fill_rect(px + 1, py + 1, pw - 2, PIN_ROW_H - 1, TH_HOVER);
	{
		lud_texture_t pt = w->menu_pinned ? pin_in_tex : pin_out_tex;
		float ptw = w->menu_pinned ? PIN_IN_W : PIN_OUT_W;
		float pth = w->menu_pinned ? PIN_IN_H : PIN_OUT_H;
		float pen_x = (float)(px + POPUP_PAD);
		float base_y = chrome_baseline_y(py, PIN_ROW_H);
		float sprite_y = (float)py + ((float)PIN_ROW_H - pth) * 0.5f;

		pen_x += queue_sprite(pt, pen_x, sprite_y,
		    ptw, pth, ptw, pth, TH_TEXT_HI);
		pen_x += POPUP_PAD;
		queue_text(w->label, pen_x, base_y, TH_TEXT);
	}

	/* separator */
	fill_rect(px + 1, py + PIN_ROW_H, pw - 2, 1, TH_BORDER);

	/* menu items */
	bd_id c;
	for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib) {
		struct widget *ch = &pool[c];
		if (ch->hover)
			fill_rect(ch->x, ch->y, ch->w, ch->h, TH_HOVER);
		if (ch->label) {
			float tx = (float)(ch->x + POPUP_PAD);
			float ty = chrome_baseline_y(ch->y, ch->h);
			uint32_t fg = ch->enabled ? TH_TEXT_HI : TH_TEXT;
			queue_text(ch->label, tx, ty, fg);
		}
	}
}

static void
render_popups(void)
{
	bd_id i;
	for (i = 1; i < (bd_id)pool_next; i++) {
		if (pool[i].alive && pool[i].type == BD_MENU && pool[i].menu_open)
			render_popup(i);
	}
}

static bd_id
find_menu_sibling(bd_id menu_id, int mx, int my)
{
	struct widget *m = &pool[menu_id];
	if (m->parent == BD_NONE)
		return BD_NONE;
	bd_id c;
	for (c = pool[m->parent].first_child; c != BD_NONE;
	    c = pool[c].next_sib) {
		if (c != menu_id && pool[c].type == BD_MENU &&
		    in_rect(mx, my, pool[c].x, pool[c].y,
		        pool[c].w, pool[c].h))
			return c;
	}
	return BD_NONE;
}

static void
pin_menu(bd_id id)
{
	pool[id].menu_pinned = 1;
	if (active_menu == id)
		active_menu = BD_NONE;
}

static int
handle_menu_event(const lud_event_t *ev)
{
	switch (ev->type) {
	case LUD_EV_MOUSE_MOVE:
		mouse_x = ev->mouse_move.x;
		mouse_y = ev->mouse_move.y;
		if (drag_menu != BD_NONE) {
			struct widget *dm = &pool[drag_menu];
			dm->popup_x = mouse_x - drag_off_x;
			dm->popup_y = mouse_y - drag_off_y;
			position_popup_items(dm);
			return 1;
		}
		if (active_menu != BD_NONE) {
			bd_id sib = find_menu_sibling(active_menu,
			    mouse_x, mouse_y);
			if (sib != BD_NONE) {
				menu_close(active_menu);
				menu_open_at(sib);
				layout_popup(sib);
			}
		}
		return 0;

	case LUD_EV_MOUSE_UP:
		if (drag_menu != BD_NONE) {
			drag_menu = BD_NONE;
			return 1;
		}
		return 0;

	case LUD_EV_MOUSE_DOWN: {
		if (ev->mouse_button.button != LUD_MOUSE_LEFT)
			return 0;
		int mx = ev->mouse_button.x;
		int my = ev->mouse_button.y;
		bd_id i;

		for (i = 1; i < (bd_id)pool_next; i++) {
			struct widget *m = &pool[i];
			if (!m->alive || m->type != BD_MENU || !m->menu_open)
				continue;
			if (!in_rect(mx, my, m->popup_x, m->popup_y,
			    m->popup_w, m->popup_h))
				continue;

			/* title bar */
			if (my < m->popup_y + PIN_ROW_H) {
				if (mx < m->popup_x + PIN_CLICK_W) {
					if (m->menu_pinned)
						menu_close(i);
					else
						pin_menu(i);
				} else {
					if (!m->menu_pinned)
						pin_menu(i);
					drag_menu = i;
					drag_off_x = mx - m->popup_x;
					drag_off_y = my - m->popup_y;
				}
				return 1;
			}

			/* menu item */
			bd_id c;
			for (c = m->first_child; c != BD_NONE;
			    c = pool[c].next_sib) {
				struct widget *ch = &pool[c];
				if (in_rect(mx, my, ch->x, ch->y,
				    ch->w, ch->h)) {
					if (ch->on_click && ch->enabled)
						ch->on_click(c, ch->on_click_data);
					if (!m->menu_pinned)
						menu_close(i);
					return 1;
				}
			}
			return 1;
		}

		/* triggers */
		for (i = 1; i < (bd_id)pool_next; i++) {
			struct widget *m = &pool[i];
			if (!m->alive || m->type != BD_MENU)
				continue;
			if (!in_rect(mx, my, m->x, m->y, m->w, m->h))
				continue;
			if (m->menu_open) {
				if (!m->menu_pinned)
					menu_close(i);
			} else {
				if (active_menu != BD_NONE)
					menu_close(active_menu);
				menu_open_at(i);
				layout_popup(i);
			}
			return 1;
		}

		/* click outside — close active unpinned menu */
		if (active_menu != BD_NONE) {
			menu_close(active_menu);
			return 1;
		}
		return 0;
	}

	default:
		return 0;
	}
}

/* ------------------------------------------------------------------ */
/* input line editing                                                 */
/* ------------------------------------------------------------------ */

static void
input_delete_selection(struct widget *w)
{
	int s0 = w->sel_anchor < w->cursor ? w->sel_anchor : w->cursor;
	int s1 = w->sel_anchor < w->cursor ? w->cursor : w->sel_anchor;
	memmove(w->text_buf + s0, w->text_buf + s1, w->text_len - s1);
	w->text_len -= (s1 - s0);
	w->text_buf[w->text_len] = '\0';
	w->cursor = s0;
	w->sel_anchor = -1;
}

static void
input_insert_char(bd_id id, unsigned codepoint)
{
	struct widget *w = &pool[id];
	char enc[4];
	int n = utf8_encode(codepoint, enc);

	if (w->sel_anchor >= 0 && w->sel_anchor != w->cursor)
		input_delete_selection(w);

	if (w->text_len + n >= (int)sizeof(w->text_buf))
		return;

	memmove(w->text_buf + w->cursor + n,
	    w->text_buf + w->cursor,
	    w->text_len - w->cursor);
	memcpy(w->text_buf + w->cursor, enc, n);
	w->text_len += n;
	w->cursor += n;
	w->text_buf[w->text_len] = '\0';
	w->sel_anchor = -1;
	cursor_blink = (float)lud_time();
}

static void
input_click(bd_id id, int mx)
{
	struct widget *w = &pool[id];
	float text_x = (float)(w->x + w->pad) - w->scroll_x;
	int best = 0;
	float best_d = (float)mx - text_x;
	if (best_d < 0) best_d = -best_d;

	int pos = 0;
	while (pos < w->text_len) {
		int next = utf8_next(w->text_buf, pos, w->text_len);
		float px = text_x + input_text_px(w->text_buf, next);
		float d = (float)mx - px;
		if (d < 0) d = -d;
		if (d < best_d) {
			best = next;
			best_d = d;
		}
		pos = next;
	}
	w->cursor = best;
	w->sel_anchor = -1;
	cursor_blink = (float)lud_time();
}

static int
input_key(bd_id id, int key, unsigned mods)
{
	struct widget *w = &pool[id];
	int shift = mods & LUD_MOD_SHIFT;
	int ctrl = mods & LUD_MOD_CTRL;
	int old_cursor = w->cursor;

	switch (key) {
	case LUD_KEY_LEFT:
		if (w->sel_anchor >= 0 && !shift) {
			w->cursor = w->sel_anchor < w->cursor
			    ? w->sel_anchor : w->cursor;
			w->sel_anchor = -1;
			cursor_blink = (float)lud_time();
			return 1;
		}
		if (ctrl)
			w->cursor = word_left(w->text_buf, w->cursor);
		else
			w->cursor = utf8_prev(w->text_buf, w->cursor);
		break;
	case LUD_KEY_RIGHT:
		if (w->sel_anchor >= 0 && !shift) {
			w->cursor = w->sel_anchor > w->cursor
			    ? w->sel_anchor : w->cursor;
			w->sel_anchor = -1;
			cursor_blink = (float)lud_time();
			return 1;
		}
		if (ctrl)
			w->cursor = word_right(w->text_buf, w->cursor,
			    w->text_len);
		else
			w->cursor = utf8_next(w->text_buf, w->cursor,
			    w->text_len);
		break;
	case LUD_KEY_HOME:
		w->cursor = 0;
		break;
	case LUD_KEY_END:
		w->cursor = w->text_len;
		break;

	case LUD_KEY_BACKSPACE:
		if (w->sel_anchor >= 0 && w->sel_anchor != w->cursor) {
			input_delete_selection(w);
		} else if (w->cursor > 0) {
			int prev = utf8_prev(w->text_buf, w->cursor);
			memmove(w->text_buf + prev,
			    w->text_buf + w->cursor,
			    w->text_len - w->cursor);
			w->text_len -= (w->cursor - prev);
			w->cursor = prev;
			w->text_buf[w->text_len] = '\0';
		}
		w->sel_anchor = -1;
		cursor_blink = (float)lud_time();
		return 1;
	case LUD_KEY_DELETE:
		if (w->sel_anchor >= 0 && w->sel_anchor != w->cursor) {
			input_delete_selection(w);
		} else if (w->cursor < w->text_len) {
			int next = utf8_next(w->text_buf, w->cursor,
			    w->text_len);
			memmove(w->text_buf + w->cursor,
			    w->text_buf + next,
			    w->text_len - next);
			w->text_len -= (next - w->cursor);
			w->text_buf[w->text_len] = '\0';
		}
		w->sel_anchor = -1;
		cursor_blink = (float)lud_time();
		return 1;

	case LUD_KEY_ENTER:
		if (w->on_click)
			w->on_click(id, w->on_click_data);
		w->text_buf[0] = '\0';
		w->text_len = 0;
		w->cursor = 0;
		w->sel_anchor = -1;
		w->scroll_x = 0.0f;
		cursor_blink = (float)lud_time();
		return 1;
	case LUD_KEY_ESCAPE:
		focus_id = BD_NONE;
		return 1;
	case LUD_KEY_A:
		if (ctrl) {
			w->sel_anchor = 0;
			w->cursor = w->text_len;
			cursor_blink = (float)lud_time();
			return 1;
		}
		return 0;
	default:
		return 0;
	}

	if (shift) {
		if (w->sel_anchor < 0)
			w->sel_anchor = old_cursor;
	} else {
		w->sel_anchor = -1;
	}
	cursor_blink = (float)lud_time();
	return 1;
}

/* ------------------------------------------------------------------ */
/* public: lifecycle                                                  */
/* ------------------------------------------------------------------ */

void
bd_gui_init(void)
{
	font_tex = lud_load_texture("src/birdie/assets/font8x16.png",
	    LUD_FILTER_NEAREST, LUD_FILTER_NEAREST);
	if (font_tex.id == 0)
		fprintf(stderr, "bd: failed to load chrome font atlas\n");
	gui_font = lud_load_vfont(
	    "src/thirdparty/ludica/assets/fonts/dejavu-sans");
	if (gui_font.id == 0)
		fprintf(stderr, "bd: failed to load GUI font\n");
	pin_out_tex = lud_load_texture("src/birdie/assets/pushpin/pushpin-out-14.png",
	    LUD_FILTER_NEAREST, LUD_FILTER_NEAREST);
	pin_in_tex = lud_load_texture("src/birdie/assets/pushpin/pushpin-in-14.png",
	    LUD_FILTER_NEAREST, LUD_FILTER_NEAREST);
}

void
bd_gui_cleanup(void)
{
	if (gui_font.id != 0)
		lud_destroy_vfont(gui_font);
	if (pin_out_tex.id != 0)
		lud_destroy_texture(pin_out_tex);
	if (pin_in_tex.id != 0)
		lud_destroy_texture(pin_in_tex);
	if (font_tex.id != 0)
		lud_destroy_texture(font_tex);
	memset(pool, 0, sizeof pool);
	pool_next = 1;
	root = BD_NONE;
	active_menu = BD_NONE;
	active_press = BD_NONE;
	drag_menu = BD_NONE;
}

void
bd_gui_layout(int win_w, int win_h)
{
	if (root == BD_NONE)
		return;
	struct widget *r = &pool[root];
	r->x = 0;
	r->y = 0;
	r->w = win_w;
	r->h = win_h;
	layout_children(root);

	bd_id i;
	for (i = 1; i < (bd_id)pool_next; i++) {
		if (pool[i].alive && pool[i].type == BD_MENU && pool[i].menu_open)
			layout_popup(i);
	}
}

void
bd_gui_render(void)
{
	if (root == BD_NONE)
		return;
	int w = lud_width(), h = lud_height();

	lud_viewport(0, 0, w, h);
	lud_clear(0.0f, 0.0f, 0.0f, 1.0f);

	/* layer 1: main widget tree */
	lud_sprite_begin(0, 0, w, h);
	render_widget(root);
	lud_sprite_end();
	flush_text(w, h);

	/* layer 2: popup overlays */
	lud_sprite_begin(0, 0, w, h);
	render_popups();
	lud_sprite_end();
	flush_text(w, h);
}

int
bd_gui_event(const void *evp)
{
	const lud_event_t *ev = evp;
	if (root == BD_NONE)
		return 0;

	if (handle_menu_event(ev))
		return 1;

	/* keyboard events for focused input line */
	if (focus_id != BD_NONE &&
	    pool[focus_id].alive &&
	    pool[focus_id].type == BD_INPUT_LINE) {
		if (ev->type == LUD_EV_CHAR && ev->ch.codepoint >= 32) {
			input_insert_char(focus_id, ev->ch.codepoint);
			return 1;
		}
		if (ev->type == LUD_EV_KEY_DOWN)
			return input_key(focus_id, ev->key.keycode,
			    ev->modifiers);
	}

	switch (ev->type) {
	case LUD_EV_MOUSE_MOVE:
		update_hover(root, ev->mouse_move.x, ev->mouse_move.y);
		return 0;

	case LUD_EV_MOUSE_DOWN:
		if (ev->mouse_button.button == LUD_MOUSE_LEFT) {
			int mx = ev->mouse_button.x;
			int my = ev->mouse_button.y;
			bd_id hit = hit_interactive(root, mx, my);

			if (hit != BD_NONE &&
			    pool[hit].type == BD_INPUT_LINE) {
				focus_id = hit;
				input_click(hit, mx);
				return 1;
			}

			focus_id = BD_NONE;

			if (hit != BD_NONE) {
				pool[hit].pressed = 1;
				active_press = hit;
				return 1;
			}
		}
		return 0;

	case LUD_EV_MOUSE_UP:
		if (ev->mouse_button.button == LUD_MOUSE_LEFT &&
		    active_press != BD_NONE) {
			struct widget *w = &pool[active_press];
			w->pressed = 0;
			if (w->on_click &&
			    in_rect(ev->mouse_button.x, ev->mouse_button.y,
			        w->x, w->y, w->w, w->h))
				w->on_click(active_press, w->on_click_data);
			active_press = BD_NONE;
			return 1;
		}
		return 0;

	default:
		return 0;
	}
}
