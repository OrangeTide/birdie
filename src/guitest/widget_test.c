/*
 * widget_test.c -- birdie-gui widget gallery.
 *
 * A standalone sample that exhibits and exercises every working widget on the
 * raw-GLES backend (bd_backend_gles + window.h), independent of ludica. birdie
 * the MUD client runs on ludica; this gallery runs on GLES, so both backends
 * stay exercised. It also doubles as the place to grow interactive widget
 * tests as new widgets land.
 *
 * The build stages the fonts next to the binary, so it runs
 * from any directory.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "widget.h"
#include "widget_ext.h"
#include "bd_widget_vt.h"
#include "bd_widget_value.h"
#include "bd_widget_form.h"
#include "bd_widget_combo.h"
#include "bd_widget_explorer.h"
#include "bd_widget_editor.h"
#include "bd_widget_sketch.h"
#include "bd_widget_table.h"
#include "bd_widget_inventory.h"
#include "bd_widget_indicator.h"
#include "bd_widget_tabview.h"
#include "bd_widget_dock.h"
#include "bd_widget_actionbar.h"
#include "bd_widget_icon.h"
#include "bd_widget_meter.h"
#include "bd_widget_progress.h"
#include "bd_widget_tree.h"
#include "bd_widget_chart.h"
#include "bd_backend_gles.h"
#include "bd_backend_gles_core.h"
#include "window.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

#include <EGL/egl.h>

static bd_id status;
static bd_id term;
static bd_id tab_view;        /* the gallery's BD_TAB_VIEW (for auto-select) */
static int   desktop_tab;     /* index of the "Desktop" MDI pane */
static bd_id desk_logwin;     /* a Desktop-pane frame (auto-minimize for a shot) */
/* animated meters, driven from the main loop to show ballistics/peak/fill */
static bd_id m_vu, m_eye, m_load, m_sig, m_hp, m_mp, m_disk, m_prog;
static bd_id m_chart;   /* system-monitor strip chart */
static bd_id m_hpbar, m_mpbar;   /* glass liquid tubes (bar-form BD_METER_VIAL) */
static bd_id focus_led;   /* mirrors bd_gui_focused() each frame */
static int   running = 1;

/* Desktop-tab wallpaper: a procedural texture run through an effect shader as
 * the managed canvas backdrop, toggled on/off from the Settings dialog. */
static bd_id      desk_canvas;
static bd_texture wall_tex;
static bd_shader  wall_fx;

/* Echo a line to both the status bar and the terminal log, so every widget
 * interaction is visible. */
static void
report(const char *msg)
{
	bd_set(status, BD_LABEL_S, msg, BD_END);
	if (term) {
		bd_terminal_write(term, msg, -1);
		bd_terminal_write(term, "\r\n", 2);
	}
}

/* ---- callbacks ---- */

static void on_btn(bd_id id, void *arg)   { (void)id; report((const char *)arg); }

static void on_launch(bd_id id, uint64_t key, void *arg)
{
	(void)id; (void)key;
	static char b[48];
	snprintf(b, sizeof b, "launch %s", (const char *)arg);
	report(b);
}

static bd_id sketch;   /* the sketch pad, cleared by its Clear button */
static void on_canvas_clear(bd_id id, void *arg)
{
	(void)id; (void)arg;
	bd_sketch_clear(sketch);
	report("Sketch cleared");
}

static void
on_quit_confirm(bd_id n, int button, void *arg)
{
	(void)n; (void)arg;
	if (button == 0)            /* "Quit" */
		running = 0;
}

static void
on_quit(bd_id id, void *arg)
{
	(void)id; (void)arg;
	bd_notice_open("Quit the widget gallery?", "Quit\nCancel",
		on_quit_confirm, NULL);
}

/* Close the window the Close button lives in: walk up to its top-level frame
 * and destroy it (which closes the native window). */
static void
on_close_window(bd_id id, void *arg)
{
	(void)arg;
	bd_id f = bd_parent(id);
	while (f != BD_NONE && bd_parent(f) != BD_NONE)
		f = bd_parent(f);
	if (f != BD_NONE)
		bd_destroy(f);
}

static void
on_slider(bd_id id, void *arg, float t)
{
	(void)id;
	static char b[64];
	snprintf(b, sizeof b, "%s: %d%%", (const char *)arg, (int)(t * 100 + 0.5f));
	report(b);
}

static void
on_knob(bd_id id, void *arg, float v)
{
	(void)id;
	static char b[64];
	snprintf(b, sizeof b, "%s: %g", (const char *)arg, v);
	report(b);
}

static void
on_indicator(bd_id id, void *arg, int state)
{
	(void)id;
	static char b[64];
	snprintf(b, sizeof b, "%s -> state %d", (const char *)arg, state);
	report(b);
}

static void
on_jog(bd_id id, void *arg, float d)
{
	(void)id; (void)arg;
	static float acc;
	static char b[64];
	acc += d;
	snprintf(b, sizeof b, "Jog: %+.2f (sum %.2f)", d, acc);
	report(b);
}

static void
on_toggle(bd_id id, void *arg, int on)
{
	(void)id; (void)arg;
	report(on ? "Toggle: ON" : "Toggle: OFF");
}

static void
on_check(bd_id id, void *arg, int checked)
{
	(void)id;
	report(checked ? (const char *)arg : "unchecked");
}

static void
on_radio(bd_id id, void *arg, int idx)
{
	(void)id; (void)arg;
	static const char *names[] = { "Radio: Slow", "Radio: Normal", "Radio: Fast" };
	report(idx >= 0 && idx < 3 ? names[idx] : "Radio: none");
}

static void
on_combo(bd_id id, void *arg, int idx)
{
	(void)id; (void)arg;
	static const char *names[] = { "Combo: UTF-8", "Combo: Latin-1",
		"Combo: ASCII", "Combo: CP437" };
	report(idx >= 0 && idx < 4 ? names[idx] : "Combo: none");
}

static void
on_spinner(bd_id id, void *arg, int value)
{
	(void)id; (void)arg;
	static char msg[32];
	snprintf(msg, sizeof msg, "Port: %d", value);
	report(msg);
}

static void
on_xy(bd_id id, void *arg, float x, float y)
{
	(void)id;
	static char b[64];
	snprintf(b, sizeof b, "%s: %.2f, %.2f", (const char *)arg, x, y);
	report(b);
}

/* ---- a small explorer model (drag to rearrange, F2 to rename) ---- */
static struct srv { uint64_t key; char name[24]; int x, y; } servers[] = {
	{ 1, "Aardwolf",  -1, -1 }, { 2, "BatMUD",    -1, -1 },
	{ 3, "Discworld", -1, -1 }, { 4, "Lensmoor",  -1, -1 },
	{ 5, "Nanvaent",  -1, -1 }, { 6, "Threshold", -1, -1 },
	{ 7, "Achaea",    -1, -1 }, { 8, "Genesis",   -1, -1 },
	{ 9, "Realms",    -1, -1 }, { 10, "Medievia", -1, -1 },
	{ 11, "Gemstone", -1, -1 }, { 12, "Armageddon",-1, -1 },
};
static int srv_count(void *ctx){ (void)ctx; return (int)(sizeof servers / sizeof *servers); }
static void
srv_get(void *ctx, int i, bd_explorer_item *out)
{
	(void)ctx;
	out->key = servers[i].key;
	out->label = servers[i].name;
	out->icon = (bd_texture){0};
	out->enabled = 1;
	out->x = servers[i].x;
	out->y = servers[i].y;
	out->user = &servers[i];
}
static void
srv_set_pos(void *ctx, uint64_t key, int x, int y)
{
	(void)ctx;
	for (int i = 0; i < srv_count(NULL); i++)
		if (servers[i].key == key) { servers[i].x = x; servers[i].y = y; }
}
static void
srv_set_name(void *ctx, uint64_t key, const char *name)
{
	(void)ctx;
	for (int i = 0; i < srv_count(NULL); i++)
		if (servers[i].key == key)
			snprintf(servers[i].name, sizeof servers[i].name, "%s", name);
}
static void
srv_activate(bd_id w, uint64_t key, void *user)
{
	(void)w; (void)key;
	static char b[64];
	snprintf(b, sizeof b, "activate %s", ((struct srv *)user)->name);
	report(b);
}

