/*
 * bd_widget_meter.c — compact 0..1 meters in several instrument styles.
 *
 * Output-only extension widget (widget_ext.h), the same recipe as the knob and
 * indicator. One value drives five appearances: a level bar, a VU needle, a
 * magic-eye tube, a pie, and a liquid vial. Color zones split 0..1 into bands
 * (parsed like the indicator's color list); an optional peak marker and optional
 * VU / peak-hold ballistics round out a faithful panel meter. BAR/VU/PIE draw
 * with the batched quad primitives; EYE and VIAL are one fragment shader.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_widget_meter.h"
#include "bd_widget_value.h"   /* BD_HORIZONTAL / BD_VERTICAL */
#include "widget_ext.h"
#include "bd_draw.h"
#include "bd_color.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#define METER_MAX_ZONES 8
#define METER_DEF_ACCENT 0x33CC55FFu   /* a calm green */
#define METER_DEF_DIAM   64
#define METER_DEF_THICK  18

struct meter {
	int      style;
	uint32_t zones[METER_MAX_ZONES];   /* low->high band colors */
	int      nzones;
	float    brk[METER_MAX_ZONES];     /* nzones-1 ascending break points */
	int      nbrk;
	int      peak_on;
	int      ballistic;
	int      orient;
	int      segments;
	int      size;

	float    target;   /* set by the host */
	float    disp;      /* currently displayed (trails target under ballistics) */
	float    peak;      /* peak-hold marker */
	double   last_t;    /* last tick time, for ballistics dt */
};

/* ------------------------------------------------------------------ */
/* color + stops parsing (color parser: shared core bd_color.h)       */
/* ------------------------------------------------------------------ */

/* Parse the zone color list, defaulting an empty/all-unparsed string to one
 * green accent. Returns N (>= 1). */
static int
parse_colors(const char *s, uint32_t *out)
{
	int n = bd_color_parse_list(s, out, METER_MAX_ZONES);
	if (n == 0)
		out[n++] = METER_DEF_ACCENT;
	return n;
}

/* Parse up to `max` floats from a space/comma-separated string. Returns count. */
static int
parse_stops(const char *s, float *out, int max)
{
	int n = 0;
	if (!s) return 0;
	while (*s && n < max) {
		while (*s == ' ' || *s == ',' || *s == '\t' || *s == '\n') s++;
		if (!*s) break;
		char *end;
		float v = strtof(s, &end);
		if (end == s) break;
		if (v < 0.0f) v = 0.0f;
		if (v > 1.0f) v = 1.0f;
		out[n++] = v;
		s = end;
	}
	return n;
}

/* Fill m->brk with nzones-1 ascending break points: explicit stops if given and
 * well-formed, else an even split. */
static void
set_breaks(struct meter *m, const char *stops)
{
	int want = m->nzones - 1;
	if (want <= 0) { m->nbrk = 0; return; }
	float tmp[METER_MAX_ZONES];
	int got = parse_stops(stops, tmp, want);
	if (got == want) {
		for (int i = 0; i < want; i++) m->brk[i] = tmp[i];
	} else {
		for (int i = 0; i < want; i++) m->brk[i] = (float)(i + 1) / m->nzones;
	}
	m->nbrk = want;
}

/* The zone color for a value: the band it falls in. */
static uint32_t
zone_color(const struct meter *m, float v)
{
	int i = 0;
	while (i < m->nbrk && v >= m->brk[i]) i++;
	if (i >= m->nzones) i = m->nzones - 1;
	return m->zones[i];
}

/* ------------------------------------------------------------------ */
/* ballistics                                                         */
/* ------------------------------------------------------------------ */

