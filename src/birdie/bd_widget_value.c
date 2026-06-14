#include "bd_widget_value.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

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

/* ================================================================== */
/* knob: a chrome rotary control drawn entirely in a fragment shader  */
/* ================================================================== */

/* Standard UI vertex shader: pixel position -> clip via u_res, pass uv. */
static const char *KNOB_VERT =
	"#version 300 es\n"
	"layout(location=0) in vec2 a_pos;\n"
	"layout(location=1) in vec2 a_uv;\n"
	"layout(location=2) in vec4 a_col;\n"
	"uniform vec2 u_res;\n"
	"out vec2 v_uv;\n"
	"void main(){\n"
	"    vec2 p = a_pos / u_res * 2.0 - 1.0;\n"
	"    gl_Position = vec4(p.x, -p.y, 0.0, 1.0);\n"
	"    v_uv = a_uv;\n"
	"}\n";

/*
 * One quad, the whole knob. p is centered [-1,1], y-down. A chrome face with a
 * top-left key light, a brushed concentric texture, a beveled rim and drop
 * shadow, an orange-red indicator at the value angle, and tick dots around a
 * 300-degree sweep. Edges are anti-aliased with fwidth.
 */
static const char *KNOB_FRAG =
	"#version 300 es\n"
	"precision highp float;\n"
	"in vec2 v_uv;\n"
	"out vec4 frag;\n"
	"uniform float u_value;\n"
	"uniform int u_marks;\n"     /* number of evenly spaced dial dots (0 = none) */
	"uniform int u_dimples;\n"   /* jog finger-dimples (0 = none, knob mode) */
	"uniform float u_indicator;\n" /* 1 = draw the indicator line, 0 = jog */
	"#define SWEEP 4.712389\n"   /* 270 degrees (typical audio/panel pot) */
	"float h(float x){ return fract(sin(x * 12.9898) * 43758.5453); }\n"
	"void main(){\n"
	"    vec2 p = (v_uv - 0.5) * 2.0;\n"
	"    float r = length(p);\n"
	"    float ang = atan(p.y, p.x);\n"
	"    float aaw = fwidth(r) * 1.3;\n"
	"    float R = 0.78;\n"
	"    vec3 col = vec3(0.0);\n"
	"    float alpha = 0.0;\n"
	"    /* drop shadow */\n"
	"    float sh = smoothstep(R + 0.16, R - 0.02, length(p - vec2(0.05, 0.07)));\n"
	"    col = mix(col, vec3(0.0), sh); alpha = max(alpha, sh * 0.5);\n"
	"    float body = smoothstep(R + aaw, R - aaw, r);\n"
	"    /* flat turned-aluminum face: near-uniform tone + concentric grain */\n"
	"    vec3 alum = vec3(0.55, 0.56, 0.58);\n"
	"    float g = 0.6 * h(floor(r * 240.0)) + 0.4 * h(floor(r * 600.0));\n"
	"    alum *= 0.93 + 0.11 * g;\n"
	"    alum *= 0.995 + 0.005 * h(floor(ang * 200.0));\n"      /* tiny angular break */
	"    /* anisotropic circular highlight (Ward): grooves run tangentially, so\n"
	"       the spec is tight across the radius and smeared along the circle */\n"
	"    vec2 rd = r > 0.0001 ? p / r : vec2(1.0, 0.0);\n"
	"    vec3 Ll = normalize(vec3(-0.42, -0.52, 0.55));\n"
	"    vec3 Hh = normalize(Ll + vec3(0.0, 0.0, 1.0));\n"
	"    vec3 Tg = vec3(-rd.y, rd.x, 0.0);\n"   /* along grooves */
	"    vec3 Bn = vec3(rd.x, rd.y, 0.0);\n"    /* across grooves */
	"    float ht = dot(Hh, Tg) / 0.85;\n"
	"    float hb = dot(Hh, Bn) / 0.09;\n"
	"    float hn = dot(Hh, vec3(0.0, 0.0, 1.0));\n"
	"    float aniso = exp(-2.0 * (ht * ht + hb * hb) / (1.0 + hn));\n"
	"    alum += aniso * 1.0 + 0.05;\n"      /* highlight + faint ambient sheen */
	"    /* edge: dark bevel + thin bright lip, so it reads as a raised knob */\n"
	"    float bevel = smoothstep(R - 0.09, R, r);\n"
	"    alum = mix(alum, alum * 0.46, bevel);\n"
	"    alum += smoothstep(0.03, 0.0, abs(r - (R - 0.045))) * 0.16;\n"
	"    col = mix(col, alum, body);\n"
	"    alpha = max(alpha, body);\n"
	"    /* indicator (absolute knob only) */\n"
	"    if (u_indicator > 0.5) {\n"
	"        float a = (u_value - 0.5) * SWEEP;\n"
	"        vec2 dir = vec2(sin(a), -cos(a));\n"
	"        float along = dot(p, dir);\n"
	"        float perp = length(p - dir * along);\n"
	"        float ind = smoothstep(0.044, 0.028, perp) *\n"
	"                    smoothstep(0.08, 0.11, along) *\n"
	"                    smoothstep(0.70, 0.67, along) * body;\n"
	"        col = mix(col, vec3(0.98, 0.26, 0.05), ind);\n"
	"    }\n"
	"    /* jog finger-dimples (relative mode), rotated by u_value */\n"
	"    for (int i = 0; i < 8; i++) {\n"
	"        if (i >= u_dimples) break;\n"
	"        float ai = u_value + float(i) * 6.2831853 / float(max(u_dimples, 1));\n"
	"        vec2 dp = vec2(sin(ai), -cos(ai)) * 0.46;\n"
	"        vec2 dl = p - dp;\n"
	"        float d = length(dl);\n"
	"        float dimR = 0.15;\n"
	"        float inside = smoothstep(dimR, dimR - 0.05, d) * body;\n"
	"        float lit = clamp(0.5 + dl.y / dimR * 0.8, 0.0, 1.0);\n"  /* lower wall lit */
	"        col = mix(col, col * (0.40 + 0.55 * lit), inside);\n"
	"        col += smoothstep(0.035, 0.0, abs(d - dimR)) *\n"
	"               (-dl.y / dimR) * 0.20 * body;\n"            /* lit top rim */
	"    }\n"
	"    /* dial dots: u_marks evenly spaced over the sweep */\n"
	"    for (int i = 0; i < 64; i++) {\n"
	"        if (i >= u_marks) break;\n"
	"        float ti = u_marks > 1 ? float(i) / float(u_marks - 1) : 0.5;\n"
	"        float ai = (ti - 0.5) * SWEEP;\n"
	"        vec2 dp = vec2(sin(ai), -cos(ai)) * 0.92;\n"
	"        float d = smoothstep(0.05, 0.032, length(p - dp));\n"
	"        col = mix(col, vec3(0.82), d); alpha = max(alpha, d);\n"
	"    }\n"
	"    frag = vec4(col, alpha);\n"
	"}\n";

