#include "bd_widget_explorer.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Explorer / icon-browser widget — skeleton.
 *
 * Implemented: model query + grid/free layout, rendering (recessed panel,
 * icons or a placeholder, labels, selection highlight, disabled dimming),
 * click selection (replace / Ctrl-toggle), double-click activate, right-click
 * context callback, wheel scroll, drag-move of the selection (committing via
 * model.set_pos + moved()), rubber-band rectangle selection (Ctrl = additive),
 * and the accessor API.
 *
 * TODO (the interaction still to fill in):
 *   - Shift+click range selection from the anchor
 *   - scissor-clip the scrolled content to the widget bounds
 *   - keyboard navigation (arrows / Enter / Ctrl-A); needs focus traversal
 *   - label truncation / ellipsis and a focus ring
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#define CELL_PAD     8       /* padding inside a cell, px */
#define DBLCLICK_S   0.40    /* max gap between clicks of a double-click */

struct explorer {
	bd_explorer_model model;
	bd_explorer_cb    cb;

	int      icon_size;      /* icon edge length, px */
	int      scroll_y;       /* content scroll offset, px */
	int      content_h;      /* last laid-out content height, px */

	uint64_t *sel;           /* selected keys */
	int       sel_n, sel_cap;
	uint64_t  anchor;        /* for shift-range selection (TODO) */

	/* double-click tracking */
	uint64_t  last_key;
	double    last_time;

	/* pointer drag state */
	int       mode;          /* DRAG_* */
	int       press_x, press_y;
	uint64_t  press_key;     /* item pressed (0 if empty space) */
	int       press_collapse;/* collapse multi-select to press_key on a click */

	/* drag-move: base content positions of the dragged (selected) items */
	uint64_t *drag_keys;
	int      *drag_bx, *drag_by;
	int       drag_n;
	int       drag_dx, drag_dy;   /* live offset from the press point */

	/* rubber-band: the selection that existed when the band started, so an
	 * additive (Ctrl) band can union with it each move */
	uint64_t *band_base;
	int       band_base_n;
	int       band_x0, band_y0, band_x1, band_y1;
};

enum { DRAG_NONE = 0, DRAG_PENDING, DRAG_MOVE, DRAG_BAND };

#define DRAG_THRESHOLD 4     /* px the pointer must move to start a drag */

static int explorer_type;

/* ------------------------------------------------------------------ */
/* selection set (keys)                                               */
/* ------------------------------------------------------------------ */

static int
sel_has(const struct explorer *e, uint64_t key)
{
	for (int i = 0; i < e->sel_n; i++)
		if (e->sel[i] == key)
			return 1;
	return 0;
}

static void
sel_clear(struct explorer *e)
{
	e->sel_n = 0;
}

static void
sel_add(struct explorer *e, uint64_t key)
{
	if (sel_has(e, key))
		return;
	if (e->sel_n == e->sel_cap) {
		int cap = e->sel_cap ? e->sel_cap * 2 : 8;
		uint64_t *p = realloc(e->sel, (size_t)cap * sizeof *p);
		if (!p)
			return;
		e->sel = p;
		e->sel_cap = cap;
	}
	e->sel[e->sel_n++] = key;
}

static void
sel_remove(struct explorer *e, uint64_t key)
{
	for (int i = 0; i < e->sel_n; i++)
		if (e->sel[i] == key) {
			e->sel[i] = e->sel[--e->sel_n];
			return;
		}
}

static void
sel_changed(bd_id id, struct explorer *e)
{
	if (e->cb.selection_changed)
		e->cb.selection_changed(id, e->cb.ctx);
}

/* ------------------------------------------------------------------ */
/* geometry                                                           */
/* ------------------------------------------------------------------ */

static int cell_w(const struct explorer *e) { return e->icon_size + 2 * CELL_PAD; }
static int
cell_h(const struct explorer *e)
{
	return e->icon_size + 2 * CELL_PAD + (int)bd_draw_line_height();
}

static int
columns(bd_id id, const struct explorer *e)
{
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	int cols = (w - 2 * CELL_PAD) / cell_w(e);
	return cols < 1 ? 1 : cols;
}

/* Index of `key` among the items currently being dragged, or -1. */
static int
drag_index(const struct explorer *e, uint64_t key)
{
	for (int i = 0; i < e->drag_n; i++)
		if (e->drag_keys[i] == key)
			return i;
	return -1;
}

