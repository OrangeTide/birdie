# Core pipeline

The **core** is everything below the GUI and above the OS: the
byte-in-to-line-out pipeline that every Birdie front-end shares. It
has no `ludica` dependency, no OpenGL context, no widget tree. Given a
byte stream and a profile it produces trigger-fired events, script
calls, log records, and bytes back to the server.

Keeping this seam clean is the single most load-bearing architectural
decision in Birdie (`concept.md`, `doc/wishlist.md`). It is what makes
a later screen-reader / curses / headless front-end tractable instead
of a rewrite.

## Stages

    bytes in ──► transport ──► telopt ──► vt ──► line retirement ──► triggers ──► log sinks
                                                                   └──► bd_vm (Lua)
                                                                   └──► front-end (scrollback, UI events)

    bytes out ◄── transport ◄── telopt ◄── send queue ◄── triggers / bd_vm / front-end input

Each arrow is a concrete C function call, not a thread boundary.
There is one thread boundary, between **transport** and everything
else (`doc/network.md`). Everything from telopt down runs on the UI
thread.

## Modules

| module           | owns                                      | docs                |
|------------------|-------------------------------------------|---------------------|
| `src/net/`       | libiox, TLS, telopt wrapper over MTH      | `doc/network.md`    |
| `src/vt/`        | libvt + UTF-8 transcode + line retirement | `doc/terminal.md`   |
| `src/triggers/`  | trigger table, classes, dispatch          | `doc/triggers.md`   |
| `src/vm/`        | `bd_vm` abstraction, Lua 5.4 + LPeg back  | `doc/triggers.md`   |
| `src/log/`       | NDJSON + plaintext sinks                  | `doc/logging.md`    |
| `src/profile/`   | property-list storage, CSV I/O            | `doc/profiles.md`   |
| `src/core/`      | session glue: wires the above together    | this doc            |

`src/core/` is the only place that knows about all of the others. No
front-end source file includes anything from `src/net/`, `src/vt/`,
`src/triggers/`, `src/vm/`, or `src/log/` directly — they go through
`bd_session` (below).

## `bd_session`: the front-end seam

One session = one connected MUD. The front-end holds a
`bd_session *` per tab; everything the front-end needs is reachable
from it.

    typedef struct bd_session bd_session;

    bd_session *bd_session_new(const bd_profile *profile);
    void        bd_session_free(bd_session *s);

    /* lifecycle */
    int  bd_session_connect(bd_session *s);
    int  bd_session_disconnect(bd_session *s);
    bd_conn_state bd_session_state(const bd_session *s);

    /* input from the user */
    int  bd_session_send_line(bd_session *s, const char *utf8);
    int  bd_session_send_raw(bd_session *s, const void *bytes, size_t n);

    /* read-side: the front-end polls or subscribes */
    struct vt_buf *bd_session_vt(bd_session *s);
    bd_vm        *bd_session_vm(bd_session *s);
    int           bd_session_drain(bd_session *s);   /* apply queued bytes from net thread */

    /* events into the front-end */
    typedef void (*bd_session_event_fn)(bd_session *, const bd_session_event *,
                                        void *userdata);
    void bd_session_on_event(bd_session *s, bd_session_event_fn fn, void *userdata);

`bd_session_event` carries the union of things a front-end cares about
but cannot synthesize: `connect`, `disconnect`, `state_changed`,
`bell`, `title_changed`, `naws_requested`, `gmcp_arrived`,
`log_rotated`. These fire on the UI thread after `bd_session_drain`
processes the network ring.

The deliberately absent APIs are as important as the present ones:

- No "give me the next line" call — lines live in `vt_buf` and in the
  log; the front-end renders from `vt_buf` and scrolls through
  scrollback.
- No trigger registration here — scripts manage triggers through
  `bd_vm`, and the built-in verbs (`#action` etc.) are parsed by the
  input pipeline, not by front-end code.
- No settings / theme / font — those belong to the front-end. Core
  is indifferent to how the cells are drawn or whether they are
  drawn at all.

## Ownership model

`bd_session` owns:

- one `bd_net *` (the transport + telopt wrapper),
- one `struct vt_buf *` (the terminal grid),
- one `bd_triggers *` (the trigger table),
- one `bd_vm *` (the scripting engine),
- zero or more `bd_log_sink *` entries,
- the `bd_profile *` is borrowed, not owned — the profile store
  outlives sessions.

Free order on disconnect/destroy: sinks (flush), vm (close), triggers
(drop), vt_buf (drop), net (close). Reverse of build order.

## Threading contract

