#ifndef BD_NET_H
#define BD_NET_H

/*
 * bd_net -- birdie's network connection layer.
 *
 * A single TCP/telnet connection to a MUD. The socket lives on a dedicated
 * network thread (a libiox poll loop); resolution, connect, recv/send, and
 * telnet IAC filtering all happen there so the UI thread never blocks on the
 * network. Decoded bytes and state transitions cross back over a lock-free
 * ring and are delivered through the callbacks below, on the UI thread,
 * during bd_net_poll(). The embedded telnet filter refuses every option and
 * strips IAC negotiation, so callers see clean text.
 *
 * Deferred (see doc/network.md): TLS (mbedTLS), the MTH telopt set
 * (CHARSET/ECHO/EOR/GMCP/MCCP/MSDP/...), Happy Eyeballs, reconnect, and
 * encoding transcode. The callback-driven API here is meant to stay stable
 * when those land behind it.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

typedef enum bd_net_state {
	BD_NET_IDLE,            /* no connection */
	BD_NET_CONNECTING,      /* socket opened, awaiting connect completion */
	BD_NET_CONNECTED,       /* stream is live */
	BD_NET_CLOSED,          /* peer or local close, no error */
	BD_NET_ERROR            /* failed to resolve/connect, or I/O error */
} bd_net_state;

/* Clean application bytes received from the peer (telnet markup removed). */
typedef void (*bd_net_data_cb)(const char *data, int len, void *arg);

/* Connection state changed. msg is a short human-readable detail (may be NULL),
 * e.g. an error string. */
typedef void (*bd_net_state_cb)(bd_net_state state, const char *msg, void *arg);

/* Server-controlled echo changed (telnet ECHO). suppress != 0 means the client
 * should stop echoing typed input, e.g. mask a password. */
typedef void (*bd_net_echo_cb)(int suppress, void *arg);

/* A prompt boundary was received (telnet EOR / GA). */
typedef void (*bd_net_prompt_cb)(void *arg);

/* An out-of-band package arrived. proto is BD_TELOPT_GMCP / BD_TELOPT_MSDP
 * (see bd_telopt.h); name is the package/variable name; json is its payload
 * as JSON. Strings are valid only for the call. */
typedef void (*bd_net_package_cb)(int proto, const char *name,
                                  const char *json, void *arg);

/* MXP (telnet option 91) became active (1) or off (0); the consumer should run
 * the data stream through an MXP parser while active (see bd_mxp). */
typedef void (*bd_net_mxp_cb)(int active, void *arg);

typedef struct bd_net bd_net;

/* Create a connection object. Callbacks may be NULL. arg is passed back to
 * both callbacks. Returns NULL on allocation failure. */
bd_net *bd_net_new(bd_net_data_cb on_data, bd_net_state_cb on_state, void *arg);

/* Tear down: closes any open socket and frees the object. */
void bd_net_free(bd_net *n);

/* Begin connecting to host:port (both as strings; port may be a service name).
 * If tls != 0, run the telnet stream over TLS (certificate verified against
 * the trust store; SNI/verification use host). Returns 0 if the attempt
 * started (state becomes BD_NET_CONNECTING), -1 if it could not even begin.
 * Resolution and handshake failures surface later via the state callback.
 * A connection already in progress is closed first. */
int bd_net_connect(bd_net *n, const char *host, const char *port, int tls);

/* Drive the connection: complete a pending connect, flush queued output, and
 * read available input (delivered through the data callback). Call once per
 * frame. Cheap and non-blocking when idle. */
void bd_net_poll(bd_net *n);

/* Queue len bytes for sending (telnet-escaping any 0xFF). Returns the number of
 * bytes accepted, or -1 if not connected. Actual transmission happens here and
 * in bd_net_poll(). */
int bd_net_send(bd_net *n, const void *data, int len);

/* Close the connection (user-initiated). Safe to call when already closed.
 * Cancels any pending auto-reconnect. */
void bd_net_close(bd_net *n);

/* Enable/disable automatic reconnect on unexpected disconnect (default on).
 * On an unexpected drop or failed connect, bd_net redials with exponential
 * backoff (1s doubling up to 60s); a successful connect resets the backoff,
 * and a user-initiated close or connect cancels it. */
void bd_net_set_autoreconnect(bd_net *n, int enable);

/* Optional callbacks for telnet-driven events, delivered on the UI thread
 * during bd_net_poll() like the others. Set before connecting. */
void bd_net_set_echo_cb(bd_net *n, bd_net_echo_cb cb);
void bd_net_set_prompt_cb(bd_net *n, bd_net_prompt_cb cb);
void bd_net_set_package_cb(bd_net *n, bd_net_package_cb cb);
void bd_net_set_mxp_cb(bd_net *n, bd_net_mxp_cb cb);

/* Advertise the terminal type (TTYPE/MTTS), e.g. "birdie/0.0". Copied.
 * Takes effect on the next negotiation; set before connecting. */
void bd_net_set_termtype(bd_net *n, const char *type);

/* Report the terminal window size in character cells (NAWS). Resent to the
 * server if it changes while connected. */
void bd_net_set_winsize(bd_net *n, int cols, int rows);

/* Send an outbound GMCP package "pkg <json>" (json may be NULL for a bare
 * package). Framed and escaped on the net thread. Returns 0 if queued, -1 if
 * it could not be (too large, or not enough tx room). */
int bd_net_send_gmcp(bd_net *n, const char *pkg, const char *json);

/* Current state. */
bd_net_state bd_net_state_get(const bd_net *n);

#endif /* BD_NET_H */
