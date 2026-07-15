/* bd_widget_split.c : resizable split-pane (sash) container widget */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * A binary split: two BD_PANEL panes with a draggable sash between them, laid
 * out by the core flex engine (BD_LAYOUT_ROW for a horizontal split, COL for a
 * vertical one). The split stores a ratio and converts it into grow weights on
 * the two panes, so the flex engine divides the space every frame and rescales
 * it on resize for free; the panes carry a pref of 1 on the main axis so the
 * grow weights, not the 64px flex default, decide the division.
 *
 * The sash is a separate private extension widget, not part of the split's own
 * class. That matters: hit-testing (hit_extension) returns the deepest child
 * whose class has an event hook, bubbling to the container otherwise. If the
 * split itself carried the event hook it would swallow every press over its
 * panes (a button inside a pane would never see the click). Isolating the drag
 * hook on the thin sash lets pane content interact normally; a press only lands
 * on the split machinery when it lands on the sash.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_widget_split.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include "bd_theme.h"
#include <stdarg.h>

#define SPLIT_SASH_DEFAULT   6    /* sash thickness, px */
#define SPLIT_MIN_DEFAULT    32   /* per-pane minimum, px */
#define SPLIT_GROW_SCALE     1000 /* grow-weight resolution for the ratio */

struct split {
	int            orient;     /* BD_SPLIT_HORIZONTAL | BD_SPLIT_VERTICAL */
	bd_id          pane_a;     /* first pane (left / top) */
	bd_id          sash;       /* the draggable divider */
	bd_id          pane_b;     /* second pane (right / bottom) */
	float          ratio;      /* first pane's fraction of the splittable extent */
	int            sash_px;    /* sash thickness */
	int            min_px;     /* per-pane minimum */
	int            dragging;   /* a sash drag is in flight */
	bd_callback_fn on_change;
	void          *on_change_data;
};

struct sash {
	int hover;                 /* pointer is over the sash */
};

static int split_type;
static int sash_type;

static int
pt_in(int px, int py, int x, int y, int w, int h)
{
	return px >= x && px < x + w && py >= y && py < y + h;
}

/* Push the current ratio onto the two panes as flex grow weights. The panes
 * keep a main-axis pref of 1 so the flex floor is 1px (not DEFAULT_MIN_W/H) and
 * the weights alone divide the splittable space. */
static void
split_apply(struct split *s)
{
	if (s->pane_a == BD_NONE || s->pane_b == BD_NONE)
		return;
	int ga = (int)(s->ratio * (float)SPLIT_GROW_SCALE + 0.5f);
	if (ga < 1)               ga = 1;
	if (ga > SPLIT_GROW_SCALE - 1) ga = SPLIT_GROW_SCALE - 1;
	int gb = SPLIT_GROW_SCALE - ga;
	int pref = (s->orient == BD_SPLIT_HORIZONTAL) ? BD_PREF_W_I : BD_PREF_H_I;
	bd_set(s->pane_a, BD_GROW_I, ga, pref, 1, BD_END);
	bd_set(s->pane_b, BD_GROW_I, gb, pref, 1, BD_END);
}

/* ------------------------------------------------------------------ */
/* sash class hooks                                                   */
/* ------------------------------------------------------------------ */

static void
sash_render(bd_id id, void *state)
{
	struct sash *sk = state;
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	if (w <= 0 || h <= 0)
		return;

	const bd_theme *th = bd_gui_theme();
	struct split *s = bd_widget_state(bd_parent(id));
	int horiz = s ? (s->orient == BD_SPLIT_HORIZONTAL) : 1;
	int active = sk->hover || (s && s->dragging);

	bd_draw_rect((float)x, (float)y, (float)w, (float)h,
	    active ? th->hover : th->widget);

	/* three grip dots down the middle of the bar */
	uint32_t grip = active ? th->text_hi : th->border;
	int cx = x + w / 2;
	int cy = y + h / 2;
	for (int i = -1; i <= 1; i++) {
		if (horiz)
			bd_draw_rect((float)(cx - 1), (float)(cy + i * 5 - 1),
			    2.0f, 2.0f, grip);
		else
			bd_draw_rect((float)(cx + i * 5 - 1), (float)(cy - 1),
			    2.0f, 2.0f, grip);
	}
}

