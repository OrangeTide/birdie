#include "bd_widget_editor.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Rich-text editor widget. Plain UTF-8 text in a growable buffer with the
 * BD_MULTILINE editing model (caret nav, newline, backspace/delete, click,
 * scroll), plus a list of style runs over byte ranges that the renderer draws
 * segment by segment (per-run fg/bg, underline, strikeout, faux-bold,
 * super/subscript). A row-oriented API and a lock complete the editor.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

struct erun { int start, end; bd_rich_style style; };

struct editor {
	char  *buf;
	int    len, cap;
	int    cursor;
	float  scroll_x;
	int    scroll_y;
	int    locked;
	struct erun *runs;
	int    nrun, runcap;
};

static int editor_type;

/* ------------------------------------------------------------------ */
/* utf-8                                                              */
/* ------------------------------------------------------------------ */

static int e_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

static int
e_prev(const char *s, int p)
{
	if (p <= 0) return 0;
	p--;
	while (p > 0 && e_cont((unsigned char)s[p])) p--;
	return p;
}

static int
e_next(const char *s, int len, int p)
{
	if (p >= len) return len;
	p++;
	while (p < len && e_cont((unsigned char)s[p])) p++;
	return p;
}

static int
e_encode(unsigned cp, char *o)
{
	if (cp < 0x80)    { o[0] = (char)cp; return 1; }
	if (cp < 0x800)   { o[0] = (char)(0xC0|(cp>>6)); o[1]=(char)(0x80|(cp&0x3F)); return 2; }
	if (cp < 0x10000) { o[0]=(char)(0xE0|(cp>>12)); o[1]=(char)(0x80|((cp>>6)&0x3F)); o[2]=(char)(0x80|(cp&0x3F)); return 3; }
	o[0]=(char)(0xF0|(cp>>18)); o[1]=(char)(0x80|((cp>>12)&0x3F));
	o[2]=(char)(0x80|((cp>>6)&0x3F)); o[3]=(char)(0x80|(cp&0x3F));
	return 4;
}

/* ------------------------------------------------------------------ */
/* buffer + style-run editing                                         */
/* ------------------------------------------------------------------ */

static int
e_ensure(struct editor *e, int need)
{
	if (need <= e->cap) return 1;
	int c = e->cap ? e->cap * 2 : 256;
	while (c < need) c *= 2;
	char *p = realloc(e->buf, (size_t)c);
	if (!p) return 0;
	e->buf = p;
	e->cap = c;
	return 1;
}

/* shift one byte offset across an edit at [off,off+dellen) growing by delta */
static int
e_shift(int x, int off, int dellen, int delta)
{
	if (x <= off) return x;
	if (x >= off + dellen) return x + delta;
	return off;
}

/* replace dellen bytes at off with ins[0..inslen); fixes runs + cursor */
static void
e_splice(struct editor *e, int off, int dellen, const char *ins, int inslen)
{
	if (off < 0) off = 0;
	if (off > e->len) off = e->len;
	if (dellen < 0) dellen = 0;
	if (off + dellen > e->len) dellen = e->len - off;

	int newlen = e->len - dellen + inslen;
	if (!e_ensure(e, newlen + 1)) return;
	memmove(e->buf + off + inslen, e->buf + off + dellen,
	    (size_t)(e->len - off - dellen));
	if (inslen) memcpy(e->buf + off, ins, (size_t)inslen);
	e->len = newlen;
	e->buf[e->len] = '\0';

	int delta = inslen - dellen;
	for (int i = 0; i < e->nrun; i++) {
		e->runs[i].start = e_shift(e->runs[i].start, off, dellen, delta);
		e->runs[i].end   = e_shift(e->runs[i].end, off, dellen, delta);
	}
	/* drop emptied runs */
	int w = 0;
	for (int i = 0; i < e->nrun; i++)
		if (e->runs[i].end > e->runs[i].start)
			e->runs[w++] = e->runs[i];
	e->nrun = w;

	e->cursor = e_shift(e->cursor, off, dellen, delta);
	if (e->cursor > e->len) e->cursor = e->len;
}

/* ------------------------------------------------------------------ */
/* rows                                                               */
/* ------------------------------------------------------------------ */

