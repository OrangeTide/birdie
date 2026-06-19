/*
 * bd_session -- one connected MUD. See bd_session.h and doc/core.md.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_session.h"

#include <stdlib.h>
#include <string.h>

struct bd_session {
	const bd_profile *profile;      /* borrowed */
	bd_net *net;
	bd_session_event_fn on_event;
	void *userdata;
	int cols, rows;
};

/* truthy values for yes/no-style profile fields */
static int
prop_bool(const bd_profile *p, const char *key, int dflt)
{
	const char *v = bd_profile_get(p, key);
	if (!v)
		return dflt;
	if (!strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "1") ||
	    !strcmp(v, "on"))
		return 1;
	if (!strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "0") ||
	    !strcmp(v, "off"))
		return 0;
	return dflt;
}

static void
emit(bd_session *s, const bd_session_event *ev)
{
	if (s->on_event)
		s->on_event(s, ev, s->userdata);
}

/* ---- bd_net callbacks (fire on the UI thread during drain) ---- */

static void
net_data(const char *data, int len, void *arg)
{
	bd_session *s = arg;
	bd_session_event ev = { 0 };
	ev.kind = BD_SESSION_DATA;
	ev.data = data;
	ev.len = len;
	emit(s, &ev);
}

static void
net_state(bd_net_state state, const char *msg, void *arg)
{
	bd_session *s = arg;
	bd_session_event ev = { 0 };
	ev.kind = BD_SESSION_STATE;
	ev.state = state;
	ev.detail = msg;
	emit(s, &ev);
}

static void
net_echo(int suppress, void *arg)
{
	bd_session *s = arg;
	bd_session_event ev = { 0 };
	ev.kind = BD_SESSION_ECHO;
	ev.echo_suppress = suppress;
	emit(s, &ev);
}

static void
net_prompt(void *arg)
{
	bd_session *s = arg;
	bd_session_event ev = { 0 };
	ev.kind = BD_SESSION_PROMPT;
	emit(s, &ev);
}

static void
net_package(int proto, const char *name, const char *json, void *arg)
{
	bd_session *s = arg;
	bd_session_event ev = { 0 };
	ev.kind = BD_SESSION_PACKAGE;
	ev.proto = proto;
	ev.name = name;
	ev.json = json;
	emit(s, &ev);
}

/* ---- API ---- */

bd_session *
bd_session_new(const bd_profile *profile)
{
	bd_session *s = calloc(1, sizeof *s);
	if (!s)
		return NULL;
	s->profile = profile;
	s->cols = 80;
	s->rows = 24;
	s->net = bd_net_new(net_data, net_state, s);
	if (!s->net) {
		free(s);
		return NULL;
	}
	bd_net_set_echo_cb(s->net, net_echo);
	bd_net_set_prompt_cb(s->net, net_prompt);
	bd_net_set_package_cb(s->net, net_package);
	return s;
}

void
bd_session_free(bd_session *s)
{
	if (!s)
		return;
	bd_net_free(s->net);            /* reverse of build order */
	free(s);
}

void
bd_session_on_event(bd_session *s, bd_session_event_fn fn, void *userdata)
{
	if (!s)
		return;
	s->on_event = fn;
	s->userdata = userdata;
}

int
bd_session_connect(bd_session *s)
{
	const char *host, *port, *ttype;
	int tls;

	if (!s || !s->profile)
		return -1;
	host = bd_profile_get(s->profile, "host");
	port = bd_profile_get(s->profile, "port");
	if (!host || !*host || !port || !*port)
		return -1;
	tls = prop_bool(s->profile, "tls", 0);

	bd_net_set_autoreconnect(s->net, prop_bool(s->profile, "autoreconnect", 1));
	ttype = bd_profile_get(s->profile, "termtype");
	bd_net_set_termtype(s->net, ttype ? ttype : "birdie/0.0");
	bd_net_set_winsize(s->net, s->cols, s->rows);

	return bd_net_connect(s->net, host, port, tls);
}

void
bd_session_disconnect(bd_session *s)
{
	if (s)
		bd_net_close(s->net);
}

bd_net_state
bd_session_state(const bd_session *s)
{
	return s ? bd_net_state_get(s->net) : BD_NET_IDLE;
}

int
bd_session_send_line(bd_session *s, const char *utf8)
{
	int n;

	if (!s || !utf8)
		return -1;
	n = bd_net_send(s->net, utf8, (int)strlen(utf8));
	if (n < 0)
		return -1;
	bd_net_send(s->net, "\r\n", 2);
	return n;
}

int
bd_session_send_raw(bd_session *s, const void *bytes, size_t n)
{
	if (!s)
		return -1;
	return bd_net_send(s->net, bytes, (int)n);
}

void
bd_session_set_winsize(bd_session *s, int cols, int rows)
{
	if (!s || cols <= 0 || rows <= 0)
		return;
	s->cols = cols;
	s->rows = rows;
	bd_net_set_winsize(s->net, cols, rows);
}

void
bd_session_drain(bd_session *s)
{
	if (s)
		bd_net_poll(s->net);
}
