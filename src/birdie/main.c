#include "widget.h"
#include "bd_widget_vt.h"
#include "bd_widget_table.h"
#include "bd_widget_value.h"
#include "bd_widget_form.h"
#include "bd_widget_combo.h"
#include "bd_widget_colorpick.h"
#include "bd_dialog.h"
#include "bd_backend_ludica.h"
#include "bd_session.h"
#include "bd_profile.h"
#include "bd_telopt.h"
#include "bd_verb.h"
#include "bd_trigger.h"
#include "ludica.h"
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static bd_id status_label;              /* connection status, in the sidebar */
static bd_id mud_label;                 /* current MUD name, in the sidebar */
static bd_id hp_label;                  /* sidebar vitals, driven by GMCP */
static bd_id mp_label;
static bd_id terminal;
static bd_id input_line;
static bd_id mudlist;                   /* BD_TABLE inside the connect dialog */
static bd_dialog *connect_dlg;          /* the modal MUD picker */
static bd_dialog *edit_dlg;             /* add/edit a profile */
static bd_id edit_name, edit_host, edit_port, edit_tls;
static bd_id edit_reconnect, edit_termtype;
static bd_dialog *settings_dlg;         /* app-wide preferences */
static bd_id set_cols, set_rows, set_scheme;
static bd_dialog *triggers_dlg;         /* live trigger editor */
static bd_id trig_table;                /* list of the session's triggers */
static bd_id trig_type, trig_pattern, trig_body, trig_class, trig_pri, trig_stop;
static bd_id import_path;               /* CSV import file-path field */

/* export column-filter dialog */
static const char *const export_cols[] = { "host", "port", "tls", "encoding",
                                           "description", "url", "tags" };
#define N_EXPORT_COLS ((int)(sizeof export_cols / sizeof export_cols[0]))
static bd_dialog *export_dlg;
static bd_id exp_check[N_EXPORT_COLS];
static bd_id export_path;

/* import-collision dialog: resolves name clashes when merging an imported list */
enum { IMP_OVERWRITE, IMP_SKIP, IMP_RENAME };
static bd_dialog *import_dlg;
static bd_id imp_msg, imp_policy;
static bd_profiles *pending_import;     /* parsed rows awaiting a resolution */
static char imp_msg_buf[160];

/* colour picker dialog: pick a colour, read its hex + #highlight SGR string */
static bd_dialog *color_dlg;
static bd_id color_pick, color_hex, color_sgr;
static char editing_name[128];          /* profile being edited ("" = add new) */
static bd_profiles *profiles;
static const bd_profile *active;        /* the profile we connect with */
static bd_session *session;

/* ---- application settings (app-wide, persisted to <data_dir>/settings.csv) ----
 * Birdie's per-connection config lives in profiles; these are the few genuinely
 * app-wide preferences with a live effect: the terminal grid the session
 * advertises over NAWS, and the terminal color scheme. */

static const char *const scheme_ids[]    = { "default", "green", "amber" };
static const char *const scheme_labels[] = { "Default", "Green phosphor",
                                             "Amber" };
#define N_SCHEMES ((int)(sizeof scheme_ids / sizeof scheme_ids[0]))

static struct {
	int cols, rows;   /* terminal grid advertised over NAWS */
	int scheme;       /* index into scheme_ids */
} settings = { 80, 24, 0 };

/* The palette for a color scheme: start from the toolkit default and recolor
 * only the default fg/bg (the 16 ANSI entries stay, so colored MUD output is
 * unchanged). Scheme 0 is the plain default. */
static bd_palette
scheme_palette(int idx)
{
	bd_palette p = bd_palette_default();
	if (idx == 1) {                     /* green phosphor */
		p.default_fg = 0x33FF66FFu;
		p.default_bg = 0x001200FFu;
		p.bold_fg    = 0x99FFB0FFu;
	} else if (idx == 2) {              /* amber */
		p.default_fg = 0xFFB000FFu;
		p.default_bg = 0x1A0F00FFu;
		p.bold_fg    = 0xFFD070FFu;
	}
	return p;
}

/* Per-user data dir (defined below) and the profile-list CSV under it. */
static const char *data_dir(void);

static const char *
profiles_path(void)
{
	static char buf[600];
	const char *d = data_dir();
	if (!d)
		return NULL;
	snprintf(buf, sizeof buf, "%s/profiles.csv", d);
	return buf;
}

/* Persist the profile store to <data_dir>/profiles.csv (best effort). */
static void
save_profiles(void)
{
	const char *path = profiles_path();
	const char *d = data_dir();
	if (!path)
		return;
	if (d)
		mkdir(d, 0755);         /* ensure the dir exists (best effort) */
	bd_profiles_save(profiles, path);
}

/* ---- application settings persistence (<data_dir>/settings.csv) ----
 * A flat "key,value" CSV (one setting per line), human-editable like the
 * profile list. Unknown keys are ignored; missing file means all defaults. */

static const char *
settings_path(void)
{
	static char buf[600];
	const char *d = data_dir();
	if (!d)
		return NULL;
	snprintf(buf, sizeof buf, "%s/settings.csv", d);
	return buf;
}

static void
load_settings(void)
{
	const char *path = settings_path();
	FILE *f;
	char line[128];

	if (!path || !(f = fopen(path, "r")))
		return;
	while (fgets(line, sizeof line, f)) {
		char *nl = strpbrk(line, "\r\n");
		char *comma = strchr(line, ',');
		if (nl)
			*nl = '\0';
		if (!comma)
			continue;
		*comma = '\0';
		const char *key = line, *val = comma + 1;
		if (!strcmp(key, "cols"))
			settings.cols = atoi(val);
		else if (!strcmp(key, "rows"))
			settings.rows = atoi(val);
		else if (!strcmp(key, "scheme")) {
			for (int i = 0; i < N_SCHEMES; i++)
				if (!strcmp(val, scheme_ids[i]))
					settings.scheme = i;
		}
	}
	fclose(f);
	/* clamp to the same ranges the spinners enforce */
	if (settings.cols < 20) settings.cols = 20;
	if (settings.cols > 500) settings.cols = 500;
	if (settings.rows < 5) settings.rows = 5;
	if (settings.rows > 200) settings.rows = 200;
	if (settings.scheme < 0 || settings.scheme >= N_SCHEMES)
		settings.scheme = 0;
}

