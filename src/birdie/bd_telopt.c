/*
 * bd_telopt -- telnet option state machine. See bd_telopt.h for the contract.
 *
 * Negotiation uses a small per-option latch (us[] / them[]) so a reply goes
 * out only when the agreed state actually changes, which keeps compliant
 * servers from looping. Refusals are sent once per request, which is all a
 * conforming server issues.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_telopt.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* telnet commands */
#define TC_EOR  239
#define TC_SE   240
#define TC_GA   249
#define TC_SB   250
#define TC_WILL 251
#define TC_WONT 252
#define TC_DO   253
#define TC_DONT 254
#define TC_IAC  255

/* options */
#define OPT_ECHO         1
#define OPT_SGA          3
#define OPT_TTYPE       24
#define OPT_EOR         25
#define OPT_NAWS        31
#define OPT_NEW_ENVIRON 39
#define OPT_CHARSET     42
#define OPT_MSDP        69
#define OPT_MCCP2       86
#define OPT_MXP         91
#define OPT_GMCP       201

/* TTYPE / NEW-ENVIRON / CHARSET subnegotiation sub-commands */
#define SB_IS        0
#define SB_SEND      1
#define ENV_VAR      0
#define ENV_VALUE    1
#define ENV_USERVAR  3
#define CS_REQUEST   1
#define CS_ACCEPTED  2
#define CS_REJECTED  3

/* MSDP control bytes */
#define MSDP_VAR          1
#define MSDP_VAL          2
#define MSDP_TABLE_OPEN   3
#define MSDP_TABLE_CLOSE  4
#define MSDP_ARRAY_OPEN   5
#define MSDP_ARRAY_CLOSE  6

#define SB_CAP_MAX  (1u << 20)  /* refuse to buffer a subnegotiation past 1 MiB */
#define MSDP_DEPTH_MAX  32      /* nesting guard for malformed MSDP */

enum state { S_DATA, S_IAC, S_OPT, S_SB, S_SB_IAC };

struct bd_telopt {
	bd_telopt_cb cb;

	enum state st;
	unsigned char verb;             /* WILL/WONT/DO/DONT awaiting its option */
	unsigned char sb_opt;           /* option under subnegotiation */
	int sb_got_opt;                 /* option byte captured this SB */
	unsigned char *sb;              /* growable subnegotiation payload */
	size_t sb_len, sb_cap;
	int sb_overflow;                /* payload exceeded SB_CAP_MAX: drop it */
	int compress_now;               /* MCCP2 began: stop and let caller inflate */

	unsigned char us[256];          /* options we have agreed to perform */
	unsigned char them[256];        /* options the server has agreed to */

	char termtype[64];
	int ttype_idx;                  /* TTYPE/MTTS cycle position */
	int cols, rows;
};

static void
xmit(struct bd_telopt *t, const unsigned char *p, size_t len)
{
	if (t->cb.xmit)
		t->cb.xmit(p, len, t->cb.arg);
}

static void
send3(struct bd_telopt *t, unsigned char cmd, unsigned char opt)
{
	unsigned char b[3] = { TC_IAC, cmd, opt };
	xmit(t, b, sizeof b);
}

/* options we will perform when the server asks (DO -> WILL) */
static int
local_supported(unsigned char opt)
{
	return opt == OPT_TTYPE || opt == OPT_NAWS ||
	       opt == OPT_NEW_ENVIRON || opt == OPT_SGA;
}

/* options we want the server to perform (WILL -> DO) */
static int
remote_wanted(unsigned char opt)
{
	return opt == OPT_CHARSET || opt == OPT_EOR ||
	       opt == OPT_ECHO || opt == OPT_SGA ||
	       opt == OPT_GMCP || opt == OPT_MSDP ||
	       opt == OPT_MCCP2 || opt == OPT_MXP;
}

/* Build and send an escaped subnegotiation: IAC SB <opt> <payload> IAC SE,
 * doubling any 0xFF in the payload. Payload here is always small (we only
 * originate fixed replies and GMCP hellos). */