#define KNOB_SWEEP 4.712389f   /* must match SWEEP in KNOB_FRAG (270 deg) */

struct knob {
	float       min, max, step;
	float       value;
	int         dial;
	int         hex;
	int         relative;       /* endless jog dial */
	int         dimples;
	float       phase;          /* accumulated rotation (relative mode) */
	bd_value_cb cb;
	void       *arg;
	float       drag_v0;        /* value at drag start */
	int         drag_y0;
};

static int       knob_type;
static bd_shader knob_shader;

static void
ensure_knob_shader(void)
{
	if (knob_shader.id == 0)
		knob_shader = bd_backend_get()->make_shader(KNOB_VERT, KNOB_FRAG);
}

/* clamp to range, then snap to the step grid (if any) */
static float
knob_snap(const struct knob *k, float v)
{
	if (v < k->min) v = k->min;
	if (v > k->max) v = k->max;
	if (k->step > 0.0f)
		v = k->min + roundf((v - k->min) / k->step) * k->step;
	if (v < k->min) v = k->min;
	if (v > k->max) v = k->max;
	return v;
}

/* normalized position [0,1] of the current value, for the indicator angle */
static float
knob_norm(const struct knob *k)
{
	return k->max > k->min ? (k->value - k->min) / (k->max - k->min) : 0.0f;
}

/* number of evenly spaced dial dots for the shader (0 = none) */
static int
knob_marks(const struct knob *k)
{
	int n;
	switch (k->dial) {
	case BD_DIAL_DOTS:
		if (k->step > 0.0f) {
			n = (int)((k->max - k->min) / k->step + 0.5f) + 1;
			return n > 64 ? 64 : n;
		}
		return 11;
	case BD_DIAL_BALANCE:
		return 3;
	default:                /* NONE and LABELS draw no shader dots */
		return 0;
	}
}

static void
knob_init(bd_id id, void *state)
{
	struct knob *k = state;
	k->min = 0.0f;
	k->max = 1.0f;
	k->step = 0.0f;
	k->value = 0.0f;
	k->dial = BD_DIAL_DOTS;
	bd_set(id, BD_PREF_W_I, 56, BD_PREF_H_I, 56, BD_END);
}

/* numeric labels around the dial (BD_DIAL_LABELS): the ends plus round
 * values between, decimal or hex */
