/*
 * bd_trigger -- the trigger / alias engine. See bd_trigger.h and
 * doc/triggers.md.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_trigger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BODY_MAX 4096
#define CHAIN_TIMEOUT_MS 8000.0   /* default chain reset timeout */

struct trigger {
	int id;
	bd_trigger_type type;
	char *pattern;
	char *body;
	char *cls;
	char *chain;    /* full chain key "class/chain", or NULL if not chained */
	int state;      /* position in the chain (1..N), 0 if not chained */
	int priority;
	int stop;
	int seq;        /* insertion order, for stable priority sort */
	int alive;
};

/* A multi-state chain: only its `cur` state is armed; firing a trigger in that
 * state advances it, wrapping to 1 past `max`. It resets to 1 on timeout. */
struct chain {
	char *key;
	int cur, max;
	double last_ms;         /* monotonic time of the last advance */
	double timeout_ms;
};

struct bd_triggers {
	bd_vm *vm;
	bd_trigger_send_fn send;
	void *ctx;
	bd_trigger_timer_fn timer_cb;
	void *timer_ctx;
	bd_trigger_event_fn event_cb;
	void *event_ctx;

	struct trigger *trig;
	int n, cap;
	int next_id;
	int next_seq;
	int sorted;             /* trig[] is in dispatch order */

	char **disabled;        /* explicitly disabled class names */
	int ndis, dis_cap;

	struct timer *timers;   /* interval timers (#tick) */
	int nt, t_cap;

	struct chain *chains;   /* multi-state chain registry */
	int nch, ch_cap;
	double now_ms;          /* current monotonic time (set by the session) */
};

struct timer {
	char *name;
	char *body;
	char *cls;
	double interval_ms;
	double next_ms;         /* monotonic deadline; <0 = not yet scheduled */
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
		free(t->trig[i].chain);
	}
	free(t->trig);
	for (i = 0; i < t->ndis; i++)
		free(t->disabled[i]);
	free(t->disabled);
	for (i = 0; i < t->nt; i++) {
		free(t->timers[i].name);
		free(t->timers[i].body);
		free(t->timers[i].cls);
	}
	free(t->timers);
	for (i = 0; i < t->nch; i++)
		free(t->chains[i].key);
	free(t->chains);
	free(t);
}

/* Find/create the chain registered under `key`. ensure_chain returns NULL on
 * allocation failure. */
static struct chain *
find_chain(bd_triggers *t, const char *key)
{
	int i;
	for (i = 0; i < t->nch; i++)
		if (strcmp(t->chains[i].key, key) == 0)
			return &t->chains[i];
	return NULL;
}

static struct chain *
ensure_chain(bd_triggers *t, const char *key)
{
	struct chain *c = find_chain(t, key);
	if (c)
		return c;
	if (t->nch == t->ch_cap) {
		int nc = t->ch_cap ? t->ch_cap * 2 : 8;
		struct chain *nv = realloc(t->chains, (size_t)nc * sizeof *nv);
		if (!nv)
			return NULL;
		t->chains = nv;
		t->ch_cap = nc;
	}
	c = &t->chains[t->nch];
	memset(c, 0, sizeof *c);
	c->key = strdup(key);
	if (!c->key)
		return NULL;
	c->cur = 1;             /* state 1 armed initially */
	c->max = 0;
	c->last_ms = t->now_ms;
	c->timeout_ms = CHAIN_TIMEOUT_MS;
	t->nch++;
	return c;
}

int
bd_trigger_add_chained(bd_triggers *t, bd_trigger_type type,
                       const char *pattern, const char *body,
                       const char *class, const char *chain, int state,
                       int priority, int stop)
{
	struct trigger *tr;
	const char *cls;

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
	cls = (class && *class) ? class : "default";
	tr->cls = strdup(cls);
	tr->priority = priority < 0 ? BD_TRIG_PRIO_DEFAULT : priority;
	tr->stop = stop ? 1 : 0;
	tr->seq = t->next_seq++;
	tr->alive = 1;
	if (chain && *chain && state > 0) {
		char key[256];
		struct chain *c;
		snprintf(key, sizeof key, "%s/%s", cls, chain);
		tr->chain = strdup(key);
		tr->state = state;
		c = ensure_chain(t, key);
		if (c && state > c->max)
			c->max = state;
	}
	if (!tr->pattern || !tr->body || !tr->cls ||
	    (chain && *chain && state > 0 && !tr->chain)) {
		free(tr->pattern);
		free(tr->body);
		free(tr->cls);
		free(tr->chain);
		return -1;
	}
	t->n++;
	t->sorted = 0;
	return tr->id;
}