static void
send_sb(struct bd_telopt *t, unsigned char opt,
        const unsigned char *payload, size_t len)
{
	unsigned char buf[600];
	size_t n = 0, i;

	buf[n++] = TC_IAC;
	buf[n++] = TC_SB;
	buf[n++] = opt;
	for (i = 0; i < len && n < sizeof buf - 2; i++) {
		buf[n++] = payload[i];
		if (payload[i] == TC_IAC)
			buf[n++] = TC_IAC;
	}
	buf[n++] = TC_IAC;
	buf[n++] = TC_SE;
	xmit(t, buf, n);
}

/* Send an outbound GMCP message: IAC SB GMCP "pkg json" IAC SE, payload 0xFF
 * doubled. Heap-buffered so a large package is not truncated (unlike send_sb,
 * which only originates small fixed replies). No-op if pkg is empty. */
void
bd_telopt_send_gmcp(struct bd_telopt *t, const char *pkg, const char *json)
{
	size_t pl, jl, plen, cap, n, i;
	unsigned char *buf;
	const unsigned char *p;

	if (!t || !pkg || !*pkg)
		return;
	pl = strlen(pkg);
	jl = json ? strlen(json) : 0;
	plen = pl + (jl ? 1 + jl : 0);          /* "pkg" + ' ' + "json" */
	cap = 3 + plen * 2 + 2;                  /* worst case all bytes doubled */
	buf = malloc(cap);
	if (!buf)
		return;

	n = 0;
	buf[n++] = TC_IAC;
	buf[n++] = TC_SB;
	buf[n++] = OPT_GMCP;
#define APPEND(byte) do { buf[n++] = (byte); \
		if ((byte) == TC_IAC) buf[n++] = TC_IAC; } while (0)
	for (i = 0, p = (const unsigned char *)pkg; i < pl; i++)
		APPEND(p[i]);
	if (jl) {
		buf[n++] = ' ';
		for (i = 0, p = (const unsigned char *)json; i < jl; i++)
			APPEND(p[i]);
	}
#undef APPEND
	buf[n++] = TC_IAC;
	buf[n++] = TC_SE;
	xmit(t, buf, n);
	free(buf);
}

static void
send_naws(struct bd_telopt *t)
{
	unsigned char p[4];
	p[0] = (unsigned char)(t->cols >> 8);
	p[1] = (unsigned char)(t->cols & 0xff);
	p[2] = (unsigned char)(t->rows >> 8);
	p[3] = (unsigned char)(t->rows & 0xff);
	send_sb(t, OPT_NAWS, p, sizeof p);      /* 0xFF doubled by send_sb */
}

/* current TTYPE cycle value */
static const char *
ttype_value(struct bd_telopt *t)
{
	switch (t->ttype_idx) {
	case 0:  return t->termtype;
	case 1:  return "XTERM-256COLOR";
	/* MTTS bitmask: ANSI|VT100|UTF-8|256-COLORS|TRUECOLOR */
	default: return "MTTS 271";
	}
}

static void
send_ttype(struct bd_telopt *t)
{
	const char *v = ttype_value(t);
	unsigned char p[1 + 64];
	size_t vl = strlen(v);
	if (vl > sizeof p - 1)
		vl = sizeof p - 1;
	p[0] = SB_IS;
	memcpy(p + 1, v, vl);
	send_sb(t, OPT_TTYPE, p, 1 + vl);
	if (t->ttype_idx < 2)           /* advance, clamp so the last repeats */
		t->ttype_idx++;
}

static void
append_env(unsigned char *p, size_t *n, size_t cap,
           unsigned char tag, const char *s)
{
	size_t l = strlen(s);
	if (*n + 1 + l > cap)
		return;
	p[(*n)++] = tag;
	memcpy(p + *n, s, l);
	*n += l;
}

