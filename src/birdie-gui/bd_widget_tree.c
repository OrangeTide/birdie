/*
 * bd_widget_tree -- indented expand/collapse hierarchy list. See
 * bd_widget_tree.h.
 *
 * The widget owns no tree structure: on each layout it re-walks the model
 * depth-first into a flat list of visible rows (descending into a node only
 * when the widget's own expand-set has it open), then renders that list like a
 * BD_LIST with a per-row indent and a twisty. Selection and expand state are
 * keyed by the app's uint64_t node ids, so both survive a re-walk.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_widget_tree.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include "bd_theme.h"

#include <stdlib.h>
#include <string.h>

#define ROW_EXTRA   6      /* row height = line height + this */
#define SBW        12      /* scrollbar gutter width */
#define PAD_X       4      /* left inset before the first level */
#define TWISTY_W   14      /* width reserved for a twisty column */
#define ICON_GAP    4
#define DBLCLICK_S  0.4

struct row {
	uint64_t key;
	int      depth;
	int      has_children;
	int      enabled;
};

struct tree {
	bd_tree_model model;
	bd_tree_cb    cb;

	struct row *rows;
	int         nrows, cap;

	uint64_t   *exp;        /* keys currently expanded */
	int         nexp, capexp;

	int         cursor;     /* visual row of the cursor, or -1 */
	uint64_t    sel;        /* selected key (0 = none), survives re-walk */
	float       scroll_y;
	int         indent;     /* px per depth level */

	int         dragging_sb;
	float       drag_dy;

	int         last_row;   /* dbl-click (visual row) */
	double      last_time;
};

static int tree_type;

/* ---- expand set ---- */

static int
is_exp(struct tree *t, uint64_t key)
{
	for (int i = 0; i < t->nexp; i++)
		if (t->exp[i] == key)
			return 1;
	return 0;
}

static void
set_exp(struct tree *t, uint64_t key, int open)
{
	int i;
	for (i = 0; i < t->nexp; i++)
		if (t->exp[i] == key)
			break;
	if (open && i == t->nexp) {
		if (t->nexp == t->capexp) {
			int nc = t->capexp ? t->capexp * 2 : 8;
			uint64_t *ne = realloc(t->exp, (size_t)nc * sizeof *ne);
			if (!ne)
				return;
			t->exp = ne;
			t->capexp = nc;
		}
		t->exp[t->nexp++] = key;
	} else if (!open && i < t->nexp) {
		t->exp[i] = t->exp[--t->nexp];
	}
}

/* ---- geometry ---- */

static int
row_h(void)
{
	return (int)bd_draw_line_height() + ROW_EXTRA;
}

/* Body rectangle, plus whether a scrollbar is needed. */
static void
geom(bd_id id, struct tree *t, int *bx, int *by, int *bw, int *bh, int *need_sb)
{
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	int iw = w - 2, ih = h - 2;
	if (ih < 0)
		ih = 0;
	int content = t->nrows * row_h();
	int sb = content > ih;
	*need_sb = sb;
	*bx = x + 1;
	*by = y + 1;
	*bw = iw - (sb ? SBW : 0);
	*bh = ih;
}

/* ---- flatten ---- */

static void
push_row(struct tree *t, uint64_t key, int depth, int has_children, int enabled)
{
	if (t->nrows == t->cap) {
		int nc = t->cap ? t->cap * 2 : 32;
		struct row *nr = realloc(t->rows, (size_t)nc * sizeof *nr);
		if (!nr)
			return;
		t->rows = nr;
		t->cap = nc;
	}
	t->rows[t->nrows].key = key;
	t->rows[t->nrows].depth = depth;
	t->rows[t->nrows].has_children = has_children;
	t->rows[t->nrows].enabled = enabled;
	t->nrows++;
}

