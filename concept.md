# Desktop MUD client

## Requirements

- Portable to Linux (x86-64 and aarch64) and Windows (x86-64). macOS as
  stretch goal.
- Friendly installer on Windows release (NSIS).
- User-managed MUD list.
- Shareable MUD list import/export through GitHub gist or paste.net links.
  Simple flat CSV format with a **title schema**: the first row holds
  defacto-standard column names that the importer maps to internal
  structures; unknown columns are preserved but ignored. Semantically
  equivalent to a SQLite dump of a strings-only table.

## Decisions

Answers to the former open questions, recorded here so they stop being
open. Further design notes for each area live under `doc/`.

- **Language:** C. Best portability story across our targets and the most
  reusable code already available in `lumi`, `boris`, and `jondev`.
- **GUI toolkit:** **roll our own**, thin **retained-mode** widget layer,
  XView-shaped attribute-list API (`bd_create(parent, TYPE, ATTR, val,
  ..., BD_NONE)`). No ImGui, no Nuklear, no GTK/FLTK. Retained-mode is
  chosen over immediate-mode because (a) a logical widget tree is what
  the future screen-reader front-end needs to expose through AT-SPI /
  UIA, and (b) Raspberry Pi Zero/3 targets benefit from dirty-region
  redraw instead of 60 Hz full repaints. Widget set is deliberately
  small — see `doc/gui.md`. Prior art: XView, and Jon's own `libquaoar`
  (`~/research/programmable-display-protocols/demo/libquaoar.h`).
- **GUI rendering:** drawing primitives on top of **`ludica`**
  (`~/DEVEL/ludica`). Targets **GLES2 only**. One set of shaders works on
  every OS via ANGLE (including macOS, avoiding the deprecated desktop-GL
  path). The GLES2 target is also a **subset of WebGL 1.0**, so a future
  wasm build is not foreclosed — `ludica` already builds to wasm. (Raw
  telnet from wasm is blocked by the browser's lack of raw-TCP; a wasm
  build would need a WebSocket-to-telnet bridge.) **GLES3 is explicitly
  out of scope** — compatibility is the priority; forks are welcome to
  add it.
- **Terminal rendering:** reuse and extend the CP437/DOS glyph-atlas
  renderer from `ludica/samples/ansiview`. Replace its ANSI parser with
  `lumi/libvt`, which already handles ANSI + 256-color, UTF-8, wide chars,
  and emoji.
- **Transport:** telnet and TLS telnet for v1.0. SSH deferred — libssh2
  is a large dependency for a small set of MUDs.
- **MUD protocol extensions:** MCP, NAWS, GMCP, MSDP, MSSP at v1.0.
- **Graphical protocols:** SIXEL in **v1.0**; MXP (including `<IMAGE>`)
  in **v1.1**. RIPterm is not planned. Kitty graphics protocol has no
  MUD adoption and is parked on the wishlist.
- **Scripting:** **Lua 5.4 + LPeg**. Picked over Wren because LPeg
  dominates the pattern-matching workload that MUD scripting actually
  is, and the installed base of Lua trigger snippets across Mudlet /
  MUSHclient / TinTin++ / TinyFugue means users can paste existing
  scripts with minimal edits. Engine lives behind a `bd_vm` abstraction
  so alt backends (null, test recorder, a future fork in another
  language) don't require changes to the trigger engine. See
  `doc/triggers.md`.
- **Triggers / aliases / timers:** TinTin++-style verb syntax over a
  ZMud-style organizing model (nestable classes, multi-state chains,
  uniform trigger table). Full Lua is the escape hatch. See
  `doc/triggers.md`.
- **Logging:** NDJSON (source of truth) and plaintext (derivable) can run
  simultaneously; multiple sinks with independent filters, formatters, and
  path templates. Path template matches Jon's IRC/screen workflow:
  `<year>/<mud>/<character>/<YYYY-MM-DD-HH>00.<ext>`, append mode. See
  `doc/logging.md`.
- **URL handling:** URLs in output are auto-detected and made clickable
  via the OS default browser.
