/*
 * bd_verb -- command-line verb parser. See bd_verb.h and doc/triggers.md.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_verb.h"
#include "bd_vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARG_MAX 1024

static const char *
skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

/* Read a {...} group at *pp (after optional leading space) into out[cap],
 * honoring nested braces. Advances *pp past the closing '}'. Returns 1 on
 * success, 0 if there is no brace group there. */
static int
read_brace(const char **pp, char *out, size_t cap)
{
	const char *p = skip_ws(*pp);
	int depth = 0;
	size_t o = 0;

	if (*p != '{')
		return 0;
	p++;            /* opening brace */
	depth = 1;
	while (*p && depth > 0) {
		if (*p == '{')
			depth++;
		else if (*p == '}') {
			depth--;
			if (depth == 0)
				break;
		}
		if (o + 1 < cap)
			out[o++] = *p;
		p++;
	}
	if (depth != 0)
		return 0;       /* unterminated */
	out[o] = '\0';
	*pp = p + 1;            /* past closing '}' */
	return 1;
}

/* Read a bare word (the optional trailing class) into out. */
static void
read_word(const char **pp, char *out, size_t cap)
{
	const char *p = skip_ws(*pp);
	size_t o = 0;
	while (*p && *p != ' ' && *p != '\t') {
		if (o + 1 < cap)
			out[o++] = *p;
		p++;
	}
	out[o] = '\0';
	*pp = p;
}

static void
fb(char *buf, size_t cap, const char *msg)
{
	if (buf && cap)
		snprintf(buf, cap, "%s", msg);
}

/* Strip a trailing/embedded "#stop" token from a command body, returning 1 if
 * one was present (so the trigger gets stop=1) and trimming trailing space. */
static int
extract_stop(char *body)
{
	char *s = strstr(body, "#stop");
	size_t len;
	if (!s)
		return 0;
	*s = '\0';                      /* drop "#stop" and anything after */
	len = strlen(body);
	while (len > 0 && (body[len - 1] == ' ' || body[len - 1] == '\t' ||
	    body[len - 1] == ';'))
		body[--len] = '\0';
	return 1;
}

/*
 * Parse a trailing class token of the form "class", "class/chain:state",
 * "class/chain", or "chain:state". Splits off ":state" first, then "/chain";
 * a token with a chain part but no explicit class uses the default class. On
 * return cls[] holds the class (empty -> default), chain[] holds the chain
 * name (empty -> no chain), and *state holds the state (0 -> none).
 */
static void
parse_class_token(const char *tok, char *cls, size_t ccap, char *chain,
                  size_t chcap, int *state)
{
	char buf[256];
	char *colon, *slash;

	cls[0] = '\0';
	chain[0] = '\0';
	*state = 0;
	if (!tok || !tok[0])
		return;
	snprintf(buf, sizeof buf, "%s", tok);

	colon = strrchr(buf, ':');
	if (colon) {
		*colon = '\0';
		*state = atoi(colon + 1);
	}
	slash = strchr(buf, '/');
	if (slash) {
		*slash = '\0';
		snprintf(cls, ccap, "%s", buf);        /* may be empty -> default */
		snprintf(chain, chcap, "%s", slash + 1);
	} else if (*state > 0) {
		/* "name:state" -> name is the chain, default class */
		snprintf(chain, chcap, "%s", buf);
	} else {
		snprintf(cls, ccap, "%s", buf);
	}
}