static void
knob_labels(const struct knob *k, float cx, float cy, float radius)
{
	const bd_theme *th = bd_gui_theme();
	float base = k->hex ? 16.0f : 10.0f;
	float lh = bd_draw_line_height();
	float ticks[24];
	int nt = 0;
	ticks[nt++] = k->min;
	for (float v = ceilf(k->min / base) * base;
	     v < k->max - 0.001f && nt < 23; v += base)
		if (v > k->min + 0.001f)
			ticks[nt++] = v;
	ticks[nt++] = k->max;

	for (int i = 0; i < nt; i++) {
		float v = ticks[i];
		float t = (v - k->min) / (k->max - k->min);
		float a = (t - 0.5f) * KNOB_SWEEP;
		float lx = cx + sinf(a) * radius;
		float ly = cy - cosf(a) * radius;
		char buf[16];
		if (k->hex)
			snprintf(buf, sizeof buf, "%X", (int)(v + 0.5f));
		else
			snprintf(buf, sizeof buf, "%d",
			    (int)(v + (v < 0 ? -0.5f : 0.5f)));
		float tw = bd_draw_text_width(buf);
		bd_draw_text(buf, lx - tw * 0.5f, ly - lh * 0.5f, th->text);
	}
}

static void
knob_render(bd_id id, void *state)
{
	struct knob *k = state;
	ensure_knob_shader();
	const bd_backend *be = bd_backend_get();

	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	int s = w < h ? w : h;
	int ox = x + (w - s) / 2;     /* center the square knob in its rect */
	int oy = y + (h - s) / 2;
	float fx = (float)ox;
	float fy = (float)oy;
	float fs = (float)s;
	bd_vertex q[6] = {
		{ fx,      fy,      0, 0, 1, 1, 1, 1 },
		{ fx + fs, fy,      1, 0, 1, 1, 1, 1 },
		{ fx + fs, fy + fs, 1, 1, 1, 1, 1, 1 },
		{ fx,      fy,      0, 0, 1, 1, 1, 1 },
		{ fx + fs, fy + fs, 1, 1, 1, 1, 1, 1 },
		{ fx,      fy + fs, 0, 1, 1, 1, 1, 1 },
	};

	bd_draw_flush();   /* land the chrome drawn so far beneath the knob */
	be->use_shader(knob_shader);
	be->set_uniform_vec2(knob_shader, "u_res",
	    (float)bd_draw_win_w(), (float)bd_draw_win_h());
	be->set_uniform_float(knob_shader, "u_value",
	    k->relative ? k->phase : knob_norm(k));
	be->set_uniform_int(knob_shader, "u_marks", k->relative ? 0 : knob_marks(k));
	be->set_uniform_int(knob_shader, "u_dimples", k->relative ? k->dimples : 0);
	be->set_uniform_float(knob_shader, "u_indicator", k->relative ? 0.0f : 1.0f);
	be->draw_verts(q, 6);

	if (!k->relative && k->dial == BD_DIAL_LABELS)
		knob_labels(k, fx + fs * 0.5f, fy + fs * 0.5f, fs * 0.5f * 0.92f);
}

static int
knob_event(bd_id id, void *state, const bd_event *ev)
{
	struct knob *k = state;
	if (ev->type == BD_EV_MOUSE_DOWN) {
		k->drag_y0 = ev->y;
		k->drag_v0 = k->value;
		return 1;
	}
	if (ev->type == BD_EV_MOUSE_MOVE) {
		if (k->relative) {
			int d = k->drag_y0 - ev->y;       /* up = + */
			k->drag_y0 = ev->y;
			k->phase += (float)d * 0.02f;     /* spin the dimples */
			if (d != 0 && k->cb)
				k->cb(id, k->arg, (float)d * 0.01f);
			return 1;
		}
		float t = (k->drag_v0 - k->min) / (k->max - k->min) +
		    (float)(k->drag_y0 - ev->y) / 150.0f;
		float v = knob_snap(k, k->min + t * (k->max - k->min));
		if (v != k->value) {
			k->value = v;
			if (k->cb)
				k->cb(id, k->arg, v);
		}
		return 1;
	}
	return 0;
}

static const bd_widget_class knob_class = {
	.name = "knob",
	.state_size = sizeof(struct knob),
	.init = knob_init,
	.render = knob_render,
	.event = knob_event,
};

bd_id
bd_knob_create(bd_id parent, const bd_knob_desc *desc, ...)
{
	if (knob_type == 0)
		knob_type = bd_register_widget_class(&knob_class);

	va_list ap;
	va_start(ap, desc);
	bd_id id = bd_create_va(parent, knob_type, ap);
	va_end(ap);

	struct knob *k = bd_widget_state(id);
	if (k && desc) {
		k->min = desc->min;
		k->max = desc->max;
		if (k->min == k->max) {     /* default range */
			k->min = 0.0f;
			k->max = 1.0f;
		}
		k->step = desc->step;
		k->dial = desc->dial;
		k->hex = desc->hex;
		k->relative = desc->relative;
		k->dimples = desc->dimples > 0 ? desc->dimples : 3;
		k->cb = desc->cb;
		k->arg = desc->arg;
		k->value = knob_snap(k, desc->value);
	}
	return id;
}