- **Net thread** runs the libiox loop. It owns `bd_net` internals and
  the TLS state. Its only output is bytes-plus-metadata pushed onto a
  single-producer / single-consumer lock-free ring per session.
- **UI thread** calls `bd_session_drain(s)` once per frame (or on a
  wake notification from the net thread). Drain pulls from the ring,
  runs the byte stream through telopt → vt → line retirement →
  triggers → sinks, and posts `bd_session_event`s to the front-end
  callback.
- **No other threads**. Lua runs on the UI thread. Log sinks write
  synchronously from the UI thread — NDJSON writes are append-only
  and cheap; if that turns out to be false under profiling, add a
  bounded writer thread then, not now.

Invariants:

1. `vt_buf` is mutated only on the UI thread.
2. `bd_vm` is called only on the UI thread.
3. `bd_net` is mutated only on the net thread; cross-thread commands
   go through a second ring in the opposite direction (send queue).
4. `bd_profile` prop lists are read-only while any session holds a
   reference; edits go through the profile store's copy-on-write API
   (`doc/profiles.md`).

## Line retirement

A *retired line* is the unit every downstream stage consumes. It is
produced at exactly one place: `src/vt/line.c` in the UI-thread drain,
when the cursor leaves a row by newline, wrap, or scroll. At
retirement Birdie has, for that line:

- the raw bytes received (for `kind:"recv"` NDJSON with `raw:`),
- the decoded UTF-8 text with attributes stripped,
- the cell array (for ANSI-styled plaintext log output),
- a monotonic sequence number and a wall-clock timestamp,
- a `replay` flag (set during post-reconnect scrollback replay; see
  `doc/network.md`).

The line is handed in order to:

1. **Triggers** — `line` and `prompt` type dispatch, `on.line` hook
   table, `on.prompt` for EOR/GA-marked lines (`doc/triggers.md`).
2. **Log sinks** — each sink's filter decides whether to format and
   write (`doc/logging.md`).
3. **Front-end event** — if the sink configuration or UI asks, a
   `bd_session_event` of kind `line_retired` is queued for the
   front-end. Most front-ends ignore it and render from `vt_buf`
   directly; a screen-reader front-end will consume it to drive
   speech.

Triggers run before sinks so a `#gag`'d line can be marked
`suppressed` in the NDJSON record instead of absent (debuggability),
and before the front-end event so gagged lines never reach the
screen-reader path.

## Out-of-band: GMCP / MSDP / MSSP

These bypass `vt_buf` entirely. They enter at the telopt layer, get
decoded (JSON for GMCP, MSDP wire format for MSDP), and fan out to:

- `bd_vm` via `on.gmcp[pkg]` / `on.msdp[pkg]` hook tables,
- log sinks as their own `kind` (`doc/logging.md`),
- `bd_session_event` so front-ends can surface MSSP in the profile
  dialog.

No line retirement, no trigger `line` match, no scrollback entry.

## Alternate front-ends

The seam is sized so these three front-ends are tractable:

- **ludica GUI** (v1.0) — consumes `vt_buf` with the glyph atlas,
  takes user input from the input-line widget into
  `bd_session_send_line`.
- **curses / stdout+stdin** — consumes `vt_buf` via a redraw loop
  that walks dirty rows and emits ANSI; reads stdin line by line into
  `bd_session_send_line`. Zero ludica dependency; links against
  everything under `src/` except `src/gui/`.
- **accessibility front-end** — ignores cells, subscribes to
  `line_retired` events, exposes the line buffer through AT-SPI
  (Linux) or UIA (Windows). Also zero ludica dependency.

If a change to `bd_session` or line-retirement semantics would break
any of these, revisit the change. This is the seam the whole accessibility
story rides on (`doc/wishlist.md`).

## Testability

The core pipeline is pure C with no GUI and no real sockets needed.
A test harness substitutes:

- a scripted byte-feeder in place of `bd_net` (feed canned server
  output, assert on outgoing bytes),
- the `bd_vm` recording backend from `doc/triggers.md`,
- an in-memory log sink that captures NDJSON records.

With these three, trigger behavior, line retirement, log schema, and
reconnect semantics are all unit-testable without a display or a
network.

## Open questions

- Should `bd_session_drain` be push (net thread wakes UI via eventfd
  + event callback) or pull (UI thread calls each frame)? Push is
  strictly better for the curses and accessibility front-ends (no
  frame loop) — lean push, but both front-ends can coexist if needed.
- Cross-session broadcast (`#all` in TinTin++). Fits as a helper in
  `src/core/` over an array of `bd_session*`; defer the API until a
  user asks.