static void
walk(struct tree *t, uint64_t parent, int depth)
{
	if (depth > 64 || !t->model.child_count || !t->model.child)
		return;                          /* guard runaway / cyclic models */
	int n = t->model.child_count(t->model.ctx, parent);
	for (int i = 0; i < n; i++) {
		uint64_t key = t->model.child(t->model.ctx, parent, i);
		bd_tree_item it = { .enabled = 1 };
		if (t->model.get)
			t->model.get(t->model.ctx, key, &it);
		push_row(t, key, depth, it.has_children, it.enabled);
		if (it.has_children && is_exp(t, key))
			walk(t, key, depth + 1);
	}
}

/* Re-walk the model; re-derive the cursor from the retained selection key. */
static void
refresh_state(struct tree *t)
{
	t->nrows = 0;
	walk(t, 0, 0);
	t->cursor = -1;
	if (t->sel) {
		for (int v = 0; v < t->nrows; v++)
			if (t->rows[v].key == t->sel) {
				t->cursor = v;
				break;
			}
	}
}

static void
clamp_scroll(bd_id id, struct tree *t)
{
	int bx, by, bw, bh, sb;
	geom(id, t, &bx, &by, &bw, &bh, &sb);
	int max = t->nrows * row_h() - bh;
	if (max < 0)
		max = 0;
	if (t->scroll_y > max)
		t->scroll_y = (float)max;
	if (t->scroll_y < 0)
		t->scroll_y = 0;
}

static void
ensure_visible(bd_id id, struct tree *t)
{
	if (t->cursor < 0)
		return;
	int bx, by, bw, bh, sb;
	geom(id, t, &bx, &by, &bw, &bh, &sb);
	int top = t->cursor * row_h();
	int bot = top + row_h();
	if (top < (int)t->scroll_y)
		t->scroll_y = (float)top;
	else if (bot > (int)t->scroll_y + bh)
		t->scroll_y = (float)(bot - bh);
	if (t->scroll_y < 0)
		t->scroll_y = 0;
}

/* x of the twisty box for a row at `depth`, relative to body left. */
static int
twisty_x(struct tree *t, int bx, int depth)
{
	return bx + PAD_X + depth * t->indent;
}

/* ---- callbacks ---- */

static void
fire_select(bd_id id, struct tree *t)
{
	if (t->cursor < 0 || !t->cb.select)
		return;
	uint64_t key = t->rows[t->cursor].key;
	bd_tree_item it = { .enabled = 1 };
	if (t->model.get)
		t->model.get(t->model.ctx, key, &it);
	t->cb.select(id, key, it.user);
}

static void
toggle_expand(bd_id id, struct tree *t, int v)
{
	uint64_t key = t->rows[v].key;
	int open = !is_exp(t, key);
	set_exp(t, key, open);
	if (t->cb.expand) {
		bd_tree_item it = { .enabled = 1 };
		if (t->model.get)
			t->model.get(t->model.ctx, key, &it);
		t->cb.expand(id, key, open, it.user);
	}
	refresh_state(t);
}

static void
activate(bd_id id, struct tree *t, int v)
{
	uint64_t key = t->rows[v].key;
	bd_tree_item it = { .enabled = 1 };
	if (t->model.get)
		t->model.get(t->model.ctx, key, &it);
	if (it.enabled && t->cb.activate)
		t->cb.activate(id, key, it.user);
}

/* ---- rendering ---- */

/* A twisty: right-pointing triangle when collapsed, down-pointing when open. */
static void
twisty(int cx, int cy, int open, uint32_t col)
{
	float a = 3.5f;
	if (open)
		bd_draw_quad(cx - a, cy - a*0.6f, cx + a, cy - a*0.6f,
		    cx, cy + a*0.8f, cx, cy + a*0.8f, col);
	else
		bd_draw_quad(cx - a*0.6f, cy - a, cx - a*0.6f, cy + a,
		    cx + a*0.8f, cy, cx + a*0.8f, cy, col);
}

