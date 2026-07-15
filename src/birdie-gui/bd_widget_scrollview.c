/* bd_widget_scrollview.c : a viewport that scrolls a child widget subtree */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * The scroll-view is a BD_LAYOUT_FIXED container flagged CONTAINS_CHILDREN and
 * CLIP_CHILDREN. It owns two children:
 *
 *   - content: a BD_LAYOUT_COL panel the caller fills with fields.
 *   - bar:     a BD_SCROLLBAR pinned to the right edge, hidden when it fits.
 *
 * Scrolling works by offsetting the single content child, not by a render
 * transform (the render walk applies none). Each layout the view measures the
 * content's natural height (summing its children's PREF_H, since the layout
 * engine never measures a subtree itself), clamps the scroll offset, and sets
 * the content child's PREF_H to that height and its BD_Y_I to -scroll_y. The
 * core then lays the content -- and its grandchildren -- out at the scrolled
 * origin, taller than the viewport; the CLIP_CHILDREN flag scissors it to the
 * viewport rect at render time. Events need no offset: the hit walk already
 * rejects points outside the viewport before recursing.
 *
 * The layout hook runs after the core layout pass, so its geometry lands on the
 * next frame (an invisible one-frame lag). The wheel and scrollbar-drag paths
 * re-apply the content offset immediately so dragging and wheeling are crisp.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_widget_scrollview.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include <stdarg.h>

#define SV_BAR_W  14   /* scrollbar thickness, px */
#define SV_GAP     4   /* gap between stacked fields, px */

struct scrollview {
	bd_id content;
	bd_id bar;
	int   scroll_y;     /* current offset, px */
	int   content_h;    /* measured natural content height, px */
	int   viewport_h;   /* last viewport height, px */
	int   always_bar;
	int   bar_w;
};

static int sv_type;

/* Natural height of the content column: its padding, its children's preferred
 * heights, and the gaps between them. A child with no PREF_H falls back to its
 * last laid-out height. */
static int
sv_measure(bd_id content)
{
	int pad = bd_get_i(content, BD_PAD_I);
	int gap = bd_get_i(content, BD_GAP_I);
	int sum = 0, n = 0;
	for (bd_id c = bd_first_child(content); c != BD_NONE;
	    c = bd_next_sibling(c)) {
		if (!bd_get_i(c, BD_VISIBLE_B))
			continue;
		int hh = bd_get_i(c, BD_PREF_H_I);
		if (hh <= 0) {
			int cx, cy, cw, ch;
			bd_widget_rect(c, &cx, &cy, &cw, &ch);
			hh = ch;
		}
		sum += hh;
		n++;
	}
	if (n > 1)
		sum += (n - 1) * gap;
	return sum + 2 * pad;
}

/* Push the current scroll offset and content size onto the children. */
static void
sv_apply(struct scrollview *s, int vw, int vh)
{
	s->viewport_h = vh;
	s->content_h  = sv_measure(s->content);

	int max = s->content_h - vh;
	if (max < 0) max = 0;
	if (s->scroll_y > max) s->scroll_y = max;
	if (s->scroll_y < 0)   s->scroll_y = 0;

	int show = s->always_bar || s->content_h > vh;
	int cw = show ? vw - s->bar_w : vw;
	if (cw < 0) cw = 0;

	bd_set(s->content, BD_X_I, 0, BD_Y_I, -s->scroll_y,
	    BD_PREF_W_I, cw, BD_PREF_H_I, s->content_h, BD_END);
	bd_set(s->bar, BD_VISIBLE_B, show, BD_X_I, cw, BD_Y_I, 0,
	    BD_PREF_W_I, s->bar_w, BD_PREF_H_I, vh, BD_END);
	if (show) {
		float pos  = max > 0 ? (float)s->scroll_y / (float)max : 0.0f;
		float frac = s->content_h > 0 ? (float)vh / (float)s->content_h : 1.0f;
		bd_scrollbar_set(s->bar, pos, frac);
	}
}

/* ------------------------------------------------------------------ */
/* class hooks                                                        */
/* ------------------------------------------------------------------ */

