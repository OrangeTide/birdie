/*
 * bd_trigger -- the trigger / alias engine. See bd_trigger.h and
 * doc/triggers.md.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_trigger.h"

#include <stdlib.h>
#include <string.h>

#define BODY_MAX 4096

struct trigger {
	int id;
	bd_trigger_type type;
	char *pattern;
	char *body;
	char *cls;
	int priority;
	int stop;
	int seq;        /* insertion order, for stable priority sort */
	int alive;
};

struct bd_triggers {
	bd_vm *vm;
	bd_trigger_send_fn send;
	void *ctx;

	struct trigger *trig;
	int n, cap;
	int next_id;
	int next_seq;
	int sorted;             /* trig[] is in dispatch order */

	char **disabled;        /* explicitly disabled class names */
	int ndis, dis_cap;
};

/* ---- pattern matching (TinTin++-style %1..%9 captures) ---- */

struct span { int off, len; };

struct mctx {
	const char *line;
	int want_end;           /* pattern had a trailing '$' */
	struct span cap[10];    /* cap[1..9] used by %N; cap[0] set by caller */
	const char *end;        /* where the match finished (on success) */
};

/* Match pattern p against s. Captures are greedy (longest first, then
 * backtrack), matching TinTin++ %-pattern semantics. Returns 1 on success. */
static int
pat_match(struct mctx *c, const char *p, const char *s)
{
	while (*p) {
		if (p[0] == '%' && p[1] >= '1' && p[1] <= '9') {
			int idx = p[1] - '0';
			int max = (int)strlen(s);
			int k;
			for (k = max; k >= 0; k--) {     /* greedy: longest first */
				c->cap[idx].off = (int)(s - c->line);
				c->cap[idx].len = k;
				if (pat_match(c, p + 2, s + k))
					return 1;
			}
			return 0;
		}
		if (*s != *p)
			return 0;
		s++;
		p++;
	}
	if (c->want_end && *s)
		return 0;
	c->end = s;
	return 1;
}

/*
 * Match `pattern` anywhere in `line` (or anchored by ^ / $). On success fills
 * cap[] (cap[0] = whole match) and returns 1.
 */
static int
match(const char *pattern, const char *line, struct span cap[10])
{
	struct mctx c;
	char pat[1024];
	int anchored_start = 0;
	size_t pl;
	const char *start;

	memset(&c, 0, sizeof c);
	c.line = line;

	if (*pattern == '^') {
		anchored_start = 1;
		pattern++;
	}
	pl = strlen(pattern);
	if (pl >= sizeof pat)
		pl = sizeof pat - 1;
	memcpy(pat, pattern, pl);
	pat[pl] = '\0';
	if (pl > 0 && pat[pl - 1] == '$') {
		c.want_end = 1;
		pat[pl - 1] = '\0';
	}

	for (start = line; ; start++) {
		int i;
		for (i = 1; i < 10; i++) {
			c.cap[i].off = 0;
			c.cap[i].len = 0;
		}
		if (pat_match(&c, pat, start)) {
			cap[0].off = (int)(start - line);
			cap[0].len = (int)(c.end - start);
			for (i = 1; i < 10; i++)
				cap[i] = c.cap[i];
			return 1;
		}
		if (anchored_start || !*start)
			return 0;
	}
}

/* Expand %0..%9 in `body` using captures from `line` into out[cap_sz]. */
static void
expand(const char *body, const char *line, const struct span cap[10],
       char *out, size_t out_sz)
{
	size_t o = 0;
	const char *p = body;

	while (*p && o + 1 < out_sz) {
		if (p[0] == '%' && p[1] >= '0' && p[1] <= '9') {
			int idx = p[1] - '0';
			int k;
			for (k = 0; k < cap[idx].len && o + 1 < out_sz; k++)
				out[o++] = line[cap[idx].off + k];
			p += 2;
		} else {
			out[o++] = *p++;
		}
	}
	out[o] = '\0';
}

/* ---- class enable/disable ---- */

/* Is `name` (or any dotted ancestor) in the disabled set? */
int
bd_class_enabled(bd_triggers *t, const char *name)
{
	int i;
	size_t nl;

	if (!t || !name)
		return 1;
	nl = strlen(name);
	for (i = 0; i < t->ndis; i++) {
		size_t dl = strlen(t->disabled[i]);
		if (dl > nl)
			continue;
		if (strncmp(name, t->disabled[i], dl) != 0)
			continue;
		/* exact match, or `name` is a child: disabled + '.' boundary */
		if (name[dl] == '\0' || name[dl] == '.')
			return 0;
	}
	return 1;
}

void
bd_class_disable(bd_triggers *t, const char *name)
{
	int i;
	if (!t || !name)
		return;
	for (i = 0; i < t->ndis; i++)
		if (strcmp(t->disabled[i], name) == 0)
			return;         /* already disabled */
	if (t->ndis == t->dis_cap) {
		int nc = t->dis_cap ? t->dis_cap * 2 : 8;
		char **d = realloc(t->disabled, (size_t)nc * sizeof *d);
		if (!d)
			return;
		t->disabled = d;
		t->dis_cap = nc;
	}
	t->disabled[t->ndis++] = strdup(name);
}

void
bd_class_enable(bd_triggers *t, const char *name)
{
	int i;
	if (!t || !name)
		return;
	for (i = 0; i < t->ndis; i++) {
		if (strcmp(t->disabled[i], name) != 0)
			continue;
		free(t->disabled[i]);
		t->disabled[i] = t->disabled[--t->ndis];
		return;
	}
}

/* ---- engine ---- */

