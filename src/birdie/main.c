#include "widget.h"
#include "bd_widget_vt.h"
#include "bd_backend_ludica.h"
#include "bd_net.h"
#include "ludica.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static bd_id status_label;
static bd_id terminal;
static bd_id input_line;
static bd_net *net;

static void
net_on_data(const char *data, int len, void *arg)
{
	(void)arg;
	bd_terminal_write(terminal, data, len);
}

static void
net_on_state(bd_net_state state, const char *msg, void *arg)
{
	(void)arg;
	static const char *names[] = {
		"Disconnected", "Connecting...", "Connected",
		"Disconnected", "Error"
	};
	/* BD_LABEL_S borrows the string (it is not copied), so this must
	 * outlive the call; a static buffer suffices on the UI thread. */
	static char line[160];
	const char *name = names[state];
	int n = snprintf(line, sizeof line, "Status: %s", name);
	if (msg && (state == BD_NET_ERROR || state == BD_NET_CLOSED))
		snprintf(line + n, sizeof line - (size_t)n, " (%s)", msg);
	bd_set(status_label, BD_LABEL_S, line, BD_END);

	if (state == BD_NET_CONNECTED)
		bd_terminal_write(terminal, "\033[32m*** connected\033[0m\r\n", -1);
	else if (state == BD_NET_CLOSED || state == BD_NET_ERROR)
		bd_terminal_write(terminal, "\033[31m*** disconnected\033[0m\r\n", -1);
}

/* Server took over echo (telnet ECHO): mask the command line for password
 * entry, and restore it when the server releases echo. */
static void
net_on_echo(int suppress, void *arg)
{
	(void)arg;
	bd_set(input_line, BD_PASSWORD_B, suppress, BD_END);
}

static void
on_connect(bd_id id, void *arg)
{
	(void)id;
	(void)arg;
	const char *host = getenv("BIRDIE_HOST");
	const char *port = getenv("BIRDIE_PORT");
	if (!host)
		host = "localhost";
	if (!port)
		port = "4000";
	int tls = getenv("BIRDIE_TLS") != NULL;

	char line[160];
	snprintf(line, sizeof line, "\033[33m*** connecting to %s:%s%s\033[0m\r\n",
	    host, port, tls ? " (TLS)" : "");
	bd_terminal_write(terminal, line, -1);
	bd_net_connect(net, host, port, tls);
}

static void
on_disconnect(bd_id id, void *arg)
{
	(void)id;
	(void)arg;
	bd_net_close(net);
}

/* The input line fires its click handler on Enter, before clearing, so the
 * submitted command is still readable here. */
static void
on_submit(bd_id id, void *arg)
{
	(void)arg;
	const char *cmd = bd_get_s(id, BD_LABEL_S);
	if (bd_net_state_get(net) != BD_NET_CONNECTED)
		return;
	if (cmd && cmd[0])
		bd_net_send(net, cmd, (int)strlen(cmd));
	bd_net_send(net, "\r\n", 2);
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
	bd_gui_init(&bd_backend_ludica, NULL);

	net = bd_net_new(net_on_data, net_on_state, NULL);
	bd_net_set_echo_cb(net, net_on_echo);
	bd_net_set_termtype(net, "birdie/0.0");
	bd_net_set_winsize(net, 80, 24);   /* matches the terminal grid */

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
	bd_create(m_sess, BD_BUTTON, BD_LABEL_S, "Disconnect",
		BD_ON_CLICK_F, on_disconnect, BD_END);

	/* terminal output */
	terminal = bd_terminal_create(frame,
		BD_GROW_I, 1,
		BD_END);

	bd_terminal_write(terminal,
		"\033[1mbirdie v0.0\033[0m\r\n"
		"Terminal output area ready.\r\n",
		-1);

	/* command input */
	input_line = bd_create(frame, BD_INPUT_LINE,
		BD_PREF_H_I, 24,
		BD_PAD_I, 4,
		BD_ON_CLICK_F, on_submit,
		BD_END);
	(void)input_line;

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

	/* Optionally connect on startup (handy for testing/automation; a
	 * profile-driven autoconnect lives in the roadmap). */
	if (getenv("BIRDIE_AUTOCONNECT"))
		on_connect(BD_NONE, NULL);
}

static void
frame(float dt)
{
	(void)dt;
	bd_net_poll(net);
	bd_gui_layout(lud_width(), lud_height());
	bd_gui_render();
}

static void
cleanup(void)
{
	bd_net_free(net);
	net = NULL;
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