void
bd_knob_set(bd_id id, float value)
{
	if (bd_widget_type(id) != knob_type)
		return;
	struct knob *k = bd_widget_state(id);
	if (k)
		k->value = knob_snap(k, value);
}

float
bd_knob_get(bd_id id)
{
	if (bd_widget_type(id) != knob_type)
		return 0.0f;
	struct knob *k = bd_widget_state(id);
	return k ? k->value : 0.0f;
}

/* ================================================================== */
/* toggle: a sliding on/off switch drawn in a fragment shader         */
/* ================================================================== */

/*
 * A skeuomorphic sliding switch: a recessed pill slot (theme accent when on,
 * dark gray when off, with an OPEN-LOOK inset bevel) holding a brushed-aluminum
 * chrome thumb that reuses the knob's anisotropic-highlight metal. u_size is the
 * pill size in px, u_pos the animated 0..1 on-amount, u_accent the on color.
 */
static const char *TOGGLE_FRAG =
	"#version 300 es\n"
	"precision highp float;\n"
	"in vec2 v_uv;\n"
	"out vec4 frag;\n"
	"uniform vec2 u_size;\n"
	"uniform float u_pos;\n"
	"uniform vec3 u_accent;\n"
	"float sdbox(vec2 p, vec2 b, float r){\n"
	"    vec2 d = abs(p) - b + r;\n"
	"    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;\n"
	"}\n"
	"float hsh(float x){ return fract(sin(x * 12.9898) * 43758.5453); }\n"
	"void main(){\n"
	"    vec2 q = v_uv * u_size;\n"
	"    float H = u_size.y;\n"
	"    float aa = 1.3;\n"
	"    vec3 col = vec3(0.0);\n"
	"    float alpha = 0.0;\n"
	"    /* recessed track slot */\n"
	"    float tr = sdbox(q - u_size * 0.5, u_size * 0.5 - 1.0, H * 0.5 - 1.0);\n"
	"    float track = smoothstep(aa, -aa, tr);\n"
	"    vec3 tcol = mix(vec3(0.11, 0.12, 0.13), u_accent, u_pos);\n"
	"    tcol *= 0.74 + 0.34 * v_uv.y;\n"                /* inset: dark top, lit base */
	"    col = tcol; alpha = track;\n"
	"    float lipd = abs(tr + 1.5);\n"
	"    col += smoothstep(2.0, 0.0, lipd) * (v_uv.y - 0.5) * 0.5;\n"  /* rim bevel */
	"    /* brushed-aluminum chrome thumb */\n"
	"    float m = 2.0;\n"
	"    float rad = H * 0.5 - m;\n"
	"    float cx = mix(rad + m, u_size.x - rad - m, u_pos);\n"
	"    vec2 tc = vec2(cx, H * 0.5);\n"
	"    float dsh = length(q - tc - vec2(0.0, 1.5)) - rad;\n"
	"    col = mix(col, col * 0.45, smoothstep(aa + 2.5, -aa, dsh) * 0.45 * track);\n"
	"    vec2 lp = (q - tc) / rad;\n"          /* -1..1 within thumb */
	"    float lr = length(lp);\n"
	"    float thumb = smoothstep(0.07, -0.02, lr - 1.0);\n"
	"    vec3 al = vec3(0.60, 0.61, 0.63);\n"
	"    float g = 0.6 * hsh(floor(atan(lp.y, lp.x) * 36.0)) +\n"
	"              0.4 * hsh(floor(lr * 36.0));\n"
	"    al *= 0.92 + 0.11 * g;\n"
	"    vec2 rd = lr > 0.001 ? lp / lr : vec2(1.0, 0.0);\n"
	"    vec3 Hh = normalize(normalize(vec3(-0.4, -0.5, 0.55)) + vec3(0.0, 0.0, 1.0));\n"
	"    vec3 Tg = vec3(-rd.y, rd.x, 0.0);\n"
	"    vec3 Bn = vec3(rd.x, rd.y, 0.0);\n"
	"    float ht = dot(Hh, Tg) / 0.85, hb = dot(Hh, Bn) / 0.18;\n"
	"    float hn = dot(Hh, vec3(0.0, 0.0, 1.0));\n"
	"    al += exp(-2.0 * (ht * ht + hb * hb) / (1.0 + hn)) * 0.42 + 0.07;\n"
	"    al = mix(al, al * 0.5, smoothstep(0.82, 1.0, lr));\n"   /* rim */
	"    col = mix(col, al, thumb);\n"
	"    alpha = max(alpha, thumb);\n"
	"    frag = vec4(col, alpha);\n"
	"}\n";