bd_triggers *
bd_triggers_new(bd_vm *vm, bd_trigger_send_fn send, void *ctx)
{
	bd_triggers *t = calloc(1, sizeof *t);
	if (!t)
		return NULL;
	t->vm = vm;
	t->send = send;
	t->ctx = ctx;
	return t;
}

void
bd_triggers_free(bd_triggers *t)
{
	int i;
	if (!t)
		return;
	for (i = 0; i < t->n; i++) {
		free(t->trig[i].pattern);
		free(t->trig[i].body);
		free(t->trig[i].cls);
	}
	free(t->trig);
	for (i = 0; i < t->ndis; i++)
		free(t->disabled[i]);
	free(t->disabled);
	free(t);
}

int
bd_trigger_add(bd_triggers *t, bd_trigger_type type, const char *pattern,
               const char *body, const char *class, int priority, int stop)
{
	struct trigger *tr;

	if (!t || !pattern || !body)
		return -1;
	if (t->n == t->cap) {
		int nc = t->cap ? t->cap * 2 : 16;
		struct trigger *nt = realloc(t->trig, (size_t)nc * sizeof *nt);
		if (!nt)
			return -1;
		t->trig = nt;
		t->cap = nc;
	}
	tr = &t->trig[t->n];
	memset(tr, 0, sizeof *tr);
	tr->id = t->next_id++;
	tr->type = type;
	tr->pattern = strdup(pattern);
	tr->body = strdup(body);
	tr->cls = strdup((class && *class) ? class : "default");
	tr->priority = priority < 0 ? BD_TRIG_PRIO_DEFAULT : priority;
	tr->stop = stop ? 1 : 0;
	tr->seq = t->next_seq++;
	tr->alive = 1;
	if (!tr->pattern || !tr->body || !tr->cls) {
		free(tr->pattern);
		free(tr->body);
		free(tr->cls);
		return -1;
	}
	t->n++;
	t->sorted = 0;
	return tr->id;
}

void
bd_trigger_remove(bd_triggers *t, int id)
{
	int i;
	if (!t)
		return;
	for (i = 0; i < t->n; i++) {
		if (t->trig[i].id != id)
			continue;
		free(t->trig[i].pattern);
		free(t->trig[i].body);
		free(t->trig[i].cls);
		t->trig[i] = t->trig[--t->n];
		t->sorted = 0;
		return;
	}
}

int
bd_trigger_count(const bd_triggers *t)
{
	return t ? t->n : 0;
}

bd_vm *
bd_triggers_vm(bd_triggers *t)
{
	return t ? t->vm : NULL;
}

/* dispatch order: priority desc, then insertion order (stable) */
static int
cmp_trig(const void *a, const void *b)
{
	const struct trigger *x = a, *y = b;
	if (x->priority != y->priority)
		return y->priority - x->priority;
	return x->seq - y->seq;
}

static void
ensure_sorted(bd_triggers *t)
{
	if (t->sorted)
		return;
	qsort(t->trig, (size_t)t->n, sizeof *t->trig, cmp_trig);
	t->sorted = 1;
}

/* Run a matched trigger's body: '@' -> Lua eval, else send as a command. */
static void
fire(bd_triggers *t, struct trigger *tr, const char *subject,
     const struct span cap[10])
{
	char buf[BODY_MAX];
	const char *body = tr->body;

	if (body[0] == '@') {
		expand(body + 1, subject, cap, buf, sizeof buf);
		if (t->vm)
			bd_vm_eval(t->vm, buf);
	} else {
		expand(body, subject, cap, buf, sizeof buf);
		if (t->send)
			t->send(buf, t->ctx);
	}
}

/* Common path for the text-matched types (action/alias/prompt). */
static int
dispatch_text(bd_triggers *t, bd_trigger_type type, const char *subject)
{
	struct span cap[10];
	int i, fired = 0;

	if (!t || !subject)
		return 0;
	ensure_sorted(t);
	for (i = 0; i < t->n; i++) {
		struct trigger *tr = &t->trig[i];
		if (tr->type != type)
			continue;
		if (!bd_class_enabled(t, tr->cls))
			continue;
		memset(cap, 0, sizeof cap);
		if (!match(tr->pattern, subject, cap))
			continue;
		fire(t, tr, subject, cap);
		fired = 1;
		if (tr->stop)
			break;
	}
	return fired;
}

int
bd_triggers_line(bd_triggers *t, const char *line)
{
	return dispatch_text(t, BD_TRIG_ACTION, line);
}

int
bd_triggers_prompt(bd_triggers *t, const char *text)
{
	return dispatch_text(t, BD_TRIG_PROMPT, text);
}

int
bd_triggers_input(bd_triggers *t, const char *cmd)
{
	return dispatch_text(t, BD_TRIG_ALIAS, cmd);
}

int
bd_triggers_gmcp(bd_triggers *t, const char *pkg, const char *json)
{
	struct span cap[10];
	int i, fired = 0;

	if (!t || !pkg)
		return 0;
	ensure_sorted(t);
	memset(cap, 0, sizeof cap);
	for (i = 0; i < t->n; i++) {
		struct trigger *tr = &t->trig[i];
		if (tr->type != BD_TRIG_GMCP)
			continue;
		if (!bd_class_enabled(t, tr->cls))
			continue;
		if (strcmp(tr->pattern, pkg) != 0)
			continue;
		/* %0 = the JSON payload, so a command/Lua body can use it */
		if (json) {
			cap[0].off = 0;
			cap[0].len = (int)strlen(json);
			fire(t, tr, json, cap);
		} else {
			fire(t, tr, "", cap);
		}
		fired = 1;
		if (tr->stop)
			break;
	}
	return fired;
}
