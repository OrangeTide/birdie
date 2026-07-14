#ifndef BD_SESSION_H
#define BD_SESSION_H

#include "bd_net.h"
#include "bd_profile.h"
#include "bd_vm.h"
#include "bd_trigger.h"
#include <stddef.h>

/*
 * bd_session -- one connected MUD; the front-end seam (doc/core.md).
 *
 * A session owns the transport (bd_net), a scripting VM (bd_vm), and the
 * trigger engine (bd_triggers), and connects using a borrowed bd_profile
 * (host/port/tls/autoreconnect). Everything the net thread produces is
 * delivered to the front-end as bd_session_event callbacks on the UI thread,
 * during bd_session_drain(); along the way the session assembles the byte
 * stream into lines and runs them (and outgoing input) through the triggers.
 *
 * Still not built: the vt_buf grid + true line retirement (src/vt/) and log
 * sinks; until the vt module lands the session both forwards decoded bytes as
 * a DATA event for the front-end's terminal widget and does its own minimal
 * line assembly (split on newline, strip ANSI) to feed the trigger engine. The
 * default VM is the null backend (scripting disabled), so command-body
 * triggers work end to end while '@' Lua bodies are inert until Lua is
 * vendored; pass a Lua VM via bd_session_set_vm() when it exists.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

typedef struct bd_session bd_session;

enum bd_session_event_kind {
	BD_SESSION_STATE,       /* connection state changed */
	BD_SESSION_DATA,        /* decoded application bytes (telnet stripped) */
	BD_SESSION_PROMPT,      /* prompt boundary (telnet EOR / GA) */
	BD_SESSION_ECHO,        /* server echo control (mask input on suppress) */
	BD_SESSION_PACKAGE      /* GMCP / MSDP out-of-band package */
};

typedef struct bd_session_event {
	int kind;

	/* BD_SESSION_STATE */
	bd_net_state state;
	const char *detail;     /* optional human-readable note, may be NULL */

	/* BD_SESSION_DATA */
	const char *data;
	int len;
	int replay;             /* 1 if this is replayed scrollback, not live */

	/* BD_SESSION_ECHO */
	int echo_suppress;

	/* BD_SESSION_PACKAGE */
	int proto;              /* BD_TELOPT_GMCP / BD_TELOPT_MSDP */
	const char *name;
	const char *json;
} bd_session_event;

typedef void (*bd_session_event_fn)(bd_session *s, const bd_session_event *ev,
                                    void *userdata);

/* Create a session bound to a profile (borrowed; must outlive the session).
 * Returns NULL on failure. */
bd_session *bd_session_new(const bd_profile *profile);
void bd_session_free(bd_session *s);

/* Register the front-end event callback (set before connecting). */
void bd_session_on_event(bd_session *s, bd_session_event_fn fn, void *userdata);

/* Connect using the profile's host/port/tls/autoreconnect. Returns 0 if the
 * attempt started, -1 if the profile lacks host/port. State and errors arrive
 * via events. */
int bd_session_connect(bd_session *s);
/* User-initiated disconnect (cancels auto-reconnect). */
void bd_session_disconnect(bd_session *s);

bd_net_state bd_session_state(const bd_session *s);

/* Send a command line: it is first run through the aliases (an alias may
 * consume it and emit its own commands); otherwise the line is sent verbatim
 * with CRLF appended. Returns bytes accepted, -1 if not connected, or 0 when
 * an alias consumed the input. send_raw bypasses aliases. */
int bd_session_send_line(bd_session *s, const char *utf8);
int bd_session_send_raw(bd_session *s, const void *bytes, size_t n);

/* The session's trigger engine and scripting VM, so the verb parser, profile
 * scripts, and tests can install triggers and run script. Never NULL for a
 * live session. (The VM is the null backend until Lua is vendored.) */
bd_triggers *bd_session_triggers(bd_session *s);
bd_vm       *bd_session_vm(bd_session *s);

/* Report the terminal size to the server (NAWS). */
void bd_session_set_winsize(bd_session *s, int cols, int rows);

/* Set the base directory for this profile's persistent state. The script `var`
 * table is loaded from <dir>/profiles/<name>/vars.json now and saved back on
 * disconnect and at free. Pass NULL to make `var` session-only (no disk). */
void bd_session_set_data_dir(bd_session *s, const char *dir);

/* Load and run <data_dir>/profiles/<name>/triggers.lua if it exists (in the
 * sandboxed script environment). Returns 1 if a script ran, 0 if none is
 * present, -1 on a script error (message via bd_vm_error(bd_session_vm(s))).
 * Call after the data dir is set; the script registers the profile's triggers,
 * aliases, and on.* hooks. */
int bd_session_load_profile_script(bd_session *s);

/*
 * User triggers: a per-profile trigger set edited through the GUI and persisted
 * to <data_dir>/profiles/<name>/triggers.csv, kept separate from the
 * hand-written triggers.lua so saving never rewrites (and clobbers) that
 * script. The session mirrors this list into the live engine, so user triggers
 * fire alongside script triggers.
 *
 * _add installs the trigger in the live engine and records it in the user list
 * (a duplicate type+pattern+class replaces the existing entry). _remove drops
 * it from both. Neither writes to disk; call _save afterwards to persist. Both
 * return 0 on success, -1 on error.
 */
int bd_session_user_trigger_add(bd_session *s, bd_trigger_type type,
                                const char *pattern, const char *body,
                                const char *cls, int priority, int stop);
int bd_session_user_trigger_remove(bd_session *s, bd_trigger_type type,
                                   const char *pattern, const char *cls);

/* Number of user triggers currently held. */
int bd_session_user_trigger_count(const bd_session *s);

/* Write the user-trigger list to triggers.csv (creating the profile dir).
 * Returns 0 on success, -1 if there is no data dir / profile name or on I/O
 * error. An empty list writes a header-only file. */
int bd_session_save_triggers(bd_session *s);

/* Load triggers.csv (if present) and install each entry as a user trigger.
 * Call once during session setup, after the data dir is set, alongside
 * bd_session_load_profile_script. Returns the number loaded, or -1 on error;
 * 0 when no file exists. */
int bd_session_load_triggers(bd_session *s);

/* Pull queued network output and fire events. Call once per frame (UI thread). */
void bd_session_drain(bd_session *s);

/*
 * Replay the last `max` logged inbound lines for this profile through the
 * front-end as BD_SESSION_DATA events with `replay` set, oldest-first, each
 * terminated with CRLF. Triggers and on.* hooks do NOT fire on replayed lines
 * (doc/network.md). Needs the data dir set (that is where logs live); a no-op
 * otherwise. Returns the number of lines replayed, or -1. Intended for "show
 * last N lines on reconnect" and the scrollback viewer.
 */
int bd_session_replay(bd_session *s, int max);

#endif /* BD_SESSION_H */
