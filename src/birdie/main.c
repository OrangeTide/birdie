#include "widget.h"
#include "bd_widget_vt.h"
#include "bd_widget_value.h"
#include "bd_backend_ludica.h"
#include "ludica.h"
#include <stddef.h>
#include <stdio.h>

static bd_id status_label;
static bd_id terminal;

static void
on_connect(bd_id id, void *arg)
{
	(void)id;
	(void)arg;
	bd_set(status_label, BD_LABEL_S, "Status: Connected", BD_END);
}

static void
on_quit(bd_id id, void *arg)
{
	(void)id;
	(void)arg;
	lud_quit();
}

static void
on_slider(bd_id id, void *arg, float t)
{
	(void)id;
	(void)arg;
	static char buf[32];
	snprintf(buf, sizeof buf, "Volume: %d%%", (int)(t * 100.0f + 0.5f));
	bd_set(status_label, BD_LABEL_S, buf, BD_END);
}

static void
on_knob(bd_id id, void *arg, float v)
{
	(void)id;
	(void)arg;
	static char buf[32];
	snprintf(buf, sizeof buf, "Value: %g", v);
	bd_set(status_label, BD_LABEL_S, buf, BD_END);
}

static void
on_toggle(bd_id id, void *arg, int on)
{
	(void)id;
	(void)arg;
	bd_set(status_label, BD_LABEL_S, on ? "Switch: ON" : "Switch: OFF",
		BD_END);
}

static void
on_wheel(bd_id id, void *arg, float d)
{
	(void)id;
	(void)arg;
	static float acc;
	static char buf[32];
	acc += d;
	snprintf(buf, sizeof buf, "Spin: %+.2f", acc);
	bd_set(status_label, BD_LABEL_S, buf, BD_END);
}

static void
on_xy(bd_id id, void *arg, float x, float y)
{
	(void)id;
	(void)arg;
	static char buf[32];
	snprintf(buf, sizeof buf, "X-Y: %.2f, %.2f", x, y);
	bd_set(status_label, BD_LABEL_S, buf, BD_END);
}

static int
on_event(const lud_event_t *ev)
{
	bd_event bev;
	if (bd_event_from_lud(ev, &bev) && bd_gui_event(&bev))
		return 1;
	if (ev->type == LUD_EV_KEY_DOWN &&
	    ev->key.keycode == LUD_KEY_ESCAPE) {
		lud_quit();
		return 1;
	}
	return 0;
}

