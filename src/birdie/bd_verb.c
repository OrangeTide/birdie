/*
 * bd_verb -- command-line verb parser. See bd_verb.h and doc/triggers.md.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_verb.h"

#include <stdio.h>
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

/* #action / #alias {pattern} {body} [class] */
static int
verb_trigger(bd_triggers *t, bd_trigger_type type, const char *args,
             char *feedback, size_t fbcap)
{
	char pat[ARG_MAX], body[ARG_MAX], cls[128];
	int stop;

	if (!read_brace(&args, pat, sizeof pat) ||
	    !read_brace(&args, body, sizeof body)) {
		fb(feedback, fbcap, "usage: {pattern} {body} [class]");
		return 1;
	}
	read_word(&args, cls, sizeof cls);
	stop = extract_stop(body);
	if (bd_trigger_add(t, type, pat, body, cls[0] ? cls : NULL, -1, stop) < 0)
		fb(feedback, fbcap, "could not add trigger");
	else
		fb(feedback, fbcap,
		    type == BD_TRIG_ALIAS ? "alias added" : "action added");
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

	fb(feedback, fbcap, "unknown command");
	return 1;        /* a leading '#' was a verb attempt, so consume it */
}
