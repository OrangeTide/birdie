/*
 * Headless functional tests for the birdie MUD-client core.
 *
 * The client logic (networking, telnet, triggers, profiles, CSV) is written as
 * socket-free, callback-driven state machines, so it tests without a display,
 * ludica, or a live connection. This binary compiles the pure-logic units
 * directly and drives them through their public APIs with recording callbacks.
 * Exit code 0 = all checks passed. Run via `make test`.
 *
 * Covered here: bd_ring, bd_csv, bd_telopt, bd_trigger, bd_profile. The
 * socket/thread layers (bd_net) and the scripting VM are exercised elsewhere.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */
#include "bd_ring.h"
#include "bd_csv.h"
#include "bd_telopt.h"
#include "bd_trigger.h"
#include "bd_verb.h"
#include "bd_mxp.h"
#include "bd_profile.h"
#include "bd_encoding.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- test harness (matches test_gui.c) ---- */
static int checks, fails;
static void
check(const char *what, int ok)
{
	checks++;
	if (!ok) fails++;
	printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
}

/* ================================================================== */
/* bd_ring -- single-producer/single-consumer byte ring               */
/* ================================================================== */
static void
test_ring(void)
{
	printf("bd_ring:\n");
	bd_ring *r = bd_ring_new(16);          /* rounds up to a power of two */
	check("ring created", r != NULL);
	check("fresh ring has nothing to read", bd_ring_read_avail(r) == 0);

	check("write succeeds", bd_ring_write(r, "hello", 5) == 0);
	check("read_avail reflects the write", bd_ring_read_avail(r) == 5);

	char buf[32] = {0};
	check("peek copies without consuming", bd_ring_peek(r, buf, 5) == 5 &&
	    memcmp(buf, "hello", 5) == 0 && bd_ring_read_avail(r) == 5);

	memset(buf, 0, sizeof buf);
	check("read copies and consumes", bd_ring_read(r, buf, 5) == 5 &&
	    memcmp(buf, "hello", 5) == 0 && bd_ring_read_avail(r) == 0);

	/* all-or-nothing: a write larger than capacity is rejected whole */
	size_t cap = bd_ring_write_avail(r);
	check("oversized write is rejected atomically",
	    bd_ring_write(r, "0123456789ABCDEF0123456789", 26) == -1 &&
	    bd_ring_write_avail(r) == cap);

	/* writev joins two segments as one record */
	check("writev joins segments", bd_ring_writev(r, "AB", 2, "CD", 2) == 0);
	memset(buf, 0, sizeof buf);
	check("joined record reads back whole",
	    bd_ring_read(r, buf, 4) == 4 && memcmp(buf, "ABCD", 4) == 0);

	/* wrap-around: repeated write/read past the buffer end stays correct */
	int ok = 1;
	for (int i = 0; i < 100; i++) {
		unsigned char in = (unsigned char)i, out = 0;
		if (bd_ring_write(r, &in, 1) != 0) { ok = 0; break; }
		if (bd_ring_read(r, &out, 1) != 1 || out != in) { ok = 0; break; }
	}
	check("write/read wraps around the ring correctly", ok);

	bd_ring_skip(r, bd_ring_read_avail(r));
	check("skip drains the ring", bd_ring_read_avail(r) == 0);
	bd_ring_free(r);
}