int
bd_trigger_add(bd_triggers *t, bd_trigger_type type, const char *pattern,
               const char *body, const char *class, int priority, int stop)
{
	return bd_trigger_add_chained(t, type, pattern, body, class, NULL, 0,
	    priority, stop);
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
		free(t->trig[i].chain);
		t->trig[i] = t->trig[--t->n];
		t->sorted = 0;
		return;
	}
}

static void ensure_sorted(bd_triggers *t);

int
bd_trigger_remove_pattern(bd_triggers *t, bd_trigger_type type,
                          const char *pattern, const char *class)
{
	int i, removed = 0;
	const char *cls;

	if (!t || !pattern)
		return 0;
	cls = (class && *class) ? class : NULL;
	for (i = 0; i < t->n; ) {
		struct trigger *tr = &t->trig[i];
		if (tr->type != type || strcmp(tr->pattern, pattern) != 0 ||
		    (cls && strcmp(tr->cls, cls) != 0)) {
			i++;
			continue;
		}
		free(tr->pattern);
		free(tr->body);
		free(tr->cls);
		free(tr->chain);
		t->trig[i] = t->trig[--t->n];
		t->sorted = 0;
		removed++;
	}
	return removed;
}

int
bd_trigger_count(const bd_triggers *t)
{
	return t ? t->n : 0;
}

