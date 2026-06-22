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
	bd_vm *vm;
	bd_triggers *trig;
	bd_session_event_fn on_event;
	void *userdata;
	int cols, rows;

	/* incoming-line assembly for the trigger engine (UI thread only) */
	char *linebuf;
	size_t line_len, line_cap;
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

/* The trigger engine emits MUD commands through here. Send raw (with CRLF) so
 * a fired command does not recurse back through the aliases. */
static void
trigger_send(const char *cmd, void *ctx)
{
	bd_session *s = ctx;
	bd_net_send(s->net, cmd, (int)strlen(cmd));
	bd_net_send(s->net, "\r\n", 2);
}

/* Host function exposed to scripts: __bd_send(text) -> send a command line.
 * The mud.send(text) Lua wrapper (installed in the bootstrap below) calls it,
 * so '@' trigger bodies and #script can talk to the MUD. */
static int
host_send(bd_vm *vm, int argc, const bd_vm_val *argv, bd_vm_val *ret, void *ud)
{
	bd_session *s = ud;
	(void)vm;
	(void)ret;
	if (argc >= 1 && argv[0].type == BD_VM_STR && argv[0].u.s) {
		bd_net_send(s->net, argv[0].u.s, (int)strlen(argv[0].u.s));
		bd_net_send(s->net, "\r\n", 2);
	}
	return 0;
}

/* Host function: __bd_class(name, enable) -> toggle a trigger class.
 * enable true=enable, false=disable, nil=toggle. */
static int
host_class(bd_vm *vm, int argc, const bd_vm_val *argv, bd_vm_val *ret, void *ud)
{
	bd_session *s = ud;
	const char *n;
	(void)vm;
	(void)ret;
	if (argc < 1 || argv[0].type != BD_VM_STR)
		return 0;
	n = argv[0].u.s;
	if (argc >= 2 && argv[1].type == BD_VM_BOOL) {
		if (argv[1].u.b)
			bd_class_enable(s->trig, n);
		else
			bd_class_disable(s->trig, n);
	} else {                                /* nil -> toggle */
		if (bd_class_enabled(s->trig, n))
			bd_class_disable(s->trig, n);
		else
			bd_class_enable(s->trig, n);
	}
	return 0;
}

/*
 * The script-facing API, built in Lua over the flat host functions, so the C
 * seam stays scalar. mud.send and class.* reach the host; the on.* hook tables
 * let scripts react to events alongside the #action/#alias triggers, dispatched
 * from C via __bd_dispatch. Each handler runs in pcall so one bad hook does not
 * abort the rest. GMCP/MSDP payloads are JSON-decoded (json.decode) to a Lua
 * table before the on.gmcp handler is called. Deferred (doc/triggers.md):
 * mud.gmcp (send), log.note, the var table, on.timer/on.mxp.
 */
