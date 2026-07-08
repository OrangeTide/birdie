#include "bd_widget_inventory.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Inventory grid widget. A scrollable cols x rows grid of icon slots with
 * captions, for an RPG bag. See bd_widget_inventory.h for the design.
 *
 * Implemented: fixed-grid layout with vertical scroll (wheel), per-slot model
 * query (visible slots only), rendering (recessed panel, slotted cells, icon
 * sprite or empty frame, ellipsized label, stack-count badge, selection
 * highlight, keyboard focus ring, disabled dimming), click selection
 * (replace / Ctrl-toggle / Shift-range), double-click / Enter activate,
 * right-click context callback, drag-and-drop between slots (ghost + drop
 * ring, committing via the move() callback), keyboard navigation
 * (arrows / Home / End / Enter, Shift extends), and a dwell tooltip. Selection
 * is keyed on slot index. The scrolling content is scissor-clipped to the
 * panel interior; the tooltip draws over the top.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#define CELL_PAD       8      /* padding inside a cell, px */
#define DBLCLICK_S     0.40   /* max gap between clicks of a double-click */
#define DRAG_THRESHOLD 4      /* px the pointer must move to start a drag */
#define TOOLTIP_DELAY  0.6    /* hover dwell before the tooltip shows, s */

enum { DRAG_NONE = 0, DRAG_PENDING, DRAG_ITEM };

struct inv {
	bd_inventory_model model;
	bd_inventory_cb    cb;

	int cols, rows;          /* grid capacity */
	int icon_size;           /* icon edge length, px */
	int scroll_y;            /* content scroll offset, px */
	int content_h;           /* last laid-out content height, px */

	unsigned char *sel;      /* selection bitset, one bit per slot */
	int  sel_slots;          /* slots the bitset is sized for (cols*rows) */
	int  anchor;             /* shift-range origin slot, or -1 */
	int  cursor;             /* keyboard-nav focus slot, or -1 */

	/* double-click tracking */
	int    last_slot;
	double last_time;

	/* pointer state */
	int mode;                /* DRAG_* */
	int press_x, press_y;
	int press_slot;          /* slot the drag started on, or -1 */
	int drop_slot;           /* slot under the pointer during a drag */
	int mouse_x, mouse_y;    /* last pointer position */

	/* hover / tooltip */
	int    hover_slot;
	double hover_since;
};

static int inv_type;

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static double be_time(void) { return bd_backend_get()->time(); }

static int capacity(const struct inv *v) { return v->cols * v->rows; }
static int cell_w(const struct inv *v)   { return v->icon_size + 2 * CELL_PAD; }
static int
cell_h(const struct inv *v)
{
	return v->icon_size + 2 * CELL_PAD + (int)bd_draw_line_height();
}

/* selection bitset */
static int  sel_get(const struct inv *v, int s)
{ return (s >= 0 && s < v->sel_slots) ? (v->sel[s >> 3] >> (s & 7)) & 1 : 0; }
static void sel_set(struct inv *v, int s, int on)
{
	if (s < 0 || s >= v->sel_slots) return;
	if (on) v->sel[s >> 3] |=  (unsigned char)(1u << (s & 7));
	else    v->sel[s >> 3] &= (unsigned char)~(1u << (s & 7));
}
static void sel_clear_all(struct inv *v)
{ if (v->sel) memset(v->sel, 0, (size_t)((v->sel_slots + 7) / 8)); }
static void
sel_range(struct inv *v, int a, int b)
{
	sel_clear_all(v);
	if (a > b) { int t = a; a = b; b = t; }
	for (int s = a; s <= b; s++) sel_set(v, s, 1);
}

static void
sel_resize(struct inv *v, int slots)
{
	int bytes = (slots + 7) / 8;
	unsigned char *n = calloc(1, (size_t)(bytes > 0 ? bytes : 1));
	free(v->sel);
	v->sel = n;
	v->sel_slots = slots;
}

static void
sel_changed(bd_id id, struct inv *v)
{
	if (v->cb.selection_changed)
		v->cb.selection_changed(id, v->cb.ctx);
}

