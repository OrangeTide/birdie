/*
 * widget_test.c — birdie-gui widget gallery.
 *
 * A standalone sample that exhibits and exercises every working widget on the
 * raw-GLES backend (bd_backend_gles + window.h), independent of ludica. birdie
 * the MUD client runs on ludica; this gallery runs on GLES, so both backends
 * stay exercised. It also doubles as the place to grow interactive widget
 * tests as new widgets land.
 *
 * Run from the repo root so the default BD_ASSET_* paths resolve.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "widget.h"
#include "bd_widget_vt.h"
#include "bd_widget_value.h"
#include "bd_widget_explorer.h"
#include "bd_widget_editor.h"
#include "bd_backend_gles.h"
#include "window.h"

#include <stdio.h>
#include <stdlib.h>

static bd_id status;
static bd_id term;
static int   running = 1;

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

static void
on_quit(bd_id id, void *arg)
{
	(void)id; (void)arg;
	running = 0;
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

	/* ---- body: terminal/input on the left, widget exhibits on the right ---- */
	bd_id body = bd_create(frame, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_ROW, BD_GROW_I, 1,
		BD_PAD_I, 6, BD_GAP_I, 6, BD_END);

	bd_id left = bd_create(body, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_COL, BD_GROW_I, 1, BD_GAP_I, 4, BD_END);
	term = bd_terminal_create(left, BD_GROW_I, 1, BD_END);
	bd_terminal_write(term,
		"\033[1mbirdie-gui widget gallery\033[0m\r\n"
		"GLES backend. Interact with the widgets; events log here.\r\n", -1);
	bd_create(left, BD_INPUT_LINE, BD_PREF_H_I, 24, BD_PAD_I, 4, BD_END);

	bd_id right = bd_create(body, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_COL, BD_PREF_W_I, 420,
		BD_GAP_I, 6, BD_END);

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
	bd_toggle_create(srow, 1, on_toggle, NULL, BD_PREF_W_I, 56, BD_END);
	bd_wheel_create(srow, BD_VERTICAL, on_jog, NULL, BD_PREF_W_I, 30, BD_END);
	bd_wheel_create(srow, BD_HORIZONTAL, on_jog, NULL,
		BD_PREF_W_I, 80, BD_PREF_H_I, 30, BD_END);
	bd_knob_create(srow, &(bd_knob_desc){
		.relative = 1, .dimples = 3, .cb = on_jog }, /* endless jog dial */
		BD_PREF_W_I, 76, BD_END);

	/* X-Y pads: bounded square + spring-return joystick circle */
	bd_id xrow = section(right, "X-Y pads", BD_LAYOUT_ROW, 120);
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
	bd_id vrow = section(right, "Sliders", BD_LAYOUT_ROW, 150);
	bd_slider_create(vrow, BD_VERTICAL, 0.6f, on_slider, (void *)"Fader",
		BD_PREF_W_I, 28, BD_END);
	bd_id hwrap = bd_create(vrow, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_COL, BD_GROW_I, 1, BD_END);
	bd_slider_create(hwrap, BD_HORIZONTAL, 0.4f, on_slider, (void *)"Wet",
		BD_PREF_H_I, 24, BD_END);

	/* ---- button bar with a horizontal slider ---- */
	bd_id bar = bd_create(frame, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_ROW, BD_PREF_H_I, 30,
		BD_PAD_I, 4, BD_GAP_I, 4, BD_END);
	bd_create(bar, BD_BUTTON, BD_LABEL_S, "New Window", BD_PREF_W_I, 110,
		BD_ON_CLICK_F, on_new_window, BD_END);
	bd_create(bar, BD_BUTTON, BD_LABEL_S, "Quit", BD_PREF_W_I, 90,
		BD_ON_CLICK_F, on_quit, BD_END);
	bd_slider_create(bar, BD_HORIZONTAL, 0.5f, on_slider, (void *)"Volume",
		BD_GROW_I, 1, BD_END);

	/* ---- status bar (the event readout) ---- */
	status = bd_create(frame, BD_LABEL, BD_LABEL_S, "Ready",
		BD_PREF_H_I, 20, BD_FG_C, 0xAAAAAAFFu, BD_BG_C, 0x3C3F41FFu,
		BD_PAD_I, 4, BD_END);
}

int
main(void)
{
	if (win_open("birdie-gui widget gallery", 1024, 720) != 0) {
		fprintf(stderr, "widget_test: cannot open window\n");
		return 1;
	}

	bd_gui_init(&bd_backend_gles, NULL);
	build_ui();
	if (getenv("GALLERY_AUTODLG"))   /* open a second window for testing */
		on_new_window(BD_NONE, NULL);

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

		/* the GLES backend is multi_window, so bd_gui_render() makes each
		 * window current and presents it; no win_swap() here */
		bd_gui_layout(win_width(), win_height());
		bd_gui_render();
	}

	bd_gui_cleanup();
	win_close();
	return 0;
}