/* ================================================================== */
/* bd_csv -- RFC 4180-style reader/writer                             */
/* ================================================================== */
static void
test_csv(void)
{
	printf("bd_csv:\n");
	static const char *doc =
	    "name,host,port\r\n"
	    "Aardwolf,aardmud.org,23\r\n"
	    "\"Disc, world\",\"a\"\"b\",\"line1\nline2\"\r\n";
	bd_csv *c = bd_csv_parse(doc, strlen(doc));
	check("csv parses", c != NULL);
	check("row count excludes the trailing newline", bd_csv_rows(c) == 3);
	check("header cells", bd_csv_cols(c, 0) == 3 &&
	    strcmp(bd_csv_get(c, 0, 0), "name") == 0 &&
	    strcmp(bd_csv_get(c, 0, 2), "port") == 0);
	check("plain row", strcmp(bd_csv_get(c, 1, 1), "aardmud.org") == 0);
	check("quoted field keeps its comma",
	    strcmp(bd_csv_get(c, 2, 0), "Disc, world") == 0);
	check("doubled quotes decode to one",
	    strcmp(bd_csv_get(c, 2, 1), "a\"b") == 0);
	check("embedded newline survives",
	    strcmp(bd_csv_get(c, 2, 2), "line1\nline2") == 0);
	check("out-of-range get is NULL", bd_csv_get(c, 9, 0) == NULL);
	bd_csv_free(c);

	/* writer quotes only when needed, and round-trips through the reader */
	bd_csv_w *w = bd_csv_w_new();
	bd_csv_w_field(w, "plain");
	bd_csv_w_field(w, "has,comma");
	bd_csv_w_field(w, "has\"quote");
	bd_csv_w_endrow(w);
	size_t len = 0;
	const char *out = bd_csv_w_str(w, &len);
	check("writer quotes fields that need it and leaves others bare",
	    strstr(out, "plain,\"has,comma\",\"has\"\"quote\"") != NULL);
	bd_csv *rc = bd_csv_parse(out, len);
	check("writer output round-trips",
	    rc && bd_csv_rows(rc) == 1 && bd_csv_cols(rc, 0) == 3 &&
	    strcmp(bd_csv_get(rc, 0, 1), "has,comma") == 0 &&
	    strcmp(bd_csv_get(rc, 0, 2), "has\"quote") == 0);
	bd_csv_free(rc);
	bd_csv_w_free(w);
}

/* ================================================================== */
/* bd_telopt -- telnet option negotiation (socket-free)              */
/* ================================================================== */
/* telnet command bytes (mirrors bd_telopt.c internals) */
#define IAC 255
#define WILL 251
#define WONT 252
#define DO   253
#define DONT 254
#define O_ECHO 1
#define O_SGA  3

static unsigned char t_xmit[256];
static size_t        t_xmit_n;
static unsigned char t_data[256];
static size_t        t_data_n;
static int           t_echo_calls, t_echo_last;

static void tc_xmit(const unsigned char *p, size_t n, void *a)
{ (void)a; if (t_xmit_n + n <= sizeof t_xmit) { memcpy(t_xmit + t_xmit_n, p, n); t_xmit_n += n; } }
static void tc_data(const unsigned char *p, size_t n, void *a)
{ (void)a; if (t_data_n + n <= sizeof t_data) { memcpy(t_data + t_data_n, p, n); t_data_n += n; } }
static void tc_echo(int suppress, void *a) { (void)a; t_echo_calls++; t_echo_last = suppress; }

/* does the recorded xmit buffer contain the 3-byte sequence a,b,c ? */
static int
xmit_has(unsigned char a, unsigned char b, unsigned char c)
{
	for (size_t i = 0; i + 3 <= t_xmit_n; i++)
		if (t_xmit[i] == a && t_xmit[i+1] == b && t_xmit[i+2] == c)
			return 1;
	return 0;
}
static void reset_rec(void) { t_xmit_n = t_data_n = 0; t_echo_calls = 0; t_echo_last = -1; }

static void
feed(bd_telopt *t, const unsigned char *p, size_t n) { bd_telopt_recv(t, p, n); }

static void
test_telopt(void)
{
	printf("bd_telopt:\n");
	bd_telopt_cb cb = { .data = tc_data, .xmit = tc_xmit, .echo = tc_echo };
	bd_telopt *t = bd_telopt_new(&cb);
	check("telopt created", t != NULL);

	/* server DO SGA -> we accept: WILL SGA */
	reset_rec();
	{ unsigned char s[] = { IAC, DO, O_SGA }; feed(t, s, sizeof s); }
	check("DO SGA is answered WILL SGA", xmit_has(IAC, WILL, O_SGA));

	/* server DO <unsupported> -> WONT */
	reset_rec();
	{ unsigned char s[] = { IAC, DO, 99 }; feed(t, s, sizeof s); }
	check("DO of an unsupported option is refused (WONT)",
	    xmit_has(IAC, WONT, 99));

	/* server WILL <unwanted> -> DONT */
	reset_rec();
	{ unsigned char s[] = { IAC, WILL, 99 }; feed(t, s, sizeof s); }
	check("WILL of an unwanted option is refused (DONT)",
	    xmit_has(IAC, DONT, 99));

	/* server WILL ECHO -> DO ECHO + echo(suppress=1); WONT ECHO -> echo(0) */
	reset_rec();
	{ unsigned char s[] = { IAC, WILL, O_ECHO }; feed(t, s, sizeof s); }
	check("server WILL ECHO -> DO ECHO", xmit_has(IAC, DO, O_ECHO));
	check("server echo suppresses local echo (password masking)",
	    t_echo_calls == 1 && t_echo_last == 1);
	reset_rec();
	{ unsigned char s[] = { IAC, WONT, O_ECHO }; feed(t, s, sizeof s); }
	check("server WONT ECHO restores local echo",
	    t_echo_calls == 1 && t_echo_last == 0);

	/* application text passes through with IAC stripped; IAC IAC -> 0xFF */
	reset_rec();
	{ unsigned char s[] = { 'h', 'i', IAC, IAC, '!' }; feed(t, s, sizeof s); }
	check("plain text passes through, escaped IAC IAC becomes one 0xFF",
	    t_data_n == 4 && t_data[0] == 'h' && t_data[1] == 'i' &&
	    t_data[2] == 0xFF && t_data[3] == '!');

	bd_telopt_free(t);
}