static void
tree_render(bd_id id, void *state)
{
	struct tree *t = state;
	const bd_theme *th = bd_gui_theme();
	const bd_backend *be = bd_backend_get();
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);

	bd_draw_rect(x, y, w, h, th->press);
	bd_draw_rect_lines(x, y, w, h, th->border);

	int bx, by, bw, bh, sb;
	geom(id, t, &bx, &by, &bw, &bh, &sb);
	int focused = (bd_focused() == id);
	int rh = row_h();
	int text_top = (rh - (int)bd_draw_line_height()) / 2;
	int icon_sz = (int)bd_draw_line_height();

	bd_draw_flush();
	be->scissor(bx, by, bw, bh);
	int first = (int)t->scroll_y / rh;
	if (first < 0)
		first = 0;
	for (int v = first; v < t->nrows; v++) {
		int ry = by - (int)t->scroll_y + v * rh;
		if (ry >= by + bh)
			break;
		uint64_t key = t->rows[v].key;
		bd_tree_item it = { .enabled = 1 };
		if (t->model.get)
			t->model.get(t->model.ctx, key, &it);

		if (key == t->sel)
			bd_draw_rect(bx, ry, bw, rh, th->select);
		if (focused && v == t->cursor)
			bd_draw_rect_lines(bx, ry, bw, rh, th->focus);

		int tx = twisty_x(t, bx, t->rows[v].depth);
		if (t->rows[v].has_children)
			twisty(tx + TWISTY_W / 2, ry + rh / 2, is_exp(t, key),
			    th->text_hi);

		int lx = tx + TWISTY_W;
		if (it.icon.id) {
			uint32_t tint = it.enabled ? 0xFFFFFFFFu : 0xFFFFFF80u;
			bd_draw_sprite(it.icon, (float)lx, (float)(ry + text_top),
			    (float)icon_sz, (float)icon_sz, 0, 0, 1, 1, tint);
			lx += icon_sz + ICON_GAP;
		}
		if (it.label && it.label[0]) {
			uint32_t tc = it.enabled ? th->text : th->border;
			bd_draw_text(it.label, (float)lx, (float)(ry + text_top), tc);
		}
	}
	bd_draw_flush();
	be->scissor_off();

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
hit_row(bd_id id, struct tree *t, int my)
{
	int bx, by, bw, bh, sb;
	geom(id, t, &bx, &by, &bw, &bh, &sb);
	if (my < by || my >= by + bh)
		return -1;
	int v = ((int)t->scroll_y + (my - by)) / row_h();
	if (v < 0 || v >= t->nrows)
		return -1;
	return v;
}

/* Depth of the row just above v with a smaller depth = its parent row. */
static int
parent_row(struct tree *t, int v)
{
	int d = t->rows[v].depth;
	for (int i = v - 1; i >= 0; i--)
		if (t->rows[i].depth < d)
			return i;
	return -1;
}

static void
move_cursor(bd_id id, struct tree *t, int v)
{
	if (v < 0 || v >= t->nrows)
		return;
	t->cursor = v;
	t->sel = t->rows[v].key;
	fire_select(id, t);
	ensure_visible(id, t);
}

static int
sb_thumb_hit(bd_id id, struct tree *t, int mx, int my, int *ty, int *th_h)
{
	int bx, by, bw, bh, sb;
	geom(id, t, &bx, &by, &bw, &bh, &sb);
	if (!sb)
		return 0;
	int gx = bx + bw;
	if (mx < gx || mx >= gx + SBW || my < by || my >= by + bh)
		return 0;
	int content = t->nrows * row_h();
	float frac = (float)bh / (float)content;
	int hh = (int)(bh * frac);
	if (hh < 20)
		hh = 20;
	int max = content - bh;
	float pos = max > 0 ? t->scroll_y / max : 0;
	*ty = by + (int)((bh - hh) * pos);
	*th_h = hh;
	return 1;
}