static void
save_settings(void)
{
	const char *path = settings_path();
	const char *d = data_dir();
	FILE *f;

	if (!path)
		return;
	if (d)
		mkdir(d, 0755);
	if (!(f = fopen(path, "w")))
		return;
	fprintf(f, "cols,%d\n", settings.cols);
	fprintf(f, "rows,%d\n", settings.rows);
	fprintf(f, "scheme,%s\n", scheme_ids[settings.scheme]);
	fclose(f);
}

/* Push the current settings into the live session + terminal. Safe to call
 * before either exists (each part is guarded). */
static void
apply_settings(void)
{
	if (session)
		bd_session_set_winsize(session, settings.cols, settings.rows);
	if (terminal) {
		bd_palette pal = scheme_palette(settings.scheme);
		bd_terminal_set_palette(terminal, &pal);
	}
}

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
		if (ev->state == BD_NET_CONNECTED) {
			/* restore recent scrollback from the logs (the replayed
			 * DATA events arrive above with ev->replay set) */
			int rn = bd_session_replay(s, 20);
			if (rn > 0) {
				char note[64];
				snprintf(note, sizeof note,
				    "\033[2m*** restored %d line%s of scrollback\033[0m\r\n",
				    rn, rn == 1 ? "" : "s");
				bd_terminal_write(terminal, note, -1);
			}
			bd_terminal_write(terminal,
			    "\033[32m*** connected\033[0m\r\n", -1);
		} else if (ev->state == BD_NET_CLOSED || ev->state == BD_NET_ERROR)
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

/* Host function the front-end adds to the session VM: __bd_ui(key, value)
 * pushes a named display value into the sidebar. The Lua side (a default
 * on.gmcp handler) extracts vitals from a decoded GMCP table and calls this,
 * so the core stays unaware of widgets and a profile script can override the
 * handler. Labels borrow their string, so keep it in a static buffer. */
static int
host_ui(bd_vm *vm, int argc, const bd_vm_val *argv, bd_vm_val *ret, void *ud)
{
	static char hp[48], mp[48];
	const char *key, *val;
	(void)vm;
	(void)ret;
	(void)ud;
	if (argc < 2 || argv[0].type != BD_VM_STR || argv[1].type != BD_VM_STR)
		return 0;
	key = argv[0].u.s;
	val = argv[1].u.s;
	if (!strcmp(key, "hp")) {
		snprintf(hp, sizeof hp, "HP: %s", val);
		bd_set(hp_label, BD_LABEL_S, hp, BD_END);
	} else if (!strcmp(key, "mp")) {
		snprintf(mp, sizeof mp, "MP: %s", val);
		bd_set(mp_label, BD_LABEL_S, mp, BD_END);
	}
	return 0;
}

/* Default sidebar-vitals wiring, (re)installed on each session's VM: register
 * __bd_ui, route GMCP Char.Vitals -> the sidebar, and blank the vitals on
 * disconnect. A profile script can replace the on.gmcp handler later. */
static const char vitals_lua[] =
	"on.gmcp['Char.Vitals'] = function(v)\n"
	"  if type(v) ~= 'table' then return end\n"
	"  if v.hp then __bd_ui('hp', tostring(v.hp)) end\n"
	"  if v.mp then __bd_ui('mp', tostring(v.mp)) end\n"
	"end\n"
	"table.insert(on.disconnect, function() __bd_ui('hp','--'); __bd_ui('mp','--') end)\n";

static void
install_ui_hooks(bd_session *s)
{
	bd_vm *vm = bd_session_vm(s);
	bd_vm_register(vm, "__bd_ui", host_ui, NULL);
	bd_vm_eval(vm, vitals_lua);     /* no-op if scripting is disabled */
}

/* Per-user data directory for profile state (the var table, later logs).
 * $BIRDIE_DATA, else $XDG_CONFIG_HOME/birdie, else $HOME/.config/birdie. */
static const char *
data_dir(void)
{
	static char buf[512];
	const char *d = getenv("BIRDIE_DATA");
	const char *xdg, *home;
	if (d && *d)
		return d;
	xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg) {
		snprintf(buf, sizeof buf, "%s/birdie", xdg);
		return buf;
	}
	home = getenv("HOME");
	if (home && *home) {
		snprintf(buf, sizeof buf, "%s/.config/birdie", home);
		return buf;
	}
	return NULL;
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
	bd_session_set_winsize(session, settings.cols, settings.rows);
	install_ui_hooks(session);
	bd_session_set_data_dir(session, data_dir());   /* loads the var table */

	/* run the profile's triggers.lua so the user's triggers/aliases/hooks
	 * are live for this session */
	switch (bd_session_load_profile_script(session)) {
	case 1:
		bd_terminal_write(terminal,
		    "\033[36m*** loaded profile script\033[0m\r\n", -1);
		break;
	case -1: {
		char line[400];
		snprintf(line, sizeof line,
		    "\033[31m*** script error: %s\033[0m\r\n",
		    bd_vm_error(bd_session_vm(session)));
		bd_terminal_write(terminal, line, -1);
		break;
	}
	default:
		break;          /* no script for this profile */
	}
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
	bd_dialog_open(connect_dlg);
}

/* Connect button inside the dialog: connect the selected MUD, then close. */
static void
on_dialog_connect(bd_id id, void *arg)
{
	(void)id;
	(void)arg;
	const bd_profile *p = selected_profile();
	bd_dialog_close(connect_dlg);
	connect_profile(p);
}

/* ---- add / edit / delete / import a profile ---- */

/* Open the edit dialog. `p` NULL means "add a new profile" (blank form);
 * otherwise the form is prefilled and Save updates that profile. */