/* ================================================================== */
/* bd_trigger -- action/alias/class/rewrite dispatch                  */
/* ================================================================== */
static char t_sent[16][256];
static int  t_sent_n;
static void trig_send(const char *cmd, void *ctx)
{ (void)ctx; if (t_sent_n < 16) snprintf(t_sent[t_sent_n++], 256, "%s", cmd); }
static void reset_sent(void) { t_sent_n = 0; }

static char t_ev_name[64], t_ev_arg[64];
static int  t_ev_calls;
static void trig_event(const char *name, const char *arg, void *ctx)
{ (void)ctx; t_ev_calls++; snprintf(t_ev_name, 64, "%s", name ? name : "");
  snprintf(t_ev_arg, 64, "%s", arg ? arg : ""); }

static void
test_trigger(void)
{
	printf("bd_trigger:\n");
	bd_triggers *t = bd_triggers_new(NULL, trig_send, NULL);   /* no VM: command bodies only */
	check("engine created", t != NULL);

	/* a plain action fires its command body */
	bd_trigger_add(t, BD_TRIG_ACTION, "You are hungry", "eat bread",
	    NULL, -1, 0);
	check("one trigger registered", bd_trigger_count(t) == 1);
	reset_sent();
	check("action fires on a matching line",
	    bd_triggers_line(t, "You are hungry.") == 1);
	check("its command body was sent",
	    t_sent_n == 1 && strcmp(t_sent[0], "eat bread") == 0);
	reset_sent();
	check("no fire on a non-matching line",
	    bd_triggers_line(t, "You are full.") == 0 && t_sent_n == 0);

	/* capture substitution: %1 in the body is the first pattern capture */
	bd_trigger_add(t, BD_TRIG_ACTION, "You get %1 gold", "stash %1",
	    NULL, -1, 0);
	reset_sent();
	bd_triggers_line(t, "You get 50 gold.");
	check("%1 capture is substituted into the body",
	    t_sent_n == 1 && strcmp(t_sent[0], "stash 50") == 0);

	/* an alias rewrites outgoing input and reports it fired */
	bd_trigger_add(t, BD_TRIG_ALIAS, "kk", "kill kobold", NULL, -1, 0);
	reset_sent();
	check("alias fires on matching input", bd_triggers_input(t, "kk") == 1);
	check("alias body is sent instead of the input",
	    t_sent_n == 1 && strcmp(t_sent[0], "kill kobold") == 0);
	check("input with no alias is left for the caller to send",
	    bd_triggers_input(t, "look") == 0);

	/* classes gate firing, and dot-nesting disables descendants */
	bd_trigger_add(t, BD_TRIG_ACTION, "an orc arrives", "kill orc",
	    "combat.melee", -1, 0);
	bd_class_disable(t, "combat");
	check("disabling a parent class disables its descendant",
	    bd_class_enabled(t, "combat.melee") == 0);
	reset_sent();
	check("a trigger in a disabled class does not fire",
	    bd_triggers_line(t, "an orc arrives") == 0 && t_sent_n == 0);
	bd_class_enable(t, "combat");
	reset_sent();
	bd_triggers_line(t, "an orc arrives");
	check("re-enabling the class fires the trigger again",
	    t_sent_n == 1 && strcmp(t_sent[0], "kill orc") == 0);

	/* #gag drops a line from display */
	bd_trigger_add(t, BD_TRIG_GAG, "spammy", "", NULL, -1, 0);
	bd_line_edit e;
	bd_triggers_rewrite(t, "a spammy line", &e);
	check("#gag marks the line dropped", e.gag == 1);
	bd_triggers_rewrite(t, "an ordinary line", &e);
	check("a non-gagged line is not dropped", e.gag == 0);

	/* #substitute rewrites matched text in place */
	bd_trigger_add(t, BD_TRIG_SUBST, "gold", "GOLD", NULL, -1, 0);
	bd_triggers_rewrite(t, "you find 10 gold", &e);
	check("#substitute replaces the match and flags the line changed",
	    e.gag == 0 && e.changed == 1 && strstr(e.text, "GOLD") != NULL &&
	    strstr(e.text, "gold") == NULL);

	/* #highlight wraps the match in ANSI color (an ESC appears) */
	bd_trigger_add(t, BD_TRIG_HILITE, "orc", "red", NULL, -1, 0);
	bd_triggers_rewrite(t, "an orc appears", &e);
	check("#highlight recolors the match with an SGR escape",
	    e.changed == 1 && strstr(e.text, "orc") != NULL &&
	    strchr(e.text, '\x1b') != NULL);

	/* priority: higher fires first, and #stop halts the rest for that line */
	bd_trigger_add(t, BD_TRIG_ACTION, "duel begins", "first", NULL, 9, 1);
	bd_trigger_add(t, BD_TRIG_ACTION, "duel begins", "second", NULL, 1, 0);
	reset_sent();
	bd_triggers_line(t, "duel begins");
	check("higher-priority trigger fires first and #stop blocks the rest",
	    t_sent_n == 1 && strcmp(t_sent[0], "first") == 0);

	/* prompt triggers match prompt text */
	bd_trigger_add(t, BD_TRIG_PROMPT, "HP", "at prompt", NULL, -1, 0);
	reset_sent();
	check("prompt trigger fires on prompt text",
	    bd_triggers_prompt(t, "HP: 100/100 >") == 1 &&
	    t_sent_n == 1 && strcmp(t_sent[0], "at prompt") == 0);

	/* gmcp triggers match a package name */
	bd_trigger_add(t, BD_TRIG_GMCP, "Char.Vitals", "got vitals", NULL, -1, 0);
	reset_sent();
	check("gmcp trigger fires on its package",
	    bd_triggers_gmcp(t, "Char.Vitals", "{\"hp\":100}") == 1 &&
	    t_sent_n == 1 && strcmp(t_sent[0], "got vitals") == 0);

	/* interval timers: the first run only schedules; a later run past the
	 * deadline fires exactly once */
	bd_trigger_add_tick(t, "heal", "cast heal", 5.0, NULL);   /* 5s = 5000ms */
	reset_sent();
	check("first run_timers schedules without firing",
	    bd_triggers_run_timers(t, 1000.0) == 0 && t_sent_n == 0);
	check("timer does not fire before its deadline",
	    bd_triggers_run_timers(t, 2000.0) == 0 && t_sent_n == 0);
	check("timer fires once past its deadline",
	    bd_triggers_run_timers(t, 7000.0) == 1 &&
	    t_sent_n == 1 && strcmp(t_sent[0], "cast heal") == 0);
	bd_trigger_remove_tick(t, "heal");

	/* multi-state chains: only the armed state fires, and firing advances it */
	bd_trigger_add_chained(t, BD_TRIG_ACTION, "step one", "did one",
	    "quest", "q", 1, -1, 0);
	bd_trigger_add_chained(t, BD_TRIG_ACTION, "step two", "did two",
	    "quest", "q", 2, -1, 0);
	reset_sent();
	bd_triggers_line(t, "step two");   /* state 2 not armed yet (chain at 1) */
	check("a chained trigger does not fire out of state", t_sent_n == 0);
	reset_sent();
	bd_triggers_line(t, "step one");   /* armed at 1: fires and advances to 2 */
	check("the armed chain step fires and advances",
	    t_sent_n == 1 && strcmp(t_sent[0], "did one") == 0);
	reset_sent();
	bd_triggers_line(t, "step two");   /* now armed at 2 */
	check("the next chain step fires once armed",
	    t_sent_n == 1 && strcmp(t_sent[0], "did two") == 0);
	bd_trigger_reset(t, "q");

	/* user events invoke the event callback */
	bd_triggers_set_event_cb(t, trig_event, NULL);
	t_ev_calls = 0;
	bd_triggers_event(t, "onDeath", "by an orc");
	check("raising an event invokes the event callback with name + arg",
	    t_ev_calls == 1 && strcmp(t_ev_name, "onDeath") == 0 &&
	    strcmp(t_ev_arg, "by an orc") == 0);

	/* removal */
	int before = bd_trigger_count(t);
	int removed = bd_trigger_remove_pattern(t, BD_TRIG_ALIAS, "kk", NULL);
	check("remove_pattern removes the alias",
	    removed == 1 && bd_trigger_count(t) == before - 1);

	bd_triggers_free(t);
}

