/*
 * bd_mxp -- MXP tag parser. See bd_mxp.h and doc/terminal.md.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_mxp.h"
#include "bd_utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* line-security modes (collapsed: we only care whether tags are allowed) */
enum { MXP_OPEN, MXP_SECURE, MXP_LOCKED };

/* parser states */
enum { ST_TEXT, ST_ESC, ST_ESCSEQ, ST_TAG, ST_ENT };

#define TOK_MAX  4096
#define OUT_MAX  8192

struct bd_mxp {
	bd_mxp_cb cb;
	int state;
	int cur_mode;           /* active line mode */
	int def_mode;           /* default reverted to at newline / reset */
	int secure_once;        /* mode 4: revert to default after one tag */
	unsigned char tok[TOK_MAX];
	size_t toklen;
	char out[OUT_MAX];
	size_t outlen;
};

bd_mxp *
bd_mxp_new(const bd_mxp_cb *cb)
{
	bd_mxp *m = calloc(1, sizeof *m);
	if (!m)
		return NULL;
	if (cb)
		m->cb = *cb;
	m->cur_mode = MXP_OPEN;
	m->def_mode = MXP_OPEN;
	return m;
}

void
bd_mxp_free(bd_mxp *m)
{
	free(m);
}

void
bd_mxp_reset(bd_mxp *m)
{
	if (!m)
		return;
	m->state = ST_TEXT;
	m->toklen = 0;
	m->outlen = 0;
	m->cur_mode = MXP_OPEN;
	m->def_mode = MXP_OPEN;
	m->secure_once = 0;
}

/* ---- text output buffer ---- */

static void
out_flush(bd_mxp *m)
{
	if (m->outlen && m->cb.text)
		m->cb.text(m->out, m->outlen, m->cb.arg);
	m->outlen = 0;
}

static void
out_putc(bd_mxp *m, char c)
{
	if (m->outlen >= sizeof m->out)
		out_flush(m);
	m->out[m->outlen++] = c;
}

static void
out_put(bd_mxp *m, const unsigned char *p, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++)
		out_putc(m, (char)p[i]);
}

/* ---- token accumulation ---- */

static void
tok_putc(bd_mxp *m, unsigned char c)
{
	if (m->toklen < sizeof m->tok)
		m->tok[m->toklen++] = c;
}

static int
tag_allowed(const bd_mxp *m)
{
	return m->cur_mode != MXP_LOCKED;
}

/* ---- ESC[<n>z line-mode handling ---- */

static void
apply_mode(bd_mxp *m, int n)
{
	switch (n) {
	case 0: m->cur_mode = MXP_OPEN; break;          /* open (this line) */
	case 1: m->cur_mode = MXP_SECURE; break;        /* secure (this line) */
	case 2: m->cur_mode = MXP_LOCKED; break;        /* locked (this line) */
	case 3: m->cur_mode = m->def_mode; break;       /* reset to default */
	case 4: m->cur_mode = MXP_SECURE; m->secure_once = 1; break;
	case 5: m->def_mode = MXP_OPEN; m->cur_mode = MXP_OPEN; break;
	case 6: m->def_mode = MXP_SECURE; m->cur_mode = MXP_SECURE; break;
	case 7: m->def_mode = MXP_LOCKED; m->cur_mode = MXP_LOCKED; break;
	default: break;
	}
}

/* The accumulated tok is an ESC sequence "ESC [ ... final". If the final byte
 * is 'z' it is an MXP mode set (consumed); otherwise it is an ordinary ANSI
 * sequence that belongs in the display text. */
static void
finish_escseq(bd_mxp *m, unsigned char final)
{
	if (final == 'z') {
		int n = 0, seen = 0;
		size_t i;
		/* tok holds ESC '[' digits; parse the leading integer */
		for (i = 2; i < m->toklen; i++) {
			if (m->tok[i] >= '0' && m->tok[i] <= '9') {
				n = n * 10 + (m->tok[i] - '0');
				seen = 1;
			} else {
				break;
			}
		}
		apply_mode(m, seen ? n : 0);
	} else {
		out_put(m, m->tok, m->toklen);  /* pass ANSI through to display */
		out_putc(m, (char)final);
	}
	m->toklen = 0;
}

/* ---- entity decoding ---- */

/* tok holds the entity body without the leading '&' or trailing ';'. Decode a
 * known entity into the text, or emit it verbatim (with & and ;) if unknown. */
