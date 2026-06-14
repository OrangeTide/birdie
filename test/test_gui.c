/*
 * Headless functional test for the birdie-gui bundle.
 *
 * Drives the toolkit through a recording stub backend (no window, no ludica,
 * no X11). Exercises: backend injection, widget tree + layout, button click
 * via synthesized events, custom theme, terminal widget + write + 256-color
 * palette, and render. Exit code 0 = all checks passed.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */
#include "widget.h"
#include "widget_ext.h"
#include "bd_widget_vt.h"
#include "bd_widget_value.h"
#include "bd_draw.h"
#include <stdio.h>
#include <string.h>

static int checks, fails;
static void
check(const char *what, int ok)
{
	checks++;
	if (!ok) fails++;
	printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
}

/* ---- recording stub backend (GPU interface) ---- */
static int n_shader, n_drawverts, n_maketex, n_scissor;
static unsigned next_texid = 1;   /* distinct id per texture, like a real backend */

static int    be_width(void)  { return 800; }
static int    be_height(void) { return 500; }
static double be_time(void)   { return 0.0; }
static void   be_viewport(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
static void   be_clear(float r,float g,float b,float a){(void)r;(void)g;(void)b;(void)a;}

static bd_shader be_make_shader(const char *v, const char *f)
{ (void)v; (void)f; n_shader++; return (bd_shader){1}; }
static void be_destroy_shader(bd_shader s){ (void)s; }
static void be_use_shader(bd_shader s){ (void)s; }
static void be_uni_i(bd_shader s,const char *n,int v){ (void)s;(void)n;(void)v; }
static void be_uni_f(bd_shader s,const char *n,float v){ (void)s;(void)n;(void)v; }
static void be_uni_2(bd_shader s,const char *n,float x,float y){ (void)s;(void)n;(void)x;(void)y; }
static void be_uni_3(bd_shader s,const char *n,float x,float y,float z){ (void)s;(void)n;(void)x;(void)y;(void)z; }
static void be_uni_4(bd_shader s,const char *n,float x,float y,float z,float w){ (void)s;(void)n;(void)x;(void)y;(void)z;(void)w; }
static void be_uni_m(bd_shader s,const char *n,const float m[16]){ (void)s;(void)n;(void)m; }
static void be_draw_verts(const bd_vertex *v, int n){ (void)v; (void)n; n_drawverts++; }

static bd_texture be_load_tex(const char *p){ (void)p; return (bd_texture){next_texid++}; }
static bd_texture be_make_tex(int w, int h, const void *px)
{ (void)w; (void)h; (void)px; n_maketex++; return (bd_texture){next_texid++}; }
static void be_update_tex(bd_texture t,int x,int y,int w,int h,const void *px)
{ (void)t;(void)x;(void)y;(void)w;(void)h;(void)px; }
static void be_bind_tex(bd_texture t,int u){ (void)t;(void)u; }
static void be_destroy_tex(bd_texture t){ (void)t; }

static void be_scissor(int x,int y,int w,int h){ (void)x;(void)y;(void)w;(void)h; n_scissor++; }
static void be_scissor_off(void){}

static const bd_backend stub = {
	.width=be_width, .height=be_height, .time=be_time, .viewport=be_viewport,
	.clear=be_clear,
	.make_shader=be_make_shader, .destroy_shader=be_destroy_shader,
	.use_shader=be_use_shader, .set_uniform_int=be_uni_i, .set_uniform_float=be_uni_f,
	.set_uniform_vec2=be_uni_2, .set_uniform_vec3=be_uni_3, .set_uniform_vec4=be_uni_4,
	.set_uniform_mat4=be_uni_m, .draw_verts=be_draw_verts,
	.load_texture=be_load_tex, .make_texture=be_make_tex, .update_texture=be_update_tex,
	.bind_texture=be_bind_tex, .destroy_texture=be_destroy_tex,
	.scissor=be_scissor, .scissor_off=be_scissor_off,
};

/* ---- click callback ---- */
static int clicked;
static void on_click(bd_id id, void *arg){ (void)id; (void)arg; clicked++; }

/* ---- slider / knob callbacks ---- */
static int slider_calls;
static void on_slider_test(bd_id id, void *arg, float t)
{ (void)id; (void)arg; (void)t; slider_calls++; }
static int knob_calls;
static void on_knob_test(bd_id id, void *arg, float t)
{ (void)id; (void)arg; (void)t; knob_calls++; }
static int toggle_calls, toggle_last;
static void on_toggle_test(bd_id id, void *arg, int on)
{ (void)id; (void)arg; toggle_calls++; toggle_last = on; }

/* ---- drag-recording extension widget (stands in for a slider/knob) ---- */
static int rec_down, rec_move, rec_up;
static int
rec_event(bd_id id, void *state, const bd_event *ev)
{
	(void)id; (void)state;
	if (ev->type == BD_EV_MOUSE_DOWN)      rec_down++;
	else if (ev->type == BD_EV_MOUSE_MOVE) rec_move++;
	else if (ev->type == BD_EV_MOUSE_UP)   rec_up++;
	return 1;
}
static const bd_widget_class rec_class = {
	.name = "rec", .state_size = 0, .event = rec_event,
};
static int rec_type;

static bd_event
mouse(int type, int x, int y)
{
	bd_event e; memset(&e, 0, sizeof e);
	e.type = type; e.x = x; e.y = y; e.button = BD_MOUSE_LEFT;
	return e;
}

int
main(void)
{
	/* custom theme to exercise the config path */
	bd_theme th = bd_theme_default();
	th.focus = 0x88CCFFFFu;
	bd_gui_init(&stub, &th);

	bd_id frame = bd_create(BD_NONE, BD_FRAME,
	    BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id term = bd_terminal_create(frame, BD_GROW_I, 1, BD_END);
	bd_id btn = bd_create(frame, BD_BUTTON,
	    BD_LABEL_S, "Go", BD_PREF_H_I, 28,
	    BD_ON_CLICK_F, on_click, BD_END);

	rec_type = bd_register_widget_class(&rec_class);
	bd_id rec = bd_create(frame, rec_type, BD_PREF_H_I, 40, BD_END);

	bd_id sld = bd_slider_create(frame, BD_HORIZONTAL, 0.5f,
	    on_slider_test, NULL, BD_PREF_H_I, 20, BD_END);
	bd_id knb = bd_knob_create(frame, &(bd_knob_desc){
	    .min = 0, .max = 1, .value = 0.5f, .cb = on_knob_test },
	    BD_PREF_H_I, 56, BD_END);
	bd_id ksw = bd_knob_create(frame, &(bd_knob_desc){
	    .min = 0, .max = 6, .step = 1, .value = 2, .dial = BD_DIAL_DOTS },
	    BD_PREF_H_I, 56, BD_END);
	bd_id tg = bd_toggle_create(frame, 0, on_toggle_test, NULL,
	    BD_PREF_H_I, 26, BD_END);

	check("widgets created", frame && term && btn && rec && sld && knb);

	bd_gui_layout(800, 500);

	int bx, by, bw, bh;
	bd_widget_rect(btn, &bx, &by, &bw, &bh);
	check("button got nonzero geometry", bw > 0 && bh > 0);

	int tx, ty, tw, tht;
	bd_widget_rect(term, &tx, &ty, &tw, &tht);
	check("terminal grew to fill width", tw > 0 && tht > 0);

	/* synthesize a click on the button center */
	int cx = bx + bw/2, cy = by + bh/2;
	bd_gui_event(&(bd_event){0});           /* harmless */
	bd_event d = mouse(BD_EV_MOUSE_DOWN, cx, cy);
	bd_event u = mouse(BD_EV_MOUSE_UP, cx, cy);
	bd_gui_event(&d);
	bd_gui_event(&u);
	check("button click fired callback", clicked == 1);

	/* drag the extension widget; capture must keep delivering moves even
	 * after the pointer leaves its rect, and release on mouse up */
	int rx, ry, rw, rh;
	bd_widget_rect(rec, &rx, &ry, &rw, &rh);
	int rcx = rx + rw/2, rcy = ry + rh/2;
	bd_event rd  = mouse(BD_EV_MOUSE_DOWN, rcx, rcy);
	bd_event rm1 = mouse(BD_EV_MOUSE_MOVE, rcx + 5, rcy + 5);   /* inside */
	bd_event rm2 = mouse(BD_EV_MOUSE_MOVE, rcx + 9000, rcy);    /* far outside */
	bd_event ru  = mouse(BD_EV_MOUSE_UP, rcx + 9000, rcy);
	bd_gui_event(&rd);
	bd_gui_event(&rm1);
	bd_gui_event(&rm2);
	bd_gui_event(&ru);
	check("extension widget received mouse down", rec_down == 1);
	check("captured drag delivers moves past the widget rect", rec_move == 2);
	check("extension widget received mouse up", rec_up == 1);

	/* after release, a move far away must NOT reach the widget */
	int prev_move = rec_move;
	bd_event after = mouse(BD_EV_MOUSE_MOVE, rcx + 9000, rcy);
	bd_gui_event(&after);
	check("capture released on mouse up", rec_move == prev_move);

	/* slider: set/get and pointer-driven value */
	check("slider initial value 0.5", bd_slider_get(sld) == 0.5f);
	bd_slider_set(sld, 0.25f);
	check("slider set/get round-trips", bd_slider_get(sld) == 0.25f);
	int sx, sy, sw, sh;
	bd_widget_rect(sld, &sx, &sy, &sw, &sh);
	int smy = sy + sh / 2;
	bd_event sr = mouse(BD_EV_MOUSE_DOWN, sx + sw - 1, smy);
	bd_event sru = mouse(BD_EV_MOUSE_UP, sx + sw - 1, smy);
	bd_gui_event(&sr);
	bd_gui_event(&sru);
	check("slider drag to right edge -> ~1 + callback",
	    bd_slider_get(sld) > 0.9f && slider_calls > 0);
	bd_event sl = mouse(BD_EV_MOUSE_DOWN, sx, smy);
	bd_event slu = mouse(BD_EV_MOUSE_UP, sx, smy);
	bd_gui_event(&sl);
	bd_gui_event(&slu);
	check("slider drag to left edge -> ~0", bd_slider_get(sld) < 0.1f);

	/* knob: vertical drag (up) raises the value past its capture */
	check("knob initial value 0.5", bd_knob_get(knb) == 0.5f);
	int kx, ky, kw, kh;
	bd_widget_rect(knb, &kx, &ky, &kw, &kh);
	int kcx = kx + kw / 2, kcy = ky + kh / 2;
	bd_event kd = mouse(BD_EV_MOUSE_DOWN, kcx, kcy);
	bd_event km = mouse(BD_EV_MOUSE_MOVE, kcx, kcy - 100);   /* drag up */
	bd_event ku = mouse(BD_EV_MOUSE_UP, kcx, kcy - 100);
	bd_gui_event(&kd);
	bd_gui_event(&km);
	bd_gui_event(&ku);
	check("knob drag up raises value + callback",
	    bd_knob_get(knb) > 0.9f && knob_calls > 0);

	/* rotary switch: range 0..6 step 1 snaps to integer detents */
	check("rotary switch initial value 2", bd_knob_get(ksw) == 2.0f);
	bd_knob_set(ksw, 4.4f);
	check("rotary switch snaps to step (4.4 -> 4)", bd_knob_get(ksw) == 4.0f);
	bd_knob_set(ksw, 99.0f);
	check("rotary switch clamps to max (6)", bd_knob_get(ksw) == 6.0f);

	/* toggle: click flips, set/get */
	check("toggle initial off", bd_toggle_get(tg) == 0);
	int tgx, tgy, tgw, tgh;
	bd_widget_rect(tg, &tgx, &tgy, &tgw, &tgh);
	bd_event tdn = mouse(BD_EV_MOUSE_DOWN, tgx + tgw / 2, tgy + tgh / 2);
	bd_event tup = mouse(BD_EV_MOUSE_UP, tgx + tgw / 2, tgy + tgh / 2);
	bd_gui_event(&tdn);
	bd_gui_event(&tup);
	check("toggle click flips on + callback",
	    bd_toggle_get(tg) == 1 && toggle_calls == 1 && toggle_last == 1);
	bd_toggle_set(tg, 0);
	check("toggle set off", bd_toggle_get(tg) == 0);

	/* terminal write: plain text + bold + 256-color SGR */
	bd_terminal_write(term,
	    "\033[1mbold\033[0m \033[38;5;202morange\033[0m\r\n", -1);
	bd_terminal_write(term, "second line\r\n", -1);

	/* custom palette */
	bd_palette pal = bd_palette_default();
	pal.ansi[1] = 0xE06C75FFu;
	bd_terminal_set_palette(term, &pal);

	/* rendering now flows through the toolkit renderer (bd_draw) onto the
	 * GPU interface; bd_gui_init already created the shader + mesh and baked
	 * the chrome font */
	check("renderer initialized shader", n_shader == 1);
	check("chrome font baked (real DejaVu): text width > 0",
	    bd_draw_text_width("Quit") > 0.0f);
	n_drawverts = 0;
	bd_gui_render();
	check("render issued GPU draws (chrome + terminal)", n_drawverts > 0);

	/* write to a non-terminal must be a safe no-op */
	bd_terminal_write(btn, "ignored", -1);
	check("write to non-terminal is a no-op (no crash)", 1);

	bd_gui_cleanup();
	check("cleanup completed", 1);

	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