static int
e_row_count(const struct editor *e)
{
	int n = 1;
	for (int i = 0; i < e->len; i++)
		if (e->buf[i] == '\n') n++;
	return n;
}

/* byte offset of the start of `row` (clamped to len) */
static int
e_row_start(const struct editor *e, int row)
{
	int p = 0;
	for (int r = 0; r < row; r++) {
		while (p < e->len && e->buf[p] != '\n') p++;
		if (p < e->len) p++;
		else break;
	}
	return p;
}

static int
e_row_end(const struct editor *e, int off)
{
	while (off < e->len && e->buf[off] != '\n') off++;
	return off;
}

/* start of the line containing pos (scan back to after the previous \n) */
static int
e_line_start(const struct editor *e, int pos)
{
	while (pos > 0 && e->buf[pos - 1] != '\n') pos--;
	return pos;
}

static int
e_line_index(const struct editor *e, int pos)
{
	int n = 0;
	for (int i = 0; i < pos; i++)
		if (e->buf[i] == '\n') n++;
	return n;
}

/* ------------------------------------------------------------------ */
/* style runs                                                         */
/* ------------------------------------------------------------------ */

static void
e_add_run(struct editor *e, int s, int en, bd_rich_style st)
{
	if (s < 0) s = 0;
	if (en > e->len) en = e->len;
	if (en <= s) return;
	if (e->nrun == e->runcap) {
		int c = e->runcap ? e->runcap * 2 : 8;
		struct erun *p = realloc(e->runs, (size_t)c * sizeof *p);
		if (!p) return;
		e->runs = p;
		e->runcap = c;
	}
	e->runs[e->nrun++] = (struct erun){ s, en, st };
}

/* combined style at byte pos: later runs win */
static bd_rich_style
e_style_at(const struct editor *e, int pos)
{
	bd_rich_style st = { 0, 0, 0 };
	for (int i = 0; i < e->nrun; i++)
		if (e->runs[i].start <= pos && pos < e->runs[i].end)
			st = e->runs[i].style;
	return st;
}

static int
e_style_eq(bd_rich_style a, bd_rich_style b)
{
	return a.flags == b.flags && a.fg == b.fg && a.bg == b.bg;
}

/* ------------------------------------------------------------------ */
/* measuring                                                          */
/* ------------------------------------------------------------------ */

/* bd_draw font flags for a style */
static int
e_font(bd_rich_style st)
{
	int f = 0;
	if (st.flags & BD_RT_BOLD)   f |= BD_FONT_BOLD;
	if (st.flags & BD_RT_ITALIC) f |= BD_FONT_ITALIC;
	return f;
}

/* width of buf[a,b) drawn in `font` */
static float
e_seg_w(const struct editor *e, int a, int b, int font)
{
	char tmp[1024];
	int n = b - a;
	if (n <= 0) return 0.0f;
	if (n >= (int)sizeof tmp) n = (int)sizeof tmp - 1;
	memcpy(tmp, e->buf + a, (size_t)n);
	tmp[n] = '\0';
	return bd_draw_text_width_styled(tmp, font);
}

/* style-aware width of buf[a,b): segment by run, measure each in its face */
static float
e_span_px(const struct editor *e, int a, int b)
{
	float w = 0.0f;
	int p = a;
	while (p < b) {
		bd_rich_style st = e_style_at(e, p);
		int q = e_next(e->buf, b, p);
		while (q < b && e_style_eq(e_style_at(e, q), st))
			q = e_next(e->buf, b, q);
		w += e_seg_w(e, p, q, e_font(st));
		p = q;
	}
	return w;
}

static int
e_col_at_px(const struct editor *e, int start, int end, float target)
{
	int pos = start;
	float w = 0.0f;
	while (pos < end) {
		int nx = e_next(e->buf, end, pos);
		float cw = e_seg_w(e, pos, nx, e_font(e_style_at(e, pos)));
		if (w + cw * 0.5f >= target) return pos;
		w += cw;
		pos = nx;
	}
	return end;
}

static int
e_line_h(void)
{
	int h = (int)bd_draw_line_height();
	return h > 0 ? h : 14;
}

/* ------------------------------------------------------------------ */
/* class hooks                                                        */
/* ------------------------------------------------------------------ */

