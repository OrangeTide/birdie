/*
 * bd_log -- structured logging layer. See bd_log.h and doc/logging.md.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

/* ---- growable string buffer with snprintf-style overflow accounting ---- */

struct sbuf {
	char *buf;
	size_t cap;     /* capacity of buf (including the NUL slot) */
	size_t len;     /* chars that *would* be written (may exceed cap-1) */
};

static void
sb_putc(struct sbuf *s, char c)
{
	if (s->len + 1 < s->cap)
		s->buf[s->len] = c;
	s->len++;
}

static void
sb_puts(struct sbuf *s, const char *str)
{
	if (!str)
		return;
	while (*str)
		sb_putc(s, *str++);
}

/* Append `str` as a JSON string body (no surrounding quotes), escaping per
 * RFC 8259. Control chars below 0x20 become \uXXXX. */
static void
sb_putjson(struct sbuf *s, const char *str)
{
	if (!str)
		return;
	for (; *str; str++) {
		unsigned char c = (unsigned char)*str;
		switch (c) {
		case '"':  sb_puts(s, "\\\""); break;
		case '\\': sb_puts(s, "\\\\"); break;
		case '\b': sb_puts(s, "\\b"); break;
		case '\f': sb_puts(s, "\\f"); break;
		case '\n': sb_puts(s, "\\n"); break;
		case '\r': sb_puts(s, "\\r"); break;
		case '\t': sb_puts(s, "\\t"); break;
		default:
			if (c < 0x20) {
				char u[8];
				snprintf(u, sizeof u, "\\u%04x", c);
				sb_puts(s, u);
			} else {
				sb_putc(s, (char)c);
			}
		}
	}
}

/* Append a "key":"value" pair (quoted), value JSON-escaped. */
static void
sb_kv(struct sbuf *s, const char *key, const char *val)
{
	sb_putc(s, '"');
	sb_puts(s, key);
	sb_puts(s, "\":\"");
	sb_putjson(s, val);
	sb_putc(s, '"');
}

static void
sb_terminate(struct sbuf *s)
{
	if (s->cap)
		s->buf[s->len < s->cap ? s->len : s->cap - 1] = '\0';
}

/* ---- time ---- */

double
bd_log_now_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
		return 0.0;
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void
utc_parts(double t_ms, struct tm *tm, int *ms)
{
	time_t secs = (time_t)(t_ms / 1000.0);
	*ms = (int)(t_ms - (double)secs * 1000.0);
	if (*ms < 0)
		*ms = 0;
	gmtime_r(&secs, tm);
}

