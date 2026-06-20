/*
 * bd_widget_table -- multi-column table / data grid. See bd_widget_table.h.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_widget_table.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include "bd_theme.h"

#include <stdlib.h>
#include <string.h>

#define CELL_PAD   6        /* horizontal padding inside a cell */
#define ROW_EXTRA  6        /* row height = line height + this */
#define SBW       12        /* scrollbar gutter width */
#define MIN_COLW  24
#define DBLCLICK_S 0.4

struct table {
	bd_table_column *cols;
	int ncols;
	bd_table_model model;
	bd_table_cb cb;

	int *order;             /* visual row -> model row (length nrows) */
	char *sel;              /* selected flag per model row (length nrows) */
	int nrows;              /* model rows at last refresh */

	int *colw;              /* resolved pixel widths (length ncols) */

	int cursor;             /* visual row of the cursor, or -1 */
	int anchor;             /* visual row anchor for shift-range */
	float scroll_y;

	int sort_col;           /* -1 = unsorted */
	int sort_desc;

	int dragging_sb;        /* scrollbar thumb held */
	float drag_dy;          /* pointer offset within the thumb */

	int last_row;           /* for double-click detection (visual row) */
	double last_time;
};

static int table_type;

/* ---- geometry ---- */

static int
row_h(void)
{
	return (int)bd_draw_line_height() + ROW_EXTRA;
}

/* Resolve per-column widths over avail_w; zero-width columns share leftover. */
static void
resolve_cols(struct table *t, int avail_w)
{
	int i, fixed = 0, grow = 0, last_grow = -1;

	for (i = 0; i < t->ncols; i++) {
		if (t->cols[i].width > 0)
			fixed += t->cols[i].width;
		else {
			grow++;
			last_grow = i;
		}
	}
	int rem = avail_w - fixed;
	if (rem < 0)
		rem = 0;
	for (i = 0; i < t->ncols; i++) {
		if (t->cols[i].width > 0) {
			t->colw[i] = t->cols[i].width;
		} else {
			int wv = grow ? rem / grow : 0;
			t->colw[i] = wv < MIN_COLW ? MIN_COLW : wv;
		}
	}
	if (grow && last_grow >= 0) {           /* last grow col absorbs rounding */
		int used = 0;
		for (i = 0; i < t->ncols; i++)
			if (t->cols[i].width == 0 && i != last_grow)
				used += t->colw[i];
		int wv = rem - used;
		t->colw[last_grow] = wv < MIN_COLW ? MIN_COLW : wv;
	} else if (!grow && t->ncols > 0 && rem > 0) {
		t->colw[t->ncols - 1] += rem;   /* no grow col: stretch the last */
	}
}

/* Compute body rectangle, header height, scrollbar need, and column widths. */
static void
geom(bd_id id, struct table *t, int *bx, int *by, int *bw, int *bh,
     int *hh, int *need_sb)
{
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	int inner_x = x + 1, inner_y = y + 1, inner_w = w - 2, inner_h = h - 2;
	int header = row_h();
	int body_h = inner_h - header;
	if (body_h < 0)
		body_h = 0;
	int content = t->nrows * row_h();
	int sb = content > body_h;

	*hh = header;
	*need_sb = sb;
	*bx = inner_x;
	*by = inner_y + header;
	*bw = inner_w - (sb ? SBW : 0);
	*bh = body_h;
	resolve_cols(t, *bw);
}

/* ---- sorting ---- */

static struct table *g_sort;    /* qsort is not reentrant; UI is single-thread */

static int
cmp_rows(const void *a, const void *b)
{
	int ra = *(const int *)a, rb = *(const int *)b;
	struct table *t = g_sort;
	const bd_table_column *col = &t->cols[t->sort_col];
	const char *sa = t->model.cell ? t->model.cell(t->model.ctx, ra, t->sort_col) : NULL;
	const char *sb = t->model.cell ? t->model.cell(t->model.ctx, rb, t->sort_col) : NULL;
	int c;

	if (!sa)
		sa = "";
	if (!sb)
		sb = "";
	if (col->flags & BD_TABLE_COL_NUMERIC) {
		long la = strtol(sa, NULL, 10), lb = strtol(sb, NULL, 10);
		c = (la > lb) - (la < lb);
	} else {
		c = strcmp(sa, sb);
	}
	if (t->sort_desc)
		c = -c;
	if (c == 0)
		c = (ra > rb) - (ra < rb);      /* keep it stable */
	return c;
}

