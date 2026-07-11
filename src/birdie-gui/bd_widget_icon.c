/*
 * bd_widget_icon -- the shared icon cell (dock / action bar / inventory build
 * on the helpers here) plus a standalone single-icon widget used as an app
 * launcher or a desktop icon. See bd_widget_icon.h.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_widget_icon.h"
#include "bd_draw.h"
#include "bd_theme.h"

#include <stdlib.h>
#include <string.h>

#define ICON_PAD        4
#define ICON_THRESHOLD  4     /* px before a press becomes a drag */
#define ICON_DBLCLICK_S 0.4
#define ICON_LABEL      64

/* ---- shared cell helpers ---- */

void
bd_icon_draw(float rx, float ry, int cell_w, int pad, int icon_size,
    const bd_icon_desc *d, uint32_t bg, uint32_t border, uint32_t fg)
{
	if (!d)
		return;
	bd_draw_tile(rx, ry, cell_w, pad, icon_size,
	    d->icon, d->label, d->count, d->enabled, bg, border, fg);
}

void
bd_icon_dnd_begin(bd_id source, const bd_icon_desc *d, void *user)
{
	if (!d)
		return;
	bd_dnd_payload p = {0};
	p.source = source;
	p.key    = d->key;
	p.label  = d->label;
	p.icon   = d->icon;
	p.user   = user;
	bd_dnd_begin(&p);
}

/* ---- standalone icon widget ---- */

struct icon {
	bd_icon_desc desc;
	char         label[ICON_LABEL];
	bd_icon_activate_fn on_activate;
	void        *act_user;
	bd_icon_drop_fn on_drop;
	void        *drop_user;
	int          press, dragging, px, py;
	double       last_time;   /* for double-click */
};

static int icon_type;

static void
set_label(struct icon *e, const char *s)
{
	if (s && s[0]) {
		strncpy(e->label, s, ICON_LABEL - 1);
		e->label[ICON_LABEL - 1] = '\0';
		e->desc.label = e->label;
	} else {
		e->label[0] = '\0';
		e->desc.label = NULL;
	}
}

static void
icon_activate(bd_id id, struct icon *e)
{
	if (e->on_activate)
		e->on_activate(id, e->desc.key, e->act_user);
}

static void
icon_render(bd_id id, void *state)
{
	struct icon *e = state;
	const bd_theme *th = bd_gui_theme();
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);

	int lh = (e->desc.label && e->desc.label[0]) ? (int)bd_draw_line_height() : 0;
	int is = w - 2 * ICON_PAD;
	if (is > h - 2 * ICON_PAD - lh)
		is = h - 2 * ICON_PAD - lh;
	if (is < 4)
		is = 4;

	bd_icon_draw((float)x, (float)y, w, ICON_PAD, is, &e->desc,
	    th->bg, th->border, th->text);

	if (bd_focused() == id)
		bd_draw_rect_lines((float)(x + ICON_PAD), (float)(y + ICON_PAD),
		    (float)is, (float)is, th->focus);
}

static int
icon_event(bd_id id, void *state, const bd_event *ev)
{
	struct icon *e = state;

	switch (ev->type) {
	case BD_EV_MOUSE_DOWN:
		if (ev->button != BD_MOUSE_LEFT)
			return 1;
		e->press = 1;
		e->dragging = 0;
		e->px = ev->x;
		e->py = ev->y;
		return 1;

	case BD_EV_MOUSE_MOVE:
		if (e->press && !e->dragging &&
		    (abs(ev->x - e->px) > ICON_THRESHOLD ||
		     abs(ev->y - e->py) > ICON_THRESHOLD)) {
			e->dragging = 1;
			bd_icon_dnd_begin(id, &e->desc, NULL);
		}
		return 1;

	case BD_EV_MOUSE_UP:
		if (e->press && !e->dragging) {
			double t = bd_time();
			if (t - e->last_time < ICON_DBLCLICK_S) {
				icon_activate(id, e);
				e->last_time = 0;
			} else {
				e->last_time = t;
			}
		}
		e->press = 0;
		e->dragging = 0;
		return 1;

	case BD_EV_DROP: {
		if (!e->on_drop)
			return 0;
		const bd_dnd_payload *p = bd_dnd_get();
		int acc = e->on_drop(id, p, e->drop_user);
		if (acc && p) {
			e->desc.key = p->key;
			e->desc.icon = p->icon;
			e->desc.enabled = 1;
			set_label(e, p->label);
		}
		return acc ? 1 : 0;
	}

	case BD_EV_KEY_DOWN:
		if (ev->key == BD_KEY_ENTER) {
			icon_activate(id, e);
			return 1;
		}
		return 0;

	default:
		return 0;
	}
}

static const bd_widget_class icon_class = {
	.name = "icon",
	.state_size = sizeof(struct icon),
	.render = icon_render,
	.event = icon_event,
};

static struct icon *
icon_of(bd_id id)
{
	if (bd_widget_type(id) != icon_type)
		return NULL;
	return bd_widget_state(id);
}

bd_id
bd_icon_create(bd_id parent, const bd_icon_desc *desc, ...)
{
	if (!icon_type)
		icon_type = bd_register_widget_class(&icon_class);

	va_list ap;
	va_start(ap, desc);
	bd_id id = bd_create_va(parent, icon_type, ap);
	va_end(ap);

	struct icon *e = bd_widget_state(id);
	if (!e)
		return id;
	e->desc.enabled = 1;
	if (desc) {
		e->desc = *desc;
		set_label(e, desc->label);
	}
	bd_set(id, BD_PREF_W_I, 64, BD_PREF_H_I, 72, BD_END);
	return id;
}

void
bd_icon_set(bd_id id, const bd_icon_desc *desc)
{
	struct icon *e = icon_of(id);
	if (!e || !desc)
		return;
	const char *lbl = desc->label;
	e->desc = *desc;
	set_label(e, lbl);
}

void
bd_icon_set_texture(bd_id id, bd_texture tex)
{
	struct icon *e = icon_of(id);
	if (e) e->desc.icon = tex;
}

uint64_t
bd_icon_key(bd_id id)
{
	struct icon *e = icon_of(id);
	return e ? e->desc.key : 0;
}

void
bd_icon_on_activate(bd_id id, bd_icon_activate_fn fn, void *user)
{
	struct icon *e = icon_of(id);
	if (!e) return;
	e->on_activate = fn;
	e->act_user = user;
}

void
bd_icon_on_drop(bd_id id, bd_icon_drop_fn fn, void *user)
{
	struct icon *e = icon_of(id);
	if (!e) return;
	e->on_drop = fn;
	e->drop_user = user;
}
