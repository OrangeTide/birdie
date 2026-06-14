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
#include "bd_widget_explorer.h"
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
/* advance each call so consecutive clicks aren't mistaken for double-clicks */
static double be_time(void)   { static double t; t += 1.0; return t; }
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

/* ---- multi-window stub: like `stub` but advertises native windows ---- */
static int mw_next_id = 2;          /* id 1 is the adopted primary */
static int mw_open(const char *t, int w, int h){ (void)t;(void)w;(void)h; return mw_next_id++; }
static void mw_close(int id){ (void)id; }
static void mw_begin(int id){ (void)id; }
static void mw_swap(int id){ (void)id; }
static int  mw_w(int id){ (void)id; return 400; }
static int  mw_h(int id){ (void)id; return 300; }
static void mw_title(int id, const char *t){ (void)id; (void)t; }

static const bd_backend mwstub = {
	.width=be_width, .height=be_height, .time=be_time, .viewport=be_viewport,
	.clear=be_clear,
	.make_shader=be_make_shader, .destroy_shader=be_destroy_shader,
	.use_shader=be_use_shader, .set_uniform_int=be_uni_i, .set_uniform_float=be_uni_f,
	.set_uniform_vec2=be_uni_2, .set_uniform_vec3=be_uni_3, .set_uniform_vec4=be_uni_4,
	.set_uniform_mat4=be_uni_m, .draw_verts=be_draw_verts,
	.load_texture=be_load_tex, .make_texture=be_make_tex, .update_texture=be_update_tex,
	.bind_texture=be_bind_tex, .destroy_texture=be_destroy_tex,
	.scissor=be_scissor, .scissor_off=be_scissor_off,
	.multi_window=1, .window_open=mw_open, .window_close=mw_close,
	.window_begin=mw_begin, .window_swap=mw_swap,
	.window_width=mw_w, .window_height=mw_h, .window_set_title=mw_title,
};

/* per-window click counters */
static int click_w1, click_w2;
static void on_click_w1(bd_id id, void *a){ (void)id; (void)a; click_w1++; }
static void on_click_w2(bd_id id, void *a){ (void)id; (void)a; click_w2++; }