static void
finish_entity(bd_mxp *m)
{
	m->tok[m->toklen] = '\0';
	const char *e = (const char *)m->tok;
	if (!strcmp(e, "lt"))        out_putc(m, '<');
	else if (!strcmp(e, "gt"))   out_putc(m, '>');
	else if (!strcmp(e, "amp"))  out_putc(m, '&');
	else if (!strcmp(e, "quot")) out_putc(m, '"');
	else if (!strcmp(e, "apos")) out_putc(m, '\'');
	else if (!strcmp(e, "nbsp")) out_putc(m, ' ');
	else if (e[0] == '#') {
		long v;
		char *end;
		if (e[1] == 'x' || e[1] == 'X')
			v = strtol(e + 2, &end, 16);
		else
			v = strtol(e + 1, &end, 10);
		unsigned char u[BD_UTF8_MAX];
		int ul;
		if (*end == '\0' && v > 0 && (unsigned long)v <= BD_UTF8_RUNE_MAX &&
		    (ul = bd_utf8_encode(u, (uint32_t)v)) > 0) {
			out_put(m, u, (size_t)ul);   /* handles all planes */
		} else {                        /* malformed/invalid: verbatim */
			out_putc(m, '&');
			out_put(m, m->tok, m->toklen);
			out_putc(m, ';');
		}
	} else {                                /* unknown entity: verbatim */
		out_putc(m, '&');
		out_put(m, m->tok, m->toklen);
		out_putc(m, ';');
	}
	m->toklen = 0;
}

/* ---- tag dispatch ---- */

/* tok holds the tag body without the angle brackets. Split into name + attrs
 * and fire the callback. */
static void
finish_tag(bd_mxp *m)
{
	char name[128], attrs[1024];
	size_t i = 0, n = 0;
	int closing = 0;

	m->tok[m->toklen] = '\0';
	if (m->toklen > 0 && m->tok[0] == '/') {
		closing = 1;
		i = 1;
	}
	while (i < m->toklen && m->tok[i] != ' ' && m->tok[i] != '\t' &&
	    n + 1 < sizeof name)
		name[n++] = (char)m->tok[i++];
	name[n] = '\0';
	while (i < m->toklen && (m->tok[i] == ' ' || m->tok[i] == '\t'))
		i++;
	snprintf(attrs, sizeof attrs, "%s", (const char *)m->tok + i);

	if (name[0] && m->cb.tag)
		m->cb.tag(name, attrs, closing, m->cb.arg);

	if (m->secure_once) {                   /* mode 4: one tag, then revert */
		m->cur_mode = m->def_mode;
		m->secure_once = 0;
	}
	m->toklen = 0;
}

/* ---- feed ---- */

void
bd_mxp_feed(bd_mxp *m, const unsigned char *p, size_t len)
{
	size_t i;

	if (!m || !p)
		return;
	for (i = 0; i < len; i++) {
		unsigned char c = p[i];
		switch (m->state) {
		case ST_TEXT:
			if (c == 0x1b) {                /* ESC: maybe ESC[..z */
				m->toklen = 0;
				tok_putc(m, c);
				m->state = ST_ESC;
			} else if (c == '<' && tag_allowed(m)) {
				m->toklen = 0;
				m->state = ST_TAG;
			} else if (c == '&' && tag_allowed(m)) {
				m->toklen = 0;
				m->state = ST_ENT;
			} else {
				out_putc(m, (char)c);
				if (c == '\n') {        /* temp modes expire at newline */
					m->cur_mode = m->def_mode;
					m->secure_once = 0;
				}
			}
			break;
		case ST_ESC:
			if (c == '[') {
				tok_putc(m, c);
				m->state = ST_ESCSEQ;
			} else {                        /* not a CSI: emit ESC + byte */
				out_put(m, m->tok, m->toklen);
				out_putc(m, (char)c);
				m->toklen = 0;
				m->state = ST_TEXT;
			}
			break;
		case ST_ESCSEQ:
			if (c >= 0x40 && c <= 0x7e) {   /* final byte */
				finish_escseq(m, c);
				m->state = ST_TEXT;
			} else {
				tok_putc(m, c);
				if (m->toklen >= sizeof m->tok - 1) {   /* runaway */
					out_put(m, m->tok, m->toklen);
					m->toklen = 0;
					m->state = ST_TEXT;
				}
			}
			break;
		case ST_TAG:
			if (c == '>') {
				finish_tag(m);
				m->state = ST_TEXT;
			} else if (c == '\n' || m->toklen >= sizeof m->tok - 1) {
				/* unterminated tag: treat as literal text */
				out_putc(m, '<');
				out_put(m, m->tok, m->toklen);
				out_putc(m, (char)c);
				m->toklen = 0;
				m->state = ST_TEXT;
			} else {
				tok_putc(m, c);
			}
			break;
		case ST_ENT:
			if (c == ';') {
				finish_entity(m);
				m->state = ST_TEXT;
			} else if (c == '&' || c == ' ' || c == '\n' ||
			    m->toklen >= 31) {
				/* not a well-formed entity: emit '&' + buffer literally,
				 * reprocess this byte */
				out_putc(m, '&');
				out_put(m, m->tok, m->toklen);
				m->toklen = 0;
				m->state = ST_TEXT;
				i--;                    /* reprocess c in ST_TEXT */
			} else {
				tok_putc(m, c);
			}
			break;
		}
	}
	out_flush(m);
}