static void
rebuild_order(struct table *t)
{
	int i;
	for (i = 0; i < t->nrows; i++)
		t->order[i] = i;
	if (t->sort_col >= 0 && t->sort_col < t->ncols && t->nrows > 1) {
		g_sort = t;
		qsort(t->order, (size_t)t->nrows, sizeof(int), cmp_rows);
		g_sort = NULL;
	}
}

static void
refresh_state(struct table *t)
{
	int n = t->model.rows ? t->model.rows(t->model.ctx) : 0;
	if (n != t->nrows) {            /* size changed: rebuild, drop selection */
		free(t->order);
		free(t->sel);
		t->order = n ? malloc((size_t)n * sizeof(int)) : NULL;
		t->sel = n ? calloc((size_t)n, 1) : NULL;
		t->nrows = (t->order && (!n || t->sel)) ? n : 0;
		if (t->cursor >= t->nrows)
			t->cursor = t->nrows ? t->nrows - 1 : -1;
		t->anchor = t->cursor;
	}
	rebuild_order(t);
}

/* ---- selection helpers (operate on model rows) ---- */

static void
sel_clear(struct table *t)
{
	if (t->sel)
		memset(t->sel, 0, (size_t)t->nrows);
}

static void
sel_changed(bd_id id, struct table *t)
{
	if (t->cb.selection_changed)
		t->cb.selection_changed(id, t->cb.ctx);
}

static void
ensure_visible(bd_id id, struct table *t)
{
	int bx, by, bw, bh, hh, sb;
	if (t->cursor < 0)
		return;
	geom(id, t, &bx, &by, &bw, &bh, &hh, &sb);
	int top = t->cursor * row_h();
	int bot = top + row_h();
	if (top < (int)t->scroll_y)
		t->scroll_y = (float)top;
	else if (bot > (int)t->scroll_y + bh)
		t->scroll_y = (float)(bot - bh);
	if (t->scroll_y < 0)
		t->scroll_y = 0;
}

static void
clamp_scroll(bd_id id, struct table *t)
{
	int bx, by, bw, bh, hh, sb;
	geom(id, t, &bx, &by, &bw, &bh, &hh, &sb);
	int max = t->nrows * row_h() - bh;
	if (max < 0)
		max = 0;
	if (t->scroll_y > max)
		t->scroll_y = (float)max;
	if (t->scroll_y < 0)
		t->scroll_y = 0;
}

/* ---- rendering ---- */

/* Draw text inside a cell, ellipsized to avail and aligned. */
static void
draw_cell(const char *s, int cx, int cy, int cw, int align, uint32_t col)
{
	char buf[512];
	int avail = cw - 2 * CELL_PAD;
	if (!s || !*s || avail <= 0)
		return;
	float tw = bd_draw_text_width(s);
	if (tw > avail) {                       /* ellipsize from the right */
		int n = (int)strlen(s);
		if (n > (int)sizeof buf - 3)
			n = (int)sizeof buf - 3;
		while (n > 0) {
			memcpy(buf, s, (size_t)n);
			buf[n] = '.';
			buf[n + 1] = '.';
			buf[n + 2] = '\0';
			if (bd_draw_text_width(buf) <= avail)
				break;
			n--;
		}
		if (n <= 0)
			return;
		s = buf;
		tw = bd_draw_text_width(s);
		align = BD_TABLE_LEFT;
	}
	float tx = cx + CELL_PAD;
	if (align == BD_TABLE_RIGHT)
		tx = cx + cw - CELL_PAD - tw;
	else if (align == BD_TABLE_CENTER)
		tx = cx + (cw - tw) / 2.0f;
	bd_draw_text(s, tx, cy, col);
}

/* Up/down sort triangle near a header cell's right edge. */
static void
sort_arrow(int cx, int cy, int desc, uint32_t col)
{
	float a = 4, b = 3;
	if (desc)
		bd_draw_quad(cx - a, cy - b, cx + a, cy - b,
		    cx, cy + b, cx, cy + b, col);
	else
		bd_draw_quad(cx - a, cy + b, cx + a, cy + b,
		    cx, cy - b, cx, cy - b, col);
}