/* ================================================================== */
/* bd_profile -- profile store + CSV import/export round-trip         */
/* ================================================================== */
static void
test_profile(void)
{
	printf("bd_profile:\n");
	bd_profiles *ps = bd_profiles_new();
	check("store created", ps != NULL);

	bd_profile *p = bd_profiles_add(ps, "Aardwolf");
	check("profile added and found",
	    p != NULL && bd_profiles_find(ps, "Aardwolf") == p);
	bd_profile_set(p, "host", "aardmud.org");
	bd_profile_set(p, "port", "23");
	check("get returns a set value",
	    strcmp(bd_profile_get(p, "host"), "aardmud.org") == 0);

	bd_profiles_add(ps, "BatMUD");
	check("store counts both profiles", bd_profiles_count(ps) == 2);

	/* export, then import into a fresh store: the data must survive intact */
	size_t len = 0;
	char *csv = bd_profiles_export_csv(ps, NULL, &len);
	check("export produces CSV", csv != NULL && len > 0);

	bd_profiles *ps2 = bd_profiles_new();
	int n = bd_profiles_import_csv(ps2, csv, len, NULL);
	check("import reports the profiles read", n == 2);
	bd_profile *p2 = bd_profiles_find(ps2, "Aardwolf");
	check("a profile round-trips through export/import with its fields",
	    p2 != NULL && strcmp(bd_profile_get(p2, "host"), "aardmud.org") == 0 &&
	    strcmp(bd_profile_get(p2, "port"), "23") == 0);

	/* import merges onto an existing profile by name */
	static const char *upd = "name,host\nAardwolf,new.host\n";
	bd_profiles_import_csv(ps2, upd, strlen(upd), NULL);
	check("import merges by name (no duplicate row)",
	    bd_profiles_count(ps2) == 2 &&
	    strcmp(bd_profile_get(p2, "host"), "new.host") == 0);

	check("remove drops a profile",
	    bd_profiles_remove(ps2, "BatMUD") == 1 &&
	    bd_profiles_find(ps2, "BatMUD") == NULL);

	free(csv);
	bd_profiles_free(ps);
	bd_profiles_free(ps2);
}

