#ifndef BD_SESSION_H
#define BD_SESSION_H

#include "bd_net.h"
#include "bd_profile.h"
#include <stddef.h>

/*
 * bd_session -- one connected MUD; the front-end seam (doc/core.md).
 *
 * A session owns the transport (bd_net) and connects using a borrowed
 * bd_profile (host/port/tls/autoreconnect). Everything the net thread
 * produces is delivered to the front-end as bd_session_event callbacks on
 * the UI thread, during bd_session_drain().
 *
 * This is the current, transport-stage slice of the design's seam. The
 * vt_buf grid, line retirement, trigger engine, scripting VM, and log sinks
 * are not built yet; when they land they hang off bd_session without changing
 * this front-end contract. Until then a session forwards decoded bytes as a
 * DATA event so the front-end can feed its own terminal widget.
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

/* Send a command line (CRLF appended) or raw bytes. Return bytes accepted,
 * or -1 if not connected. */
int bd_session_send_line(bd_session *s, const char *utf8);
int bd_session_send_raw(bd_session *s, const void *bytes, size_t n);

/* Report the terminal size to the server (NAWS). */
void bd_session_set_winsize(bd_session *s, int cols, int rows);

/* Pull queued network output and fire events. Call once per frame (UI thread). */
void bd_session_drain(bd_session *s);

#endif /* BD_SESSION_H */