static void
open_edit(const bd_profile *p)
{
	const char *host = p ? bd_profile_get(p, "host") : NULL;
	const char *port = p ? bd_profile_get(p, "port") : NULL;
	const char *name = p ? bd_profile_get(p, "name") : NULL;
	const char *tls = p ? bd_profile_get(p, "tls") : NULL;
	const char *rc = p ? bd_profile_get(p, "autoreconnect") : NULL;
	const char *tt = p ? bd_profile_get(p, "termtype") : NULL;

	snprintf(editing_name, sizeof editing_name, "%s", name ? name : "");
	bd_set(edit_name, BD_LABEL_S, name ? name : "", BD_END);
	bd_set(edit_host, BD_LABEL_S, host ? host : "", BD_END);
	bd_set(edit_port, BD_LABEL_S, port ? port : "23", BD_END);
	bd_checkbox_set(edit_tls, tls && strcmp(tls, "no") != 0 &&
	    strcmp(tls, "") != 0);
	/* autoreconnect defaults on (the session's default), so only an explicit
	 * off value ("no"/"false"/"0"/"off") unticks it */
	bd_checkbox_set(edit_reconnect, !(rc && (!strcmp(rc, "no") ||
	    !strcmp(rc, "false") || !strcmp(rc, "0") || !strcmp(rc, "off"))));
	bd_set(edit_termtype, BD_LABEL_S, tt ? tt : "", BD_END);
	bd_dialog_open(edit_dlg);
}

static void
on_add(bd_id id, void *arg)
{
	(void)id;
	(void)arg;
	open_edit(NULL);
}

static void
on_edit(bd_id id, void *arg)
{
	(void)id;
	(void)arg;
	open_edit(selected_profile());
}

/* Save the edit form into the store (creating or updating), persist, refresh. */
static void
on_edit_save(bd_id id, void *arg)
{
	const char *name = bd_get_s(edit_name, BD_LABEL_S);
	bd_profile *p;
	(void)id;
	(void)arg;

	if (!name || !name[0]) {
		bd_terminal_write(terminal,
		    "\033[31m*** profile needs a name\033[0m\r\n", -1);
		return;
	}
	/* a rename (editing an existing profile to a new name) drops the old */
	if (editing_name[0] && strcmp(editing_name, name) != 0)
		bd_profiles_remove(profiles, editing_name);
	p = bd_profiles_add(profiles, name);            /* find or create */
	if (!p) {
		bd_terminal_write(terminal,
		    "\033[31m*** could not save profile\033[0m\r\n", -1);
		return;
	}
	bd_profile_set(p, "host", bd_get_s(edit_host, BD_LABEL_S));
	bd_profile_set(p, "port", bd_get_s(edit_port, BD_LABEL_S));
	bd_profile_set(p, "tls", bd_checkbox_get(edit_tls) ? "yes" : "no");
	bd_profile_set(p, "autoreconnect",
	    bd_checkbox_get(edit_reconnect) ? "yes" : "no");
	/* an empty term type removes the key so the session's default applies */
	{
		const char *tt = bd_get_s(edit_termtype, BD_LABEL_S);
		bd_profile_set(p, "termtype", (tt && tt[0]) ? tt : NULL);
	}
	save_profiles();
	bd_table_refresh(mudlist);
	bd_dialog_close(edit_dlg);
}

static void
on_delete(bd_id id, void *arg)
{
	const bd_profile *p = selected_profile();
	const char *name = p ? bd_profile_get(p, "name") : NULL;
	(void)id;
	(void)arg;
	if (!name)
		return;
	bd_profiles_remove(profiles, name);
	save_profiles();
	bd_table_refresh(mudlist);
}

/* ---- export the profile list (a chosen column subset) ---- */

/* The Export button in the connect dialog opens the column picker. */
static void
on_open_export(bd_id id, void *arg)
{
	(void)id;
	(void)arg;
	bd_dialog_open(export_dlg);
}

/* Build the column filter from the ticked boxes (name is always included, it
 * is the row identity), export, and write it to the given path. */
static void
on_export(bd_id panel, void *arg)
{
	char filter[256] = "name";
	const char *path = bd_get_s(export_path, BD_LABEL_S);
	char *csv;
	size_t len;
	FILE *f;
	int i;
	(void)arg;

	if (!path || !path[0]) {
		bd_terminal_write(terminal,
		    "\033[33m*** enter a path to export to\033[0m\r\n", -1);
		return;
	}
	for (i = 0; i < N_EXPORT_COLS; i++)
		if (bd_checkbox_get(exp_check[i])) {
			size_t used = strlen(filter);
			snprintf(filter + used, sizeof filter - used, ",%s",
			    export_cols[i]);
		}
	csv = bd_profiles_export_csv(profiles, filter, &len);
	if (!csv) {
		bd_terminal_write(terminal,
		    "\033[31m*** export failed\033[0m\r\n", -1);
		return;
	}
	f = fopen(path, "wb");
	if (!f) {
		free(csv);
		bd_terminal_write(terminal,
		    "\033[31m*** cannot write the export file\033[0m\r\n", -1);
		return;
	}
	fwrite(csv, 1, len, f);
	fclose(f);
	free(csv);
	bd_terminal_write(terminal,
	    "\033[36m*** exported the profile list\033[0m\r\n", -1);
	bd_modal_close(panel);
}

/* ---- import merge helpers (used with the collision policy) ---- */

/* Copy every key from src into dst. force_name (if set) is kept as dst's name
 * instead of src's, for the rename policy. */
static void
copy_profile_keys(bd_profile *dst, const bd_profile *src, const char *force_name)
{
	int i, n = bd_profile_count(src);
	for (i = 0; i < n; i++) {
		const char *k = bd_profile_key(src, i);
		if (force_name && !strcmp(k, "name"))
			continue;
		bd_profile_set(dst, k, bd_profile_val(src, i));
	}
	if (force_name)
		bd_profile_set(dst, "name", force_name);
}

/* "base (2)", "base (3)", ... : the first that no profile in ps already uses. */
static void
unique_profile_name(bd_profiles *ps, const char *base, char *out, size_t outsz)
{
	int i;
	for (i = 2; i < 100000; i++) {
		snprintf(out, outsz, "%s (%d)", base, i);
		if (!bd_profiles_find(ps, out))
			return;
	}
}