static void
iso8601(double t_ms, char *out, size_t cap)
{
	struct tm tm;
	int ms;
	utc_parts(t_ms, &tm, &ms);
	snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
	    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
	    tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

/* ---- formatters ---- */

static const char *
kind_name(bd_log_kind k)
{
	switch (k) {
	case BD_LOG_CONNECT:    return "connect";
	case BD_LOG_DISCONNECT: return "disconnect";
	case BD_LOG_RECV:       return "recv";
	case BD_LOG_SEND:       return "send";
	case BD_LOG_GMCP:       return "gmcp";
	case BD_LOG_MXP:        return "mxp";
	case BD_LOG_NOTE:       return "note";
	case BD_LOG_ERROR:      return "error";
	}
	return "?";
}

static void
fmt_ndjson(struct sbuf *s, const bd_log_rec *r)
{
	char ts[40];
	iso8601(r->t_ms, ts, sizeof ts);

	sb_putc(s, '{');
	sb_kv(s, "t", ts);
	sb_puts(s, ",\"v\":1,");
	sb_kv(s, "kind", kind_name(r->kind));
	if (r->mud)     { sb_putc(s, ','); sb_kv(s, "mud", r->mud); }
	if (r->session) { sb_putc(s, ','); sb_kv(s, "session", r->session); }

	switch (r->kind) {
	case BD_LOG_CONNECT: {
		char p[16];
		if (r->host) { sb_putc(s, ','); sb_kv(s, "host", r->host); }
		snprintf(p, sizeof p, "%d", r->port);
		sb_puts(s, ",\"port\":");
		sb_puts(s, p);
		sb_puts(s, ",\"tls\":");
		sb_puts(s, r->tls ? "true" : "false");
		if (r->remote_addr) {
			sb_putc(s, ',');
			sb_kv(s, "remote_addr", r->remote_addr);
		}
		break;
	}
	case BD_LOG_DISCONNECT:
		if (r->reason) { sb_putc(s, ','); sb_kv(s, "reason", r->reason); }
		break;
	case BD_LOG_RECV:
	case BD_LOG_SEND:
		if (r->raw)  { sb_putc(s, ','); sb_kv(s, "raw", r->raw); }
		if (r->text) { sb_putc(s, ','); sb_kv(s, "text", r->text); }
		break;
	case BD_LOG_GMCP:
		if (r->package) { sb_putc(s, ','); sb_kv(s, "package", r->package); }
		if (r->name)    { sb_putc(s, ','); sb_kv(s, "name", r->name); }
		/* data is already-parsed JSON: emit verbatim, not as a string */
		if (r->data) {
			sb_puts(s, ",\"data\":");
			sb_puts(s, r->data);
		}
		break;
	case BD_LOG_MXP:
		if (r->tag)   { sb_putc(s, ','); sb_kv(s, "tag", r->tag); }
		if (r->attrs) { sb_putc(s, ','); sb_kv(s, "attrs", r->attrs); }
		if (r->text)  { sb_putc(s, ','); sb_kv(s, "text", r->text); }
		break;
	case BD_LOG_NOTE:
		if (r->text) { sb_putc(s, ','); sb_kv(s, "text", r->text); }
		break;
	case BD_LOG_ERROR:
		if (r->where)   { sb_putc(s, ','); sb_kv(s, "where", r->where); }
		if (r->message) { sb_putc(s, ','); sb_kv(s, "message", r->message); }
		break;
	}
	sb_puts(s, "}\n");
}

/* plaintext: one human-readable line. `ansi` selects raw (with escapes) over
 * the stripped text for recv/send. */
static void
fmt_plaintext(struct sbuf *s, const bd_log_rec *r, int ansi)
{
	const char *body;

	switch (r->kind) {
	case BD_LOG_RECV:
		body = (ansi && r->raw) ? r->raw : r->text;
		sb_puts(s, body ? body : "");
		break;
	case BD_LOG_SEND:
		sb_puts(s, ">> ");
		body = (ansi && r->raw) ? r->raw : r->text;
		sb_puts(s, body ? body : "");
		break;
	case BD_LOG_CONNECT:
		sb_puts(s, "*** connected");
		if (r->host) { sb_puts(s, " to "); sb_puts(s, r->host); }
		break;
	case BD_LOG_DISCONNECT:
		sb_puts(s, "*** disconnected");
		if (r->reason) { sb_puts(s, ": "); sb_puts(s, r->reason); }
		break;
	case BD_LOG_GMCP:
		sb_puts(s, "[gmcp ");
		sb_puts(s, r->package ? r->package : "?");
		sb_puts(s, "] ");
		sb_puts(s, r->data ? r->data : "");
		break;
	case BD_LOG_MXP:
		sb_puts(s, "[mxp ");
		sb_puts(s, r->tag ? r->tag : "?");
		sb_puts(s, "] ");
		sb_puts(s, r->text ? r->text : "");
		break;
	case BD_LOG_NOTE:
		sb_puts(s, "-- ");
		sb_puts(s, r->text ? r->text : "");
		break;
	case BD_LOG_ERROR:
		sb_puts(s, "!! ");
		sb_puts(s, r->where ? r->where : "");
		sb_puts(s, ": ");
		sb_puts(s, r->message ? r->message : "");
		break;
	}
	sb_putc(s, '\n');
}

size_t
bd_log_format_rec(bd_log_format fmt, const bd_log_rec *rec, char *out,
                  size_t cap)
{
	struct sbuf s;
	s.buf = out;
	s.cap = cap;
	s.len = 0;
	switch (fmt) {
	case BD_LOGFMT_NDJSON:        fmt_ndjson(&s, rec); break;
	case BD_LOGFMT_PLAINTEXT:     fmt_plaintext(&s, rec, 0); break;
	case BD_LOGFMT_PLAINTEXT_ANSI:fmt_plaintext(&s, rec, 1); break;
	}
	sb_terminate(&s);
	return s.len;
}

/* ---- filters ---- */

static int
filter_accept(bd_log_filter f, bd_log_kind k)
{
	switch (f) {
	case BD_LOGF_ALL:     return 1;
	case BD_LOGF_RECV:    return k == BD_LOG_RECV;
	case BD_LOGF_SEND:    return k == BD_LOG_SEND;
	case BD_LOGF_TRAFFIC: return k == BD_LOG_RECV || k == BD_LOG_SEND;
	case BD_LOGF_GMCP:    return k == BD_LOG_GMCP;
	case BD_LOGF_MXP:     return k == BD_LOG_MXP;
	case BD_LOGF_NOTES:   return k == BD_LOG_NOTE;
	}
	return 0;
}

int
bd_log_filter_from_name(const char *name)
{
	if (!name) return -1;
	if (!strcmp(name, "all"))     return BD_LOGF_ALL;
	if (!strcmp(name, "recv"))    return BD_LOGF_RECV;
	if (!strcmp(name, "send"))    return BD_LOGF_SEND;
	if (!strcmp(name, "traffic")) return BD_LOGF_TRAFFIC;
	if (!strcmp(name, "gmcp"))    return BD_LOGF_GMCP;
	if (!strcmp(name, "mxp"))     return BD_LOGF_MXP;
	if (!strcmp(name, "notes"))   return BD_LOGF_NOTES;
	return -1;
}

int
bd_log_format_from_name(const char *name)
{
	if (!name) return -1;
	if (!strcmp(name, "ndjson"))         return BD_LOGFMT_NDJSON;
	if (!strcmp(name, "plaintext"))      return BD_LOGFMT_PLAINTEXT;
	if (!strcmp(name, "plaintext-ansi")) return BD_LOGFMT_PLAINTEXT_ANSI;
	return -1;
}

static const char *
fmt_ext(bd_log_format f)
{
	return f == BD_LOGFMT_NDJSON ? "ndjson" : "log";
}

/* ---- sinks ---- */

enum sink_dest { DEST_FILE, DEST_CB };

struct sink {
	bd_log_filter filter;
	bd_log_format fmt;
	enum sink_dest dest;

	/* file sink */
	char *root;
	char *templ;
	char *cur_path;         /* last expanded path (current hour bucket) */
	FILE *fp;

	/* callback sink */
	bd_log_emit_fn emit;
	void *ctx;
};

struct bd_log {
	struct sink *sinks;
	int n, cap;
};

bd_log *
bd_log_new(void)
{
	return calloc(1, sizeof(bd_log));
}

static void
sink_close(struct sink *sk)
{
	if (sk->fp) {
		fclose(sk->fp);
		sk->fp = NULL;
	}
}

void
bd_log_free(bd_log *l)
{
	int i;
	if (!l)
		return;
	for (i = 0; i < l->n; i++) {
		sink_close(&l->sinks[i]);
		free(l->sinks[i].root);
		free(l->sinks[i].templ);
		free(l->sinks[i].cur_path);
	}
	free(l->sinks);
	free(l);
}

static struct sink *
add_sink(bd_log *l, bd_log_filter filter, bd_log_format fmt,
         enum sink_dest dest)
{
	struct sink *sk;
	if (!l)
		return NULL;
	if (l->n == l->cap) {
		int nc = l->cap ? l->cap * 2 : 4;
		struct sink *ns = realloc(l->sinks, (size_t)nc * sizeof *ns);
		if (!ns)
			return NULL;
		l->sinks = ns;
		l->cap = nc;
	}
	sk = &l->sinks[l->n];
	memset(sk, 0, sizeof *sk);
	sk->filter = filter;
	sk->fmt = fmt;
	sk->dest = dest;
	l->n++;
	return sk;
}

int
bd_log_add_file(bd_log *l, bd_log_filter filter, bd_log_format fmt,
                const char *root, const char *path_template)
{
	struct sink *sk;
	if (!path_template)
		return -1;
	sk = add_sink(l, filter, fmt, DEST_FILE);
	if (!sk)
		return -1;
	sk->root = strdup(root ? root : "");
	sk->templ = strdup(path_template);
	if (!sk->root || !sk->templ) {
		free(sk->root);
		free(sk->templ);
		l->n--;
		return -1;
	}
	return 0;
}

int
bd_log_add_callback(bd_log *l, bd_log_filter filter, bd_log_format fmt,
                    bd_log_emit_fn emit, void *ctx)
{
	struct sink *sk;
	if (!emit)
		return -1;
	sk = add_sink(l, filter, fmt, DEST_CB);
	if (!sk)
		return -1;
	sk->emit = emit;
	sk->ctx = ctx;
	return 0;
}

/* Expand a path template into out[cap]. */
static void
expand_path(const struct sink *sk, const bd_log_rec *r, char *out, size_t cap)
{
	struct sbuf s;
	struct tm tm;
	int ms;
	const char *p = sk->templ;
	char num[16];

	utc_parts(r->t_ms, &tm, &ms);
	s.buf = out;
	s.cap = cap;
	s.len = 0;

	while (*p) {
		if (*p != '{') {
			sb_putc(&s, *p++);
			continue;
		}
		if (!strncmp(p, "{root}", 6)) { sb_puts(&s, sk->root); p += 6; }
		else if (!strncmp(p, "{year}", 6)) {
			snprintf(num, sizeof num, "%04d", tm.tm_year + 1900);
			sb_puts(&s, num); p += 6;
		} else if (!strncmp(p, "{month}", 7)) {
			snprintf(num, sizeof num, "%02d", tm.tm_mon + 1);
			sb_puts(&s, num); p += 7;
		} else if (!strncmp(p, "{day}", 5)) {
			snprintf(num, sizeof num, "%02d", tm.tm_mday);
			sb_puts(&s, num); p += 5;
		} else if (!strncmp(p, "{hour}", 6)) {
			snprintf(num, sizeof num, "%02d", tm.tm_hour);
			sb_puts(&s, num); p += 6;
		} else if (!strncmp(p, "{mud}", 5)) {
			sb_puts(&s, r->mud ? r->mud : "_"); p += 5;
		} else if (!strncmp(p, "{profile}", 9)) {
			sb_puts(&s, r->mud ? r->mud : "_"); p += 9;
		} else if (!strncmp(p, "{character}", 11)) {
			sb_puts(&s, (r->character && *r->character) ?
			    r->character : "_"); p += 11;
		} else if (!strncmp(p, "{ext}", 5)) {
			sb_puts(&s, fmt_ext(sk->fmt)); p += 5;
		} else {
			sb_putc(&s, *p++);      /* unknown token: copy '{' */
		}
	}
	sb_terminate(&s);
}

/* mkdir -p for the directory part of `path`. */
static void
mkdirs(const char *path)
{
	char buf[1024];
	size_t i, n;

	n = strlen(path);
	if (n >= sizeof buf)
		return;
	memcpy(buf, path, n + 1);
	for (i = 1; i < n; i++) {
		if (buf[i] != '/')
			continue;
		buf[i] = '\0';
		if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
			buf[i] = '/';
			return;
		}
		buf[i] = '/';
	}
}

