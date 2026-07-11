/*
 * bd_widget_actionbar -- a game hotbar / floating tool palette. See
 * bd_widget_actionbar.h.
 *
 * The mutable sibling of bd_widget_dock: same beveled tiles (bd_draw_tile) and
 * the same enum bd_gravity edge/corner anchoring, but the slots are OWNED and
 * host-fillable rather than a projection of window state. It is a drop target
 * for the toolkit's cross-widget drag (widget_ext.h bd_dnd_*): release an
 * inventory item or a dock tile over a slot to bind it. Slots reorder by
 * dragging one onto another; a plain click fires the activate callback; a
 * floating (BD_GRAVITY_NONE) bar is dragged around by the grip at its lead edge.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_widget_actionbar.h"
#include "bd_widget_icon.h"
#include "bd_draw.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define AB_MAX        32   /* slot cap */
#define AB_LABEL      32   /* per-slot caption bytes (incl. NUL) */
#define AB_PAD         4   /* inset of the icon square within a tile cell */
#define AB_GRIP       12   /* main-axis thickness of the drag grip */
#define AB_THRESHOLD   4   /* px the pointer must move to start a drag */

enum { AB_IDLE = 0, AB_PENDING, AB_DRAG_SLOT, AB_DRAG_BAR };

struct ab_slot {
	uint64_t   key;
	char       label[AB_LABEL];
	bd_texture icon;
	int        enabled;
	int        filled;
	void      *user;
	int        hotkey;    /* ASCII / BD_KEY_* binding, 0 = none (slot property) */
	int        hotmods;   /* BD_MOD_* bitmask for the hotkey */
};

struct actionbar {
	int  gravity;      /* enum bd_gravity: edge/corner, or NONE = floating */
	int  vertical;     /* floating orientation: nonzero = vertical strip */
	int  tile_size;    /* icon edge length, px */
	int  nslots;
	struct ab_slot slots[AB_MAX];

	bd_actionbar_cb cb;
	int  has_cb;

	/* pointer state */
	int  mode;         /* AB_* */
	int  press_slot;   /* slot pressed, -1 = none/grip */
	int  press_x, press_y;
	int  grab_dx, grab_dy; /* pointer offset within the bar (for AB_DRAG_BAR) */
	int  drop_slot;    /* slot under the pointer during an internal drag */
	int  hover_slot;   /* slot under the pointer (hover), -1 = none */
	int  mouse_x, mouse_y;
};

static int ab_type;

/* ------------------------------------------------------------------ */
/* geometry                                                           */
/* ------------------------------------------------------------------ */

static int cell_w(const struct actionbar *a) { return a->tile_size + 2 * AB_PAD; }
static int cell_h(const struct actionbar *a)
{
	return a->tile_size + 2 * AB_PAD + (int)bd_draw_line_height();
}

/* LEFT/RIGHT and the four corners stack vertically; TOP/BOTTOM run across; a
 * floating bar uses its explicit orientation. */
static int
is_vertical(const struct actionbar *a)
{
	switch (a->gravity) {
	case BD_GRAVITY_TOP:
	case BD_GRAVITY_BOTTOM:
		return 0;
	case BD_GRAVITY_NONE:
		return a->vertical;
	default:
		return 1;   /* LEFT / RIGHT / corners */
	}
}

/* the anchor corner the bar pins to, from its gravity (mirrors the dock) */
static int
ab_anchor(int g)
{
	switch (g) {
	case BD_GRAVITY_RIGHT:
	case BD_GRAVITY_TOP_RIGHT:    return BD_ANCHOR_NE;
	case BD_GRAVITY_BOTTOM_RIGHT: return BD_ANCHOR_SE;
	case BD_GRAVITY_BOTTOM_LEFT:
	case BD_GRAVITY_BOTTOM:       return BD_ANCHOR_SW;
	case BD_GRAVITY_NONE:         return BD_ANCHOR_NW; /* float at X/Y */
	default:                      return BD_ANCHOR_NW; /* LEFT/TOP_LEFT/TOP */
	}
}