/* Number of profiles in src whose name already exists in dst. */
static int
count_collisions(bd_profiles *dst, bd_profiles *src)
{
	int i, c = 0, n = bd_profiles_count(src);
	for (i = 0; i < n; i++) {
		const char *name = bd_profile_get(bd_profiles_at(src, i), "name");
		if (name && *name && bd_profiles_find(dst, name))
			c++;
	}
	return c;
}

/* Merge src into dst under `policy` (IMP_*). Returns the count added/updated. */
static int
merge_profiles(bd_profiles *dst, bd_profiles *src, int policy)
{
	int i, changed = 0, n = bd_profiles_count(src);
	for (i = 0; i < n; i++) {
		bd_profile *sp = bd_profiles_at(src, i);
		const char *name = bd_profile_get(sp, "name");
		bd_profile *ex, *np;
		if (!name || !*name)
			continue;
		ex = bd_profiles_find(dst, name);
		if (!ex) {                          /* no clash: add as-is */
			np = bd_profiles_add(dst, name);
			if (np) { copy_profile_keys(np, sp, NULL); changed++; }
		} else if (policy == IMP_SKIP) {
			continue;                       /* keep the existing one */
		} else if (policy == IMP_RENAME) {
			char nm[160];
			unique_profile_name(dst, name, nm, sizeof nm);
			np = bd_profiles_add(dst, nm);
			if (np) { copy_profile_keys(np, sp, nm); changed++; }
		} else {                            /* IMP_OVERWRITE (merge fields) */
			copy_profile_keys(ex, sp, NULL);
			changed++;
		}
	}
	return changed;
}

/* ---- import a CSV file, resolving name collisions ---- */

/* Import a local CSV file (path from the import field): parse into a scratch
 * store, and if any name already exists ask how to resolve it, else merge. */
static void
on_import(bd_id id, void *arg)
{
	const char *path = bd_get_s(import_path, BD_LABEL_S);
	char line[200];
	FILE *f;
	long sz;
	char *buf;
	size_t got;
	int n, coll;
	bd_profiles *scratch;
	(void)id;
	(void)arg;

	if (!path || !path[0]) {
		bd_terminal_write(terminal,
		    "\033[33m*** enter a CSV file path to import\033[0m\r\n", -1);
		return;
	}
	f = fopen(path, "rb");
	if (!f) {
		snprintf(line, sizeof line,
		    "\033[31m*** cannot open %s\033[0m\r\n", path);
		bd_terminal_write(terminal, line, -1);
		return;
	}
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0) {
		fclose(f);
		return;
	}
	buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return;
	}
	got = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[got] = '\0';

	scratch = bd_profiles_new();
	if (!scratch) {
		free(buf);
		return;
	}
	n = bd_profiles_import_csv(scratch, buf, got, BD_PROFILE_SAFE_COLUMNS);
	free(buf);
	if (n < 0) {
		bd_profiles_free(scratch);
		bd_terminal_write(terminal,
		    "\033[31m*** import failed (parse error)\033[0m\r\n", -1);
		return;
	}

	coll = count_collisions(profiles, scratch);
	if (coll == 0) {                        /* no clashes: merge straight in */
		int added = merge_profiles(profiles, scratch, IMP_OVERWRITE);
		bd_profiles_free(scratch);
		save_profiles();
		bd_table_refresh(mudlist);
		snprintf(line, sizeof line,
		    "\033[36m*** imported %d profile%s\033[0m\r\n",
		    added, added == 1 ? "" : "s");
		bd_terminal_write(terminal, line, -1);
		return;
	}

	/* clashes exist: hold the parsed rows and ask the user how to resolve */
	pending_import = scratch;
	snprintf(imp_msg_buf, sizeof imp_msg_buf,
	    "%d of %d imported profile%s already exist. Resolve conflicts:",
	    coll, n, n == 1 ? "" : "s");
	bd_set(imp_msg, BD_LABEL_S, imp_msg_buf, BD_END);
	bd_radio_set(imp_policy, IMP_OVERWRITE);
	bd_dialog_open(import_dlg);
}

/* Apply the chosen collision policy to the held rows. */
static void
on_import_apply(bd_id panel, void *arg)
{
	int policy = bd_radio_get(imp_policy);
	int added;
	char line[120];
	(void)arg;

	if (!pending_import) {
		bd_modal_close(panel);
		return;
	}
	added = merge_profiles(profiles, pending_import,
	    policy < 0 ? IMP_OVERWRITE : policy);
	bd_profiles_free(pending_import);
	pending_import = NULL;
	save_profiles();
	bd_table_refresh(mudlist);
	snprintf(line, sizeof line,
	    "\033[36m*** imported %d profile%s\033[0m\r\n", added, added == 1 ? "" : "s");
	bd_terminal_write(terminal, line, -1);
	bd_modal_close(panel);
}

/* Cancel / Escape: drop the held rows (fires for both the button and Escape). */
static void
on_import_cancel(bd_id panel, void *arg)
{
	(void)panel;
	(void)arg;
	if (pending_import) {
		bd_profiles_free(pending_import);
		pending_import = NULL;
	}
	bd_terminal_write(terminal,
	    "\033[33m*** import cancelled\033[0m\r\n", -1);
}

/* Double-click / Enter on a dialog row connects to it and closes the dialog. */
static void
mudlist_activate(bd_id w, int row, void *ctx)
{
	const bd_profile *p = bd_profiles_at(profiles, row);
	(void)w;
	(void)ctx;
	bd_dialog_close(connect_dlg);
	connect_profile(p);
}

static void
on_disconnect(bd_id id, void *arg)
{
	(void)id;
	(void)arg;
	bd_session_disconnect(session);
}

/* "Session > Reload Script" rebuilds the session for the active profile so the
 * profile script re-runs on a clean engine (re-running it on the live engine
 * would additively duplicate triggers and hooks). Only when disconnected. */