/* ================================================================== */
/* bd_profile -- import-collision merge (skip / rename / overwrite)    */
/* ================================================================== */
/* These mirror main.c's import-collision helpers (pure, public-API only).
 * The app has no client-UI test harness, so the merge algorithm the import
 * dialog runs is validated here against the real profile store. */
enum { M_OVERWRITE, M_SKIP, M_RENAME };

static void
mtest_copy_keys(bd_profile *dst, const bd_profile *src, const char *force_name)
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

static void
mtest_unique(bd_profiles *ps, const char *base, char *out, size_t sz)
{
	int i;
	for (i = 2; i < 100000; i++) {
		snprintf(out, sz, "%s (%d)", base, i);
		if (!bd_profiles_find(ps, out))
			return;
	}
}

static int
mtest_count_coll(bd_profiles *dst, bd_profiles *src)
{
	int i, c = 0, n = bd_profiles_count(src);
	for (i = 0; i < n; i++) {
		const char *name = bd_profile_get(bd_profiles_at(src, i), "name");
		if (name && *name && bd_profiles_find(dst, name))
			c++;
	}
	return c;
}

static int
mtest_merge(bd_profiles *dst, bd_profiles *src, int policy)
{
	int i, changed = 0, n = bd_profiles_count(src);
	for (i = 0; i < n; i++) {
		bd_profile *sp = bd_profiles_at(src, i);
		const char *name = bd_profile_get(sp, "name");
		bd_profile *ex, *np;
		if (!name || !*name)
			continue;
		ex = bd_profiles_find(dst, name);
		if (!ex) {
			np = bd_profiles_add(dst, name);
			if (np) { mtest_copy_keys(np, sp, NULL); changed++; }
		} else if (policy == M_SKIP) {
			continue;
		} else if (policy == M_RENAME) {
			char nm[160];
			mtest_unique(dst, name, nm, sizeof nm);
			np = bd_profiles_add(dst, nm);
			if (np) { mtest_copy_keys(np, sp, nm); changed++; }
		} else {
			mtest_copy_keys(ex, sp, NULL);
			changed++;
		}
	}
	return changed;
}