static void
send_environ(struct bd_telopt *t)
{
	unsigned char p[128];
	size_t n = 0;
	p[n++] = SB_IS;
	append_env(p, &n, sizeof p, ENV_USERVAR, "CHARSET");
	append_env(p, &n, sizeof p, ENV_VALUE, "UTF-8");
	append_env(p, &n, sizeof p, ENV_USERVAR, "CLIENT");
	append_env(p, &n, sizeof p, ENV_VALUE, "birdie");
	send_sb(t, OPT_NEW_ENVIRON, p, n);
}

/* Case-insensitive search of a separated charset list for "UTF-8". The list
 * is sb[2..], separated by sb[1]. Returns 1 and fills *name if found. */
static int
charset_pick(const unsigned char *list, size_t len, char *name, size_t cap)
{
	unsigned char sep;
	size_t i, start;

	if (len < 2)
		return 0;
	sep = list[1];
	for (start = 2, i = 2; i <= len; i++) {
		if (i == len || list[i] == sep) {
			size_t l = i - start;
			if (l == 5 && strncasecmp((const char *)list + start,
			    "UTF-8", 5) == 0) {
				if (l >= cap)
					l = cap - 1;
				memcpy(name, list + start, l);
				name[l] = '\0';
				return 1;
			}
			start = i + 1;
		}
	}
	return 0;
}

/* Append one byte to the growable subnegotiation buffer, capped at
 * SB_CAP_MAX. On overflow the whole subnegotiation is dropped. */
static void
sb_append(struct bd_telopt *t, unsigned char b)
{
	if (t->sb_overflow)
		return;
	if (t->sb_len + 1 > t->sb_cap) {
		size_t nc = t->sb_cap ? t->sb_cap * 2 : 256;
		unsigned char *np;
		if (nc > SB_CAP_MAX)
			nc = SB_CAP_MAX;
		if (t->sb_len + 1 > nc) {
			t->sb_overflow = 1;
			return;
		}
		np = realloc(t->sb, nc);
		if (!np) {
			t->sb_overflow = 1;
			return;
		}
		t->sb = np;
		t->sb_cap = nc;
	}
	t->sb[t->sb_len++] = b;
}

/* ---- GMCP / MSDP -> JSON ---------------------------------------------- */

/* a small growable text buffer for building JSON */
struct jbuf { char *p; size_t len, cap; int oom; };

static void
jb_putc(struct jbuf *j, char c)
{
	if (j->oom)
		return;
	if (j->len + 1 > j->cap) {
		size_t nc = j->cap ? j->cap * 2 : 128;
		char *np = realloc(j->p, nc);
		if (!np) {
			j->oom = 1;
			return;
		}
		j->p = np;
		j->cap = nc;
	}
	j->p[j->len++] = c;
}

static void
jb_str(struct jbuf *j, const char *z)
{
	while (*z)
		jb_putc(j, *z++);
}

/* append bytes as a JSON string literal (quoted, escaped) */
static void
jb_jstr(struct jbuf *j, const unsigned char *b, size_t n)
{
	size_t i;
	jb_putc(j, '"');
	for (i = 0; i < n; i++) {
		unsigned char c = b[i];
		switch (c) {
		case '"':  jb_str(j, "\\\""); break;
		case '\\': jb_str(j, "\\\\"); break;
		case '\n': jb_str(j, "\\n"); break;
		case '\r': jb_str(j, "\\r"); break;
		case '\t': jb_str(j, "\\t"); break;
		default:
			if (c < 0x20) {
				char u[7];
				snprintf(u, sizeof u, "\\u%04x", c);
				jb_str(j, u);
			} else {
				jb_putc(j, (char)c);
			}
		}
	}
	jb_putc(j, '"');
}

static int
msdp_ctl(unsigned char b)
{
	return b >= MSDP_VAR && b <= MSDP_ARRAY_CLOSE;
}

/* Convert one MSDP value at *i to JSON, appended to j. Scalars become strings;
 * TABLE -> object, ARRAY -> array. depth guards malformed nesting. */