/* ---- a small table model (click a header to sort) ---- */
static const char *tbl_rows_data[][3] = {
	{ "Aardwolf",  "aardmud.org",              "23"   },
	{ "BatMUD",    "batmud.bat.org",           "23"   },
	{ "Discworld", "discworld.starturtle.net", "4242" },
	{ "Achaea",    "achaea.com",               "23"   },
	{ "Genesis",   "mud.genesismud.org",       "3011" },
	{ "Threshold", "thresholdrpg.com",         "23"   },
	{ "Lensmoor",  "lensmoor.org",             "23"   },
	{ "Medievia",  "medievia.com",             "4000" },
};
static int gtbl_rows(void *c){ (void)c; return (int)(sizeof tbl_rows_data / sizeof *tbl_rows_data); }
static const char *gtbl_cell(void *c, int r, int col)
{ (void)c; return (col >= 0 && col < 3) ? tbl_rows_data[r][col] : ""; }
static void
gtbl_activate(bd_id w, int row, void *c)
{
	(void)w; (void)c;
	static char b[64];
	snprintf(b, sizeof b, "connect %s", tbl_rows_data[row][0]);
	report(b);
}

/* ---- a small tree model (Session sidebar: worlds grouped by category) ---- */
static const struct wnode {
	uint64_t key; const char *label; uint64_t parent; int folder;
} wnodes[] = {
	{ 1, "Favorites", 0, 1 }, { 2, "Fantasy", 0, 1 }, { 3, "Sci-Fi", 0, 1 },
	{ 10, "Aardwolf", 1, 0 }, { 11, "BatMUD", 1, 0 },
	{ 20, "Discworld", 2, 0 }, { 21, "Threshold", 2, 0 }, { 22, "Lensmoor", 2, 0 },
	{ 30, "Genesis", 3, 0 }, { 31, "Federation", 3, 0 },
};
#define WNODE_N ((int)(sizeof wnodes / sizeof wnodes[0]))
static int
w_child_count(void *c, uint64_t p)
{ (void)c; int n = 0; for (int i = 0; i < WNODE_N; i++) if (wnodes[i].parent == p) n++; return n; }
static uint64_t
w_child(void *c, uint64_t p, int idx)
{ (void)c; int k = 0; for (int i = 0; i < WNODE_N; i++) if (wnodes[i].parent == p && k++ == idx) return wnodes[i].key; return 0; }
static void
w_get(void *c, uint64_t key, bd_tree_item *out)
{ (void)c; for (int i = 0; i < WNODE_N; i++) if (wnodes[i].key == key) {
	out->label = wnodes[i].label; out->has_children = wnodes[i].folder;
	out->enabled = 1; out->user = (void *)&wnodes[i]; return; } }
static void
w_activate(bd_id w, uint64_t key, void *user)
{
	(void)w; (void)key;
	static char b[64];
	snprintf(b, sizeof b, "connect %s", ((const struct wnode *)user)->label);
	report(b);
}

/* ---- an editor completion provider (MUD verbs + directions) ---- */
static int
mud_complete(bd_id ed, const char *prefix, bd_completion *out, int max, void *user)
{
	(void)ed; (void)user;
	static const struct { const char *w, *kind; } vocab[] = {
		{ "connect", "cmd" }, { "cast", "cmd" }, { "consider", "cmd" },
		{ "north", "dir" }, { "northeast", "dir" }, { "northwest", "dir" },
		{ "south", "dir" }, { "look", "cmd" }, { "inventory", "cmd" },
		{ "kill", "cmd" }, { "score", "cmd" }, { "wield", "cmd" },
	};
	int n = 0, pl = (int)strlen(prefix);
	for (int i = 0; i < (int)(sizeof vocab / sizeof vocab[0]); i++)
		if (n < max && strncmp(vocab[i].w, prefix, (size_t)pl) == 0)
			out[n++] = (bd_completion){ .text = vocab[i].w,
			    .detail = vocab[i].kind };
	return n;
}

/* Open a second native window: a small dialog with its own widgets, proving
 * windows render and take input independently. */
static int dialog_n;

static void
on_new_window(bd_id id, void *arg)
{
	(void)id; (void)arg;
	static char title[32];
	snprintf(title, sizeof title, "Dialog %d", ++dialog_n);

	bd_id dlg = bd_create(BD_NONE, BD_FRAME,
		BD_LABEL_S, title, BD_LAYOUT_I, BD_LAYOUT_COL,
		BD_PREF_W_I, 380, BD_PREF_H_I, 440, BD_END);

	bd_id body = bd_create(dlg, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_COL, BD_GROW_I, 1,
		BD_PAD_I, 8, BD_GAP_I, 6, BD_END);
	bd_create(body, BD_LABEL,
		BD_LABEL_S, "Servers (drag to arrange, F2 to rename)",
		BD_PREF_H_I, 18, BD_END);
	bd_id ex = bd_explorer_create(body,
		&(bd_explorer_model){ .count = srv_count, .get = srv_get,
			.set_pos = srv_set_pos, .set_name = srv_set_name },
		&(bd_explorer_cb){ .activate = srv_activate },
		BD_GROW_I, 1, BD_END);
	if (getenv("GALLERY_AUTORENAME"))   /* open the rename editor for a shot */
		bd_explorer_begin_rename(ex, 1);

	bd_id med = bd_editor_create(body, BD_PREF_H_I, 92, BD_END);
	bd_editor_set_text(med,
		"X:1\nT:Demo\nK:C\nCDEF GABc|\nc2c2 d2e2 |");
	/* header field: bold underlined accent */
	bd_editor_style_span(med, 0, 3,
		(bd_rich_style){ BD_RT_BOLD | BD_RT_UNDERLINE, 0x7FB2FFFFu, 0 });
	/* title row: true italic */
	bd_editor_highlight_row(med, 1,
		(bd_rich_style){ BD_RT_ITALIC, 0xC8C8C8FFu, 0 });
	/* the "playing" row: bold dark text on amber */
	bd_editor_highlight_row(med, 3,
		(bd_rich_style){ BD_RT_BOLD, 0x202020FFu, 0xFFD54AFFu });

	bd_create(body, BD_LABEL, BD_LABEL_S, "Recent (BD_LIST):",
		BD_PREF_H_I, 16, BD_END);
	bd_create(body, BD_LIST, BD_PREF_H_I, 72,
		BD_LABEL_S, "Aardwolf  (yesterday)\nBatMUD  (last week)\n"
		"Discworld  (last month)\nLensmoor  (2 months ago)",
		BD_ON_CLICK_F, on_btn, BD_ON_CLICK_P, (void *)"connect to recent",
		BD_END);

	bd_id bar = bd_create(dlg, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_ROW, BD_PREF_H_I, 30,
		BD_PAD_I, 4, BD_GAP_I, 4, BD_END);
	bd_create(bar, BD_BUTTON, BD_LABEL_S, "Close", BD_PREF_W_I, 90,
		BD_ON_CLICK_F, on_close_window, BD_END);

	report("opened a new window");
}