/* fetch a slot's item (zeroed for empty / no model) */
static void
slot_item(const struct inv *v, int slot, bd_inventory_item *out)
{
	memset(out, 0, sizeof *out);
	if (v->model.get && slot >= 0 && slot < capacity(v))
		v->model.get(v->model.ctx, slot, out);
}
static int
slot_filled(const struct inv *v, int slot)
{
	bd_inventory_item it;
	slot_item(v, slot, &it);
	return it.icon.id != 0 || (it.label && it.label[0]);
}
static uint64_t
slot_key(const struct inv *v, int slot)
{
	bd_inventory_item it;
	slot_item(v, slot, &it);
	return it.key;
}

/* slot under a point, or -1 */
static int
hit_slot(bd_id id, const struct inv *v, int px, int py)
{
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	int lx = px - x - CELL_PAD;
	int ly = py - y - CELL_PAD + v->scroll_y;
	if (px < x || px >= x + w || py < y || py >= y + h)
		return -1;
	if (lx < 0 || ly < 0)
		return -1;
	int col = lx / cell_w(v);
	int row = ly / cell_h(v);
	if (col >= v->cols || row >= v->rows)
		return -1;
	int slot = row * v->cols + col;
	return slot < capacity(v) ? slot : -1;
}

/* scroll so `slot`'s row is fully visible */
static void
ensure_visible(bd_id id, struct inv *v, int slot)
{
	if (slot < 0) return;
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	int row = slot / v->cols;
	int cy = CELL_PAD + row * cell_h(v);
	if (cy < v->scroll_y)
		v->scroll_y = cy;
	else if (cy + cell_h(v) > v->scroll_y + h)
		v->scroll_y = cy + cell_h(v) - h;
	if (v->scroll_y < 0) v->scroll_y = 0;
}

/* preferred width follows the column count and cell size */
static void
update_pref_w(bd_id id, const struct inv *v)
{
	bd_set(id, BD_PREF_W_I, 2 * CELL_PAD + v->cols * cell_w(v), BD_END);
}

/* ------------------------------------------------------------------ */
/* class hooks                                                        */
/* ------------------------------------------------------------------ */

static void
inv_init(bd_id id, void *state)
{
	struct inv *v = state;
	v->cols = v->rows = 1;
	v->icon_size = 48;
	v->anchor = v->cursor = -1;
	v->last_slot = -1;
	v->press_slot = v->drop_slot = -1;
	v->hover_slot = -1;
	bd_set(id, BD_PREF_H_I, 220, BD_END);
}

static void
inv_destroy(bd_id id, void *state)
{
	(void)id;
	struct inv *v = state;
	free(v->sel);
	v->sel = NULL;
}

static void
inv_layout(bd_id id, void *state, int w, int h)
{
	(void)w; (void)h;
	struct inv *v = state;
	v->content_h = 2 * CELL_PAD + v->rows * cell_h(v);
	int x, y, vw, vh;
	bd_widget_rect(id, &x, &y, &vw, &vh);
	int max_scroll = v->content_h - vh;
	if (max_scroll < 0) max_scroll = 0;
	if (v->scroll_y > max_scroll) v->scroll_y = max_scroll;
	if (v->scroll_y < 0) v->scroll_y = 0;
}

