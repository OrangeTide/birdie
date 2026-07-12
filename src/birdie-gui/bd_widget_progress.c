/*
 * bd_widget_progress.c -- a plain determinate/indeterminate progress bar.
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
	int      glass;
	int      orient;
	uint32_t color;
};

static int prog_type;

/* ------------------------------------------------------------------ */
/* glass liquid-tube shader (matches BD_METER_VIAL)                   */
/* ------------------------------------------------------------------ */

static const char *TUBE_FRAG =
	"#version 300 es\n"
	"precision highp float;\n"
	"in vec2 v_uv;\n"
	"out vec4 frag;\n"
	"uniform vec3  u_color;\n"
	"uniform float u_value;\n"
	"uniform float u_aspect;\n"   /* main length / cross thickness */
	"uniform int   u_vert;\n"
	"void main(){\n"
	"    float mn = (u_vert==1) ? (1.0 - v_uv.y) : v_uv.x;\n"  /* along fill */
	"    float cr = (u_vert==1) ? v_uv.x : v_uv.y;\n"          /* across tube */
	"    /* rounded-capsule SDF in a space where the cross axis spans 1.0 */\n"
	"    vec2 q = vec2((mn-0.5)*u_aspect, cr-0.5);\n"
	"    vec2 dd = abs(q) - vec2(u_aspect*0.5 - 0.5, 0.0);\n"
	"    float sd = length(max(dd,0.0)) + min(max(dd.x,dd.y),0.0) - 0.5;\n"
	"    float aa = fwidth(sd)*1.2;\n"
	"    float body = smoothstep(aa, -aa, sd);\n"
	"    if (body <= 0.001) { frag = vec4(0.0); return; }\n"
	"    float on = step(0.004, u_value);\n"
	"    float liquid = smoothstep(-0.02, 0.02, u_value - mn) * on;\n"
	"    float men = exp(-pow((mn - u_value)/0.03, 2.0)) * on;\n"  /* meniscus */
	"    float vol = 1.0 - pow(abs(cr-0.5)*2.0, 2.0);\n"           /* tube volume */
	"    vec3 lc = u_color * (0.72 + 0.5*vol);\n"
	"    lc += (u_color*0.6 + vec3(0.4))*men*0.7;\n"
	"    vec3 empty = mix(vec3(0.05,0.06,0.08), u_color*0.10, 0.35);\n"
	"    vec3 col = mix(empty, lc, liquid);\n"
	"    col += vec3(1.0)*smoothstep(0.18,0.0,cr)*0.16;\n"        /* top gloss */
	"    float rim = smoothstep(-0.07, 0.0, sd);\n"
	"    col = mix(col, vec3(0.60,0.68,0.80)*0.7, rim*0.5);\n"    /* glass rim */
	"    frag = vec4(col, body);\n"
	"}\n";

static bd_shader tube_shader;

static void
draw_glass_tube(uint32_t color, float value, int vert,
    int x, int y, int w, int h)
{
	const bd_backend *be = bd_backend_get();
	if (tube_shader.id == 0)
		tube_shader = be->make_shader(BD_SHADER_QUAD_VERT, TUBE_FRAG);
	float cr = ((color>>24)&0xFF)/255.0f, cg = ((color>>16)&0xFF)/255.0f,
	      cb = ((color>>8)&0xFF)/255.0f;
	float aspect = vert ? (w > 0 ? (float)h / w : 1.0f)
	                    : (h > 0 ? (float)w / h : 1.0f);
	if (aspect < 1.0f) aspect = 1.0f;
	be->set_uniform_vec3(tube_shader, "u_color", cr, cg, cb);
	be->set_uniform_float(tube_shader, "u_value", value);
	be->set_uniform_float(tube_shader, "u_aspect", aspect);
	be->set_uniform_int(tube_shader, "u_vert", vert);
	bd_draw_shader_quad(tube_shader, x, y, w, h);
}

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

	int vert = (p->orient == BD_VERTICAL);

	/* glass liquid tube: a self-contained shader (its own body + rim) */
	if (p->glass) {
		draw_glass_tube(p->color, p->value, vert, bx, y, bw, h);
		if (p->show_percent) {
			char buf[8];
			snprintf(buf, sizeof buf, "%d%%",
			    (int)lroundf(p->value * 100.0f));
			float tw = bd_draw_text_width(buf);
			bd_draw_text(buf, (float)bx + (bw - tw) * 0.5f,
			    (float)(y + (h - lh) * 0.5f), th->text_hi);
		}
		return;
	}

	bd_draw_rect(bx, y, bw, h, th->press);          /* track */
	bd_draw_rect_lines(bx, y, bw, h, th->border);

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
			p->glass = desc->glass;
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