/* origin of slot i within the bar rect at (x,y) (past the leading grip) */
static void
slot_origin(const struct actionbar *a, int x, int y, int i, int *rx, int *ry)
{
	if (is_vertical(a)) {
		*rx = x;
		*ry = y + AB_GRIP + i * cell_h(a);
	} else {
		*rx = x + AB_GRIP + i * cell_w(a);
		*ry = y;
	}
}

/* slot index under (px,py) within the bar rect at (x,y), or -1 (grip / gap) */
static int
slot_at(const struct actionbar *a, int x, int y, int px, int py)
{
	int cw = cell_w(a), ch = cell_h(a);
	for (int i = 0; i < a->nslots; i++) {
		int sx, sy;
		slot_origin(a, x, y, i, &sx, &sy);
		if (px >= sx && px < sx + cw && py >= sy && py < sy + ch)
			return i;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* slot storage                                                       */
/* ------------------------------------------------------------------ */

/* Human-readable hotkey label into buf, e.g. "^A", "⇧⌥5". Modifier symbols
 * follow the enumerated order Shift ⇧, Control ^, Alt ⌥; the key is shown as its
 * uppercase ASCII character (other codes fall back to '?'). */
static void
hotkey_label(int key, int mods, char *buf, size_t n)
{
	buf[0] = '\0';
	if (!key)
		return;
	size_t len = 0;
	const char *mod[3] = { NULL, NULL, NULL };
	if (mods & BD_MOD_SHIFT) mod[0] = "⇧";   /* ⇧ */
	if (mods & BD_MOD_CTRL)  mod[1] = "^";
	if (mods & BD_MOD_ALT)   mod[2] = "⌥";   /* ⌥ */
	for (int i = 0; i < 3; i++) {
		if (!mod[i]) continue;
		size_t ml = strlen(mod[i]);
		if (len + ml < n) { memcpy(buf + len, mod[i], ml); len += ml; }
	}
	char kc = (key > 32 && key < 127) ? (char)key : '?';
	if (kc >= 'a' && kc <= 'z') kc = (char)(kc - 'a' + 'A');
	if (len + 1 < n) buf[len++] = kc;
	buf[len] = '\0';
}

static void
slot_set(struct ab_slot *s, const bd_action *a)
{
	int hk = s->hotkey, hm = s->hotmods;   /* the binding is the slot's, kept */
	memset(s, 0, sizeof *s);
	s->hotkey = hk;
	s->hotmods = hm;
	if (!a)
		return;
	s->key = a->key;
	s->icon = a->icon;
	s->enabled = a->enabled;
	s->user = a->user;
	s->filled = 1;
	if (a->label) {
		size_t n = strlen(a->label);
		if (n >= AB_LABEL)
			n = AB_LABEL - 1;
		memcpy(s->label, a->label, n);
		s->label[n] = '\0';
	}
}

static void
slot_swap(struct actionbar *a, int i, int j)
{
	struct ab_slot t = a->slots[i];
	a->slots[i] = a->slots[j];
	a->slots[j] = t;
}

/* ------------------------------------------------------------------ */
/* class hooks                                                        */
/* ------------------------------------------------------------------ */

static void
ab_init(bd_id id, void *state)
{
	(void)id;
	struct actionbar *a = state;
	a->gravity = BD_GRAVITY_NONE;
	a->tile_size = 48;
	a->nslots = 1;
	a->press_slot = a->drop_slot = a->hover_slot = -1;
}

/* Push the bar's preferred size and edge anchor onto the widget. Called from
 * the layout hook and from every geometry-affecting setter (and create), so the
 * parent has a correct size to lay out against on the very first pass, the way
 * bd_widget_inventory sets its preferred width at create time. */
static void
ab_update_geom(bd_id id, struct actionbar *a)
{
	int cw = cell_w(a), ch = cell_h(a);
	int pw, ph;
	if (is_vertical(a)) {
		pw = cw;
		ph = AB_GRIP + a->nslots * ch;
	} else {
		pw = AB_GRIP + a->nslots * cw;
		ph = ch;
	}
	bd_set(id, BD_ANCHOR_I, ab_anchor(a->gravity),
	    BD_PREF_W_I, pw, BD_PREF_H_I, ph, BD_END);
}

static void
ab_layout(bd_id id, void *state, int w, int h)
{
	(void)w; (void)h;
	ab_update_geom(id, state);
}

/* the leading grip: a few ridges the user grabs to drag a floating bar */
static void
draw_grip(const struct actionbar *a, int x, int y, int w, int h, uint32_t c)
{
	if (is_vertical(a)) {
		for (int gx = x + 4; gx < x + w - 4; gx += 4)
			bd_draw_rect(gx, y + 3, 2, AB_GRIP - 6, c);
	} else {
		for (int gy = y + 4; gy < y + h - 4; gy += 4)
			bd_draw_rect(x + 3, gy, AB_GRIP - 6, 2, c);
	}
}

static void
ab_render(bd_id id, void *state)
{
	struct actionbar *a = state;
	const bd_theme *th = bd_gui_theme();
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);

	/* raised backing panel */
	bd_draw_rect(x, y, w, h, th->panel);
	bd_draw_rect_lines(x, y, w, h, th->border);
	draw_grip(a, x, y, w, h, th->border);

	for (int i = 0; i < a->nslots; i++) {
		int rx, ry;
		slot_origin(a, x, y, i, &rx, &ry);
		struct ab_slot *s = &a->slots[i];
		bd_icon_desc dc = { .key = s->key,
		    .label = s->filled ? s->label : NULL, .icon = s->icon,
		    .count = 0, .enabled = s->filled ? s->enabled : 1 };
		bd_icon_draw((float)rx, (float)ry, cell_w(a), AB_PAD, a->tile_size,
		    &dc, th->bg, th->border, th->text);

		/* hotkey label in the top-left corner of the icon, shown when the
		 * slot is empty (so the binding is discoverable) or hovered */
		if (s->hotkey && (i == a->hover_slot || !s->filled)) {
			char hk[24];
			hotkey_label(s->hotkey, s->hotmods, hk, sizeof hk);
			bd_draw_text(hk, (float)(rx + AB_PAD + 2),
			    (float)(ry + AB_PAD + 1), th->text_hi);
		}

		/* ring the drop target while a slot is dragged within the bar */
		if (a->mode == AB_DRAG_SLOT && i == a->drop_slot &&
		    i != a->press_slot)
			bd_draw_rect_lines((float)(rx + AB_PAD), (float)(ry + AB_PAD),
			    (float)a->tile_size, (float)a->tile_size, th->focus);
	}
}

/* ------------------------------------------------------------------ */
/* events                                                             */
/* ------------------------------------------------------------------ */

static void
ab_start_slot_drag(bd_id id, struct actionbar *a)
{
	struct ab_slot *s = &a->slots[a->press_slot];
	bd_icon_desc dc = { .key = s->key, .label = s->label, .icon = s->icon,
	    .count = 0, .enabled = s->enabled };
	/* the toolkit draws the ghost + delivers any drop */
	bd_icon_dnd_begin(id, &dc, s->user);
}

static int
ab_drop(bd_id id, struct actionbar *a, const bd_event *ev)
{
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	int slot = slot_at(a, x, y, ev->x, ev->y);
	if (slot < 0)
		return 0;
	const bd_dnd_payload *p = bd_dnd_get();
	if (!p)
		return 0;
	int accept = 1;
	if (a->has_cb && a->cb.drop)
		accept = a->cb.drop(id, slot, p, a->cb.ctx);
	if (accept) {
		bd_action act = {0};
		act.key = p->key;
		act.label = p->label;
		act.icon = p->icon;
		act.enabled = 1;
		act.user = p->user;
		slot_set(&a->slots[slot], &act);
	}
	return 1;
}

static int
ab_event(bd_id id, void *state, const bd_event *ev)
{
	struct actionbar *a = state;
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);

	switch (ev->type) {
	case BD_EV_DROP:
		return ab_drop(id, a, ev);

	case BD_EV_MOUSE_DOWN: {
		int slot = slot_at(a, x, y, ev->x, ev->y);
		if (ev->button == BD_MOUSE_RIGHT) {
			if (slot >= 0 && a->slots[slot].filled && a->has_cb &&
			    a->cb.context)
				a->cb.context(id, slot, a->slots[slot].key,
				    ev->x, ev->y, a->cb.ctx);
			return 1;
		}
		if (ev->button != BD_MOUSE_LEFT)
			return 0;
		a->press_slot = slot;
		a->press_x = ev->x;
		a->press_y = ev->y;
		a->grab_dx = ev->x - x;
		a->grab_dy = ev->y - y;
		a->drop_slot = slot;
		a->mode = AB_PENDING;
		return 1;
	}

	case BD_EV_MOUSE_MOVE:
		a->mouse_x = ev->x;
		a->mouse_y = ev->y;
		if (a->mode == AB_PENDING &&
		    (abs(ev->x - a->press_x) > AB_THRESHOLD ||
		     abs(ev->y - a->press_y) > AB_THRESHOLD)) {
			if (a->press_slot >= 0 && a->slots[a->press_slot].filled) {
				a->mode = AB_DRAG_SLOT;
				ab_start_slot_drag(id, a);
			} else if (a->gravity == BD_GRAVITY_NONE) {
				a->mode = AB_DRAG_BAR;   /* drag the whole floating bar */
			} else {
				a->mode = AB_IDLE;       /* nothing to drag on a docked bar */
			}
		}
		if (a->mode == AB_DRAG_SLOT) {
			a->drop_slot = slot_at(a, x, y, ev->x, ev->y);
			return 1;
		}
		if (a->mode == AB_DRAG_BAR) {
			int nx = ev->x - a->grab_dx;
			int ny = ev->y - a->grab_dy;
			if (nx < 0) nx = 0;
			if (ny < 0) ny = 0;
			bd_set(id, BD_X_I, nx, BD_Y_I, ny, BD_END);
			return 1;
		}
		/* idle hover: track the slot under the pointer for the hotkey label
		 * (moves arrive here via the class BD_WC_WANTS_HOVER flag) */
		a->hover_slot = slot_at(a, x, y, ev->x, ev->y);
		return 0;

	case BD_EV_MOUSE_UP:
		if (ev->button != BD_MOUSE_LEFT)
			return 0;
		if (a->mode == AB_PENDING && a->press_slot >= 0) {
			struct ab_slot *s = &a->slots[a->press_slot];
			if (s->filled && s->enabled && a->has_cb && a->cb.activate)
				a->cb.activate(id, a->press_slot, s->key, a->cb.ctx);
		} else if (a->mode == AB_DRAG_SLOT && a->press_slot >= 0) {
			int to = slot_at(a, x, y, ev->x, ev->y);
			if (to >= 0 && to != a->press_slot) {
				slot_swap(a, a->press_slot, to);
				if (a->has_cb && a->cb.move)
					a->cb.move(id, a->press_slot, to, a->cb.ctx);
			}
		}
		a->mode = AB_IDLE;
		a->press_slot = a->drop_slot = -1;
		return 1;

	default:
		return 0;
	}
}