static void
inv_render(bd_id id, void *state)
{
	struct inv *v = state;
	const bd_theme *th = bd_gui_theme();
	const bd_backend *be = bd_backend_get();
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);

	/* recessed panel + outline, before the clip so the border stays crisp */
	bd_draw_rect(x, y, w, h, th->press);
	bd_draw_rect_lines(x, y, w, h, th->border);

	bd_draw_flush();
	be->scissor(x + 1, y + 1, w - 2, h - 2);

	int cw = cell_w(v), ch = cell_h(v), is = v->icon_size, cap = capacity(v);

	int first_row = (v->scroll_y - CELL_PAD) / ch;
	if (first_row < 0) first_row = 0;
	int last_row = (v->scroll_y + h) / ch + 1;
	if (last_row > v->rows) last_row = v->rows;

	for (int row = first_row; row < last_row; row++) {
		for (int col = 0; col < v->cols; col++) {
			int slot = row * v->cols + col;
			if (slot >= cap) break;
			int rx = x + CELL_PAD + col * cw;
			int ry = y + CELL_PAD + row * ch - v->scroll_y;

			if (sel_get(v, slot))
				bd_draw_rect(rx, ry, cw, ch, th->select);

			int ix = rx + CELL_PAD, iy = ry + CELL_PAD;

			bd_inventory_item it;
			slot_item(v, slot, &it);

			/* the shared tile: recessed square + icon + badge + caption
			 * (identical to the dock widget) */
			bd_draw_tile(rx, ry, cw, CELL_PAD, is, it.icon, it.label,
			    it.count, it.enabled, th->bg, th->border, th->text);

			/* drop-target ring during a drag-move */
			if (v->mode == DRAG_ITEM && slot == v->drop_slot &&
			    slot != v->press_slot)
				bd_draw_rect_lines(ix - 1, iy - 1, is + 2, is + 2, th->focus);

			if (slot == v->cursor)
				bd_draw_rect_lines(rx, ry, cw, ch, th->focus);
		}
	}

	/* ghost of the dragged item, following the pointer */
	if (v->mode == DRAG_ITEM && v->press_slot >= 0) {
		bd_inventory_item it;
		slot_item(v, v->press_slot, &it);
		if (it.icon.id)
			bd_draw_sprite(it.icon, v->mouse_x - is / 2, v->mouse_y - is / 2,
			    is, is, 0, 0, 1, 1, 0xFFFFFFB0u);
	}

	bd_draw_flush();
	be->scissor_off();

	/* dwell tooltip, drawn unclipped so it can overhang the panel */
	if (v->hover_slot >= 0 && v->mode == DRAG_NONE &&
	    be_time() - v->hover_since > TOOLTIP_DELAY) {
		bd_inventory_item it;
		slot_item(v, v->hover_slot, &it);
		const char *tip = it.tooltip ? it.tooltip : it.label;
		if (tip && tip[0] && (it.icon.id || it.label)) {
			float lh = bd_draw_line_height();
			float tw = bd_draw_text_width(tip);
			int pad = 4;
			int bw = (int)tw + 2 * pad, bh = (int)lh + 2 * pad;
			int bx = v->mouse_x + 12, by = v->mouse_y + 16;
			if (bx + bw > bd_draw_win_w()) bx = bd_draw_win_w() - bw;
			if (by + bh > bd_draw_win_h()) by = v->mouse_y - bh - 4;
			if (bx < 0) bx = 0;
			if (by < 0) by = 0;
			bd_draw_rect(bx, by, bw, bh, th->panel);
			bd_draw_rect_lines(bx, by, bw, bh, th->focus);
			bd_draw_text(tip, bx + pad, by + pad, th->text_hi);
		}
	}
}

/* ------------------------------------------------------------------ */
/* events                                                             */
/* ------------------------------------------------------------------ */

static void
select_click(bd_id id, struct inv *v, int slot, int mods)
{
	if (mods & BD_MOD_CTRL) {
		sel_set(v, slot, !sel_get(v, slot));
		v->anchor = v->cursor = slot;
		sel_changed(id, v);
	} else if ((mods & BD_MOD_SHIFT) && v->anchor >= 0) {
		sel_range(v, v->anchor, slot);
		v->cursor = slot;
		sel_changed(id, v);
	} else {
		if (!sel_get(v, slot)) {
			sel_clear_all(v);
			sel_set(v, slot, 1);
			sel_changed(id, v);
		}
		v->anchor = v->cursor = slot;
	}
}

