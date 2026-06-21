#include "widget.h"
#include "bd_widget_vt.h"
#include "bd_widget_table.h"
#include "bd_backend_ludica.h"
#include "bd_session.h"
#include "bd_profile.h"
#include "bd_telopt.h"
#include "ludica.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static bd_id status_label;              /* connection status, in the sidebar */
static bd_id mud_label;                 /* current MUD name, in the sidebar */
static bd_id terminal;
static bd_id input_line;
static bd_id mudlist;                   /* BD_TABLE inside the connect dialog */
static bd_id connect_dialog;            /* the modal MUD picker */
static bd_profiles *profiles;
static const bd_profile *active;        /* the profile we connect with */
static bd_session *session;

/* ---- MUD-list table model (reads the profile store) ---- */

static int
mudlist_rows(void *ctx)
{
	(void)ctx;
	return bd_profiles_count(profiles);
}

static const char *
mudlist_cell(void *ctx, int row, int col)
{
	bd_profile *p = bd_profiles_at(profiles, row);
	(void)ctx;
	if (!p)
		return "";
	switch (col) {
	case 0: return bd_profile_get(p, "name");
	case 1: return bd_profile_get(p, "host");
	case 2: return bd_profile_get(p, "port");
	}
	return "";
}

/* Single front-end sink for everything a session produces (UI thread). */
static void
on_session_event(bd_session *s, const bd_session_event *ev, void *arg)
{
	(void)s;
	(void)arg;
	switch (ev->kind) {
	case BD_SESSION_DATA:
		bd_terminal_write(terminal, ev->data, ev->len);
		break;
	case BD_SESSION_STATE: {
		static const char *names[] = {
			"Disconnected", "Connecting...", "Connected",
			"Disconnected", "Error"
		};
		/* BD_LABEL_S borrows the string (not copied), so it must
		 * outlive the call; a static buffer suffices on the UI thread. */
		static char line[160];
		int n = snprintf(line, sizeof line, "Status: %s",
		    names[ev->state]);
		if (ev->detail && (ev->state == BD_NET_ERROR ||
		    ev->state == BD_NET_CLOSED))
			snprintf(line + n, sizeof line - (size_t)n, " (%s)",
			    ev->detail);
		bd_set(status_label, BD_LABEL_S, line, BD_END);
		if (ev->state == BD_NET_CONNECTED)
			bd_terminal_write(terminal,
			    "\033[32m*** connected\033[0m\r\n", -1);
		else if (ev->state == BD_NET_CLOSED || ev->state == BD_NET_ERROR)
			bd_terminal_write(terminal,
			    "\033[31m*** disconnected\033[0m\r\n", -1);
		break;
	}
	case BD_SESSION_ECHO:
		/* server took over echo: mask the command line for a password */
		bd_set(input_line, BD_PASSWORD_B, ev->echo_suppress, BD_END);
		break;
	case BD_SESSION_PACKAGE:
		/* the trigger/scripting layer is the real consumer (not wired
		 * yet); log so packages are observable */
		fprintf(stdout, "[%s] %s %s\n",
		    ev->proto == BD_TELOPT_GMCP ? "GMCP" : "MSDP",
		    ev->name, ev->json);
		fflush(stdout);
		break;
	case BD_SESSION_PROMPT:
	default:
		break;
	}
}

/* Point the session at `p`, recreating it if the profile changed. The session
 * borrows the profile, so switching MUDs means a fresh session (and net
 * thread); reconnecting to the same one reuses it. */
static void
rebind_session(const bd_profile *p)
{
	if (p == active && session)
		return;
	if (session)
		bd_session_free(session);
	active = p;
	session = bd_session_new(active);
	bd_session_on_event(session, on_session_event, NULL);
	bd_session_set_winsize(session, 80, 24);
}

/* Sidebar label showing the current MUD (borrowed string, so keep it in a
 * static buffer for the label to point at). */
static void
set_mud_label(const bd_profile *p)
{
	static char buf[80];
	const char *name = p ? bd_profile_get(p, "name") : NULL;
	snprintf(buf, sizeof buf, "MUD: %s", name ? name : "(none)");
	bd_set(mud_label, BD_LABEL_S, buf, BD_END);
}

static void
connect_profile(const bd_profile *p)
{
	const char *host, *port;
	int tls;
	char line[200];

	if (!p) {
		bd_terminal_write(terminal,
		    "\033[31m*** no MUD selected\033[0m\r\n", -1);
		return;
	}
	rebind_session(p);
	set_mud_label(p);

	host = bd_profile_get(p, "host");
	port = bd_profile_get(p, "port");
	tls = bd_profile_get(p, "tls") &&
	    strcmp(bd_profile_get(p, "tls"), "no") != 0;
	snprintf(line, sizeof line,
	    "\033[33m*** connecting to %s (%s:%s%s)\033[0m\r\n",
	    bd_profile_get(p, "name") ? bd_profile_get(p, "name") : "?",
	    host ? host : "?", port ? port : "?", tls ? " TLS" : "");
	bd_terminal_write(terminal, line, -1);
	if (bd_session_connect(session) < 0)
		bd_terminal_write(terminal,
		    "\033[31m*** profile has no host/port\033[0m\r\n", -1);
}