/* Build a store with one "Aardwolf" profile (host=old.host). */
static bd_profiles *
mtest_base(void)
{
	bd_profiles *d = bd_profiles_new();
	bd_profile_set(bd_profiles_add(d, "Aardwolf"), "host", "old.host");
	return d;
}

static void
test_profile_merge(void)
{
	printf("bd_profile (import-collision merge):\n");
	static const char *incoming =
	    "name,host\nAardwolf,new.host\nNewMud,new.example\n";

	bd_profiles *base = mtest_base();
	bd_profiles *scratch = bd_profiles_new();
	bd_profiles_import_csv(scratch, incoming, strlen(incoming), NULL);
	check("scratch parsed the two incoming profiles",
	    bd_profiles_count(scratch) == 2);
	check("exactly one incoming name collides with the base",
	    mtest_count_coll(base, scratch) == 1);
	bd_profiles_free(base);

	/* OVERWRITE: the clash is updated, the fresh name is added */
	bd_profiles *d1 = mtest_base();
	int c1 = mtest_merge(d1, scratch, M_OVERWRITE);
	check("overwrite updates the clash and adds the new profile",
	    c1 == 2 && bd_profiles_count(d1) == 2 &&
	    strcmp(bd_profile_get(bd_profiles_find(d1, "Aardwolf"), "host"),
	           "new.host") == 0 &&
	    bd_profiles_find(d1, "NewMud") != NULL);
	bd_profiles_free(d1);

	/* SKIP: the clash is left as-is, the fresh name still lands */
	bd_profiles *d2 = mtest_base();
	int c2 = mtest_merge(d2, scratch, M_SKIP);
	check("skip keeps the existing clash but adds the new profile",
	    c2 == 1 && bd_profiles_count(d2) == 2 &&
	    strcmp(bd_profile_get(bd_profiles_find(d2, "Aardwolf"), "host"),
	           "old.host") == 0 &&
	    bd_profiles_find(d2, "NewMud") != NULL);
	bd_profiles_free(d2);

	/* RENAME: the incoming clash lands under a fresh name, original kept */
	bd_profiles *d3 = mtest_base();
	int c3 = mtest_merge(d3, scratch, M_RENAME);
	check("rename keeps the original and adds a renamed copy",
	    c3 == 2 && bd_profiles_count(d3) == 3 &&
	    strcmp(bd_profile_get(bd_profiles_find(d3, "Aardwolf"), "host"),
	           "old.host") == 0 &&
	    bd_profiles_find(d3, "Aardwolf (2)") != NULL &&
	    strcmp(bd_profile_get(bd_profiles_find(d3, "Aardwolf (2)"), "host"),
	           "new.host") == 0);
	bd_profiles_free(d3);

	bd_profiles_free(scratch);
}

