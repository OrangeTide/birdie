# Logging

Birdie supports two log formats, running simultaneously if desired:

1. **NDJSON** — the source of truth. Structured, queryable with `jq`,
   reingestable for scrollback replay and the built-in history viewer.
2. **Plain text** — human-readable, `tail -f`-friendly, derivable from the
   NDJSON stream.

Any number of log **sinks** can be attached. Each sink is:

    filter -> formatter -> path-template

so the user can, e.g., keep one plaintext-all-traffic log per day and a
separate NDJSON log of only GMCP messages, written to different trees.

## Path template

Follows the user's IRC/screen workflow:

    <root>/<YYYY>/<app-or-server>/<character-or-channel>/<YYYY-MM-DD-HH>00.<ext>

- Top-level `<YYYY>` subdirectory per year.
- One file per **hour-bucket**, append mode.
- Single-session assumption (no concurrent sessions to the same
  character/channel). Documented as a known limitation.

The template is configurable; tokens available include `{year}`, `{month}`,
`{day}`, `{hour}`, `{mud}`, `{character}`, `{profile}`, `{ext}`.

## NDJSON schema (v1)

One JSON object per line. Unknown fields are preserved on re-ingest. Times
are ISO 8601 UTC with millisecond precision.

### Common fields (every record)

    t         string   ISO-8601 UTC timestamp with milliseconds
    v         integer  schema version (currently 1)
    kind      string   record type (see below)
    mud       string   MUD profile name
    session   string   session UUID (stable for the life of one connection)

### `kind` values

- `connect` — `{host, port, tls, remote_addr}`
- `disconnect` — `{reason}`
- `recv` — an inbound line from the MUD
- `send` — an outbound command
- `gmcp` — `{package, name, data}` where `data` is already-parsed JSON
- `mxp` — `{tag, attrs, text}`
- `note` — user/script annotation via `log.note(...)` (`{text}`)
- `error` — client-side error (`{where, message}`)

### `recv` / `send` payload

    raw       string   original bytes with ANSI escapes intact (UTF-8)
    text      string   ANSI-stripped text
    ansi      array?   optional span list: [{start,end,fg,bg,flags}, ...]

`raw` is the source of truth for replay. `text` is present for convenience
(searching, triggers that don't care about color). `ansi` is optional and
only emitted when the sink requests it (keeps log size down by default).

### Example

    {"t":"2026-04-19T14:22:03.117Z","v":1,"kind":"recv","mud":"discworld","session":"e6c…","raw":"\u001b[32mYou see a small dog.\u001b[0m","text":"You see a small dog."}
    {"t":"2026-04-19T14:22:05.002Z","v":1,"kind":"send","mud":"discworld","session":"e6c…","raw":"pet dog","text":"pet dog"}
    {"t":"2026-04-19T14:22:05.214Z","v":1,"kind":"gmcp","mud":"discworld","session":"e6c…","package":"Char.Vitals","name":"update","data":{"hp":87,"maxhp":100}}

## Plaintext format

Straightforward. Each line is the ANSI-stripped MUD output. Sent commands
are interleaved with a `>> ` prefix. An optional hourly header is written
when a new file is opened:

    === 2026-04-19 14:00 UTC — discworld (session e6c…) ===

ANSI-preserving plaintext is available as a separate formatter
(`plaintext-ansi`) for users who want to `cat` the file into a terminal and
see color.

## Sink configuration

A sink is defined by three things:

- **filter** — a predicate over records. Built-ins: `all`, `recv`, `send`,
  `traffic` (recv+send), `gmcp`, `mxp`, `notes`, or a Lua predicate.
- **formatter** — `ndjson`, `plaintext`, `plaintext-ansi`.
- **path** — path template as above.

Example (TOML sketch, not final):

    [[log.sink]]
    filter    = "traffic"
    formatter = "plaintext"
    path      = "{root}/{year}/{mud}/{character}/{year}-{month}-{day}-{hour}00.log"

    [[log.sink]]
    filter    = "all"
    formatter = "ndjson"
    path      = "{root}/{year}/{mud}/{character}/{year}-{month}-{day}-{hour}00.ndjson"

    [[log.sink]]
    filter    = "gmcp"
    formatter = "ndjson"
    path      = "{root}/gmcp/{year}/{mud}/{year}-{month}-{day}.ndjson"

## Replay and history viewer

Because NDJSON is the source of truth and includes `raw` with ANSI intact,
the last N lines of history can be rendered identically to live output by
re-feeding `raw` from `recv` records through the terminal widget. This
powers:

- "show last N lines on reconnect"
- a built-in scrollback viewer that can page backwards across hour-bucket
  boundaries
- external tooling: `jq 'select(.kind=="recv") | .text'` just works

## Rotation and retention

Rotation is inherent (new hour → new file). Retention is a user concern;
Birdie does not delete logs. Document a suggested cron/`tmpwatch` recipe
and move on.

## Implementation status

Built (`src/birdie/bd_log.{c,h}`, wired through `bd_session`):

- **`bd_log`** — a set of sinks, each a `(filter, formatter, destination)`
  triple. Filters: `all` / `recv` / `send` / `traffic` / `gmcp` / `mxp` /
  `notes`. Formatters: `ndjson`, `plaintext`, `plaintext-ansi`. Destinations:
  hour-bucketed files (path template expanded per record, reopened as records
  cross hour boundaries, parent dirs created, plaintext gets the hourly header
  on a fresh bucket) and a caller callback (tests / future front-ends).
  Records carry their own wall-clock time (`bd_log_now_ms()` in production) so
  the layer is deterministic under test. NDJSON escapes strings per RFC 8259
  and emits already-parsed `gmcp` `data` verbatim (not re-quoted).
- **`bd_session` integration** — when a session's data dir is set, two default
  sinks are installed under `<data_dir>/logs`: a full NDJSON log (source of
  truth) and a plaintext `traffic` log, both hour-bucketed per profile via the
  documented path template. The session emits `recv` (raw + ANSI-stripped),
  `send` (every outbound path: aliases, `@`/`mud.send`, and un-aliased input),
  `connect` (`host`/`port`/`tls`), `disconnect` (`reason`), and `gmcp`
  (`package` + parsed `data`) records, plus `note` records via the
  `log.note(text)` script API (`__bd_note` host fn). Each session gets a
  process-unique `session` id at creation.

- **replay** (`src/birdie/bd_replay.{c,h}`) — `bd_replay_recv` walks the
  NDJSON buckets for a `(mud, character)` pair newest-first (stopping once it
  has enough), unescapes each `recv` record's `raw`, and hands the last N back
  oldest-first. `bd_session_replay(s, max)` re-emits those as
  `BD_SESSION_DATA` events with the `replay` flag set (no triggers, no `on.*`
  hooks); the ludica front-end calls it on connect to restore recent
  scrollback and notes the restored count.

Not yet built: configurable/multiple user-defined sinks (only the two defaults
are wired), Lua-predicate filters, the `mxp` record path (no MXP parser yet),
`error` records, password redaction on `send`, and the paging scrollback
viewer (the replay primitive above is its building block).

## Open questions

- Compression of old buckets. `.ndjson.zst` is attractive; we can read it
  transparently in the replay path. Not v1.0.
- Redaction of passwords in `send` records (login sequences). v1.0: a
  configurable regex that replaces matched groups with `***` before the
  record hits any sink.
- Whether to log keepalive/NAWS/TELNET negotiation. Default off, opt-in
  `kind:"telnet"` records for debugging.