static void
editor_init(bd_id id, void *state)
{
	struct editor *e = state;
	(void)e;
	bd_set(id, BD_PREF_W_I, 280, BD_PREF_H_I, 140, BD_PAD_I, 4, BD_END);
}

static void
editor_destroy(bd_id id, void *state)
{
	(void)id;
	struct editor *e = state;
	free(e->buf);
	free(e->runs);
	e->buf = NULL;
	e->runs = NULL;
}

/* draw one line [a,b) at (x0,top), segmenting by style run */
static void
editor_draw_line(struct editor *e, int a, int b, float x0, int top,
    const bd_theme *th)
{
	int lh = e_line_h();
	float penx = x0;
	int seg = a;
	while (seg < b) {
		bd_rich_style st = e_style_at(e, seg);
		int q = e_next(e->buf, b, seg);
		while (q < b && e_style_eq(e_style_at(e, q), st))
			q = e_next(e->buf, b, q);

		char tmp[1024];
		int n = q - seg;
		if (n >= (int)sizeof tmp) n = (int)sizeof tmp - 1;
		memcpy(tmp, e->buf + seg, (size_t)n);
		tmp[n] = '\0';
		int font = e_font(st);             /* true bold/italic face */
		float w = bd_draw_text_width_styled(tmp, font);

		uint32_t fg = st.fg ? st.fg : th->text;
		int y = top;
		if (st.flags & BD_RT_SUPER) y -= lh / 4;
		else if (st.flags & BD_RT_SUB) y += lh / 4;

		if (st.bg)
			bd_draw_rect(penx, (float)top, w, (float)lh, st.bg);
		bd_draw_text_styled(tmp, penx, (float)y, fg, font);
		if (st.flags & BD_RT_UNDERLINE)
			bd_draw_rect(penx, (float)(y + lh - 2), w, 1.0f, fg);
		if (st.flags & BD_RT_STRIKE)
			bd_draw_rect(penx, (float)(y + lh / 2), w, 1.0f, fg);

		penx += w;
		seg = q;
	}
}

static void
editor_render(bd_id id, void *state)
{
	struct editor *e = state;
	const bd_theme *th = bd_gui_theme();
	const bd_backend *be = bd_backend_get();
	int focused = (bd_focused() == id);

	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	bd_draw_rect(x, y, w, h, th->press);
	bd_draw_rect_lines(x, y, w, h, focused ? th->focus : th->border);

	int pad = bd_get_i(id, BD_PAD_I);
	int ix = x + pad, iy = y + pad;
	int iw = w - 2 * pad, ih = h - 2 * pad;
	int lh = e_line_h();

	/* caret line/x, then keep it in view */
	int cls = e_line_start(e, e->cursor);
	int caret_line = e_line_index(e, e->cursor);
	float caret_px = e_span_px(e, cls, e->cursor);
	int caret_top = caret_line * lh;
	if (caret_top - e->scroll_y < 0) e->scroll_y = caret_top;
	if (caret_top + lh - e->scroll_y > ih) e->scroll_y = caret_top + lh - ih;
	if (e->scroll_y < 0) e->scroll_y = 0;
	if (caret_px - e->scroll_x > (float)iw) e->scroll_x = caret_px - (float)iw;
	if (caret_px - e->scroll_x < 0.0f) e->scroll_x = caret_px;
	if (e->scroll_x < 0.0f) e->scroll_x = 0.0f;

	bd_draw_flush();
	be->scissor(ix, iy, iw, ih);

	int li = 0, pos = 0;
	for (;;) {
		int le = e_row_end(e, pos);
		int top = iy + li * lh - e->scroll_y;
		if (top + lh >= iy && top <= iy + ih)
			editor_draw_line(e, pos, le, (float)ix - e->scroll_x, top, th);
		if (le >= e->len) break;
		pos = le + 1;
		li++;
	}

	if (focused && !e->locked) {
		double t = be->time();
		if (((int)(t * 2.0)) % 2 == 0) {
			int cx = ix + (int)(caret_px - e->scroll_x);
			int cy = iy + caret_top - e->scroll_y;
			bd_draw_rect((float)cx, (float)cy, 2.0f, (float)lh, th->text_hi);
		}
	}

	bd_draw_flush();
	be->scissor_off();
}

