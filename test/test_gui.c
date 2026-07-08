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
#include "bd_widget_editor.h"
#include "bd_widget_canvas.h"
#include "bd_widget_table.h"
#include "bd_widget_inventory.h"
#include "bd_widget_dock.h"
#include "bd_widget_actionbar.h"
#include "bd_draw.h"
#include <stdio.h>
#include <string.h>

/* ---- table model: deliberately unsorted, with string!=numeric port order ---- */
static const char *tbl_data[3][2] = {
	{ "Discworld", "4242" },
	{ "Aardwolf",  "23"   },
	{ "Achaea",    "100"  },
};
static int tbl_rows(void *c) { (void)c; return 3; }
static const char *tbl_cell(void *c, int r, int col)
{ (void)c; return (r >= 0 && r < 3 && col >= 0 && col < 2) ? tbl_data[r][col] : ""; }
static int tbl_sel_changed;
static void tbl_on_sel(bd_id w, void *c) { (void)w; (void)c; tbl_sel_changed++; }
static int tbl_activated_row = -1;
static void tbl_on_activate(bd_id w, int row, void *c) { (void)w; (void)c; tbl_activated_row = row; }

/* ---- inventory grid model + recorded callbacks ---- */
static struct inv_slot { uint64_t key; const char *label; bd_texture icon;
    int count; int enabled; } inv_slots[80];
static void
inv_get(void *c, int slot, bd_inventory_item *out)
{
	(void)c;
	if (slot < 0 || slot >= 80) return;
	out->key     = inv_slots[slot].key;
	out->label   = inv_slots[slot].label;
	out->icon    = inv_slots[slot].icon;
	out->count   = inv_slots[slot].count;
	out->enabled = inv_slots[slot].enabled;
}
static int inv_act = -1;
static void inv_on_act(bd_id w, int s, uint64_t k, void *c)
{ (void)w; (void)k; (void)c; inv_act = s; }
static int inv_mfrom = -1, inv_mto = -1;
static void inv_on_move(bd_id w, int f, int t, void *c)
{
	(void)w; (void)c;
	inv_mfrom = f; inv_mto = t;
	struct inv_slot tmp = inv_slots[t];   /* app performs the swap */
	inv_slots[t] = inv_slots[f];
	inv_slots[f] = tmp;
}
static int inv_ctx = -1;
static void inv_on_ctx(bd_id w, int s, uint64_t k, int sx, int sy, void *c)
{ (void)w; (void)k; (void)sx; (void)sy; (void)c; inv_ctx = s; }
static int inv_hov = -2;
static void inv_on_hover(bd_id w, int s, uint64_t k, void *c)
{ (void)w; (void)k; (void)c; inv_hov = s; }

/* ---- action bar recorded callbacks ---- */
static int ab_act_slot = -1; static uint64_t ab_act_key;
static void ab_on_act(bd_id w, int s, uint64_t k, void *c)
{ (void)w; (void)c; ab_act_slot = s; ab_act_key = k; }
static int ab_drop_slot = -1; static uint64_t ab_drop_key;
static int ab_on_drop(bd_id w, int s, const bd_dnd_payload *p, void *c)
{ (void)w; (void)c; ab_drop_slot = s; ab_drop_key = p->key; return 1; }
static int ab_move_from = -1, ab_move_to = -1;
static void ab_on_move(bd_id w, int f, int t, void *c)
{ (void)w; (void)c; ab_move_from = f; ab_move_to = t; }

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
static bd_texture be_load_tex_mem(const unsigned char *d, int n){ (void)d; (void)n; return (bd_texture){next_texid++}; }
static bd_texture be_make_tex(int w, int h, const void *px)
{ (void)w; (void)h; (void)px; n_maketex++; return (bd_texture){next_texid++}; }
static void be_update_tex(bd_texture t,int x,int y,int w,int h,const void *px)
{ (void)t;(void)x;(void)y;(void)w;(void)h;(void)px; }
static void be_bind_tex(bd_texture t,int u){ (void)t;(void)u; }
static void be_destroy_tex(bd_texture t){ (void)t; }

static void be_scissor(int x,int y,int w,int h){ (void)x;(void)y;(void)w;(void)h; n_scissor++; }
static void be_scissor_off(void){}

static char be_clip[256];
static void be_clip_set(const char *s){ snprintf(be_clip, sizeof be_clip, "%s", s ? s : ""); }
static const char *be_clip_get(void){ return be_clip; }

static int be_ime_on = -1, be_ime_rects;
static void be_ime_enable(int on){ be_ime_on = on; }
static void be_ime_rect(int x,int y,int w,int h){ (void)x;(void)y;(void)w;(void)h; be_ime_rects++; }

