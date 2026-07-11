/*
 * bd_widget_chart -- scrolling multi-series time-series strip chart. Drawn as
 * colored ink traces on a graph-paper grid (a paper chart recorder). Each
 * series is a ring buffer the app pushes samples into; the widget autoscales
 * each series over its window, pins "%" series to 0..100, and gives up to two
 * non-percentage series a labeled value axis (left, then right). See
 * bd_widget_chart.h.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_widget_chart.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include "bd_theme.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define CHART_MAX_SERIES 8
#define CHART_DEF_WINDOW 120
#define CHART_MIN_WINDOW 8
#define CHART_MAX_WINDOW 4096
#define AXIS_W           40      /* px reserved for a value axis */

/* paper chart-recorder palette */
#define PAPER_BG   0xEAF0E0FFu   /* pale green graph paper */
#define GRID_MAJOR 0x9FB48FFFu
#define GRID_MINOR 0xC3D2B4FFu
#define BEZEL      0x1C1F24FFu

static const uint32_t PALETTE[CHART_MAX_SERIES] = {
	0xC22E2EFFu, 0x2E6FC2FFu, 0x2FA84FFFu, 0xD08A2EFFu,
	0x8A4FC0FFu, 0x1F9AA8FFu, 0xB0243BFFu, 0x555F6BFFu,
};

struct cseries {
	char    *label;
	char    *unit;
	uint32_t color;
	int      is_pct;
	float   *ring;
	int      count;      /* valid samples, <= window */
	int      head;       /* next write slot */
};

struct chart {
	int window;
	struct cseries s[CHART_MAX_SERIES];
	int nseries;
	int grid, legend;
};

static int chart_type;

/* j-th sample of a series, oldest (0) to newest (count-1). */
static float
samp(const struct cseries *cs, int window, int j)
{
	int idx = (cs->head - cs->count + j + window * 2) % window;
	return cs->ring[idx];
}

/* lo/hi range for a series: 0..100 for "%", else autoscaled with headroom. */
static void
series_range(const struct cseries *cs, int window, float *lo, float *hi)
{
	if (cs->is_pct) { *lo = 0.0f; *hi = 100.0f; return; }
	float mn = 0, mx = 0;
	int first = 1;
	for (int j = 0; j < cs->count; j++) {
		float v = samp(cs, window, j);
		if (first) { mn = mx = v; first = 0; }
		else { if (v < mn) mn = v; if (v > mx) mx = v; }
	}
	if (first) { *lo = 0.0f; *hi = 1.0f; return; }
	if (mx <= mn) { mx = mn + 1.0f; }
	float pad = (mx - mn) * 0.08f;
	*lo = mn - pad;
	*hi = mx + pad;
}

static void
fmt_val(char *buf, size_t cap, float v)
{
	if (fabsf(v) >= 100.0f || v == floorf(v))
		snprintf(buf, cap, "%.0f", v);
	else
		snprintf(buf, cap, "%.1f", v);
}