/* ---- inventory grid: an RPG bag of icon slots ---- */

static bd_texture inv_icons[6];
static struct game_item { const char *name; int icon; int count; } inv_bag[48];

/* build a 32x32 solid-color icon with a darker border, uploaded as a texture */
static bd_texture
make_icon(uint32_t rgba)
{
	unsigned char px[32 * 32 * 4];
	uint32_t edge = ((rgba >> 1) & 0x7F7F7F00u) | 0xFFu;   /* halved, opaque */
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++) {
			uint32_t c = (x < 2 || x > 29 || y < 2 || y > 29) ? edge : rgba;
			unsigned char *p = px + (y * 32 + x) * 4;
			p[0] = (c >> 24) & 0xFF; p[1] = (c >> 16) & 0xFF;
			p[2] = (c >> 8) & 0xFF;  p[3] = c & 0xFF;
		}
	return bd_backend_get()->make_texture(32, 32, px);
}

static void
inv_get(void *ctx, int slot, bd_inventory_item *out)
{
	(void)ctx;
	if (slot < 0 || slot >= 48 || !inv_bag[slot].name)
		return;   /* empty slot */
	struct game_item *g = &inv_bag[slot];
	out->key     = (uint64_t)(slot + 1);
	out->label   = g->name;
	out->icon    = inv_icons[g->icon];
	out->count   = g->count;
	out->enabled = 1;
	out->tooltip = g->name;
}

static bd_id inv_widget;
static void
inv_activate(bd_id w, int slot, uint64_t key, void *ctx)
{
	(void)w; (void)key; (void)ctx;
	char b[64];
	snprintf(b, sizeof b, "inventory: use slot %d (%s)", slot,
	    inv_bag[slot].name ? inv_bag[slot].name : "?");
	report(b);
}
static void
inv_context(bd_id w, int slot, uint64_t key, int sx, int sy, void *ctx)
{
	(void)w; (void)key; (void)sx; (void)sy; (void)ctx;
	char b[64];
	snprintf(b, sizeof b, "inventory: right-click slot %d", slot);
	report(b);
}
static void
inv_move(bd_id w, int from, int to, void *ctx)
{
	(void)w; (void)ctx;
	struct game_item t = inv_bag[to];   /* swap the two slots */
	inv_bag[to] = inv_bag[from];
	inv_bag[from] = t;
	char b[64];
	snprintf(b, sizeof b, "inventory: move slot %d -> %d", from, to);
	report(b);
}

/* ---- a labeled framing panel for a gallery section ---- */

static bd_id
section(bd_id parent, const char *title, int layout, int height)
{
	bd_id box = bd_create(parent, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_COL,
		BD_PREF_H_I, height,
		BD_BG_C, 0x2B2D30FFu,
		BD_PAD_I, 6,
		BD_GAP_I, 4,
		BD_END);
	bd_create(box, BD_LABEL, BD_LABEL_S, title,
		BD_PREF_H_I, 16, BD_FG_C, 0x9DA3AAFFu, BD_END);
	bd_id row = bd_create(box, BD_PANEL,
		BD_LAYOUT_I, layout,
		BD_GAP_I, 14,
		BD_GROW_I, 1,
		BD_END);
	return row;
}

/* A procedural desktop wallpaper: a diagonal indigo gradient with a faint grid,
 * uploaded as an RGBA texture the effect shader samples. */
static bd_texture
make_wallpaper(void)
{
	enum { N = 256 };
	static unsigned char px[N * N * 4];
	for (int y = 0; y < N; y++)
		for (int x = 0; x < N; x++) {
			float fx = x / (float)N, fy = y / (float)N;
			float g = 0.5f * (fx + fy);
			float r = 0.10f + 0.10f * g;
			float gg = 0.13f + 0.16f * (1.0f - g);
			float b = 0.22f + 0.30f * (1.0f - fy);
			if ((x & 31) == 0 || (y & 31) == 0)      /* grid lines */
				{ r += 0.05f; gg += 0.06f; b += 0.09f; }
			unsigned char *p = &px[(y * N + x) * 4];
			p[0] = (unsigned char)(fminf(r, 1.0f) * 255.0f);
			p[1] = (unsigned char)(fminf(gg, 1.0f) * 255.0f);
			p[2] = (unsigned char)(fminf(b, 1.0f) * 255.0f);
			p[3] = 255;
		}
	return bd_backend_gles.make_texture(N, N, px);
}

/* The wallpaper effect: a slow drifting ripple, a moving sheen band, and a
 * vignette over the sampled texture. Pairs with BD_SHADER_QUAD_VERT (v_uv). */
static const char *const WALL_FRAG =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform float u_time;\n"
	"out vec4 frag;\n"
	"void main() {\n"
	"    vec2 uv = v_uv;\n"
	"    uv += 0.008 * vec2(sin(uv.y*10.0 + u_time*0.8),\n"
	"                       cos(uv.x*9.0  + u_time*0.6));\n"
	"    vec3 c = texture(u_tex, uv).rgb;\n"
	"    float band = fract(uv.x*0.5 - u_time*0.05)*2.0 - 1.0;\n"
	"    c += 0.06 * smoothstep(0.5, 0.0, abs(band));\n"    /* sheen */
	"    c *= 0.9 + 0.1 * sin(u_time*0.5);\n"               /* breathing */
	"    vec2 d = v_uv - 0.5;\n"
	"    c *= 1.0 - 0.6 * dot(d, d);\n"                     /* vignette */
	"    frag = vec4(c, 1.0);\n"
	"}\n";

/* Settings-dialog toggle: swap the canvas between its solid backdrop and the
 * GLES wallpaper (a texture drawn through WALL_FRAG). */
static void
on_wallpaper(bd_id id, void *arg, int on)
{
	(void)id; (void)arg;
	bd_managed_canvas_set_backdrop(desk_canvas, wall_tex,
	    on ? wall_fx : (bd_shader){ 0 });
	report(on ? "GLES wallpaper on" : "solid background");
}