static const char bootstrap_lua[] =
	"mud = mud or {}\n"
	"function mud.send(s) __bd_send(tostring(s)) end\n"
	"class = class or {}\n"
	"function class.enable(n) __bd_class(n, true) end\n"
	"function class.disable(n) __bd_class(n, false) end\n"
	"function class.toggle(n) __bd_class(n, nil) end\n"
	/* a compact recursive-descent JSON decoder. byte-code comparisons keep
	 * the source free of quote/backslash escaping; json.decode returns the
	 * value or nil on malformed input. Used to hand GMCP/MSDP payloads to
	 * scripts as Lua tables. */
	"local function jdec(s)\n"
	" local p=1\n"
	" local function sk() while p<=#s do local b=s:byte(p)\n"
	"   if b==32 or b==9 or b==10 or b==13 then p=p+1 else break end end end\n"
	" local pv\n"
	" local function pstr()\n"
	"  p=p+1; local t={}\n"
	"  while p<=#s do local b=s:byte(p)\n"
	"   if b==34 then p=p+1; return table.concat(t) end\n"
	"   if b==92 then p=p+1; local e=s:byte(p)\n"
	"    if e==110 then t[#t+1]=string.char(10)\n"
	"    elseif e==116 then t[#t+1]=string.char(9)\n"
	"    elseif e==114 then t[#t+1]=string.char(13)\n"
	"    elseif e==98 then t[#t+1]=string.char(8)\n"
	"    elseif e==102 then t[#t+1]=string.char(12)\n"
	"    elseif e==117 then local h=tonumber(s:sub(p+1,p+4),16) or 63; p=p+4\n"
	"     if h<128 then t[#t+1]=string.char(h)\n"
	"     elseif h<2048 then t[#t+1]=string.char(192+h//64,128+h%64)\n"
	"     else t[#t+1]=string.char(224+h//4096,128+(h//64)%64,128+h%64) end\n"
	"    else t[#t+1]=string.char(e) end\n"
	"    p=p+1\n"
	"   else t[#t+1]=string.char(b); p=p+1 end end\n"
	"  error('eos')\n"
	" end\n"
	" pv=function()\n"
	"  sk(); local b=s:byte(p)\n"
	"  if b==123 then p=p+1; local o={}; sk(); if s:byte(p)==125 then p=p+1; return o end\n"
	"   while true do sk(); local k=pstr(); sk()\n"
	"    if s:byte(p)~=58 then error('colon') end; p=p+1; o[k]=pv(); sk()\n"
	"    local d=s:byte(p)\n"
	"    if d==44 then p=p+1 elseif d==125 then p=p+1; return o else error('obj') end end\n"
	"  elseif b==91 then p=p+1; local a={}; sk(); if s:byte(p)==93 then p=p+1; return a end\n"
	"   while true do a[#a+1]=pv(); sk(); local d=s:byte(p)\n"
	"    if d==44 then p=p+1 elseif d==93 then p=p+1; return a else error('arr') end end\n"
	"  elseif b==34 then return pstr()\n"
	"  elseif b==116 then p=p+4; return true\n"
	"  elseif b==102 then p=p+5; return false\n"
	"  elseif b==110 then p=p+4; return nil\n"
	"  else local st=p\n"
	"   while p<=#s do local c=s:byte(p)\n"
	"    if (c>=48 and c<=57) or c==45 or c==43 or c==46 or c==101 or c==69 then p=p+1 else break end end\n"
	"   return tonumber(s:sub(st,p-1)) end\n"
	" end\n"
	" return pv()\n"
	"end\n"
	"json = json or {}\n"
	"function json.decode(s) local ok,v=pcall(jdec,s); if ok then return v end return nil end\n"
	"on = on or { line={}, prompt={}, connect={}, disconnect={}, gmcp={} }\n"
	"function __bd_dispatch(kind, a, b)\n"
	"  if kind=='line' then for _,f in ipairs(on.line) do pcall(f,a) end\n"
	"  elseif kind=='prompt' then for _,f in ipairs(on.prompt) do pcall(f,a) end\n"
	"  elseif kind=='connect' then for _,f in ipairs(on.connect) do pcall(f) end\n"
	"  elseif kind=='disconnect' then for _,f in ipairs(on.disconnect) do pcall(f) end\n"
	/* hand GMCP/MSDP handlers the decoded table (nil if the payload was not
	 * valid JSON) */
	"  elseif kind=='gmcp' then local h=on.gmcp[a]\n"
	"   if h then pcall(h, json.decode(b)) end end\n"
	"end\n";

static void
install_script_api(bd_session *s)
{
	bd_vm_register(s->vm, "__bd_send", host_send, s);
	bd_vm_register(s->vm, "__bd_class", host_class, s);
	bd_vm_eval(s->vm, bootstrap_lua);   /* no-op on the null backend */
}

/* Fire the Lua on.* hooks for an event (no-op if scripting is disabled or the
 * dispatcher is absent). Always passes three string args; unused ones empty. */
static void
dispatch_hook(bd_session *s, const char *kind, const char *a, const char *b)
{
	bd_vm_call(s->vm, "__bd_dispatch", "sss", kind, a ? a : "", b ? b : "");
}

/* Copy `src` to `dst` (cap incl. NUL) dropping ANSI/VT escape sequences, so
 * triggers match on plain text. Handles CSI (ESC [ ... final) and a lone ESC +
 * next byte; other control bytes except tab are dropped. */