/* ================================================================== */
/* bd_verb -- the TinTin++-style #verb command parser                 */
/* ================================================================== */
static void
test_verb(void)
{
	printf("bd_verb:\n");
	bd_triggers *t = bd_triggers_new(NULL, trig_send, NULL);
	char fb[128];
	const char *lit;

	/* #action compiles to a trigger that then fires on a matching line */
	int handled = bd_verb_exec(t, "#action {You are hungry} {eat bread}",
	    &lit, fb, sizeof fb);
	check("#action is handled and reports success",
	    handled == 1 && strcmp(fb, "action added") == 0 &&
	    bd_trigger_count(t) == 1);
	reset_sent();
	bd_triggers_line(t, "You are hungry now");
	check("the verb-created action fires",
	    t_sent_n == 1 && strcmp(t_sent[0], "eat bread") == 0);

	/* #alias */
	bd_verb_exec(t, "#alias {gg} {get all}", &lit, fb, sizeof fb);
	check("#alias is handled", strcmp(fb, "alias added") == 0);
	reset_sent();
	check("the verb-created alias fires on input",
	    bd_triggers_input(t, "gg") == 1 &&
	    t_sent_n == 1 && strcmp(t_sent[0], "get all") == 0);

	/* #class <name> on|off toggles a class */
	bd_verb_exec(t, "#class combat off", &lit, fb, sizeof fb);
	check("#class off disables the class",
	    strcmp(fb, "class disabled") == 0 && bd_class_enabled(t, "combat") == 0);
	bd_verb_exec(t, "#class combat on", &lit, fb, sizeof fb);
	check("#class on re-enables the class",
	    strcmp(fb, "class enabled") == 0 && bd_class_enabled(t, "combat") == 1);

	/* #unaction removes by pattern */
	int before = bd_trigger_count(t);
	bd_verb_exec(t, "#unaction {You are hungry}", &lit, fb, sizeof fb);
	check("#unaction removes the trigger",
	    strcmp(fb, "removed 1") == 0 && bd_trigger_count(t) == before - 1);

	/* a non-verb line is not handled; "##x" escapes a literal leading '#' */
	check("ordinary input is not a verb",
	    bd_verb_exec(t, "look north", &lit, fb, sizeof fb) == 0);
	lit = NULL;
	check("\"##text\" is an escaped literal, not a verb",
	    bd_verb_exec(t, "##friend", &lit, fb, sizeof fb) == 0 &&
	    lit != NULL && strcmp(lit, "#friend") == 0);

	bd_triggers_free(t);
}

/* ================================================================== */
/* bd_mxp -- MXP tag parser (socket-free, incremental)                */
/* ================================================================== */
static char m_text[256];
static size_t m_text_n;
static char m_tag[8][96];
static int  m_tag_n;
static void mxp_text(const char *b, size_t n, void *a)
{ (void)a; if (m_text_n + n < sizeof m_text) { memcpy(m_text + m_text_n, b, n); m_text_n += n; m_text[m_text_n] = '\0'; } }
static void mxp_tag(const char *name, const char *attrs, int closing, void *a)
{ (void)a; if (m_tag_n < 8) snprintf(m_tag[m_tag_n++], 96, "%s%s|%s",
      closing ? "/" : "", name, attrs ? attrs : ""); }
static void mxp_reset_rec(void) { m_text_n = 0; m_text[0] = '\0'; m_tag_n = 0; }

static void
test_mxp(void)
{
	printf("bd_mxp:\n");
	bd_mxp_cb cb = { .text = mxp_text, .tag = mxp_tag };
	bd_mxp *m = bd_mxp_new(&cb);
	check("mxp parser created", m != NULL);

	/* an open/close tag pair around text: markup removed, text kept */
	mxp_reset_rec();
	{ const char *s = "<b>bold</b> plain";
	  bd_mxp_feed(m, (const unsigned char *)s, strlen(s)); }
	check("tags are stripped and their text kept",
	    strcmp(m_text, "bold plain") == 0);
	check("open and close tags are reported in order",
	    m_tag_n == 2 && strcmp(m_tag[0], "b|") == 0 &&
	    strcmp(m_tag[1], "/b|") == 0);

	/* a tag with attributes surfaces the attribute string */
	mxp_reset_rec();
	{ const char *s = "<send href=\"north\">go</send>";
	  bd_mxp_feed(m, (const unsigned char *)s, strlen(s)); }
	check("tag attributes are passed through",
	    m_tag_n == 2 && strncmp(m_tag[0], "send|", 5) == 0 &&
	    strstr(m_tag[0], "href=\"north\"") != NULL);

	/* entity decoding */
	mxp_reset_rec();
	{ const char *s = "a &lt; b &amp; c &gt; d &#65;";
	  bd_mxp_feed(m, (const unsigned char *)s, strlen(s)); }
	check("named and numeric entities decode",
	    strcmp(m_text, "a < b & c > d A") == 0);

	/* an unknown entity is emitted verbatim */
	mxp_reset_rec();
	{ const char *s = "x &bogus; y";
	  bd_mxp_feed(m, (const unsigned char *)s, strlen(s)); }
	check("an unknown entity is left verbatim",
	    strcmp(m_text, "x &bogus; y") == 0);

	/* a tag split across two feeds is buffered until complete */
	mxp_reset_rec();
	bd_mxp_feed(m, (const unsigned char *)"<bo", 3);
	check("a partial tag emits nothing yet", m_tag_n == 0 && m_text_n == 0);
	bd_mxp_feed(m, (const unsigned char *)"ld>hi", 5);
	check("the tag completes across the feed boundary",
	    m_tag_n == 1 && strcmp(m_tag[0], "bold|") == 0 &&
	    strcmp(m_text, "hi") == 0);

	bd_mxp_free(m);
}