static void
table_render(bd_id id, void *state)
{
	struct table *t = state;
	const bd_theme *th = bd_gui_theme();
	const bd_backend *be = bd_backend_get();
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);

	bd_draw_rect(x, y, w, h, th->press);
	bd_draw_rect_lines(x, y, w, h, th->border);

	int bx, by, bw, bh, hh, sb;
	geom(id, t, &bx, &by, &bw, &bh, &hh, &sb);

	int focused = (bd_focused() == id);
	int text_top = (hh - (int)bd_draw_line_height()) / 2;

	/* header */
	bd_draw_rect(bx, y + 1, bw + (sb ? SBW : 0), hh, th->widget);
	int hx = bx, i;
	for (i = 0; i < t->ncols; i++) {
		int cwid = t->colw[i];
		draw_cell(t->cols[i].title ? t->cols[i].title : "",
		    hx, y + 1 + text_top, cwid, t->cols[i].align, th->text_hi);
		if (t->sort_col == i)
			sort_arrow(hx + cwid - CELL_PAD - 4, y + 1 + hh / 2,
			    t->sort_desc, th->text_hi);
		if (i > 0)
			bd_draw_rect(hx, y + 1, 1, hh, th->border);
		hx += cwid;
	}
	bd_draw_rect(bx, by - 1, bw + (sb ? SBW : 0), 1, th->border);

	/* clipped body */
	bd_draw_flush();
	be->scissor(bx, by, bw, bh);
	int rh = row_h();
	int first = (int)t->scroll_y / rh;
	if (first < 0)
		first = 0;
	int v;
	for (v = first; v < t->nrows; v++) {
		int ry = by - (int)t->scroll_y + v * rh;
		if (ry >= by + bh)
			break;
		int mrow = t->order[v];
		if (t->sel[mrow])
			bd_draw_rect(bx, ry, bw, rh, th->select);
		if (focused && v == t->cursor)
			bd_draw_rect_lines(bx, ry, bw, rh, th->focus);
		int cx = bx;
		for (i = 0; i < t->ncols; i++) {
			const char *s = t->model.cell
			    ? t->model.cell(t->model.ctx, mrow, i) : NULL;
			draw_cell(s, cx, ry + text_top, t->colw[i],
			    t->cols[i].align, th->text);
			cx += t->colw[i];
		}
	}
	bd_draw_flush();
	be->scissor_off();

	/* scrollbar */
	if (sb) {
		int gx = bx + bw;
		int content = t->nrows * rh;
		bd_draw_rect(gx, by, SBW, bh, th->press);
		float frac = (float)bh / (float)content;
		int th_h = (int)(bh * frac);
		if (th_h < 20)
			th_h = 20;
		int max = content - bh;
		float pos = max > 0 ? t->scroll_y / max : 0;
		int th_y = by + (int)((bh - th_h) * pos);
		bd_draw_rect(gx + 2, th_y, SBW - 4, th_h, th->border);
	}
}

/* ---- events ---- */

static int
hit_visual_row(bd_id id, struct table *t, int my)
{
	int bx, by, bw, bh, hh, sb;
	geom(id, t, &bx, &by, &bw, &bh, &hh, &sb);
	if (my < by || my >= by + bh)
		return -1;
	int v = ((int)t->scroll_y + (my - by)) / row_h();
	if (v < 0 || v >= t->nrows)
		return -1;
	return v;
}

static int
hit_header_col(bd_id id, struct table *t, int mx, int my)
{
	int bx, by, bw, bh, hh, sb;
	geom(id, t, &bx, &by, &bw, &bh, &hh, &sb);
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	if (my < y + 1 || my >= y + 1 + hh)
		return -1;
	int cx = bx, i;
	for (i = 0; i < t->ncols; i++) {
		if (mx >= cx && mx < cx + t->colw[i])
			return i;
		cx += t->colw[i];
	}
	return -1;
}

static void
select_single(bd_id id, struct table *t, int v)
{
	sel_clear(t);
	if (v >= 0 && v < t->nrows)
		t->sel[t->order[v]] = 1;
	t->cursor = v;
	t->anchor = v;
	sel_changed(id, t);
}

static void
select_range(bd_id id, struct table *t, int a, int b, int additive)
{
	int lo = a < b ? a : b, hi = a < b ? b : a, v;
	if (!additive)
		sel_clear(t);
	for (v = lo; v <= hi; v++)
		if (v >= 0 && v < t->nrows)
			t->sel[t->order[v]] = 1;
	sel_changed(id, t);
}