static void
sb_to_pointer(bd_id id, struct tree *t, int my)
{
	int bx, by, bw, bh, sb;
	geom(id, t, &bx, &by, &bw, &bh, &sb);
	int content = t->nrows * row_h();
	float frac = (float)bh / (float)content;
	int hh = (int)(bh * frac);
	if (hh < 20)
		hh = 20;
	int track = bh - hh;
	int max = content - bh;
	float pos = track > 0 ? (my - by - t->drag_dy) / track : 0;
	if (pos < 0)
		pos = 0;
	if (pos > 1)
		pos = 1;
	t->scroll_y = pos * (max > 0 ? max : 0);
}

/* Type-ahead: jump to the next visible row whose label starts with `ch`. */
static void
type_ahead(bd_id id, struct tree *t, unsigned ch)
{
	if (ch < 32 || ch > 126)
		return;
	int lo = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
	int start = t->cursor < 0 ? 0 : t->cursor;
	for (int k = 1; k <= t->nrows; k++) {
		int v = (start + k) % t->nrows;
		bd_tree_item it = { .enabled = 1 };
		if (t->model.get)
			t->model.get(t->model.ctx, t->rows[v].key, &it);
		int c0 = it.label ? (unsigned char)it.label[0] : 0;
		if (c0 >= 'A' && c0 <= 'Z')
			c0 += 32;
		if (c0 == lo) {
			move_cursor(id, t, v);
			return;
		}
	}
}