struct toggle {
	int          on;
	float        pos;       /* animated 0..1 thumb position */
	bd_toggle_cb cb;
	void        *arg;
};

static int       toggle_type;
static bd_shader toggle_shader;

static void
ensure_toggle_shader(void)
{
	if (toggle_shader.id == 0)
		toggle_shader = bd_backend_get()->make_shader(KNOB_VERT, TOGGLE_FRAG);
}

static void
toggle_init(bd_id id, void *state)
{
	struct toggle *t = state;
	t->on = 0;
	t->pos = 0.0f;
	bd_set(id, BD_PREF_W_I, 48, BD_PREF_H_I, 26, BD_END);
}

static void
toggle_render(bd_id id, void *state)
{
	struct toggle *t = state;
	ensure_toggle_shader();
	const bd_backend *be = bd_backend_get();

	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	int ph = h > 26 ? 26 : (h < 16 ? 16 : h);
	int pw = ph * 9 / 5;          /* ~1.8 aspect pill */
	if (pw > w) pw = w;
	int px = x + (w - pw) / 2;
	int py = y + (h - ph) / 2;
	float fx = (float)px, fy = (float)py;
	float fw = (float)pw, fh = (float)ph;

	/* ease the thumb toward the target each frame */
	float target = t->on ? 1.0f : 0.0f;
	t->pos += (target - t->pos) * 0.25f;
	if (fabsf(target - t->pos) < 0.002f)
		t->pos = target;

	bd_vertex q[6] = {
		{ fx,      fy,      0, 0, 1, 1, 1, 1 },
		{ fx + fw, fy,      1, 0, 1, 1, 1, 1 },
		{ fx + fw, fy + fh, 1, 1, 1, 1, 1, 1 },
		{ fx,      fy,      0, 0, 1, 1, 1, 1 },
		{ fx + fw, fy + fh, 1, 1, 1, 1, 1, 1 },
		{ fx,      fy + fh, 0, 1, 1, 1, 1, 1 },
	};

	const bd_theme *th = bd_gui_theme();
	uint32_t a = th->focus;
	float ar = ((a >> 24) & 0xFF) / 255.0f;
	float ag = ((a >> 16) & 0xFF) / 255.0f;
	float ab = ((a >>  8) & 0xFF) / 255.0f;

	bd_draw_flush();
	be->use_shader(toggle_shader);
	be->set_uniform_vec2(toggle_shader, "u_res",
	    (float)bd_draw_win_w(), (float)bd_draw_win_h());
	be->set_uniform_vec2(toggle_shader, "u_size", fw, fh);
	be->set_uniform_float(toggle_shader, "u_pos", t->pos);
	be->set_uniform_vec3(toggle_shader, "u_accent", ar, ag, ab);
	be->draw_verts(q, 6);
}

static int
toggle_event(bd_id id, void *state, const bd_event *ev)
{
	struct toggle *t = state;
	if (ev->type == BD_EV_MOUSE_DOWN) {
		t->on = !t->on;
		if (t->cb)
			t->cb(id, t->arg, t->on);
		return 1;
	}
	return 0;
}

static const bd_widget_class toggle_class = {
	.name = "toggle",
	.state_size = sizeof(struct toggle),
	.init = toggle_init,
	.render = toggle_render,
	.event = toggle_event,
};

bd_id
bd_toggle_create(bd_id parent, int on, bd_toggle_cb cb, void *arg, ...)
{
	if (toggle_type == 0)
		toggle_type = bd_register_widget_class(&toggle_class);

	va_list ap;
	va_start(ap, arg);
	bd_id id = bd_create_va(parent, toggle_type, ap);
	va_end(ap);

	struct toggle *t = bd_widget_state(id);
	if (t) {
		t->on = on ? 1 : 0;
		t->pos = (float)t->on;
		t->cb = cb;
		t->arg = arg;
	}
	return id;
}

void
bd_toggle_set(bd_id id, int on)
{
	if (bd_widget_type(id) != toggle_type)
		return;
	struct toggle *t = bd_widget_state(id);
	if (t)
		t->on = on ? 1 : 0;
}

int
bd_toggle_get(bd_id id)
{
	if (bd_widget_type(id) != toggle_type)
		return 0;
	struct toggle *t = bd_widget_state(id);
	return t ? t->on : 0;
}

/* ================================================================== */
/* scroll wheel: a ribbed jog cylinder, relative output               */
/* ================================================================== */

/*
 * A ribbed cylinder seen edge-on. The short axis is shaded as a cylinder
 * (bright center, dark sides); along the spin axis the ribs bunch toward the
 * ends (perspective) and scroll by u_phase. u_vert picks the axis.
 */
