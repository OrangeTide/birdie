/*
 * Headless integration test for bd_session -- the seam that wires the network
 * transport to line assembly, the trigger engine, the scripting hooks, and the
 * front-end event stream.
 *
 * bd_session talks to the network only through bd_net's opaque public API, so
 * this test links a FAKE bd_net (below) in place of the real socket/thread/TLS
 * stack. The fake stores the callbacks bd_session registers and lets the test
 * inject decoded server bytes and telnet events (data / prompt / echo / package
 * / state / mxp), then asserts what the session did: which front-end events it
 * emitted, which triggers fired, and what it sent back "to the server". This
 * exercises the real bd_session.c pipeline end to end, deterministically, with
 * no sockets, threads, or Lua.
 *
 * bd_session_new() creates its VM on the Lua backend; to avoid linking Lua/LPeg
 * a no-op `bd_vm_lua` backend is defined here (command-body triggers, the focus
 * of these tests, work without a live interpreter; '@' bodies and on.* hooks are
 * inert). Exit code 0 = all checks passed. Run via `make test`.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */
#include "bd_session.h"
#include "bd_net.h"
#include "bd_vm.h"
#include "bd_trigger.h"
#include "bd_profile.h"
#include "bd_telopt.h"   /* BD_TELOPT_GMCP */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* rmdir */

/* ---- test harness (matches test_gui.c / test_client.c) ---- */
static int checks, fails;
static void
check(const char *what, int ok)
{
	checks++;
	if (!ok) fails++;
	printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
}

/* ================================================================== */
/* Fake bd_net: records outbound bytes, replays the stored callbacks    */
/* ================================================================== */
struct bd_net {
	bd_net_data_cb    on_data;
	bd_net_state_cb   on_state;
	bd_net_echo_cb    echo;
	bd_net_prompt_cb  prompt;
	bd_net_package_cb package;
	bd_net_mxp_cb     mxp;
	void             *arg;
	bd_net_state      state;
};

static struct bd_net *g_net;          /* the session's one fake transport */
static char   g_sent[4096];           /* bytes the session sent "to the server" */
static size_t g_sent_n;
static void sent_clear(void) { g_sent_n = 0; g_sent[0] = '\0'; }

bd_net *
bd_net_new(bd_net_data_cb on_data, bd_net_state_cb on_state, void *arg)
{
	struct bd_net *n = calloc(1, sizeof *n);
	if (!n) return NULL;
	n->on_data = on_data;
	n->on_state = on_state;
	n->arg = arg;
	n->state = BD_NET_IDLE;
	g_net = n;
	return n;
}
void bd_net_free(bd_net *n) { if (n == g_net) g_net = NULL; free(n); }
void bd_net_set_echo_cb(bd_net *n, bd_net_echo_cb cb) { n->echo = cb; }
void bd_net_set_prompt_cb(bd_net *n, bd_net_prompt_cb cb) { n->prompt = cb; }
void bd_net_set_package_cb(bd_net *n, bd_net_package_cb cb) { n->package = cb; }
void bd_net_set_mxp_cb(bd_net *n, bd_net_mxp_cb cb) { n->mxp = cb; }
void bd_net_set_autoreconnect(bd_net *n, int e) { (void)n; (void)e; }
void bd_net_set_encoding(bd_net *n, const char *s) { (void)n; (void)s; }
void bd_net_set_termtype(bd_net *n, const char *t) { (void)n; (void)t; }
void bd_net_set_winsize(bd_net *n, int c, int r) { (void)n; (void)c; (void)r; }
void bd_net_poll(bd_net *n) { (void)n; }              /* the test injects directly */
bd_net_state bd_net_state_get(const bd_net *n) { return n->state; }

int
bd_net_connect(bd_net *n, const char *host, const char *port, int tls)
{
	(void)host; (void)port; (void)tls;
	n->state = BD_NET_CONNECTED;   /* synchronous, so send_line works */
	return 0;
}
void bd_net_close(bd_net *n) { n->state = BD_NET_CLOSED; }