/* A line segment as a thin quad. */
static void
seg(float x0, float y0, float x1, float y1, float w, uint32_t c)
{
	float dx = x1 - x0, dy = y1 - y0;
	float len = sqrtf(dx * dx + dy * dy);
	if (len < 0.001f) {
		bd_draw_rect(x0 - w * 0.5f, y0 - w * 0.5f, w, w, c);
		return;
	}
	float nx = -dy / len * (w * 0.5f), ny = dx / len * (w * 0.5f);
	bd_draw_quad(x0 + nx, y0 + ny, x1 + nx, y1 + ny,
	    x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}

static void
plot_series(const struct cseries *cs, int window, int px, int py,
    int pw, int ph)
{
	if (cs->count < 1)
		return;
	float lo, hi;
	series_range(cs, window, &lo, &hi);
	float span = hi - lo;
	if (span < 1e-6f)
		span = 1.0f;
	float dx = window > 1 ? (float)pw / (float)(window - 1) : 0.0f;
	float px0 = 0, py0 = 0;
	for (int j = 0; j < cs->count; j++) {
		float v = samp(cs, window, j);
		/* newest (j == count-1) sits at the right edge */
		float x = px + pw - (float)(cs->count - 1 - j) * dx;
		float t = (v - lo) / span;
		if (t < 0) t = 0;
		if (t > 1) t = 1;
		float y = py + ph - t * ph;
		if (j > 0)
			seg(px0, py0, x, y, 1.6f, cs->color);
		px0 = x;
		py0 = y;
	}
}

/* left-aligned (side 0) or right-aligned (side 1) value axis for a series. */
static void
draw_axis(const struct cseries *cs, int window, int edge_x, int py, int ph,
    int side)
{
	float lo, hi;
	series_range(cs, window, &lo, &hi);
	char top[24], bot[24];
	fmt_val(top, sizeof top, hi);
	fmt_val(bot, sizeof bot, lo);
	int lh = (int)bd_draw_line_height();
	const char *unit = cs->unit && cs->unit[0] ? cs->unit : "";
	/* unit + max near the top, min near the bottom, in the ink color */
	if (side == 0) {
		bd_draw_text(unit, (float)edge_x, (float)(py - lh - 1), cs->color);
		bd_draw_text(top,  (float)edge_x, (float)py, cs->color);
		bd_draw_text(bot,  (float)edge_x, (float)(py + ph - lh), cs->color);
	} else {
		float uw = bd_draw_text_width(unit);
		float tw = bd_draw_text_width(top);
		float bw = bd_draw_text_width(bot);
		bd_draw_text(unit, (float)edge_x - uw, (float)(py - lh - 1), cs->color);
		bd_draw_text(top,  (float)edge_x - tw, (float)py, cs->color);
		bd_draw_text(bot,  (float)edge_x - bw, (float)(py + ph - lh), cs->color);
	}
}

static void
chart_render(bd_id id, void *state)
{
	struct chart *c = state;
	const bd_theme *th = bd_gui_theme();
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);

	bd_draw_rect(x, y, w, h, PAPER_BG);
	bd_draw_rect_lines(x, y, w, h, BEZEL);

	int lh = (int)bd_draw_line_height();

	/* which series claim a value axis (non-%, has a unit): first two */
	int left = -1, right = -1;
	for (int i = 0; i < c->nseries; i++) {
		if (c->s[i].is_pct || !(c->s[i].unit && c->s[i].unit[0]))
			continue;
		if (left < 0) left = i;
		else if (right < 0) { right = i; break; }
	}

	int top_m = (c->legend && c->nseries) ? lh + 4 : 3;
	int left_m = left >= 0 ? AXIS_W : 4;
	int right_m = right >= 0 ? AXIS_W : 4;
	int bot_m = 4;
	int px = x + left_m, py = y + top_m;
	int pw = w - left_m - right_m, ph = h - top_m - bot_m;
	if (pw < 4 || ph < 4)
		return;

	/* graph-paper grid */
	if (c->grid) {
		for (int k = 0; k <= 8; k++) {   /* vertical time divisions */
			int gx = px + k * pw / 8;
			bd_draw_rect(gx, py, 1, ph, k % 2 ? GRID_MINOR : GRID_MAJOR);
		}
		for (int k = 0; k <= 4; k++) {   /* horizontal levels (0..100%) */
			int gy = py + k * ph / 4;
			bd_draw_rect(px, gy, pw, 1, GRID_MAJOR);
		}
	}

	/* traces */
	for (int i = 0; i < c->nseries; i++)
		plot_series(&c->s[i], c->window, px, py, pw, ph);

	/* value axes for up to two non-% series */
	if (left >= 0)
		draw_axis(&c->s[left], c->window, x + 3, py, ph, 0);
	if (right >= 0)
		draw_axis(&c->s[right], c->window, x + w - 3, py, ph, 1);

	/* legend: swatch + label + current value, left to right */
	if (c->legend && c->nseries) {
		int lx = x + left_m;
		int ly = y + 2;
		for (int i = 0; i < c->nseries; i++) {
			struct cseries *cs = &c->s[i];
			bd_draw_rect(lx, ly + 2, 10, lh - 4, cs->color);
			lx += 14;
			char buf[64];
			if (cs->count > 0) {
				char vb[24];
				fmt_val(vb, sizeof vb, samp(cs, c->window, cs->count - 1));
				snprintf(buf, sizeof buf, "%s %s%s",
				    cs->label ? cs->label : "", vb,
				    cs->unit ? cs->unit : "");
			} else {
				snprintf(buf, sizeof buf, "%s",
				    cs->label ? cs->label : "");
			}
			bd_draw_text(buf, (float)lx, (float)ly, th->text);
			lx += (int)bd_draw_text_width(buf) + 14;
			if (lx > x + w - 8)
				break;
		}
	}
}