/* The selected MUD-list row, or the active profile if nothing is selected. */
static const bd_profile *
selected_profile(void)
{
	int row = bd_table_current(mudlist);
	if (row >= 0)
		return bd_profiles_at(profiles, row);
	return active;
}

/* "Session > Connect..." opens the modal MUD picker. */
static void
on_open_connect(bd_id id, void *arg)
{
	(void)id;
	(void)arg;
	bd_modal_open(connect_dialog);
}

/* Connect button inside the dialog: connect the selected MUD, then close. */
static void
on_dialog_connect(bd_id id, void *arg)
{
	(void)id;
	(void)arg;
	const bd_profile *p = selected_profile();
	bd_modal_close(connect_dialog);
	connect_profile(p);
}

static void
on_dialog_cancel(bd_id id, void *arg)
{
	(void)id;
	(void)arg;
	bd_modal_close(connect_dialog);
}

/* Double-click / Enter on a dialog row connects to it and closes the dialog. */
static void
mudlist_activate(bd_id w, int row, void *ctx)
{
	const bd_profile *p = bd_profiles_at(profiles, row);
	(void)w;
	(void)ctx;
	bd_modal_close(connect_dialog);
	connect_profile(p);
}

static void
on_disconnect(bd_id id, void *arg)
{
	(void)id;
	(void)arg;
	bd_session_disconnect(session);
}

/* The input line fires its click handler on Enter, before clearing, so the
 * submitted command is still readable here. */
static void
on_submit(bd_id id, void *arg)
{
	(void)arg;
	const char *cmd = bd_get_s(id, BD_LABEL_S);
	if (bd_session_state(session) != BD_NET_CONNECTED)
		return;
	bd_session_send_line(session, cmd ? cmd : "");
}

/* Add a profile (name/host/port/tls) to the store if absent. */
static void
seed_profile(const char *name, const char *host, const char *port,
             const char *tls)
{
	bd_profile *p = bd_profiles_add(profiles, name);
	if (!p)
		return;
	bd_profile_set(p, "host", host);
	bd_profile_set(p, "port", port);
	bd_profile_set(p, "tls", tls);
}

/* Populate the profile store and return the profile to start on:
 *  - a CSV at BIRDIE_PROFILES is loaded if present;
 *  - otherwise a few well-known MUDs are seeded as examples;
 *  - a "localhost" entry (from BIRDIE_HOST/PORT/TLS) is always added for
 *    the loopback smoke tests.
 * The initial profile is BIRDIE_PROFILE if named, else the first row. */