static void
meter_tick(struct meter *m)
{
	const bd_backend *be = bd_backend_get();
	double dt = 0.0;
	if (be->time) {
		double now = be->time();
		if (m->last_t > 0.0 && now > m->last_t) {
			dt = now - m->last_t;
			if (dt > 0.1) dt = 0.1;
		}
		m->last_t = now;
	}

	if (m->ballistic == BD_METER_VU_BALLISTIC && dt > 0.0) {
		double k = 1.0 - exp(-dt / 0.065);   /* ~300 ms to 99% */
		m->disp += (float)((m->target - m->disp) * k);
	} else if (m->ballistic == BD_METER_PEAK_HOLD && dt > 0.0) {
		if (m->target >= m->disp) m->disp = m->target;         /* snap up */
		else { m->disp -= (float)(dt * 0.8);                    /* fall slow */
		       if (m->disp < m->target) m->disp = m->target; }
	} else {
		m->disp = m->target;   /* exact, no clock, or first frame */
	}
	if (m->disp < 0.0f) m->disp = 0.0f;
	if (m->disp > 1.0f) m->disp = 1.0f;

	if (m->peak_on) {
		if (m->disp >= m->peak) m->peak = m->disp;
		else if (dt > 0.0) {
			m->peak -= (float)(dt * 0.3);
			if (m->peak < m->disp) m->peak = m->disp;
		}
	} else {
		m->peak = m->disp;
	}
}

/* ------------------------------------------------------------------ */
/* shared drawing helpers                                             */
/* ------------------------------------------------------------------ */

static void
rgba_f(uint32_t c, float *r, float *g, float *b)
{
	*r = ((c >> 24) & 0xFF) / 255.0f;
	*g = ((c >> 16) & 0xFF) / 255.0f;
	*b = ((c >>  8) & 0xFF) / 255.0f;
}

/* A radial spoke (tick / needle segment) from (cx,cy): a quad of width `wpx`
 * running between radii r0 and r1 at angle `ang` (0 = up, clockwise). */
static void
spoke(float cx, float cy, float ang, float r0, float r1, float wpx, uint32_t c)
{
	float s = sinf(ang), co = cosf(ang);
	float nx = co, ny = s;                 /* perpendicular to the radial */
	float hw = wpx * 0.5f;
	float ax = cx + s * r0, ay = cy - co * r0;
	float bx = cx + s * r1, by = cy - co * r1;
	bd_draw_quad(ax - nx*hw, ay - ny*hw, ax + nx*hw, ay + ny*hw,
	             bx + nx*hw, by + ny*hw, bx - nx*hw, by - ny*hw, c);
}

/* A filled disc via a triangle fan (small; for hubs). */
static void
disc(float cx, float cy, float r, uint32_t c)
{
	int N = 20;
	for (int i = 0; i < N; i++) {
		float a0 = (float)i / N * 6.2831853f, a1 = (float)(i+1) / N * 6.2831853f;
		bd_draw_quad(cx, cy, cx + cosf(a0)*r, cy + sinf(a0)*r,
		    cx + cosf(a1)*r, cy + sinf(a1)*r, cx + cosf(a1)*r, cy + sinf(a1)*r, c);
	}
}

/* ------------------------------------------------------------------ */
/* shader (EYE + VIAL)                                                */
/* ------------------------------------------------------------------ */

