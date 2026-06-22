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

/* Pull queued network output and fire events. Call once per frame (UI thread). */
void bd_session_drain(bd_session *s);

#endif /* BD_SESSION_H */