static const bd_widget_class ab_class = {
	.name       = "actionbar",
	.state_size = sizeof(struct actionbar),
	.init       = ab_init,
	.render     = ab_render,
	.layout     = ab_layout,
	.event      = ab_event,
	.flags      = BD_WC_WANTS_HOVER,   /* hover moves drive the hotkey label */
};

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

void
bd_actionbar_register(void)
{
	if (!ab_type)
		ab_type = bd_register_widget_class(&ab_class);
}

static int
clamp_slots(int n)
{
	if (n < 1) return 1;
	if (n > AB_MAX) return AB_MAX;
	return n;
}

bd_id
bd_actionbar_create(bd_id parent, int slots, const bd_actionbar_cb *cb, ...)
{
	bd_actionbar_register();
	va_list ap;
	va_start(ap, cb);
	bd_id id = bd_create_va(parent, ab_type, ap);
	va_end(ap);
	struct actionbar *a = bd_widget_state(id);
	if (!a)
		return id;
	a->nslots = clamp_slots(slots);
	if (cb) {
		a->cb = *cb;
		a->has_cb = 1;
	}
	ab_update_geom(id, a);   /* size it now so the first layout is correct */
	return id;
}

void
bd_actionbar_set_gravity(bd_id bar, int gravity)
{
	if (bd_widget_type(bar) != ab_type)
		return;
	struct actionbar *a = bd_widget_state(bar);
	a->gravity = gravity;
	ab_update_geom(bar, a);
}