static const bd_backend stub = {
	.width=be_width, .height=be_height, .time=be_time, .viewport=be_viewport,
	.clear=be_clear,
	.make_shader=be_make_shader, .destroy_shader=be_destroy_shader,
	.use_shader=be_use_shader, .set_uniform_int=be_uni_i, .set_uniform_float=be_uni_f,
	.set_uniform_vec2=be_uni_2, .set_uniform_vec3=be_uni_3, .set_uniform_vec4=be_uni_4,
	.set_uniform_mat4=be_uni_m, .draw_verts=be_draw_verts,
	.load_texture=be_load_tex, .load_texture_mem=be_load_tex_mem,
	.make_texture=be_make_tex, .update_texture=be_update_tex,
	.bind_texture=be_bind_tex, .destroy_texture=be_destroy_tex,
	.scissor=be_scissor, .scissor_off=be_scissor_off,
	.clipboard_set=be_clip_set, .clipboard_get=be_clip_get,
	.ime_set_enabled=be_ime_enable, .ime_set_cursor_rect=be_ime_rect,
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
static int mw_min_id = -1, mw_restore_id = -1;
static void mw_minimize(int id){ mw_min_id = id; }
static void mw_restore(int id){ mw_restore_id = id; }

static const bd_backend mwstub = {
	.width=be_width, .height=be_height, .time=be_time, .viewport=be_viewport,
	.clear=be_clear,
	.make_shader=be_make_shader, .destroy_shader=be_destroy_shader,
	.use_shader=be_use_shader, .set_uniform_int=be_uni_i, .set_uniform_float=be_uni_f,
	.set_uniform_vec2=be_uni_2, .set_uniform_vec3=be_uni_3, .set_uniform_vec4=be_uni_4,
	.set_uniform_mat4=be_uni_m, .draw_verts=be_draw_verts,
	.load_texture=be_load_tex, .load_texture_mem=be_load_tex_mem,
	.make_texture=be_make_tex, .update_texture=be_update_tex,
	.bind_texture=be_bind_tex, .destroy_texture=be_destroy_tex,
	.scissor=be_scissor, .scissor_off=be_scissor_off,
	.multi_window=1, .window_open=mw_open, .window_close=mw_close,
	.window_begin=mw_begin, .window_swap=mw_swap,
	.window_width=mw_w, .window_height=mw_h, .window_set_title=mw_title,
	.window_minimize=mw_minimize, .window_restore=mw_restore,
};

static int text_committed;
static void on_text_commit(bd_id id, void *a){ (void)id; (void)a; text_committed++; }

static int notice_btn = -2;
static void on_notice(bd_id n, int b, void *a){ (void)n; (void)a; notice_btn = b; }

/* per-window click counters */
static int click_w1, click_w2;
static void on_click_w1(bd_id id, void *a){ (void)id; (void)a; click_w1++; }
static void on_click_w2(bd_id id, void *a){ (void)id; (void)a; click_w2++; }

/* in-surface WM: which floating window's content got a click */
static int wm_click_a, wm_click_b;
static void on_wm_a(bd_id id, void *a){ (void)id; (void)a; wm_click_a++; }
static void on_wm_b(bd_id id, void *a){ (void)id; (void)a; wm_click_b++; }

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
static int exp_act_n; static uint64_t exp_act_key;
static void exp_act(bd_id w, uint64_t key, void *user){ (void)w; (void)user; exp_act_n++; exp_act_key = key; }
static int exp_name_n; static uint64_t exp_name_key; static char exp_name_buf[64];
static void
exp_set_name(void *ctx, uint64_t key, const char *name)
{
	(void)ctx;
	exp_name_n++; exp_name_key = key;
	snprintf(exp_name_buf, sizeof exp_name_buf, "%s", name);
}

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

/* ---- key-event recording extension (key-down/up + repeat) ---- */
static int key_dn, key_up, key_rep;
static int
keyrec_event(bd_id id, void *st, const bd_event *ev)
{
	(void)id; (void)st;
	if (ev->type == BD_EV_KEY_DOWN) { key_dn++; if (ev->repeat) key_rep++; return 1; }
	if (ev->type == BD_EV_KEY_UP)   { key_up++; return 1; }
	if (ev->type == BD_EV_MOUSE_DOWN) return 1;   /* accept focus on click */
	return 0;
}
static const bd_widget_class keyrec_class = {
	.name = "keyrec", .state_size = 0, .event = keyrec_event,
};

/* ---- per-instance pointer recorder (for multitouch routing) ---- */
struct mt_state { int down, move, up; };
static int
mt_event(bd_id id, void *st, const bd_event *ev)
{
	(void)id;
	struct mt_state *m = st;
	if (ev->type == BD_EV_MOUSE_DOWN)      m->down++;
	else if (ev->type == BD_EV_MOUSE_MOVE) m->move++;
	else if (ev->type == BD_EV_MOUSE_UP)   m->up++;
	return 1;
}
static const bd_widget_class mt_class = {
	.name = "mt", .state_size = sizeof(struct mt_state), .event = mt_event,
};

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

	int tfld, ty, tw, tht;
	bd_widget_rect(term, &tfld, &ty, &tw, &tht);
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

	/* native minimize/restore delegate to the backend (OS iconify, option A):
	 * bd_window_minimize/restore call window_minimize/restore with the win id */
	mw_min_id = mw_restore_id = -1;
	bd_window_minimize(f2);
	check("native minimize iconifies the OS window (win 2)", mw_min_id == 2);
	bd_window_restore(f2);
	check("native restore de-iconifies the OS window (win 2)", mw_restore_id == 2);

	/* closing the second frame releases its window and unregisters it */
	bd_destroy(f2);
	check("destroying frame-2 frees window 2", bd_frame_for_window(2) == BD_NONE);
	(void)b2;

	bd_gui_cleanup();

	/* ---- in-surface window manager: gravity, snapping, docking, lock ----
	 * The stub backend is single-surface (multi_window == 0), so the toolkit
	 * runs its own WM: windows[0] is the full-surface desktop and later frames
	 * float, decorated with a title bar the user drags/snaps/docks/locks. */
	bd_gui_init(&stub, NULL);              /* surface is be_width x be_height = 800x500 */

	bd_id desk = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id fa = bd_create(BD_NONE, BD_FRAME,
	    BD_PREF_W_I, 200, BD_PREF_H_I, 150, BD_X_I, 100, BD_Y_I, 100, BD_END);
	bd_create(fa, BD_BUTTON, BD_LABEL_S, "A", BD_GROW_I, 1,
	    BD_ON_CLICK_F, on_wm_a, BD_END);

	bd_gui_layout(800, 500);

	int wrx, wry, wrw, wrh;
	bd_widget_rect(desk, &wrx, &wry, &wrw, &wrh);
	check("desktop root fills the surface",
	    wrx == 0 && wry == 0 && wrw == 800 && wrh == 500);

	int wax, way, waw, wah;
	bd_widget_rect(fa, &wax, &way, &waw, &wah);
	check("floating window laid out at its X/Y and preferred size",
	    wax == 100 && way == 100 && waw == 200 && wah == 150);

	/* drag the title bar: press at (150,110) [title spans y 100..122], move
	 * to (300,210); the window follows by the same delta and stays floating */
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_DOWN, .x = 150, .y = 110,
	    .button = BD_MOUSE_LEFT });
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_MOVE, .x = 300, .y = 210 });
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_UP, .x = 300, .y = 210,
	    .button = BD_MOUSE_LEFT });
	bd_gui_layout(800, 500);
	bd_widget_rect(fa, &wax, &way, &waw, &wah);
	check("title-bar drag moves the window", wax == 250 && way == 200);
	check("dragging away from edges leaves it floating",
	    bd_window_gravity(fa) == BD_GRAVITY_NONE);

	/* drag the window's left edge into the snap zone: it docks LEFT, becoming
	 * a full-height strip at the preferred width */
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_DOWN, .x = 300, .y = 210,
	    .button = BD_MOUSE_LEFT });   /* grab title, offset (50,10) */
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_MOVE, .x = 55, .y = 260 });
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_UP, .x = 55, .y = 260,
	    .button = BD_MOUSE_LEFT });
	bd_gui_layout(800, 500);
	bd_widget_rect(fa, &wax, &way, &waw, &wah);
	check("snapping to the left edge docks the window",
	    bd_window_gravity(fa) == BD_GRAVITY_LEFT);
	check("a LEFT dock is a full-height strip at the preferred width",
	    wax == 0 && way == 0 && waw == 200 && wah == 500);

	/* lock the dock: it can no longer be dragged, but keeps its gravity */
	bd_window_set_locked(fa, 1);
	check("window reports locked", bd_window_locked(fa) == 1);
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_DOWN, .x = 100, .y = 10,
	    .button = BD_MOUSE_LEFT });
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_MOVE, .x = 400, .y = 300 });
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_UP, .x = 400, .y = 300,
	    .button = BD_MOUSE_LEFT });
	bd_gui_layout(800, 500);
	bd_widget_rect(fa, &wax, &way, &waw, &wah);
	check("a locked window ignores title-bar drags",
	    bd_window_gravity(fa) == BD_GRAVITY_LEFT && wax == 0 && waw == 200);

	/* a docked window re-snaps to its edge when the surface resizes */
	bd_gui_layout(1000, 600);
	bd_widget_rect(fa, &wax, &way, &waw, &wah);
	check("docked window re-snaps to its edge on resize",
	    wax == 0 && way == 0 && waw == 200 && wah == 600);

	bd_gui_cleanup();

	/* ---- in-surface WM: raising brings a window to the front ---- */
	wm_click_a = wm_click_b = 0;
	bd_gui_init(&stub, NULL);
	bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END); /* desktop */
	bd_id wa = bd_create(BD_NONE, BD_FRAME,
	    BD_PREF_W_I, 200, BD_PREF_H_I, 150, BD_X_I, 100, BD_Y_I, 100, BD_END);
	bd_create(wa, BD_BUTTON, BD_LABEL_S, "A", BD_GROW_I, 1,
	    BD_ON_CLICK_F, on_wm_a, BD_END);
	bd_id wb = bd_create(BD_NONE, BD_FRAME,
	    BD_PREF_W_I, 200, BD_PREF_H_I, 150, BD_X_I, 150, BD_Y_I, 120, BD_END);
	bd_create(wb, BD_BUTTON, BD_LABEL_S, "B", BD_GROW_I, 1,
	    BD_ON_CLICK_F, on_wm_b, BD_END);
	bd_gui_layout(800, 500);

	/* wb was created last, so it is in front; a click in the overlap (200,200)
	 * reaches wb's content */
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_DOWN, .x = 200, .y = 200,
	    .button = BD_MOUSE_LEFT });
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_UP, .x = 200, .y = 200,
	    .button = BD_MOUSE_LEFT });
	check("front window receives clicks in the overlap",
	    wm_click_b == 1 && wm_click_a == 0);

	/* click wa's exposed title strip (120,110) to raise it above wb */
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_DOWN, .x = 120, .y = 110,
	    .button = BD_MOUSE_LEFT });
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_UP, .x = 120, .y = 110,
	    .button = BD_MOUSE_LEFT });
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_DOWN, .x = 200, .y = 200,
	    .button = BD_MOUSE_LEFT });
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_UP, .x = 200, .y = 200,
	    .button = BD_MOUSE_LEFT });
	check("clicking a window's title raises it to the front",
	    wm_click_a == 1 && wm_click_b == 1);

	/* rendering the WM (desktop + decorated floating windows) does not crash */
	n_drawverts = 0;
	bd_gui_render();
	check("WM render issued GPU draws", n_drawverts > 0);

	bd_gui_cleanup();

	/* ---- in-surface WM: minimize / restore (dock prerequisite) ---- */
	wm_click_a = 0;
	bd_gui_init(&stub, NULL);
	bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END); /* desktop */
	bd_id mwin = bd_create(BD_NONE, BD_FRAME, BD_LABEL_S, "Win",
	    BD_PREF_W_I, 200, BD_PREF_H_I, 150, BD_X_I, 100, BD_Y_I, 100, BD_END);
	bd_create(mwin, BD_BUTTON, BD_LABEL_S, "A", BD_GROW_I, 1,
	    BD_ON_CLICK_F, on_wm_a, BD_END);
	bd_gui_layout(800, 500);

	/* bd_window_list enumerates the two top-level frames */
	bd_id wlist[8];
	int nwin = bd_window_list(wlist, 8);
	check("bd_window_list returns all top-level frames", nwin == 2);

	/* click the title-bar minimize glyph (leftmost of the three), at
	 * min_x = x+w-16-4 -16-2 -16-2 = 100+200-56 = 244, centered */
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_DOWN, .x = 252, .y = 111,
	    .button = BD_MOUSE_LEFT });
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_UP, .x = 252, .y = 111,
	    .button = BD_MOUSE_LEFT });
	check("title-bar minimize button minimizes the window",
	    bd_window_minimized(mwin) == 1);

	/* a minimized window is hidden: a click where its body was reaches the
	 * desktop, not the window's button */
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_DOWN, .x = 150, .y = 200,
	    .button = BD_MOUSE_LEFT });
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_UP, .x = 150, .y = 200,
	    .button = BD_MOUSE_LEFT });
	check("a minimized window is not hit-tested", wm_click_a == 0);

	/* rendering skips the minimized window without crashing */
	bd_gui_layout(800, 500);
	bd_gui_render();
	check("render skips a minimized window", 1);

	bd_window_restore(mwin);
	check("bd_window_restore clears the minimized flag",
	    bd_window_minimized(mwin) == 0);
	bd_gui_layout(800, 500);
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_DOWN, .x = 150, .y = 200,
	    .button = BD_MOUSE_LEFT });
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_UP, .x = 150, .y = 200,
	    .button = BD_MOUSE_LEFT });
	check("a restored window receives clicks again", wm_click_a == 1);
	bd_gui_cleanup();

	/* ---- an extension widget (inventory) hosted as a WM window ----
	 * An inventory placed in a top-level BD_FRAME is a first-class floating
	 * window: the WM decorates it with a title bar and minimize/lock/close, and
	 * routes body input into the inventory. This is how BD_INVENTORY (and any
	 * content widget) becomes "like other windows": drag, minimize, restore. */
	{
	bd_gui_init(&stub, NULL);
	memset(inv_slots, 0, sizeof inv_slots);
	inv_slots[0] = (struct inv_slot){ 200, "Sword",  (bd_texture){1}, 1, 1 };
	inv_slots[1] = (struct inv_slot){ 201, "Shield", (bd_texture){1}, 1, 1 };

	bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END); /* desktop */
	bd_id bagwin = bd_create(BD_NONE, BD_FRAME, BD_LABEL_S, "Bag",
	    BD_LAYOUT_I, BD_LAYOUT_COL, BD_PAD_I, 0,
	    BD_PREF_W_I, 240, BD_PREF_H_I, 220, BD_X_I, 100, BD_Y_I, 100, BD_END);
	bd_id binv = bd_inventory_create(bagwin, 4, 4,
	    &(bd_inventory_model){ .get = inv_get }, NULL, BD_GROW_I, 1, BD_END);
	bd_inventory_set_cell_size(binv, 40);
	bd_gui_layout(800, 500);

	int bx, by, bw, bh; bd_widget_rect(binv, &bx, &by, &bw, &bh);
	check("inventory content lays out below the window title bar",
	    by >= 100 + 22 && bx == 100);

	int s0x = bx + 36, s0y = by + 28;      /* slot-0 / slot-1 centers (cell 56) */
	int s1x = bx + 36 + 56;

	/* a body click routes through the WM into the inventory */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=s0x, .y=s0y });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=s0x, .y=s0y });
	check("a click in the floating window reaches the inventory",
	    bd_inventory_selected(binv) == 0);

	/* drag the title bar (y 100..122) to move the whole window */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=150, .y=110 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_MOVE, .x=300, .y=210 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=300, .y=210 });
	bd_gui_layout(800, 500);
	int nx, ny; bd_widget_rect(bagwin, &nx, &ny, NULL, NULL);
	check("dragging the title bar moves the inventory window",
	    nx == 250 && ny == 200);

	/* minimize it: the window (and its inventory) vanishes from hit-testing */
	bd_window_minimize(bagwin);
	bd_gui_layout(800, 500);
	bd_widget_rect(binv, &bx, &by, &bw, &bh);   /* now over the moved window */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=bx+36+56, .y=by+28 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=bx+36+56, .y=by+28 });
	check("a minimized inventory window is not hit-tested",
	    bd_inventory_selected(binv) == 0);   /* still slot 0, click went nowhere */

	/* restore it and the inventory takes input again */
	bd_window_restore(bagwin);
	bd_gui_layout(800, 500);
	bd_widget_rect(binv, &bx, &by, &bw, &bh);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=bx+36+56, .y=by+28 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=bx+36+56, .y=by+28 });
	check("a restored inventory window takes input again",
	    bd_inventory_selected(binv) == 1);
	(void)s1x;
	bd_gui_cleanup();
	}

	/* ---- BD_DOCK: tiles derived from minimized windows, click restores ---- */
	bd_gui_init(&stub, NULL);
	bd_id dskt = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_FIXED,
	    BD_PAD_I, 0, BD_END);                    /* desktop (FIXED so the dock anchors) */
	bd_id dock = bd_dock_create(dskt, NULL, BD_END);
	bd_dock_set_gravity(dock, BD_GRAVITY_LEFT);  /* vertical strip, top-left */
	bd_id da = bd_create(BD_NONE, BD_FRAME, BD_LABEL_S, "Aa",
	    BD_PREF_W_I, 200, BD_PREF_H_I, 150, BD_X_I, 300, BD_Y_I, 100, BD_END);
	bd_id db = bd_create(BD_NONE, BD_FRAME, BD_LABEL_S, "Bb",
	    BD_PREF_W_I, 200, BD_PREF_H_I, 150, BD_X_I, 350, BD_Y_I, 120, BD_END);

	bd_gui_layout(800, 500);
	check("dock is empty with no minimized windows", bd_dock_count(dock) == 0);

	bd_window_minimize(da);
	bd_window_minimize(db);
	bd_gui_layout(800, 500);   /* reconcile: two tiles */
	check("dock shows one tile per minimized window", bd_dock_count(dock) == 2);

	/* second layout lets the FIXED parent place the now-sized dock at the edge */
	bd_gui_layout(800, 500);
	bd_gui_render();
	check("dock renders its tiles without crashing", 1);

	/* click the first tile (top-left, ~(30,30)) -> restores that window */
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_DOWN, .x = 30, .y = 30,
	    .button = BD_MOUSE_LEFT });
	bd_gui_event(&(bd_event){ .type = BD_EV_MOUSE_UP, .x = 30, .y = 30,
	    .button = BD_MOUSE_LEFT });
	check("clicking a dock tile restores its window",
	    bd_window_minimized(da) == 0);

	bd_gui_layout(800, 500);
	check("the restored window's tile is dropped (derived state)",
	    bd_dock_count(dock) == 1);

	/* closing the other minimized window also drops its tile */
	bd_destroy(db);
	bd_gui_layout(800, 500);
	check("destroying a minimized window drops its tile too",
	    bd_dock_count(dock) == 0);
	bd_gui_cleanup();

	/* ---- layout: cross-axis gravity (BD_ANCHOR_I) in a column ----
	 * The surface is 800x500; a COL frame with no padding gives each child a
	 * 800px-wide cross cell. A non-FILL anchor makes the child take its
	 * preferred width and align within that cell instead of stretching. */
	bd_gui_init(&stub, NULL);
	bd_id lcol = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL,
	    BD_PAD_I, 0, BD_GAP_I, 0, BD_END);
	bd_id c_fill = bd_create(lcol, BD_BUTTON, BD_LABEL_S, "fill",
	    BD_PREF_W_I, 100, BD_PREF_H_I, 30, BD_END);           /* default FILL */
	bd_id c_w = bd_create(lcol, BD_BUTTON, BD_LABEL_S, "w",
	    BD_PREF_W_I, 100, BD_PREF_H_I, 30, BD_ANCHOR_I, BD_ANCHOR_W, BD_END);
	bd_id c_e = bd_create(lcol, BD_BUTTON, BD_LABEL_S, "e",
	    BD_PREF_W_I, 100, BD_PREF_H_I, 30, BD_ANCHOR_I, BD_ANCHOR_E, BD_END);
	bd_id c_c = bd_create(lcol, BD_BUTTON, BD_LABEL_S, "c",
	    BD_PREF_W_I, 100, BD_PREF_H_I, 30, BD_ANCHOR_I, BD_ANCHOR_CENTER, BD_END);
	bd_gui_layout(800, 500);
	int layx, layy, layw, layh;
	bd_widget_rect(c_fill, &layx, &layy, &layw, &layh);
	check("FILL child stretches the full cross axis", layx == 0 && layw == 800);
	bd_widget_rect(c_w, &layx, &layy, &layw, &layh);
	check("W anchor left-aligns at preferred width", layx == 0 && layw == 100);
	bd_widget_rect(c_e, &layx, &layy, &layw, &layh);
	check("E anchor right-aligns at preferred width", layx == 700 && layw == 100);
	bd_widget_rect(c_c, &layx, &layy, &layw, &layh);
	check("CENTER anchor centers at preferred width", layx == 350 && layw == 100);
	bd_gui_cleanup();

	/* ---- layout: main-axis packing (BD_PACK_I) in a row ---- */
	bd_gui_init(&stub, NULL);
	bd_id lrow = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_ROW,
	    BD_PAD_I, 0, BD_GAP_I, 0, BD_PACK_I, BD_PACK_END, BD_END);
	bd_id p1 = bd_create(lrow, BD_BUTTON, BD_LABEL_S, "1", BD_PREF_W_I, 100, BD_END);
	bd_id p2 = bd_create(lrow, BD_BUTTON, BD_LABEL_S, "2", BD_PREF_W_I, 100, BD_END);
	bd_gui_layout(800, 500);   /* 800 wide, 200 used -> 600 leftover */
	bd_widget_rect(p1, &layx, &layy, &layw, &layh);
	int layp2x; bd_widget_rect(p2, &layp2x, &layy, &layw, &layh);
	check("PACK_END pushes the group to the main-axis end",
	    layx == 600 && layp2x == 700);
	bd_set(lrow, BD_PACK_I, BD_PACK_CENTER, BD_END);
	bd_gui_layout(800, 500);
	bd_widget_rect(p1, &layx, &layy, &layw, &layh);
	bd_widget_rect(p2, &layp2x, &layy, &layw, &layh);
	check("PACK_CENTER centers the group", layx == 300 && layp2x == 400);
	bd_set(lrow, BD_PACK_I, BD_PACK_SPACE_BETWEEN, BD_END);
	bd_gui_layout(800, 500);
	bd_widget_rect(p1, &layx, &layy, &layw, &layh);
	bd_widget_rect(p2, &layp2x, &layy, &layw, &layh);
	check("PACK_SPACE_BETWEEN spreads to both ends", layx == 0 && layp2x == 700);
	bd_gui_cleanup();

	/* ---- layout: FIXED edge/corner anchoring (BD_ANCHOR_I + margins) ---- */
	bd_gui_init(&stub, NULL);
	bd_id lfix = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_FIXED,
	    BD_PAD_I, 0, BD_END);
	bd_id a_se = bd_create(lfix, BD_PANEL, BD_PREF_W_I, 120, BD_PREF_H_I, 24,
	    BD_ANCHOR_I, BD_ANCHOR_SE, BD_X_I, 10, BD_Y_I, 8, BD_END);
	bd_id a_nw = bd_create(lfix, BD_PANEL, BD_PREF_W_I, 120, BD_PREF_H_I, 24,
	    BD_ANCHOR_I, BD_ANCHOR_NW, BD_X_I, 5, BD_Y_I, 5, BD_END);
	bd_id a_ctr = bd_create(lfix, BD_PANEL, BD_PREF_W_I, 120, BD_PREF_H_I, 24,
	    BD_ANCHOR_I, BD_ANCHOR_CENTER, BD_END);
	bd_id a_leg = bd_create(lfix, BD_PANEL, BD_PREF_W_I, 120, BD_PREF_H_I, 24,
	    BD_X_I, 12, BD_Y_I, 12, BD_END);   /* default FILL: legacy top-left */
	bd_gui_layout(800, 500);
	bd_widget_rect(a_se, &layx, &layy, &layw, &layh);
	check("SE anchor pins bottom-right with inward margins",
	    layx == 800 - 120 - 10 && layy == 500 - 24 - 8);
	bd_widget_rect(a_nw, &layx, &layy, &layw, &layh);
	check("NW anchor pins top-left with margins", layx == 5 && layy == 5);
	bd_widget_rect(a_ctr, &layx, &layy, &layw, &layh);
	check("CENTER anchor centers in the parent box", layx == 340 && layy == 238);
	bd_widget_rect(a_leg, &layx, &layy, &layw, &layh);
	check("FILL keeps legacy top-left X/Y placement", layx == 12 && layy == 12);
	/* docked window re-tracks on resize: shrink the surface, SE stays pinned */
	bd_gui_layout(600, 400);
	bd_widget_rect(a_se, &layx, &layy, &layw, &layh);
	check("anchored FIXED child tracks the parent on resize",
	    layx == 600 - 120 - 10 && layy == 400 - 24 - 8);
	bd_gui_cleanup();

	/* ---- explorer: drag-move and rubber-band selection ---- */
	bd_gui_init(&stub, NULL);
	bd_explorer_model emodel = {
	    .count = exp_count, .get = exp_get, .set_pos = exp_set_pos,
	    .set_name = exp_set_name };
	bd_explorer_cb ecb = { .moved = exp_moved, .selection_changed = exp_selchg,
	    .activate = exp_act };

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

	/* keyboard navigation: clicking the explorer focuses it for key events */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=i0x, .y=iy });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=i0x, .y=iy });
	uint64_t k = 0;
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_RIGHT });
	bd_explorer_selection(expl, &k, 1);
	check("Right arrow moves selection to the next item",
	    bd_explorer_selection(expl, NULL, 0) == 1 && k == 101);
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_END });
	bd_explorer_selection(expl, &k, 1);
	check("End selects the last item", k == 102);
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_HOME });
	bd_explorer_selection(expl, &k, 1);
	check("Home selects the first item", k == 100);
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_END, .mods=BD_MOD_SHIFT });
	check("Shift+End extends the range to the end",
	    bd_explorer_selection(expl, NULL, 0) == 3);
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_HOME });
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_A, .mods=BD_MOD_CTRL });
	check("Ctrl+A selects all", bd_explorer_selection(expl, NULL, 0) == 3);
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_HOME });
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_ENTER });
	check("Enter activates the cursor item",
	    exp_act_n == 1 && exp_act_key == 100);

	/* in-place rename: begin, type, Enter commits via set_name */
	bd_explorer_begin_rename(expl, 100);   /* item "a" */
	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint='b' });
	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint='c' });
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_ENTER });
	check("rename commits the edited name via set_name",
	    exp_name_n == 1 && exp_name_key == 100 &&
	    strcmp(exp_name_buf, "abc") == 0);

	/* Escape cancels without committing */
	bd_explorer_begin_rename(expl, 101);
	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint='z' });
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_ESCAPE });
	check("Escape cancels rename (no commit)", exp_name_n == 1);

	/* F2 starts a rename of the cursor item */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=i0x, .y=iy });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=i0x, .y=iy });
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_F2 });
	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint='!' });
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_ENTER });
	check("F2 renames the cursor item",
	    exp_name_n == 2 && exp_name_key == 100 &&
	    strcmp(exp_name_buf, "a!") == 0);

	n_scissor = 0;
	bd_gui_render();   /* exercises the band/selection/clip render path */
	check("explorer clips its content with scissor", n_scissor > 0);
	bd_gui_cleanup();

	/* ---- Tab focus traversal across widget kinds ---- */
	bd_gui_init(&stub, NULL);
	bd_id tf = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id ti = bd_create(tf, BD_INPUT_LINE, BD_PREF_H_I, 24, BD_END);
	bd_id ba = bd_create(tf, BD_BUTTON, BD_LABEL_S, "A",
	    BD_ON_CLICK_F, on_click, BD_PREF_H_I, 24, BD_END);
	bd_id bb = bd_create(tf, BD_BUTTON, BD_LABEL_S, "B",
	    BD_ON_CLICK_F, on_click, BD_PREF_H_I, 24, BD_END);
	bd_explorer_model em2 = { .count = exp_count, .get = exp_get };
	bd_id tex = bd_explorer_create(tf, &em2, NULL, BD_GROW_I, 1, BD_END);
	bd_create(tf, BD_LABEL, BD_LABEL_S, "not focusable", BD_PREF_H_I, 18, BD_END);
	bd_gui_layout(800, 600);

	bd_event tab  = { .type=BD_EV_KEY_DOWN, .key=BD_KEY_TAB };
	bd_event stab = { .type=BD_EV_KEY_DOWN, .key=BD_KEY_TAB, .mods=BD_MOD_SHIFT };
	bd_gui_event(&tab); check("Tab focuses the input line", bd_focused() == ti);
	bd_gui_event(&tab); check("Tab -> button A", bd_focused() == ba);
	bd_gui_event(&tab); check("Tab -> button B", bd_focused() == bb);
	bd_gui_event(&tab); check("Tab -> explorer", bd_focused() == tex);
	bd_gui_event(&tab); check("Tab wraps to the input line", bd_focused() == ti);
	bd_gui_event(&stab); check("Shift-Tab wraps back to the explorer", bd_focused() == tex);
	bd_gui_event(&stab); check("Shift-Tab -> button B", bd_focused() == bb);

	int before = clicked;
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_ENTER });
	check("Enter activates the focused button", clicked == before + 1);
	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint=' ' });
	check("Space activates the focused button", clicked == before + 2);
	bd_gui_cleanup();

	/* ---- BD_TEXT single-line field ---- */
	bd_gui_init(&stub, NULL);
	bd_id txf = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id tedit = bd_create(txf, BD_TEXT, BD_PREF_H_I, 24,
	    BD_ON_CLICK_F, on_text_commit, BD_END);
	bd_gui_layout(800, 600);

	int txx, txy, txw, txh;
	bd_widget_rect(tedit, &txx, &txy, &txw, &txh);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=txx+5, .y=txy+5 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=txx+5, .y=txy+5 });
	check("clicking a BD_TEXT focuses it", bd_focused() == tedit);

	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint='h' });
	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint='i' });
	check("typing into BD_TEXT updates its text",
	    strcmp(bd_get_s(tedit, BD_LABEL_S), "hi") == 0);

	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_ENTER });
	check("Enter commits BD_TEXT (fires callback, keeps text)",
	    text_committed == 1 && strcmp(bd_get_s(tedit, BD_LABEL_S), "hi") == 0);

	bd_set(tedit, BD_LABEL_S, "xyz", BD_END);
	check("bd_set BD_LABEL_S replaces BD_TEXT contents",
	    strcmp(bd_get_s(tedit, BD_LABEL_S), "xyz") == 0);

	bd_event tabx = { .type=BD_EV_KEY_DOWN, .key=BD_KEY_TAB };
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=0, .y=0 });
	bd_gui_event(&tabx);
	check("Tab reaches the BD_TEXT field", bd_focused() == tedit);

	/* password mode: masks on screen but keeps the real buffer editable */
	bd_set(tedit, BD_PASSWORD_B, 1, BD_END);
	check("BD_PASSWORD_B reads back", bd_get_i(tedit, BD_PASSWORD_B) == 1);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=txx+5, .y=txy+5 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=txx+5, .y=txy+5 });
	bd_set(tedit, BD_LABEL_S, "", BD_END);
	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint='p' });
	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint='w' });
	check("typing into a masked field still updates the real buffer",
	    strcmp(bd_get_s(tedit, BD_LABEL_S), "pw") == 0);
	bd_gui_render();   /* exercise the masked render path (must not crash) */
	bd_gui_cleanup();

	/* ---- BD_MULTILINE multi-line editor ---- */
	bd_gui_init(&stub, NULL);
	bd_id mlf = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id ml = bd_create(mlf, BD_MULTILINE, BD_GROW_I, 1, BD_END);
	bd_gui_layout(800, 600);

	int mlx, mly, mlw, mlh;
	bd_widget_rect(ml, &mlx, &mly, &mlw, &mlh);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=mlx+5, .y=mly+5 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=mlx+5, .y=mly+5 });
	check("clicking BD_MULTILINE focuses it", bd_focused() == ml);

	/* type "ab", Enter, "cd" -> two lines */
	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint='a' });
	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint='b' });
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_ENTER });
	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint='c' });
	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint='d' });
	check("Enter inserts a newline", strcmp(bd_get_s(ml, BD_LABEL_S), "ab\ncd") == 0);

	/* Up to the first line, then insert -> lands on line 0 */
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_UP });
	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint='X' });
	check("Up moves the caret to the previous line",
	    strcmp(bd_get_s(ml, BD_LABEL_S), "abX\ncd") == 0);

	/* Home to line start, then insert */
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_HOME });
	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint='Y' });
	check("Home goes to the line start",
	    strcmp(bd_get_s(ml, BD_LABEL_S), "YabX\ncd") == 0);

	/* Down then End reach the end of the line below */
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_DOWN });
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_END });
	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint='Z' });
	check("Down + End reach the end of the next line",
	    strcmp(bd_get_s(ml, BD_LABEL_S), "YabX\ncdZ") == 0);

	/* Backspace at a line start joins with the previous line */
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_HOME });
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_BACKSPACE });
	check("Backspace at line start joins lines",
	    strcmp(bd_get_s(ml, BD_LABEL_S), "YabXcdZ") == 0);

	bd_gui_render();   /* exercises the multiline render/scissor path */
	bd_gui_cleanup();

	/* ---- rich-text editor widget ---- */
	bd_gui_init(&stub, NULL);
	bd_id edf = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id ed = bd_editor_create(edf, BD_GROW_I, 1, BD_END);
	bd_gui_layout(800, 600);

	bd_editor_set_text(ed, "X:1\nK:C\nCDEF");
	char rb[64];
	check("editor row_count", bd_editor_row_count(ed) == 3);
	bd_editor_row_text(ed, 1, rb, sizeof rb);
	check("editor row_text reads a row", strcmp(rb, "K:C") == 0);

	bd_editor_replace_row(ed, 1, "K:G");
	bd_editor_row_text(ed, 1, rb, sizeof rb);
	check("editor replace_row", strcmp(rb, "K:G") == 0);

	bd_editor_insert_row(ed, 1, "T:Song");
	check("editor insert_row grows the row count", bd_editor_row_count(ed) == 4);
	bd_editor_row_text(ed, 1, rb, sizeof rb);
	check("editor insert_row places the new row", strcmp(rb, "T:Song") == 0);

	bd_editor_delete_row(ed, 0);
	bd_editor_row_text(ed, 0, rb, sizeof rb);
	check("editor delete_row", bd_editor_row_count(ed) == 3 &&
	    strcmp(rb, "T:Song") == 0);

	bd_editor_text(ed, rb, sizeof rb);
	check("editor text reads the whole buffer",
	    strcmp(rb, "T:Song\nK:G\nCDEF") == 0);

	/* focus by click, then locked typing is rejected */
	int edx, edy, edw, edh;
	bd_widget_rect(ed, &edx, &edy, &edw, &edh);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=edx+2, .y=edy+2 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=edx+2, .y=edy+2 });
	check("clicking the editor focuses it", bd_focused() == ed);

	bd_editor_set_locked(ed, 1);
	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint='Z' });
	bd_editor_text(ed, rb, sizeof rb);
	check("locked editor rejects typing",
	    strcmp(rb, "T:Song\nK:G\nCDEF") == 0);

	bd_editor_set_locked(ed, 0);
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_HOME });
	bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint='Z' });
	bd_editor_text(ed, rb, sizeof rb);
	check("unlocked editor accepts typing",
	    strcmp(rb, "ZT:Song\nK:G\nCDEF") == 0);

	/* styling: a highlight run renders without disturbing the text */
	bd_editor_highlight_row(ed, 2,
	    (bd_rich_style){ BD_RT_BOLD | BD_RT_UNDERLINE, 0xFF8800FFu, 0x103040FFu });
	n_drawverts = 0;
	bd_gui_render();
	bd_editor_text(ed, rb, sizeof rb);
	check("styled render leaves text intact + draws",
	    n_drawverts > 0 && strcmp(rb, "ZT:Song\nK:G\nCDEF") == 0);
	bd_gui_cleanup();

	/* ---- BD_LIST scrolling/selectable list ---- */
	bd_gui_init(&stub, NULL);
	bd_id lif = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id lst = bd_create(lif, BD_LIST, BD_GROW_I, 1,
	    BD_LABEL_S, "alpha\nbeta\ngamma\ndelta",
	    BD_ON_CLICK_F, on_click, BD_END);
	bd_gui_layout(800, 600);
	check("list_count counts items", bd_list_count(lst) == 4);

	int lx, ly, lw, lh_, lh = (int)bd_draw_line_height();
	bd_widget_rect(lst, &lx, &ly, &lw, &lh_);
	int rowy = ly + 2 + 1 * lh + lh / 2;     /* row 1 (pad 2) */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=lx+5, .y=rowy });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=lx+5, .y=rowy });
	check("clicking selects a row + focuses",
	    bd_list_selected(lst) == 1 && bd_focused() == lst);

	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_DOWN });
	check("Down moves selection", bd_list_selected(lst) == 2);
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_END });
	check("End selects the last row", bd_list_selected(lst) == 3);
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_HOME });
	check("Home selects the first row", bd_list_selected(lst) == 0);

	int lc = clicked;
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_ENTER });
	check("Enter activates the selection", clicked == lc + 1);

	bd_list_select(lst, 2);
	check("bd_list_select sets the row", bd_list_selected(lst) == 2);
	bd_gui_render();   /* exercises the list render/scissor path */
	bd_gui_cleanup();

	/* ---- BD_TAB_BAR folder tabs ---- */
	bd_gui_init(&stub, NULL);
	bd_id tbf = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id tb = bd_create(tbf, BD_TAB_BAR, BD_PREF_H_I, 28,
	    BD_LABEL_S, "Aardwolf\nBatMUD\nDiscworld",
	    BD_ON_CLICK_F, on_click, BD_END);
	bd_gui_layout(800, 600);
	check("tabbar_count counts tabs", bd_tabbar_count(tb) == 3);
	check("first tab active by default", bd_tabbar_active(tb) == 0);

	int tbx, tby, tbw, tbh;
	bd_widget_rect(tb, &tbx, &tby, &tbw, &tbh);
	int tab0w = (int)bd_draw_text_width("Aardwolf") + 44; /* text + 2*pad+2*slant */
	int tcx = tbx + tab0w + 10, tcy = tby + tbh / 2;     /* inside tab 1 */
	int tc = clicked;
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=tcx, .y=tcy });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=tcx, .y=tcy });
	check("clicking a tab activates it + fires",
	    bd_tabbar_active(tb) == 1 && clicked == tc + 1);

	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_RIGHT });
	check("Right moves to the next tab", bd_tabbar_active(tb) == 2);
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_LEFT });
	check("Left moves to the previous tab", bd_tabbar_active(tb) == 1);

	bd_tabbar_set_active(tb, 0);
	check("bd_tabbar_set_active sets the tab", bd_tabbar_active(tb) == 0);
	bd_gui_render();   /* exercises the folder-tab render path */
	bd_gui_cleanup();

	/* ---- BD_SCROLLBAR ---- */
	bd_gui_init(&stub, NULL);
	/* a ROW frame keeps the bar narrow (pref_w) and tall -> vertical */
	bd_id sbf = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_ROW, BD_END);
	bd_id sb = bd_create(sbf, BD_SCROLLBAR, BD_PREF_W_I, 14,
	    BD_ON_CLICK_F, on_click, BD_END);
	bd_gui_layout(800, 600);
	bd_scrollbar_set(sb, 0.0f, 0.25f);
	check("scrollbar starts at 0", bd_scrollbar_value(sb) == 0.0f);

	int sbx, sby, sbw, sbh;
	bd_widget_rect(sb, &sbx, &sby, &sbw, &sbh);
	check("scrollbar is vertical (tall + narrow)", sbh > sbw);

	int sc = clicked;
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=sbx+sbw/2, .y=sby+sbh-1 });
	check("clicking the bottom scrolls toward the end",
	    bd_scrollbar_value(sb) > 0.9f && clicked == sc + 1);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP, .button=BD_MOUSE_LEFT, .x=sbx+sbw/2, .y=sby+sbh-1 });

	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=sbx+sbw/2, .y=sby });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_MOVE, .x=sbx+sbw/2, .y=sby });
	check("dragging to the top scrolls to 0", bd_scrollbar_value(sb) == 0.0f);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP, .button=BD_MOUSE_LEFT, .x=sbx+sbw/2, .y=sby });

	bd_scrollbar_set(sb, 0.5f, 0.25f);
	check("bd_scrollbar_set sets the value", bd_scrollbar_value(sb) == 0.5f);
	bd_gui_render();   /* exercises the scrollbar render path */
	bd_gui_cleanup();

	/* ---- BD_NOTICE modal dialog ---- */
	bd_gui_init(&stub, NULL);
	bd_id nrf = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id gobtn = bd_create(nrf, BD_BUTTON, BD_LABEL_S, "Go", BD_PREF_H_I, 28,
	    BD_ON_CLICK_F, on_click, BD_END);
	bd_gui_layout(800, 600);

	notice_btn = -2;
	bd_id ntc = bd_notice_open("Disconnect from the server?", "Yes\nNo",
	    on_notice, NULL);
	check("notice opens", ntc != BD_NONE);

	/* modality: a click on the underlying button is swallowed */
	int gbx, gby, gbw, gbh;
	bd_widget_rect(gobtn, &gbx, &gby, &gbw, &gbh);
	int gc = clicked;
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=gbx+gbw/2, .y=gby+gbh/2 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=gbx+gbw/2, .y=gby+gbh/2 });
	check("modal notice blocks the UI behind it", clicked == gc);

	/* render lays out the notice buttons; then click "Yes" (index 0) */
	bd_gui_render();
	bd_id yes = bd_first_child(ntc);
	int yx, yy, yw, yh;
	bd_widget_rect(yes, &yx, &yy, &yw, &yh);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=yx+yw/2, .y=yy+yh/2 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=yx+yw/2, .y=yy+yh/2 });
	check("notice button fires the callback with its index", notice_btn == 0);

	/* notice closed -> the UI is interactive again */
	int gc2 = clicked;
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=gbx+gbw/2, .y=gby+gbh/2 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=gbx+gbw/2, .y=gby+gbh/2 });
	check("UI is interactive after the notice closes", clicked == gc2 + 1);

	/* Escape cancels a notice with index -1 */
	notice_btn = -2;
	bd_notice_open("Quit?", "Quit\nCancel", on_notice, NULL);
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_ESCAPE });
	check("Escape cancels the notice (-1)", notice_btn == -1);
	bd_gui_cleanup();

	/* ---- clipboard: copy / paste / cut in a text field ---- */
	bd_gui_init(&stub, NULL);
	bd_id cbf = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id ctf = bd_create(cbf, BD_TEXT, BD_PREF_H_I, 24, BD_END);
	bd_gui_layout(800, 600);
	int cbx, cby, cbw, cbh;
	bd_widget_rect(ctf, &cbx, &cby, &cbw, &cbh);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=cbx+5, .y=cby+5 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=cbx+5, .y=cby+5 });
	const char *hello = "hello";
	for (const char *p = hello; *p; p++)
		bd_gui_event(&(bd_event){ .type=BD_EV_CHAR, .codepoint=(unsigned)*p });
	check("typed into the field", strcmp(bd_get_s(ctf, BD_LABEL_S), "hello") == 0);

	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_A, .mods=BD_MOD_CTRL });
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key='C', .mods=BD_MOD_CTRL });
	check("Ctrl-C copies the selection to the clipboard",
	    strcmp(be_clip, "hello") == 0);

	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_END });
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key='V', .mods=BD_MOD_CTRL });
	check("Ctrl-V pastes at the cursor",
	    strcmp(bd_get_s(ctf, BD_LABEL_S), "hellohello") == 0);

	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_A, .mods=BD_MOD_CTRL });
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key='X', .mods=BD_MOD_CTRL });
	check("Ctrl-X cuts to the clipboard",
	    strcmp(bd_get_s(ctf, BD_LABEL_S), "") == 0 &&
	    strcmp(be_clip, "hellohello") == 0);

	/* paste keeps newlines in a multi-line field */
	be_clip_set("a\nb");
	bd_id cml = bd_create(cbf, BD_MULTILINE, BD_GROW_I, 1, BD_END);
	bd_gui_layout(800, 600);
	int mlx2, mly2, mlw2, mlh2;
	bd_widget_rect(cml, &mlx2, &mly2, &mlw2, &mlh2);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=mlx2+3, .y=mly2+3 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=mlx2+3, .y=mly2+3 });
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key='V', .mods=BD_MOD_CTRL });
	check("paste preserves newlines in BD_MULTILINE",
	    strcmp(bd_get_s(cml, BD_LABEL_S), "a\nb") == 0);
	bd_gui_cleanup();

	/* ---- key-up + repeat delivered to a focused extension ---- */
	bd_gui_init(&stub, NULL);
	int krt = bd_register_widget_class(&keyrec_class);
	bd_id krf = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id krw = bd_create(krf, krt, BD_GROW_I, 1, BD_END);
	bd_gui_layout(800, 600);
	int krx, kry, krw_, krh;
	bd_widget_rect(krw, &krx, &kry, &krw_, &krh);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=krx+5, .y=kry+5 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=krx+5, .y=kry+5 });
	check("clicking focuses the extension", bd_focused() == krw);

	key_dn = key_up = key_rep = 0;
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_A });
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_A, .repeat=1 });
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_UP,   .key=BD_KEY_A });
	check("extension receives key-down, repeat, and key-up",
	    key_dn == 2 && key_rep == 1 && key_up == 1);
	bd_gui_cleanup();

	/* ---- IME: commit / preedit / enable-on-focus ---- */
	bd_gui_init(&stub, NULL);
	bd_id imf = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id itx = bd_create(imf, BD_TEXT, BD_PREF_H_I, 24, BD_END);
	bd_gui_layout(800, 600);
	int imx, imy, imw, imh;
	bd_widget_rect(itx, &imx, &imy, &imw, &imh);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=imx+5, .y=imy+5 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=imx+5, .y=imy+5 });

	be_ime_on = -1;
	bd_gui_render();
	check("IME enabled while a text field is focused", be_ime_on == 1);

	bd_gui_event(&(bd_event){ .type=BD_EV_TEXT_COMMIT, .text="ni" });
	bd_gui_event(&(bd_event){ .type=BD_EV_TEXT_COMMIT, .text="hao" });
	check("TEXT_COMMIT inserts committed text",
	    strcmp(bd_get_s(itx, BD_LABEL_S), "nihao") == 0);

	bd_gui_event(&(bd_event){ .type=BD_EV_TEXT_PREEDIT, .text="X", .caret=1 });
	check("preedit does not change the buffer",
	    strcmp(bd_get_s(itx, BD_LABEL_S), "nihao") == 0);

	/* a multi-byte commit (CJK) replaces the preedit and inserts */
	bd_gui_event(&(bd_event){ .type=BD_EV_TEXT_COMMIT, .text="\xe6\x97\xa5" });
	check("multi-byte TEXT_COMMIT inserts + clears preedit",
	    strcmp(bd_get_s(itx, BD_LABEL_S), "nihao\xe6\x97\xa5") == 0);

	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_ESCAPE });
	be_ime_on = -1;
	bd_gui_render();
	check("IME disabled when focus leaves the field", be_ime_on == 0);
	bd_gui_cleanup();

	/* ---- multitouch: two fingers drive two widgets at once ---- */
	bd_gui_init(&stub, NULL);
	int mtt = bd_register_widget_class(&mt_class);
	bd_id mtf = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_ROW, BD_END);
	bd_id ma = bd_create(mtf, mtt, BD_GROW_I, 1, BD_END);
	bd_id mb = bd_create(mtf, mtt, BD_GROW_I, 1, BD_END);
	bd_gui_layout(800, 600);
	int ax, ay, aw, ah, bx2, by2, bw2, bh2;
	bd_widget_rect(ma, &ax, &ay, &aw, &ah);
	bd_widget_rect(mb, &bx2, &by2, &bw2, &bh2);
	int acx = ax + aw/2, acy = ay + ah/2, bcx = bx2 + bw2/2, bcy = by2 + bh2/2;

	/* finger 1 on A and finger 2 on B, interleaved */
	bd_gui_event(&(bd_event){ .type=BD_EV_TOUCH_DOWN, .touch=1, .x=acx, .y=acy });
	bd_gui_event(&(bd_event){ .type=BD_EV_TOUCH_DOWN, .touch=2, .x=bcx, .y=bcy });
	bd_gui_event(&(bd_event){ .type=BD_EV_TOUCH_MOVE, .touch=1, .x=acx, .y=acy-10 });
	bd_gui_event(&(bd_event){ .type=BD_EV_TOUCH_MOVE, .touch=2, .x=bcx, .y=bcy+10 });
	bd_gui_event(&(bd_event){ .type=BD_EV_TOUCH_UP,   .touch=1, .x=acx, .y=acy-10 });
	bd_gui_event(&(bd_event){ .type=BD_EV_TOUCH_UP,   .touch=2, .x=bcx, .y=bcy+10 });

	struct mt_state *sa = bd_widget_state(ma), *smb = bd_widget_state(mb);
	check("each finger drives its own widget (down/move/up)",
	    sa->down == 1 && sa->move == 1 && sa->up == 1 &&
	    smb->down == 1 && smb->move == 1 && smb->up == 1);
	bd_gui_cleanup();

	/* ---- pen / drawing canvas: pressure strokes + eraser ---- */
	bd_gui_init(&stub, NULL);
	bd_id pf = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id cv = bd_canvas_create(pf, BD_GROW_I, 1, BD_END);
	bd_gui_layout(800, 600);
	int cvx, cvy, cvw, cvh;
	bd_widget_rect(cv, &cvx, &cvy, &cvw, &cvh);
	int pcx = cvx + cvw/2, pcy = cvy + cvh/2;

	/* hovering in proximity must not lay down ink */
	bd_gui_event(&(bd_event){ .type=BD_EV_PEN_HOVER, .x=pcx, .y=pcy,
	    .pen_flags=BD_PEN_INRANGE });
	check("pen hover draws no stroke", bd_canvas_stroke_count(cv) == 0);

	/* a tip-down / move / up makes one stroke; the move past the widget edge
	 * still lands because contact captures the canvas */
	bd_gui_event(&(bd_event){ .type=BD_EV_PEN_DOWN, .x=pcx, .y=pcy,
	    .pressure=0.8f, .pen_flags=BD_PEN_INRANGE });
	bd_gui_event(&(bd_event){ .type=BD_EV_PEN_MOVE, .x=pcx+20, .y=pcy+5,
	    .pressure=0.9f, .pen_flags=BD_PEN_INRANGE });
	bd_gui_event(&(bd_event){ .type=BD_EV_PEN_MOVE, .x=cvx+cvw+50, .y=pcy,
	    .pressure=0.5f, .pen_flags=BD_PEN_INRANGE });
	bd_gui_event(&(bd_event){ .type=BD_EV_PEN_UP, .x=cvx+cvw+50, .y=pcy,
	    .pen_flags=BD_PEN_INRANGE });
	check("pen stroke committed on tip up", bd_canvas_stroke_count(cv) == 1);

	/* a second stroke */
	bd_gui_event(&(bd_event){ .type=BD_EV_PEN_DOWN, .x=pcx-30, .y=pcy-30,
	    .pressure=1.0f, .pen_flags=BD_PEN_INRANGE });
	bd_gui_event(&(bd_event){ .type=BD_EV_PEN_UP, .x=pcx-30, .y=pcy-30,
	    .pen_flags=BD_PEN_INRANGE });
	check("second pen stroke committed", bd_canvas_stroke_count(cv) == 2);

	/* the eraser end removes a stroke it passes over (no new stroke added) */
	bd_gui_event(&(bd_event){ .type=BD_EV_PEN_DOWN, .x=pcx, .y=pcy,
	    .pressure=1.0f, .pen_flags=BD_PEN_INRANGE|BD_PEN_ERASER });
	bd_gui_event(&(bd_event){ .type=BD_EV_PEN_UP, .x=pcx, .y=pcy,
	    .pen_flags=BD_PEN_INRANGE|BD_PEN_ERASER });
	check("eraser removes the stroke under it", bd_canvas_stroke_count(cv) == 1);

	bd_canvas_clear(cv);
	check("canvas clear empties strokes", bd_canvas_stroke_count(cv) == 0);
	bd_gui_cleanup();

	/* ---- multi-column table ---- */
	{
	bd_gui_init(&stub, NULL);
	bd_id tfr = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_table_column tcols[] = {
		{ "Name", 0,  BD_TABLE_LEFT,  0 },
		{ "Port", 70, BD_TABLE_RIGHT, BD_TABLE_COL_NUMERIC },
	};
	bd_table_model tmodel = { tbl_rows, tbl_cell, NULL };
	bd_table_cb tcb = { tbl_on_activate, NULL, tbl_on_sel, NULL };
	bd_id tbl = bd_table_create(tfr, tcols, 2, &tmodel, &tcb, BD_GROW_I, 1, BD_END);
	bd_gui_layout(800, 600);

	int trh = (int)bd_draw_line_height() + 6;    /* must match ROW_EXTRA */
	int tbx, tby, tbw, tbh;
	bd_widget_rect(tbl, &tbx, &tby, &tbw, &tbh);
	/* y of a visual row's center; header occupies one row height */
#define rowy(v) (tby + trh + (v) * trh + trh / 2)

	n_scissor = 0;
	bd_gui_render();
	check("table clips its body with scissor", n_scissor > 0);

	/* click row 1 (model index 1, no sort yet) */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=tbx+20, .y=rowy(1) });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=tbx+20, .y=rowy(1) });
	check("clicking a row selects it (model index, unsorted)", bd_table_current(tbl) == 1);
	check("selection_changed fired", tbl_sel_changed > 0);
	int selrows[4];
	check("one row selected", bd_table_selection(tbl, selrows, 4) == 1);

	/* keyboard: Down moves the cursor */
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_DOWN });
	check("Down moves the cursor to the next row", bd_table_current(tbl) == 2);

	/* Shift+Up extends the selection */
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_UP, .mods=BD_MOD_SHIFT });
	check("Shift+Up extends to a 2-row selection", bd_table_selection(tbl, selrows, 4) == 2);

	/* sort by Name ascending: header click on column 0. Data order is
	   Discworld(0), Aardwolf(1), Achaea(2) -> ascending visual: 1,2,0 */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=tbx+20, .y=tby+trh/2 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=tbx+20, .y=tby+trh/2 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=tbx+20, .y=rowy(0) });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=tbx+20, .y=rowy(0) });
	check("sort by Name asc puts Aardwolf (model 1) first", bd_table_current(tbl) == 1);

	/* numeric sort by Port asc: ports 4242(0),23(1),100(2) -> visual 1,2,0.
	   A string sort would order "100","23","4242" -> model 2 first. */
	int portx = tbx + tbw - 30;
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=portx, .y=tby+trh/2 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=portx, .y=tby+trh/2 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=tbx+20, .y=rowy(0) });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=tbx+20, .y=rowy(0) });
	check("numeric sort by Port asc puts port 23 (model 1) first", bd_table_current(tbl) == 1);

	/* Enter activates the cursor row */
	tbl_activated_row = -1;
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_ENTER });
	check("Enter activates the cursor row", tbl_activated_row == 1);

	/* API selection + render with a scrollbar path doesn't crash */
	bd_table_select(tbl, 0, 0);
	check("bd_table_select replaces selection", bd_table_current(tbl) == 1 &&
	    bd_table_selection(tbl, selrows, 4) == 1 && selrows[0] == 0);
	bd_gui_render();
	bd_gui_cleanup();
