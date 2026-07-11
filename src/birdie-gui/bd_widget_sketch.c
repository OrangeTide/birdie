#include "bd_widget_sketch.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include "bd_theme.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/*
 * Sketch pad extension. Strokes are polylines of sampled points, each with
 * a half-width derived from the stylus pressure (and widened by tilt). They are
 * rendered as a run of convex quads (one per segment) with a square dab stamped
 * at every sample to cap the ends and fill the joins, so a fast sparse stroke
 * still reads as a continuous variable-width line. The eraser end removes whole
 * strokes it passes over. All ink is scissor-clipped to the sketch interior.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

static int sketch_type;

struct cpoint { float x, y, hw; };   /* hw = nib half-width here */

struct stroke {
	struct cpoint *pts;
	int            n, cap;
	uint32_t       color;
};

struct sketch {
	struct stroke *strokes;
	int            n, cap;
	struct stroke  cur;          /* in-progress stroke */
	int            drawing;      /* tip is down, building cur */
	int            erasing;      /* current pass is the eraser */
	uint32_t       ink, alt_ink; /* tip ink, barrel-button ink */
	float          nib;          /* nib width (px) at full pressure */
	int            hover;        /* pen in proximity, show the nib cursor */
	float          hx, hy, hhw;  /* hover cursor centre + half-width */
};

static void
sketch_init(bd_id id, void *state)
{
	(void)id;
	struct sketch *c = state;
	c->ink = 0x202428ffu;        /* near-black ink */
	c->alt_ink = 0xc02418ffu;    /* red, for the barrel button */
	c->nib = 6.0f;
}

static void
stroke_free(struct stroke *s)
{
	free(s->pts);
	s->pts = NULL;
	s->n = s->cap = 0;
}

static void
sketch_destroy(bd_id id, void *state)
{
	(void)id;
	struct sketch *c = state;
	for (int i = 0; i < c->n; i++)
		stroke_free(&c->strokes[i]);
	free(c->strokes);
	stroke_free(&c->cur);
}

static void
stroke_push(struct stroke *s, float x, float y, float hw)
{
	if (s->n == s->cap) {
		int cap = s->cap ? s->cap * 2 : 32;
		struct cpoint *p = realloc(s->pts, (size_t)cap * sizeof *p);
		if (!p)
			return;
		s->pts = p;
		s->cap = cap;
	}
	s->pts[s->n].x = x;
	s->pts[s->n].y = y;
	s->pts[s->n].hw = hw;
	s->n++;
}

/* Push the finished current stroke onto the sketch. */
static void
sketch_commit(struct sketch *c)
{
	if (c->cur.n < 1) {
		stroke_free(&c->cur);
		return;
	}
	if (c->n == c->cap) {
		int cap = c->cap ? c->cap * 2 : 16;
		struct stroke *s = realloc(c->strokes, (size_t)cap * sizeof *s);
		if (!s) {
			stroke_free(&c->cur);
			return;
		}
		c->strokes = s;
		c->cap = cap;
	}
	c->strokes[c->n++] = c->cur;
	c->cur = (struct stroke){0};
}

/* Half-width for a sample from pressure and tilt. A light touch still leaves a
 * thin line; a tilted pen broadens the nib (a real chisel nib lays down more
 * ink when leaned over). */
static float
nib_halfwidth(const struct sketch *c, const bd_event *ev)
{
	float p = ev->pressure;
	if (p <= 0.0f)
		p = 1.0f;            /* mouse / no-pressure source: full width */
	float tilt = sqrtf(ev->tilt_x * ev->tilt_x + ev->tilt_y * ev->tilt_y);
	float broaden = 1.0f + 0.5f * (tilt / 90.0f);
	float w = c->nib * (0.18f + 0.82f * p) * broaden;
	return w * 0.5f;
}

/* Remove strokes with any point within r of (px,py) — the eraser. */
static void
sketch_erase_at(struct sketch *c, float px, float py, float r)
{
	float r2 = r * r;
	for (int i = 0; i < c->n; ) {
		struct stroke *s = &c->strokes[i];
		int hit = 0;
		for (int k = 0; k < s->n; k++) {
			float dx = s->pts[k].x - px, dy = s->pts[k].y - py;
			if (dx * dx + dy * dy <= r2) { hit = 1; break; }
		}
		if (hit) {
			stroke_free(s);
			c->strokes[i] = c->strokes[--c->n];
		} else {
			i++;
		}
	}
}

