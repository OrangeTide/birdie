# MCP 2.1 and the simpleedit package

## What MCP is

MCP (MUD Client Protocol) is an out-of-band message layer carried **in band**,
as ordinary text lines that begin with a fixed prefix. It is what LambdaMOO and
its descendants use for structured client/server messages: editors, file
transfer, GUIs. Unlike GMCP/MSDP (telnet subnegotiation, handled by
`bd_telopt`), MCP rides the normal line stream, so it is intercepted in the
session's line pipeline rather than the telnet layer.

Birdie implements MCP 2.1 with two built-in packages: `mcp-negotiate` (the
negotiation package every MCP session needs) and `dns-org-mud-moo-simpleedit`
(the remote-edit package: the server hands the client text to edit and the
client sends the edited text back).

This is distinct from the *Model Context Protocol* MCP used by ludica's
agent-introspection layer (`doc/ludica-mcp-setup.md`). Same acronym, unrelated.

## The wire format

Every MCP line begins with one of two three-character markers:

- `#$#` -- an MCP **message**.
- `#$"` -- a **quoted** ordinary line: the client strips the marker and treats
  the remainder as normal text. Servers quote any real output line that would
  otherwise start with `#$#` or `#$"`, so ordinary text can never be mistaken
  for a message.

A message line is:

```
#$#<message-name> <auth-key> <key>: <value> <key>: <value> ...
```

- `<message-name>` follows the marker with no space (`#$#mcp-negotiate-can`).
- `<auth-key>` is present on every message **except** the very first `mcp`
  message (below). It is a shared secret so that other players' text, which
  passes through the same stream, cannot inject messages.
- Keys carry a trailing colon (`version:`); values are either an unquoted
  simple value (no spaces or specials) or a `"..."` string with `\"` and `\\`
  escapes. Message names and keys are case-insensitive (lowercased on receipt).

### Multiline values

A key written `name*:` marks a **multiline** value; its inline value is ignored
and the data arrives on later lines keyed by a data tag:

```
#$#some-message <auth> content*: "" _data-tag: 7f2a
#$#* 7f2a content: first line of the value
#$#* 7f2a content: second line
#$#: 7f2a
```

`#$#* <tag> <key>: <text>` appends one line to multiline value `<key>` of the
in-progress message with `_data-tag == <tag>`. `#$#: <tag>` closes the tag; the
message is complete and is dispatched then, not when its opening line arrived.

## Handshake and negotiation

1. On connect the server sends, with no auth key:
   `#$#mcp version: 2.1 to: 2.1` (a low/high supported range).
2. The client picks a random `auth-key`, remembers it, and replies:
   `#$#mcp authentication-key: <key> version: 2.1 to: 2.1`.
3. The client announces each package it supports and ends the list:
   ```
   #$#mcp-negotiate-can <key> package: mcp-negotiate min-version: 1.0 max-version: 2.0
   #$#mcp-negotiate-can <key> package: dns-org-mud-moo-simpleedit min-version: 1.0 max-version: 1.0
   #$#mcp-negotiate-end <key>
   ```
4. The server sends its own `mcp-negotiate-can` / `-end`; the client records
   which packages (and versions) the server supports.

From then on every server message carries the auth key as its first token; the
client drops any `#$#` message whose key does not match (an injection attempt or
a stray line), while still displaying `#$"`-quoted text.

## simpleedit

Package `dns-org-mud-moo-simpleedit`, version 1.0. The server sends content to
edit; the client edits it and sends it back.

Server to client:

```
#$#dns-org-mud-moo-simpleedit-content <key> reference: "#123:look" \
    name: "look message" type: "string-list" content*: "" _data-tag: <tag>
#$#* <tag> content: You see nothing special.
#$#: <tag>
```

- `reference` opaquely identifies what is being edited; the client echoes it back
  unchanged.
- `type` is `string` (one line), `string-list` (many lines), or `moo-code`.
- `content` is the multiline body.

Client to server on save:

```
#$#dns-org-mud-moo-simpleedit-set <key> reference: "#123:look" type: "string-list" \
    content*: "" _data-tag: <tag>
#$#* <tag> content: <edited line 1>
#$#* <tag> content: <edited line 2>
#$#: <tag>
```

The client generates a fresh data tag for the reply.

## Architecture in birdie

Three layers, matching the GMCP precedent (parser -> session -> app):

### `bd_mcp` (this note's core; `src/birdie/bd_mcp.{c,h}`)

A self-contained state machine, no session or UI dependency, driven by
callbacks. It owns the auth key, the negotiated-package table, and the set of
in-progress multiline messages.

```c
typedef struct bd_mcp_cb {
    void (*send)(const char *line, void *ctx);        /* transmit one MCP line (no CRLF) */
    void (*edit)(const bd_mcp_edit *req, void *ctx);   /* a simpleedit-content arrived */
    void *ctx;
} bd_mcp_cb;

bd_mcp *bd_mcp_new(const bd_mcp_cb *cb);
void    bd_mcp_free(bd_mcp *m);

/* Feed one received line (terminator stripped). Returns:
 *   BD_MCP_CONSUMED  an MCP control line; do not display or run triggers.
 *   BD_MCP_TEXT      an ordinary line to display; *out/*out_len give the text
 *                    (a `#$"` line with the marker stripped, or the line as-is).
 * The session displays/triggers only on BD_MCP_TEXT. */
int bd_mcp_feed(bd_mcp *m, const char *line, int len,
                const char **out, int *out_len);

/* Send edited content back (simpleedit-set) for a prior edit request. */
void bd_mcp_edit_done(bd_mcp *m, const char *reference, const char *type,
                      const char *text);

void bd_mcp_reset(bd_mcp *m);   /* on (re)connect: clear key + state */
```

`bd_mcp_edit` carries `reference`, `name`, `type`, and the assembled `content`
(a single `\n`-joined string) plus its length. Strings are borrowed for the
duration of the `edit` callback.

Line classification is by the first three bytes only, so the session can decide
without a full parse: `#$#` message, `#$"` quote, else ordinary.

### Session wiring (`bd_session`)

`feed_lines` already streams line bytes to the display live (so prompts appear
before their newline). To hide MCP lines it must defer that live stream for any
line whose start could be a marker (`#`, `#$`, `#$#`, `#$"`) until the newline
classifies it. Concretely: track how much of the current line has been streamed;
withhold streaming while the buffered prefix is a prefix of a marker; at the
newline call `bd_mcp_feed` and act on the result (`CONSUMED` -> drop; `TEXT` ->
stream the returned text and retire it through triggers as usual). Lines that
are plainly not markers (any second byte other than `$`) stream live exactly as
today, so normal output and prompts are unaffected.

The session provides `bd_mcp`'s `send` callback (via `bd_session_send_raw` plus
CRLF) and, on a simpleedit-content, emits a new event
`BD_SESSION_MCP_EDIT` carrying reference/name/type/content. A new
`bd_session_mcp_edit_done(s, reference, type, text)` forwards the save to
`bd_mcp_edit_done`.

### App wiring (`src/birdie/main.c`)

On `BD_SESSION_MCP_EDIT` the app opens the existing script editor (`bd_editor`)
with the content, titled by `name`; `moo-code` gets Lua-ish syntax highlighting
from `bd_syntax` (close enough to read MOO verbs). Save calls
`bd_session_mcp_edit_done`, which sends `simpleedit-set` back.

## Scope and non-goals

In scope: MCP 2.1 core (handshake, auth key, quoting, multiline),
`mcp-negotiate`, and `dns-org-mud-moo-simpleedit`. Out of scope for now: cord
support (`mcp-cord`), `dns-org-mud-moo-simpleedit` file references beyond text,
and other packages (they can register later against the same core). MCP is
always-on at the line level (the client always watches for `#$#`); it does
nothing until a server actually opens an MCP session, so non-MOO MUDs are
unaffected.

<!-- Made by a machine. PUBLIC DOMAIN (CC0-1.0) -->