static void
msdp_value(const unsigned char *p, size_t len, size_t *i, struct jbuf *j,
           int depth)
{
	if (*i < len && p[*i] == MSDP_TABLE_OPEN) {
		int first = 1;
		(*i)++;
		jb_putc(j, '{');
		while (*i < len && p[*i] != MSDP_TABLE_CLOSE) {
			size_t s;
			if (p[*i] != MSDP_VAR)
				break;
			(*i)++;
			s = *i;
			while (*i < len && !msdp_ctl(p[*i]))
				(*i)++;
			if (!first)
				jb_putc(j, ',');
			first = 0;
			jb_jstr(j, p + s, *i - s);
			jb_putc(j, ':');
			if (*i < len && p[*i] == MSDP_VAL && depth < MSDP_DEPTH_MAX) {
				(*i)++;
				msdp_value(p, len, i, j, depth + 1);
			} else {
				jb_str(j, "null");
			}
		}
		if (*i < len && p[*i] == MSDP_TABLE_CLOSE)
			(*i)++;
		jb_putc(j, '}');
	} else if (*i < len && p[*i] == MSDP_ARRAY_OPEN) {
		int first = 1;
		(*i)++;
		jb_putc(j, '[');
		while (*i < len && p[*i] != MSDP_ARRAY_CLOSE) {
			if (p[*i] != MSDP_VAL)
				break;
			(*i)++;
			if (!first)
				jb_putc(j, ',');
			first = 0;
			if (depth < MSDP_DEPTH_MAX)
				msdp_value(p, len, i, j, depth + 1);
			else
				jb_str(j, "null");
		}
		if (*i < len && p[*i] == MSDP_ARRAY_CLOSE)
			(*i)++;
		jb_putc(j, ']');
	} else {
		size_t s = *i;
		while (*i < len && !msdp_ctl(p[*i]))
			(*i)++;
		jb_jstr(j, p + s, *i - s);
	}
}

/* Route each top-level MSDP variable as a JSON-valued package. */
static void
handle_msdp(struct bd_telopt *t, const unsigned char *p, size_t len)
{
	size_t i = 0;

	while (i < len) {
		char name[128];
		size_t s, nl;
		struct jbuf j = { 0 };

		if (p[i] != MSDP_VAR) {
			i++;
			continue;
		}
		i++;
		s = i;
		while (i < len && !msdp_ctl(p[i]))
			i++;
		nl = i - s;
		if (nl >= sizeof name)
			nl = sizeof name - 1;
		memcpy(name, p + s, nl);
		name[nl] = '\0';

		if (i < len && p[i] == MSDP_VAL) {
			i++;
			msdp_value(p, len, &i, &j, 0);
		} else {
			jb_str(&j, "null");
		}
		jb_putc(&j, '\0');
		if (!j.oom && t->cb.package)
			t->cb.package(BD_TELOPT_MSDP, name, j.p, t->cb.arg);
		free(j.p);
	}
}

/* GMCP payload is "Package.Name <json>"; split and route as-is. */
static void
handle_gmcp(struct bd_telopt *t, const unsigned char *p, size_t len)
{
	char name[128];
	size_t i = 0, nl, js;
	char *json;

	while (i < len && p[i] != ' ')
		i++;
	nl = i;
	if (nl >= sizeof name)
		nl = sizeof name - 1;
	memcpy(name, p, nl);
	name[nl] = '\0';

	js = (i < len) ? i + 1 : len;       /* skip the single separating space */
	json = malloc(len - js + 1);
	if (!json)
		return;
	memcpy(json, p + js, len - js);
	json[len - js] = '\0';
	if (t->cb.package)
		t->cb.package(BD_TELOPT_GMCP, name, json, t->cb.arg);
	free(json);
}

/* Dispatch a completed subnegotiation. sb_opt is the option; sb[0..sb_len-1]
 * is the payload (option byte already consumed, IAC-unescaped). */
