/*
 * bd_widget_dock -- a NeXTSTEP / WindowMaker-style dock. See bd_widget_dock.h
 * and doc/gui/dock-design.md.
 *
 * The tile set is derived, not owned: each frame -> tile projection of the WM's
 * minimized-window set (bd_window_list + bd_window_minimized), rebuilt every
 * layout/render. The dock sizes itself to its tiles and anchors to its gravity
 * edge via BD_ANCHOR_I, so a BD_LAYOUT_FIXED parent (the desktop root) places
 * it against that edge. Tiles are drawn with the shared bd_draw_tile primitive,
 * identical to bd_widget_inventory.
 *
 * v1: an auto-packed strip; click a tile to restore its window. A 2D snap-grid
 * with drag-relocate is a roadmap item.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_widget_dock.h"
#include "bd_widget_icon.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define DOCK_PAD 4       /* inset of the icon square within a tile cell */
#define DOCK_MAX 64      /* tile cap */

struct dock {
	int          gravity;      /* enum bd_gravity: which edge/corner */
	int          tile_size;    /* icon edge length, px */
	bd_dock_model model;
	int          has_model;
	bd_id        tiles[DOCK_MAX];  /* current minimized frames, in order */
	int          ntiles;
	int          press_tile;   /* index pressed, -1 = none */
	int          press_x, press_y; /* pointer at press, for the drag threshold */
	int          dragging;     /* a cross-widget drag started from press_tile */
};

#define DOCK_DRAG_THRESHOLD 4  /* px the pointer must move to start a drag */

static int dock_type;

static int cell_w(const struct dock *d) { return d->tile_size + 2 * DOCK_PAD; }
static int cell_h(const struct dock *d)
{
	return d->tile_size + 2 * DOCK_PAD + (int)bd_draw_line_height();
}

/* LEFT/RIGHT and the four corners stack vertically; TOP/BOTTOM run across. */
static int
is_vertical(int g)
{
	return g != BD_GRAVITY_TOP && g != BD_GRAVITY_BOTTOM;
}

/* the anchor point (edge-center or corner) the strip pins to, from its gravity */
static int
dock_anchor(int g)
{
	switch (g) {
	case BD_GRAVITY_LEFT:         return BD_ANCHOR_W;
	case BD_GRAVITY_RIGHT:        return BD_ANCHOR_E;
	case BD_GRAVITY_TOP:          return BD_ANCHOR_N;
	case BD_GRAVITY_BOTTOM:       return BD_ANCHOR_S;
	case BD_GRAVITY_TOP_LEFT:     return BD_ANCHOR_NW;
	case BD_GRAVITY_TOP_RIGHT:    return BD_ANCHOR_NE;
	case BD_GRAVITY_BOTTOM_LEFT:  return BD_ANCHOR_SW;
	case BD_GRAVITY_BOTTOM_RIGHT: return BD_ANCHOR_SE;
	default:                      return BD_ANCHOR_NW; /* NONE */
	}
}

/* rebuild tiles[] from the minimized top-level frames (derived state). Scoped
 * to the dock's WM host: a dock inside a managed canvas shows only that canvas's
 * minimized frames; a dock on the desktop shows the surface's. */
static void
dock_reconcile(struct dock *d, bd_id id)
{
	bd_id all[DOCK_MAX];
	int n = bd_window_list_in(bd_managed_canvas_of(id), all, DOCK_MAX);
	d->ntiles = 0;
	for (int i = 0; i < n && d->ntiles < DOCK_MAX; i++)
		if (bd_window_minimized(all[i]))
			d->tiles[d->ntiles++] = all[i];
	if (d->press_tile >= d->ntiles)
		d->press_tile = -1;
}

/* per-tile content: model override, else defaults (empty icon + frame title) */
static void
dock_item(struct dock *d, bd_id frame, bd_dock_item *out)
{
	memset(out, 0, sizeof *out);
	out->key = frame;
	out->enabled = 1;
	out->label = bd_get_s(frame, BD_LABEL_S);
	if (d->has_model && d->model.get)
		d->model.get(d->model.ctx, frame, out);
}