- **Auto-mapping:** deferred. The scripting API will expose enough room /
  exit / GMCP-room events for a user script to prototype it first.
- **License:** dual-licensed. SPDX expression
  `MIT-0 OR LicenseRef-PD-hd`. Each source file carries
  `SPDX-License-Identifier: MIT-0 OR LicenseRef-PD-hd`.
  `LicenseRef-PD-hd` reads: *"This work is hereby dedicated to the public
  domain."* Jurisdictions that do not recognize public-domain dedication
  fall back to MIT-0. Reference: https://cr.yp.to/spdx.html
- **Agentic debugging:** `ludica` is gaining an MCP (Model Context
  Protocol) layer so AI agents can introspect a running session for
  debugging. This is a notable capability sokol and SDL do not offer
  and is a reason to prefer `ludica` independent of the graphics
  portability argument.

## Architecture seam to preserve

Core pipeline (network → parser → line buffer → trigger engine → log
sink) is kept independent of `ludica` and the GUI. This costs little in
v1.0 and keeps alternate front-ends — a stdin/stdout or curses build, a
screen-reader front-end speaking platform accessibility APIs — tractable
later. See `doc/wishlist.md`.

## Documents

- `doc/core.md` — core pipeline seam (`bd_session`, line retirement,
  threading contract) that keeps the GUI swappable.
- `doc/gui.md` — retained-mode widget layer, XView-style API, v1.0
  widget set.
- `doc/terminal.md` — terminal widget built on `lumi/libvt` (parser,
  cell grid, scrollback) + `ludica` glyph atlas.
- `doc/network.md` — sockets, TLS (mbedTLS), telnet, and MUD telnet
  extensions (MTH from boris as the starting point).
- `doc/profiles.md` — the MUD list: property-list storage, CSV title
  schema, import/export column filters.
- `doc/triggers.md` — trigger / alias / timer design.
- `doc/logging.md` — NDJSON schema, plaintext format, sink configuration.
- `doc/build.md` — GNU make layout, targets, cross-build story.
- `doc/vendoring.md` — conventions for third-party code under
  `src/thirdparty/` (provenance, update scripts, refresh workflow).
- `doc/wishlist.md` — post-v1.0 ideas, including accessibility /
  screen-reader mode inspired by VIP-Mud.

## Build system and vendoring

- Build system: GNU make, following the conventions of
  `/home/jon/DEVEL/modular-make/GNUmakefile` (the same system `ludica`
  uses).
- Third-party code, including `ludica`, is **vendored** under
  `src/thirdparty/<name>/`, not submoduled. Provenance lives in an
  `UPSTREAM` file beside the code; refreshes run through a paired
  `scripts/update-<name>.sh`. Full conventions in `doc/vendoring.md`.
- Upstream for ludica: https://github.com/OrangeTide/ludica.git

## Resources (reusable code in sibling repos)

- **modular-make** — starting point for `GNUmakefile`:
  `/home/jon/DEVEL/modular-make`
- **lumimux** — `/home/jon/DEVEL/lumi`
  - terminal escape sequence processing: `src/libvt`
    (ANSI + 256-color, UTF-8, wide chars, emoji already handled)
  - networking: `src/libiox`
  - config files: `src/libcfg`
  - UTF-8: `src/libutf8`
  - misc: `src/libcore`
- **boris** — MUD telnet processing (server-side, useful reference):
  `/home/jon/boris`
- **WebRPG** — complete windowed GUI running in wasm+JS, reference for
  the wasm-future question: `/home/jon/DEVEL/rust/webrpg`
- **ludica** — `~/DEVEL/ludica`. Graphics library (sokol-like with
  utilities). GLES2 target, ANGLE on macOS, wasm-capable. MCP layer in
  progress. See `samples/ansiview` for CP437 glyph-atlas rendering.
- **jondev snippets** — `/home/jon/jondev/code`. Grep before writing new
  utility code. Notable: `csv.[ch]`, `base64.[ch]`, `base26.c`,
  `base85.c`.