int
bd_net_send(bd_net *n, const void *data, int len)
{
	if (n->state != BD_NET_CONNECTED) return -1;
	if (len < 0) return -1;
	if (g_sent_n + (size_t)len < sizeof g_sent) {
		memcpy(g_sent + g_sent_n, data, (size_t)len);
		g_sent_n += (size_t)len;
		g_sent[g_sent_n] = '\0';
	}
	return len;
}

int
bd_net_send_gmcp(bd_net *n, const char *pkg, const char *json)
{
	(void)n;
	int len = snprintf(g_sent + g_sent_n, sizeof g_sent - g_sent_n,
	    "GMCP:%s %s", pkg ? pkg : "", json ? json : "");
	if (len > 0) g_sent_n += (size_t)len;
	return 0;
}

/* ---- helpers the test uses to play "the server" ---- */
static void net_fire_state(bd_net_state st, const char *msg)
{ if (g_net && g_net->on_state) g_net->on_state(st, msg, g_net->arg); }
static void net_recv(const char *s)
{ if (g_net && g_net->on_data) g_net->on_data(s, (int)strlen(s), g_net->arg); }
static void net_fire_prompt(void)
{ if (g_net && g_net->prompt) g_net->prompt(g_net->arg); }
static void net_fire_echo(int suppress)
{ if (g_net && g_net->echo) g_net->echo(suppress, g_net->arg); }
static void net_fire_package(int proto, const char *name, const char *json)
{ if (g_net && g_net->package) g_net->package(proto, name, json, g_net->arg); }
static void net_fire_mxp(int active)
{ if (g_net && g_net->mxp) g_net->mxp(active, g_net->arg); }

/* ================================================================== */
/* Stub Lua backend so bd_session_new links without Lua/LPeg           */
/* ================================================================== */
static void *lv_create(void) { return (void *)1; }   /* non-NULL = "ok" */
static void  lv_destroy(void *i) { (void)i; }
static int   lv_eval(void *i, const char *s) { (void)i; (void)s; return 0; }
static int   lv_call(void *i, const char *n, int c, const bd_vm_val *a)
{ (void)i; (void)n; (void)c; (void)a; return -1; }   /* no script hooks */
static void  lv_reg(void *i, const char *n, bd_host_fn f, void *u, bd_vm *v)
{ (void)i; (void)n; (void)f; (void)u; (void)v; }
static const char *lv_error(void *i) { (void)i; return ""; }
const bd_vm_backend bd_vm_lua = {
	"lua-stub", lv_create, lv_destroy, lv_eval, lv_call, lv_reg, lv_error
};

/* ================================================================== */
/* Front-end event recorder                                            */
/* ================================================================== */
static char ev_data[4096];   /* concatenation of all DATA event bytes */
static size_t ev_data_n;
static int  ev_state_n, ev_last_state;
static int  ev_echo_n, ev_echo_last;
static int  ev_prompt_n;
static int  ev_pkg_n, ev_pkg_proto;
static char ev_pkg_name[64], ev_pkg_json[64];
static int  ev_edit_n;
static char ev_edit_ref[128], ev_edit_name[64], ev_edit_type[32], ev_edit_content[512];

static void
on_event(bd_session *s, const bd_session_event *ev, void *ud)
{
	(void)s; (void)ud;
	switch (ev->kind) {
	case BD_SESSION_STATE: ev_state_n++; ev_last_state = ev->state; break;
	case BD_SESSION_DATA:
		if (ev->len > 0 && ev_data_n + (size_t)ev->len < sizeof ev_data) {
			memcpy(ev_data + ev_data_n, ev->data, (size_t)ev->len);
			ev_data_n += (size_t)ev->len;
			ev_data[ev_data_n] = '\0';
		}
		break;
	case BD_SESSION_ECHO: ev_echo_n++; ev_echo_last = ev->echo_suppress; break;
	case BD_SESSION_PROMPT: ev_prompt_n++; break;
	case BD_SESSION_PACKAGE:
		ev_pkg_n++; ev_pkg_proto = ev->proto;
		snprintf(ev_pkg_name, sizeof ev_pkg_name, "%s", ev->name ? ev->name : "");
		snprintf(ev_pkg_json, sizeof ev_pkg_json, "%s", ev->json ? ev->json : "");
		break;
	case BD_SESSION_MCP_EDIT: {
		int n = ev->len < (int)sizeof ev_edit_content - 1
		    ? ev->len : (int)sizeof ev_edit_content - 1;
		ev_edit_n++;
		snprintf(ev_edit_ref, sizeof ev_edit_ref, "%s", ev->reference ? ev->reference : "");
		snprintf(ev_edit_name, sizeof ev_edit_name, "%s", ev->name ? ev->name : "");
		snprintf(ev_edit_type, sizeof ev_edit_type, "%s", ev->edit_type ? ev->edit_type : "");
		if (n > 0) memcpy(ev_edit_content, ev->data, (size_t)n);
		ev_edit_content[n > 0 ? n : 0] = 0;
		break; }
	}
}
static void ev_clear_data(void) { ev_data_n = 0; ev_data[0] = '\0'; }