static const char *METER_FRAG =
	"#version 300 es\n"
	"precision highp float;\n"
	"in vec2 v_uv;\n"
	"out vec4 frag;\n"
	"uniform vec3  u_color;\n"
	"uniform float u_value;\n"
	"uniform int   u_mode;\n"    /* 0 eye, 1 vial */
	"void main(){\n"
	"    vec2 p = (v_uv - 0.5) * 2.0;\n"
	"    float r = length(p);\n"
	"    float aa = fwidth(r) * 1.2;\n"
	"    vec3 col = vec3(0.0); float alpha = 0.0;\n"
	"    if (u_mode == 0) {\n"
	"        /* magic eye: green fluorescent disc, dark shadow wedge at bottom */\n"
	"        float disc = smoothstep(0.92+aa, 0.92-aa, r);\n"
	"        float glow = exp(-2.2*r*r);\n"
	"        float ang = atan(p.x, p.y);\n"           /* 0 at bottom (+y down) */
	"        float wedge = mix(1.9, 0.05, clamp(u_value,0.0,1.0));\n"
	"        float inW = smoothstep(wedge+0.05, wedge-0.05, abs(ang));\n"
	"        vec3 face = u_color * (0.22 + 0.95*glow);\n"
	"        face *= (1.0 - 0.93*inW);\n"             /* wedge darkens */
	"        face *= smoothstep(0.05, 0.13, r)*0.9 + 0.1;\n"  /* dark hub */
	"        col = face * disc; alpha = disc;\n"
	"        float rim = smoothstep(0.92-aa,0.92,r)*smoothstep(1.0,0.92,r);\n"
	"        col = mix(col, vec3(0.05,0.07,0.05), rim);\n"
	"        alpha = max(alpha, rim);\n"
	"    } else {\n"
	"        /* vial: glass globe, liquid filled to u_value */\n"
	"        float globe = smoothstep(0.96+aa, 0.96-aa, r);\n"
	"        float on = step(0.004, u_value);\n"
	"        float level = 1.0 - clamp(u_value,0.0,1.0);\n"  /* uv.y of surface */
	"        /* surface dips slightly in the middle (a meniscus curve) */\n"
	"        float surf = level - 0.035*(1.0 - p.x*p.x)*on;\n"
	"        float liquid = smoothstep(-0.012, 0.012, v_uv.y - surf) * on;\n"
	"        /* liquid: bright near the surface, darker with depth, edges shaded\n"
	"           down so the sphere reads as a volume */\n"
	"        float depth = clamp((v_uv.y - surf)/(1.0 - surf + 0.001), 0.0, 1.0);\n"
	"        vec3 lc = u_color * (1.18 - 0.6*depth);\n"
	"        lc *= 0.72 + 0.28*(1.0 - r);\n"
	"        float men = exp(-pow((v_uv.y-surf)/0.03, 2.0)) * on;\n"
	"        lc += (u_color*0.6 + vec3(0.4))*men*0.7;\n"           /* bright meniscus */
	"        /* empty glass above the liquid: neutral dark, high contrast */\n"
	"        vec3 empty = mix(vec3(0.05,0.06,0.08), u_color*0.10, 0.35);\n"
	"        vec3 body = mix(empty, lc, liquid);\n"
	"        /* interior shading: darken toward the inside of the rim */\n"
	"        body *= 0.62 + 0.38*smoothstep(0.95, 0.6, r);\n"
	"        col = body * globe; alpha = globe;\n"
	"        float rim = smoothstep(0.96-3.0*aa,0.96,r);\n"
	"        col = mix(col, vec3(0.60,0.68,0.80), rim*0.55);\n"    /* glass rim */
	"        /* soft broad highlight + a small sharp glint, top-left */\n"
	"        float spec = smoothstep(0.40,0.0,length(p - vec2(-0.32,-0.42)));\n"
	"        col += vec3(1.0)*spec*0.40*globe;\n"
	"        float glint = smoothstep(0.09,0.0,length(p - vec2(-0.30,-0.40)));\n"
	"        col += vec3(1.0)*glint*0.7*globe;\n"
	"    }\n"
	"    frag = vec4(col, alpha);\n"
	"}\n";

static bd_shader meter_shader;

static void
ensure_shader(void)
{
	if (meter_shader.id == 0)
		meter_shader = bd_backend_get()->make_shader(BD_SHADER_QUAD_VERT, METER_FRAG);
}

/* Draw a shader disc (EYE/VIAL) filling a square of side `qs` at (fx,fy). */
static void
draw_shader_round(int mode, uint32_t color, float value,
    float fx, float fy, float qs)
{
	const bd_backend *be = bd_backend_get();
	ensure_shader();
	float cr, cg, cb;
	rgba_f(color, &cr, &cg, &cb);
	be->set_uniform_vec3(meter_shader, "u_color", cr, cg, cb);
	be->set_uniform_float(meter_shader, "u_value", value);
	be->set_uniform_int(meter_shader, "u_mode", mode);
	bd_draw_shader_quad(meter_shader, (int)fx, (int)fy, (int)qs, (int)qs);
}

/* ------------------------------------------------------------------ */
/* per-style render                                                   */
/* ------------------------------------------------------------------ */

#define TAU 6.2831853f