void
bd_trigger_foreach(bd_triggers *t, bd_trigger_iter_fn fn, void *ctx)
{
	int i;
	if (!t || !fn)
		return;
	ensure_sorted(t);
	for (i = 0; i < t->n; i++) {
		struct trigger *tr = &t->trig[i];
		fn(tr->type, tr->pattern, tr->body, tr->cls, tr->chain,
		   tr->state, tr->priority, bd_class_enabled(t, tr->cls), ctx);
	}
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
fire_body(bd_triggers *t, const char *body, const char *subject,
          const struct span cap[10])
{
	char buf[BODY_MAX];

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

static void
fire(bd_triggers *t, struct trigger *tr, const char *subject,
     const struct span cap[10])
{
	fire_body(t, tr->body, subject, cap);
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
		struct chain *c = NULL;
		if (tr->type != type)
			continue;
		if (!bd_class_enabled(t, tr->cls))
			continue;
		if (tr->chain) {
			c = find_chain(t, tr->chain);
			if (c) {
				/* a stalled chain resets to state 1 on timeout */
				if (c->cur > 1 &&
				    t->now_ms - c->last_ms > c->timeout_ms)
					c->cur = 1;
				if (c->cur != tr->state)
					continue;       /* this state not armed */
			}
		}
		memset(cap, 0, sizeof cap);
		if (!match(tr->pattern, subject, cap))
			continue;
		fire(t, tr, subject, cap);
		fired = 1;
		if (c) {                        /* advance the chain, wrap past max */
			c->cur = tr->state + 1;
			if (c->cur > c->max)
				c->cur = 1;
			c->last_ms = t->now_ms;
		}
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

/* Dispatch an MXP tag to mxp-type triggers (matched by tag name). %0 in the
 * body expands to the tag's attribute string. */
int
bd_triggers_mxp(bd_triggers *t, const char *tag, const char *attrs)
{
	struct span cap[10];
	int i, fired = 0;

	if (!t || !tag)
		return 0;
	ensure_sorted(t);
	memset(cap, 0, sizeof cap);
	for (i = 0; i < t->n; i++) {
		struct trigger *tr = &t->trig[i];
		if (tr->type != BD_TRIG_MXP)
			continue;
		if (!bd_class_enabled(t, tr->cls))
			continue;
		if (strcmp(tr->pattern, tag) != 0)
			continue;
		if (attrs) {
			cap[0].off = 0;
			cap[0].len = (int)strlen(attrs);
			fire(t, tr, attrs, cap);
		} else {
			fire(t, tr, "", cap);
		}
		fired = 1;
		if (tr->stop)
			break;
	}
	return fired;
}

/* ---- line rewriting (#gag / #substitute / #highlight) ---- */

/* Resolve a #highlight color token to SGR parameter text. A leading digit is
 * taken as raw SGR params ("1;33"); otherwise a small set of names is mapped. */
static const char *
hilite_sgr(const char *color)
{
	if (!color || !*color)
		return "1";                     /* bold, a safe default */
	if (color[0] >= '0' && color[0] <= '9')
		return color;
	if (!strcmp(color, "bold"))    return "1";
	if (!strcmp(color, "red"))     return "31";
	if (!strcmp(color, "green"))   return "32";
	if (!strcmp(color, "yellow"))  return "33";
	if (!strcmp(color, "blue"))    return "34";
	if (!strcmp(color, "magenta")) return "35";
	if (!strcmp(color, "cyan"))    return "36";
	if (!strcmp(color, "white"))   return "37";
	if (!strcmp(color, "bred"))    return "91";
	if (!strcmp(color, "bgreen"))  return "92";
	if (!strcmp(color, "byellow")) return "93";
	if (!strcmp(color, "bcyan"))   return "96";
	return "1";
}

/* Append up to `n` bytes of `src` to out[*o] (bounded by cap). */
static void
app(char *out, size_t cap, size_t *o, const char *src, size_t n)
{
	size_t k;
	for (k = 0; k < n && *o + 1 < cap; k++)
		out[(*o)++] = src[k];
}

/* Rewrite every non-overlapping match of `pattern` in `work` (NUL-terminated)
 * into out[outcap]. For highlight, each match is wrapped in ESC[<sgr>m..ESC[0m;
 * otherwise `body` is the substitution (%0..%9 expanded). Returns 1 if any
 * match was rewritten. */
static int
rewrite_pass(const char *work, char *out, size_t outcap, const char *pattern,
             int hilite, const char *body)
{
	struct span cap[10];
	const char *s = work;
	size_t o = 0;
	int changed = 0;

	while (*s) {
		int adv;
		memset(cap, 0, sizeof cap);
		if (!match(pattern, s, cap)) {
			app(out, outcap, &o, s, strlen(s));
			break;
		}
		app(out, outcap, &o, s, (size_t)cap[0].off);   /* text before */
		if (hilite) {
			app(out, outcap, &o, "\033[", 2);
			app(out, outcap, &o, body, strlen(body));
			app(out, outcap, &o, "m", 1);
			app(out, outcap, &o, s + cap[0].off, (size_t)cap[0].len);
			app(out, outcap, &o, "\033[0m", 4);
		} else {
			char rep[BODY_MAX];
			expand(body, s, cap, rep, sizeof rep);
			app(out, outcap, &o, rep, strlen(rep));
		}
		changed = 1;
		adv = cap[0].off + cap[0].len;
		if (cap[0].len == 0) {          /* zero-width: emit a char, advance */
			if (s[adv]) {
				app(out, outcap, &o, s + adv, 1);
				adv++;
			} else {
				break;
			}
		}
		s += adv;
	}
	out[o < outcap ? o : outcap - 1] = '\0';
	return changed;
}

void
bd_triggers_rewrite(bd_triggers *t, const char *line, bd_line_edit *e)
{
	char work[sizeof e->text];
	char next[sizeof e->text];
	int i;

	if (!e)
		return;
	e->gag = 0;
	e->changed = 0;
	e->text[0] = '\0';
	if (!t || !line)
		return;
	snprintf(work, sizeof work, "%s", line);

	ensure_sorted(t);
	for (i = 0; i < t->n; i++) {
		struct trigger *tr = &t->trig[i];
		struct span cap[10];
		if (tr->type != BD_TRIG_GAG && tr->type != BD_TRIG_SUBST &&
		    tr->type != BD_TRIG_HILITE)
			continue;
		if (!bd_class_enabled(t, tr->cls))
			continue;
		if (tr->type == BD_TRIG_GAG) {
			/* match the original line, so a prior substitution cannot
			 * hide text from a gag (predictable ordering) */
			memset(cap, 0, sizeof cap);
			if (match(tr->pattern, line, cap)) {
				e->gag = 1;
				return;                 /* dropped: nothing else matters */
			}
			continue;
		}
		if (rewrite_pass(work, next, sizeof next, tr->pattern,
		    tr->type == BD_TRIG_HILITE,
		    tr->type == BD_TRIG_HILITE ? hilite_sgr(tr->body) : tr->body)) {
			memcpy(work, next, sizeof work);
			e->changed = 1;
		}
	}
	snprintf(e->text, sizeof e->text, "%s", work);
}

/* ---- interval timers (#tick) ---- */

int
bd_trigger_add_tick(bd_triggers *t, const char *name, const char *body,
                    double seconds, const char *class)
{
	struct timer *tm;

	if (!t || !name || !*name || !body || seconds <= 0)
		return -1;
	bd_trigger_remove_tick(t, name);        /* replace same-named timer */
	if (t->nt == t->t_cap) {
		int nc = t->t_cap ? t->t_cap * 2 : 8;
		struct timer *nv = realloc(t->timers, (size_t)nc * sizeof *nv);
		if (!nv)
			return -1;
		t->timers = nv;
		t->t_cap = nc;
	}
	tm = &t->timers[t->nt];
	memset(tm, 0, sizeof *tm);
	tm->name = strdup(name);
	tm->body = strdup(body);
	tm->cls = strdup((class && *class) ? class : "default");
	tm->interval_ms = seconds * 1000.0;
	tm->next_ms = -1.0;                     /* scheduled on first run */
	if (!tm->name || !tm->body || !tm->cls) {
		free(tm->name);
		free(tm->body);
		free(tm->cls);
		return -1;
	}
	t->nt++;
	return 0;
}

void
bd_trigger_remove_tick(bd_triggers *t, const char *name)
{
	int i;
	if (!t || !name)
		return;
	for (i = 0; i < t->nt; i++) {
		if (strcmp(t->timers[i].name, name) != 0)
			continue;
		free(t->timers[i].name);
		free(t->timers[i].body);
		free(t->timers[i].cls);
		t->timers[i] = t->timers[--t->nt];
		return;
	}
}

void
bd_triggers_set_timer_cb(bd_triggers *t, bd_trigger_timer_fn fn, void *ctx)
{
	if (!t)
		return;
	t->timer_cb = fn;
	t->timer_ctx = ctx;
}

void
bd_triggers_set_event_cb(bd_triggers *t, bd_trigger_event_fn fn, void *ctx)
{
	if (!t)
		return;
	t->event_cb = fn;
	t->event_ctx = ctx;
}

void
bd_triggers_event(bd_triggers *t, const char *name, const char *arg)
{
	if (!t || !name || !*name || !t->event_cb)
		return;
	t->event_cb(name, arg ? arg : "", t->event_ctx);
}

int
bd_triggers_run_timers(bd_triggers *t, double now_ms)
{
	struct span cap[10];
	int i, fired = 0;

	if (!t)
		return 0;
	memset(cap, 0, sizeof cap);
	for (i = 0; i < t->nt; i++) {
		struct timer *tm = &t->timers[i];
		if (tm->next_ms < 0) {          /* first sight: schedule */
			tm->next_ms = now_ms + tm->interval_ms;
			continue;
		}
		if (now_ms < tm->next_ms)
			continue;
		if (bd_class_enabled(t, tm->cls)) {
			if (tm->body[0])                /* empty body: hook-only timer */
				fire_body(t, tm->body, "", cap);
			if (t->timer_cb)
				t->timer_cb(tm->name, t->timer_ctx);
			fired++;
		}
		/* reschedule from now, so a stalled frame does not fire a burst */
		tm->next_ms = now_ms + tm->interval_ms;
	}
	return fired;
}

/* ---- multi-state chains ---- */

void
bd_triggers_set_now(bd_triggers *t, double now_ms)
{
	if (t)
		t->now_ms = now_ms;
}

void
bd_trigger_reset(bd_triggers *t, const char *chain)
{
	int i;
	if (!t)
		return;
	for (i = 0; i < t->nch; i++) {
		const char *key = t->chains[i].key;
		const char *slash;
		if (chain && *chain) {
			/* match the full key "class/chain" or just the chain part */
			slash = strrchr(key, '/');
			if (strcmp(key, chain) != 0 &&
			    !(slash && strcmp(slash + 1, chain) == 0))
				continue;
		}
		t->chains[i].cur = 1;
		t->chains[i].last_ms = t->now_ms;
	}
}