static void
on_reload_script(bd_id id, void *arg)
{
	const bd_profile *p;
	(void)id;
	(void)arg;
	if (!session || !active) {
		bd_terminal_write(terminal,
		    "\033[31m*** no session to reload\033[0m\r\n", -1);
		return;
	}
	if (bd_session_state(session) == BD_NET_CONNECTED) {
		bd_terminal_write(terminal,
		    "\033[33m*** disconnect before reloading the script\033[0m\r\n",
		    -1);
		return;
	}
	p = active;
	bd_session_free(session);
	session = NULL;
	active = NULL;
	rebind_session(p);              /* fresh engine; reports the script result */
}

/* The input line fires its click handler on Enter, before clearing, so the
 * submitted command is still readable here. A leading '#' is a trigger verb
 * (#action / #alias / #class); otherwise the line is sent (via the aliases). */
static void
on_submit(bd_id id, void *arg)
{
	(void)arg;
	const char *cmd = bd_get_s(id, BD_LABEL_S);
	const char *literal = NULL;
	char fb[2048];

	if (!cmd || !cmd[0])
		return;

	if (bd_verb_exec(bd_session_triggers(session), cmd, &literal,
	    fb, sizeof fb)) {
		char line[2100];
		snprintf(line, sizeof line, "\033[36m# %s\033[0m\r\n", fb);
		bd_terminal_write(terminal, line, -1);
		return;
	}
	if (literal)            /* "##cmd" -> send literal "#cmd" */
		cmd = literal;

	if (bd_session_state(session) != BD_NET_CONNECTED) {
		bd_terminal_write(terminal,
		    "\033[31m*** not connected\033[0m\r\n", -1);
		return;
	}
	bd_session_send_line(session, cmd);
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
	const char *saved = profiles_path();
	const char *want = getenv("BIRDIE_PROFILE");
	const char *host = getenv("BIRDIE_HOST");
	const char *port = getenv("BIRDIE_PORT");
	bd_profile *p;

	if (path && bd_profiles_load(profiles, path) == 0) {
		/* an explicit BIRDIE_PROFILES list wins */
	} else if (saved && bd_profiles_load(profiles, saved) == 0 &&
	    bd_profiles_count(profiles) > 0) {
		/* the user's persisted list (edited/imported in-app) */
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

/* ---- settings dialog ---- */

static void
on_open_settings(bd_id id, void *arg)
{
	(void)id;
	(void)arg;
	bd_spinner_set(set_cols, settings.cols);
	bd_spinner_set(set_rows, settings.rows);
	bd_combo_set(set_scheme, settings.scheme);
	bd_dialog_open(settings_dlg);
}

/* Save reads the controls, applies live (NAWS resize + palette), and persists. */
static void
on_settings_save(bd_id panel, void *arg)
{
	int sc;
	(void)arg;
	settings.cols = bd_spinner_get(set_cols);
	settings.rows = bd_spinner_get(set_rows);
	sc = bd_combo_get(set_scheme);
	settings.scheme = (sc >= 0 && sc < N_SCHEMES) ? sc : 0;
	apply_settings();
	save_settings();
	bd_modal_close(panel);
}

/* ---- colour picker (compose a #highlight colour) ---- */

/* Refresh the hex and SGR readouts from a packed colour. */
static void
color_fields_update(uint32_t rgba)
{
	static char hex[16], sgr[32];
	unsigned r = (rgba >> 24) & 0xFF, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF;
	snprintf(hex, sizeof hex, "#%02X%02X%02X", r, g, b);
	snprintf(sgr, sizeof sgr, "38;2;%u;%u;%u", r, g, b);   /* a #highlight body */
	bd_set(color_hex, BD_LABEL_S, hex, BD_END);
	bd_set(color_sgr, BD_LABEL_S, sgr, BD_END);
}

static void
on_color_change(bd_id id, void *arg, uint32_t rgba)
{
	(void)id;
	(void)arg;
	color_fields_update(rgba);
}

static void
on_open_color(bd_id id, void *arg)
{
	(void)id;
	(void)arg;
	color_fields_update(bd_colorpick_get(color_pick));
	bd_dialog_open(color_dlg);
}

/* ---- trigger editor (live: edits the current session's trigger table) ----
 * Triggers are persisted in each profile's triggers.lua (a user-editable Lua
 * script loaded when the session is created); there is no API to write them
 * back, and rewriting the script would clobber hand-written Lua. So this editor
 * operates on the running session only, exactly like the #action / #alias
 * verbs: adds and removes take effect immediately and live for the life of the
 * session (they are not saved; switching profiles rebuilds it from the script).
 * The engine exists before connecting, so the editor works while offline. */

/* combo/table order matches bd_trigger_type, so a combo index IS the enum. */
static const char *const trig_type_labels[] = {
	"Action", "Alias", "Prompt", "GMCP", "Gag", "Substitute", "Highlight", "MXP"
};
#define N_TRIG_TYPES ((int)(sizeof trig_type_labels / sizeof trig_type_labels[0]))

static const char *
trig_type_name(bd_trigger_type type)
{
	int i = (int)type;
	return (i >= 0 && i < N_TRIG_TYPES) ? trig_type_labels[i] : "?";
}

/* A snapshot of one trigger, copied out of the engine so the table model can
 * read it after bd_trigger_foreach returns (the callback strings are borrowed).*/
struct trig_row {
	bd_trigger_type type;
	int             priority;
	int             enabled;
	char            pattern[512];
	char            body[512];
	char            cls[64];
};
static struct trig_row trig_rows[256];
static int trig_nrows;

static void
trig_collect(bd_trigger_type type, const char *pattern, const char *body,
             const char *cls, const char *chain, int state, int priority,
             int enabled, void *ctx)
{
	struct trig_row *r;
	(void)chain;
	(void)state;
	(void)ctx;
	if (trig_nrows >= (int)(sizeof trig_rows / sizeof trig_rows[0]))
		return;
	r = &trig_rows[trig_nrows++];
	r->type = type;
	r->priority = priority;
	r->enabled = enabled;
	snprintf(r->pattern, sizeof r->pattern, "%s", pattern ? pattern : "");
	snprintf(r->body, sizeof r->body, "%s", body ? body : "");
	snprintf(r->cls, sizeof r->cls, "%s", cls ? cls : "");
}

/* Re-read the session's triggers into trig_rows and refresh the table. */
static void
trig_reload(void)
{
	bd_triggers *t = session ? bd_session_triggers(session) : NULL;
	trig_nrows = 0;
	if (t)
		bd_trigger_foreach(t, trig_collect, NULL);
	if (trig_table)
		bd_table_refresh(trig_table);
}

static int
trig_rows_count(void *ctx)
{
	(void)ctx;
	return trig_nrows;
}

static const char *
trig_cell(void *ctx, int row, int col)
{
	static char num[8];
	(void)ctx;
	if (row < 0 || row >= trig_nrows)
		return "";
	struct trig_row *r = &trig_rows[row];
	switch (col) {
	case 0: return trig_type_name(r->type);
	case 1: return r->pattern;
	case 2: return r->body;
	case 3: return r->cls;
	case 4: snprintf(num, sizeof num, "%d", r->priority); return num;
	case 5: return r->enabled ? "yes" : "no";
	}
	return "";
}

static void
on_trig_add(bd_id id, void *arg)
{
	bd_triggers *t = session ? bd_session_triggers(session) : NULL;
	const char *pat = bd_get_s(trig_pattern, BD_LABEL_S);
	const char *body = bd_get_s(trig_body, BD_LABEL_S);
	const char *cls = bd_get_s(trig_class, BD_LABEL_S);
	int ti = bd_combo_get(trig_type);
	(void)id;
	(void)arg;
	if (!t) {
		bd_terminal_write(terminal,
		    "\033[31m*** no active session to add a trigger to\033[0m\r\n", -1);
		return;
	}
	if (!pat || !pat[0]) {
		bd_terminal_write(terminal,
		    "\033[31m*** a trigger needs a pattern\033[0m\r\n", -1);
		return;
	}
	bd_trigger_add(t, (bd_trigger_type)(ti >= 0 ? ti : 0), pat, body,
	    (cls && cls[0]) ? cls : NULL, bd_spinner_get(trig_pri),
	    bd_checkbox_get(trig_stop));
	bd_set(trig_pattern, BD_LABEL_S, "", BD_END);   /* ready for the next add */
	bd_set(trig_body, BD_LABEL_S, "", BD_END);
	trig_reload();
}

static void
on_trig_remove(bd_id id, void *arg)
{
	bd_triggers *t = session ? bd_session_triggers(session) : NULL;
	int sel = trig_table ? bd_table_current(trig_table) : -1;
	(void)id;
	(void)arg;
	if (!t || sel < 0 || sel >= trig_nrows)
		return;
	struct trig_row *r = &trig_rows[sel];
	bd_trigger_remove_pattern(t, r->type, r->pattern, r->cls[0] ? r->cls : NULL);
	trig_reload();
}

static void
on_open_triggers(bd_id id, void *arg)
{
	(void)id;
	(void)arg;
	trig_reload();
	bd_dialog_open(triggers_dlg);
}

static void
init(void)
{
	bd_gui_init(&bd_backend_ludica, NULL);
	load_settings();   /* before the session/terminal so both start configured */

	profiles = bd_profiles_new();
	active = load_profiles();
	session = bd_session_new(active);
	bd_session_on_event(session, on_session_event, NULL);
	bd_session_set_winsize(session, settings.cols, settings.rows);
	install_ui_hooks(session);   /* GMCP Char.Vitals -> sidebar (host_ui only
	                              * fires at runtime, after the labels exist) */
	bd_session_set_data_dir(session, data_dir());   /* loads the var table */

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
	bd_create(m_edit, BD_BUTTON, BD_LABEL_S, "Settings...",
		BD_ON_CLICK_F, on_open_settings, BD_END);
	bd_create(m_edit, BD_BUTTON, BD_LABEL_S, "Colour picker...",
		BD_ON_CLICK_F, on_open_color, BD_END);

	bd_id m_sess = bd_create(menu, BD_MENU, BD_LABEL_S, "Session", BD_END);
	bd_create(m_sess, BD_BUTTON, BD_LABEL_S, "Connect...",
		BD_ON_CLICK_F, on_open_connect, BD_END);
	bd_create(m_sess, BD_BUTTON, BD_LABEL_S, "Disconnect",
		BD_ON_CLICK_F, on_disconnect, BD_END);
	bd_create(m_sess, BD_BUTTON, BD_LABEL_S, "Triggers...",
		BD_ON_CLICK_F, on_open_triggers, BD_END);
	bd_create(m_sess, BD_BUTTON, BD_LABEL_S, "Reload Script",
		BD_ON_CLICK_F, on_reload_script, BD_END);

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
	/* the GMCP Char.Vitals handler (install_ui_hooks) fills these in */
	hp_label = bd_create(side, BD_LABEL, BD_LABEL_S, "HP: --", BD_PREF_H_I, 16, BD_END);
	mp_label = bd_create(side, BD_LABEL, BD_LABEL_S, "MP: --", BD_PREF_H_I, 16, BD_END);
	bd_create(side, BD_LABEL, BD_LABEL_S, "", BD_GROW_I, 1, BD_END); /* filler */

	/* right: terminal output + command input */
	bd_id right = bd_create(body, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_COL, BD_GROW_I, 1, BD_GAP_I, 4, BD_END);
	terminal = bd_terminal_create(right, BD_GROW_I, 1, BD_END);
	apply_settings();   /* colour scheme now that the terminal exists */
	bd_terminal_write(terminal,
		"\033[1mbirdie v0.0\033[0m\r\n"
		"Session > Connect... to choose a MUD.\r\n",
		-1);
	input_line = bd_create(right, BD_INPUT_LINE,
		BD_PREF_H_I, 24, BD_PAD_I, 4, BD_ON_CLICK_F, on_submit, BD_END);

	/* the modal connect dialog: the MUD-list table plus manage / import rows,
	 * composed with the bd_dialog helper (Enter connects, Escape cancels) */
	connect_dlg = bd_dialog_create("Connect to a MUD (double-click a row)",
		460, 340);
	bd_id cbody = bd_dialog_content(connect_dlg);
	static const bd_table_column mcols[] = {
		{ "MUD",  0,   BD_TABLE_LEFT,  0 },
		{ "Host", 170, BD_TABLE_LEFT,  0 },
		{ "Port", 50,  BD_TABLE_RIGHT, BD_TABLE_COL_NUMERIC },
	};
	mudlist = bd_table_create(cbody, mcols, 3,
		&(bd_table_model){ mudlist_rows, mudlist_cell, NULL },
		&(bd_table_cb){ .activate = mudlist_activate },
		BD_GROW_I, 1, BD_END);
	/* manage row: add / edit / delete profiles */
	bd_id mbtn = bd_create(cbody, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_ROW, BD_PREF_H_I, 28, BD_GAP_I, 6, BD_END);
	bd_create(mbtn, BD_BUTTON, BD_LABEL_S, "Add", BD_GROW_I, 1,
		BD_ON_CLICK_F, on_add, BD_END);
	bd_create(mbtn, BD_BUTTON, BD_LABEL_S, "Edit", BD_GROW_I, 1,
		BD_ON_CLICK_F, on_edit, BD_END);
	bd_create(mbtn, BD_BUTTON, BD_LABEL_S, "Delete", BD_GROW_I, 1,
		BD_ON_CLICK_F, on_delete, BD_END);
	/* import / export row: a local CSV file path + import + export */
	bd_id ibtn = bd_create(cbody, BD_PANEL,
		BD_LAYOUT_I, BD_LAYOUT_ROW, BD_PREF_H_I, 26, BD_GAP_I, 6, BD_END);
	import_path = bd_create(ibtn, BD_INPUT_LINE, BD_GROW_I, 1,
		BD_PAD_I, 3, BD_END);
	bd_create(ibtn, BD_BUTTON, BD_LABEL_S, "Import CSV", BD_PREF_W_I, 90,
		BD_ON_CLICK_F, on_import, BD_END);
	bd_create(ibtn, BD_BUTTON, BD_LABEL_S, "Export...", BD_PREF_W_I, 80,
		BD_ON_CLICK_F, on_open_export, BD_END);
	bd_dialog_button(connect_dlg, "Cancel", BD_DIALOG_CANCEL, NULL, NULL);
	bd_dialog_button(connect_dlg, "Connect", BD_DIALOG_DEFAULT,
		on_dialog_connect, NULL);

	/* the add/edit profile form, composed with the bd_dialog helper */
	edit_dlg = bd_dialog_create("Profile", 380, 300);
	edit_name = bd_create(bd_dialog_field(edit_dlg, "Name"), BD_INPUT_LINE,
		BD_GROW_I, 1, BD_PAD_I, 3, BD_END);
	edit_host = bd_create(bd_dialog_field(edit_dlg, "Host"), BD_INPUT_LINE,
		BD_GROW_I, 1, BD_PAD_I, 3, BD_END);
	edit_port = bd_create(bd_dialog_field(edit_dlg, "Port"), BD_INPUT_LINE,
		BD_GROW_I, 1, BD_PAD_I, 3, BD_END);
	edit_tls = bd_checkbox_create(bd_dialog_field(edit_dlg, "Security"),
		&(bd_checkbox_desc){ .label = "TLS" }, BD_END);
	edit_reconnect = bd_checkbox_create(bd_dialog_field(edit_dlg, "Reconnect"),
		&(bd_checkbox_desc){ .label = "Automatically" }, BD_END);
	edit_termtype = bd_create(bd_dialog_field(edit_dlg, "Term type"),
		BD_INPUT_LINE, BD_GROW_I, 1, BD_PAD_I, 3, BD_END);
	bd_dialog_button(edit_dlg, "Cancel", BD_DIALOG_CANCEL, NULL, NULL);
	bd_dialog_button(edit_dlg, "Save", BD_DIALOG_DEFAULT, on_edit_save, NULL);

	/* the app-wide settings form (terminal grid + colour scheme) */
	settings_dlg = bd_dialog_create("Settings", 340, 200);
	set_cols = bd_spinner_create(bd_dialog_field(settings_dlg, "Columns"),
		&(bd_spinner_desc){ .min = 20, .max = 500, .value = settings.cols },
		BD_PREF_W_I, 90, BD_END);
	set_rows = bd_spinner_create(bd_dialog_field(settings_dlg, "Rows"),
		&(bd_spinner_desc){ .min = 5, .max = 200, .value = settings.rows },
		BD_PREF_W_I, 90, BD_END);
	set_scheme = bd_combo_create(bd_dialog_field(settings_dlg, "Colors"),
		&(bd_combo_desc){ .items = scheme_labels, .count = N_SCHEMES,
		.selected = settings.scheme }, BD_GROW_I, 1, BD_END);
	bd_dialog_button(settings_dlg, "Cancel", BD_DIALOG_CANCEL, NULL, NULL);
	bd_dialog_button(settings_dlg, "Save", BD_DIALOG_DEFAULT,
		on_settings_save, NULL);

	/* the live trigger editor: a table of the session's triggers plus a compact
	 * add form. Add is the default button (Enter adds and keeps the dialog open;
	 * a trigger pattern typed in a field confirms with Enter), Close cancels. */
	triggers_dlg = bd_dialog_create("Triggers (this session)", 620, 380);
	bd_id tbody = bd_dialog_content(triggers_dlg);
	static const bd_table_column tcols[] = {
		{ "Type",    64, BD_TABLE_LEFT,  0 },
		{ "Pattern", 0,  BD_TABLE_LEFT,  0 },
		{ "Body",    0,  BD_TABLE_LEFT,  0 },
		{ "Class",   80, BD_TABLE_LEFT,  0 },
		{ "Pri",     34, BD_TABLE_RIGHT, BD_TABLE_COL_NUMERIC },
		{ "On",      32, BD_TABLE_CENTER, BD_TABLE_COL_NOSORT },
	};
	trig_table = bd_table_create(tbody, tcols, 6,
		&(bd_table_model){ trig_rows_count, trig_cell, NULL }, NULL,
		BD_GROW_I, 1, BD_END);
	bd_create(tbody, BD_BUTTON, BD_LABEL_S, "Remove selected", BD_PREF_H_I, 26,
		BD_ON_CLICK_F, on_trig_remove, BD_END);
	/* add form, row 1: type + pattern */
	bd_id trow1 = bd_create(tbody, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_ROW,
		BD_PREF_H_I, 28, BD_GAP_I, 6, BD_END);
	trig_type = bd_combo_create(trow1, &(bd_combo_desc){
		.items = trig_type_labels, .count = N_TRIG_TYPES, .selected = 0 },
		BD_PREF_W_I, 110, BD_END);
	trig_pattern = bd_create(trow1, BD_INPUT_LINE, BD_GROW_I, 1, BD_PAD_I, 3,
		BD_END);
	/* row 2: body */
	bd_id trow2 = bd_create(tbody, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_ROW,
		BD_PREF_H_I, 28, BD_GAP_I, 6, BD_END);
	bd_create(trow2, BD_LABEL, BD_LABEL_S, "Body", BD_PREF_W_I, 40, BD_END);
	trig_body = bd_create(trow2, BD_INPUT_LINE, BD_GROW_I, 1, BD_PAD_I, 3,
		BD_END);
	/* row 3: class + priority + stop */
	bd_id trow3 = bd_create(tbody, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_ROW,
		BD_PREF_H_I, 28, BD_GAP_I, 6, BD_END);
	bd_create(trow3, BD_LABEL, BD_LABEL_S, "Class", BD_PREF_W_I, 40, BD_END);
	trig_class = bd_create(trow3, BD_INPUT_LINE, BD_GROW_I, 1, BD_PAD_I, 3,
		BD_END);
	bd_create(trow3, BD_LABEL, BD_LABEL_S, "Pri", BD_PREF_W_I, 26, BD_END);
	trig_pri = bd_spinner_create(trow3, &(bd_spinner_desc){
		.min = 0, .max = 9, .value = BD_TRIG_PRIO_DEFAULT }, BD_PREF_W_I, 64,
		BD_END);
	trig_stop = bd_checkbox_create(trow3, &(bd_checkbox_desc){ .label = "Stop" },
		BD_PREF_W_I, 70, BD_END);
	bd_dialog_button(triggers_dlg, "Close", BD_DIALOG_CANCEL, NULL, NULL);
	bd_dialog_button(triggers_dlg, "Add", BD_DIALOG_DEFAULT, on_trig_add, NULL);

	/* the export column-filter form: pick columns (name is always included),
	 * choose a path, Export writes the CSV */
	export_dlg = bd_dialog_create("Export profiles", 360, 300);
	bd_id ebody = bd_dialog_content(export_dlg);
	bd_create(ebody, BD_LABEL, BD_LABEL_S, "Columns to include (name is always):",
		BD_PREF_H_I, 18, BD_END);
	for (int i = 0; i < N_EXPORT_COLS; i++)
		exp_check[i] = bd_checkbox_create(ebody, &(bd_checkbox_desc){
			.label = export_cols[i], .checked = 1 }, BD_END);
	export_path = bd_create(bd_dialog_field(export_dlg, "To file"),
		BD_INPUT_LINE, BD_GROW_I, 1, BD_PAD_I, 3, BD_END);
	bd_dialog_button(export_dlg, "Cancel", BD_DIALOG_CANCEL, NULL, NULL);
	bd_dialog_button(export_dlg, "Export", BD_DIALOG_DEFAULT, on_export, NULL);

	/* the import-collision form: a radio of resolution policies. Opened only
	 * when an import hits an existing name (see on_import). */
	static const char *const imp_policy_labels[] = {
		"Overwrite existing", "Skip existing", "Rename imported"
	};
	import_dlg = bd_dialog_create("Import conflicts", 380, 190);
	imp_msg = bd_create(bd_dialog_content(import_dlg), BD_LABEL, BD_LABEL_S, "",
		BD_PREF_H_I, 20, BD_END);
	imp_policy = bd_radio_create(bd_dialog_content(import_dlg),
		&(bd_radio_desc){ .labels = imp_policy_labels, .count = 3,
		.selected = IMP_OVERWRITE, .orient = BD_VERTICAL }, BD_END);
	bd_dialog_button(import_dlg, "Cancel", BD_DIALOG_CANCEL, on_import_cancel,
		NULL);
	bd_dialog_button(import_dlg, "Import", BD_DIALOG_DEFAULT, on_import_apply,
		NULL);

	/* the colour picker: a BD_COLORPICK plus hex + #highlight-SGR readouts to
	 * copy into a highlight trigger body */
	color_dlg = bd_dialog_create("Colour picker", 260, 320);
	color_pick = bd_colorpick_create(bd_dialog_content(color_dlg),
		&(bd_colorpick_desc){ .color = 0x33FF66FFu, .cb = on_color_change },
		BD_PREF_H_I, 180, BD_END);
	color_hex = bd_create(bd_dialog_field(color_dlg, "Hex"), BD_INPUT_LINE,
		BD_GROW_I, 1, BD_PAD_I, 3, BD_END);
	color_sgr = bd_create(bd_dialog_field(color_dlg, "Highlight"), BD_INPUT_LINE,
		BD_GROW_I, 1, BD_PAD_I, 3, BD_END);
	bd_dialog_button(color_dlg, "Close", BD_DIALOG_CANCEL, NULL, NULL);

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
	if (pending_import) {
		bd_profiles_free(pending_import);
		pending_import = NULL;
	}
	bd_dialog_free(connect_dlg);    /* before bd_gui_cleanup frees the pool */
	bd_dialog_free(edit_dlg);
	bd_dialog_free(settings_dlg);
	bd_dialog_free(triggers_dlg);
	bd_dialog_free(export_dlg);
	bd_dialog_free(import_dlg);
	bd_dialog_free(color_dlg);
	connect_dlg = edit_dlg = settings_dlg = triggers_dlg = NULL;
	export_dlg = import_dlg = color_dlg = NULL;
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