static int
inv_key(bd_id id, struct inv *v, const bd_event *ev)
{
	int cap = capacity(v);
	int cur = v->cursor < 0 ? 0 : v->cursor;
	int nxt = cur;

	switch (ev->key) {
	case BD_KEY_LEFT:  nxt = cur - 1; break;
	case BD_KEY_RIGHT: nxt = cur + 1; break;
	case BD_KEY_UP:    nxt = cur - v->cols; break;
	case BD_KEY_DOWN:  nxt = cur + v->cols; break;
	case BD_KEY_HOME:  nxt = 0; break;
	case BD_KEY_END:   nxt = cap - 1; break;
	case BD_KEY_ENTER:
		if (v->cursor >= 0 && slot_filled(v, v->cursor) && v->cb.activate)
			v->cb.activate(id, v->cursor, slot_key(v, v->cursor), v->cb.ctx);
		return 1;
	default:
		return 0;
	}
	if (nxt < 0 || nxt >= cap)
		return 1;   /* consumed, but at the edge */
	v->cursor = nxt;
	if (ev->mods & BD_MOD_SHIFT)
		sel_range(v, v->anchor >= 0 ? v->anchor : nxt, nxt);
	else {
		sel_clear_all(v);
		sel_set(v, nxt, 1);
		v->anchor = nxt;
	}
	sel_changed(id, v);
	ensure_visible(id, v, nxt);
	return 1;
}

static int
inv_event(bd_id id, void *state, const bd_event *ev)
{
	struct inv *v = state;

	switch (ev->type) {
	case BD_EV_MOUSE_SCROLL:
		v->scroll_y -= (int)(ev->scroll_dy * (float)cell_h(v));
		if (v->scroll_y < 0) v->scroll_y = 0;
		return 1;

	case BD_EV_MOUSE_MOVE:
		v->mouse_x = ev->x;
		v->mouse_y = ev->y;
		if (v->mode == DRAG_PENDING &&
		    (abs(ev->x - v->press_x) > DRAG_THRESHOLD ||
		     abs(ev->y - v->press_y) > DRAG_THRESHOLD))
			v->mode = DRAG_ITEM;
		if (v->mode == DRAG_ITEM) {
			v->drop_slot = hit_slot(id, v, ev->x, ev->y);
			return 1;
		}
		/* hover tracking (moves arrive via the class wants_hover flag) */
		{
			int hs = hit_slot(id, v, ev->x, ev->y);
			if (hs != v->hover_slot) {
				v->hover_slot = hs;
				v->hover_since = be_time();
				if (v->cb.hover)
					v->cb.hover(id, hs, hs >= 0 ? slot_key(v, hs) : 0,
					    v->cb.ctx);
			}
		}
		return 0;

	case BD_EV_MOUSE_DOWN: {
		int slot = hit_slot(id, v, ev->x, ev->y);

		if (ev->button == BD_MOUSE_RIGHT) {
			if (slot >= 0 && slot_filled(v, slot)) {
				if (!sel_get(v, slot)) {
					sel_clear_all(v);
					sel_set(v, slot, 1);
					sel_changed(id, v);
				}
				v->cursor = slot;
				if (v->cb.context)
					v->cb.context(id, slot, slot_key(v, slot),
					    ev->x, ev->y, v->cb.ctx);
			}
			return 1;
		}
		if (ev->button != BD_MOUSE_LEFT)
			return 0;

		v->mode = DRAG_NONE;
		v->press_slot = -1;

		if (slot < 0 || !slot_filled(v, slot)) {
			if (!(ev->mods & (BD_MOD_CTRL | BD_MOD_SHIFT))) {
				sel_clear_all(v);
				sel_changed(id, v);
			}
			v->cursor = slot >= 0 ? slot : -1;
			return 1;
		}

		select_click(id, v, slot, ev->mods);

		double now = be_time();
		if (slot == v->last_slot && now - v->last_time < DBLCLICK_S) {
			if (v->cb.activate)
				v->cb.activate(id, slot, slot_key(v, slot), v->cb.ctx);
			v->last_slot = -1;
		} else {
			v->last_slot = slot;
			v->last_time = now;
		}

		/* arm a possible drag from this slot */
		if (!(ev->mods & (BD_MOD_CTRL | BD_MOD_SHIFT))) {
			v->mode = DRAG_PENDING;
			v->press_slot = slot;
			v->press_x = ev->x;
			v->press_y = ev->y;
			v->drop_slot = slot;
		}
		return 1;
	}

	case BD_EV_MOUSE_UP:
		if (ev->button != BD_MOUSE_LEFT)
			return 0;
		if (v->mode == DRAG_ITEM && v->press_slot >= 0) {
			int to = hit_slot(id, v, ev->x, ev->y);
			if (to >= 0 && to != v->press_slot && v->cb.move) {
				v->cb.move(id, v->press_slot, to, v->cb.ctx);
				sel_clear_all(v);
				sel_set(v, to, 1);
				v->anchor = v->cursor = to;
				sel_changed(id, v);
			}
		}
		v->mode = DRAG_NONE;
		v->press_slot = v->drop_slot = -1;
		return 1;

	case BD_EV_KEY_DOWN:
		return inv_key(id, v, ev);

	default:
		return 0;
	}
}

