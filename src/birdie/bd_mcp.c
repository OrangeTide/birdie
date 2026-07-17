/*
 * bd_mcp -- MUD Client Protocol 2.1. See bd_mcp.h and doc/mcp.md.
 *
 * The client is fed complete received lines. Lines beginning with the marker
 * `#$#` are MCP messages (handshake, negotiation, packages); `#$"` is a quoted
 * ordinary line (marker stripped, then displayed); everything else is ordinary
 * text. A message may span several lines via a data-tagged multiline value,
 * collected until its closing `#$#: <tag>` line before it is dispatched.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_mcp.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define KEYLEN   40
#define TAGLEN   32
#define REFLEN   256
#define NAMELEN  128
#define TYPELEN  32
#define MLKEYLEN 32
#define NCOLL    4
#define SIMPLEEDIT "dns-org-mud-moo-simpleedit"

/* One in-progress multiline message, keyed by its data tag. */
struct coll {
	int   used;
	char  tag[TAGLEN];
	char  msg[64];
	char  reference[REFLEN];
	char  name[NAMELEN];
	char  type[TYPELEN];
	char  mlkey[MLKEYLEN];   /* which key the continuation lines feed */
	char *body;
	int   body_len, body_cap;
};

struct bd_mcp {
	bd_mcp_cb   cb;
	int         active;
	char        key[KEYLEN];  /* our authentication key */
	unsigned    rng;          /* tag/key generator state */
	int         server_simpleedit;
	struct coll coll[NCOLL];
};

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

