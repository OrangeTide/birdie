# Terminal rendering

The terminal widget is the heart of a MUD client. Birdie gets it by
**reusing `lumi/libvt`** — the VT engine inside `lumimux`, a production
terminal emulator/multiplexor — rather than writing a parser, cell grid,
or scrollback from scratch. That code represents hard-won work on edge
cases (wide chars, combining marks, scroll regions, reflow on resize)
that is expensive to redo and easy to get subtly wrong.

## Reusable code

- **`~/DEVEL/lumi/src/libvt/`** — the whole VT stack:
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
  - `test_vt.c` — conformance tests worth running against any change.
- **`~/DEVEL/lumi/src/libutf8/`** — UTF-8 decode / width. `libvt`
  already calls into it; birdie ships the same way.
- **`~/DEVEL/ludica/samples/ansiview/`** — the CP437 glyph-atlas
  renderer. We swap its bundled parser for `libvt` and keep the
  atlas-drawing path.

Vendor both under `src/thirdparty/libvt/` and `src/thirdparty/libutf8/`
per `doc/vendoring.md`. The `UPSTREAM` file points at `~/DEVEL/lumi`.

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

Do not reinvent any of these. If `libvt` is missing something, extend
it upstream in `lumi` rather than forking the API inside Birdie.

## What Birdie adds on top

### `bd_terminal` widget

The retained-mode widget (`BD_TERMINAL` — `doc/gui.md`) wraps a
`struct vt_buf *` and a glyph-atlas renderer. Incoming bytes arrive
from the network thread (`doc/network.md`), are handed to
`vt_parse_feed()`, and mutate the `vt_buf`. The widget's redraw path
iterates visible rows, skips ones not marked `VT_ROW_DIRTY`, and emits
textured quads through `ludica`.

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

### Glyph atlas

From `ludica/samples/ansiview`. One texture atlas per font; the
renderer looks up `codepoint` → atlas cell, then emits a textured quad
with fg/bg colors. Fallback glyph for missing codepoints is the CP437
replacement block.

- v1.0 ships a bundled VGA-style CP437 font (public domain) for the
  MUDs that still expect DOS aesthetics, plus a Unicode coverage font
  (proposed: **Cozette** or **Unifont** — final pick at build time)
  for everything else.
- Per-profile font override is a `font` column on the profile plist
  (`doc/profiles.md`). It is a **custom column** — not in the v1.0
  title schema — so that font preferences do not leak across users
  via CSV export.
- Font fallback chain: profile font → Unicode font → CP437 font →
  replacement block.

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
(proposed: `VT_ATTR_HYPERLINK` — upstream `libvt` change) so the
renderer can draw underlines and the hit-test layer can open the
default browser on click. An alternative is an overlay layer in the
widget that does not touch `libvt`; decide at implementation time based
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

- Upstream the hyperlink attribute to `libvt` vs. keep it in a birdie
  overlay. Lean upstream — the attribute has general terminal value.
- Default scrollback size. 10000 lines is generous for a MUD and cheap
  on modern RAM; revisit if Pi Zero profiling says otherwise.
- Ligatures / HarfBuzz. Out of scope; MUD output is overwhelmingly
  monospaced ASCII art and Unicode box drawing where ligatures are
  harmful. Revisit only if a user request surfaces.
- Bundled Unicode font pick (Cozette vs. Unifont vs. other). Defer
  until the atlas builder exists and we can eyeball coverage.