/* ---- in-memory explorer model (3 auto-placed icons) ---- */
static struct exp_item { uint64_t key; const char *label; int x, y; } exp_items[] = {
	{ 100, "a", -1, -1 }, { 101, "b", -1, -1 }, { 102, "c", -1, -1 },
};
static int exp_count(void *ctx){ (void)ctx; return 3; }
static void
exp_get(void *ctx, int i, bd_explorer_item *out)
{
	(void)ctx;
	out->key = exp_items[i].key;
	out->label = exp_items[i].label;
	out->icon = (bd_texture){0};
	out->enabled = 1;
	out->x = exp_items[i].x;
	out->y = exp_items[i].y;
	out->user = &exp_items[i];
}
static int exp_setpos_n;
static void
exp_set_pos(void *ctx, uint64_t key, int x, int y)
{
	(void)ctx;
	for (int i = 0; i < 3; i++)
		if (exp_items[i].key == key) { exp_items[i].x = x; exp_items[i].y = y; }
	exp_setpos_n++;
}
static int exp_moved_n; static uint64_t exp_moved_key; static int exp_moved_x, exp_moved_y;
static void
exp_moved(bd_id w, uint64_t key, int x, int y, void *ctx)
{
	(void)w; (void)ctx;
	exp_moved_n++; exp_moved_key = key; exp_moved_x = x; exp_moved_y = y;
}
static int exp_selchg_n;
static void exp_selchg(bd_id w, void *ctx){ (void)w; (void)ctx; exp_selchg_n++; }

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
static int wheel_calls; static float wheel_acc;
static void on_wheel_test(bd_id id, void *arg, float d)
{ (void)id; (void)arg; wheel_calls++; wheel_acc += d; }
static int xy_calls; static float xy_x, xy_y;
static void on_xy_test(bd_id id, void *arg, float x, float y)
{ (void)id; (void)arg; xy_calls++; xy_x = x; xy_y = y; }

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
	bd_id wh = bd_wheel_create(frame, BD_VERTICAL, on_wheel_test, NULL,
	    BD_PREF_H_I, 60, BD_END);
	bd_id xy = bd_xypad_create(frame, &(bd_xypad_desc){
	    .shape = BD_XY_CIRCLE, .x = 0.5f, .y = 0.5f, .cb = on_xy_test },
	    BD_PREF_H_I, 76, BD_END);

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

	/* jog dial: relative drag emits deltas (no absolute value) */
	knob_calls = 0;
	bd_id jog = bd_knob_create(frame, &(bd_knob_desc){
	    .relative = 1, .dimples = 3, .cb = on_knob_test },
	    BD_PREF_H_I, 84, BD_END);
	bd_gui_layout(800, 500);
	int jx, jy, jw, jh;
	bd_widget_rect(jog, &jx, &jy, &jw, &jh);
	bd_event jd = mouse(BD_EV_MOUSE_DOWN, jx + jw / 2, jy + jh / 2);
	bd_event jm = mouse(BD_EV_MOUSE_MOVE, jx + jw / 2, jy + jh / 2 - 20);
	bd_gui_event(&jd);
	bd_gui_event(&jm);
	check("jog dial drag emits a delta", knob_calls > 0);

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

	/* scroll wheel: vertical drag up emits a positive (relative) delta */
	int whx, why, whw, whh;
	bd_widget_rect(wh, &whx, &why, &whw, &whh);
	bd_event whd = mouse(BD_EV_MOUSE_DOWN, whx + whw / 2, why + whh / 2);
	bd_event whm = mouse(BD_EV_MOUSE_MOVE, whx + whw / 2, why + whh / 2 - 30);
	bd_gui_event(&whd);
	bd_gui_event(&whm);
	check("wheel drag up emits positive delta",
	    wheel_calls > 0 && wheel_acc > 0.0f);

	/* X-Y pad: drag sets x,y; circle limit clamps to the unit circle */
	check("xypad initial center", xy_x == 0.0f && xy_y == 0.0f);  /* not fired yet */
	int pdx, pdy, pdw, pdh;
	bd_widget_rect(xy, &pdx, &pdy, &pdw, &pdh);
	int ps = pdw < pdh ? pdw : pdh;
	int psx = pdx + (pdw - ps) / 2, psy = pdy + (pdh - ps) / 2;
	bd_event xyd = mouse(BD_EV_MOUSE_DOWN, psx + ps / 4, psy + ps / 4);
	bd_gui_event(&xyd);
	check("xypad drag reports a position + callback",
	    xy_calls > 0 && xy_x < 0.5f && xy_y > 0.5f);   /* upper-left quadrant */
	bd_xypad_set(xy, 2.0f, 2.0f);   /* far corner: circle clamps inside */
	float gx, gy;
	bd_xypad_get(xy, &gx, &gy);
	float dxc = gx - 0.5f, dyc = gy - 0.5f;
	check("xypad circle clamps to unit radius",
	    dxc * dxc + dyc * dyc <= 0.25f + 0.001f);

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

	/* ---- multiple windows: event routing by window id ---- */
	bd_gui_init(&mwstub, NULL);

	bd_id f1 = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id b1 = bd_create(f1, BD_BUTTON, BD_LABEL_S, "one", BD_GROW_I, 1,
	    BD_ON_CLICK_F, on_click_w1, BD_END);
	bd_id f2 = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id b2 = bd_create(f2, BD_BUTTON, BD_LABEL_S, "two", BD_GROW_I, 1,
	    BD_ON_CLICK_F, on_click_w2, BD_END);

	/* first frame adopts the primary (window 1); the second opens window 2 */
	check("primary frame adopts window 1", bd_frame_for_window(1) == f1);
	check("second frame opened window 2", bd_frame_for_window(2) == f2);

	bd_gui_layout(0, 0);   /* multi_window: sizes come from the backend */

	/* both buttons fill their window, so they share the same coordinates;
	 * only ev.window distinguishes which one a click reaches */
	int x1, y1, w1_, h1_;
	bd_widget_rect(b1, &x1, &y1, &w1_, &h1_);
	int mcx = x1 + w1_/2, mcy = y1 + h1_/2;

	bd_event d1 = mouse(BD_EV_MOUSE_DOWN, mcx, mcy); d1.window = 1;
	bd_event u1 = mouse(BD_EV_MOUSE_UP,   mcx, mcy); u1.window = 1;
	bd_gui_event(&d1); bd_gui_event(&u1);
	check("click tagged window 1 hit frame-1 button",
	    click_w1 == 1 && click_w2 == 0);

	bd_event d2 = mouse(BD_EV_MOUSE_DOWN, mcx, mcy); d2.window = 2;
	bd_event u2 = mouse(BD_EV_MOUSE_UP,   mcx, mcy); u2.window = 2;
	bd_gui_event(&d2); bd_gui_event(&u2);
	check("click tagged window 2 hit frame-2 button",
	    click_w2 == 1 && click_w1 == 1);

	/* render must visit both windows */
	n_drawverts = 0;
	bd_gui_render();
	check("render drew both windows", n_drawverts > 0);

	/* closing the second frame releases its window and unregisters it */
	bd_destroy(f2);
	check("destroying frame-2 frees window 2", bd_frame_for_window(2) == BD_NONE);
	(void)b2;

	bd_gui_cleanup();

	/* ---- explorer: drag-move and rubber-band selection ---- */
	bd_gui_init(&stub, NULL);
	bd_explorer_model emodel = {
	    .count = exp_count, .get = exp_get, .set_pos = exp_set_pos };
	bd_explorer_cb ecb = { .moved = exp_moved, .selection_changed = exp_selchg };

	bd_id eframe = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id expl = bd_explorer_create(eframe, &emodel, &ecb, BD_GROW_I, 1, BD_END);
	bd_explorer_set_icon_size(expl, 48);   /* cell 64 wide; row of 3 at x=8,72,136 */
	bd_gui_layout(800, 600);

	int ex, ey, ew, eh;
	bd_widget_rect(expl, &ex, &ey, &ew, &eh);
	check("explorer got geometry", ew > 200 && eh > 200);

	/* press item 0 (cell at content 8,8), drag +100,+40, release */
	int px = ex + 13, py = ey + 13;
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=px, .y=py });
	check("press selected item 0", bd_explorer_selection(expl, NULL, 0) == 1);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_MOVE, .x=px+100, .y=py+40 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP, .button=BD_MOUSE_LEFT, .x=px+100, .y=py+40 });
	check("drag-move committed via set_pos", exp_setpos_n == 1);
	check("drag-move reported moved() to base+offset",
	    exp_moved_n == 1 && exp_moved_key == 100 &&
	    exp_moved_x == 108 && exp_moved_y == 48);
	check("dragged item's saved position updated",
	    exp_items[0].x == 108 && exp_items[0].y == 48);

	/* reset to auto-placement and rubber-band over the whole first row */
	for (int i = 0; i < 3; i++) { exp_items[i].x = -1; exp_items[i].y = -1; }
	bd_gui_layout(800, 600);
	int bdownx = ex + 10, bdowny = ey + 200;     /* empty space below the row */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=bdownx, .y=bdowny });
	check("band on empty space cleared selection",
	    bd_explorer_selection(expl, NULL, 0) == 0);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_MOVE, .x=ex+200, .y=ey+5 });
	uint64_t skeys[8];
	check("rubber-band covers the three icons",
	    bd_explorer_selection(expl, skeys, 8) == 3);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP, .button=BD_MOUSE_LEFT, .x=ex+200, .y=ey+5 });

	/* Shift-range selection: items 0,1,2 sit at content x=8,72,136 (row 0) */
	int i0x = ex + 13, i1x = ex + 77, i2x = ex + 141, iy = ey + 13;
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=i0x, .y=iy });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=i0x, .y=iy });
	check("plain click selects one (sets the anchor)",
	    bd_explorer_selection(expl, NULL, 0) == 1);
	/* Shift-click item 2 -> range [0..2] */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT,
	    .mods=BD_MOD_SHIFT, .x=i2x, .y=iy });
	check("Shift-click selects the anchor..clicked range",
	    bd_explorer_selection(expl, NULL, 0) == 3);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP, .button=BD_MOUSE_LEFT,
	    .mods=BD_MOD_SHIFT, .x=i2x, .y=iy });
	/* Shift-click item 1 re-ranges from the same anchor -> [0..1] */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT,
	    .mods=BD_MOD_SHIFT, .x=i1x, .y=iy });
	check("Shift-click re-ranges from the unchanged anchor",
	    bd_explorer_selection(expl, NULL, 0) == 2);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP, .button=BD_MOUSE_LEFT,
	    .mods=BD_MOD_SHIFT, .x=i1x, .y=iy });

	bd_gui_render();   /* exercises the band/selection render path */
	bd_gui_cleanup();

	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