static void
init(void)
{
	bd_gui_init(&bd_backend_ludica, NULL);

	bd_id frame = bd_create(BD_NONE, BD_FRAME,
		BD_LABEL_S, "Birdie",
		BD_LAYOUT_I, BD_LAYOUT_COL,
		BD_END);

	/* menu bar */
	bd_id menu = bd_create(frame, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_ROW,
		BD_PREF_H_I, 20,
		BD_BG_C, 0x3C3F41FFu,
		BD_PAD_I, 2,
		BD_GAP_I, 16,
		BD_END);

	bd_id m_file = bd_create(menu, BD_MENU, BD_LABEL_S, "File", BD_END);
	bd_create(m_file, BD_BUTTON, BD_LABEL_S, "New", BD_END);
	bd_create(m_file, BD_BUTTON, BD_LABEL_S, "Open...", BD_END);
	bd_create(m_file, BD_BUTTON, BD_LABEL_S, "Save", BD_END);
	bd_create(m_file, BD_BUTTON, BD_LABEL_S, "Quit",
		BD_ON_CLICK_F, on_quit, BD_END);

	bd_id m_edit = bd_create(menu, BD_MENU, BD_LABEL_S, "Edit", BD_END);
	bd_create(m_edit, BD_BUTTON, BD_LABEL_S, "Copy", BD_END);
	bd_create(m_edit, BD_BUTTON, BD_LABEL_S, "Paste", BD_END);

	bd_id m_sess = bd_create(menu, BD_MENU, BD_LABEL_S, "Session", BD_END);
	bd_create(m_sess, BD_BUTTON, BD_LABEL_S, "Connect",
		BD_ON_CLICK_F, on_connect, BD_END);
	bd_create(m_sess, BD_BUTTON, BD_LABEL_S, "Disconnect", BD_END);

	/* terminal output */
	terminal = bd_terminal_create(frame,
		BD_GROW_I, 1,
		BD_END);

	bd_terminal_write(terminal,
		"\033[1mbirdie v0.0\033[0m\r\n"
		"Terminal output area ready.\r\n",
		-1);

	/* command input */
	bd_create(frame, BD_INPUT_LINE,
		BD_PREF_H_I, 24,
		BD_PAD_I, 4,
		BD_END);

	/* knob row: dots, balance, hex MIDI labels, and a 7-way rotary switch */
	bd_id krow = bd_create(frame, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_ROW,
		BD_PREF_H_I, 96,
		BD_PAD_I, 14,
		BD_GAP_I, 18,
		BD_BG_C, 0x313335FFu,
		BD_END);
	bd_knob_create(krow, &(bd_knob_desc){
		.min = 0, .max = 1, .value = 0.3f,
		.dial = BD_DIAL_DOTS, .cb = on_knob }, BD_PREF_W_I, 64, BD_END);
	bd_knob_create(krow, &(bd_knob_desc){
		.min = -1, .max = 1, .value = 0,
		.dial = BD_DIAL_BALANCE, .cb = on_knob }, BD_PREF_W_I, 64, BD_END);
	bd_knob_create(krow, &(bd_knob_desc){
		.min = 0, .max = 127, .value = 64,
		.dial = BD_DIAL_LABELS, .hex = 1, .cb = on_knob },
		BD_PREF_W_I, 120, BD_END);
	bd_knob_create(krow, &(bd_knob_desc){
		.min = 0, .max = 6, .step = 1, .value = 2,
		.dial = BD_DIAL_DOTS, .cb = on_knob }, BD_PREF_W_I, 64, BD_END);
	bd_toggle_create(krow, 1, on_toggle, NULL, BD_PREF_W_I, 56, BD_END);
	bd_wheel_create(krow, BD_VERTICAL, on_wheel, NULL, BD_PREF_W_I, 30, BD_END);
	bd_knob_create(krow, &(bd_knob_desc){
		.relative = 1, .dimples = 3, .cb = on_wheel },  /* jog dial */
		BD_PREF_W_I, 84, BD_END);
	bd_xypad_create(krow, &(bd_xypad_desc){
		.shape = BD_XY_CIRCLE, .spring = 1, .x = 0.5f, .y = 0.5f,
		.cb = on_xy }, BD_PREF_W_I, 76, BD_END);

	/* button bar */
	bd_id bar = bd_create(frame, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_ROW,
		BD_PREF_H_I, 28,
		BD_PAD_I, 4,
		BD_GAP_I, 4,
		BD_END);

	bd_create(bar, BD_BUTTON,
		BD_LABEL_S, "Connect",
		BD_PREF_W_I, 80,
		BD_ON_CLICK_F, on_connect,
		BD_END);

	bd_create(bar, BD_BUTTON,
		BD_LABEL_S, "Quit",
		BD_PREF_W_I, 80,
		BD_ON_CLICK_F, on_quit,
		BD_END);

	/* a horizontal slider fills the rest of the bar */
	bd_slider_create(bar, BD_HORIZONTAL, 0.5f, on_slider, NULL,
		BD_GROW_I, 1,
		BD_END);

	/* status bar */
	status_label = bd_create(frame, BD_LABEL,
		BD_LABEL_S, "Status: Disconnected",
		BD_PREF_H_I, 20,
		BD_FG_C, 0xAAAAAAFFu,
		BD_BG_C, 0x3C3F41FFu,
		BD_PAD_I, 4,
		BD_END);
}

static void
frame(float dt)
{
	(void)dt;
	bd_gui_layout(lud_width(), lud_height());
	bd_gui_render();
}

static void
cleanup(void)
{
	bd_gui_cleanup();
}

int
main(int argc, char **argv)
{
	return lud_run(&(lud_desc_t){
		.app_name  = "birdie",
		.width     = 800,
		.height    = 500,
		.resizable = 1,
		.gles_version = 3,   /* toolkit shaders are #version 300 es */
		.argc      = argc,
		.argv      = argv,
		.init      = init,
		.frame     = frame,
		.cleanup   = cleanup,
		.event     = on_event,
	});
}
