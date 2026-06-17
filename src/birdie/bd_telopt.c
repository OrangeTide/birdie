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

/* TTYPE / NEW-ENVIRON / CHARSET subnegotiation sub-commands */
#define SB_IS        0
#define SB_SEND      1
#define ENV_VAR      0
#define ENV_VALUE    1
#define ENV_USERVAR  3
#define CS_REQUEST   1
#define CS_ACCEPTED  2
#define CS_REJECTED  3

#define SB_MAX  512     /* largest subnegotiation payload we retain */

enum state { S_DATA, S_IAC, S_OPT, S_SB, S_SB_IAC };

struct bd_telopt {
	bd_telopt_cb cb;

	enum state st;
	unsigned char verb;             /* WILL/WONT/DO/DONT awaiting its option */
	unsigned char sb_opt;           /* option under subnegotiation */
	unsigned char sb[SB_MAX];
	size_t sb_len;

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
	       opt == OPT_ECHO || opt == OPT_SGA;
}

/* Build and send an escaped subnegotiation: IAC SB <opt> <payload> IAC SE,
 * doubling any 0xFF in the payload. */
static void
send_sb(struct bd_telopt *t, unsigned char opt,
        const unsigned char *payload, size_t len)
{
	unsigned char buf[SB_MAX + 16];
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

/* Dispatch a completed subnegotiation. sb_opt is the option; sb[0..plen-1]
 * is the payload (the bytes after the option, IAC-unescaped). */
static void
handle_subneg(struct bd_telopt *t)
{
	size_t plen = t->sb_len ? t->sb_len - 1 : 0;

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
			} else {
				unsigned char r = CS_REJECTED;
				send_sb(t, OPT_CHARSET, &r, 1);
			}
		}
		break;
	default:
		break;
	}
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
	free(t);
}

void
bd_telopt_reset(bd_telopt *t)
{
	if (!t)
		return;
	t->st = S_DATA;
	t->sb_len = 0;
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

void
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
			} else if (t->sb_len == 0) {
				t->sb_opt = b;          /* first SB byte is the option */
				t->sb_len = 1;          /* mark: option captured */
			} else if (t->sb_len - 1 < SB_MAX) {
				t->sb[t->sb_len - 1] = b;
				t->sb_len++;
			}
			break;

		case S_SB_IAC:
			if (b == TC_SE) {
				handle_subneg(t);
				t->st = S_DATA;
			} else if (b == TC_IAC) {
				if (t->sb_len && t->sb_len - 1 < SB_MAX) {
					t->sb[t->sb_len - 1] = TC_IAC;
					t->sb_len++;
				}
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
	}

	if (cn && t->cb.data)
		t->cb.data(clean, cn, t->cb.arg);
}