/* #action / #alias {pattern} {body} [class[/chain:state]] */
static int
verb_trigger(bd_triggers *t, bd_trigger_type type, const char *args,
             char *feedback, size_t fbcap)
{
	char pat[ARG_MAX], body[ARG_MAX], tok[128];
	char cls[128], chain[128];
	int stop, state, rc;

	if (!read_brace(&args, pat, sizeof pat) ||
	    !read_brace(&args, body, sizeof body)) {
		fb(feedback, fbcap, "usage: {pattern} {body} [class[/chain:state]]");
		return 1;
	}
	read_word(&args, tok, sizeof tok);
	parse_class_token(tok, cls, sizeof cls, chain, sizeof chain, &state);
	stop = extract_stop(body);
	if (chain[0] && state > 0)
		rc = bd_trigger_add_chained(t, type, pat, body,
		    cls[0] ? cls : NULL, chain, state, -1, stop);
	else
		rc = bd_trigger_add(t, type, pat, body, cls[0] ? cls : NULL,
		    -1, stop);
	if (rc < 0)
		fb(feedback, fbcap, "could not add trigger");
	else
		fb(feedback, fbcap,
		    type == BD_TRIG_ALIAS ? "alias added" : "action added");
	return 1;
}

/* #unaction / #unalias {pattern} [class] -- remove matching triggers */
static int
verb_untrigger(bd_triggers *t, bd_trigger_type type, const char *args,
               char *feedback, size_t fbcap)
{
	char pat[ARG_MAX], cls[128];
	int n;

	if (!read_brace(&args, pat, sizeof pat)) {
		fb(feedback, fbcap, "usage: {pattern} [class]");
		return 1;
	}
	read_word(&args, cls, sizeof cls);
	n = bd_trigger_remove_pattern(t, type, pat, cls[0] ? cls : NULL);
	if (n == 0)
		fb(feedback, fbcap, "no match");
	else if (n == 1)
		fb(feedback, fbcap, "removed 1");
	else {
		char msg[48];
		snprintf(msg, sizeof msg, "removed %d", n);
		fb(feedback, fbcap, msg);
	}
	return 1;
}

/* #list collects a compact one-line-per-trigger listing into a buffer. */
struct list_ctx {
	char *buf;
	size_t cap, len;
	const char *only;       /* class filter, or NULL */
	int count;
};

static const char *
type_tag(bd_trigger_type type)
{
	switch (type) {
	case BD_TRIG_ACTION: return "act";
	case BD_TRIG_ALIAS:  return "ali";
	case BD_TRIG_PROMPT: return "prm";
	case BD_TRIG_GMCP:   return "gmcp";
	case BD_TRIG_GAG:    return "gag";
	case BD_TRIG_SUBST:  return "sub";
	case BD_TRIG_HILITE: return "hi";
	}
	return "?";
}

static void
list_cb(bd_trigger_type type, const char *pattern, const char *body,
        const char *cls, const char *chain, int state, int priority,
        int enabled, void *ctx)
{
	struct list_ctx *l = ctx;
	char line[ARG_MAX];
	int k;

	(void)body;
	(void)chain;
	if (l->only && strcmp(cls, l->only) != 0)
		return;
	l->count++;
	if (l->len + 1 >= l->cap)
		return;         /* full; count still climbs so we can note it */
	if (state > 0)
		snprintf(line, sizeof line, "\r\n  [%s] %s {%s} p%d s%d",
		    enabled ? type_tag(type) : "off", cls, pattern, priority,
		    state);
	else
		snprintf(line, sizeof line, "\r\n  [%s] %s {%s} p%d",
		    enabled ? type_tag(type) : "off", cls, pattern, priority);
	k = snprintf(l->buf + l->len, l->cap - l->len, "%s", line);
	if (k > 0)
		l->len += (size_t)k;
	if (l->len >= l->cap)
		l->len = l->cap - 1;
}

/* #list [class] -- enumerate triggers (optionally one class) into feedback */
static int
verb_list(bd_triggers *t, const char *args, char *feedback, size_t fbcap)
{
	char cls[128];
	struct list_ctx l;

	read_word(&args, cls, sizeof cls);
	if (!feedback || fbcap == 0)
		return 1;
	memset(&l, 0, sizeof l);
	l.buf = feedback;
	l.cap = fbcap;
	l.only = cls[0] ? cls : NULL;
	l.len = (size_t)snprintf(feedback, fbcap, "triggers:");
	bd_trigger_foreach(t, list_cb, &l);
	if (l.count == 0)
		fb(feedback, fbcap, "no triggers");
	return 1;
}