static void
build_ui(void)
{
	bd_id frame = bd_create(BD_NONE, BD_FRAME,
		BD_LABEL_S, "birdie-gui widget gallery",
		BD_LAYOUT_I, BD_LAYOUT_COL,
		BD_END);

	/* ---- menu bar (exhibits pull-downs + a pinnable menu) ---- */
	bd_id menu = bd_create(frame, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_ROW,
		BD_PREF_H_I, 22, BD_BG_C, 0x3C3F41FFu,
		BD_PAD_I, 2, BD_GAP_I, 16, BD_END);

	bd_id m_file = bd_create(menu, BD_MENU, BD_LABEL_S, "File", BD_END);
	bd_create(m_file, BD_BUTTON, BD_LABEL_S, "New",
		BD_ON_CLICK_F, on_btn, BD_ON_CLICK_P, (void *)"File > New", BD_END);
	bd_create(m_file, BD_BUTTON, BD_LABEL_S, "Open...",
		BD_ON_CLICK_F, on_btn, BD_ON_CLICK_P, (void *)"File > Open", BD_END);
	bd_create(m_file, BD_BUTTON, BD_LABEL_S, "Quit",
		BD_ON_CLICK_F, on_quit, BD_END);

	bd_id m_edit = bd_create(menu, BD_MENU, BD_LABEL_S, "Edit", BD_END);
	bd_create(m_edit, BD_BUTTON, BD_LABEL_S, "Copy",
		BD_ON_CLICK_F, on_btn, BD_ON_CLICK_P, (void *)"Edit > Copy", BD_END);
	bd_create(m_edit, BD_BUTTON, BD_LABEL_S, "Paste",
		BD_ON_CLICK_F, on_btn, BD_ON_CLICK_P, (void *)"Edit > Paste", BD_END);

	/* a menu opened pinned, to exhibit olvwm-style pushpins */
	bd_id m_view = bd_create(menu, BD_MENU, BD_LABEL_S, "View (pinnable)",
		BD_END);
	bd_create(m_view, BD_BUTTON, BD_LABEL_S, "Toggle A",
		BD_ON_CLICK_F, on_btn, BD_ON_CLICK_P, (void *)"View > A", BD_END);
	bd_create(m_view, BD_BUTTON, BD_LABEL_S, "Toggle B",
		BD_ON_CLICK_F, on_btn, BD_ON_CLICK_P, (void *)"View > B", BD_END);

	/* ---- tab view: the gallery split into folder-tab panes ---- */
	bd_id tabs = bd_tabview_create(frame, BD_GROW_I, 1, BD_END);
	tab_view = tabs;

	/* -- Session: the MUD-client surface (terminal, command line, MUD list) -- */
	bd_id session = bd_tabview_add_pane(tabs, "Session");
	bd_set(session, BD_PAD_I, 6, BD_GAP_I, 6, BD_END);
	/* a worlds tree (BD_TREE) sidebar, then the terminal + its scrollbar */
	bd_id termrow = bd_create(session, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_ROW, BD_GROW_I, 1, BD_GAP_I, 6, BD_END);
	bd_id wtree = bd_tree_create(termrow,
		&(bd_tree_model){ .child_count = w_child_count, .child = w_child,
			.get = w_get },
		&(bd_tree_cb){ .activate = w_activate },
		BD_PREF_W_I, 170, BD_END);
	bd_tree_set_expanded(wtree, 1, 1);   /* open "Favorites" */
	bd_tree_set_expanded(wtree, 2, 1);   /* open "Fantasy" */
	term = bd_terminal_create(termrow, BD_GROW_I, 1, BD_END);
	bd_id sbar = bd_create(termrow, BD_SCROLLBAR, BD_PREF_W_I, 14, BD_END);
	bd_scrollbar_set(sbar, 0.25f, 0.4f);
	bd_terminal_write(term,
		"\033[1mbirdie-gui widget gallery\033[0m\r\n"
		"GLES backend. Interact with the widgets; events log here.\r\n"
		"Each folder tab holds a different group of widgets.\r\n", -1);
	bd_create(session, BD_INPUT_LINE, BD_PREF_H_I, 24, BD_PAD_I, 4, BD_END);

	/* MUD list: a sortable multi-column table (click a header to sort) */
	bd_create(session, BD_LABEL, BD_LABEL_S, "MUD list (BD_TABLE -- click a header to sort):",
		BD_PREF_H_I, 16, BD_END);
	static const bd_table_column gcols[] = {
		{ "MUD",  0,   BD_TABLE_LEFT,  0 },
		{ "Host", 170, BD_TABLE_LEFT,  0 },
		{ "Port", 56,  BD_TABLE_RIGHT, BD_TABLE_COL_NUMERIC },
	};
	bd_table_create(session, gcols, 3,
		&(bd_table_model){ gtbl_rows, gtbl_cell, NULL },
		&(bd_table_cb){ .activate = gtbl_activate },
		BD_PREF_H_I, 160, BD_END);

	/* -- Inventory: an RPG bag (drag between slots, right-click, wheel to
	 * scroll, stack-count badges, hover tooltips) + a recent-connections list -- */
	bd_id invpane = bd_tabview_add_pane(tabs, "Inventory");
	bd_set(invpane, BD_PAD_I, 6, BD_GAP_I, 6, BD_END);
	static const uint32_t pal[6] = { 0x9AA4B0FFu, 0xC08A3EFFu, 0xD24B4BFFu,
	    0xE8C24AFFu, 0x6FB36FFFu, 0x8A6FC0FFu };
	for (int i = 0; i < 6; i++)
		inv_icons[i] = make_icon(pal[i]);
	inv_bag[0]  = (struct game_item){ "Sword",  0, 1  };
	inv_bag[1]  = (struct game_item){ "Shield", 1, 1  };
	inv_bag[2]  = (struct game_item){ "Potion", 2, 5  };
	inv_bag[3]  = (struct game_item){ "Gold",   3, 99 };
	inv_bag[5]  = (struct game_item){ "Herb",   4, 12 };
	inv_bag[6]  = (struct game_item){ "Amulet", 5, 1  };
	inv_bag[8]  = (struct game_item){ "Key",    0, 1  };
	inv_bag[11] = (struct game_item){ "Old Map",1, 1  };
	inv_bag[14] = (struct game_item){ "Bow",    5, 1  };

	bd_create(invpane, BD_LABEL,
		BD_LABEL_S, "Inventory (BD_INVENTORY -- drag between slots, wheel to scroll):",
		BD_PREF_H_I, 16, BD_END);
	inv_widget = bd_inventory_create(invpane, 6, 8,
		&(bd_inventory_model){ .get = inv_get },
		&(bd_inventory_cb){ .activate = inv_activate, .context = inv_context,
		    .move = inv_move },
		BD_GROW_I, 1, BD_END);
	bd_inventory_set_cell_size(inv_widget, 40);
	bd_create(invpane, BD_LABEL, BD_LABEL_S, "Recent (BD_LIST):",
		BD_PREF_H_I, 16, BD_END);
	bd_create(invpane, BD_LIST, BD_PREF_H_I, 84,
		BD_LABEL_S, "Aardwolf  (yesterday)\nBatMUD  (last week)\n"
		"Discworld  (last month)\nLensmoor  (2 months ago)",
		BD_ON_CLICK_F, on_btn, BD_ON_CLICK_P, (void *)"connect to recent",
		BD_END);

	/* -- Controls: knobs, switches, indicator lamps -- */
	bd_id right = bd_tabview_add_pane(tabs, "Controls");
	bd_set(right, BD_PAD_I, 6, BD_GAP_I, 6, BD_END);

	/* knobs: every dial style + a stepped N-way rotary switch */
	bd_id krow = section(right, "Knobs & dials", BD_LAYOUT_ROW, 96);
	bd_knob_create(krow, &(bd_knob_desc){
		.min = 0, .max = 1, .value = 0.3f, .dial = BD_DIAL_DOTS,
		.cb = on_knob, .arg = (void *)"Gain" }, BD_PREF_W_I, 60, BD_END);
	bd_knob_create(krow, &(bd_knob_desc){
		.min = -1, .max = 1, .value = 0, .dial = BD_DIAL_BALANCE,
		.cb = on_knob, .arg = (void *)"Balance" }, BD_PREF_W_I, 60, BD_END);
	bd_knob_create(krow, &(bd_knob_desc){
		.min = 0, .max = 127, .value = 64, .dial = BD_DIAL_LABELS, .hex = 1,
		.cb = on_knob, .arg = (void *)"MIDI CC" }, BD_PREF_W_I, 100, BD_END);
	bd_knob_create(krow, &(bd_knob_desc){
		.min = 0, .max = 6, .step = 1, .value = 2, .dial = BD_DIAL_DOTS,
		.cb = on_knob, .arg = (void *)"Mode" }, BD_PREF_W_I, 60, BD_END);

	/* switches and relative wheels */
	bd_id srow = section(right, "Switches & wheels", BD_LAYOUT_ROW, 96);
	bd_toggle_create(srow, &(bd_toggle_desc){ .on = 1, .cb = on_toggle },
		BD_PREF_W_I, 56, BD_END);
	bd_wheel_create(srow, &(bd_wheel_desc){ .orient = BD_VERTICAL, .cb = on_jog },
		BD_PREF_W_I, 30, BD_END);
	bd_wheel_create(srow, &(bd_wheel_desc){ .orient = BD_HORIZONTAL, .cb = on_jog },
		BD_PREF_W_I, 80, BD_PREF_H_I, 30, BD_END);
	bd_knob_create(srow, &(bd_knob_desc){
		.relative = 1, .dimples = 3, .cb = on_jog }, /* endless jog dial */
		BD_PREF_W_I, 76, BD_END);
	/* checkboxes: a column of labeled booleans (click or Space to toggle) */
	bd_id chkcol = bd_create(srow, BD_PANEL, BD_LAYOUT_COL, BD_PREF_W_I, 120,
		BD_PAD_I, 4, BD_GAP_I, 2, BD_END);
	bd_checkbox_create(chkcol, &(bd_checkbox_desc){ .label = "Enable TLS",
		.checked = 1, .cb = on_check, .arg = (void *)"TLS on" }, BD_END);
	bd_checkbox_create(chkcol, &(bd_checkbox_desc){ .label = "Auto-login",
		.cb = on_check, .arg = (void *)"auto-login" }, BD_END);
	bd_checkbox_create(chkcol, &(bd_checkbox_desc){ .label = "Log to file",
		.cb = on_check, .arg = (void *)"logging" }, BD_END);
	/* radio group: one exclusive choice (click or arrow keys) */
	static const char *const rate_opts[] = { "Slow", "Normal", "Fast" };
	bd_radio_create(srow, &(bd_radio_desc){ .labels = rate_opts, .count = 3,
		.selected = 1, .orient = BD_VERTICAL, .cb = on_radio }, BD_END);
	/* combo drop-down (opens a floating list via the shared overlay) */
	static const char *const enc_opts[] = { "UTF-8", "Latin-1", "ASCII", "CP437" };
	bd_combo_create(srow, &(bd_combo_desc){ .items = enc_opts, .count = 4,
		.selected = 0, .placeholder = "Encoding", .cb = on_combo },
		BD_PREF_W_I, 100, BD_END);
	/* numeric spinner (steppers, arrow keys, digit entry) */
	bd_spinner_create(srow, &(bd_spinner_desc){ .min = 1, .max = 65535,
		.step = 1, .value = 4000, .cb = on_spinner }, BD_PREF_W_I, 76, BD_END);

	/* indicator lamps: LEDs (clear/frosted/jewel lens), a bi-color, and a
	 * clickable lamp button that cycles off/red/green/amber */
	bd_id irow = section(right, "Indicators (click the last to cycle)",
		BD_LAYOUT_ROW, 60);
	/* FOCUS mirrors the window focus state: amber (unfocused) / green
	 * (focused). Driven each frame in the main loop from bd_gui_focused(),
	 * so it exercises the BD_EV_FOCUS_IN/OUT handler end to end. */
	focus_led = bd_indicator_create(irow, &(bd_indicator_desc){
		.colors = "amber green", .state = 2,
		.diameter = 16, .label = "FOCUS" }, BD_END);
	bd_indicator_create(irow, &(bd_indicator_desc){
		.lens = BD_LENS_CLEAR, .colors = "green",
		.state = 1, .diameter = 16, .label = "LINK" }, BD_END);
	bd_indicator_create(irow, &(bd_indicator_desc){
		.lens = BD_LENS_FROSTED, .colors = "blue",
		.diameter = 16, .label = "off" }, BD_END);
	bd_indicator_create(irow, &(bd_indicator_desc){
		.lens = BD_LENS_JEWEL, .colors = "red", .state = 1,
		.diameter = 24, .label = "ALARM" }, BD_END);
	bd_indicator_create(irow, &(bd_indicator_desc){
		.colors = "red green amber", .state = 2,
		.diameter = 16, .label = "MODE" }, BD_END);
	bd_indicator_create(irow, &(bd_indicator_desc){
		.colors = "red green amber",
		.clickable = 1, .diameter = 18, .label = "STATUS",
		.cb = on_indicator, .arg = (void *)"Status" }, BD_END);

	/* -- Pads: X-Y pads and sliders -- */
	bd_id pads = bd_tabview_add_pane(tabs, "Pads");
	bd_set(pads, BD_PAD_I, 6, BD_GAP_I, 6, BD_END);

	/* X-Y pads: bounded square + spring-return joystick circle */
	bd_id xrow = section(pads, "X-Y pads", BD_LAYOUT_ROW, 120);
	bd_xypad_create(xrow, &(bd_xypad_desc){
		.shape = BD_XY_SQUARE, .x = 0.5f, .y = 0.5f,
		.cb = on_xy, .arg = (void *)"Pad" }, BD_PREF_W_I, 90, BD_END);
	bd_xypad_create(xrow, &(bd_xypad_desc){
		.shape = BD_XY_CIRCLE, .spring = 1, .x = 0.5f, .y = 0.5f,
		.cb = on_xy, .arg = (void *)"Stick" }, BD_PREF_W_I, 90, BD_END);

	/* sliders: a tall vertical fader beside a horizontal one. The vertical
	 * fader keeps a fixed width (it fills the row height); the horizontal
	 * slider sits in a column wrapper so it keeps a short, fixed height
	 * instead of stretching its thumb to fill the tall row. */
	bd_id vrow = section(pads, "Sliders", BD_LAYOUT_ROW, 150);
	bd_slider_create(vrow, &(bd_slider_desc){ .orient = BD_VERTICAL,
		.value = 0.6f, .cb = on_slider, .arg = (void *)"Fader" },
		BD_PREF_W_I, 28, BD_END);
	bd_id hwrap = bd_create(vrow, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_COL, BD_GROW_I, 1, BD_END);
	bd_slider_create(hwrap, &(bd_slider_desc){ .orient = BD_HORIZONTAL,
		.value = 0.4f, .cb = on_slider, .arg = (void *)"Wet" },
		BD_PREF_H_I, 24, BD_END);

	/* -- Meters: the 0..1 instrument styles + a progress bar (animated in the
	 * main loop so ballistics, peak-hold, and the liquid fill are visible) -- */
	bd_id meters = bd_tabview_add_pane(tabs, "Meters");
	bd_set(meters, BD_PAD_I, 6, BD_GAP_I, 8, BD_END);

	/* system-monitor strip chart (BD_CHART): CPU %, latency ms, memory MB */
	bd_id chrow = section(meters, "System monitor (BD_CHART -- scrolling time series)",
		BD_LAYOUT_ROW, 130);
	m_chart = bd_chart_create(chrow, 160, BD_GROW_I, 1, BD_END);
	bd_chart_add_series(m_chart, &(bd_chart_series){ .label = "CPU", .unit = "%" });
	bd_chart_add_series(m_chart, &(bd_chart_series){ .label = "Latency", .unit = "ms" });
	bd_chart_add_series(m_chart, &(bd_chart_series){ .label = "Mem", .unit = "MB" });

	/* round instruments in a row */
	bd_id mrow = section(meters, "Round meters (VU / magic eye / pie / vials)",
		BD_LAYOUT_ROW, 130);
	m_vu = bd_meter_create(mrow, &(bd_meter_desc){ .style = BD_METER_VU,
		.value = 0.3f, .zones = "green amber red", .stops = "0.6 0.85",
		.ballistic = BD_METER_VU_BALLISTIC, .peak = 1, .size = 110,
		.label = "VU" }, BD_END);
	m_eye = bd_meter_create(mrow, &(bd_meter_desc){ .style = BD_METER_EYE,
		.value = 0.5f, .size = 96, .label = "Tune" }, BD_END);
	m_disk = bd_meter_create(mrow, &(bd_meter_desc){ .style = BD_METER_PIE,
		.value = 0.67f, .zones = "green amber red", .peak = 1, .size = 96,
		.label = "Disk" }, BD_END);
	/* RPG life/mana orbs: red drains, blue refills */
	m_hp = bd_meter_create(mrow, &(bd_meter_desc){ .style = BD_METER_VIAL,
		.value = 0.8f, .color = 0xD2233BFFu, .size = 96, .label = "HP" }, BD_END);
	m_mp = bd_meter_create(mrow, &(bd_meter_desc){ .style = BD_METER_VIAL,
		.value = 0.4f, .color = 0x2E7DE0FFu, .size = 96, .label = "MP" }, BD_END);

	/* bars: a zoned horizontal load meter and a vertical LED signal meter */
	bd_id brow = section(meters, "Level bars (zones, LED segments, peak-hold)",
		BD_LAYOUT_ROW, 140);
	bd_id bcol = bd_create(brow, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_COL,
		BD_GROW_I, 1, BD_GAP_I, 10, BD_END);
	m_load = bd_meter_create(bcol, &(bd_meter_desc){ .style = BD_METER_BAR,
		.value = 0.3f, .zones = "green amber red", .stops = "0.65 0.88",
		.ballistic = BD_METER_PEAK_HOLD, .peak = 1, .size = 22,
		.label = "Load" }, BD_GROW_I, 1, BD_END);
	m_prog = bd_progress_create(bcol, &(bd_progress_desc){ .value = 0.0f,
		.show_percent = 1, .label = "Copy" }, BD_GROW_I, 1, BD_END);
	m_sig = bd_meter_create(brow, &(bd_meter_desc){ .style = BD_METER_BAR,
		.orient = BD_VERTICAL, .segments = 12, .value = 0.5f,
		.zones = "green green amber red", .stops = "0.4 0.6 0.85",
		.peak = 1, .size = 26, .label = "Sig" }, BD_END);

	/* glass liquid tubes: the horizontal bar form of the BD_METER_VIAL orb,
	 * a life/mana bar (bd_progress with .glass) */
	bd_id grow = section(meters, "Glass liquid tubes (bar-form vials)",
		BD_LAYOUT_ROW, 64);
	bd_id gcol = bd_create(grow, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_COL,
		BD_GROW_I, 1, BD_GAP_I, 8, BD_END);
	m_hpbar = bd_progress_create(gcol, &(bd_progress_desc){ .value = 0.7f,
		.glass = 1, .show_percent = 1, .color = 0xD2233BFFu,
		.label = "Health" }, BD_GROW_I, 1, BD_END);
	m_mpbar = bd_progress_create(gcol, &(bd_progress_desc){ .value = 0.4f,
		.glass = 1, .show_percent = 1, .color = 0x2E7DE0FFu,
		.label = "Mana" }, BD_GROW_I, 1, BD_END);

	/* -- Paint & Layout: the sketch pad and the layout demos -- */
	bd_id paint = bd_tabview_add_pane(tabs, "Paint & Layout");
	bd_set(paint, BD_PAD_I, 6, BD_GAP_I, 6, BD_END);

	/* ---- pressure-sensitive sketch pad ---- */
	bd_id crow = section(paint, "Sketch pad (pen: pressure / tilt / "
		"barrel = red / eraser)", BD_LAYOUT_ROW, 220);
	sketch = bd_sketch_create(crow, BD_GROW_I, 1, BD_END);
	bd_sketch_set_nib(sketch, 10.0f);
	bd_id ccol = bd_create(crow, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_COL, BD_PREF_W_I, 70, BD_END);
	bd_create(ccol, BD_BUTTON, BD_LABEL_S, "Clear", BD_PREF_H_I, 24,
		BD_ON_CLICK_F, on_canvas_clear, BD_END);

	/* ---- layout: anchor (cell alignment) and packing (main-axis) ---- */
	bd_id lrow = section(paint, "Anchor & packing "
		"(BD_ANCHOR_I / BD_PACK_I)", BD_LAYOUT_ROW, 150);

	/* cross-axis anchor: fixed-width buttons align L / center / R in a column */
	bd_id acol = bd_create(lrow, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_COL,
		BD_GROW_I, 1, BD_GAP_I, 4, BD_PAD_I, 4, BD_BG_C, 0x232629FFu, BD_END);
	bd_create(acol, BD_BUTTON, BD_LABEL_S, "W", BD_PREF_W_I, 64,
		BD_PREF_H_I, 22, BD_ANCHOR_I, BD_ANCHOR_W, BD_END);
	bd_create(acol, BD_BUTTON, BD_LABEL_S, "center", BD_PREF_W_I, 64,
		BD_PREF_H_I, 22, BD_ANCHOR_I, BD_ANCHOR_CENTER, BD_END);
	bd_create(acol, BD_BUTTON, BD_LABEL_S, "E", BD_PREF_W_I, 64,
		BD_PREF_H_I, 22, BD_ANCHOR_I, BD_ANCHOR_E, BD_END);

	/* packing: each row justifies its two chips differently */
	static const struct { const char *name; int pack; } packs[] = {
		{ "start",   BD_PACK_START },
		{ "center",  BD_PACK_CENTER },
		{ "end",     BD_PACK_END },
		{ "between", BD_PACK_SPACE_BETWEEN },
	};
	bd_id pcol = bd_create(lrow, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_COL,
		BD_GROW_I, 1, BD_GAP_I, 4, BD_PAD_I, 4, BD_BG_C, 0x232629FFu, BD_END);
	for (int i = 0; i < (int)(sizeof packs / sizeof packs[0]); i++) {
		bd_id pr = bd_create(pcol, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_ROW,
			BD_GROW_I, 1, BD_GAP_I, 3, BD_PACK_I, packs[i].pack, BD_END);
		bd_create(pr, BD_BUTTON, BD_LABEL_S, packs[i].name,
			BD_PREF_W_I, 58, BD_END);
		bd_create(pr, BD_BUTTON, BD_LABEL_S, "+", BD_PREF_W_I, 20, BD_END);
	}

	/* FIXED anchoring: pills pinned to the corners and centre, tracking resize */
	bd_id fbox = bd_create(lrow, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_FIXED,
		BD_GROW_I, 1, BD_BG_C, 0x232629FFu, BD_END);
	static const struct { const char *name; int anchor; } pins[] = {
		{ "NW", BD_ANCHOR_NW }, { "NE", BD_ANCHOR_NE },
		{ "SW", BD_ANCHOR_SW }, { "SE", BD_ANCHOR_SE },
		{ "mid", BD_ANCHOR_CENTER },
	};
	for (int i = 0; i < (int)(sizeof pins / sizeof pins[0]); i++)
		bd_create(fbox, BD_LABEL, BD_LABEL_S, pins[i].name,
			BD_PREF_W_I, 30, BD_PREF_H_I, 16, BD_ANCHOR_I, pins[i].anchor,
			BD_X_I, 4, BD_Y_I, 4,
			BD_BG_C, 0x3C6E8FFFu, BD_FG_C, 0xEAF0F4FFu, BD_END);

	/* -- Desktop: an embedded window manager (BD_MANAGED_CANVAS) hosting
	 * floating frames plus a BD_DOCK, on the GLES multi_window backend. Each
	 * frame is a real top-level BD_FRAME parented to the canvas: drag its title
	 * bar to move it, snap it to a canvas edge, minimize (_) it to the dock, and
	 * click a dock tile to restore. This is the same in-surface WM the toolkit
	 * uses on a single-surface backend, now scoped to a widget. -- */
	bd_id deskpane = bd_tabview_add_pane(tabs, "Desktop");
	desktop_tab = bd_tabview_count(tabs) - 1;
	bd_set(deskpane, BD_PAD_I, 6, BD_GAP_I, 4, BD_END);
	bd_create(deskpane, BD_LABEL, BD_LABEL_S,
		"BD_MANAGED_CANVAS: terminal / editor / dialog windows; minimize (_) one "
		"to a dock tile or desktop icon (click restores). Settings toggles a GLES wallpaper.",
		BD_PREF_H_I, 16, BD_FG_C, 0x9DA3AAFFu, BD_END);

	/* standalone BD_ICON app launchers + an action-bar drop target: double-
	 * click a launcher to run it, or drag one onto the bar to bind it (the
	 * icon cell + cross-widget drag are shared with the dock/inventory) */
	bd_id launchrow = bd_create(deskpane, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_ROW,
		BD_PREF_H_I, 78, BD_GAP_I, 10, BD_END);
	static const struct { const char *name; int col; } apps[] = {
		{ "Telnet", 1 }, { "Editor", 4 }, { "Map", 5 },
	};
	for (int i = 0; i < 3; i++) {
		bd_id ic = bd_icon_create(launchrow,
			&(bd_icon_desc){ .key = 100 + i, .label = apps[i].name,
				.icon = inv_icons[apps[i].col], .enabled = 1 },
			BD_PREF_W_I, 64, BD_END);
		bd_icon_on_activate(ic, on_launch, (void *)apps[i].name);
	}
	bd_create(launchrow, BD_LABEL, BD_LABEL_S, "drag onto ->",
		BD_PREF_W_I, 72, BD_END);
	bd_actionbar_create(launchrow, 6, NULL, BD_END);

	bd_id cv = bd_managed_canvas_create(deskpane, BD_GROW_I, 1,
		BD_BG_C, 0x1E2024FFu, BD_END);
	desk_canvas = cv;

	/* build the wallpaper texture + effect shader now that GL is up; the
	 * Settings dialog's toggle swaps it in as the canvas backdrop. The sampler
	 * unit is fixed at 0; u_time is fed by the toolkit from the backend clock. */
	wall_tex = make_wallpaper();
	wall_fx = bd_backend_gles.make_shader(BD_SHADER_QUAD_VERT, WALL_FRAG);
	bd_backend_gles.use_shader(wall_fx);
	bd_backend_gles.set_uniform_int(wall_fx, "u_tex", 0);

	/* a top-left dock scoped to this canvas (empty until a frame is minimized),
	 * claimed as the canvas's exclusive minimize target so a minimized frame
	 * lands in the dock instead of also becoming a WM desktop icon */
	bd_id dk = bd_dock_create(cv, NULL, BD_END);
	bd_dock_set_gravity(dk, BD_GRAVITY_TOP_LEFT);
	bd_dock_set_tile_size(dk, 40);
	bd_managed_canvas_set_minimize(cv, BD_MINIMIZE_DOCK, dk);

	/* Servers: the icon-browser explorer, floating (drag to arrange, F2 rename) */
	bd_id srvwin = bd_create(cv, BD_FRAME, BD_LABEL_S, "Servers",
		BD_PREF_W_I, 250, BD_PREF_H_I, 230, BD_X_I, 90, BD_Y_I, 24,
		BD_BG_C, 0x2B2D30FFu, BD_END);
	bd_explorer_create(srvwin,
		&(bd_explorer_model){ .count = srv_count, .get = srv_get,
			.set_pos = srv_set_pos, .set_name = srv_set_name },
		&(bd_explorer_cb){ .activate = srv_activate }, BD_GROW_I, 1, BD_END);

	/* Notes: the rich-text editor, floating (its text survives tab switches) */
	bd_id noteswin = bd_create(cv, BD_FRAME, BD_LABEL_S, "Notes",
		BD_PREF_W_I, 260, BD_PREF_H_I, 150, BD_X_I, 380, BD_Y_I, 54,
		BD_BG_C, 0x2B2D30FFu, BD_END);
	bd_id ned = bd_editor_create(noteswin, BD_GROW_I, 1, BD_END);
	bd_editor_set_text(ned,
		"A floating editor pane.\nType a MUD verb (co, no...)\nfor autocomplete.");
	bd_editor_set_completer(ned, mud_complete, NULL);

	/* Log: a BD_LIST, floating */
	bd_id logwin = bd_create(cv, BD_FRAME, BD_LABEL_S, "Log",
		BD_PREF_W_I, 240, BD_PREF_H_I, 140, BD_X_I, 210, BD_Y_I, 240,
		BD_BG_C, 0x2B2D30FFu, BD_END);
	desk_logwin = logwin;
	bd_create(logwin, BD_LIST, BD_GROW_I, 1,
		BD_LABEL_S, "* connected to Aardwolf\n* MOTD received\n"
		"> look\nYou are in a small room.\n> north\nYou cannot go that way.",
		BD_ON_CLICK_F, on_btn, BD_ON_CLICK_P, (void *)"log line", BD_END);

	/* Terminal: a real BD_TERMINAL (libvt) in a floating frame, minimizable to
	 * the dock like the others */
	bd_id termwin = bd_create(cv, BD_FRAME, BD_LABEL_S, "Terminal",
		BD_PREF_W_I, 360, BD_PREF_H_I, 170, BD_X_I, 300, BD_Y_I, 150,
		BD_BG_C, 0x101418FFu, BD_END);
	bd_id dterm = bd_terminal_create(termwin, BD_GROW_I, 1, BD_END);
	bd_terminal_write(dterm,
		"\x1b[1;32mAardwolf\x1b[0m MUD  --  \x1b[36mterminal window\x1b[0m\r\n"
		"> \x1b[1mnorth\x1b[0m\r\n"
		"You walk north into a torch-lit hall.\r\n"
		"\x1b[33mA guard\x1b[0m eyes you warily.\r\n> ", -1);

	/* Settings: a dialog window whose toggle switches the desktop background
	 * between the solid color and the GLES wallpaper */
	bd_id setwin = bd_create(cv, BD_FRAME, BD_LABEL_S, "Settings",
		BD_PREF_W_I, 214, BD_PREF_H_I, 92, BD_X_I, 470, BD_Y_I, 300,
		BD_LAYOUT_I, BD_LAYOUT_COL, BD_PAD_I, 10, BD_GAP_I, 8,
		BD_BG_C, 0x2B2D30FFu, BD_END);
	bd_create(setwin, BD_LABEL, BD_LABEL_S, "Desktop background",
		BD_PREF_H_I, 16, BD_FG_C, 0xDCE3EAFFu, BD_BG_C, 0u, BD_END);
	bd_id setrow = bd_create(setwin, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_ROW,
		BD_PREF_H_I, 28, BD_GAP_I, 8, BD_END);
	bd_toggle_create(setrow, &(bd_toggle_desc){ .on = 0, .cb = on_wallpaper },
		BD_PREF_W_I, 52, BD_END);
	bd_create(setrow, BD_LABEL, BD_LABEL_S, "GLES wallpaper", BD_GROW_I, 1,
		BD_FG_C, 0xB8C0C8FFu, BD_BG_C, 0u, BD_END);

	/* ---- button bar with a horizontal slider ---- */
	bd_id bar = bd_create(frame, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_ROW, BD_PREF_H_I, 30,
		BD_PAD_I, 4, BD_GAP_I, 4, BD_END);
	bd_create(bar, BD_BUTTON, BD_LABEL_S, "New Window", BD_PREF_W_I, 110,
		BD_ON_CLICK_F, on_new_window, BD_END);
	bd_create(bar, BD_BUTTON, BD_LABEL_S, "Quit", BD_PREF_W_I, 90,
		BD_ON_CLICK_F, on_quit, BD_END);
	bd_slider_create(bar, &(bd_slider_desc){ .orient = BD_HORIZONTAL,
		.value = 0.5f, .cb = on_slider, .arg = (void *)"Volume" },
		BD_GROW_I, 1, BD_END);

	/* ---- status bar (the event readout) ---- */
	status = bd_create(frame, BD_LABEL, BD_LABEL_S, "Ready",
		BD_PREF_H_I, 20, BD_FG_C, 0xAAAAAAFFu, BD_BG_C, 0x3C3F41FFu,
		BD_PAD_I, 4, BD_END);
}

