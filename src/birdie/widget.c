#include "widget.h"
#include "ludica.h"
#include "ludica_gfx.h"
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

/* flat dark theme — RGBA8 */
#define TH_BG       0x2B2B2BFFu
#define TH_PANEL    0x313335FFu
#define TH_WIDGET   0x3C3F41FFu
#define TH_HOVER    0x4C5052FFu
#define TH_PRESS    0x27292AFFu
#define TH_TEXT     0xBBBBBBFFu
#define TH_TEXT_HI  0xFFFFFFFFu
#define TH_BORDER   0x555555FFu

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
};

/* ------------------------------------------------------------------ */
/* module state                                                       */
/* ------------------------------------------------------------------ */

static struct widget pool[MAX_WIDGETS];
static int pool_next = 1;
static bd_id root = BD_NONE;
static bd_id active_press = BD_NONE;
static lud_texture_t font_tex;

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
	case BD_LABEL_S: w->label = val; break;
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
	case BD_VISIBLE_B: w->visible = val; break;
	case BD_ENABLED_B: w->enabled = val; break;
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
		w->pref_h = GLYPH_H + 8;
		break;
	case BD_LABEL:
		w->pref_h = GLYPH_H;
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
	case BD_VISIBLE_B: return w->visible;
	case BD_ENABLED_B: return w->enabled;
	}
	return 0;
}

const char *
bd_get_s(bd_id id, int attr)
{
	if (id == BD_NONE || !pool[id].alive)
		return NULL;
	switch (attr) {
	case BD_LABEL_S: return pool[id].label;
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
			float ty = (float)(w->y + (w->h - GLYPH_H) / 2);
			draw_str(w->label, tx, ty, w->fg, 0);
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
			int tw = str_px(w->label);
			float tx = (float)(w->x + (w->w - tw) / 2);
			float ty = (float)(w->y + (w->h - GLYPH_H) / 2);
			draw_str(w->label, tx, ty, w->fg, 0);
		}
		break;
	}

	default:
		if (w->bg & 0xFF)
			fill_rect(w->x, w->y, w->w, w->h, w->bg);
		break;
	}

	bd_id c;
	for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib)
		render_widget(c);
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
	if (w->on_click && w->enabled)
		return id;
	return BD_NONE;
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
}

void
bd_gui_cleanup(void)
{
	if (font_tex.id != 0)
		lud_destroy_texture(font_tex);
	memset(pool, 0, sizeof pool);
	pool_next = 1;
	root = BD_NONE;
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
}

void
bd_gui_render(void)
{
	if (root == BD_NONE)
		return;
	int w = lud_width(), h = lud_height();

	lud_viewport(0, 0, w, h);
	lud_clear(0.0f, 0.0f, 0.0f, 1.0f);
	lud_sprite_begin(0, 0, w, h);
	render_widget(root);
	lud_sprite_end();
}

int
bd_gui_event(const void *evp)
{
	const lud_event_t *ev = evp;
	if (root == BD_NONE)
		return 0;

	switch (ev->type) {
	case LUD_EV_MOUSE_MOVE:
		update_hover(root, ev->mouse_move.x, ev->mouse_move.y);
		return 0;

	case LUD_EV_MOUSE_DOWN:
		if (ev->mouse_button.button == LUD_MOUSE_LEFT) {
			bd_id hit = hit_interactive(root,
			    ev->mouse_button.x, ev->mouse_button.y);
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