static void
sv_layout(bd_id id, void *state, int w, int h)
{
	(void)id;
	sv_apply(state, w, h);
}

static int
sv_event(bd_id id, void *state, const bd_event *ev)
{
	struct scrollview *s = state;
	if (ev->type != BD_EV_MOUSE_SCROLL)
		return 0;
	int max = s->content_h - s->viewport_h;
	if (max <= 0)
		return 0;   /* nothing to scroll; let an outer view have it */
	int step = (int)bd_draw_line_height() * 3;
	s->scroll_y -= (int)(ev->scroll_dy * (float)step);
	if (s->scroll_y < 0)   s->scroll_y = 0;
	if (s->scroll_y > max) s->scroll_y = max;
	bd_set(s->content, BD_Y_I, -s->scroll_y, BD_END);
	bd_scrollbar_set(s->bar, (float)s->scroll_y / (float)max,
	    s->content_h > 0 ? (float)s->viewport_h / (float)s->content_h : 1.0f);
	(void)id;
	return 1;
}

/* the scrollbar was dragged: adopt its position */
static void
sv_bar_changed(bd_id bar, void *data)
{
	(void)data;
	bd_id id = bd_parent(bar);
	struct scrollview *s = bd_widget_state(id);
	if (!s)
		return;
	int max = s->content_h - s->viewport_h;
	if (max < 0) max = 0;
	s->scroll_y = (int)(bd_scrollbar_value(bar) * (float)max + 0.5f);
	bd_set(s->content, BD_Y_I, -s->scroll_y, BD_END);
}

static const bd_widget_class sv_class = {
	.name       = "scrollview",
	.state_size = sizeof(struct scrollview),
	.layout     = sv_layout,
	.event      = sv_event,
	.flags      = BD_WC_CONTAINS_CHILDREN | BD_WC_CLIP_CHILDREN,
};

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

void
bd_scrollview_register(void)
{
	if (!sv_type)
		sv_type = bd_register_widget_class(&sv_class);
}

bd_id
bd_scrollview_create(bd_id parent, const bd_scrollview_desc *desc, ...)
{
	bd_scrollview_register();
	va_list ap;
	va_start(ap, desc);
	bd_id id = bd_create_va(parent, sv_type, ap);
	va_end(ap);
	struct scrollview *s = bd_widget_state(id);
	if (!s)
		return id;
	s->always_bar = desc ? desc->always_bar : 0;
	s->bar_w = SV_BAR_W;

	bd_set(id, BD_LAYOUT_I, BD_LAYOUT_FIXED, BD_PAD_I, 0, BD_END);
	s->content = bd_create(id, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_COL,
	    BD_PAD_I, 0, BD_GAP_I, SV_GAP, BD_END);
	s->bar = bd_create(id, BD_SCROLLBAR, BD_PREF_W_I, s->bar_w,
	    BD_ON_CLICK_F, sv_bar_changed, BD_END);
	return id;
}

bd_id
bd_scrollview_content(bd_id id)
{
	if (bd_widget_type(id) != sv_type)
		return BD_NONE;
	return ((struct scrollview *)bd_widget_state(id))->content;
}

void
bd_scrollview_scroll_to(bd_id id, int y_px)
{
	if (bd_widget_type(id) != sv_type)
		return;
	struct scrollview *s = bd_widget_state(id);
	int max = s->content_h - s->viewport_h;
	if (max < 0) max = 0;
	if (y_px < 0)   y_px = 0;
	if (y_px > max) y_px = max;
	s->scroll_y = y_px;
	bd_set(s->content, BD_Y_I, -s->scroll_y, BD_END);
}

int
bd_scrollview_scroll(bd_id id)
{
	if (bd_widget_type(id) != sv_type)
		return 0;
	return ((struct scrollview *)bd_widget_state(id))->scroll_y;
}

int
bd_scrollview_content_height(bd_id id)
{
	if (bd_widget_type(id) != sv_type)
		return 0;
	return ((struct scrollview *)bd_widget_state(id))->content_h;
}