/* #gag {pattern} [class] -- drop matching lines from display */
static int
verb_gag(bd_triggers *t, const char *args, char *feedback, size_t fbcap)
{
	char pat[ARG_MAX], cls[128];
	if (!read_brace(&args, pat, sizeof pat)) {
		fb(feedback, fbcap, "usage: #gag {pattern} [class]");
		return 1;
	}
	read_word(&args, cls, sizeof cls);
	if (bd_trigger_add(t, BD_TRIG_GAG, pat, "", cls[0] ? cls : NULL, -1, 0) < 0)
		fb(feedback, fbcap, "could not add gag");
	else
		fb(feedback, fbcap, "gag added");
	return 1;
}

/* #substitute {pattern} {replacement} [class] -- rewrite matched text */
static int
verb_subst(bd_triggers *t, const char *args, char *feedback, size_t fbcap)
{
	char pat[ARG_MAX], rep[ARG_MAX], cls[128];
	if (!read_brace(&args, pat, sizeof pat) ||
	    !read_brace(&args, rep, sizeof rep)) {
		fb(feedback, fbcap, "usage: #substitute {pattern} {replacement} [class]");
		return 1;
	}
	read_word(&args, cls, sizeof cls);
	if (bd_trigger_add(t, BD_TRIG_SUBST, pat, rep, cls[0] ? cls : NULL, -1, 0) < 0)
		fb(feedback, fbcap, "could not add substitution");
	else
		fb(feedback, fbcap, "substitution added");
	return 1;
}

/* #highlight {pattern} {color} [class] -- recolor matched text */
static int
verb_hilite(bd_triggers *t, const char *args, char *feedback, size_t fbcap)
{
	char pat[ARG_MAX], color[64], cls[128];
	if (!read_brace(&args, pat, sizeof pat) ||
	    !read_brace(&args, color, sizeof color)) {
		fb(feedback, fbcap, "usage: #highlight {pattern} {color} [class]");
		return 1;
	}
	read_word(&args, cls, sizeof cls);
	if (bd_trigger_add(t, BD_TRIG_HILITE, pat, color, cls[0] ? cls : NULL, -1, 0) < 0)
		fb(feedback, fbcap, "could not add highlight");
	else
		fb(feedback, fbcap, "highlight added");
	return 1;
}

/* #reset [chain] -- reset all chains, or one named "class/chain" or "chain" */
static int
verb_reset(bd_triggers *t, const char *args, char *feedback, size_t fbcap)
{
	char name[128];
	read_word(&args, name, sizeof name);
	bd_trigger_reset(t, name[0] ? name : NULL);
	fb(feedback, fbcap, "chain reset");
	return 1;
}

/* #tick {name} {body} <seconds> [class] */
static int
verb_tick(bd_triggers *t, const char *args, char *feedback, size_t fbcap)
{
	char name[128], body[ARG_MAX], secs[32], cls[128];
	double s;

	if (!read_brace(&args, name, sizeof name) ||
	    !read_brace(&args, body, sizeof body)) {
		fb(feedback, fbcap, "usage: #tick {name} {body} <seconds> [class]");
		return 1;
	}
	read_word(&args, secs, sizeof secs);
	read_word(&args, cls, sizeof cls);
	s = atof(secs);
	if (s <= 0) {
		fb(feedback, fbcap, "usage: #tick {name} {body} <seconds> [class]");
		return 1;
	}
	if (bd_trigger_add_tick(t, name, body, s, cls[0] ? cls : NULL) < 0)
		fb(feedback, fbcap, "could not add timer");
	else
		fb(feedback, fbcap, "timer added");
	return 1;
}

