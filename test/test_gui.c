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

/* ---- recording stub backend ---- */
static int n_fill, n_stroke, n_tinted, n_vfont;

static int    be_width(void)  { return 800; }
static int    be_height(void) { return 500; }
static double be_time(void)   { return 0.0; }
static void   be_viewport(int x,int y,int w,int h){(void)x;(void)y;(void)w;(void)h;}
static void   be_clear(float r,float g,float b,float a){(void)r;(void)g;(void)b;(void)a;}
static void   be_sprite_begin(float x,float y,float w,float h){(void)x;(void)y;(void)w;(void)h;}
static void   be_sprite_end(void){}
static void   be_fill(float x,float y,float w,float h,float r,float g,float b,float a)
{ (void)x;(void)y;(void)w;(void)h;(void)r;(void)g;(void)b;(void)a; n_fill++; }
static void   be_stroke(float x,float y,float w,float h,float r,float g,float b,float a)
{ (void)x;(void)y;(void)w;(void)h;(void)r;(void)g;(void)b;(void)a; n_stroke++; }
static void   be_tinted(bd_texture t,float dx,float dy,float dw,float dh,
    float sx,float sy,float sw,float sh,float r,float g,float b,float a)
{ (void)t;(void)dx;(void)dy;(void)dw;(void)dh;(void)sx;(void)sy;(void)sw;(void)sh;
  (void)r;(void)g;(void)b;(void)a; n_tinted++; }
static void   be_vfont_begin(float a,float b,float c,float d){(void)a;(void)b;(void)c;(void)d;}
static void   be_vfont_end(void){}
static void   be_vfont_draw(bd_font f,float x,float y,float s,
    float r,float g,float b,float a,const char *t)
{ (void)f;(void)x;(void)y;(void)s;(void)r;(void)g;(void)b;(void)a;(void)t; n_vfont++; }
static float  be_vfont_w(bd_font f,float s,const char *t)
{ (void)f;(void)s; return t ? (float)strlen(t) * 7.0f : 0.0f; }
static bd_texture be_load_tex(const char *p){ (void)p; return (bd_texture){1}; }
static void   be_destroy_tex(bd_texture t){ (void)t; }
static bd_font be_load_font(const char *p){ (void)p; return (bd_font){1}; }
static void   be_destroy_font(bd_font f){ (void)f; }

/* ---- GPU interface recorders (v0.2) ---- */
static int n_shader, n_mesh, n_drawmesh, n_updatemesh, n_maketex;
static bd_shader be_make_shader(const char *v, const char *f)
{ (void)v; (void)f; n_shader++; return (bd_shader){1}; }
static void be_destroy_shader(bd_shader s){ (void)s; }
static bd_mesh be_make_mesh(const bd_vertex *v, int n, int dyn)
{ (void)v; (void)n; (void)dyn; n_mesh++; return (bd_mesh){1}; }
static void be_update_mesh(bd_mesh m, const bd_vertex *v, int n)
{ (void)m; (void)v; (void)n; n_updatemesh++; }
static void be_destroy_mesh(bd_mesh m){ (void)m; }
static bd_texture be_make_tex(int w, int h, const void *px)
{ (void)w; (void)h; (void)px; n_maketex++; return (bd_texture){2}; }
static void be_update_tex(bd_texture t,int x,int y,int w,int h,const void *px)
{ (void)t;(void)x;(void)y;(void)w;(void)h;(void)px; }
static void be_use_shader(bd_shader s){ (void)s; }
static void be_uni_i(bd_shader s,const char *n,int v){ (void)s;(void)n;(void)v; }
static void be_uni_f(bd_shader s,const char *n,float v){ (void)s;(void)n;(void)v; }
static void be_uni_2(bd_shader s,const char *n,float x,float y){ (void)s;(void)n;(void)x;(void)y; }
static void be_uni_3(bd_shader s,const char *n,float x,float y,float z){ (void)s;(void)n;(void)x;(void)y;(void)z; }
static void be_uni_4(bd_shader s,const char *n,float x,float y,float z,float w){ (void)s;(void)n;(void)x;(void)y;(void)z;(void)w; }
static void be_uni_m(bd_shader s,const char *n,const float m[16]){ (void)s;(void)n;(void)m; }
static void be_bind_tex(bd_texture t,int u){ (void)t;(void)u; }
static void be_draw_mesh(bd_mesh m){ (void)m; n_drawmesh++; }