/* ================================================================== */
/* bd_encoding -- legacy 8-bit to UTF-8 transcode                     */
/* ================================================================== */
static void
test_encoding(void)
{
	unsigned char out[64];
	size_t n;

	printf("bd_encoding:\n");

	check("name aliases parse to Latin-1",
	    bd_encoding_parse("latin1") == BD_ENC_LATIN1 &&
	    bd_encoding_parse("ISO-8859-1") == BD_ENC_LATIN1);
	check("name aliases parse to CP1252",
	    bd_encoding_parse("cp1252") == BD_ENC_CP1252 &&
	    bd_encoding_parse("Windows-1252") == BD_ENC_CP1252);
	check("NULL / unknown fall back to UTF-8",
	    bd_encoding_parse(NULL) == BD_ENC_UTF8 &&
	    bd_encoding_parse("koi8-r") == BD_ENC_UTF8);
	check("canonical names round-trip",
	    strcmp(bd_encoding_name(BD_ENC_LATIN1), "ISO-8859-1") == 0 &&
	    strcmp(bd_encoding_name(BD_ENC_CP1252), "Windows-1252") == 0);

	/* UTF-8 is a verbatim passthrough */
	n = bd_encoding_decode(BD_ENC_UTF8, (const unsigned char *)"caf\xc3\xa9",
	    5, out, sizeof out);
	check("UTF-8 passes bytes through unchanged",
	    n == 5 && memcmp(out, "caf\xc3\xa9", 5) == 0);

	/* Latin-1: ASCII stays 1 byte, high bytes become 2-byte UTF-8.
	 * 0xE9 is 'é' (U+00E9) -> 0xC3 0xA9. */
	n = bd_encoding_decode(BD_ENC_LATIN1, (const unsigned char *)"caf\xe9",
	    4, out, sizeof out);
	out[n] = '\0';
	check("Latin-1 high byte expands to 2-byte UTF-8",
	    n == 5 && strcmp((char *)out, "caf\xc3\xa9") == 0);

	/* Latin-1 leaves ASCII / control bytes (escape sequences) untouched */
	n = bd_encoding_decode(BD_ENC_LATIN1,
	    (const unsigned char *)"\033[31mhi\033[0m", 10, out, sizeof out);
	check("Latin-1 preserves ASCII escape sequences byte-for-byte",
	    n == 10 && memcmp(out, "\033[31mhi\033[0m", 10) == 0);

	/* CP1252: 0x80 is the euro sign U+20AC -> 3-byte UTF-8 E2 82 AC;
	 * 0x93/0x94 are curly quotes; 0xE9 matches Latin-1. */
	n = bd_encoding_decode(BD_ENC_CP1252, (const unsigned char *)"\x80\x93x\x94",
	    4, out, sizeof out);
	check("CP1252 euro sign becomes 3-byte UTF-8",
	    n >= 3 && (unsigned char)out[0] == 0xE2 &&
	    (unsigned char)out[1] == 0x82 && (unsigned char)out[2] == 0xAC);
	check("CP1252 0xA0-0xFF match Latin-1 (é)",
	    bd_encoding_decode(BD_ENC_CP1252, (const unsigned char *)"\xe9", 1,
	        out, sizeof out) == 2 &&
	    (unsigned char)out[0] == 0xC3 && (unsigned char)out[1] == 0xA9);

	/* output is bounded by dstcap, never overruns */
	n = bd_encoding_decode(BD_ENC_LATIN1, (const unsigned char *)"\xe9\xe9\xe9",
	    3, out, 3);
	check("decode truncates to the destination capacity",
	    n <= 3);
}

int
main(void)
{
	test_ring();
	test_csv();
	test_telopt();
	test_trigger();
	test_verb();
	test_mxp();
	test_profile();
	test_profile_merge();
	test_encoding();
	printf("\n%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