static const bd_profile *
load_profiles(void)
{
	const char *path = getenv("BIRDIE_PROFILES");
	const char *want = getenv("BIRDIE_PROFILE");
	const char *host = getenv("BIRDIE_HOST");
	const char *port = getenv("BIRDIE_PORT");
	bd_profile *p;

	if (path && bd_profiles_load(profiles, path) == 0) {
		/* loaded a real list */
	} else {
		seed_profile("Aardwolf", "aardmud.org", "23", "no");
		seed_profile("Discworld", "discworld.starturtle.net", "4242", "no");
		seed_profile("Achaea", "achaea.com", "23", "no");
		seed_profile("BatMUD", "batmud.bat.org", "23", "no");
	}
	seed_profile("localhost", host ? host : "localhost",
	    port ? port : "4000", getenv("BIRDIE_TLS") ? "yes" : "no");

	/* If the loopback env knobs are set (smoke tests / a custom target),
	 * start on the localhost entry; otherwise start on the first row. */
	if (!want && (host || port || getenv("BIRDIE_TLS") ||
	    getenv("BIRDIE_AUTOCONNECT")))
		want = "localhost";
	if (want) {
		p = bd_profiles_find(profiles, want);
		if (p)
			return p;
	}
	return bd_profiles_at(profiles, 0);
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

	profiles = bd_profiles_new();
	active = load_profiles();
	session = bd_session_new(active);
	bd_session_on_event(session, on_session_event, NULL);
	bd_session_set_winsize(session, 80, 24);   /* matches the terminal grid */

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
	bd_create(m_sess, BD_BUTTON, BD_LABEL_S, "Connect...",
		BD_ON_CLICK_F, on_open_connect, BD_END);
	bd_create(m_sess, BD_BUTTON, BD_LABEL_S, "Disconnect",
		BD_ON_CLICK_F, on_disconnect, BD_END);

	/* body: a session-context sidebar on the left, the session on the right */
	bd_id body = bd_create(frame, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_ROW, BD_GROW_I, 1,
		BD_PAD_I, 4, BD_GAP_I, 6, BD_END);

	/* left sidebar: state about the current session (and, later, vitals
	 * scraped from GMCP/MSDP -- the placeholders mark where that lands) */
	bd_id side = bd_create(body, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_COL, BD_PREF_W_I, 190,
		BD_BG_C, 0x313335FFu, BD_PAD_I, 8, BD_GAP_I, 6, BD_END);
	bd_create(side, BD_LABEL, BD_LABEL_S, "Session",
		BD_PREF_H_I, 18, BD_FG_C, 0xFFFFFFFFu, BD_END);
	mud_label = bd_create(side, BD_LABEL, BD_LABEL_S, "MUD: (none)",
		BD_PREF_H_I, 18, BD_END);
	status_label = bd_create(side, BD_LABEL, BD_LABEL_S, "Status: Disconnected",
		BD_PREF_H_I, 18, BD_END);
	bd_create(side, BD_LABEL, BD_LABEL_S, "Vitals",
		BD_PREF_H_I, 18, BD_FG_C, 0xFFFFFFFFu, BD_END);
	/* placeholders: a future GMCP Char.Vitals handler fills these in */
	bd_create(side, BD_LABEL, BD_LABEL_S, "HP: --", BD_PREF_H_I, 16, BD_END);
	bd_create(side, BD_LABEL, BD_LABEL_S, "MP: --", BD_PREF_H_I, 16, BD_END);
	bd_create(side, BD_LABEL, BD_LABEL_S, "", BD_GROW_I, 1, BD_END); /* filler */

	/* right: terminal output + command input */
	bd_id right = bd_create(body, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_COL, BD_GROW_I, 1, BD_GAP_I, 4, BD_END);
	terminal = bd_terminal_create(right, BD_GROW_I, 1, BD_END);
	bd_terminal_write(terminal,
		"\033[1mbirdie v0.0\033[0m\r\n"
		"Session > Connect... to choose a MUD.\r\n",
		-1);
	input_line = bd_create(right, BD_INPUT_LINE,
		BD_PREF_H_I, 24, BD_PAD_I, 4, BD_ON_CLICK_F, on_submit, BD_END);

	/* the modal connect dialog: a detached panel hosting the MUD-list table */
	connect_dialog = bd_create(BD_NONE, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_COL, BD_PREF_W_I, 460, BD_PREF_H_I, 340,
		BD_BG_C, 0x313335FFu, BD_PAD_I, 10, BD_GAP_I, 8, BD_END);
	bd_create(connect_dialog, BD_LABEL,
		BD_LABEL_S, "Connect to a MUD (double-click a row):",
		BD_PREF_H_I, 18, BD_FG_C, 0xFFFFFFFFu, BD_END);
	static const bd_table_column mcols[] = {
		{ "MUD",  0,   BD_TABLE_LEFT,  0 },
		{ "Host", 170, BD_TABLE_LEFT,  0 },
		{ "Port", 50,  BD_TABLE_RIGHT, BD_TABLE_COL_NUMERIC },
	};
	mudlist = bd_table_create(connect_dialog, mcols, 3,
		&(bd_table_model){ mudlist_rows, mudlist_cell, NULL },
		&(bd_table_cb){ .activate = mudlist_activate },
		BD_GROW_I, 1, BD_END);
	bd_id dbtn = bd_create(connect_dialog, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_ROW, BD_PREF_H_I, 30, BD_GAP_I, 6, BD_END);
	bd_create(dbtn, BD_BUTTON, BD_LABEL_S, "Connect", BD_GROW_I, 1,
		BD_ON_CLICK_F, on_dialog_connect, BD_END);
	bd_create(dbtn, BD_BUTTON, BD_LABEL_S, "Cancel", BD_GROW_I, 1,
		BD_ON_CLICK_F, on_dialog_cancel, BD_END);

	/* Optionally connect on startup (testing/automation). */
	if (getenv("BIRDIE_AUTOCONNECT"))
		connect_profile(active);
}

static void
frame(float dt)
{
	(void)dt;
	bd_session_drain(session);
	bd_gui_layout(lud_width(), lud_height());
	bd_gui_render();
}

static void
cleanup(void)
{
	bd_session_free(session);       /* before the profile store it borrows */
	session = NULL;
	bd_profiles_free(profiles);
	profiles = NULL;
	bd_gui_cleanup();
}

int
main(int argc, char **argv)
{
	return lud_run(&(lud_desc_t){
		.app_name  = "birdie",
		.width     = 920,
		.height    = 560,
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