static const bd_backend stub = {
	.width=be_width, .height=be_height, .time=be_time, .viewport=be_viewport,
	.clear=be_clear, .sprite_begin=be_sprite_begin, .sprite_end=be_sprite_end,
	.fill_rect=be_fill, .stroke_rect=be_stroke, .draw_tinted=be_tinted,
	.vfont_begin=be_vfont_begin, .vfont_end=be_vfont_end, .vfont_draw=be_vfont_draw,
	.vfont_text_width=be_vfont_w, .load_texture=be_load_tex,
	.destroy_texture=be_destroy_tex, .load_font=be_load_font,
	.destroy_font=be_destroy_font,
	.make_shader=be_make_shader, .destroy_shader=be_destroy_shader,
	.make_mesh=be_make_mesh, .update_mesh=be_update_mesh, .destroy_mesh=be_destroy_mesh,
	.make_texture=be_make_tex, .update_texture=be_update_tex,
	.use_shader=be_use_shader, .set_uniform_int=be_uni_i, .set_uniform_float=be_uni_f,
	.set_uniform_vec2=be_uni_2, .set_uniform_vec3=be_uni_3, .set_uniform_vec4=be_uni_4,
	.set_uniform_mat4=be_uni_m, .bind_texture=be_bind_tex, .draw_mesh=be_draw_mesh,
};

/* ---- click callback ---- */
static int clicked;
static void on_click(bd_id id, void *arg){ (void)id; (void)arg; clicked++; }

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

	check("widgets created", frame && term && btn && rec);

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

	/* terminal write: plain text + bold + 256-color SGR */
	bd_terminal_write(term,
	    "\033[1mbold\033[0m \033[38;5;202morange\033[0m\r\n", -1);
	bd_terminal_write(term, "second line\r\n", -1);

	/* custom palette */
	bd_palette pal = bd_palette_default();
	pal.ansi[1] = 0xE06C75FFu;
	bd_terminal_set_palette(term, &pal);

	/* render: stub records draw calls */
	n_fill = n_tinted = n_vfont = 0;
	bd_gui_render();
	check("render produced fills (chrome + term bg)", n_fill > 0);
	check("render produced glyph quads (terminal text)", n_tinted > 0);
	check("render queued vfont chrome text (button label)", n_vfont > 0);

	/* write to a non-terminal must be a safe no-op */
	bd_terminal_write(btn, "ignored", -1);
	check("write to non-terminal is a no-op (no crash)", 1);

	/* toolkit renderer (bd_draw) on the stub GPU interface */
	int ok = bd_draw_init(&stub, NULL, 14.0f);   /* NULL font: text no-ops */
	check("bd_draw_init created shader + mesh", ok && n_shader == 1 && n_mesh == 1);
	bd_draw_begin(800, 500);
	bd_draw_rect(10, 10, 100, 20, 0x334455FFu);
	bd_draw_rect_lines(10, 10, 100, 20, 0x778899FFu);
	bd_draw_sprite((bd_texture){7}, 0, 0, 16, 16, 0, 0, 1, 1, 0xFFFFFFFFu);
	bd_draw_end();
	check("bd_draw batched and drew (draw_mesh called)", n_drawmesh >= 1);
	check("bd_draw text width is 0 without a font",
	    bd_draw_text_width("hi") == 0.0f);
	bd_draw_text("ignored", 0, 0, 0xFFFFFFFFu);  /* no font: no-op, no crash */
	bd_draw_shutdown();
	check("bd_draw shutdown (no crash)", 1);

	bd_gui_cleanup();
	check("cleanup completed", 1);

	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
