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
 * context callback, wheel scroll, and the accessor API.
 *
 * TODO (the interaction still to fill in):
 *   - drag-move of the selection, committing via model.set_pos + moved()
 *   - rubber-band rectangle selection (with Ctrl = additive)
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

	/* drag state (TODO) */
	int       pressing;      /* a button is down inside the widget */
	int       press_x, press_y;
};

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

/*
 * On-screen rect of item `index` (with `item` already fetched). Items with a
 * saved position use it (content coords); others auto-place row-major. `slot`
 * counts auto-placed items seen so far and is advanced for each.
 */
static void
item_rect(bd_id id, const struct explorer *e, const bd_explorer_item *item,
    int *slot, int *rx, int *ry, int *rw, int *rh)
{
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	(void)w;
	int cx, cy;
	if (item->x >= 0 && item->y >= 0) {
		cx = item->x;
		cy = item->y;
	} else {
		int cols = columns(id, e);
		cx = CELL_PAD + (*slot % cols) * cell_w(e);
		cy = CELL_PAD + (*slot / cols) * cell_h(e);
		(*slot)++;
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

	/* TODO: rubber-band rectangle and drag ghost overlays */
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

		e->pressing = 1;
		e->press_x = ev->x;
		e->press_y = ev->y;

		if (idx < 0) {
			/* TODO: begin a rubber-band selection here */
			if (!(ev->mods & (BD_MOD_CTRL | BD_MOD_SHIFT))) {
				sel_clear(e);
				sel_changed(id, e);
			}
			return 1;
		}

		/* double-click activates */
		double now = bd_backend_get()->time();
		if (key == e->last_key && now - e->last_time < DBLCLICK_S) {
			if (e->cb.activate)
				e->cb.activate(id, key, NULL); /* TODO: pass item.user */
			e->last_key = 0;
			return 1;
		}
		e->last_key = key;
		e->last_time = now;

		if (ev->mods & BD_MOD_CTRL) {
			if (sel_has(e, key))
				sel_remove(e, key);
			else
				sel_add(e, key);
		} else if (ev->mods & BD_MOD_SHIFT) {
			/* TODO: range-select from anchor to this item */
			sel_add(e, key);
		} else if (!sel_has(e, key)) {
			sel_clear(e);
			sel_add(e, key);
		}
		e->anchor = key;
		sel_changed(id, e);
		/* TODO: arm a drag-move if the pointer leaves a small threshold */
		return 1;
	}

	case BD_EV_MOUSE_MOVE:
		/* TODO: update rubber-band, or move the selection if dragging */
		return e->pressing;

	case BD_EV_MOUSE_UP:
		/* TODO: commit a drag-move via model.set_pos + moved(), or finish
		 * the rubber-band selection */
		e->pressing = 0;
		return 1;

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