static const char *WHEEL_FRAG =
	"#version 300 es\n"
	"precision highp float;\n"
	"in vec2 v_uv;\n"
	"out vec4 frag;\n"
	"uniform vec2 u_size;\n"
	"uniform float u_phase;\n"
	"uniform float u_vert;\n"
	"float sdbox(vec2 p, vec2 b, float r){\n"
	"    vec2 d = abs(p) - b + r;\n"
	"    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;\n"
	"}\n"
	"void main(){\n"
	"    vec2 q = v_uv * u_size;\n"
	"    float aa = 1.3;\n"
	"    float bd = sdbox(q - u_size * 0.5, u_size * 0.5 - 1.0, 4.0);\n"
	"    float body = smoothstep(aa, -aa, bd);\n"
	"    float across = u_vert > 0.5 ? v_uv.x : v_uv.y;\n"
	"    float along  = u_vert > 0.5 ? v_uv.y : v_uv.x;\n"
	"    float cyl = sin(across * 3.14159265);\n"           /* 0 sides .. 1 center */
	"    vec3 col = mix(vec3(0.06, 0.06, 0.07), vec3(0.5, 0.5, 0.52), cyl * cyl);\n"
	"    /* ridges: even around the wheel, projected through asin so they bunch\n"
	"       toward the ends (forced perspective) and scroll with u_phase */\n"
	"    float a = along - 0.5;\n"
	"    float theta = asin(clamp(a * 2.0, -1.0, 1.0));\n"
	"    float s = sin((theta * 8.0 + u_phase) * 6.2831853);\n"
	"    col *= 0.42 + 0.58 * (0.5 + 0.5 * s);\n"            /* deep grooves */
	"    col += pow(max(0.0, s), 4.0) * 0.40 * cyl;\n"       /* lit crests */
	"    col *= 0.45 + 0.55 * clamp(cos(a * 3.14159265), 0.0, 1.0);\n"  /* end fade */
	"    col += cyl * cyl * 0.10;\n"                          /* center sheen */
	"    frag = vec4(col, body);\n"
	"}\n";

struct wheel {
	int         vert;
	float       phase;
	bd_value_cb cb;
	void       *arg;
	int         drag_last;
};

static int       wheel_type;
static bd_shader wheel_shader;

static void
ensure_wheel_shader(void)
{
	if (wheel_shader.id == 0)
		wheel_shader = bd_backend_get()->make_shader(KNOB_VERT, WHEEL_FRAG);
}

static void
wheel_init(bd_id id, void *state)
{
	struct wheel *wl = state;
	wl->vert = 1;
	bd_set(id, BD_PREF_W_I, 30, BD_PREF_H_I, 60, BD_END);
}

static void
wheel_render(bd_id id, void *state)
{
	struct wheel *wl = state;
	ensure_wheel_shader();
	const bd_backend *be = bd_backend_get();
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	float fx = (float)x, fy = (float)y, fw = (float)w, fh = (float)h;
	bd_vertex q[6] = {
		{ fx,      fy,      0, 0, 1, 1, 1, 1 },
		{ fx + fw, fy,      1, 0, 1, 1, 1, 1 },
		{ fx + fw, fy + fh, 1, 1, 1, 1, 1, 1 },
		{ fx,      fy,      0, 0, 1, 1, 1, 1 },
		{ fx + fw, fy + fh, 1, 1, 1, 1, 1, 1 },
		{ fx,      fy + fh, 0, 1, 1, 1, 1, 1 },
	};
	bd_draw_flush();
	be->use_shader(wheel_shader);
	be->set_uniform_vec2(wheel_shader, "u_res",
	    (float)bd_draw_win_w(), (float)bd_draw_win_h());
	be->set_uniform_vec2(wheel_shader, "u_size", fw, fh);
	be->set_uniform_float(wheel_shader, "u_phase", wl->phase);
	be->set_uniform_float(wheel_shader, "u_vert", wl->vert ? 1.0f : 0.0f);
	be->draw_verts(q, 6);
}

static int
wheel_event(bd_id id, void *state, const bd_event *ev)
{
	struct wheel *wl = state;
	if (ev->type == BD_EV_MOUSE_DOWN) {
		wl->drag_last = wl->vert ? ev->y : ev->x;
		return 1;
	}
	if (ev->type == BD_EV_MOUSE_MOVE) {
		int cur = wl->vert ? ev->y : ev->x;
		int d = cur - wl->drag_last;
		wl->drag_last = cur;
		float dir = wl->vert ? -(float)d : (float)d;  /* up / right = + */
		wl->phase += dir * 0.015f;
		if (d != 0 && wl->cb)
			wl->cb(id, wl->arg, dir * 0.01f);
		return 1;
	}
	return 0;
}

