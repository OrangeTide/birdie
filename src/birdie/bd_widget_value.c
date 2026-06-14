#include "bd_widget_value.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include <stdarg.h>
#include <stdint.h>

/*
 * Slider value widget. Built on the extension API: the core captures the
 * pointer on press and routes the drag to slider_event, which maps the
 * pointer position along the axis to a normalized value.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#define THUMB   10      /* thumb size along the travel axis, px */
#define GROOVE  6       /* groove thickness across the axis, px */

struct slider {
	int         orient;     /* BD_HORIZONTAL / BD_VERTICAL */
	float       t;          /* [0,1] */
	bd_value_cb cb;
	void       *arg;
};

static int slider_type;

static float
clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

static void
slider_init(bd_id id, void *state)
{
	struct slider *s = state;
	s->orient = BD_HORIZONTAL;
	s->t = 0.0f;
	/* default footprint; caller attributes (applied after init) override */
	bd_set(id, BD_PREF_W_I, 24, BD_PREF_H_I, 20, BD_PAD_I, 4, BD_END);
}

/* travel length of the thumb's center, and the axis coord at t==0 */
static float
travel(bd_id id, const struct slider *s, int *origin)
{
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	int pad = bd_get_i(id, BD_PAD_I);
	if (s->orient == BD_HORIZONTAL) {
		*origin = x + pad + THUMB / 2;
		return (float)(w - 2 * pad - THUMB);
	}
	*origin = y + pad + THUMB / 2;
	return (float)(h - 2 * pad - THUMB);
}

static void
set_from_pointer(bd_id id, struct slider *s, int px, int py)
{
	int origin;
	float span = travel(id, s, &origin);
	if (span <= 0.0f)
		return;
	float pos = (s->orient == BD_HORIZONTAL) ? (float)px : (float)py;
	float t = (pos - (float)origin) / span;
	if (s->orient == BD_VERTICAL)
		t = 1.0f - t;          /* bottom = 0, top = 1 */
	t = clamp01(t);
	if (t != s->t) {
		s->t = t;
		if (s->cb)
			s->cb(id, s->arg, t);
	}
}

static void
slider_render(bd_id id, void *state)
{
	struct slider *s = state;
	const bd_theme *th = bd_gui_theme();
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	int pad = bd_get_i(id, BD_PAD_I);

	if (s->orient == BD_HORIZONTAL) {
		int gy = y + h / 2 - GROOVE / 2;
		bd_draw_rect(x + pad, gy, w - 2 * pad, GROOVE, th->press);
		bd_draw_rect_lines(x + pad, gy, w - 2 * pad, GROOVE, th->border);
		int tx = x + pad + (int)(s->t * (float)(w - 2 * pad - THUMB));
		bd_draw_rect(tx, y + 2, THUMB, h - 4, th->widget);
		bd_draw_rect_lines(tx, y + 2, THUMB, h - 4, th->border);
	} else {
		int gx = x + w / 2 - GROOVE / 2;
		bd_draw_rect(gx, y + pad, GROOVE, h - 2 * pad, th->press);
		bd_draw_rect_lines(gx, y + pad, GROOVE, h - 2 * pad, th->border);
		int ty = y + pad + (int)((1.0f - s->t) *
		    (float)(h - 2 * pad - THUMB));
		bd_draw_rect(x + 2, ty, w - 4, THUMB, th->widget);
		bd_draw_rect_lines(x + 2, ty, w - 4, THUMB, th->border);
	}
}

static int
slider_event(bd_id id, void *state, const bd_event *ev)
{
	struct slider *s = state;
	if (ev->type == BD_EV_MOUSE_DOWN || ev->type == BD_EV_MOUSE_MOVE) {
		set_from_pointer(id, s, ev->x, ev->y);
		return 1;
	}
	return 0;
}

static const bd_widget_class slider_class = {
	.name = "slider",
	.state_size = sizeof(struct slider),
	.init = slider_init,
	.render = slider_render,
	.event = slider_event,
};

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

bd_id
bd_slider_create(bd_id parent, int orient, float value,
    bd_value_cb cb, void *arg, ...)
{
	if (slider_type == 0)
		slider_type = bd_register_widget_class(&slider_class);

	va_list ap;
	va_start(ap, arg);
	bd_id id = bd_create_va(parent, slider_type, ap);
	va_end(ap);

	struct slider *s = bd_widget_state(id);
	if (s) {
		s->orient = orient;
		s->t = clamp01(value);
		s->cb = cb;
		s->arg = arg;
	}
	return id;
}

void
bd_slider_set(bd_id id, float value)
{
	if (bd_widget_type(id) != slider_type)
		return;
	struct slider *s = bd_widget_state(id);
	if (s)
		s->t = clamp01(value);
}

float
bd_slider_get(bd_id id)
{
	if (bd_widget_type(id) != slider_type)
		return 0.0f;
	struct slider *s = bd_widget_state(id);
	return s ? s->t : 0.0f;
}
