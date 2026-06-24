#ifndef BD_MXP_H
#define BD_MXP_H

#include <stddef.h>

/*
 * bd_mxp -- the MUD eXtension Protocol (MXP) tag parser (doc/terminal.md).
 *
 * A socket-free, incremental parser that splits an MXP application byte stream
 * (the clean text bd_telopt emits once option 91 / MXP is negotiated) into two
 * outputs: the plain display text, with entities decoded and tag markup
 * removed, and a sequence of MXP tag events (name + attributes, open or close).
 * It tracks the MXP line-security mode set by ESC[<n>z sequences and only
 * recognizes tags when the mode permits them; in a locked line, '<' and '&'
 * are literal text.
 *
 * This is the parsing seam only. Visual rendering of MXP formatting (colors,
 * <send> links) is the terminal widget's job; bd_session routes the tag events
 * to the on.mxp hook table, the `mxp` trigger type, and the log sinks.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

typedef struct bd_mxp_cb {
	/* Cleaned display text (tags removed, entities decoded). */
	void (*text)(const char *bytes, size_t len, void *arg);
	/* An MXP tag. `name` is upper/lower as sent; `attrs` is everything after
	 * the name (may be ""); `closing` != 0 for a </name> tag. Both strings are
	 * NUL-terminated and valid only for the call. */
	void (*tag)(const char *name, const char *attrs, int closing, void *arg);
	void *arg;
} bd_mxp_cb;

typedef struct bd_mxp bd_mxp;

bd_mxp *bd_mxp_new(const bd_mxp_cb *cb);
void    bd_mxp_free(bd_mxp *m);

/* Drop any pending partial token and reset the line mode to the default
 * (open). Call on (re)connect or when MXP is renegotiated. */
void    bd_mxp_reset(bd_mxp *m);

/* Feed inbound application bytes. Text and tag callbacks fire as parsed; a
 * token split across feeds is buffered until completed. */
void    bd_mxp_feed(bd_mxp *m, const unsigned char *p, size_t len);

#endif /* BD_MXP_H */