static char
lc(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* case-insensitive compare of s[0,n) to NUL-terminated lit */
static int
ci_eq(const char *s, int n, const char *lit)
{
	int i;
	for (i = 0; i < n; i++) {
		if (!lit[i] || lc(s[i]) != lc(lit[i]))
			return 0;
	}
	return lit[i] == '\0';
}

/* bounded copy that never warns about truncation (snprintf "%s" does) */
static void
cpstr(char *dst, size_t cap, const char *src)
{
	size_t n = strlen(src);
	if (n >= cap)
		n = cap - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static unsigned
rng_next(struct bd_mcp *m)
{
	/* xorshift32; seeded once in bd_mcp_new */
	unsigned x = m->rng;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return (m->rng = x);
}

/* fill buf with `len` base36 chars from the rng (NUL-terminated) */
static void
gen_token(struct bd_mcp *m, char *buf, int len)
{
	static const char alpha[] = "0123456789abcdefghijklmnopqrstuvwxyz";
	int i;
	for (i = 0; i < len - 1; i++)
		buf[i] = alpha[rng_next(m) % 36];
	buf[i] = '\0';
}

/* ------------------------------------------------------------------ */
/* line building + quoting for outbound messages                      */
/* ------------------------------------------------------------------ */

/* Append a value to a build buffer, quoting if it contains a space, a quote, a
 * backslash, or is empty. Returns the new offset (clamped to cap). */
static int
put_value(char *b, int off, int cap, const char *v)
{
	int need_quote = (v[0] == '\0');
	const char *p;
	for (p = v; *p && !need_quote; p++)
		if (*p == ' ' || *p == '"' || *p == '\\' || *p == '*' || *p == ':')
			need_quote = 1;
	if (!need_quote) {
		for (p = v; *p && off < cap - 1; p++)
			b[off++] = *p;
		return off;
	}
	if (off < cap - 1)
		b[off++] = '"';
	for (p = v; *p && off < cap - 2; p++) {
		if (*p == '"' || *p == '\\')
			b[off++] = '\\';
		b[off++] = *p;
	}
	if (off < cap - 1)
		b[off++] = '"';
	return off;
}

static int
put_str(char *b, int off, int cap, const char *s)
{
	while (*s && off < cap - 1)
		b[off++] = *s++;
	return off;
}

/* ------------------------------------------------------------------ */
/* parsing                                                            */
/* ------------------------------------------------------------------ */

/* Read one whitespace-delimited token into buf. Advances *pp. */
static int
tok(const char **pp, const char *end, char *buf, int cap)
{
	const char *p = *pp;
	int n = 0;
	while (p < end && *p == ' ')
		p++;
	while (p < end && *p != ' ') {
		if (n < cap - 1)
			buf[n++] = *p;
		p++;
	}
	buf[n] = '\0';
	*pp = p;
	return n;
}

/* Read a value: a "quoted string" (with \" \\ escapes) or a bare token. */
static void
read_value(const char **pp, const char *end, char *buf, int cap)
{
	const char *p = *pp;
	int n = 0;
	while (p < end && *p == ' ')
		p++;
	if (p < end && *p == '"') {
		p++;
		while (p < end && *p != '"') {
			if (*p == '\\' && p + 1 < end)
				p++;
			if (n < cap - 1)
				buf[n++] = *p;
			p++;
		}
		if (p < end && *p == '"')
			p++;
	} else {
		while (p < end && *p != ' ') {
			if (n < cap - 1)
				buf[n++] = *p;
			p++;
		}
	}
	buf[n] = '\0';
	*pp = p;
}

/* ------------------------------------------------------------------ */
/* collectors                                                         */
/* ------------------------------------------------------------------ */

static struct coll *
coll_find(struct bd_mcp *m, const char *tag)
{
	int i;
	for (i = 0; i < NCOLL; i++)
		if (m->coll[i].used && strcmp(m->coll[i].tag, tag) == 0)
			return &m->coll[i];
	return NULL;
}

static struct coll *
coll_open(struct bd_mcp *m, const char *tag)
{
	int i;
	for (i = 0; i < NCOLL; i++)
		if (!m->coll[i].used) {
			struct coll *c = &m->coll[i];
			memset(c, 0, sizeof *c);
			c->used = 1;
			snprintf(c->tag, sizeof c->tag, "%s", tag);
			return c;
		}
	return NULL;   /* too many concurrent multiline messages; drop */
}

static void
coll_close(struct coll *c)
{
	free(c->body);
	c->body = NULL;
	c->used = 0;
}

static void
coll_append(struct coll *c, const char *line, int len)
{
	int need = c->body_len + len + 1;   /* + separator newline */
	if (need > c->body_cap) {
		int nc = c->body_cap ? c->body_cap * 2 : 256;
		char *nb;
		while (nc < need)
			nc *= 2;
		nb = realloc(c->body, (size_t)nc);
		if (!nb)
			return;
		c->body = nb;
		c->body_cap = nc;
	}
	if (c->body_len > 0)
		c->body[c->body_len++] = '\n';
	memcpy(c->body + c->body_len, line, (size_t)len);
	c->body_len += len;
}

/* ------------------------------------------------------------------ */
/* dispatch                                                           */
/* ------------------------------------------------------------------ */

static void
send_line(struct bd_mcp *m, const char *line)
{
	if (m->cb.send)
		m->cb.send(line, m->cb.ctx);
}

/* Announce our packages and finish negotiation. */
static void
send_negotiate(struct bd_mcp *m)
{
	char b[256];
	int off;

	off = snprintf(b, sizeof b,
	    "#$#mcp-negotiate-can %s package: mcp-negotiate "
	    "min-version: 1.0 max-version: 2.0", m->key);
	(void)off;
	send_line(m, b);
	snprintf(b, sizeof b,
	    "#$#mcp-negotiate-can %s package: " SIMPLEEDIT
	    " min-version: 1.0 max-version: 1.0", m->key);
	send_line(m, b);
	snprintf(b, sizeof b, "#$#mcp-negotiate-end %s", m->key);
	send_line(m, b);
}

/* The initial `#$#mcp` handshake (no auth key on this message). */
static void
handle_mcp(struct bd_mcp *m)
{
	char b[128];
	if (m->active)
		return;
	gen_token(m, m->key, 13);           /* 12-char random auth key */
	m->active = 1;
	snprintf(b, sizeof b,
	    "#$#mcp authentication-key: %s version: 2.1 to: 2.1", m->key);
	send_line(m, b);
	send_negotiate(m);
}

/* A completed simpleedit-content collector: hand it to the app. */
static void
dispatch_edit(struct bd_mcp *m, struct coll *c)
{
	bd_mcp_edit e;
	if (!m->cb.edit)
		return;
	e.reference = c->reference;
	e.name = c->name;
	e.type = c->type[0] ? c->type : "string-list";
	e.content = c->body ? c->body : "";
	e.content_len = c->body_len;
	m->cb.edit(&e, m->cb.ctx);
}

/* ------------------------------------------------------------------ */
/* message handling                                                   */
/* ------------------------------------------------------------------ */

/* Parse the key: value tail of a message (after name and auth key) into a
 * collector-shaped record, opening a multiline collector when the message
 * carries a `*` value plus a _data-tag. Dispatches single-line messages here.
 * `p`..`end` is the tail. */
static void
parse_message(struct bd_mcp *m, const char *name, const char *p, const char *end)
{
	char reference[REFLEN] = "", name_v[NAMELEN] = "", type[TYPELEN] = "";
	char mlkey[MLKEYLEN] = "", tag[TAGLEN] = "";
	char package[64] = "";
	int has_ml = 0;

	while (p < end) {
		char key[MLKEYLEN];
		char val[REFLEN];
		const char *ks;
		int kn, ml = 0;

		while (p < end && *p == ' ')
			p++;
		if (p >= end)
			break;
		ks = p;
		while (p < end && *p != ':' && *p != ' ')
			p++;
		if (p >= end || *p != ':')
			break;                       /* malformed; stop */
		kn = (int)(p - ks);
		p++;                                 /* skip ':' */
		if (kn > 0 && ks[kn - 1] == '*') {   /* multiline marker */
			ml = 1;
			kn--;
		}
		if (kn >= (int)sizeof key)
			kn = (int)sizeof key - 1;
		memcpy(key, ks, (size_t)kn);
		key[kn] = '\0';
		read_value(&p, end, val, sizeof val);

		if (ml) {
			has_ml = 1;
			snprintf(mlkey, sizeof mlkey, "%s", key);
		} else if (ci_eq(key, kn, "reference")) {
			cpstr(reference, sizeof reference, val);
		} else if (ci_eq(key, kn, "name")) {
			cpstr(name_v, sizeof name_v, val);
		} else if (ci_eq(key, kn, "type")) {
			cpstr(type, sizeof type, val);
		} else if (ci_eq(key, kn, "_data-tag")) {
			cpstr(tag, sizeof tag, val);
		} else if (ci_eq(key, kn, "package")) {
			cpstr(package, sizeof package, val);
		}
	}

	if (ci_eq(name, (int)strlen(name), "mcp-negotiate-can")) {
		if (ci_eq(package, (int)strlen(package), SIMPLEEDIT))
			m->server_simpleedit = 1;
		return;
	}
	if (ci_eq(name, (int)strlen(name), "mcp-negotiate-end"))
		return;

	if (has_ml && tag[0]) {                  /* a multiline message: collect */
		struct coll *c = coll_open(m, tag);
		if (!c)
			return;
		snprintf(c->msg, sizeof c->msg, "%s", name);
		snprintf(c->reference, sizeof c->reference, "%s", reference);
		snprintf(c->name, sizeof c->name, "%s", name_v);
		snprintf(c->type, sizeof c->type, "%s", type);
		snprintf(c->mlkey, sizeof c->mlkey, "%s", mlkey);
		return;                              /* dispatched on close */
	}

	/* single-line message with no multiline body */
	if (ci_eq(name, (int)strlen(name), SIMPLEEDIT "-content")) {
		struct coll tmp;
		memset(&tmp, 0, sizeof tmp);
		snprintf(tmp.reference, sizeof tmp.reference, "%s", reference);
		snprintf(tmp.name, sizeof tmp.name, "%s", name_v);
		snprintf(tmp.type, sizeof tmp.type, "%s", type);
		dispatch_edit(m, &tmp);
	}
}

/* ------------------------------------------------------------------ */
/* feed                                                               */
/* ------------------------------------------------------------------ */

int
bd_mcp_feed(bd_mcp *m, const char *line, int len, const char **out, int *out_len)
{
	const char *p, *end = line + len;

	/* classify by the three-byte marker */
	if (len < 3 || line[0] != '#' || line[1] != '$' ||
	    (line[2] != '#' && line[2] != '"')) {
		*out = line;
		*out_len = len;
		return BD_MCP_TEXT;              /* ordinary line */
	}
	if (line[2] == '"') {                    /* quoted ordinary line */
		*out = line + 3;
		*out_len = len - 3;
		return BD_MCP_TEXT;
	}

	/* line[2] == '#' : an MCP control line */
	p = line + 3;

	if (p < end && *p == '*') {              /* #$#* <tag> <key>: <value> */
		char tag[TAGLEN], key[MLKEYLEN], val[REFLEN];
		struct coll *c;
		const char *ks;
		int kn;
		p++;
		tok(&p, end, tag, sizeof tag);
		c = coll_find(m, tag);
		while (p < end && *p == ' ')
			p++;
		ks = p;
		while (p < end && *p != ':' && *p != ' ')
			p++;
		kn = (int)(p - ks);
		if (kn >= (int)sizeof key)
			kn = (int)sizeof key - 1;
		memcpy(key, ks, (size_t)kn);
		key[kn] = '\0';
		if (p < end && *p == ':')
			p++;
		if (p < end && *p == ' ')
			p++;                         /* one separator space */
		/* the rest of the line is the raw value (verbatim, incl. spaces) */
		if (c && ci_eq(key, kn, c->mlkey))
			coll_append(c, p, (int)(end - p));
		(void)val;
		return BD_MCP_CONSUMED;
	}

	if (p < end && *p == ':') {              /* #$#: <tag> -- close */
		char tag[TAGLEN];
		struct coll *c;
		p++;
		tok(&p, end, tag, sizeof tag);
		c = coll_find(m, tag);
		if (c) {
			if (ci_eq(c->msg, (int)strlen(c->msg), SIMPLEEDIT "-content"))
				dispatch_edit(m, c);
			coll_close(c);
		}
		return BD_MCP_CONSUMED;
	}

	/* #$#<name> [<auth-key>] <key>: <value> ... */
	{
		char name[64], authkey[KEYLEN];
		tok(&p, end, name, sizeof name);
		if (name[0] == '\0')
			return BD_MCP_CONSUMED;
		if (ci_eq(name, (int)strlen(name), "mcp")) {
			handle_mcp(m);           /* the keyless handshake message */
			return BD_MCP_CONSUMED;
		}
		/* every other message carries our auth key first */
		tok(&p, end, authkey, sizeof authkey);
		if (!m->active || strcmp(authkey, m->key) != 0)
			return BD_MCP_CONSUMED;  /* not ours: drop the #$# line */
		parse_message(m, name, p, end);
		return BD_MCP_CONSUMED;
	}
}

/* ------------------------------------------------------------------ */
/* outbound simpleedit-set                                            */
/* ------------------------------------------------------------------ */

void
bd_mcp_edit_done(bd_mcp *m, const char *reference, const char *type,
                 const char *text)
{
	char b[2048];
	char tag[TAGLEN];
	const char *ls, *p;
	int off;

	if (!m || !m->active)
		return;
	gen_token(m, tag, 9);

	off = 0;
	off = put_str(b, off, sizeof b, "#$#" SIMPLEEDIT "-set ");
	off = put_str(b, off, sizeof b, m->key);
	off = put_str(b, off, sizeof b, " reference: ");
	off = put_value(b, off, sizeof b, reference ? reference : "");
	off = put_str(b, off, sizeof b, " type: ");
	off = put_value(b, off, sizeof b, (type && type[0]) ? type : "string-list");
	off = put_str(b, off, sizeof b, " content*: \"\" _data-tag: ");
	off = put_str(b, off, sizeof b, tag);
	b[off] = '\0';
	send_line(m, b);

	/* one `#$#* <tag> content: <line>` per line of text */
	ls = text ? text : "";
	for (;;) {
		p = ls;
		while (*p && *p != '\n')
			p++;
		off = 0;
		off = put_str(b, off, sizeof b, "#$#* ");
		off = put_str(b, off, sizeof b, tag);
		off = put_str(b, off, sizeof b, " content: ");
		{
			int n = (int)(p - ls);
			int i;
			for (i = 0; i < n && off < (int)sizeof b - 1; i++)
				b[off++] = ls[i];
		}
		b[off] = '\0';
		send_line(m, b);
		if (!*p)
			break;
		ls = p + 1;
	}

	off = 0;
	off = put_str(b, off, sizeof b, "#$#: ");
	off = put_str(b, off, sizeof b, tag);
	b[off] = '\0';
	send_line(m, b);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

bd_mcp *
bd_mcp_new(const bd_mcp_cb *cb)
{
	static unsigned seed_counter = 0x9e3779b9u;
	bd_mcp *m = calloc(1, sizeof *m);
	if (!m)
		return NULL;
	if (cb)
		m->cb = *cb;
	seed_counter += 0x6d2b79f5u;
	m->rng = seed_counter | 1u;          /* nonzero xorshift seed */
	return m;
}

void
bd_mcp_reset(bd_mcp *m)
{
	int i;
	if (!m)
		return;
	for (i = 0; i < NCOLL; i++)
		if (m->coll[i].used)
			coll_close(&m->coll[i]);
	m->active = 0;
	m->server_simpleedit = 0;
	m->key[0] = '\0';
}

void
bd_mcp_free(bd_mcp *m)
{
	if (!m)
		return;
	bd_mcp_reset(m);
	free(m);
}

int
bd_mcp_active(const bd_mcp *m)
{
	return m ? m->active : 0;
}

const char *
bd_mcp_authkey(const bd_mcp *m)
{
	return m ? m->key : "";
}
