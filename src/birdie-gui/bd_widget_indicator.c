/*
 * bd_widget_indicator.c -- panel-mount LED indicator lamp.
 *
 * An extension widget built on widget_ext.h and drawn in a fragment shader,
 * the same recipe as the knob and toggle in bd_widget_value.c. The state is an
 * index into a parsed list of lit colors (0 = off); the shader renders a bezel
 * ring, a color-tinted glass lens, and an emissive glow that is a bright
 * hotspot on a clear lens or an even diffuse fill on a frosted one.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_widget_indicator.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include "bd_color.h"

#include <math.h>
#include <string.h>

#define IND_MAX_STATES 8
#define IND_DEFAULT_COLOR 0xFFB000FFu   /* amber: a clear "lit" default */

struct indicator {
	uint32_t colors[IND_MAX_STATES]; /* lit colors; index i == state i+1 */
	int      ncolors;                /* N */
	int      state;                  /* 0 = off, else 1..N */
	int      lens;
	int      diameter;
	int      clickable;
	int      pressed;
	bd_indicator_cb cb;
	void    *arg;
};

/* ------------------------------------------------------------------ */
/* color parsing (shared core: bd_color.h)                            */
/* ------------------------------------------------------------------ */

/* Parse the state color list, defaulting an empty/all-unparsed string to one
 * amber "lit" state. Returns N (>= 1). */
static int
parse_colors(const char *s, uint32_t *out)
{
	int n = bd_color_parse_list(s, out, IND_MAX_STATES);
	if (n == 0)
		out[n++] = IND_DEFAULT_COLOR;
	return n;
}

/* ------------------------------------------------------------------ */
/* shader                                                             */
/* ------------------------------------------------------------------ */

/*
 * One quad. p is centered [-1,1], y-down. The lens sits at radius u_lensr and a
 * metal bezel ring runs out to u_bezelr; beyond that a halo spills the lit
 * color onto whatever is behind (a panel-mount glow). u_lit scales the
 * emission; u_lens picks the finish: 0 clear (hotspot), 1 frosted (diffuse
 * fill), 2 jewel (radial cut-glass facets that sparkle even when unlit).
 */
static const char *IND_FRAG =
	"#version 300 es\n"
	"precision highp float;\n"
	"in vec2 v_uv;\n"
	"out vec4 frag;\n"
	"uniform vec3  u_color;\n"
	"uniform float u_lit;\n"     /* 0 = off, 1 = energized */
	"uniform int   u_lens;\n"    /* 0 clear, 1 frosted, 2 jewel */
	"uniform float u_press;\n"   /* button depress 0..1 */
	"uniform float u_lensr;\n"
	"uniform float u_bezelr;\n"
	"float h1(float x){ return fract(sin(x) * 43758.5453); }\n"
	"void main(){\n"
	"    vec2 p = (v_uv - 0.5) * 2.0;\n"
	"    float r = length(p);\n"
	"    float aa = fwidth(r) * 1.2;\n"
	"    vec3 col = vec3(0.0);\n"
	"    float alpha = 0.0;\n"
	"    /* halo: color bleeding onto the panel behind, strongest just past the\n"
	"       lens and fading out to the quad edge */\n"
	"    float halo = u_lit * exp(-4.5 * max(r - u_lensr, 0.0)) * 0.6;\n"
	"    col += u_color * halo;\n"
	"    alpha = max(alpha, halo * 0.85);\n"
	"    /* metal bezel ring (panel-mount), lit brighter along the top */\n"
	"    float bez = smoothstep(u_bezelr + aa, u_bezelr - aa, r) *\n"
	"                smoothstep(u_lensr - aa, u_lensr + aa, r);\n"
	"    float top = clamp(0.5 - p.y * 0.5, 0.0, 1.0);\n"
	"    vec3 metal = mix(vec3(0.10,0.11,0.12), vec3(0.30,0.31,0.33), top);\n"
	"    metal *= 1.0 - 0.5 * smoothstep(u_lensr + 0.05, u_lensr, r);\n" /* inner lip shadow */
	"    col = mix(col, metal, bez);\n"
	"    alpha = max(alpha, bez);\n"
	"    /* lens */\n"
	"    float lens = smoothstep(u_lensr + aa, u_lensr - aa, r);\n"
	"    float d = clamp(r / u_lensr, 0.0, 1.0);\n"
	"    vec3 glass = u_color * 0.10 + vec3(0.015);\n" /* dark tinted glass when off */
	"    vec3 lc = glass;\n"
	"    float glow;\n"
	"    if (u_lens == 1) {\n"
	"        glow = 0.9 - 0.25 * d;\n"                    /* even diffuse fill */
	"    } else if (u_lens == 2) {\n"
	"        /* radial cut-glass facets lit by a single top-left key light:\n"
	"           facets facing the light are bright, dark grooves cut between\n"
	"           them, and a small flat table crowns the centre */\n"
	"        float ang = atan(p.y, p.x);\n"
	"        float N = 7.0;\n"                            /* facet count */
	"        float seg = 6.2831853 / N;\n"
	"        float f = fract(ang / seg);\n"
	"        float db = min(f, 1.0 - f);\n"               /* 0 at edge, .5 at centre */
	"        float fc = (floor(ang / seg) + 0.5) * seg;\n"/* facet centre angle */
	"        vec2  fn = vec2(cos(fc), sin(fc));\n"        /* facet outward normal */
	"        vec2  L  = normalize(vec2(-0.55, -0.8));\n"  /* key light, top-left */
	"        float facing = 0.5 + 0.5 * dot(fn, L);\n"    /* 0..1 toward light */
	"        float fh = h1(floor(ang / seg) * 12.9898);\n"/* small irregularity */
	"        float table = smoothstep(0.28, 0.20, d);\n"  /* flat bright centre */
	"        float amb = (0.26 + 0.5 * facing + 0.12 * fh) * (0.6 + 0.4 * table);\n"
	"        lc = mix(glass, u_color * 0.5 + vec3(0.05), amb);\n" /* faceted even off */
	"        float groove = smoothstep(0.07, 0.0, db) *\n"
	"                       smoothstep(0.16, 0.34, d);\n" /* cut lines, not at centre */
	"        lc *= 1.0 - 0.5 * groove;\n"
	"        float glint = smoothstep(0.7, 1.0, db * 2.0) *\n" /* near facet centre */
	"                      smoothstep(0.5, 1.0, facing) *\n"   /* lit facets only */
	"                      smoothstep(0.18, 0.5, d);\n"        /* off the table */
	"        lc += vec3(1.0) * glint * 0.4;\n"
	"        glow = (0.4 + 0.85 * table + 0.35 * facing) *\n"
	"               (0.55 + 0.6 * exp(-3.0 * d * d));\n"
	"    } else {\n"
	"        glow = exp(-7.0 * d * d) * 1.25 + 0.28 * smoothstep(1.0, 0.0, d);\n" /* hotspot */
	"    }\n"
	"    glow = clamp(glow, 0.0, 1.5);\n"
	"    lc = mix(lc, u_color, clamp(glow * u_lit, 0.0, 1.0));\n"
	"    lc += u_color * glow * u_lit * 0.5;\n"           /* overbright core */
	"    if (u_lens != 2) {\n"                            /* dome specular (not jewel) */
	"        float spec = smoothstep(0.22, 0.0, length(p - vec2(-0.24, -0.28)));\n"
	"        lc += vec3(1.0) * spec * (u_lens == 1 ? 0.18 : 0.5);\n"
	"    }\n"
	"    lc *= 1.0 - 0.15 * u_press;\n"
	"    col = mix(col, lc, lens);\n"
	"    alpha = max(alpha, lens);\n"
	"    frag = vec4(col, alpha);\n"
	"}\n";