static const bd_widget_class inv_class = {
	.name        = "inventory",
	.state_size  = sizeof(struct inv),
	.init        = inv_init,
	.destroy     = inv_destroy,
	.render      = inv_render,
	.layout      = inv_layout,
	.event       = inv_event,
	.flags       = BD_WC_WANTS_HOVER,
};

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

bd_id
bd_inventory_create(bd_id parent, int cols, int rows,
    const bd_inventory_model *model, const bd_inventory_cb *cb, ...)
{
	if (inv_type == 0)
		inv_type = bd_register_widget_class(&inv_class);

	va_list ap;
	va_start(ap, cb);
	bd_id id = bd_create_va(parent, inv_type, ap);
	va_end(ap);

	struct inv *v = bd_widget_state(id);
	if (!v)
		return id;
	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;
	v->cols = cols;
	v->rows = rows;
	sel_resize(v, cols * rows);
	if (model) v->model = *model;
	if (cb)    v->cb = *cb;
	update_pref_w(id, v);
	return id;
}

void
bd_inventory_set_dims(bd_id id, int cols, int rows)
{
	struct inv *v = bd_widget_state(id);
	if (!v) return;
	if (cols < 1) cols = 1;
	if (rows < 1) rows = 1;
	v->cols = cols;
	v->rows = rows;
	sel_resize(v, cols * rows);
	v->anchor = v->cursor = -1;
	v->hover_slot = v->last_slot = -1;
	v->scroll_y = 0;
	update_pref_w(id, v);
}

void
bd_inventory_set_cell_size(bd_id id, int px)
{
	struct inv *v = bd_widget_state(id);
	if (!v || px < 8) return;
	v->icon_size = px;
	update_pref_w(id, v);
}

void
bd_inventory_refresh(bd_id id)
{
	struct inv *v = bd_widget_state(id);
	if (!v) return;
	v->hover_slot = -1;
}

int bd_inventory_cols(bd_id id)
{ struct inv *v = bd_widget_state(id); return v ? v->cols : 0; }
int bd_inventory_rows(bd_id id)
{ struct inv *v = bd_widget_state(id); return v ? v->rows : 0; }

int
bd_inventory_selected(bd_id id)
{
	struct inv *v = bd_widget_state(id);
	if (!v) return -1;
	for (int s = 0; s < v->sel_slots; s++)
		if (sel_get(v, s)) return s;
	return -1;
}

int
bd_inventory_selection(bd_id id, int *slots, int max)
{
	struct inv *v = bd_widget_state(id);
	if (!v) return 0;
	int n = 0;
	for (int s = 0; s < v->sel_slots; s++)
		if (sel_get(v, s)) {
			if (slots && n < max) slots[n] = s;
			n++;
		}
	return n;
}

void
bd_inventory_select(bd_id id, int slot, int add)
{
	struct inv *v = bd_widget_state(id);
	if (!v || slot < 0 || slot >= capacity(v)) return;
	if (!add) sel_clear_all(v);
	sel_set(v, slot, 1);
	v->anchor = v->cursor = slot;
	sel_changed(id, v);
}