static const bd_widget_class wheel_class = {
	.name = "wheel",
	.state_size = sizeof(struct wheel),
	.init = wheel_init,
	.render = wheel_render,
	.event = wheel_event,
};

bd_id
bd_wheel_create(bd_id parent, int orient, bd_value_cb cb, void *arg, ...)
{
	if (wheel_type == 0)
		wheel_type = bd_register_widget_class(&wheel_class);

	va_list ap;
	va_start(ap, arg);
	bd_id id = bd_create_va(parent, wheel_type, ap);
	va_end(ap);

	struct wheel *wl = bd_widget_state(id);
	if (wl) {
		wl->vert = (orient == BD_VERTICAL);
		wl->cb = cb;
		wl->arg = arg;
	}
	return id;
}

/* ================================================================== */
/* X-Y pad: a recessed surface with a draggable chrome puck           */
/* ================================================================== */

static const char *XYPAD_FRAG =
	"#version 300 es\n"
	"precision highp float;\n"
	"in vec2 v_uv;\n"
	"out vec4 frag;\n"
	"uniform vec2 u_size;\n"
	"uniform vec2 u_pos;\n"        /* puck position in uv space */
	"uniform float u_circle;\n"
	"float sdbox(vec2 p, vec2 b, float r){\n"
	"    vec2 d = abs(p) - b + r;\n"
	"    return length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - r;\n"
	"}\n"
	"float hsh(float x){ return fract(sin(x * 12.9898) * 43758.5453); }\n"
	"void main(){\n"
	"    vec2 q = v_uv * u_size;\n"
	"    vec2 c = u_size * 0.5;\n"
	"    float S = min(u_size.x, u_size.y);\n"
	"    float aa = 1.3;\n"
	"    float field = u_circle > 0.5\n"
	"        ? smoothstep(aa, -aa, length(q - c) - (S * 0.5 - 1.0))\n"
	"        : smoothstep(aa, -aa, sdbox(q - c, u_size * 0.5 - 1.0, 6.0));\n"
	"    vec3 col = vec3(0.11, 0.12, 0.14) * (0.82 + 0.32 * v_uv.y);\n"
	"    float alpha = field;\n"
	"    vec2 pp = u_pos * u_size;\n"
	"    col += (smoothstep(1.5, 0.0, abs(q.x - pp.x)) +\n"
	"            smoothstep(1.5, 0.0, abs(q.y - pp.y))) * 0.05 * field;\n"
	"    float pr = S * 0.13;\n"
	"    float ds = length(q - pp - vec2(0.0, 2.0)) - pr;\n"
	"    col = mix(col, col * 0.5, smoothstep(3.0, -1.0, ds) * 0.4 * field);\n"
	"    vec2 lp = (q - pp) / pr;\n"
	"    float lr = length(lp);\n"
	"    float puck = smoothstep(0.08, -0.02, lr - 1.0);\n"
	"    vec3 al = vec3(0.60, 0.61, 0.63);\n"
	"    vec2 rd = lr > 0.001 ? lp / lr : vec2(1.0, 0.0);\n"
	"    vec3 Hh = normalize(normalize(vec3(-0.4, -0.5, 0.55)) + vec3(0.0, 0.0, 1.0));\n"
	"    float ht = dot(Hh, vec3(-rd.y, rd.x, 0.0)) / 0.85;\n"
	"    float hb = dot(Hh, vec3(rd.x, rd.y, 0.0)) / 0.18;\n"
	"    float hn = dot(Hh, vec3(0.0, 0.0, 1.0));\n"
	"    al += exp(-2.0 * (ht * ht + hb * hb) / (1.0 + hn)) * 0.42 + 0.06;\n"
	"    al = mix(al, al * 0.5, smoothstep(0.82, 1.0, lr));\n"
	"    col = mix(col, al, puck);\n"
	"    alpha = max(alpha, puck);\n"
	"    frag = vec4(col, alpha);\n"
	"}\n";

struct xypad {
	int      shape;
	int      spring;
	float    x, y;          /* [0,1], y up */
	bd_xy_cb cb;
	void    *arg;
};

static int       xypad_type;
static bd_shader xypad_shader;

static void
ensure_xypad_shader(void)
{
	if (xypad_shader.id == 0)
		xypad_shader = bd_backend_get()->make_shader(KNOB_VERT, XYPAD_FRAG);
}

/* the centered square region the pad occupies within its rect */
static void
xypad_square(bd_id id, int *sx, int *sy, int *ss)
{
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	int s = w < h ? w : h;
	*sx = x + (w - s) / 2;
	*sy = y + (h - s) / 2;
	*ss = s;
}