/* which tile index is under (px,py) within the dock rect at (x,y), or -1 */
static int
dock_tile_at(const struct dock *d, int x, int y, int px, int py)
{
	int vert = is_vertical(d->gravity);
	int cw = cell_w(d), ch = cell_h(d);
	for (int i = 0; i < d->ntiles; i++) {
		int rx = vert ? x : x + i * cw;
		int ry = vert ? y + i * ch : y;
		if (px >= rx && px < rx + cw && py >= ry && py < ry + ch)
			return i;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* class hooks                                                        */
/* ------------------------------------------------------------------ */

static void
dock_init(bd_id id, void *state)
{
	(void)id;
	struct dock *d = state;
	d->gravity = BD_GRAVITY_LEFT;
	d->tile_size = 64;
	d->press_tile = -1;
}

static void
dock_layout(bd_id id, void *state, int w, int h)
{
	(void)w; (void)h;
	struct dock *d = state;
	dock_reconcile(d, id);
	int vert = is_vertical(d->gravity);
	int cw = cell_w(d), ch = cell_h(d);
	int pw = vert ? cw : d->ntiles * cw;
	int ph = vert ? d->ntiles * ch : ch;
	/* self-anchor to the gravity edge; an empty dock shrinks to nothing */
	bd_set(id, BD_ANCHOR_I, dock_anchor(d->gravity),
	    BD_PREF_W_I, pw, BD_PREF_H_I, ph, BD_END);
}

static void
dock_render(bd_id id, void *state)
{
	struct dock *d = state;
	dock_reconcile(d, id);
	if (d->ntiles == 0)
		return;
	const bd_theme *th = bd_gui_theme();
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	int vert = is_vertical(d->gravity);
	int cw = cell_w(d), ch = cell_h(d);

	/* raised backing panel behind the tiles */
	bd_draw_rect(x, y, w, h, th->panel);
	bd_draw_rect_lines(x, y, w, h, th->border);

	for (int i = 0; i < d->ntiles; i++) {
		int rx = vert ? x : x + i * cw;
		int ry = vert ? y + i * ch : y;
		bd_dock_item it;
		dock_item(d, d->tiles[i], &it);
		bd_icon_desc dc = { .key = it.key, .label = it.label,
		    .icon = it.icon, .count = it.count, .enabled = it.enabled };
		bd_icon_draw((float)rx, (float)ry, cw, DOCK_PAD, d->tile_size,
		    &dc, th->bg, th->border, th->text);
		if (i == d->press_tile)
			bd_draw_rect_lines((float)(rx + DOCK_PAD),
			    (float)(ry + DOCK_PAD), (float)d->tile_size,
			    (float)d->tile_size, th->focus);
	}
}

static int
dock_event(bd_id id, void *state, const bd_event *ev)
{
	struct dock *d = state;
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);

	if (ev->type == BD_EV_MOUSE_DOWN && ev->button == BD_MOUSE_LEFT) {
		d->press_tile = dock_tile_at(d, x, y, ev->x, ev->y);
		d->press_x = ev->x;
		d->press_y = ev->y;
		d->dragging = 0;
		return d->press_tile >= 0;
	}
	if (ev->type == BD_EV_MOUSE_MOVE && d->press_tile >= 0 && !d->dragging &&
	    (abs(ev->x - d->press_x) > DOCK_DRAG_THRESHOLD ||
	     abs(ev->y - d->press_y) > DOCK_DRAG_THRESHOLD)) {
		/* offer the tile's window as a cross-widget drag payload (e.g. drop
		 * onto an action bar to bind a "restore this window" action). The
		 * toolkit draws the ghost and delivers the drop on release. */
		d->dragging = 1;
		bd_dock_item it;
		dock_item(d, d->tiles[d->press_tile], &it);
		bd_icon_desc dc = { .key = d->tiles[d->press_tile],
		    .label = it.label, .icon = it.icon, .count = it.count,
		    .enabled = it.enabled };
		bd_icon_dnd_begin(id, &dc, NULL);
		return 1;
	}
	if (ev->type == BD_EV_MOUSE_UP && ev->button == BD_MOUSE_LEFT &&
	    d->press_tile >= 0) {
		int t = dock_tile_at(d, x, y, ev->x, ev->y);
		if (!d->dragging && t == d->press_tile && t < d->ntiles)
			bd_window_restore(d->tiles[t]);   /* a plain click restores */
		d->press_tile = -1;
		d->dragging = 0;
		return 1;
	}
	return 0;
}

static const bd_widget_class dock_class = {
	.name = "dock",
	.state_size = sizeof(struct dock),
	.init = dock_init,
	.render = dock_render,
	.layout = dock_layout,
	.event = dock_event,
};

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

void
bd_dock_register(void)
{
	if (!dock_type)
		dock_type = bd_register_widget_class(&dock_class);
}

bd_id
bd_dock_create(bd_id parent, const bd_dock_model *model, ...)
{
	bd_dock_register();
	va_list ap;
	va_start(ap, model);
	bd_id id = bd_create_va(parent, dock_type, ap);
	va_end(ap);
	if (id != BD_NONE && model) {
		struct dock *d = bd_widget_state(id);
		d->model = *model;
		d->has_model = 1;
	}
	return id;
}

void
bd_dock_set_gravity(bd_id dock, int gravity)
{
	if (bd_widget_type(dock) != dock_type)
		return;
	((struct dock *)bd_widget_state(dock))->gravity = gravity;
}

void
bd_dock_set_tile_size(bd_id dock, int px)
{
	if (bd_widget_type(dock) != dock_type)
		return;
	((struct dock *)bd_widget_state(dock))->tile_size = px > 8 ? px : 8;
}

int
bd_dock_count(bd_id dock)
{
	if (bd_widget_type(dock) != dock_type)
		return 0;
	return ((struct dock *)bd_widget_state(dock))->ntiles;
}
