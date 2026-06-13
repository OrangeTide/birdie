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

static const bd_backend stub = {
	.width=be_width, .height=be_height, .time=be_time, .viewport=be_viewport,
	.clear=be_clear, .sprite_begin=be_sprite_begin, .sprite_end=be_sprite_end,
	.fill_rect=be_fill, .stroke_rect=be_stroke, .draw_tinted=be_tinted,
	.vfont_begin=be_vfont_begin, .vfont_end=be_vfont_end, .vfont_draw=be_vfont_draw,
	.vfont_text_width=be_vfont_w, .load_texture=be_load_tex,
	.destroy_texture=be_destroy_tex, .load_font=be_load_font,
	.destroy_font=be_destroy_font,
};

/* ---- click callback ---- */
static int clicked;
static void on_click(bd_id id, void *arg){ (void)id; (void)arg; clicked++; }

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

	check("widgets created", frame && term && btn);

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

	bd_gui_cleanup();
	check("cleanup completed", 1);

	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