void
bd_actionbar_set_orientation(bd_id bar, int vertical)
{
	if (bd_widget_type(bar) != ab_type)
		return;
	struct actionbar *a = bd_widget_state(bar);
	a->vertical = vertical ? 1 : 0;
	ab_update_geom(bar, a);
}

void
bd_actionbar_set_tile_size(bd_id bar, int px)
{
	if (bd_widget_type(bar) != ab_type)
		return;
	struct actionbar *a = bd_widget_state(bar);
	a->tile_size = px > 8 ? px : 8;
	ab_update_geom(bar, a);
}

void
bd_actionbar_set_slots(bd_id bar, int slots)
{
	if (bd_widget_type(bar) != ab_type)
		return;
	struct actionbar *a = bd_widget_state(bar);
	int n = clamp_slots(slots);
	for (int i = n; i < a->nslots; i++)
		memset(&a->slots[i], 0, sizeof a->slots[i]);
	a->nslots = n;
	ab_update_geom(bar, a);
}

int
bd_actionbar_slots(bd_id bar)
{
	if (bd_widget_type(bar) != ab_type)
		return 0;
	return ((struct actionbar *)bd_widget_state(bar))->nslots;
}

void
bd_actionbar_set_slot(bd_id bar, int slot, const bd_action *a)
{
	if (bd_widget_type(bar) != ab_type)
		return;
	struct actionbar *ab = bd_widget_state(bar);
	if (slot < 0 || slot >= ab->nslots)
		return;
	slot_set(&ab->slots[slot], a);
}