static int
sb_thumb_hit(bd_id id, struct table *t, int mx, int my, int *thumb_y, int *thumb_h)
{
	int bx, by, bw, bh, hh, sb;
	geom(id, t, &bx, &by, &bw, &bh, &hh, &sb);
	if (!sb)
		return 0;
	int gx = bx + bw;
	if (mx < gx || mx >= gx + SBW || my < by || my >= by + bh)
		return 0;
	int content = t->nrows * row_h();
	float frac = (float)bh / (float)content;
	int h = (int)(bh * frac);
	if (h < 20)
		h = 20;
	int max = content - bh;
	float pos = max > 0 ? t->scroll_y / max : 0;
	*thumb_y = by + (int)((bh - h) * pos);
	*thumb_h = h;
	return 1;
}

static void
sb_to_pointer(bd_id id, struct table *t, int my)
{
	int bx, by, bw, bh, hh, sb;
	geom(id, t, &bx, &by, &bw, &bh, &hh, &sb);
	int content = t->nrows * row_h();
	float frac = (float)bh / (float)content;
	int h = (int)(bh * frac);
	if (h < 20)
		h = 20;
	int track = bh - h;
	int max = content - bh;
	float pos = track > 0 ? (my - by - t->drag_dy) / track : 0;
	if (pos < 0)
		pos = 0;
	if (pos > 1)
		pos = 1;
	t->scroll_y = pos * (max > 0 ? max : 0);
}

static int
table_event(bd_id id, void *state, const bd_event *ev)
{
	struct table *t = state;

	switch (ev->type) {
	case BD_EV_MOUSE_SCROLL:
		t->scroll_y -= ev->scroll_dy * row_h() * 3;
		clamp_scroll(id, t);
		return 1;

	case BD_EV_MOUSE_DOWN: {
		int thy, thh;

		if (ev->button == BD_MOUSE_RIGHT) {
			int v = hit_visual_row(id, t, ev->y);
			if (v >= 0) {
				if (!t->sel[t->order[v]])
					select_single(id, t, v);
				if (t->cb.context)
					t->cb.context(id, t->order[v], ev->x,
					    ev->y, t->cb.ctx);
			}
			return 1;
		}
		if (ev->button != BD_MOUSE_LEFT)
			return 0;

		/* scrollbar thumb grab */
		if (sb_thumb_hit(id, t, ev->x, ev->y, &thy, &thh)) {
			if (ev->y >= thy && ev->y < thy + thh) {
				t->dragging_sb = 1;
				t->drag_dy = ev->y - thy;
			} else {                /* click in track: page */
				t->scroll_y += (ev->y < thy ? -1 : 1) *
				    (float)thh;
				clamp_scroll(id, t);
			}
			return 1;
		}

		/* header click sorts */
		int hc = hit_header_col(id, t, ev->x, ev->y);
		if (hc >= 0) {
			if (!(t->cols[hc].flags & BD_TABLE_COL_NOSORT)) {
				if (t->sort_col == hc)
					t->sort_desc = !t->sort_desc;
				else {
					t->sort_col = hc;
					t->sort_desc = 0;
				}
				rebuild_order(t);
			}
			return 1;
		}

		/* body click selects */
		int v = hit_visual_row(id, t, ev->y);
		if (v < 0) {
			sel_clear(t);
			t->cursor = -1;
			sel_changed(id, t);
			return 1;
		}

		double now = bd_backend_get()->time();
		if (v == t->last_row && now - t->last_time < DBLCLICK_S) {
			if (t->cb.activate)
				t->cb.activate(id, t->order[v], t->cb.ctx);
			t->last_row = -1;
			return 1;
		}
		t->last_row = v;
		t->last_time = now;

		if (ev->mods & BD_MOD_SHIFT) {
			int a = t->anchor >= 0 ? t->anchor : v;
			select_range(id, t, a, v, (ev->mods & BD_MOD_CTRL) != 0);
			t->cursor = v;
		} else if (ev->mods & BD_MOD_CTRL) {
			t->sel[t->order[v]] = !t->sel[t->order[v]];
			t->cursor = v;
			t->anchor = v;
			sel_changed(id, t);
		} else {
			select_single(id, t, v);
		}
		return 1;
	}

	case BD_EV_MOUSE_MOVE:
		if (t->dragging_sb) {
			sb_to_pointer(id, t, ev->y);
			return 1;
		}
		return 0;

	case BD_EV_MOUSE_UP:
		t->dragging_sb = 0;
		return 1;

	case BD_EV_KEY_DOWN: {
		int v = t->cursor, moved = 0;
		switch (ev->key) {
		case BD_KEY_UP:    if (v > 0) { v--; moved = 1; } else if (v < 0 && t->nrows) { v = 0; moved = 1; } break;
		case BD_KEY_DOWN:  if (v < t->nrows - 1) { v++; moved = 1; } break;
		case BD_KEY_HOME:  if (t->nrows) { v = 0; moved = 1; } break;
		case BD_KEY_END:   if (t->nrows) { v = t->nrows - 1; moved = 1; } break;
		case BD_KEY_ENTER:
			if (t->cursor >= 0 && t->cb.activate)
				t->cb.activate(id, t->order[t->cursor], t->cb.ctx);
			return 1;
		default:
			if (ev->codepoint == ' ' && t->cursor >= 0) {
				t->sel[t->order[t->cursor]] ^= 1;
				sel_changed(id, t);
				return 1;
			}
			return 0;
		}
		if (!moved)
			return 1;
		if (ev->mods & BD_MOD_SHIFT) {
			int a = t->anchor >= 0 ? t->anchor : v;
			select_range(id, t, a, v, 0);
			t->cursor = v;
		} else {
			select_single(id, t, v);
		}
		ensure_visible(id, t);
		return 1;
	}

	default:
		return 0;
	}
}