static void
render_bar(struct meter *m, int x, int y, int w, int h)
{
	const bd_theme *th = bd_gui_theme();
	int vert = (m->orient == BD_VERTICAL);
	int len = vert ? h : w;

	bd_draw_rect(x, y, w, h, th->press);            /* track */
	bd_draw_rect_lines(x, y, w, h, th->border);

	float d = m->disp;
	/* piecewise-colored fill by zone band */
	float lo = 0.0f;
	for (int i = 0; i < m->nzones; i++) {
		float hi = (i < m->nbrk) ? m->brk[i] : 1.0f;
		float top = d < hi ? d : hi;
		if (top > lo) {
			int a = (int)lroundf(lo * len);
			int b = (int)lroundf(top * len);
			if (vert)   /* fill upward from the bottom */
				bd_draw_rect(x + 1, y + h - b, w - 2, b - a, m->zones[i]);
			else
				bd_draw_rect(x + a + 1, y + 1, b - a, h - 2, m->zones[i]);
		}
		lo = hi;
		if (d < hi) break;
	}

	/* LED segment gaps: overlay track-colored slots to cut the fill */
	if (m->segments > 1) {
		for (int s = 1; s < m->segments; s++) {
			int at = (int)lroundf((float)s / m->segments * len);
			if (vert) bd_draw_rect(x, y + h - at - 1, w, 2, th->press);
			else      bd_draw_rect(x + at - 1, y, 2, h, th->press);
		}
	}

	/* peak marker */
	if (m->peak_on && m->peak > 0.001f) {
		int at = (int)lroundf(m->peak * len);
		if (vert) bd_draw_rect(x, y + h - at - 1, w, 2, th->text_hi);
		else      bd_draw_rect(x + at - 1, y, 2, h, th->text_hi);
	}
}

static void
render_pie(struct meter *m, int cx, int cy, float R)
{
	const bd_theme *th = bd_gui_theme();
	/* faint background disc */
	int N = 48;
	for (int i = 0; i < N; i++) {
		float a0 = (float)i / N * TAU, a1 = (float)(i + 1) / N * TAU;
		bd_draw_quad(cx, cy, cx + sinf(a0)*R, cy - cosf(a0)*R,
		    cx + sinf(a1)*R, cy - cosf(a1)*R, cx + sinf(a1)*R, cy - cosf(a1)*R,
		    th->press);
	}
	/* lit sector from 12 o'clock, clockwise */
	uint32_t c = zone_color(m, m->disp);
	int lit = (int)lroundf(m->disp * N);
	for (int i = 0; i < lit; i++) {
		float a0 = (float)i / N * TAU, a1 = (float)(i + 1) / N * TAU;
		bd_draw_quad(cx, cy, cx + sinf(a0)*R, cy - cosf(a0)*R,
		    cx + sinf(a1)*R, cy - cosf(a1)*R, cx + sinf(a1)*R, cy - cosf(a1)*R, c);
	}
	/* peak radial tick */
	if (m->peak_on && m->peak > 0.001f)
		spoke((float)cx, (float)cy, m->peak * TAU, R * 0.2f, R, 2.0f, th->text_hi);
}