/*
 * Content-space position (pre-scroll, relative to the widget origin) of an
 * item. Items with a saved position use it; others auto-place row-major, with
 * `slot` counting the auto-placed items seen so far.
 */
static void
item_content_pos(bd_id id, const struct explorer *e,
    const bd_explorer_item *item, int *slot, int *cx, int *cy)
{
	if (item->x >= 0 && item->y >= 0) {
		*cx = item->x;
		*cy = item->y;
	} else {
		int cols = columns(id, e);
		*cx = CELL_PAD + (*slot % cols) * cell_w(e);
		*cy = CELL_PAD + (*slot / cols) * cell_h(e);
		(*slot)++;
	}
}

/*
 * On-screen rect of an item. While a drag-move is in progress the dragged
 * items follow the pointer (base position + live offset) instead of their
 * model/auto position. `slot` advances for auto-placed items.
 */
static void
item_rect(bd_id id, const struct explorer *e, const bd_explorer_item *item,
    int *slot, int *rx, int *ry, int *rw, int *rh)
{
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	(void)w;
	int cx, cy;
	item_content_pos(id, e, item, slot, &cx, &cy);

	if (e->mode == DRAG_MOVE) {
		int d = drag_index(e, item->key);
		if (d >= 0) {
			cx = e->drag_bx[d] + e->drag_dx;
			cy = e->drag_by[d] + e->drag_dy;
		}
	}

	*rx = x + cx;
	*ry = y + cy - e->scroll_y;
	*rw = cell_w(e);
	*rh = cell_h(e);
}