static int
sash_event(bd_id id, void *state, const bd_event *ev)
{
	struct sash *sk = state;
	bd_id sid = bd_parent(id);
	struct split *s = bd_widget_state(sid);
	if (!s)
		return 0;
	int horiz = (s->orient == BD_SPLIT_HORIZONTAL);

	switch (ev->type) {
	case BD_EV_MOUSE_DOWN:
		if (ev->button != BD_MOUSE_LEFT)
			return 0;
		s->dragging = 1;
		sk->hover = 1;
		return 1;

	case BD_EV_MOUSE_MOVE:
		if (!s->dragging) {
			/* uncaptured hover: light the sash when pointed at */
			int sx, sy, sw, sh;
			bd_widget_rect(id, &sx, &sy, &sw, &sh);
			sk->hover = pt_in(ev->x, ev->y, sx, sy, sw, sh);
			return 0;
		} else {
			/* captured drag: place the sash under the pointer within
			 * the split, clamped to keep both panes >= the minimum */
			int px, py, pw, ph;
			bd_widget_rect(sid, &px, &py, &pw, &ph);
			int total = horiz ? pw : ph;
			int splittable = total - s->sash_px;
			if (splittable < 1)
				splittable = 1;
			int pos = horiz ? ev->x - px : ev->y - py;
			int lo = s->min_px;
			int hi = splittable - s->min_px;
			if (hi < lo) {          /* too small to honor the minimum */
				lo = splittable / 2;
				hi = lo;
			}
			if (pos < lo) pos = lo;
			if (pos > hi) pos = hi;
			float r = (float)pos / (float)splittable;
			if (r != s->ratio) {
				s->ratio = r;
				split_apply(s);
				if (s->on_change)
					s->on_change(sid, s->on_change_data);
			}
			return 1;
		}

	case BD_EV_MOUSE_UP:
		s->dragging = 0;
		return 1;

	default:
		return 0;
	}
}

static const bd_widget_class sash_class = {
	.name       = "split-sash",
	.state_size = sizeof(struct sash),
	.render     = sash_render,
	.event      = sash_event,
	.flags      = BD_WC_WANTS_HOVER,
};

static const bd_widget_class split_class = {
	.name       = "split",
	.state_size = sizeof(struct split),
	.flags      = BD_WC_CONTAINS_CHILDREN,
};

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

void
bd_split_register(void)
{
	if (!split_type)
		split_type = bd_register_widget_class(&split_class);
	if (!sash_type)
		sash_type = bd_register_widget_class(&sash_class);
}

bd_id
bd_split_create(bd_id parent, int orient, ...)
{
	bd_split_register();
	va_list ap;
	va_start(ap, orient);
	bd_id id = bd_create_va(parent, split_type, ap);
	va_end(ap);
	struct split *s = bd_widget_state(id);
	if (!s)
		return id;

	s->orient  = (orient == BD_SPLIT_VERTICAL) ? BD_SPLIT_VERTICAL
	                                           : BD_SPLIT_HORIZONTAL;
	s->ratio   = 0.5f;
	s->sash_px = SPLIT_SASH_DEFAULT;
	s->min_px  = SPLIT_MIN_DEFAULT;

	int horiz = (s->orient == BD_SPLIT_HORIZONTAL);
	bd_set(id, BD_LAYOUT_I, horiz ? BD_LAYOUT_ROW : BD_LAYOUT_COL,
	    BD_PAD_I, 0, BD_GAP_I, 0, BD_END);

	s->pane_a = bd_create(id, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	s->sash   = bd_create(id, sash_type,
	    horiz ? BD_PREF_W_I : BD_PREF_H_I, s->sash_px, BD_END);
	s->pane_b = bd_create(id, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);

	split_apply(s);
	return id;
}

bd_id
bd_split_pane(bd_id split, int index)
{
	if (bd_widget_type(split) != split_type)
		return BD_NONE;
	struct split *s = bd_widget_state(split);
	if (index == 0) return s->pane_a;
	if (index == 1) return s->pane_b;
	return BD_NONE;
}

float
bd_split_ratio(bd_id split)
{
	if (bd_widget_type(split) != split_type)
		return 0.0f;
	return ((struct split *)bd_widget_state(split))->ratio;
}

void
bd_split_set_ratio(bd_id split, float ratio)
{
	if (bd_widget_type(split) != split_type)
		return;
	struct split *s = bd_widget_state(split);
	if (ratio < 0.0f) ratio = 0.0f;
	if (ratio > 1.0f) ratio = 1.0f;
	s->ratio = ratio;
	split_apply(s);
}

void
bd_split_set_min_size(bd_id split, int px)
{
	if (bd_widget_type(split) != split_type)
		return;
	((struct split *)bd_widget_state(split))->min_px = px < 0 ? 0 : px;
}

void
bd_split_set_sash_size(bd_id split, int px)
{
	if (bd_widget_type(split) != split_type)
		return;
	struct split *s = bd_widget_state(split);
	s->sash_px = px < 1 ? 1 : px;
	int pref = (s->orient == BD_SPLIT_HORIZONTAL) ? BD_PREF_W_I : BD_PREF_H_I;
	bd_set(s->sash, pref, s->sash_px, BD_END);
}

void
bd_split_on_change(bd_id split, bd_callback_fn fn, void *data)
{
	if (bd_widget_type(split) != split_type)
		return;
	struct split *s = bd_widget_state(split);
	s->on_change = fn;
	s->on_change_data = data;
}