static void
render_vu(struct meter *m, int x, int y, int w, int h)
{
	const bd_theme *th = bd_gui_theme();
	/* dark bezel + cream face with a faint top-lit gradient */
	bd_draw_rect(x, y, w, h, 0x1C1F24FFu);
	int in = 3;
	bd_draw_rect(x + in, y + in, w - 2*in, h - 2*in, 0xEAE3CFFFu);
	bd_draw_rect(x + in, y + in, w - 2*in, (h - 2*in) / 2, 0xF3EEDDFFu);
	bd_draw_rect_lines(x, y, w, h, th->border);

	const float SPAN = 2.0f;                  /* ~115 deg total sweep */
	const float A = SPAN * 0.5f;              /* half sweep from vertical */
	/* Size the arc so the widest marks (at +-A) stay inside the cream face,
	 * then center the needle sweep (hub at cy, tip reach at cy-R) vertically.
	 * The old fixed R/pivot overshot the face and spilled ticks/red band
	 * past the panel edges. */
	float R = ((w * 0.5f) - in - 1.0f) / sinf(A);   /* width-limited */
	float Rv = (h - 2*in) - 2.0f;                    /* height-limited */
	if (R > Rv) R = Rv;
	float cx = x + w * 0.5f;
	float cy = y + h * 0.5f + R * 0.5f;       /* center the sweep vertically */
	float redStart = (m->nbrk > 0) ? m->brk[m->nbrk - 1] : 0.85f;

	/* red danger band, a filled arc annulus from redStart..1 */
	int RS = 24;
	for (int i = 0; i < RS; i++) {
		float f0 = redStart + (1.0f - redStart) * i / RS;
		float f1 = redStart + (1.0f - redStart) * (i + 1) / RS;
		float a0 = (f0 - 0.5f) * SPAN, a1 = (f1 - 0.5f) * SPAN;
		float s0 = sinf(a0), c0 = cosf(a0), s1 = sinf(a1), c1 = cosf(a1);
		float r0 = R * 0.9f, r1 = R;
		bd_draw_quad(cx + s0*r0, cy - c0*r0, cx + s0*r1, cy - c0*r1,
		    cx + s1*r1, cy - c1*r1, cx + s1*r0, cy - c1*r0, 0xC22222FFu);
	}
	/* scale ticks: 21 minor, every 5th a longer major */
	int NT = 21;
	for (int i = 0; i < NT; i++) {
		float f = (float)i / (NT - 1);
		float ang = (f - 0.5f) * SPAN;
		int major = (i % 5 == 0);
		uint32_t tc = (f >= redStart) ? 0x8A1414FFu : 0x2A2A22FFu;
		spoke(cx, cy, ang, R * (major ? 0.80f : 0.88f), R,
		    major ? 2.0f : 1.0f, tc);
	}
	/* peak marker (thin blue) */
	if (m->peak_on && m->peak > 0.001f)
		spoke(cx, cy, (m->peak - 0.5f) * SPAN, R * 0.55f, R * 0.9f,
		    2.0f, 0x1552C8FFu);

	/* needle: a tapered blade (wide at the hub, sharp at the tip) with a short
	 * counterweight tail and a two-tone hub */
	float ang = (m->disp - 0.5f) * SPAN;
	float s = sinf(ang), co = cosf(ang);
	float nx = co, ny = s;                    /* perpendicular to the blade */
	float bw = 3.0f;                          /* half-width at the hub */
	float tipx = cx + s * R * 0.94f, tipy = cy - co * R * 0.94f;
	float tailx = cx - s * R * 0.16f, taily = cy + co * R * 0.16f;
	bd_draw_quad(cx - nx*bw, cy - ny*bw, cx + nx*bw, cy + ny*bw,
	    tipx, tipy, tipx, tipy, 0x14161CFFu);          /* blade */
	bd_draw_quad(cx - nx*bw, cy - ny*bw, cx + nx*bw, cy + ny*bw,
	    tailx, taily, tailx, taily, 0x14161CFFu);       /* counterweight */
	disc(cx, cy, 4.5f, 0x14161CFFu);
	disc(cx, cy, 2.0f, 0x6A6A74FFu);
}

/* ------------------------------------------------------------------ */
/* class hooks                                                        */
/* ------------------------------------------------------------------ */

static void
meter_init(bd_id id, void *state)
{
	struct meter *m = state;
	m->nzones = parse_colors(NULL, m->zones);   /* one accent */
	set_breaks(m, NULL);
	m->size = 0;
	bd_set(id, BD_PREF_W_I, METER_DEF_DIAM, BD_PREF_H_I, METER_DEF_DIAM, BD_END);
}

