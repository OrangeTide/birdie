#ifndef BD_LOG_H
#define BD_LOG_H

#include <stddef.h>

/*
 * bd_log -- the structured logging layer (doc/logging.md).
 *
 * A bd_log holds any number of sinks. Each sink is a (filter, formatter,
 * destination) triple: a filter predicate decides which records it cares
 * about, a formatter renders a record to bytes, and the destination is either
 * an hour-bucketed file (path template) or a caller callback (used by tests
 * and front-ends).
 *
 * NDJSON is the source of truth (one JSON object per line); plaintext and
 * plaintext-ansi are human-readable derivations. Records carry their own
 * wall-clock time so the layer stays deterministic under test; pass
 * bd_log_now_ms() in production.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

typedef enum bd_log_kind {
	BD_LOG_CONNECT,         /* {host, port, tls, remote_addr} */
	BD_LOG_DISCONNECT,      /* {reason} */
	BD_LOG_RECV,            /* an inbound line: {raw, text} */
	BD_LOG_SEND,            /* an outbound command: {raw, text} */
	BD_LOG_GMCP,            /* {package, name, data} (data is JSON) */
	BD_LOG_MXP,             /* {tag, attrs, text} */
	BD_LOG_NOTE,            /* user/script annotation: {text} */
	BD_LOG_ERROR            /* {where, message} */
} bd_log_kind;

/* A single record. Only the fields relevant to `kind` need be set; the rest
 * may be NULL/0. Strings are borrowed for the duration of bd_log_write. */
typedef struct bd_log_rec {
	bd_log_kind kind;
	double      t_ms;       /* wall-clock ms since the Unix epoch */
	const char *mud;        /* MUD profile name */
	const char *session;    /* session id, stable for one connection */
	const char *character;  /* character/channel (for the path template) */

	/* recv / send / note */
	const char *raw;        /* original bytes, ANSI intact (recv/send) */
	const char *text;       /* ANSI-stripped text; the note body for NOTE */
	int         suppressed; /* recv: 1 if gagged from display (still logged) */

	/* connect */
	const char *host;
	int         port;
	int         tls;
	const char *remote_addr;

	/* disconnect */
	const char *reason;

	/* gmcp / mxp */
	const char *package;    /* gmcp package, e.g. "Char.Vitals" */
	const char *name;       /* gmcp sub-name, e.g. "update" */
	const char *data;       /* gmcp payload, already-parsed JSON text */
	const char *tag;        /* mxp tag */
	const char *attrs;      /* mxp attrs */

	/* error */
	const char *where;
	const char *message;
} bd_log_rec;

typedef enum bd_log_filter {
	BD_LOGF_ALL,
	BD_LOGF_RECV,
	BD_LOGF_SEND,
	BD_LOGF_TRAFFIC,        /* recv + send */
	BD_LOGF_GMCP,
	BD_LOGF_MXP,
	BD_LOGF_NOTES
} bd_log_filter;

typedef enum bd_log_format {
	BD_LOGFMT_NDJSON,
	BD_LOGFMT_PLAINTEXT,    /* ANSI-stripped text */
	BD_LOGFMT_PLAINTEXT_ANSI/* raw bytes with ANSI intact */
} bd_log_format;

typedef struct bd_log bd_log;

/* Current wall-clock time in ms since the Unix epoch (CLOCK_REALTIME). */
double bd_log_now_ms(void);

bd_log *bd_log_new(void);
void    bd_log_free(bd_log *l);

/*
 * Add a file sink. `path_template` may contain the tokens {root} {year}
 * {month} {day} {hour} {mud} {character} {profile} {ext}; a new hour-bucket
 * file is opened (append) as records cross hour boundaries. `root` substitutes
 * {root}; pass "" if the template embeds an absolute path. Returns 0 on
 * success, -1 on error.
 */
int bd_log_add_file(bd_log *l, bd_log_filter filter, bd_log_format fmt,
                    const char *root, const char *path_template);

/* Add a callback sink (tests, front-ends): `emit` gets each formatted record
 * (NUL-terminated, `len` excludes the NUL). Returns 0 on success, -1. */
typedef void (*bd_log_emit_fn)(const char *formatted, size_t len, void *ctx);
int bd_log_add_callback(bd_log *l, bd_log_filter filter, bd_log_format fmt,
                        bd_log_emit_fn emit, void *ctx);

/* Format `rec` to bytes and hand it to every sink whose filter accepts it. */
void bd_log_write(bd_log *l, const bd_log_rec *rec);

/* Format a record into `out` (NUL-terminated) the way `fmt` would; returns the
 * length written (excluding the NUL), or the length it would have written if
 * that exceeds `cap` (truncated, like snprintf). Exposed for tests. */
size_t bd_log_format_rec(bd_log_format fmt, const bd_log_rec *rec,
                         char *out, size_t cap);

/* Map a filter/format name to its enum. Returns -1 on an unknown name. */
int bd_log_filter_from_name(const char *name);
int bd_log_format_from_name(const char *name);

#endif /* BD_LOG_H */