/* ------------------------------------------------------------------ */
/* editing helpers                                                    */
/* ------------------------------------------------------------------ */

static void
editor_insert(struct editor *e, unsigned cp)
{
	if (e->locked) return;
	char enc[4];
	int n = e_encode(cp, enc);
	int off = e->cursor;
	e_splice(e, off, 0, enc, n);
	e->cursor = off + n;
}

static int
editor_event(bd_id id, void *state, const bd_event *ev)
{
	struct editor *e = state;
	int lh = e_line_h();

	switch (ev->type) {
	case BD_EV_MOUSE_SCROLL:
		e->scroll_y -= (int)(ev->scroll_dy * (float)lh);
		if (e->scroll_y < 0) e->scroll_y = 0;
		return 1;

	case BD_EV_MOUSE_DOWN: {
		if (ev->button != BD_MOUSE_LEFT) return 1;
		int x, y, w, h;
		bd_widget_rect(id, &x, &y, &w, &h);
		int pad = bd_get_i(id, BD_PAD_I);
		int ix = x + pad, iy = y + pad;
		int line = (ev->y - iy + e->scroll_y) / (lh > 0 ? lh : 1);
		if (line < 0) line = 0;
		int pos = e_row_start(e, line);
		int le = e_row_end(e, pos);
		e->cursor = e_col_at_px(e, pos, le,
		    (float)(ev->x - ix) + e->scroll_x);
		return 1;
	}
	case BD_EV_MOUSE_MOVE:
	case BD_EV_MOUSE_UP:
		return 1;   /* consume capture; selection drag is TODO */

	case BD_EV_CHAR:
		if (ev->codepoint >= 32)
			editor_insert(e, ev->codepoint);
		return 1;

	case BD_EV_KEY_DOWN:
		break;
	default:
		return 0;
	}

	/* key handling */
	int ls = e_line_start(e, e->cursor);
	switch (ev->key) {
	case BD_KEY_LEFT:
		e->cursor = e_prev(e->buf, e->cursor);
		break;
	case BD_KEY_RIGHT:
		e->cursor = e_next(e->buf, e->len, e->cursor);
		break;
	case BD_KEY_HOME:
		e->cursor = ls;
		break;
	case BD_KEY_END:
		e->cursor = e_row_end(e, e->cursor);
		break;
	case BD_KEY_UP: {
		if (ls == 0) { e->cursor = 0; break; }
		float gx = e_span_px(e, ls, e->cursor);
		int ps = e_line_start(e, ls - 1);
		e->cursor = e_col_at_px(e, ps, ls - 1, gx);
		break;
	}
	case BD_KEY_DOWN: {
		int le = e_row_end(e, e->cursor);
		if (le >= e->len) { e->cursor = e->len; break; }
		float gx = e_span_px(e, ls, e->cursor);
		int ns = le + 1, ne = e_row_end(e, ns);
		e->cursor = e_col_at_px(e, ns, ne, gx);
		break;
	}
	case BD_KEY_ENTER:
		editor_insert(e, '\n');
		break;
	case BD_KEY_BACKSPACE:
		if (!e->locked && e->cursor > 0) {
			int p = e_prev(e->buf, e->cursor);
			e_splice(e, p, e->cursor - p, NULL, 0);  /* cursor -> p */
		}
		break;
	case BD_KEY_DELETE:
		if (!e->locked && e->cursor < e->len) {
			int nx = e_next(e->buf, e->len, e->cursor);
			e_splice(e, e->cursor, nx - e->cursor, NULL, 0);
		}
		break;
	case BD_KEY_ESCAPE:
		return 0;   /* let the toolkit drop focus */
	default:
		return 0;
	}
	return 1;
}

static const bd_widget_class editor_class = {
	.name = "editor",
	.state_size = sizeof(struct editor),
	.init = editor_init,
	.destroy = editor_destroy,
	.render = editor_render,
	.event = editor_event,
};

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

static struct editor *
editor_of(bd_id id)
{
	if (bd_widget_type(id) != editor_type) return NULL;
	return bd_widget_state(id);
}