/* #untick {name} */
static int
verb_untick(bd_triggers *t, const char *args, char *feedback, size_t fbcap)
{
	char name[128];
	read_word(&args, name, sizeof name);
	if (!name[0]) {
		fb(feedback, fbcap, "usage: #untick <name>");
		return 1;
	}
	bd_trigger_remove_tick(t, name);
	fb(feedback, fbcap, "timer removed");
	return 1;
}

/* #class <name> on|off  (also accepts "enable"/"disable") */
static int
verb_class(bd_triggers *t, const char *args, char *feedback, size_t fbcap)
{
	char name[128], state[16];

	read_word(&args, name, sizeof name);
	read_word(&args, state, sizeof state);
	if (!name[0] || !state[0]) {
		fb(feedback, fbcap, "usage: #class <name> on|off");
		return 1;
	}
	if (!strcmp(state, "on") || !strcmp(state, "enable")) {
		bd_class_enable(t, name);
		fb(feedback, fbcap, "class enabled");
	} else if (!strcmp(state, "off") || !strcmp(state, "disable")) {
		bd_class_disable(t, name);
		fb(feedback, fbcap, "class disabled");
	} else {
		fb(feedback, fbcap, "usage: #class <name> on|off");
	}
	return 1;
}

int
bd_verb_exec(bd_triggers *t, const char *input, const char **literal,
             char *feedback, size_t fbcap)
{
	const char *p, *args;
	char verb[32];
	size_t v = 0;

	if (literal)
		*literal = NULL;
	if (!input || input[0] != '#')
		return 0;               /* not a verb */
	if (input[1] == '#') {          /* "##" -> literal leading '#' */
		if (literal)
			*literal = input + 1;
		return 0;
	}

	p = input + 1;
	while (p[0] && p[0] != ' ' && p[0] != '\t' && p[0] != '{' && v + 1 < sizeof verb)
		verb[v++] = *p++;
	verb[v] = '\0';
	args = p;

	if (!strcmp(verb, "action") || !strcmp(verb, "act"))
		return verb_trigger(t, BD_TRIG_ACTION, args, feedback, fbcap);
	if (!strcmp(verb, "alias"))
		return verb_trigger(t, BD_TRIG_ALIAS, args, feedback, fbcap);
	if (!strcmp(verb, "class"))
		return verb_class(t, args, feedback, fbcap);
	if (!strcmp(verb, "tick"))
		return verb_tick(t, args, feedback, fbcap);
	if (!strcmp(verb, "untick"))
		return verb_untick(t, args, feedback, fbcap);
	if (!strcmp(verb, "reset"))
		return verb_reset(t, args, feedback, fbcap);
	if (!strcmp(verb, "unaction") || !strcmp(verb, "unact"))
		return verb_untrigger(t, BD_TRIG_ACTION, args, feedback, fbcap);
	if (!strcmp(verb, "unalias"))
		return verb_untrigger(t, BD_TRIG_ALIAS, args, feedback, fbcap);
	if (!strcmp(verb, "list"))
		return verb_list(t, args, feedback, fbcap);
	if (!strcmp(verb, "gag"))
		return verb_gag(t, args, feedback, fbcap);
	if (!strcmp(verb, "substitute") || !strcmp(verb, "sub"))
		return verb_subst(t, args, feedback, fbcap);
	if (!strcmp(verb, "highlight") || !strcmp(verb, "hi"))
		return verb_hilite(t, args, feedback, fbcap);
	if (!strcmp(verb, "script")) {
		char src[ARG_MAX];
		bd_vm *vm = bd_triggers_vm(t);
		if (!read_brace(&args, src, sizeof src)) {
			fb(feedback, fbcap, "usage: #script {lua}");
		} else if (!vm || bd_vm_eval(vm, src) != 0) {
			fb(feedback, fbcap, vm ? bd_vm_error(vm) : "no scripting");
		} else {
			fb(feedback, fbcap, "ok");
		}
		return 1;
	}

	fb(feedback, fbcap, "unknown command");
	return 1;        /* a leading '#' was a verb attempt, so consume it */
}