static bd_shader ind_shader;

static void
ensure_shader(void)
{
	if (ind_shader.id == 0)
		ind_shader = bd_backend_get()->make_shader(BD_SHADER_QUAD_VERT, IND_FRAG);
}

/* ------------------------------------------------------------------ */
/* geometry / layout                                                  */
/* ------------------------------------------------------------------ */

#define IND_DEF_DIAM 18
#define IND_FOOTPRINT 1.8f   /* quad side as a multiple of the lens diameter */
#define IND_GAP       6      /* px between lamp and label */

static int
lamp_diam(const struct indicator *in)
{
	return in->diameter > 0 ? in->diameter : IND_DEF_DIAM;
}

/* Size the widget to the lamp footprint plus an optional label. Called from
 * create once the color/label are set, so the font is already baked. */
static void
size_to_content(bd_id id, const struct indicator *in, const char *label)
{
	int qs = (int)lroundf(lamp_diam(in) * IND_FOOTPRINT);
	int w = qs, h = qs;
	if (label && label[0]) {
		int lh = (int)ceilf(bd_draw_line_height());
		if (h < lh)
			h = lh;
		w += IND_GAP + (int)ceilf(bd_draw_text_width(label));
	}
	bd_set(id, BD_PREF_W_I, w, BD_PREF_H_I, h, BD_END);
}

/* ------------------------------------------------------------------ */
/* class hooks                                                        */
/* ------------------------------------------------------------------ */

static void
ind_init(bd_id id, void *state)
{
	struct indicator *in = state;
	in->ncolors = parse_colors(NULL, in->colors);   /* one amber state */
	in->lens = BD_LENS_CLEAR;
	in->diameter = IND_DEF_DIAM;
	bd_set(id, BD_PREF_W_I, (int)lroundf(IND_DEF_DIAM * IND_FOOTPRINT),
	    BD_PREF_H_I, (int)lroundf(IND_DEF_DIAM * IND_FOOTPRINT), BD_END);
}