/* ---- class ---- */

static void
chart_destroy(bd_id id, void *state)
{
	struct chart *c = state;
	(void)id;
	for (int i = 0; i < c->nseries; i++) {
		free(c->s[i].label);
		free(c->s[i].unit);
		free(c->s[i].ring);
	}
}

static const bd_widget_class chart_class = {
	.name = "chart",
	.state_size = sizeof(struct chart),
	.destroy = chart_destroy,
	.render = chart_render,
};

static struct chart *
chart_of(bd_id id)
{
	if (bd_widget_type(id) != chart_type)
		return NULL;
	return bd_widget_state(id);
}

bd_id
bd_chart_create(bd_id parent, int window, ...)
{
	if (!chart_type)
		chart_type = bd_register_widget_class(&chart_class);

	va_list ap;
	va_start(ap, window);
	bd_id id = bd_create_va(parent, chart_type, ap);
	va_end(ap);

	struct chart *c = bd_widget_state(id);
	if (!c)
		return id;
	if (window <= 0)
		window = CHART_DEF_WINDOW;
	if (window < CHART_MIN_WINDOW)
		window = CHART_MIN_WINDOW;
	if (window > CHART_MAX_WINDOW)
		window = CHART_MAX_WINDOW;
	c->window = window;
	c->grid = 1;
	c->legend = 1;
	bd_set(id, BD_PREF_W_I, 240, BD_PREF_H_I, 120, BD_END);
	return id;
}

int
bd_chart_add_series(bd_id id, const bd_chart_series *s)
{
	struct chart *c = chart_of(id);
	if (!c || c->nseries >= CHART_MAX_SERIES)
		return -1;
	struct cseries *cs = &c->s[c->nseries];
	memset(cs, 0, sizeof *cs);
	cs->ring = calloc((size_t)c->window, sizeof *cs->ring);
	if (!cs->ring)
		return -1;
	cs->color = PALETTE[c->nseries];
	if (s) {
		if (s->label) cs->label = strdup(s->label);
		if (s->unit)  cs->unit = strdup(s->unit);
		if (s->color) cs->color = s->color;
		cs->is_pct = (s->unit && strcmp(s->unit, "%") == 0);
	}
	return c->nseries++;
}

void
bd_chart_push(bd_id id, int series, float value)
{
	struct chart *c = chart_of(id);
	if (!c || series < 0 || series >= c->nseries)
		return;
	struct cseries *cs = &c->s[series];
	cs->ring[cs->head] = value;
	cs->head = (cs->head + 1) % c->window;
	if (cs->count < c->window)
		cs->count++;
}

void
bd_chart_push_row(bd_id id, const float *values, int n)
{
	struct chart *c = chart_of(id);
	if (!c || !values)
		return;
	if (n > c->nseries)
		n = c->nseries;
	for (int i = 0; i < n; i++)
		bd_chart_push(id, i, values[i]);
}

void
bd_chart_clear(bd_id id)
{
	struct chart *c = chart_of(id);
	if (!c)
		return;
	for (int i = 0; i < c->nseries; i++) {
		c->s[i].count = 0;
		c->s[i].head = 0;
	}
}

void
bd_chart_set_grid(bd_id id, int on)
{
	struct chart *c = chart_of(id);
	if (c) c->grid = on ? 1 : 0;
}

void
bd_chart_set_legend(bd_id id, int on)
{
	struct chart *c = chart_of(id);
	if (c) c->legend = on ? 1 : 0;
}