static int
tree_event(bd_id id, void *state, const bd_event *ev)
{
	struct tree *t = state;

	switch (ev->type) {
	case BD_EV_MOUSE_SCROLL:
		t->scroll_y -= ev->scroll_dy * row_h() * 3;
		clamp_scroll(id, t);
		return 1;

	case BD_EV_MOUSE_DOWN: {
		int thy, thh;
		if (ev->button != BD_MOUSE_LEFT)
			return 0;

		if (sb_thumb_hit(id, t, ev->x, ev->y, &thy, &thh)) {
			if (ev->y >= thy && ev->y < thy + thh) {
				t->dragging_sb = 1;
				t->drag_dy = ev->y - thy;
			} else {
				t->scroll_y += (ev->y < thy ? -1 : 1) * (float)thh;
				clamp_scroll(id, t);
			}
			return 1;
		}

		int v = hit_row(id, t, ev->y);
		if (v < 0)
			return 1;

		int bx, by, bw, bh, sb;
		geom(id, t, &bx, &by, &bw, &bh, &sb);
		int tx = twisty_x(t, bx, t->rows[v].depth);
		int on_twisty = t->rows[v].has_children &&
		    ev->x >= tx && ev->x < tx + TWISTY_W;

		if (on_twisty) {
			toggle_expand(id, t, v);
			clamp_scroll(id, t);
			t->last_row = -1;
			return 1;
		}

		double now = bd_time();
		int dbl = (v == t->last_row && now - t->last_time < DBLCLICK_S);
		move_cursor(id, t, v);
		if (dbl) {
			if (t->rows[v].has_children) {
				toggle_expand(id, t, v);
				clamp_scroll(id, t);
			} else {
				activate(id, t, v);
			}
			t->last_row = -1;
		} else {
			t->last_row = v;
			t->last_time = now;
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
		int v = t->cursor;
		switch (ev->key) {
		case BD_KEY_UP:
			if (v > 0)
				move_cursor(id, t, v - 1);
			else if (v < 0 && t->nrows)
				move_cursor(id, t, 0);
			return 1;
		case BD_KEY_DOWN:
			if (v < 0 && t->nrows)
				move_cursor(id, t, 0);
			else if (v < t->nrows - 1)
				move_cursor(id, t, v + 1);
			return 1;
		case BD_KEY_HOME:
			if (t->nrows)
				move_cursor(id, t, 0);
			return 1;
		case BD_KEY_END:
			if (t->nrows)
				move_cursor(id, t, t->nrows - 1);
			return 1;
		case BD_KEY_LEFT:
			if (v >= 0) {
				if (t->rows[v].has_children && is_exp(t, t->rows[v].key)) {
					toggle_expand(id, t, v);
					clamp_scroll(id, t);
				} else {
					int p = parent_row(t, v);
					if (p >= 0)
						move_cursor(id, t, p);
				}
			}
			return 1;
		case BD_KEY_RIGHT:
			if (v >= 0 && t->rows[v].has_children) {
				if (!is_exp(t, t->rows[v].key)) {
					toggle_expand(id, t, v);
					clamp_scroll(id, t);
				} else if (v + 1 < t->nrows &&
				    t->rows[v + 1].depth > t->rows[v].depth) {
					move_cursor(id, t, v + 1);
				}
			}
			return 1;
		case BD_KEY_ENTER:
			if (v >= 0) {
				if (t->rows[v].has_children) {
					toggle_expand(id, t, v);
					clamp_scroll(id, t);
				} else {
					activate(id, t, v);
				}
			}
			return 1;
		default:
			if (ev->codepoint) {
				type_ahead(id, t, ev->codepoint);
				return 1;
			}
			return 0;
		}
	}

	default:
		return 0;
	}
}

/* ---- class ---- */

static void
tree_destroy(bd_id id, void *state)
{
	struct tree *t = state;
	(void)id;
	free(t->rows);
	free(t->exp);
}

static void
tree_layout(bd_id id, void *state, int w, int h)
{
	(void)w;
	(void)h;
	refresh_state(state);
	clamp_scroll(id, state);
}

static const bd_widget_class tree_class = {
	.name = "tree",
	.state_size = sizeof(struct tree),
	.destroy = tree_destroy,
	.render = tree_render,
	.layout = tree_layout,
	.event = tree_event,
};

bd_id
bd_tree_create(bd_id parent, const bd_tree_model *model,
               const bd_tree_cb *cb, ...)
{
	if (!tree_type)
		tree_type = bd_register_widget_class(&tree_class);

	va_list ap;
	va_start(ap, cb);
	bd_id id = bd_create_va(parent, tree_type, ap);
	va_end(ap);

	struct tree *t = bd_widget_state(id);
	if (!t)
		return id;
	if (model)
		t->model = *model;
	if (cb)
		t->cb = *cb;
	t->cursor = -1;
	t->indent = 16;
	t->last_row = -1;
	refresh_state(t);
	return id;
}

static struct tree *
tree_of(bd_id id)
{
	if (bd_widget_type(id) != tree_type)
		return NULL;
	return bd_widget_state(id);
}

void
bd_tree_refresh(bd_id id)
{
	struct tree *t = tree_of(id);
	if (t)
		refresh_state(t);
}

uint64_t
bd_tree_selected(bd_id id)
{
	struct tree *t = tree_of(id);
	return t ? t->sel : 0;
}

void
bd_tree_set_selected(bd_id id, uint64_t node)
{
	struct tree *t = tree_of(id);
	if (!t)
		return;
	t->sel = node;
	t->cursor = -1;
	for (int v = 0; v < t->nrows; v++)
		if (t->rows[v].key == node) {
			t->cursor = v;
			ensure_visible(id, t);
			break;
		}
}

void
bd_tree_set_expanded(bd_id id, uint64_t node, int open)
{
	struct tree *t = tree_of(id);
	if (!t)
		return;
	set_exp(t, node, open);
	refresh_state(t);
}

int
bd_tree_is_expanded(bd_id id, uint64_t node)
{
	struct tree *t = tree_of(id);
	return t ? is_exp(t, node) : 0;
}

void
bd_tree_set_indent(bd_id id, int px)
{
	struct tree *t = tree_of(id);
	if (t && px >= 0)
		t->indent = px;
}