static void
xypad_clamp(const struct xypad *p, float *x, float *y)
{
	if (p->shape == BD_XY_CIRCLE) {
		float dx = *x - 0.5f, dy = *y - 0.5f;
		float r = sqrtf(dx * dx + dy * dy);
		if (r > 0.5f) {
			dx *= 0.5f / r;
			dy *= 0.5f / r;
		}
		*x = 0.5f + dx;
		*y = 0.5f + dy;
	} else {
		*x = *x < 0.0f ? 0.0f : (*x > 1.0f ? 1.0f : *x);
		*y = *y < 0.0f ? 0.0f : (*y > 1.0f ? 1.0f : *y);
	}
}

static void
xypad_init(bd_id id, void *state)
{
	struct xypad *p = state;
	p->shape = BD_XY_SQUARE;
	p->x = 0.5f;
	p->y = 0.5f;
	bd_set(id, BD_PREF_W_I, 76, BD_PREF_H_I, 76, BD_END);
}

static void
xypad_render(bd_id id, void *state)
{
	struct xypad *p = state;
	ensure_xypad_shader();
	const bd_backend *be = bd_backend_get();
	int sx, sy, ss;
	xypad_square(id, &sx, &sy, &ss);
	float fx = (float)sx, fy = (float)sy, fs = (float)ss;
	bd_vertex q[6] = {
		{ fx,      fy,      0, 0, 1, 1, 1, 1 },
		{ fx + fs, fy,      1, 0, 1, 1, 1, 1 },
		{ fx + fs, fy + fs, 1, 1, 1, 1, 1, 1 },
		{ fx,      fy,      0, 0, 1, 1, 1, 1 },
		{ fx + fs, fy + fs, 1, 1, 1, 1, 1, 1 },
		{ fx,      fy + fs, 0, 1, 1, 1, 1, 1 },
	};
	bd_draw_flush();
	be->use_shader(xypad_shader);
	be->set_uniform_vec2(xypad_shader, "u_res",
	    (float)bd_draw_win_w(), (float)bd_draw_win_h());
	be->set_uniform_vec2(xypad_shader, "u_size", fs, fs);
	be->set_uniform_vec2(xypad_shader, "u_pos", p->x, 1.0f - p->y);
	be->set_uniform_float(xypad_shader, "u_circle",
	    p->shape == BD_XY_CIRCLE ? 1.0f : 0.0f);
	be->draw_verts(q, 6);
}

static int
xypad_event(bd_id id, void *state, const bd_event *ev)
{
	struct xypad *p = state;
	if (ev->type == BD_EV_MOUSE_DOWN || ev->type == BD_EV_MOUSE_MOVE) {
		int sx, sy, ss;
		xypad_square(id, &sx, &sy, &ss);
		float x = (float)(ev->x - sx) / (float)ss;
		float y = 1.0f - (float)(ev->y - sy) / (float)ss;
		xypad_clamp(p, &x, &y);
		p->x = x;
		p->y = y;
		if (p->cb)
			p->cb(id, p->arg, x, y);
		return 1;
	}
	if (ev->type == BD_EV_MOUSE_UP && p->spring) {
		p->x = 0.5f;
		p->y = 0.5f;
		if (p->cb)
			p->cb(id, p->arg, 0.5f, 0.5f);
		return 1;
	}
	return 0;
}

static const bd_widget_class xypad_class = {
	.name = "xypad",
	.state_size = sizeof(struct xypad),
	.init = xypad_init,
	.render = xypad_render,
	.event = xypad_event,
};

bd_id
bd_xypad_create(bd_id parent, const bd_xypad_desc *desc, ...)
{
	if (xypad_type == 0)
		xypad_type = bd_register_widget_class(&xypad_class);

	va_list ap;
	va_start(ap, desc);
	bd_id id = bd_create_va(parent, xypad_type, ap);
	va_end(ap);

	struct xypad *p = bd_widget_state(id);
	if (p && desc) {
		p->shape = desc->shape;
		p->spring = desc->spring;
		p->x = desc->x;
		p->y = desc->y;
		p->cb = desc->cb;
		p->arg = desc->arg;
		xypad_clamp(p, &p->x, &p->y);
	}
	return id;
}

void
bd_xypad_get(bd_id id, float *x, float *y)
{
	if (bd_widget_type(id) != xypad_type)
		return;
	struct xypad *p = bd_widget_state(id);
	if (p) {
		if (x) *x = p->x;
		if (y) *y = p->y;
	}
}

void
bd_xypad_set(bd_id id, float x, float y)
{
	if (bd_widget_type(id) != xypad_type)
		return;
	struct xypad *p = bd_widget_state(id);
	if (p) {
		p->x = x;
		p->y = y;
		xypad_clamp(p, &p->x, &p->y);
	}
}