bd_id
bd_editor_create(bd_id parent, ...)
{
	if (editor_type == 0)
		editor_type = bd_register_widget_class(&editor_class);
	va_list ap;
	va_start(ap, parent);
	bd_id id = bd_create_va(parent, editor_type, ap);
	va_end(ap);
	return id;
}

void
bd_editor_set_text(bd_id id, const char *text)
{
	struct editor *e = editor_of(id);
	if (!e) return;
	e->nrun = 0;
	int n = text ? (int)strlen(text) : 0;
	e_splice(e, 0, e->len, text, n);
	e->cursor = 0;
	e->scroll_x = 0.0f;
	e->scroll_y = 0;
}

int
bd_editor_text(bd_id id, char *out, int cap)
{
	struct editor *e = editor_of(id);
	if (!e) return 0;
	if (out && cap > 0) {
		int n = e->len < cap - 1 ? e->len : cap - 1;
		if (n > 0 && e->buf) memcpy(out, e->buf, (size_t)n);
		out[n] = '\0';
	}
	return e->len;
}

int
bd_editor_row_count(bd_id id)
{
	struct editor *e = editor_of(id);
	return e ? e_row_count(e) : 0;
}

int
bd_editor_row_text(bd_id id, int row, char *out, int cap)
{
	struct editor *e = editor_of(id);
	if (!e) return 0;
	int a = e_row_start(e, row);
	int b = e_row_end(e, a);
	int len = b - a;
	if (out && cap > 0) {
		int n = len < cap - 1 ? len : cap - 1;
		if (n > 0) memcpy(out, e->buf + a, (size_t)n);
		out[n] = '\0';
	}
	return len;
}

void
bd_editor_insert_row(bd_id id, int row, const char *s)
{
	struct editor *e = editor_of(id);
	if (!e) return;
	int n = s ? (int)strlen(s) : 0;
	int rc = e_row_count(e);
	if (row >= rc) {
		if (e->len > 0) e_splice(e, e->len, 0, "\n", 1);
		e_splice(e, e->len, 0, s, n);
	} else {
		int off = e_row_start(e, row);
		e_splice(e, off, 0, s, n);
		e_splice(e, off + n, 0, "\n", 1);
	}
}

void
bd_editor_replace_row(bd_id id, int row, const char *s)
{
	struct editor *e = editor_of(id);
	if (!e) return;
	int a = e_row_start(e, row);
	int b = e_row_end(e, a);
	int n = s ? (int)strlen(s) : 0;
	e_splice(e, a, b - a, s, n);
}

void
bd_editor_delete_row(bd_id id, int row)
{
	struct editor *e = editor_of(id);
	if (!e) return;
	int a = e_row_start(e, row);
	int b = e_row_end(e, a);
	if (b < e->len)            /* take the trailing newline */
		b++;
	else if (a > 0)            /* last row: take the preceding newline */
		a--;
	e_splice(e, a, b - a, NULL, 0);
}

void
bd_editor_set_locked(bd_id id, int locked)
{
	struct editor *e = editor_of(id);
	if (e) e->locked = locked;
}

int
bd_editor_locked(bd_id id)
{
	struct editor *e = editor_of(id);
	return e ? e->locked : 0;
}

void
bd_editor_clear_styles(bd_id id)
{
	struct editor *e = editor_of(id);
	if (e) e->nrun = 0;
}

void
bd_editor_style_span(bd_id id, int start, int end, bd_rich_style s)
{
	struct editor *e = editor_of(id);
	if (e) e_add_run(e, start, end, s);
}

void
bd_editor_highlight_row(bd_id id, int row, bd_rich_style s)
{
	struct editor *e = editor_of(id);
	if (!e) return;
	int a = e_row_start(e, row);
	e_add_run(e, a, e_row_end(e, a), s);
}

void
bd_editor_highlight_span(bd_id id, int row, int col0, int col1, bd_rich_style s)
{
	struct editor *e = editor_of(id);
	if (!e) return;
	int rs = e_row_start(e, row);
	int re = e_row_end(e, rs);
	int a = rs + col0, b = rs + col1;
	if (a < rs) a = rs;
	if (b > re) b = re;
	e_add_run(e, a, b, s);
}
