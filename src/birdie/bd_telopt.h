#ifndef BD_TELOPT_H
#define BD_TELOPT_H

#include <stddef.h>

/*
 * bd_telopt -- birdie's telnet option layer.
 *
 * A socket-free state machine that parses an inbound telnet byte stream:
 * it strips IAC negotiation, answers the options birdie cares about, and
 * surfaces clean application text plus a few semantic events. It owns no
 * buffers beyond its own and never touches the network; bd_net feeds it
 * received bytes and ships whatever it asks to transmit.
 *
 * Options handled as a client:
 *   TTYPE / MTTS   advertise terminal type and capability flags
 *   NAWS           report window size (resent on resize)
 *   NEW_ENVIRON    minimal env (CHARSET, CLIENT)
 *   CHARSET        prefer UTF-8
 *   SGA            suppress go-ahead (accepted both ways)
 *   ECHO           server-controlled echo (drives password masking)
 *   EOR / GA       prompt-boundary marking
 * Every other option is refused (DO->WONT, WILL->DONT).
 *
 * Deferred (doc/network.md): GMCP / MSDP / MSSP / MCCP, which the design
 * earmarks for vendored MTH rather than this native core.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

typedef struct bd_telopt_cb {
	/* Clean application bytes (escape sequences and text), telnet removed. */
	void (*data)(const unsigned char *p, size_t len, void *arg);
	/* Bytes to transmit to the server (negotiation + subnegotiation). */
	void (*xmit)(const unsigned char *p, size_t len, void *arg);
	/* A prompt boundary arrived (IAC EOR or IAC GA). May be NULL. */
	void (*prompt)(void *arg);
	/* Server echo control changed. suppress_local != 0 means the client
	 * should stop echoing typed input (password entry). May be NULL. */
	void (*echo)(int suppress_local, void *arg);
	void *arg;
} bd_telopt_cb;

typedef struct bd_telopt bd_telopt;

/* Create with the given callbacks (copied). data/xmit should be non-NULL for
 * useful behavior. Returns NULL on allocation failure. */
bd_telopt *bd_telopt_new(const bd_telopt_cb *cb);
void bd_telopt_free(bd_telopt *t);

/* Forget all negotiation state for a fresh connection. */
void bd_telopt_reset(bd_telopt *t);

/* Feed raw inbound bytes from the socket. */
void bd_telopt_recv(bd_telopt *t, const unsigned char *p, size_t len);

/* Terminal type advertised via TTYPE (e.g. "birdie/0.0"); copied. */
void bd_telopt_set_termtype(bd_telopt *t, const char *type);

/* Window size in character cells. If NAWS is active and the size changed,
 * a fresh NAWS subnegotiation is transmitted immediately. */
void bd_telopt_set_winsize(bd_telopt *t, int cols, int rows);

#endif /* BD_TELOPT_H */