int
main(void)
{
	if (win_open("birdie-gui widget gallery", 1024, 928) != 0) {
		fprintf(stderr, "widget_test: cannot open window\n");
		return 1;
	}

	/* Load GL function pointers from eglGetProcAddress. Required for Windows
	 * and portable on Linux. Fails gracefully if the loader is disabled. */
	if (bd_gles_load_gl((void *(*)(const char *))eglGetProcAddress) != 0) {
		fprintf(stderr, "widget_test: GL function loader failed\n");
		win_close();
		return 1;
	}

	bd_gui_init(&bd_backend_gles, NULL);
	build_ui();
	if (getenv("GALLERY_AUTOTAB"))    /* select a tab by index for a shot */
		bd_tabview_select(tab_view, atoi(getenv("GALLERY_AUTOTAB")));
	if (getenv("GALLERY_AUTODESK")) { /* open the Desktop (MDI) tab for a shot */
		bd_tabview_select(tab_view, desktop_tab);
		if (getenv("GALLERY_AUTOMIN"))   /* minimize a frame to show the dock */
			bd_window_minimize(desk_logwin);
		if (getenv("GALLERY_AUTOWALL"))  /* turn the GLES wallpaper on for a shot */
			bd_managed_canvas_set_backdrop(desk_canvas, wall_tex, wall_fx);
	}
	if (getenv("GALLERY_AUTODLG"))   /* open a second window for testing */
		on_new_window(BD_NONE, NULL);
	if (getenv("GALLERY_AUTONOTICE"))   /* show a modal notice for a shot */
		bd_notice_open("Disconnect from Aardwolf?", "Disconnect\nCancel",
			on_quit_confirm, NULL);
	if (getenv("GALLERY_AUTODRAW")) {   /* inject pen strokes to show ink */
		bd_gui_layout(win_width(), win_height());
		int cx, cy, cw, ch;
		bd_widget_rect(sketch, &cx, &cy, &cw, &ch);
		/* a pressure-ramped sine: width swells from light to heavy */
		bd_gui_event(&(bd_event){ .type=BD_EV_PEN_DOWN, .x=cx+12,
			.y=cy+ch/2, .pressure=0.1f, .pen_flags=BD_PEN_INRANGE });
		for (int i = 1; i <= 40; i++) {
			float t = (float)i / 40.0f;
			int px = cx + 12 + (int)(t * (cw - 24));
			int py = cy + ch/2 - (int)(28.0 * sin(t * 6.2831853));
			bd_gui_event(&(bd_event){ .type=BD_EV_PEN_MOVE, .x=px,
				.y=py, .pressure=0.1f + 0.9f*t,
				.pen_flags=BD_PEN_INRANGE });
		}
		bd_gui_event(&(bd_event){ .type=BD_EV_PEN_UP,
			.x=cx+cw-12, .y=cy+ch/2, .pen_flags=BD_PEN_INRANGE });
		/* a short red barrel-button stroke */
		bd_gui_event(&(bd_event){ .type=BD_EV_PEN_DOWN, .x=cx+30, .y=cy+20,
			.pressure=0.9f, .pen_flags=BD_PEN_INRANGE|BD_PEN_BARREL });
		bd_gui_event(&(bd_event){ .type=BD_EV_PEN_MOVE, .x=cx+cw-30,
			.y=cy+20, .pressure=0.9f,
			.pen_flags=BD_PEN_INRANGE|BD_PEN_BARREL });
		bd_gui_event(&(bd_event){ .type=BD_EV_PEN_UP, .x=cx+cw-30, .y=cy+20,
			.pen_flags=BD_PEN_INRANGE|BD_PEN_BARREL });
	}

	while (running) {
		win_event wev;
		while (win_poll(&wev)) {
			if (wev.type == WIN_EV_CLOSE) {
				/* closing the primary quits; a secondary window
				 * just destroys its frame */
				if (wev.window == 1)
					running = 0;
				else
					bd_destroy(bd_frame_for_window(wev.window));
			} else if (wev.type == WIN_EV_KEY_DOWN
			    && wev.key == WIN_KEY_ESCAPE) {
				running = 0;
			}

			bd_event bev;
			if (bd_event_from_win(&wev, &bev))
				bd_gui_event(&bev);
		}

		/* mirror the focus state onto the FOCUS lamp: green focused,
		 * amber not (state 2 vs 1 of its "amber green" list) */
		int focused = bd_gui_focused();
		bd_indicator_set(focus_led, focused ? 2 : 1);

		/* drive the Meters tab from a clock so ballistics, peak-hold, and the
		 * liquid fills animate (a real app sets these from live data) */
		{
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);
			double t = ts.tv_sec + ts.tv_nsec * 1e-9;
			bd_meter_set(m_vu,  0.5 + 0.45 * sin(t * 2.1));
			bd_meter_set(m_eye, 0.5 + 0.5  * sin(t * 0.7));
			bd_meter_set(m_load, fabs(sin(t * 3.3)) * 0.5 +
			    0.5 * fabs(sin(t * 0.9)));
			bd_meter_set(m_sig, 0.5 + 0.5 * sin(t * 1.3 + 1.0));
			bd_meter_set(m_hp,  0.5 + 0.5 * sin(t * 0.35));      /* drains/heals */
			bd_meter_set(m_mp,  0.5 + 0.5 * sin(t * 0.5 + 2.0));
			bd_progress_set(m_hpbar, 0.5 + 0.5 * sin(t * 0.35)); /* tube twins */
			bd_progress_set(m_mpbar, 0.5 + 0.5 * sin(t * 0.5 + 2.0));
			bd_progress_set(m_prog, fmod(t, 4.0) / 4.0);          /* 0..100% ramp */

			static double last_push;   /* sample the chart at ~10 Hz */
			if (t - last_push > 0.1) {
				last_push = t;
				float row[3] = {
					(float)(45 + 35 * sin(t * 0.7) + 10 * sin(t * 5.0)),   /* CPU % */
					(float)(80 + 55 * sin(t * 0.4 + 1.0) + 18 * fabs(sin(t * 3))), /* ms */
					(float)(300 + 120 * sin(t * 0.25)),                    /* MB */
				};
				bd_chart_push_row(m_chart, row, 3);
			}
		}

		/* the GLES backend is multi_window, so bd_gui_render() makes each
		 * window current and presents it; no win_swap() here */
		bd_gui_layout(win_width(), win_height());
		bd_gui_render();

		/* When no window holds focus, back off to ~15 fps: the gallery has
		 * nothing to animate in the background, so this frees the CPU/GPU.
		 * A real app would also pause idle animation and drop quality here. */
		if (!focused)
			nanosleep(&(struct timespec){ .tv_nsec = 66 * 1000 * 1000 },
			    NULL);
	}

	bd_gui_cleanup();
	win_close();
	return 0;
}
