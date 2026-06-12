#include "widget.h"
#include "bd_widget_vt.h"
#include "bd_backend_ludica.h"
#include "ludica.h"

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
	bd_gui_init(&bd_backend_ludica);

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
		.argc      = argc,
		.argv      = argv,
		.init      = init,
		.frame     = frame,
		.cleanup   = cleanup,
		.event     = on_event,
	});
}