static void
handle_subneg(struct bd_telopt *t)
{
	size_t plen = t->sb_len;

	switch (t->sb_opt) {
	case OPT_TTYPE:
		if (plen >= 1 && t->sb[0] == SB_SEND)
			send_ttype(t);
		break;
	case OPT_NEW_ENVIRON:
		if (plen >= 1 && t->sb[0] == SB_SEND)
			send_environ(t);
		break;
	case OPT_CHARSET:
		if (plen >= 1 && t->sb[0] == CS_REQUEST) {
			char name[32];
			if (charset_pick(t->sb, plen, name, sizeof name)) {
				unsigned char p[1 + 32];
				size_t l = strlen(name);
				p[0] = CS_ACCEPTED;
				memcpy(p + 1, name, l);
				send_sb(t, OPT_CHARSET, p, 1 + l);
				if (t->cb.charset)
					t->cb.charset(name, t->cb.arg);
			} else {
				unsigned char r = CS_REJECTED;
				send_sb(t, OPT_CHARSET, &r, 1);
			}
		}
		break;
	case OPT_GMCP:
		if (plen >= 1)
			handle_gmcp(t, t->sb, plen);
		break;
	case OPT_MSDP:
		if (plen >= 1)
			handle_msdp(t, t->sb, plen);
		break;
	case OPT_MCCP2:
		/* empty subnegotiation: the compressed stream starts now */
		if (t->cb.compress) {
			t->cb.compress(t->cb.arg);
			t->compress_now = 1;
		}
		break;
	default:
		break;
	}
}

/* Send a GMCP message ("Package.Name <json>"). */
static void
send_gmcp(struct bd_telopt *t, const char *msg)
{
	send_sb(t, OPT_GMCP, (const unsigned char *)msg, strlen(msg));
}

/* On GMCP enable, introduce ourselves and declare interest. */
static void
gmcp_hello(struct bd_telopt *t)
{
	send_gmcp(t, "Core.Hello {\"client\":\"birdie\",\"version\":\"0.0\"}");
	send_gmcp(t, "Core.Supports.Set [\"Char 1\",\"Char.Vitals 1\",\"Room 1\"]");
}

/* server says WILL <opt>: it offers to perform opt */
static void
on_will(struct bd_telopt *t, unsigned char opt)
{
	if (remote_wanted(opt)) {
		if (!t->them[opt]) {
			t->them[opt] = 1;
			send3(t, TC_DO, opt);
			if (opt == OPT_ECHO && t->cb.echo)
				t->cb.echo(1, t->cb.arg);
			if (opt == OPT_GMCP)
				gmcp_hello(t);
			if (opt == OPT_MXP && t->cb.mxp)
				t->cb.mxp(1, t->cb.arg);
		}
	} else {
		send3(t, TC_DONT, opt);
	}
}

/* server says WONT <opt>: it will not / no longer perform opt */
static void
on_wont(struct bd_telopt *t, unsigned char opt)
{
	if (t->them[opt]) {
		t->them[opt] = 0;
		send3(t, TC_DONT, opt);
		if (opt == OPT_ECHO && t->cb.echo)
			t->cb.echo(0, t->cb.arg);
		if (opt == OPT_MXP && t->cb.mxp)
			t->cb.mxp(0, t->cb.arg);
	}
}

/* server says DO <opt>: it asks us to perform opt */
static void
on_do(struct bd_telopt *t, unsigned char opt)
{
	if (local_supported(opt)) {
		if (!t->us[opt]) {
			t->us[opt] = 1;
			send3(t, TC_WILL, opt);
			if (opt == OPT_NAWS)
				send_naws(t);
		}
	} else {
		send3(t, TC_WONT, opt);
	}
}

/* server says DONT <opt>: it asks us to stop performing opt */
static void
on_dont(struct bd_telopt *t, unsigned char opt)
{
	if (t->us[opt]) {
		t->us[opt] = 0;
		send3(t, TC_WONT, opt);
	}
}

bd_telopt *
bd_telopt_new(const bd_telopt_cb *cb)
{
	bd_telopt *t = calloc(1, sizeof *t);
	if (!t)
		return NULL;
	if (cb)
		t->cb = *cb;
	t->st = S_DATA;
	t->cols = 80;
	t->rows = 24;
	strcpy(t->termtype, "birdie");
	return t;
}