static void
strip_ansi(const char *src, size_t len, char *dst, size_t cap)
{
	size_t i, o = 0;
	for (i = 0; i < len && o + 1 < cap; i++) {
		unsigned char ch = (unsigned char)src[i];
		if (ch == 0x1b) {               /* ESC */
			if (i + 1 < len && src[i + 1] == '[') {
				i += 2;
				while (i < len && (src[i] < 0x40 || src[i] > 0x7e))
					i++;            /* params/intermediates */
				/* i now at the final byte; loop's i++ skips it */
			} else {
				i++;                    /* ESC + one byte */
			}
			continue;
		}
		if (ch < 0x20 && ch != '\t')
			continue;               /* drop other control bytes */
		dst[o++] = (char)ch;
	}
	dst[o] = '\0';
}

/* Run one assembled raw line through the action triggers (ANSI stripped). */
static void
dispatch_line(bd_session *s, const char *raw, size_t len)
{
	char plain[4096];
	strip_ansi(raw, len, plain, sizeof plain);
	bd_triggers_line(s->trig, plain);       /* #action triggers */
	dispatch_hook(s, "line", plain, NULL);  /* on.line hooks */
}

/* Append received bytes to the line buffer and dispatch each completed line
 * (split on '\n', trailing '\r' trimmed). The unterminated remainder stays
 * buffered and is also the pending prompt text. */
static void
feed_lines(bd_session *s, const char *data, int len)
{
	int i;
	for (i = 0; i < len; i++) {
		char c = data[i];
		if (c == '\n') {
			size_t n = s->line_len;
			if (n > 0 && s->linebuf[n - 1] == '\r')
				n--;
			dispatch_line(s, s->linebuf, n);
			s->line_len = 0;
			continue;
		}
		if (s->line_len + 1 > s->line_cap) {
			size_t nc = s->line_cap ? s->line_cap * 2 : 256;
			char *nb = realloc(s->linebuf, nc);
			if (!nb)
				continue;       /* drop the byte rather than grow */
			s->linebuf = nb;
			s->line_cap = nc;
		}
		s->linebuf[s->line_len++] = c;
	}
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
	emit(s, &ev);                   /* terminal display gets the raw bytes */
	feed_lines(s, data, len);       /* trigger engine gets assembled lines */
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

	if (state == BD_NET_CONNECTED)
		dispatch_hook(s, "connect", NULL, NULL);
	else if (state == BD_NET_CLOSED || state == BD_NET_ERROR)
		dispatch_hook(s, "disconnect", NULL, NULL);
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

	/* the buffered, unterminated text is the prompt: match it (ANSI
	 * stripped) against prompt triggers, then consume it so it does not
	 * merge with the next line */
	if (s->line_len > 0) {
		char plain[4096];
		strip_ansi(s->linebuf, s->line_len, plain, sizeof plain);
		bd_triggers_prompt(s->trig, plain);
		dispatch_hook(s, "prompt", plain, NULL);
		s->line_len = 0;
	}
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

	/* route GMCP/MSDP packages to gmcp-type triggers and on.gmcp hooks */
	bd_triggers_gmcp(s->trig, name, json);
	dispatch_hook(s, "gmcp", name, json);
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
	s->vm = bd_vm_new(&bd_vm_lua);           /* Lua 5.4 + LPeg */
	s->trig = bd_triggers_new(s->vm, trigger_send, s);
	if (!s->net || !s->vm || !s->trig) {
		bd_triggers_free(s->trig);
		bd_vm_free(s->vm);
		bd_net_free(s->net);
		free(s);
		return NULL;
	}
	install_script_api(s);
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
	/* reverse of build order: net first (stops callbacks), then the engine
	 * it fed, then the vm that engine used */
	bd_net_free(s->net);
	bd_triggers_free(s->trig);
	bd_vm_free(s->vm);
	free(s->linebuf);
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
	/* an alias may consume the input and emit its own commands */
	if (bd_triggers_input(s->trig, utf8))
		return 0;
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

bd_triggers *
bd_session_triggers(bd_session *s)
{
	return s ? s->trig : NULL;
}

bd_vm *
bd_session_vm(bd_session *s)
{
	return s ? s->vm : NULL;
}

void
bd_session_drain(bd_session *s)
{
	if (s)
		bd_net_poll(s->net);
}
