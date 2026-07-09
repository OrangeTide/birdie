# Terminal rendering

The terminal widget is the heart of a MUD client. Birdie gets its VT
engine from **lumi's `libvt`** — the parser inside `lumimux`, a production
terminal emulator/multiplexor — rather than writing a parser, cell grid,
or scrollback from scratch. That code represents hard-won work on edge
cases (wide chars, combining marks, scroll regions, reflow on resize)
that is expensive to redo and easy to get subtly wrong. It is now
**adopted as first-class birdie code** (see below), not tracked upstream.

## The VT engine (embedded)

The engine lives in **`src/birdie-gui/bd_vt/`**, adopted from lumi's
`libvt` and built as the `birdie_gui_vt` library together with the
terminal widget. The cord to lumi is cut: it is edited in place, not
re-vendored (a final hand-merge from upstream landed with the embed;
grep lumi only to cherry-pick a specific later fix). The stack:

- `vt_parse.[ch]` — state-machine parser for ANSI / VT escape
  sequences, OSC, DCS. Handles 256-color and 24-bit truecolor.
- `vt_cell.[ch]` — per-cell record: codepoint, fg/bg color (default
  / indexed / RGB union), attribute bitmask (bold, underline,
  italic, reverse, blink, undercurl, dim, hidden, strike,
  predicted-echo), width (1 or 2 for wide chars).
- `vt_buf.[ch]` — grid buffer **plus scrollback**. Rows carry
  flags (`VT_ROW_WRAPPED`, `VT_ROW_DIRTY`). Resize reflows. Scroll
  regions, row clears, dirty-tracking, and scrollback access are
  all first-class operations.
- `vt_ops.[ch]` — cursor movement, SGR apply, erase-in-display /
  erase-in-line, scroll-region ops.
- `vt_state.[ch]` — terminal modes, DEC private modes, tab stops,
  saved-cursor state.
- `rune_width.[ch]` + `width_tables.c` — Unicode display-width tables
  for wide-character cells.

UTF-8 decode is birdie-gui's own `bd_utf8` (the toolkit's single codec,
shared with the renderer), which the engine calls instead of carrying a
second copy. Cell rendering lives in the widget (`bd_widget_vt.c`), which
draws each cell through `bd_draw`'s embedded bitmap font (`bd_draw_cell`),
not a bundled renderer or a texture atlas.

## What `libvt` already handles

This list matters because it is the set of things we do **not** write:

- ANSI CSI / SGR including 256-color and 24-bit RGB.
- UTF-8 input, including surrogate handling and invalid-byte
  replacement.
- Wide characters (CJK, emoji) via per-cell `width`.
- Scrollback with dirty-row tracking.
- Scroll regions (DECSTBM), line wrapping (`VT_ROW_WRAPPED`), reflow
  on resize.
- Predictive / speculative local echo (`VT_ATTR_PREDICTED`) — useful
  later for latency-hiding on high-ping MUDs.
- Cursor save/restore, tab stops, DEC private modes.

Do not reinvent any of these. If the engine is missing something, extend
it in place under `src/birdie-gui/bd_vt/` — it is birdie's code now.
(Historically the guidance was to upstream to lumi; that ended when the
engine was embedded and the cord cut.)

## What Birdie adds on top

### `bd_terminal` widget

The retained-mode widget (`BD_TERMINAL` — `doc/gui.md`) is built in
`bd_widget_vt.c`: it wraps a `struct vt_state`/`vt_buf` and a glyph-atlas
renderer. Incoming bytes arrive from the network thread (`doc/network.md`),
are handed to `vt_parse_feed()`, and mutate the buffer. The widget's redraw
path iterates visible rows and emits textured quads through `bd_draw` (the
backend-neutral renderer), so it works on any backend, not just ludica.

Dirty-region redraw (not 60 Hz full repaint) is the reason retained
mode was chosen; `VT_ROW_DIRTY` is the signal that makes it cheap.

### Scrollback UX

`vt_buf_scrollback_lines()` + `vt_buf_scrollback_row()` are the API.
The widget exposes:

- Mouse wheel and Page Up / Page Down scroll through scrollback.
- A scrollback indicator overlay ("42 lines above / live") while
  detached from the bottom.
- Jump-to-bottom on any keyboard input, matching every MUD client's
  expected behavior.
- Copy-on-select (rectangular and line modes), text-only (ANSI
  stripped) by default, with a modifier for ANSI-preserved copy.

Scrollback depth is per-profile with a global default (proposed:
10000 lines). Memory cost is bounded because `vt_cell` is a POD
record and rows are fixed width.

### Search

Forward/backward search over the visible grid and scrollback, matching
against the `text` extraction (not raw cells). Plain substring in v1.0;
regex later. Search results dirty the matching rows so highlighting
falls out of the existing redraw path.

### Glyph rendering

The widget draws each cell through `bd_draw_cell`, birdie-gui's embedded
fixed-cell bitmap font: an 8x8 / 8x16 unscii subset (public domain)
compiled into the toolkit, looked up by real codepoint and drawn with
fg/bg colors. The terminal carries no font texture of its own. A
codepoint outside the subset renders as `?`.

- The embedded subset covers Basic Latin, Latin-1, punctuation, arrows,
  box drawing, block elements, and geometric shapes — the glyphs MUD
  output actually uses. Wider Unicode coverage (a profile-selectable
  font) is future work.
- Per-profile font override is a `font` column on the profile plist
  (`doc/profiles.md`). It is a **custom column** — not in the v1.0
  title schema — so that font preferences do not leak across users
  via CSV export.
- Font fallback: embedded bitmap font → `?` for an unmapped codepoint
  (a profile / Unicode coverage font would extend the chain).

### Copy / paste

- Copy: text-only by default (`vt_cell.codepoint` per visible cell,
  respecting `width`, skipping `VT_ATTR_HIDDEN`). Modifier copies
  with ANSI escapes for paste into another terminal.
- Paste: splits on newlines, sends lines one at a time with a small
  inter-line delay (default 10 ms) to avoid tripping spam guards.
  User-configurable per-profile.

### Hyperlinking

URL auto-detection runs over the `text` projection of each line as it
is retired from the live cursor. Matched spans get a `VT_ATTR_*`
(proposed: `VT_ATTR_HYPERLINK`, added directly in `bd_vt`) so the
renderer can draw underlines and the hit-test layer can open the
default browser on click. An alternative is an overlay layer in the
widget that does not touch the VT engine; decide at implementation time based
on how many other overlays we need (search highlights, trigger
highlights) and whether one general mechanism is cheaper.

OSC 8 (terminal hyperlink escape) is accepted from the server for free
once `libvt` parses it — no MUD ships this today but cost is near
zero.

## Threading

Parsing runs on the UI thread, not the network thread. The network
thread's job ends at "push bytes onto the ring." This matches
`doc/network.md` and `doc/triggers.md` and keeps `vt_buf` mutation
single-threaded (no locking inside `libvt`).

## Trigger / log integration

The line-retirement path (where a row leaves the cursor's reach,
either by newline or by wrap) is the seam into the trigger engine and
log sinks. At that point birdie has:

- the raw bytes (for NDJSON `recv` with `raw:`),
- the decoded UTF-8 `text`,
- the cell array (for styled rendering in the plaintext log via ANSI).

This is where `on.line` hooks fire (`doc/triggers.md`) and where NDJSON
log records are emitted (`doc/logging.md`). No other site in the code
should claim the "a line arrived" semantics.

## Open questions

- Add the hyperlink attribute to the embedded `bd_vt` engine vs. keep it
  in a birdie overlay. Lean engine — the attribute has general terminal
  value and the engine is birdie's to extend now.
- Default scrollback size. 10000 lines is generous for a MUD and cheap
  on modern RAM; revisit if Pi Zero profiling says otherwise.
- Ligatures / HarfBuzz. Out of scope; MUD output is overwhelmingly
  monospaced ASCII art and Unicode box drawing where ligatures are
  harmful. Revisit only if a user request surfaces.
- Bundled Unicode font pick (Cozette vs. Unifont vs. other). Defer
  until the atlas builder exists and we can eyeball coverage.