static void
meter_render(bd_id id, void *state)
{
	struct meter *m = state;
	meter_tick(m);

	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);

	const char *label = bd_get_s(id, BD_LABEL_S);
	int lh = (int)ceilf(bd_draw_line_height());

	if (m->style == BD_METER_BAR) {
		int bx = x, bw = w;
		if (label && label[0]) {                       /* label to the left */
			int lw = (int)ceilf(bd_draw_text_width(label)) + 6;
			if (lw < w - 10) { bx = x + lw; bw = w - lw; }
			bd_draw_text(label, (float)x, (float)(y + (h - lh) * 0.5f),
			    bd_gui_theme()->text);
		}
		render_bar(m, bx, y, bw, h);
		return;
	}

	/* round styles: a square region, label centered underneath */
	int avail_h = h;
	if (label && label[0]) avail_h = h - lh - 2;
	int qs = avail_h < w ? avail_h : w;
	if (qs < 8) qs = 8;
	int ox = x + (w - qs) / 2;
	int oy = y + (avail_h - qs) / 2;

	switch (m->style) {
	case BD_METER_PIE:
		render_pie(m, ox + qs/2, oy + qs/2, qs * 0.5f - 1.0f);
		break;
	case BD_METER_VU:
		render_vu(m, ox, oy, qs, qs);
		break;
	case BD_METER_EYE:
		draw_shader_round(0, zone_color(m, m->disp), m->disp,
		    (float)ox, (float)oy, (float)qs);
		break;
	case BD_METER_VIAL:
		draw_shader_round(1, zone_color(m, m->disp), m->disp,
		    (float)ox, (float)oy, (float)qs);
		break;
	}

	if (label && label[0]) {
		float tw = bd_draw_text_width(label);
		bd_draw_text(label, (float)x + (w - tw) * 0.5f,
		    (float)(y + h - lh), bd_gui_theme()->text);
	}
}

static const bd_widget_class meter_class = {
	.name = "meter",
	.state_size = sizeof(struct meter),
	.init = meter_init,
	.render = meter_render,
};

static int meter_type;

static struct meter *
meter_of(bd_id id)
{
	if (bd_widget_type(id) != meter_type)
		return NULL;
	return bd_widget_state(id);
}

bd_id
bd_meter_create(bd_id parent, const bd_meter_desc *desc, ...)
{
	if (meter_type == 0)
		meter_type = bd_register_widget_class(&meter_class);

	va_list ap;
	va_start(ap, desc);
	bd_id id = bd_create_va(parent, meter_type, ap);
	va_end(ap);

	struct meter *m = bd_widget_state(id);
	if (m && desc) {
		m->style = desc->style;
		if (desc->zones && desc->zones[0])
			m->nzones = parse_colors(desc->zones, m->zones);
		else {
			m->zones[0] = desc->color ? desc->color : METER_DEF_ACCENT;
			m->nzones = 1;
		}
		set_breaks(m, desc->stops);
		m->peak_on = desc->peak;
		m->ballistic = desc->ballistic;
		m->orient = desc->orient;
		m->segments = desc->segments;
		m->size = desc->size;
		m->target = m->disp = m->peak =
		    desc->value < 0 ? 0 : (desc->value > 1 ? 1 : desc->value);

		/* size to the chosen style */
		int sz = desc->size;
		if (m->style == BD_METER_BAR) {
			int t = sz > 0 ? sz : METER_DEF_THICK;
			if (desc->orient == BD_VERTICAL)
				bd_set(id, BD_PREF_W_I, t, BD_PREF_H_I, 120, BD_END);
			else
				bd_set(id, BD_PREF_W_I, 140, BD_PREF_H_I, t, BD_END);
		} else {
			int d = sz > 0 ? sz : METER_DEF_DIAM;
			int hh = d;
			if (desc->label && desc->label[0])
				hh += (int)ceilf(bd_draw_line_height()) + 2;
			bd_set(id, BD_PREF_W_I, d, BD_PREF_H_I, hh, BD_END);
		}
		if (desc->label)
			bd_set(id, BD_LABEL_S, desc->label, BD_END);
	}
	return id;
}

void
bd_meter_set(bd_id id, float value)
{
	struct meter *m = meter_of(id);
	if (!m) return;
	if (value < 0.0f) value = 0.0f;
	if (value > 1.0f) value = 1.0f;
	m->target = value;
	if (m->ballistic == BD_METER_EXACT) {
		m->disp = value;
		if (m->peak_on) { if (value > m->peak) m->peak = value; }
		else m->peak = value;
	}
}

float
bd_meter_get(bd_id id)
{
	struct meter *m = meter_of(id);
	return m ? m->disp : 0.0f;
}

void
bd_meter_set_zones(bd_id id, const char *zones, const char *stops)
{
	struct meter *m = meter_of(id);
	if (!m) return;
	if (zones && zones[0]) m->nzones = parse_colors(zones, m->zones);
	set_breaks(m, stops);
}

void
bd_meter_reset_peak(bd_id id)
{
	struct meter *m = meter_of(id);
	if (m) m->peak = m->disp;
}