#undef rowy
	}

	/* ---- generic modal dialog ---- */
	{
	bd_gui_init(&stub, NULL);
	bd_id mfr = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id mbtn = bd_create(mfr, BD_BUTTON, BD_LABEL_S, "behind",
	    BD_ON_CLICK_F, on_click, BD_PREF_H_I, 24, BD_END);
	/* a detached dialog: a panel with a button, sized 200x100 */
	bd_id dlg = bd_create(BD_NONE, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_COL,
	    BD_PREF_W_I, 200, BD_PREF_H_I, 100, BD_PAD_I, 8, BD_END);
	bd_id dbtn = bd_create(dlg, BD_BUTTON, BD_LABEL_S, "ok",
	    BD_ON_CLICK_F, on_click, BD_GROW_I, 1, BD_END);
	bd_gui_layout(800, 600);

	check("no modal active initially", bd_modal_active() == BD_NONE);
	bd_modal_open(dlg);
	check("bd_modal_open sets the active modal", bd_modal_active() == dlg);

	/* the dialog is centered at its preferred size after layout */
	bd_gui_layout(800, 600);
	int dx, dy, dw, dh;
	bd_widget_rect(dlg, &dx, &dy, &dw, &dh);
	check("modal is centered at its preferred size",
	    dw == 200 && dh == 100 && dx == (800 - 200) / 2 && dy == (600 - 100) / 2);

	n_scissor = 0;   /* unused here; just exercise render */
	bd_gui_render();
	check("rendering with a modal open does not crash", 1);

	/* a click on the button BEHIND the modal must not reach it */
	int bx, by, bw, bh;
	bd_widget_rect(mbtn, &bx, &by, &bw, &bh);
	int before = clicked;
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=bx+4, .y=by+4 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=bx+4, .y=by+4 });
	check("input to the UI behind the modal is blocked", clicked == before);

	/* a click on the dialog's own button works */
	bd_widget_rect(dbtn, &bx, &by, &bw, &bh);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=bx+4, .y=by+4 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=bx+4, .y=by+4 });
	check("the modal's own widgets receive input", clicked == before + 1);

	/* Escape closes the modal and is swallowed (returns 1) */
	int esc = bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_ESCAPE });
	check("Escape closes the modal and is consumed",
	    bd_modal_active() == BD_NONE && esc == 1);

	/* after close, the UI behind is live again */
	bd_widget_rect(mbtn, &bx, &by, &bw, &bh);
	before = clicked;
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=bx+4, .y=by+4 });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=bx+4, .y=by+4 });
	check("closing the modal re-enables the UI behind", clicked == before + 1);

	bd_gui_cleanup();
	}

	/* ---- inventory grid: select / activate / drag-move / context / hover / keys ---- */
	{
	bd_gui_init(&stub, NULL);
	memset(inv_slots, 0, sizeof inv_slots);
	inv_slots[0] = (struct inv_slot){ 100, "Sword",  (bd_texture){1}, 1, 1 };
	inv_slots[1] = (struct inv_slot){ 101, "Shield", (bd_texture){1}, 1, 1 };
	inv_slots[2] = (struct inv_slot){ 102, "Potion", (bd_texture){1}, 5, 1 };
	inv_act = inv_ctx = -1; inv_hov = -2; inv_mfrom = inv_mto = -1;

	bd_inventory_model im = { .get = inv_get };
	bd_inventory_cb icb = { .activate = inv_on_act, .move = inv_on_move,
	    .context = inv_on_ctx, .hover = inv_on_hover };
	bd_id iframe = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	bd_id inv = bd_inventory_create(iframe, 4, 20, &im, &icb, BD_GROW_I, 1, BD_END);
	bd_inventory_set_cell_size(inv, 48);   /* cell 64 wide: row 0 at content x 8,72,136 */
	bd_gui_layout(800, 600);

	int ix, iy, iw, ih;
	bd_widget_rect(inv, &ix, &iy, &iw, &ih);
	check("inventory got geometry", iw > 200 && ih > 200);

	int s0 = ix + 40, s1 = ix + 104, s2 = ix + 168, ry = iy + 20;  /* row-0 centers */

	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=s0, .y=ry });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=s0, .y=ry });
	check("click selects slot 0", bd_inventory_selected(inv) == 0);

	/* Enter activates the focused slot (the stub clock defeats double-click) */
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_ENTER });
	check("Enter activates the focused slot 0", inv_act == 0);

	/* click slot 1 (anchor), Shift-click slot 2 -> range {1,2} */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=s1, .y=ry });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=s1, .y=ry });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .mods=BD_MOD_SHIFT, .x=s2, .y=ry });
	check("Shift-range selects two slots", bd_inventory_selection(inv, NULL, 0) == 2);
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP, .button=BD_MOUSE_LEFT, .mods=BD_MOD_SHIFT, .x=s2, .y=ry });

	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_RIGHT, .x=s0, .y=ry });
	check("right-click fires context on slot 0", inv_ctx == 0);

	/* hover exercises the new wants_hover move delivery */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_MOVE, .x=s1, .y=ry });
	check("hover reports slot 1", inv_hov == 1);

	/* drag slot 0 onto slot 1 -> move(0,1) */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=s0, .y=ry });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_MOVE, .x=s0+30, .y=ry });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_MOVE, .x=s1, .y=ry });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=s1, .y=ry });
	check("drag-drop fires move(0,1)", inv_mfrom == 0 && inv_mto == 1);

	/* keyboard: focus, Right then Down (cols=4) -> slot 5 */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=s0, .y=ry });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP,   .button=BD_MOUSE_LEFT, .x=s0, .y=ry });
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_RIGHT });
	check("Right moves selection to slot 1", bd_inventory_selected(inv) == 1);
	bd_gui_event(&(bd_event){ .type=BD_EV_KEY_DOWN, .key=BD_KEY_DOWN });
	check("Down moves one grid row to slot 5", bd_inventory_selected(inv) == 5);

	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_SCROLL, .scroll_dy=-3 });
	bd_gui_render();
	check("inventory scroll + render does not crash", 1);

	bd_gui_cleanup();
	}

	/* ---- action bar: cross-widget drop / click-activate / reorder / hotkey ---- */
	{
	bd_gui_init(&stub, NULL);
	memset(inv_slots, 0, sizeof inv_slots);
	inv_slots[0] = (struct inv_slot){ 100, "Sword", (bd_texture){1}, 1, 1 };
	ab_act_slot = ab_drop_slot = ab_move_from = ab_move_to = -1;

	bd_inventory_model im = { .get = inv_get };
	bd_id fr = bd_create(BD_NONE, BD_FRAME, BD_LAYOUT_I, BD_LAYOUT_ROW,
	    BD_PAD_I, 10, BD_GAP_I, 20, BD_END);
	bd_id inv = bd_inventory_create(fr, 4, 5, &im, NULL, BD_END);
	bd_inventory_set_cell_size(inv, 48);
	bd_actionbar_cb abcb = { .activate = ab_on_act, .drop = ab_on_drop,
	    .move = ab_on_move };
	bd_id bar = bd_actionbar_create(fr, 4, &abcb, BD_END);
	bd_actionbar_set_tile_size(bar, 48);
	bd_actionbar_set_hotkey(bar, 0, 'A', BD_MOD_CTRL);
	bd_gui_layout(800, 600);

	int abx, aby, abw, abh;
	bd_widget_rect(bar, &abx, &aby, &abw, &abh);
	check("action bar got geometry", abw > 100 && abh > 40);
	check("action bar slot 0 starts empty",
	    !bd_actionbar_get_slot(bar, 0, NULL));

	int ix, iy, iw, ih; bd_widget_rect(inv, &ix, &iy, &iw, &ih);
	int inv0x = ix + 40, inv0y = iy + 20;              /* inventory slot-0 center */
	int b0x = abx + 40, b0y = aby + 28;               /* bar slot-0 center */
	int b2x = abx + 40 + 2 * 56;                      /* bar slot-2 center x */

	/* drag Sword out of the inventory and drop it on action-bar slot 0 */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=inv0x, .y=inv0y });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_MOVE, .x=inv0x+30, .y=inv0y }); /* start drag */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_MOVE, .x=b0x, .y=b0y });        /* over the bar */
	bd_gui_render();   /* draws the drag ghost; must not crash */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP, .button=BD_MOUSE_LEFT, .x=b0x, .y=b0y });
	bd_action got;
	check("drop fires drop(slot 0, Sword)", ab_drop_slot == 0 && ab_drop_key == 100);
	check("dropped item binds into action-bar slot 0",
	    bd_actionbar_get_slot(bar, 0, &got) && got.key == 100);
	check("the hotkey binding survives the drop",
	    bd_actionbar_get_hotkey(bar, 0, NULL, NULL));

	/* a plain click on the filled slot fires activate */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=b0x, .y=b0y });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP, .button=BD_MOUSE_LEFT, .x=b0x, .y=b0y });
	check("click activates action-bar slot 0", ab_act_slot == 0 && ab_act_key == 100);

	/* the Ctrl-A hotkey activates slot 0 too; a wrong modifier does not */
	ab_act_slot = -1;
	check("Ctrl-A hotkey activates slot 0",
	    bd_actionbar_key(bar, 'A', BD_MOD_CTRL) && ab_act_slot == 0);
	ab_act_slot = -1;
	check("plain A (no Ctrl) does not match the binding",
	    !bd_actionbar_key(bar, 'A', 0) && ab_act_slot == -1);

	/* drag slot 0 onto slot 2 within the bar -> reorder */
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_DOWN, .button=BD_MOUSE_LEFT, .x=b0x, .y=b0y });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_MOVE, .x=b0x+30, .y=b0y });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_MOVE, .x=b2x, .y=b0y });
	bd_gui_event(&(bd_event){ .type=BD_EV_MOUSE_UP, .button=BD_MOUSE_LEFT, .x=b2x, .y=b0y });
	check("internal reorder fires move(0,2)", ab_move_from == 0 && ab_move_to == 2);
	check("Sword moved to slot 2",
	    bd_actionbar_get_slot(bar, 2, &got) && got.key == 100);
	check("slot 0 is empty after the reorder",
	    !bd_actionbar_get_slot(bar, 0, NULL));

	bd_gui_cleanup();
	}

	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
