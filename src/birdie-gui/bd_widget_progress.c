/*
 * bd_widget_progress.c — a plain determinate/indeterminate progress bar.
 *
 * The simple sibling of BD_METER_BAR: a track with a fill to a 0..1 fraction,
 * an optional percent readout, and an optional indeterminate marquee. Pure
 * batched quads plus text; no shader. See bd_widget_progress.h.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_widget_progress.h"
#include "bd_widget_value.h"   /* BD_HORIZONTAL / BD_VERTICAL */
#include "widget_ext.h"
#include "bd_draw.h"

#include <math.h>
#include <stdio.h>

#define PROG_DEF_ACCENT 0x4A90D9FFu   /* a calm blue */
#define PROG_DEF_THICK  16

struct progress {
	float    value;
	int      indeterminate;
	int      show_percent;
	int      orient;
	uint32_t color;
};

static int prog_type;

static struct progress *
prog_of(bd_id id)
{
	if (bd_widget_type(id) != prog_type)
		return NULL;
	return bd_widget_state(id);
}

static void
prog_render(bd_id id, void *state)
{
	struct progress *p = state;
	const bd_theme *th = bd_gui_theme();
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);

	const char *label = bd_get_s(id, BD_LABEL_S);
	int lh = (int)ceilf(bd_draw_line_height());
	int bx = x, bw = w;
	if (label && label[0]) {
		int lw = (int)ceilf(bd_draw_text_width(label)) + 6;
		if (lw < w - 10) { bx = x + lw; bw = w - lw; }
		bd_draw_text(label, (float)x, (float)(y + (h - lh) * 0.5f), th->text);
	}

	bd_draw_rect(bx, y, bw, h, th->press);          /* track */
	bd_draw_rect_lines(bx, y, bw, h, th->border);

	int vert = (p->orient == BD_VERTICAL);
	int len = vert ? h : bw;

	if (p->indeterminate) {
		/* a chunk (~30% of the track) sliding back and forth */
		const bd_backend *be = bd_backend_get();
		double t = be->time ? be->time() : 0.0;
		float phase = (float)(t - floor(t / 2.0) * 2.0) / 2.0f;   /* 0..1 over 2s */
		float tri = phase < 0.5f ? phase * 2.0f : (1.0f - phase) * 2.0f;
		float chunk = 0.3f;
		float pos = tri * (1.0f - chunk);
		int a = (int)lroundf(pos * len);
		int b = (int)lroundf((pos + chunk) * len);
		if (vert) bd_draw_rect(bx + 1, y + h - b, bw - 2, b - a, p->color);
		else      bd_draw_rect(bx + a + 1, y + 1, b - a, h - 2, p->color);
		return;
	}

	int fill = (int)lroundf(p->value * len);
	if (fill > 0) {
		if (vert) bd_draw_rect(bx + 1, y + h - fill, bw - 2, fill - 1, p->color);
		else      bd_draw_rect(bx + 1, y + 1, fill - 1, h - 2, p->color);
	}

	if (p->show_percent) {
		char buf[8];
		snprintf(buf, sizeof buf, "%d%%", (int)lroundf(p->value * 100.0f));
		float tw = bd_draw_text_width(buf);
		bd_draw_text(buf, (float)bx + (bw - tw) * 0.5f,
		    (float)(y + (h - lh) * 0.5f), th->text_hi);
	}
}

static const bd_widget_class prog_class = {
	.name = "progress",
	.state_size = sizeof(struct progress),
	.render = prog_render,
};

bd_id
bd_progress_create(bd_id parent, const bd_progress_desc *desc, ...)
{
	if (prog_type == 0)
		prog_type = bd_register_widget_class(&prog_class);

	va_list ap;
	va_start(ap, desc);
	bd_id id = bd_create_va(parent, prog_type, ap);
	va_end(ap);

	struct progress *p = bd_widget_state(id);
	if (p) {
		p->color = PROG_DEF_ACCENT;
		if (desc) {
			p->value = desc->value < 0 ? 0 : (desc->value > 1 ? 1 : desc->value);
			p->indeterminate = desc->indeterminate;
			p->show_percent = desc->show_percent;
			p->orient = desc->orient;
			if (desc->color) p->color = desc->color;
			if (desc->label) bd_set(id, BD_LABEL_S, desc->label, BD_END);
		}
		if (p->orient == BD_VERTICAL)
			bd_set(id, BD_PREF_W_I, PROG_DEF_THICK, BD_PREF_H_I, 120, BD_END);
		else
			bd_set(id, BD_PREF_W_I, 160, BD_PREF_H_I, PROG_DEF_THICK, BD_END);
	}
	return id;
}

void
bd_progress_set(bd_id id, float value)
{
	struct progress *p = prog_of(id);
	if (!p) return;
	if (value < 0.0f) value = 0.0f;
	if (value > 1.0f) value = 1.0f;
	p->value = value;
}

float
bd_progress_get(bd_id id)
{
	struct progress *p = prog_of(id);
	return p ? p->value : 0.0f;
}

void
bd_progress_set_indeterminate(bd_id id, int on)
{
	struct progress *p = prog_of(id);
	if (p) p->indeterminate = on ? 1 : 0;
}
