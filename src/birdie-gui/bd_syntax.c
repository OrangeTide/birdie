/*
 * bd_syntax -- runtime-configurable syntax highlighter. See bd_syntax.h and
 * doc/gui/editor-highlight.md.
 *
 * A language compiles to a set of named states. Each state has an ordered list
 * of rules (a character class or a literal string, plus the state to move to)
 * and an optional keyword map. The tokenizer walks the buffer one byte at a
 * time, picks the first matching rule, colours the byte with the state's style,
 * and moves on, optionally re-processing the same byte in the next state
 * (noeat) or recolouring a few preceding bytes so an opening delimiter takes
 * the body's colour (recolor). State persists across newlines, so block
 * comments and strings fall out for free. The output is a per-byte style array
 * collapsed into (start, end, style) spans.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_syntax.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* representation                                                     */
/* ------------------------------------------------------------------ */

enum rule_kind { R_CLASS, R_LITERAL };

struct rule {
	int   kind;       /* R_CLASS or R_LITERAL */
	char *pat;        /* class string, or the literal */
	int   patlen;     /* literal length (R_LITERAL) */
	int   next;       /* target state index */
	int   recolor;    /* recolour this many preceding bytes (0 = none) */
	int   noeat;      /* re-process the byte in `next` */
	int   buffer;     /* mark the start of a keyword buffer here */
};

struct kw {
	char *word;
	int   wlen;
	int   style;
};

struct state {
	int   style;          /* style index for bytes coloured in this state */
	int   deflt;          /* fallback transition when no rule matches */
	int   default_noeat;  /* re-process the byte in `deflt` (identifier exit) */
	struct rule *rules;
	int   nrule, rulecap;
	struct kw *kws;
	int   nkw, kwcap;
	char *name;           /* used only during parse to resolve references */
};

struct style {
	char    *name;
	unsigned flags;
	uint32_t fg, bg;
	int      styled;      /* 0 = default/plain: emit no span */
};

struct bd_syntax_lang {
	char        *name;
	int          start;
	struct style *styles;
	int          nstyle, stylecap;
	struct state *states;
	int          nstate, statecap;
};

/* ------------------------------------------------------------------ */
/* character-class matching                                           */
/* ------------------------------------------------------------------ */

static unsigned char
cls_esc(char c)
{
	switch (c) {
	case 'n': return '\n';
	case 't': return '\t';
	case 'r': return '\r';
	case 's': return ' ';
	default:  return (unsigned char)c;
	}
}

/* Does byte `ch` fall in the class string `pat`? Classes are sequences of
 * literals and A-Z style ranges, with backslash escapes (\n \t \r \s \\ ...). */