int
main(void)
{
	printf("bd_session:\n");

	bd_profiles *ps = bd_profiles_new();
	bd_profile *p = bd_profiles_add(ps, "Test");
	bd_profile_set(p, "host", "localhost");
	bd_profile_set(p, "port", "4000");

	bd_session *s = bd_session_new(p);
	check("session created", s != NULL);
	bd_session_on_event(s, on_event, NULL);
	bd_triggers *tr = bd_session_triggers(s);
	check("session exposes its trigger engine", tr != NULL);

	check("connect starts the attempt", bd_session_connect(s) == 0);

	/* state events reach the front-end */
	net_fire_state(BD_NET_CONNECTED, "ok");
	check("a connection-state change surfaces as a STATE event",
	    ev_state_n == 1 && ev_last_state == BD_NET_CONNECTED);

	/* a received line assembles, streams as DATA, and fires an action */
	bd_trigger_add(tr, BD_TRIG_ACTION, "You are hungry", "eat bread",
	    NULL, -1, 0);
	sent_clear(); ev_clear_data();
	net_recv("You are hungry now\r\n");
	check("received text is emitted to the front-end as DATA",
	    strstr(ev_data, "You are hungry now") != NULL);
	check("a completed line fires its action and the command is sent",
	    strcmp(g_sent, "eat bread\r\n") == 0);

	/* triggers match the ANSI-stripped text, while the raw bytes still stream */
	bd_trigger_add(tr, BD_TRIG_ACTION, "it smells good", "sniff", NULL, -1, 0);
	sent_clear(); ev_clear_data();
	net_recv("\033[32mit smells good\033[0m\r\n");
	check("a trigger matches through ANSI color codes",
	    strcmp(g_sent, "sniff\r\n") == 0);
	check("the colored raw bytes are still streamed to the display",
	    strstr(ev_data, "\033[32m") != NULL);

	/* a partial line stays buffered until its newline arrives */
	bd_trigger_add(tr, BD_TRIG_ACTION, "the end", "cheer", NULL, -1, 0);
	sent_clear();
	net_recv("the ");
	check("a partial line does not fire yet", g_sent[0] == '\0');
	net_recv("end\r\n");
	check("the line fires once completed across two receives",
	    strcmp(g_sent, "cheer\r\n") == 0);

	/* outgoing input: an alias consumes it; an ordinary line is sent verbatim */
	bd_trigger_add(tr, BD_TRIG_ALIAS, "kk", "kill kobold", NULL, -1, 0);
	sent_clear();
	check("send_line reports an alias consumed the input",
	    bd_session_send_line(s, "kk") == 0);
	check("the alias body is sent, not the typed input",
	    strcmp(g_sent, "kill kobold\r\n") == 0);
	sent_clear();
	int n = bd_session_send_line(s, "look");
	check("an un-aliased line is sent verbatim with CRLF",
	    n == 4 && strcmp(g_sent, "look\r\n") == 0);

	/* #gag rewrites the retired line: the streamed row is erased */
	bd_trigger_add(tr, BD_TRIG_GAG, "SPAM", "", NULL, -1, 0);
	ev_clear_data();
	net_recv("buy our SPAM\r\n");
	check("a gagged line emits the erase sequence to the display",
	    strstr(ev_data, "\033[2K") != NULL);

	/* prompt boundary: a PROMPT event fires and prompt triggers run on the
	 * buffered (unterminated) text */
	bd_trigger_add(tr, BD_TRIG_PROMPT, "HP", "checkhp", NULL, -1, 0);
	sent_clear();
	net_recv("HP: 50/50 >");        /* no newline: this is the prompt */
	net_fire_prompt();
	check("a telnet prompt boundary surfaces as a PROMPT event",
	    ev_prompt_n == 1);
	check("prompt triggers fire on the buffered prompt text",
	    strcmp(g_sent, "checkhp\r\n") == 0);

	/* server echo control -> ECHO events */
	net_fire_echo(1);
	check("server echo-on suppresses local echo via an ECHO event",
	    ev_echo_n == 1 && ev_echo_last == 1);
	net_fire_echo(0);
	check("server echo-off restores local echo",
	    ev_echo_n == 2 && ev_echo_last == 0);

	/* GMCP package -> PACKAGE event and gmcp triggers */
	bd_trigger_add(tr, BD_TRIG_GMCP, "Char.Vitals", "gotvitals", NULL, -1, 0);
	sent_clear();
	net_fire_package(BD_TELOPT_GMCP, "Char.Vitals", "{\"hp\":100}");
	check("an out-of-band package surfaces as a PACKAGE event",
	    ev_pkg_n == 1 && strcmp(ev_pkg_name, "Char.Vitals") == 0 &&
	    strcmp(ev_pkg_json, "{\"hp\":100}") == 0);
	check("gmcp triggers fire on the package name",
	    strcmp(g_sent, "gotvitals\r\n") == 0);

	/* MXP: once active, received bytes are parsed for tags, whose text flows
	 * into the normal line pipeline and whose tags fire mxp triggers */
	bd_trigger_add(tr, BD_TRIG_MXP, "b", "sawbold", NULL, -1, 0);
	net_fire_mxp(1);
	sent_clear(); ev_clear_data();
	net_recv("<b>bold</b> text\r\n");
	check("with MXP active the tag text is cleaned into the display",
	    strstr(ev_data, "bold") != NULL && strstr(ev_data, "<b>") == NULL);
	check("mxp triggers fire on the parsed tag",
	    strcmp(g_sent, "sawbold\r\n") == 0);


	/* MCP: the in-band #$# protocol is intercepted in the line pipeline */
	net_fire_mxp(0);                        /* plain line pipeline for this part */
	ev_clear_data(); sent_clear(); ev_edit_n = 0;
	net_recv("#$#mcp version: 2.1 to: 2.1\r\n");
	check("the mcp handshake line is not displayed",
	    strstr(ev_data, "#$#") == NULL);
	check("the handshake is answered with an auth key + negotiation",
	    strstr(g_sent, "#$#mcp authentication-key:") != NULL &&
	    strstr(g_sent, "package: dns-org-mud-moo-simpleedit") != NULL &&
	    strstr(g_sent, "#$#mcp-negotiate-end") != NULL);
	{
		char mkey[64] = "", buf[512];
		const char *p = strstr(g_sent, "authentication-key: ");
		if (p) sscanf(p + 20, "%63s", mkey);

		/* a #$" quoted ordinary line displays with the marker stripped */
		ev_clear_data();
		net_recv("#$\"#$#looks-like-mcp\r\n");
		check("a quoted line displays without its #$\" marker",
		    strstr(ev_data, "#$#looks-like-mcp") != NULL &&
		    strstr(ev_data, "#$\"") == NULL);

		/* simpleedit-content assembles across multiline and fires an event */
		sent_clear(); ev_edit_n = 0;
		snprintf(buf, sizeof buf,
		    "#$#dns-org-mud-moo-simpleedit-content %s reference: \"#7:desc\" "
		    "name: \"desc\" type: string-list content*: \"\" _data-tag: e1\r\n",
		    mkey);
		net_recv(buf);
		net_recv("#$#* e1 content: hello\r\n");
		net_recv("#$#* e1 content: world\r\n");
		net_recv("#$#: e1\r\n");
		check("simpleedit-content surfaces as an MCP_EDIT event",
		    ev_edit_n == 1 && strcmp(ev_edit_ref, "#7:desc") == 0 &&
		    strcmp(ev_edit_type, "string-list") == 0 &&
		    strcmp(ev_edit_content, "hello\nworld") == 0);

		/* sending the edit back emits a simpleedit-set with the body */
		sent_clear();
		bd_session_mcp_edit_done(s, "#7:desc", "string-list", "HELLO\nWORLD");
		check("edit_done sends a simpleedit-set with the edited lines",
		    strstr(g_sent, "dns-org-mud-moo-simpleedit-set") != NULL &&
		    strstr(g_sent, "content: HELLO") != NULL &&
		    strstr(g_sent, "content: WORLD") != NULL);
	}

	/* disconnect surfaces a state event */
	net_fire_state(BD_NET_CLOSED, "bye");
	check("a close surfaces as a STATE event",
	    ev_last_state == BD_NET_CLOSED);

	/* user-trigger persistence: add through the session API, save to
	 * triggers.csv, then reload in a fresh session and confirm it fires.
	 * (After this block frees its sessions the fake g_net is NULL, so it runs
	 * last, after the checks above that depend on the original session's net.) */
	{
		char tmpl[] = "/tmp/bd_sess_XXXXXX";
		char *dir = mkdtemp(tmpl);
		check("temp data dir created", dir != NULL);

		bd_profile *p2 = bd_profiles_add(ps, "Persist");
		bd_profile_set(p2, "host", "localhost");
		bd_profile_set(p2, "port", "4000");
		bd_session *sa = bd_session_new(p2);
		bd_session_on_event(sa, on_event, NULL);
		bd_session_set_data_dir(sa, dir);

		check("user trigger add reports success",
		    bd_session_user_trigger_add(sa, BD_TRIG_ALIAS, "gg",
		        "get gold", NULL, 7, 1) == 0);
		check("re-adding the same key does not grow the list",
		    (bd_session_user_trigger_add(sa, BD_TRIG_ALIAS, "gg",
		        "grab gold", NULL, 7, 1) == 0) &&
		    bd_session_user_trigger_count(sa) == 1);
		bd_session_user_trigger_add(sa, BD_TRIG_ACTION,
		    "you see a chest", "open chest", "loot", -1, 0);
		check("two distinct user triggers held",
		    bd_session_user_trigger_count(sa) == 2);
		check("save writes triggers.csv",
		    bd_session_save_triggers(sa) == 0);
		bd_session_free(sa);

		/* fresh session, same profile + data dir: load and fire */
		bd_session *sb = bd_session_new(p2);
		bd_session_on_event(sb, on_event, NULL);
		bd_session_set_data_dir(sb, dir);
		check("load restores both user triggers",
		    bd_session_load_triggers(sb) == 2 &&
		    bd_session_user_trigger_count(sb) == 2);
		check("connect starts the reloaded session",
		    bd_session_connect(sb) == 0);
		net_fire_state(BD_NET_CONNECTED, "ok");
		sent_clear();
		check("a reloaded alias body (last write wins) is sent",
		    bd_session_send_line(sb, "gg") == 0 &&
		    strcmp(g_sent, "grab gold\r\n") == 0);
		sent_clear();
		net_recv("you see a chest here\r\n");
		check("a reloaded action fires on a matching line",
		    strcmp(g_sent, "open chest\r\n") == 0);

		/* remove drops it from the list and the live engine */
		bd_session_user_trigger_remove(sb, BD_TRIG_ALIAS, "gg", NULL);
		check("remove shrinks the user list",
		    bd_session_user_trigger_count(sb) == 1);
		sent_clear();
		check("the removed alias no longer consumes input",
		    bd_session_send_line(sb, "gg") == 2);
		bd_session_free(sb);

		/* clean up the temp files */
		char path[1024];
		snprintf(path, sizeof path, "%s/profiles/Persist/triggers.csv", dir);
		remove(path);
		snprintf(path, sizeof path, "%s/profiles/Persist", dir);
		rmdir(path);
		snprintf(path, sizeof path, "%s/profiles", dir);
		rmdir(path);
		rmdir(dir);
	}

	bd_session_free(s);
	bd_profiles_free(ps);

	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