/* Ensure sk->fp points at the current hour bucket for `r`. On a fresh open of
 * a plaintext bucket, write the hourly header. */
static void
file_open_bucket(struct sink *sk, const bd_log_rec *r)
{
	char path[1024];
	int fresh;

	expand_path(sk, r, path, sizeof path);
	if (sk->fp && sk->cur_path && !strcmp(sk->cur_path, path))
		return;                         /* same bucket, still open */
	sink_close(sk);
	free(sk->cur_path);
	sk->cur_path = strdup(path);
	mkdirs(path);
	/* "fresh" if the file does not yet exist (so we write a header once) */
	fresh = access(path, F_OK) != 0;
	sk->fp = fopen(path, "a");
	if (sk->fp && fresh &&
	    (sk->fmt == BD_LOGFMT_PLAINTEXT ||
	     sk->fmt == BD_LOGFMT_PLAINTEXT_ANSI)) {
		struct tm tm;
		int ms;
		utc_parts(r->t_ms, &tm, &ms);
		fprintf(sk->fp,
		    "=== %04d-%02d-%02d %02d:00 UTC \xe2\x80\x94 %s (session %s) ===\n",
		    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
		    r->mud ? r->mud : "?", r->session ? r->session : "?");
	}
}

void
bd_log_write(bd_log *l, const bd_log_rec *rec)
{
	char stackbuf[4096];
	int i;

	if (!l || !rec)
		return;
	for (i = 0; i < l->n; i++) {
		struct sink *sk = &l->sinks[i];
		char *buf = stackbuf;
		size_t need;

		if (!filter_accept(sk->filter, rec->kind))
			continue;
		need = bd_log_format_rec(sk->fmt, rec, stackbuf, sizeof stackbuf);
		if (need + 1 > sizeof stackbuf) {       /* truncated: reformat */
			buf = malloc(need + 1);
			if (!buf)
				continue;
			bd_log_format_rec(sk->fmt, rec, buf, need + 1);
		}
		if (sk->dest == DEST_CB) {
			sk->emit(buf, need, sk->ctx);
		} else {
			file_open_bucket(sk, rec);
			if (sk->fp) {
				fwrite(buf, 1, need, sk->fp);
				fflush(sk->fp);
			}
		}
		if (buf != stackbuf)
			free(buf);
	}
}