int
bd_actionbar_get_slot(bd_id bar, int slot, bd_action *out)
{
	if (bd_widget_type(bar) != ab_type)
		return 0;
	struct actionbar *ab = bd_widget_state(bar);
	if (slot < 0 || slot >= ab->nslots || !ab->slots[slot].filled)
		return 0;
	if (out) {
		struct ab_slot *s = &ab->slots[slot];
		out->key = s->key;
		out->label = s->label;
		out->icon = s->icon;
		out->enabled = s->enabled;
		out->user = s->user;
	}
	return 1;
}

void
bd_actionbar_set_hotkey(bd_id bar, int slot, int key, int mods)
{
	if (bd_widget_type(bar) != ab_type)
		return;
	struct actionbar *ab = bd_widget_state(bar);
	if (slot < 0 || slot >= ab->nslots)
		return;
	ab->slots[slot].hotkey = key;
	ab->slots[slot].hotmods = key ? mods : 0;
}

int
bd_actionbar_get_hotkey(bd_id bar, int slot, int *key, int *mods)
{
	if (bd_widget_type(bar) != ab_type)
		return 0;
	struct actionbar *ab = bd_widget_state(bar);
	if (slot < 0 || slot >= ab->nslots || !ab->slots[slot].hotkey)
		return 0;
	if (key)  *key  = ab->slots[slot].hotkey;
	if (mods) *mods = ab->slots[slot].hotmods;
	return 1;
}

int
bd_actionbar_key(bd_id bar, int key, int mods)
{
	if (bd_widget_type(bar) != ab_type || !key)
		return 0;
	struct actionbar *ab = bd_widget_state(bar);
	for (int i = 0; i < ab->nslots; i++) {
		struct ab_slot *s = &ab->slots[i];
		if (s->hotkey != key || s->hotmods != mods)
			continue;
		if (s->filled && s->enabled && ab->has_cb && ab->cb.activate)
			ab->cb.activate(bar, i, s->key, ab->cb.ctx);
		return 1;   /* the binding matched: consume the press */
	}
	return 0;
}