/* ---- class ---- */

static void
table_destroy(bd_id id, void *state)
{
	struct table *t = state;
	(void)id;
	free(t->cols);
	free(t->colw);
	free(t->order);
	free(t->sel);
}

static void
table_layout(bd_id id, void *state, int w, int h)
{
	(void)w;
	(void)h;
	refresh_state(state);
	clamp_scroll(id, state);
}

static const bd_widget_class table_class = {
	.name = "table",
	.state_size = sizeof(struct table),
	.destroy = table_destroy,
	.render = table_render,
	.layout = table_layout,
	.event = table_event,
};

bd_id
bd_table_create(bd_id parent, const bd_table_column *cols, int ncols,
                const bd_table_model *model, const bd_table_cb *cb, ...)
{
	if (!table_type)
		table_type = bd_register_widget_class(&table_class);

	va_list ap;
	va_start(ap, cb);
	bd_id id = bd_create_va(parent, table_type, ap);
	va_end(ap);

	struct table *t = bd_widget_state(id);
	if (!t)
		return id;
	t->ncols = ncols > 0 ? ncols : 0;
	t->cols = t->ncols ? malloc((size_t)t->ncols * sizeof *t->cols) : NULL;
	t->colw = t->ncols ? calloc((size_t)t->ncols, sizeof(int)) : NULL;
	if (t->cols && cols)
		memcpy(t->cols, cols, (size_t)t->ncols * sizeof *t->cols);
	if (model)
		t->model = *model;
	if (cb)
		t->cb = *cb;
	t->cursor = -1;
	t->anchor = -1;
	t->sort_col = -1;
	t->last_row = -1;
	refresh_state(t);
	return id;
}

void
bd_table_refresh(bd_id id)
{
	struct table *t = bd_widget_state(id);
	if (t && bd_widget_type(id) == table_type)
		refresh_state(t);
}

int
bd_table_selection(bd_id id, int *rows, int max)
{
	struct table *t = bd_widget_state(id);
	int i, n = 0;
	if (!t || bd_widget_type(id) != table_type)
		return 0;
	for (i = 0; i < t->nrows; i++) {
		if (!t->sel[i])
			continue;
		if (rows && n < max)
			rows[n] = i;
		n++;
	}
	return n;
}

void
bd_table_select(bd_id id, int row, int add)
{
	struct table *t = bd_widget_state(id);
	if (!t || bd_widget_type(id) != table_type)
		return;
	if (!add)
		sel_clear(t);
	if (row >= 0 && row < t->nrows)
		t->sel[row] = 1;
	sel_changed(id, t);
}

int
bd_table_current(bd_id id)
{
	struct table *t = bd_widget_state(id);
	if (!t || bd_widget_type(id) != table_type)
		return -1;
	if (t->cursor < 0 || t->cursor >= t->nrows)
		return -1;
	return t->order[t->cursor];
}
