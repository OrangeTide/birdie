#ifndef BD_MCP_H
#define BD_MCP_H

/*
 * bd_mcp -- MUD Client Protocol 2.1, the in-band out-of-band message layer used
 * by LambdaMOO-family servers. Self-contained and UI-agnostic: it is fed the
 * received line stream, runs the handshake and package negotiation, and talks
 * back through a send callback. Built-in packages: mcp-negotiate and
 * dns-org-mud-moo-simpleedit (remote text editing). See doc/mcp.md.
 *
 * Not to be confused with ludica's Model Context Protocol layer (same acronym).
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* bd_mcp_feed result. */
enum {
	BD_MCP_TEXT     = 0,   /* an ordinary line; display + trigger *out[0..out_len) */
	BD_MCP_CONSUMED = 1,   /* an MCP control line; do not display or trigger */
};

typedef struct bd_mcp bd_mcp;

/* A simpleedit-content request handed to the app. All strings are borrowed for
 * the callback's duration (copy what you keep). content is the \n-joined body. */
typedef struct bd_mcp_edit {
	const char *reference;   /* opaque id, echoed back unchanged on save */
	const char *name;        /* human label for the editor title */
	const char *type;        /* "string" | "string-list" | "moo-code" */
	const char *content;
	int         content_len;
} bd_mcp_edit;

typedef struct bd_mcp_cb {
	void (*send)(const char *line, void *ctx);       /* transmit one line (no CRLF) */
	void (*edit)(const bd_mcp_edit *req, void *ctx); /* a simpleedit arrived */
	void  *ctx;
} bd_mcp_cb;

bd_mcp *bd_mcp_new(const bd_mcp_cb *cb);
void    bd_mcp_free(bd_mcp *m);

/* Feed one received line (terminator already stripped, not NUL-terminated).
 * Returns BD_MCP_CONSUMED for MCP control lines, or BD_MCP_TEXT for ordinary
 * lines -- then *out / *out_len give the text to display (the line as-is, or a
 * `#$"`-quoted line with its marker removed). */
int bd_mcp_feed(bd_mcp *m, const char *line, int len,
                const char **out, int *out_len);

/* Send edited content back to the server (simpleedit-set) for an earlier edit
 * request. `text` is the whole body; lines are split on '\n'. No-op if MCP is
 * not active. */
void bd_mcp_edit_done(bd_mcp *m, const char *reference, const char *type,
                      const char *text);

/* Clear all state (call on connect / disconnect). */
void bd_mcp_reset(bd_mcp *m);

/* 1 once the mcp handshake has completed. */
int bd_mcp_active(const bd_mcp *m);

/* The session's authentication key (for tests / diagnostics); "" until the
 * handshake runs. */
const char *bd_mcp_authkey(const bd_mcp *m);

#endif