void
bd_telopt_free(bd_telopt *t)
{
	if (!t)
		return;
	free(t->sb);
	free(t);
}

void
bd_telopt_reset(bd_telopt *t)
{
	if (!t)
		return;
	t->st = S_DATA;
	t->sb_len = 0;
	t->sb_got_opt = 0;
	t->sb_overflow = 0;
	t->compress_now = 0;
	t->ttype_idx = 0;
	memset(t->us, 0, sizeof t->us);
	memset(t->them, 0, sizeof t->them);
}

void
bd_telopt_set_termtype(bd_telopt *t, const char *type)
{
	if (!t || !type)
		return;
	snprintf(t->termtype, sizeof t->termtype, "%s", type);
}

void
bd_telopt_set_winsize(bd_telopt *t, int cols, int rows)
{
	if (!t || cols <= 0 || rows <= 0)
		return;
	if (cols == t->cols && rows == t->rows)
		return;
	t->cols = cols;
	t->rows = rows;
	if (t->us[OPT_NAWS])
		send_naws(t);
}

size_t
bd_telopt_recv(bd_telopt *t, const unsigned char *p, size_t len)
{
	unsigned char clean[4096];
	size_t cn = 0, i;

	for (i = 0; i < len; i++) {
		unsigned char b = p[i];
		switch (t->st) {
		case S_DATA:
			if (b == TC_IAC)
				t->st = S_IAC;
			else
				clean[cn++] = b;
			break;

		case S_IAC:
			if (b == TC_IAC) {
				clean[cn++] = TC_IAC;   /* literal 0xFF */
				t->st = S_DATA;
			} else if (b == TC_WILL || b == TC_WONT ||
			           b == TC_DO || b == TC_DONT) {
				t->verb = b;
				t->st = S_OPT;
			} else if (b == TC_SB) {
				t->sb_len = 0;
				t->sb_got_opt = 0;
				t->sb_overflow = 0;
				t->st = S_SB;
			} else {
				if ((b == TC_EOR || b == TC_GA) && t->cb.prompt) {
					/* flush pending text before the prompt mark */
					if (cn) {
						t->cb.data(clean, cn, t->cb.arg);
						cn = 0;
					}
					t->cb.prompt(t->cb.arg);
				}
				t->st = S_DATA;         /* NOP/DM/EOR/GA/... */
			}
			break;

		case S_OPT:
			switch (t->verb) {
			case TC_WILL: on_will(t, b); break;
			case TC_WONT: on_wont(t, b); break;
			case TC_DO:   on_do(t, b);   break;
			case TC_DONT: on_dont(t, b); break;
			}
			t->st = S_DATA;
			break;

		case S_SB:
			if (b == TC_IAC) {
				t->st = S_SB_IAC;
			} else if (!t->sb_got_opt) {
				t->sb_opt = b;          /* first SB byte is the option */
				t->sb_got_opt = 1;
			} else {
				sb_append(t, b);
			}
			break;

		case S_SB_IAC:
			if (b == TC_SE) {
				if (!t->sb_overflow)
					handle_subneg(t);
				t->st = S_DATA;
			} else if (b == TC_IAC) {
				sb_append(t, TC_IAC);   /* literal 0xFF in SB data */
				t->st = S_SB;
			} else {
				t->st = S_SB;           /* unexpected; resync */
			}
			break;
		}

		if (cn == sizeof clean) {
			if (t->cb.data)
				t->cb.data(clean, cn, t->cb.arg);
			cn = 0;
		}

		/* MCCP2 just started: hand the rest of the buffer back to the
		 * caller (it is compressed) after flushing decoded text. */
		if (t->compress_now) {
			t->compress_now = 0;
			if (cn && t->cb.data)
				t->cb.data(clean, cn, t->cb.arg);
			return i + 1;
		}
	}

	if (cn && t->cb.data)
		t->cb.data(clean, cn, t->cb.arg);
	return len;
}