/* Index of the item under (px,py), or -1. Fills *key on hit. */
static int
hit_item(bd_id id, struct explorer *e, int px, int py, uint64_t *key)
{
	int n = e->model.count ? e->model.count(e->model.ctx) : 0;
	int slot = 0;
	for (int i = 0; i < n; i++) {
		bd_explorer_item it = {0};
		e->model.get(e->model.ctx, i, &it);
		int rx, ry, rw, rh;
		item_rect(id, e, &it, &slot, &rx, &ry, &rw, &rh);
		if (px >= rx && px < rx + rw && py >= ry && py < ry + rh) {
			if (key)
				*key = it.key;
			return i;
		}
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* drag-move                                                          */
/* ------------------------------------------------------------------ */

static void
drag_free(struct explorer *e)
{
	free(e->drag_keys); e->drag_keys = NULL;
	free(e->drag_bx);   e->drag_bx = NULL;
	free(e->drag_by);   e->drag_by = NULL;
	e->drag_n = 0;
}

/* Snapshot the content positions of the selected items and enter move mode. */
static void
drag_begin(bd_id id, struct explorer *e)
{
	drag_free(e);
	if (e->sel_n == 0)
		return;
	e->drag_keys = malloc((size_t)e->sel_n * sizeof *e->drag_keys);
	e->drag_bx   = malloc((size_t)e->sel_n * sizeof *e->drag_bx);
	e->drag_by   = malloc((size_t)e->sel_n * sizeof *e->drag_by);
	if (!e->drag_keys || !e->drag_bx || !e->drag_by) {
		drag_free(e);
		return;
	}

	int n = e->model.count ? e->model.count(e->model.ctx) : 0;
	int slot = 0;
	e->drag_n = 0;
	for (int i = 0; i < n; i++) {
		bd_explorer_item it = {0};
		e->model.get(e->model.ctx, i, &it);
		int cx, cy;
		item_content_pos(id, e, &it, &slot, &cx, &cy);
		if (sel_has(e, it.key)) {
			e->drag_keys[e->drag_n] = it.key;
			e->drag_bx[e->drag_n] = cx;
			e->drag_by[e->drag_n] = cy;
			e->drag_n++;
		}
	}
	e->drag_dx = e->drag_dy = 0;
	e->mode = DRAG_MOVE;
}

/* Commit the moved positions to the model and notify, then leave move mode. */
static void
drag_commit(bd_id id, struct explorer *e)
{
	for (int i = 0; i < e->drag_n; i++) {
		int fx = e->drag_bx[i] + e->drag_dx;
		int fy = e->drag_by[i] + e->drag_dy;
		if (fx < 0) fx = 0;
		if (fy < 0) fy = 0;
		if (e->model.set_pos)
			e->model.set_pos(e->model.ctx, e->drag_keys[i], fx, fy);
		if (e->cb.moved)
			e->cb.moved(id, e->drag_keys[i], fx, fy, e->cb.ctx);
	}
	drag_free(e);
}

/* ------------------------------------------------------------------ */
/* rubber-band                                                        */
/* ------------------------------------------------------------------ */

static int
rects_overlap(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh)
{
	return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

/* The band as a normalized rect (x0,y0 may be below/right of x1,y1). */
static void
band_rect(const struct explorer *e, int *rx, int *ry, int *rw, int *rh)
{
	int x0 = e->band_x0 < e->band_x1 ? e->band_x0 : e->band_x1;
	int y0 = e->band_y0 < e->band_y1 ? e->band_y0 : e->band_y1;
	int x1 = e->band_x0 > e->band_x1 ? e->band_x0 : e->band_x1;
	int y1 = e->band_y0 > e->band_y1 ? e->band_y0 : e->band_y1;
	*rx = x0; *ry = y0; *rw = x1 - x0; *rh = y1 - y0;
}

static void
band_free(struct explorer *e)
{
	free(e->band_base);
	e->band_base = NULL;
	e->band_base_n = 0;
}

/* Enter band mode; snapshot the current selection for an additive band. */
static void
band_begin(struct explorer *e, int px, int py, int additive)
{
	band_free(e);
	if (additive && e->sel_n > 0) {
		e->band_base = malloc((size_t)e->sel_n * sizeof *e->band_base);
		if (e->band_base) {
			memcpy(e->band_base, e->sel,
			    (size_t)e->sel_n * sizeof *e->band_base);
			e->band_base_n = e->sel_n;
		}
	}
	e->band_x0 = e->band_x1 = px;
	e->band_y0 = e->band_y1 = py;
	e->mode = DRAG_BAND;
}

/* Recompute the selection as the snapshot plus every item the band covers. */
static void
band_update(bd_id id, struct explorer *e, int px, int py)
{
	e->band_x1 = px;
	e->band_y1 = py;

	int bx, by, bw, bh;
	band_rect(e, &bx, &by, &bw, &bh);

	sel_clear(e);
	for (int i = 0; i < e->band_base_n; i++)
		sel_add(e, e->band_base[i]);

	int n = e->model.count ? e->model.count(e->model.ctx) : 0;
	int slot = 0;
	for (int i = 0; i < n; i++) {
		bd_explorer_item it = {0};
		e->model.get(e->model.ctx, i, &it);
		int rx, ry, rw, rh;
		item_rect(id, e, &it, &slot, &rx, &ry, &rw, &rh);
		if (rects_overlap(bx, by, bw, bh, rx, ry, rw, rh))
			sel_add(e, it.key);
	}
}

/* ------------------------------------------------------------------ */
/* class hooks                                                        */
/* ------------------------------------------------------------------ */

static void
explorer_init(bd_id id, void *state)
{
	struct explorer *e = state;
	e->icon_size = 48;
	bd_set(id, BD_PREF_W_I, 240, BD_PREF_H_I, 200, BD_END);
}

static void
explorer_destroy(bd_id id, void *state)
{
	(void)id;
	struct explorer *e = state;
	free(e->sel);
	e->sel = NULL;
	e->sel_n = e->sel_cap = 0;
	drag_free(e);
	band_free(e);
}

static void
explorer_layout(bd_id id, void *state, int w, int h)
{
	(void)w; (void)h;
	struct explorer *e = state;
	int n = e->model.count ? e->model.count(e->model.ctx) : 0;
	int cols = columns(id, e);
	int rows = (n + cols - 1) / cols;
	e->content_h = CELL_PAD + rows * cell_h(e);
	/* clamp scroll to content (TODO: account for saved free positions) */
	int x, y, vw, vh;
	bd_widget_rect(id, &x, &y, &vw, &vh);
	int max_scroll = e->content_h - vh;
	if (max_scroll < 0)
		max_scroll = 0;
	if (e->scroll_y > max_scroll)
		e->scroll_y = max_scroll;
	if (e->scroll_y < 0)
		e->scroll_y = 0;
}

static void
explorer_render(bd_id id, void *state)
{
	struct explorer *e = state;
	const bd_theme *th = bd_gui_theme();
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);

	/* recessed panel + outline */
	bd_draw_rect(x, y, w, h, th->press);
	bd_draw_rect_lines(x, y, w, h, th->border);

	/* TODO: scissor to (x,y,w,h) so scrolled cells clip at the edges */

	int n = e->model.count ? e->model.count(e->model.ctx) : 0;
	int slot = 0;
	for (int i = 0; i < n; i++) {
		bd_explorer_item it = {0};
		e->model.get(e->model.ctx, i, &it);
		int rx, ry, rw, rh;
		item_rect(id, e, &it, &slot, &rx, &ry, &rw, &rh);
		if (ry + rh < y || ry > y + h)   /* cheap vertical cull */
			continue;

		int selected = sel_has(e, it.key);
		if (selected)
			bd_draw_rect(rx, ry, rw, rh, th->select);

		int ix = rx + (rw - e->icon_size) / 2;
		int iy = ry + CELL_PAD;
		uint32_t tint = it.enabled ? 0xFFFFFFFFu : 0xFFFFFF80u; /* dim if disabled */
		if (it.icon.id)
			bd_draw_sprite(it.icon, ix, iy, e->icon_size, e->icon_size,
			    0, 0, 1, 1, tint);
		else {
			/* placeholder icon */
			bd_draw_rect(ix, iy, e->icon_size, e->icon_size, th->widget);
			bd_draw_rect_lines(ix, iy, e->icon_size, e->icon_size, th->border);
		}

		if (it.label) {
			float tw = bd_draw_text_width(it.label);
			float tx = rx + (rw - tw) / 2.0f;
			if (tx < rx)
				tx = rx;                     /* TODO: ellipsize */
			float ty = iy + e->icon_size + 2;
			uint32_t fg = it.enabled ? th->text : th->border;
			bd_draw_text(it.label, tx, ty, fg);
		}
	}

	/* rubber-band overlay */
	if (e->mode == DRAG_BAND) {
		int bx, by, bw, bh;
		band_rect(e, &bx, &by, &bw, &bh);
		uint32_t fill = (th->focus & 0xFFFFFF00u) | 0x40u; /* translucent */
		bd_draw_rect(bx, by, bw, bh, fill);
		bd_draw_rect_lines(bx, by, bw, bh, th->focus);
	}
}

/* ------------------------------------------------------------------ */
/* events                                                             */
/* ------------------------------------------------------------------ */

static int
explorer_event(bd_id id, void *state, const bd_event *ev)
{
	struct explorer *e = state;

	switch (ev->type) {
	case BD_EV_MOUSE_SCROLL:
		e->scroll_y -= (int)(ev->scroll_dy * (float)cell_h(e));
		if (e->scroll_y < 0)
			e->scroll_y = 0;
		return 1;

	case BD_EV_MOUSE_DOWN: {
		uint64_t key = 0;
		int idx = hit_item(id, e, ev->x, ev->y, &key);

		if (ev->button == BD_MOUSE_RIGHT) {
			if (idx >= 0) {
				if (!sel_has(e, key)) {
					sel_clear(e);
					sel_add(e, key);
					sel_changed(id, e);
				}
				if (e->cb.context)
					e->cb.context(id, key, ev->x, ev->y, e->cb.ctx);
			}
			return 1;
		}

		if (ev->button != BD_MOUSE_LEFT)
			return 0;

		e->press_x = ev->x;
		e->press_y = ev->y;
		e->press_key = (idx >= 0) ? key : 0;
		e->press_collapse = 0;

		if (idx < 0) {
			/* empty space: start a rubber-band (Ctrl = additive) */
			int additive = (ev->mods & BD_MOD_CTRL) != 0;
			if (!additive && e->sel_n) {
				sel_clear(e);
				sel_changed(id, e);
			}
			band_begin(e, ev->x, ev->y, additive);
			return 1;
		}

		/* double-click activates */
		double now = bd_backend_get()->time();
		if (key == e->last_key && now - e->last_time < DBLCLICK_S) {
			if (e->cb.activate) {
				bd_explorer_item it = {0};
				e->model.get(e->model.ctx, idx, &it);
				e->cb.activate(id, key, it.user);
			}
			e->last_key = 0;
			e->mode = DRAG_NONE;
			return 1;
		}
		e->last_key = key;
		e->last_time = now;

		if (ev->mods & BD_MOD_CTRL) {
			if (sel_has(e, key))
				sel_remove(e, key);
			else
				sel_add(e, key);
			sel_changed(id, e);
		} else if (ev->mods & BD_MOD_SHIFT) {
			/* TODO: range-select from anchor to this item */
			sel_add(e, key);
			sel_changed(id, e);
		} else if (!sel_has(e, key)) {
			sel_clear(e);
			sel_add(e, key);
			sel_changed(id, e);
		} else if (e->sel_n > 1) {
			/* pressed an already-selected item in a multi-selection: keep
			 * the group so a drag can move it, but collapse to just this
			 * item if the press turns out to be a plain click */
			e->press_collapse = 1;
		}
		e->anchor = key;
		e->mode = DRAG_PENDING;
		return 1;
	}

	case BD_EV_MOUSE_MOVE:
		if (e->mode == DRAG_PENDING) {
			int dx = ev->x - e->press_x, dy = ev->y - e->press_y;
			if (dx <= -DRAG_THRESHOLD || dx >= DRAG_THRESHOLD ||
			    dy <= -DRAG_THRESHOLD || dy >= DRAG_THRESHOLD)
				drag_begin(id, e);   /* -> DRAG_MOVE */
		}
		if (e->mode == DRAG_MOVE) {
			e->drag_dx = ev->x - e->press_x;
			e->drag_dy = ev->y - e->press_y;
			return 1;
		}
		if (e->mode == DRAG_BAND) {
			band_update(id, e, ev->x, ev->y);
			return 1;
		}
		return 0;

	case BD_EV_MOUSE_UP: {
		int was = e->mode;
		e->mode = DRAG_NONE;
		if (was == DRAG_MOVE) {
			drag_commit(id, e);
		} else if (was == DRAG_BAND) {
			band_free(e);
			sel_changed(id, e);
		} else if (was == DRAG_PENDING && e->press_collapse) {
			/* plain click on a selected item: reduce to just it */
			sel_clear(e);
			sel_add(e, e->press_key);
			sel_changed(id, e);
		}
		e->press_collapse = 0;
		return was != DRAG_NONE;
	}

	default:
		return 0;
	}
}

static const bd_widget_class explorer_class = {
	.name = "explorer",
	.state_size = sizeof(struct explorer),
	.init = explorer_init,
	.destroy = explorer_destroy,
	.render = explorer_render,
	.layout = explorer_layout,
	.event = explorer_event,
};

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

bd_id
bd_explorer_create(bd_id parent, const bd_explorer_model *model,
    const bd_explorer_cb *cb, ...)
{
	if (explorer_type == 0)
		explorer_type = bd_register_widget_class(&explorer_class);

	va_list ap;
	va_start(ap, cb);
	bd_id id = bd_create_va(parent, explorer_type, ap);
	va_end(ap);

	struct explorer *e = bd_widget_state(id);
	if (e) {
		if (model)
			e->model = *model;
		if (cb)
			e->cb = *cb;
	}
	return id;
}

void
bd_explorer_refresh(bd_id id)
{
	if (bd_widget_type(id) != explorer_type)
		return;
	struct explorer *e = bd_widget_state(id);
	if (!e || !e->model.count)
		return;

	/* drop selected keys that no longer exist in the model */
	int n = e->model.count(e->model.ctx);
	for (int i = e->sel_n - 1; i >= 0; i--) {
		uint64_t key = e->sel[i];
		int found = 0;
		for (int j = 0; j < n && !found; j++) {
			bd_explorer_item it = {0};
			e->model.get(e->model.ctx, j, &it);
			found = (it.key == key);
		}
		if (!found)
			sel_remove(e, key);
	}
}

int
bd_explorer_selection(bd_id id, uint64_t *keys, int max)
{
	if (bd_widget_type(id) != explorer_type)
		return 0;
	struct explorer *e = bd_widget_state(id);
	if (!e)
		return 0;
	int copy = e->sel_n < max ? e->sel_n : max;
	if (keys)
		memcpy(keys, e->sel, (size_t)copy * sizeof *keys);
	return e->sel_n;
}

void
bd_explorer_select(bd_id id, uint64_t key, int add)
{
	if (bd_widget_type(id) != explorer_type)
		return;
	struct explorer *e = bd_widget_state(id);
	if (!e)
		return;
	if (!add)
		sel_clear(e);
	sel_add(e, key);
	sel_changed(id, e);
}

void
bd_explorer_set_icon_size(bd_id id, int px)
{
	if (bd_widget_type(id) != explorer_type)
		return;
	struct explorer *e = bd_widget_state(id);
	if (e && px > 0)
		e->icon_size = px;
}