static int
cls_match(const char *pat, unsigned char ch)
{
	const char *p = pat;
	while (*p) {
		unsigned char a;
		if (*p == '\\' && p[1]) { a = cls_esc(p[1]); p += 2; }
		else { a = (unsigned char)*p; p++; }
		if (*p == '-' && p[1]) {   /* a range a-b */
			unsigned char b;
			p++;
			if (*p == '\\' && p[1]) { b = cls_esc(p[1]); p += 2; }
			else { b = (unsigned char)*p; p++; }
			if (ch >= a && ch <= b)
				return 1;
		} else if (ch == a) {
			return 1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* tokenizer                                                          */
/* ------------------------------------------------------------------ */

static int
kw_lookup(const struct state *st, const char *word, int wlen)
{
	for (int i = 0; i < st->nkw; i++)
		if (st->kws[i].wlen == wlen &&
		    memcmp(st->kws[i].word, word, (size_t)wlen) == 0)
			return st->kws[i].style;
	return -1;
}

int
bd_syntax_run(const bd_syntax_lang *lang, const char *buf, int len,
              bd_syntax_span *out, int max)
{
	if (!lang || !buf || len <= 0 || lang->nstate == 0)
		return 0;

	int *cs = calloc((size_t)len, sizeof *cs);   /* style index per byte */
	if (!cs)
		return 0;

	int state = lang->start;
	int pos = 0, buf_start = -1;
	int guard_pos = -1, guard_state = -1;

	while (pos < len) {
		const struct state *st = &lang->states[state];
		unsigned char ch = (unsigned char)buf[pos];

		/* progress guard: a noeat rule that neither advances nor changes
		 * state would loop forever; force one byte of progress */
		if (pos == guard_pos && state == guard_state) {
			cs[pos] = st->style;
			pos++;
			continue;
		}
		guard_pos = pos;
		guard_state = state;

		const struct rule *rule = NULL;
		for (int r = 0; r < st->nrule; r++) {
			const struct rule *rr = &st->rules[r];
			if (rr->kind == R_CLASS) {
				if (cls_match(rr->pat, ch)) { rule = rr; break; }
			} else if (pos + rr->patlen <= len &&
			    memcmp(buf + pos, rr->pat, (size_t)rr->patlen) == 0) {
				rule = rr;
				break;
			}
		}

		if (rule) {
			int n = rule->kind == R_LITERAL ? rule->patlen : 1;
			for (int k = 0; k < n; k++)
				cs[pos + k] = st->style;
			if (rule->buffer)
				buf_start = pos;
			if (rule->recolor) {
				int rc = rule->kind == R_LITERAL ? rule->patlen
				    : rule->recolor;
				int k0 = pos + n - rc;
				if (k0 < 0)
					k0 = 0;
				int tgt = lang->states[rule->next].style;
				for (int k = k0; k < pos + n; k++)
					cs[k] = tgt;
			}
			state = rule->next;
			if (!rule->noeat)
				pos += n;
		} else {
			cs[pos] = st->style;
			if (st->default_noeat) {
				if (st->nkw && buf_start >= 0) {
					int ks = kw_lookup(st, buf + buf_start,
					    pos - buf_start);
					if (ks >= 0)
						for (int k = buf_start; k < pos; k++)
							cs[k] = ks;
				}
				buf_start = -1;
				state = st->deflt;       /* re-process this byte */
			} else {
				state = st->deflt;
				pos++;
			}
		}
	}

	/* identifier running to end-of-buffer: flush its keyword lookup */
	if (buf_start >= 0) {
		const struct state *st = &lang->states[state];
		if (st->nkw) {
			int ks = kw_lookup(st, buf + buf_start, len - buf_start);
			if (ks >= 0)
				for (int k = buf_start; k < len; k++)
					cs[k] = ks;
		}
	}

	/* collapse the per-byte style array into spans, skipping plain styles */
	int n = 0;
	int i = 0;
	while (i < len && n < max) {
		int s = cs[i];
		int j = i + 1;
		while (j < len && cs[j] == s)
			j++;
		if (s >= 0 && s < lang->nstyle && lang->styles[s].styled) {
			out[n].start = i;
			out[n].end = j;
			out[n].flags = lang->styles[s].flags;
			out[n].fg = lang->styles[s].fg;
			out[n].bg = lang->styles[s].bg;
			n++;
		}
		i = j;
	}

	free(cs);
	return n;
}

const char *
bd_syntax_name(const bd_syntax_lang *lang)
{
	return lang ? lang->name : NULL;
}

/* ------------------------------------------------------------------ */
/* parser                                                             */
/* ------------------------------------------------------------------ */

static char *
dup_n(const char *s, int n)
{
	char *p = malloc((size_t)n + 1);
	if (!p)
		return NULL;
	memcpy(p, s, (size_t)n);
	p[n] = '\0';
	return p;
}

/* Read one whitespace-delimited token from *pp (within the current line);
 * returns its length and advances *pp, or 0 at end of line. */
static int
next_tok(const char **pp, const char *end, const char **tok)
{
	const char *p = *pp;
	while (p < end && (*p == ' ' || *p == '\t'))
		p++;
	const char *s = p;
	while (p < end && *p != ' ' && *p != '\t')
		p++;
	*tok = s;
	*pp = p;
	return (int)(p - s);
}

static int
tok_is(const char *t, int n, const char *lit)
{
	return (int)strlen(lit) == n && memcmp(t, lit, (size_t)n) == 0;
}

static int
lang_style_index(struct bd_syntax_lang *L, const char *name, int n)
{
	for (int i = 0; i < L->nstyle; i++)
		if ((int)strlen(L->styles[i].name) == n &&
		    memcmp(L->styles[i].name, name, (size_t)n) == 0)
			return i;
	return -1;
}

static int
lang_state_index(struct bd_syntax_lang *L, const char *name, int n)
{
	for (int i = 0; i < L->nstate; i++)
		if (L->states[i].name &&
		    (int)strlen(L->states[i].name) == n &&
		    memcmp(L->states[i].name, name, (size_t)n) == 0)
			return i;
	return -1;
}

/* Ensure a state named [name,n) exists (creating a placeholder if needed) and
 * return its index. Placeholders let a rule reference a state defined later. */
static int
lang_state_ensure(struct bd_syntax_lang *L, const char *name, int n)
{
	int i = lang_state_index(L, name, n);
	if (i >= 0)
		return i;
	if (L->nstate == L->statecap) {
		int c = L->statecap ? L->statecap * 2 : 8;
		struct state *ns = realloc(L->states, (size_t)c * sizeof *ns);
		if (!ns)
			return -1;
		L->states = ns;
		L->statecap = c;
	}
	struct state *st = &L->states[L->nstate];
	memset(st, 0, sizeof *st);
	st->name = dup_n(name, n);
	st->deflt = L->nstate;        /* default to self until a header sets it */
	return L->nstate++;
}

/* Parse #RRGGBB / #RRGGBBAA into RGBA8; returns 0 on a bad hex. */
static uint32_t
parse_hex(const char *s, int n)
{
	if (n < 1 || s[0] != '#')
		return 0;
	unsigned long v = 0;
	int digits = 0;
	for (int i = 1; i < n; i++) {
		int c = s[i], d;
		if (c >= '0' && c <= '9') d = c - '0';
		else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
		else return 0;
		v = v * 16 + (unsigned)d;
		digits++;
	}
	if (digits == 6)
		return (uint32_t)((v << 8) | 0xFF);      /* RRGGBB -> RRGGBBAA */
	if (digits == 8)
		return (uint32_t)v;
	return 0;
}

static int
add_style(struct bd_syntax_lang *L, const char *name, int nn, unsigned flags,
    uint32_t fg, uint32_t bg, int styled)
{
	if (L->nstyle == L->stylecap) {
		int c = L->stylecap ? L->stylecap * 2 : 8;
		struct style *ns = realloc(L->styles, (size_t)c * sizeof *ns);
		if (!ns)
			return -1;
		L->styles = ns;
		L->stylecap = c;
	}
	struct style *s = &L->styles[L->nstyle];
	s->name = dup_n(name, nn);
	s->flags = flags;
	s->fg = fg;
	s->bg = bg;
	s->styled = styled;
	return L->nstyle++;
}

static struct rule *
add_rule(struct state *st)
{
	if (st->nrule == st->rulecap) {
		int c = st->rulecap ? st->rulecap * 2 : 4;
		struct rule *nr = realloc(st->rules, (size_t)c * sizeof *nr);
		if (!nr)
			return NULL;
		st->rules = nr;
		st->rulecap = c;
	}
	struct rule *r = &st->rules[st->nrule++];
	memset(r, 0, sizeof *r);
	return r;
}

static int
add_kw(struct state *st, const char *w, int n, int style)
{
	if (st->nkw == st->kwcap) {
		int c = st->kwcap ? st->kwcap * 2 : 8;
		struct kw *nk = realloc(st->kws, (size_t)c * sizeof *nk);
		if (!nk)
			return -1;
		st->kws = nk;
		st->kwcap = c;
	}
	struct kw *k = &st->kws[st->nkw++];
	k->word = dup_n(w, n);
	k->wlen = n;
	k->style = style;
	return 0;
}

/* Unescape a literal token in place (\n \t \r \s \\), returning the new length. */
static int
unescape(char *s, int n)
{
	int w = 0;
	for (int i = 0; i < n; i++) {
		if (s[i] == '\\' && i + 1 < n) {
			s[w++] = (char)cls_esc(s[i + 1]);
			i++;
		} else {
			s[w++] = s[i];
		}
	}
	s[w] = '\0';
	return w;
}

bd_syntax_lang *
bd_syntax_parse(const char *text, int len)
{
	if (!text)
		return NULL;
	if (len < 0)
		len = (int)strlen(text);

	struct bd_syntax_lang *L = calloc(1, sizeof *L);
	if (!L)
		return NULL;
	L->start = 0;

	const char *p = text, *end = text + len;
	int cur_state = -1;                  /* state a rule/keyword line applies to */
	char pending_start[64];              /* name of `start` before it exists */
	int  pending_start_n = 0;

	while (p < end) {
		const char *ls = p;
		while (p < end && *p != '\n')
			p++;
		const char *le = p;
		if (p < end)
			p++;                     /* skip newline */

		const char *q = ls, *tok;
		int tn = next_tok(&q, le, &tok);
		if (tn == 0 || tok[0] == '#')
			continue;

		if (tok_is(tok, tn, "name")) {
			int nn = next_tok(&q, le, &tok);
			free(L->name);
			L->name = dup_n(tok, nn);
		} else if (tok_is(tok, tn, "start")) {
			int nn = next_tok(&q, le, &tok);
			if (nn > 0 && nn < (int)sizeof pending_start) {
				memcpy(pending_start, tok, (size_t)nn);
				pending_start_n = nn;
			}
		} else if (tok_is(tok, tn, "style")) {
			int nn = next_tok(&q, le, &tok);
			const char *sname = tok;
			int snn = nn;
			unsigned flags = 0;
			uint32_t fg = 0, bg = 0;
			int styled = 0;
			int an;
			const char *arg;
			while ((an = next_tok(&q, le, &arg)) > 0) {
				if (tok_is(arg, an, "bold")) flags |= BD_SYN_BOLD, styled = 1;
				else if (tok_is(arg, an, "italic")) flags |= BD_SYN_ITALIC, styled = 1;
				else if (tok_is(arg, an, "underline")) flags |= BD_SYN_UNDERLINE, styled = 1;
				else if (an > 3 && memcmp(arg, "bg:", 3) == 0) { bg = parse_hex(arg + 3, an - 3); styled = 1; }
				else if (arg[0] == '#') { fg = parse_hex(arg, an); styled = 1; }
				/* "-" (or anything else) leaves the style plain */
			}
			add_style(L, sname, snn, flags, fg, bg, styled);
		} else if (tok_is(tok, tn, "state")) {
			int nn = next_tok(&q, le, &tok);
			cur_state = lang_state_ensure(L, tok, nn);
			if (cur_state < 0)
				break;
			struct state *st = &L->states[cur_state];
			int an;
			const char *arg;
			while ((an = next_tok(&q, le, &arg)) > 0) {
				if (tok_is(arg, an, "style")) {
					an = next_tok(&q, le, &arg);
					int si = lang_style_index(L, arg, an);
					if (si >= 0) st->style = si;
				} else if (tok_is(arg, an, "default")) {
					an = next_tok(&q, le, &arg);
					st->deflt = lang_state_ensure(L, arg, an);
				} else if (tok_is(arg, an, "noeat")) {
					st->default_noeat = 1;
				}
			}
		} else if (tok_is(tok, tn, "rule") || tok_is(tok, tn, "str")) {
			if (cur_state < 0)
				continue;
			int literal = tok_is(tok, tn, "str");
			int pn = next_tok(&q, le, &tok);
			char patbuf[128];
			if (pn <= 0 || pn >= (int)sizeof patbuf)
				continue;
			memcpy(patbuf, tok, (size_t)pn);
			patbuf[pn] = '\0';
			int plen = pn;
			if (literal)
				plen = unescape(patbuf, pn);
			int nn = next_tok(&q, le, &tok);      /* next-state name */
			int nxt = lang_state_ensure(L, tok, nn);
			struct rule *r = add_rule(&L->states[cur_state]);
			if (!r || nxt < 0)
				continue;
			r->kind = literal ? R_LITERAL : R_CLASS;
			r->pat = dup_n(patbuf, plen);
			r->patlen = plen;
			r->next = nxt;
			int an;
			const char *arg;
			while ((an = next_tok(&q, le, &arg)) > 0) {
				if (tok_is(arg, an, "noeat")) r->noeat = 1;
				else if (tok_is(arg, an, "buffer")) r->buffer = 1;
				else if (an >= 7 && memcmp(arg, "recolor", 7) == 0)
					r->recolor = an > 7 ? (arg[7] - '0') : 1;
			}
		} else if (tok_is(tok, tn, "keyword")) {
			if (cur_state < 0)
				continue;
			int sn = next_tok(&q, le, &tok);      /* style name */
			int style = lang_style_index(L, tok, sn);
			if (style < 0)
				continue;
			int wn;
			const char *w;
			while ((wn = next_tok(&q, le, &w)) > 0)
				add_kw(&L->states[cur_state], w, wn, style);
		}
	}

	/* resolve the start state */
	if (pending_start_n > 0) {
		int si = lang_state_index(L, pending_start, pending_start_n);
		if (si >= 0)
			L->start = si;
	}
	if (L->nstate == 0) {
		bd_syntax_free(L);
		return NULL;
	}
	return L;
}

void
bd_syntax_free(bd_syntax_lang *L)
{
	if (!L)
		return;
	for (int i = 0; i < L->nstate; i++) {
		struct state *st = &L->states[i];
		for (int r = 0; r < st->nrule; r++)
			free(st->rules[r].pat);
		free(st->rules);
		for (int k = 0; k < st->nkw; k++)
			free(st->kws[k].word);
		free(st->kws);
		free(st->name);
	}
	for (int i = 0; i < L->nstyle; i++)
		free(L->styles[i].name);
	free(L->states);
	free(L->styles);
	free(L->name);
	free(L);
}

/* ------------------------------------------------------------------ */
/* built-in languages + registry                                      */
/* ------------------------------------------------------------------ */

static const char SYN_LUA[] =
	"name lua\n"
	"start idle\n"
	"style default -\n"
	"style keyword #E0A030 bold\n"
	"style comment #6A9955 italic\n"
	"style string  #CE9178\n"
	"style number  #B5CEA8\n"
	"state idle style default default idle\n"
	"  rule a-zA-Z_ ident buffer\n"
	"  rule 0-9 number recolor\n"
	"  rule \" string recolor\n"
	"  rule ' sqstring recolor\n"
	"  str -- comment recolor\n"
	"state ident style default default idle noeat\n"
	"  rule a-zA-Z0-9_ ident\n"
	"  keyword keyword and break do else elseif end false for function if in"
	" local nil not or repeat return then true until while\n"
	"state number style number default idle noeat\n"
	"  rule 0-9 number\n"
	"  rule .xXeEabcdefABCDEF number\n"
	"state comment style comment default comment\n"
	"  rule \\n idle\n"
	"state string style string default string\n"
	"  rule \" idle\n"
	"  rule \\n idle\n"
	"state sqstring style string default sqstring\n"
	"  rule ' idle\n"
	"  rule \\n idle\n";

static const char SYN_ABC[] =
	"name abc\n"
	"start bol\n"
	"style default -\n"
	"style header  #7FB2FF bold\n"
	"style comment #6A9955 italic\n"
	"style bar     #E0A030\n"
	"state bol style default default body\n"
	"  rule % comment recolor\n"
	"  rule A-Za-z hdrmaybe\n"
	"  rule \\n bol\n"
	"state hdrmaybe style default default body noeat\n"
	"  rule : hdrtag recolor2\n"
	"state hdrtag style header default body noeat\n"
	"state body style default default body\n"
	"  rule | bar recolor\n"
	"  rule \\n bol\n"
	"state bar style bar default body noeat\n"
	"state comment style comment default comment\n"
	"  rule \\n bol\n";

static struct {
	const char  *name;
	const char  *src;
	bd_syntax_lang *lang;    /* lazily parsed cache */
} builtins[] = {
	{ "lua", SYN_LUA, NULL },
	{ "abc", SYN_ABC, NULL },
};
#define NBUILTIN ((int)(sizeof builtins / sizeof builtins[0]))

const bd_syntax_lang *
bd_syntax_builtin(const char *name)
{
	if (!name)
		return NULL;
	for (int i = 0; i < NBUILTIN; i++)
		if (strcmp(builtins[i].name, name) == 0) {
			if (!builtins[i].lang)
				builtins[i].lang = bd_syntax_parse(builtins[i].src, -1);
			return builtins[i].lang;
		}
	return NULL;
}

/* extension -> language registry */
#define REG_MAX 32
static struct {
	char  ext[16];
	const bd_syntax_lang *lang;
} registry[REG_MAX];
static int registry_n;
static int registry_seeded;

static void
reg_put(const char *ext, const bd_syntax_lang *lang)
{
	for (int i = 0; i < registry_n; i++)
		if (strcmp(registry[i].ext, ext) == 0) {
			registry[i].lang = lang;
			return;
		}
	if (registry_n >= REG_MAX || (int)strlen(ext) >= (int)sizeof registry[0].ext)
		return;
	strcpy(registry[registry_n].ext, ext);
	registry[registry_n].lang = lang;
	registry_n++;
}

static void
reg_seed(void)
{
	if (registry_seeded)
		return;
	registry_seeded = 1;
	reg_put("lua", bd_syntax_builtin("lua"));
	reg_put("abc", bd_syntax_builtin("abc"));
}

void
bd_syntax_register(const char *name, const char *const *exts,
                   const bd_syntax_lang *lang)
{
	(void)name;
	reg_seed();
	if (!exts)
		return;
	for (int i = 0; exts[i]; i++)
		reg_put(exts[i], lang);
}

const bd_syntax_lang *
bd_syntax_for_name(const char *filename)
{
	if (!filename)
		return NULL;
	reg_seed();
	const char *dot = NULL;
	for (const char *s = filename; *s; s++) {
		if (*s == '/' || *s == '\\')
			dot = NULL;
		else if (*s == '.')
			dot = s;
	}
	if (!dot || !dot[1])
		return NULL;
	const char *ext = dot + 1;
	for (int i = 0; i < registry_n; i++)
		if (strcmp(registry[i].ext, ext) == 0)
			return registry[i].lang;
	return NULL;
}