static int
sketch_event(bd_id id, void *state, const bd_event *ev)
{
	struct sketch *c = state;
	int eraser = (ev->pen_flags & BD_PEN_ERASER) != 0;
	float hw;
	(void)id;

	switch (ev->type) {
	case BD_EV_PEN_HOVER:
		c->hover = (ev->pen_flags & BD_PEN_INRANGE) != 0;
		c->hx = (float)ev->x;
		c->hy = (float)ev->y;
		c->hhw = nib_halfwidth(c, ev);
		return 1;

	case BD_EV_PEN_DOWN:
	case BD_EV_MOUSE_DOWN:
		c->hover = 0;
		c->erasing = eraser;
		hw = nib_halfwidth(c, ev);
		if (eraser) {
			sketch_erase_at(c, (float)ev->x, (float)ev->y, hw + 4.0f);
			c->drawing = 1;
			return 1;
		}
		c->cur = (struct stroke){0};
		c->cur.color = (ev->pen_flags & BD_PEN_BARREL) ? c->alt_ink
		    : c->ink;
		stroke_push(&c->cur, (float)ev->x, (float)ev->y, hw);
		c->drawing = 1;
		return 1;

	case BD_EV_PEN_MOVE:
	case BD_EV_MOUSE_MOVE:
		if (!c->drawing)
			return 1;
		hw = nib_halfwidth(c, ev);
		if (c->erasing) {
			sketch_erase_at(c, (float)ev->x, (float)ev->y, hw + 4.0f);
			return 1;
		}
		stroke_push(&c->cur, (float)ev->x, (float)ev->y, hw);
		return 1;

	case BD_EV_PEN_UP:
	case BD_EV_MOUSE_UP:
		if (c->drawing && !c->erasing)
			sketch_commit(c);
		c->drawing = 0;
		c->erasing = 0;
		return 1;
	}
	return 0;
}

/* Draw one variable-width stroke: a quad per segment plus a square dab at each
 * sample to cap ends and fill the joins. */
static void
draw_stroke(const struct stroke *s)
{
	for (int k = 0; k < s->n; k++) {
		float hw = s->pts[k].hw;
		bd_draw_rect(s->pts[k].x - hw, s->pts[k].y - hw,
		    hw * 2.0f, hw * 2.0f, s->color);
	}
	for (int k = 0; k + 1 < s->n; k++) {
		float ax = s->pts[k].x,   ay = s->pts[k].y,   aw = s->pts[k].hw;
		float bx = s->pts[k+1].x, by = s->pts[k+1].y, bw = s->pts[k+1].hw;
		float dx = bx - ax, dy = by - ay;
		float len = sqrtf(dx * dx + dy * dy);
		if (len < 0.0001f)
			continue;
		float nx = -dy / len, ny = dx / len;   /* unit normal */
		bd_draw_quad(ax + nx * aw, ay + ny * aw,
		             ax - nx * aw, ay - ny * aw,
		             bx - nx * bw, by - ny * bw,
		             bx + nx * bw, by + ny * bw, s->color);
	}
}

static void
sketch_render(bd_id id, void *state)
{
	struct sketch *c = state;
	const bd_theme *th = bd_gui_theme();
	const bd_backend *be = bd_backend_get();
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);

	/* recessed paper surface */
	bd_draw_rect(x, y, w, h, 0xf7f4ecffu);
	bd_draw_rect_lines(x, y, w, h, th->border);

	bd_draw_flush();
	be->scissor(x + 1, y + 1, w - 2, h - 2);
	for (int i = 0; i < c->n; i++)
		draw_stroke(&c->strokes[i]);
	if (c->drawing && !c->erasing)
		draw_stroke(&c->cur);
	/* hover nib cursor: a hollow ring approximated by a small square outline */
	if (c->hover) {
		float r = c->hhw < 2.0f ? 2.0f : c->hhw;
		bd_draw_rect_lines(c->hx - r, c->hy - r, r * 2.0f, r * 2.0f,
		    th->border);
	}
	bd_draw_flush();
	be->scissor_off();
}

static const bd_widget_class sketch_class = {
	.name = "sketch",
	.state_size = sizeof(struct sketch),
	.init = sketch_init,
	.destroy = sketch_destroy,
	.render = sketch_render,
	.event = sketch_event,
};

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

bd_id
bd_sketch_create(bd_id parent, ...)
{
	if (sketch_type == 0)
		sketch_type = bd_register_widget_class(&sketch_class);

	va_list ap;
	va_start(ap, parent);
	bd_id id = bd_create_va(parent, sketch_type, ap);
	va_end(ap);
	return id;
}

void
bd_sketch_set_ink(bd_id id, uint32_t rgba)
{
	if (bd_widget_type(id) != sketch_type)
		return;
	struct sketch *c = bd_widget_state(id);
	if (c)
		c->ink = rgba;
}

void
bd_sketch_set_alt_ink(bd_id id, uint32_t rgba)
{
	if (bd_widget_type(id) != sketch_type)
		return;
	struct sketch *c = bd_widget_state(id);
	if (c)
		c->alt_ink = rgba;
}

void
bd_sketch_set_nib(bd_id id, float px)
{
	if (bd_widget_type(id) != sketch_type)
		return;
	struct sketch *c = bd_widget_state(id);
	if (c && px > 0.0f)
		c->nib = px;
}

void
bd_sketch_clear(bd_id id)
{
	if (bd_widget_type(id) != sketch_type)
		return;
	struct sketch *c = bd_widget_state(id);
	if (!c)
		return;
	for (int i = 0; i < c->n; i++)
		stroke_free(&c->strokes[i]);
	c->n = 0;
	stroke_free(&c->cur);
	c->drawing = 0;
}

int
bd_sketch_stroke_count(bd_id id)
{
	if (bd_widget_type(id) != sketch_type)
		return 0;
	struct sketch *c = bd_widget_state(id);
	return c ? c->n : 0;
}