static void
ind_render(bd_id id, void *state)
{
	struct indicator *in = state;
	ensure_shader();
	const bd_backend *be = bd_backend_get();

	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);

	int diam = lamp_diam(in);
	int qs = (int)lroundf(diam * IND_FOOTPRINT);
	if (qs > h)                    /* keep the footprint inside the row */
		qs = h;
	float lensr = (float)diam / qs;
	float bezelr = 1.25f * diam / qs;
	if (bezelr > 0.98f)
		bezelr = 0.98f;

	int cx = x + qs / 2;
	int cy = y + h / 2;
	int qx = cx - qs / 2;
	int qy = cy - qs / 2;

	/* color of the current (or, when off, the first) state */
	int ci = in->state > 0 ? in->state - 1 : 0;
	uint32_t c = in->colors[ci];
	float cr = ((c >> 24) & 0xFF) / 255.0f;
	float cg = ((c >> 16) & 0xFF) / 255.0f;
	float cb = ((c >>  8) & 0xFF) / 255.0f;

	be->set_uniform_vec3(ind_shader, "u_color", cr, cg, cb);
	be->set_uniform_float(ind_shader, "u_lit", in->state > 0 ? 1.0f : 0.0f);
	be->set_uniform_int(ind_shader, "u_lens", in->lens);
	be->set_uniform_float(ind_shader, "u_press", in->pressed ? 1.0f : 0.0f);
	be->set_uniform_float(ind_shader, "u_lensr", lensr);
	be->set_uniform_float(ind_shader, "u_bezelr", bezelr);
	bd_draw_shader_quad(ind_shader, qx, qy, qs, qs);

	const char *label = bd_get_s(id, BD_LABEL_S);
	if (label && label[0]) {
		const bd_theme *th = bd_gui_theme();
		float lh = bd_draw_line_height();
		bd_draw_text(label, (float)(x + qs + IND_GAP),
		    (float)cy - lh * 0.5f, th->text);
	}
}

static int
ind_event(bd_id id, void *state, const bd_event *ev)
{
	struct indicator *in = state;
	switch (ev->type) {
	case BD_EV_MOUSE_DOWN:
		if (ev->button == BD_MOUSE_LEFT) {
			in->pressed = 1;
			return 1;
		}
		return 0;
	case BD_EV_MOUSE_UP:
		if (ev->button == BD_MOUSE_LEFT) {
			in->pressed = 0;
			int x, y, w, h;
			bd_widget_rect(id, &x, &y, &w, &h);
			if (ev->x >= x && ev->x < x + w &&
			    ev->y >= y && ev->y < y + h) {
				in->state = (in->state + 1) % (in->ncolors + 1);
				if (in->cb)
					in->cb(id, in->arg, in->state);
			}
			return 1;
		}
		return 0;
	}
	return 0;
}

/* Two registered classes over the same state/render: an output-only lamp (no
 * event hook, so clicks pass through) and a clickable lamp button. */
static const bd_widget_class ind_class = {
	.name = "indicator",
	.state_size = sizeof(struct indicator),
	.init = ind_init,
	.render = ind_render,
};

static const bd_widget_class ind_btn_class = {
	.name = "indicator_button",
	.state_size = sizeof(struct indicator),
	.init = ind_init,
	.render = ind_render,
	.event = ind_event,
};

static int ind_type;
static int ind_btn_type;

bd_id
bd_indicator_create(bd_id parent, const bd_indicator_desc *desc, ...)
{
	if (ind_type == 0)
		ind_type = bd_register_widget_class(&ind_class);
	if (ind_btn_type == 0)
		ind_btn_type = bd_register_widget_class(&ind_btn_class);

	int type = (desc && desc->clickable) ? ind_btn_type : ind_type;

	va_list ap;
	va_start(ap, desc);
	bd_id id = bd_create_va(parent, type, ap);
	va_end(ap);

	struct indicator *in = bd_widget_state(id);
	if (in && desc) {
		in->ncolors = parse_colors(desc->colors, in->colors);
		in->lens = desc->lens;
		in->diameter = desc->diameter > 0 ? desc->diameter : IND_DEF_DIAM;
		in->clickable = desc->clickable;
		in->cb = desc->cb;
		in->arg = desc->arg;
		in->state = desc->state;
		if (in->state < 0) in->state = 0;
		if (in->state > in->ncolors) in->state = in->ncolors;
		if (desc->label)
			bd_set(id, BD_LABEL_S, desc->label, BD_END);
		size_to_content(id, in, desc->label);
	}
	return id;
}

static struct indicator *
indicator_of(bd_id id)
{
	int t = bd_widget_type(id);
	if (t != ind_type && t != ind_btn_type)
		return NULL;
	return bd_widget_state(id);
}

void
bd_indicator_set_colors(bd_id id, const char *colors)
{
	struct indicator *in = indicator_of(id);
	if (!in)
		return;
	in->ncolors = parse_colors(colors, in->colors);
	if (in->state > in->ncolors)
		in->state = in->ncolors;
}

void
bd_indicator_set(bd_id id, int state)
{
	struct indicator *in = indicator_of(id);
	if (!in)
		return;
	if (state < 0) state = 0;
	if (state > in->ncolors) state = in->ncolors;
	in->state = state;
}

int
bd_indicator_get(bd_id id)
{
	struct indicator *in = indicator_of(id);
	return in ? in->state : 0;
}

int
bd_indicator_states(bd_id id)
{
	struct indicator *in = indicator_of(id);
	return in ? in->ncolors + 1 : 0;
}
