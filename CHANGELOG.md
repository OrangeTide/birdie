# Changelog

## [0.9.0]

Batches the breaking toolkit API changes made while breakage is cheap, plus a
large round of editor, dialog, and MUD-client features.

### Added

Editor

- Layered style runs and find/replace highlighting in `bd_editor`
  (`bd_editor_find` / `_next` / `_prev` / `_replace` / `_replace_all`), composed
  over independent syntax / app / find / mark layers so highlights never clobber
  each other.
- Runtime-configurable syntax highlighting: `bd_syntax`, a state-machine
  tokenizer with an in-memory text format, built-in Lua and ABC languages, and
  filename autodetect; installed with `bd_editor_set_syntax`.
- `bd_findbar`, a composed find/replace bar bound to an editor (live search, a
  match counter, prev/next, an optional replace row). Text fields gained
  `BD_ON_CHANGE_F` (fires on every edit) and `BD_ON_CLOSE_F` (fires on Escape).
- Cross-line stream text selection in `BD_TEXT_AREA` and the editor, with
  clipboard copy and cut.

Widgets and dialogs

- `BD_SPLIT`, a nestable resizable sash-pane container.
- `BD_GROUPBOX` etched fieldset and `BD_SCROLLVIEW` scrolling form container (on
  a new `BD_WC_CLIP_CHILDREN` render-clip flag), with `bd_dialog` integration.
- `BD_TABLE` rich cells: optional per-cell icons and per-row bold / colour / tint
  via model hooks.
- `BD_TREE` optional right-aligned dimmed per-node `detail` badge.
- Explorer icon / list / details view modes.
- Dwell-triggered tooltips (`BD_TIP_S`) and an app-driven context menu
  (`bd_popmenu`).
- Form controls and modal stacking: `BD_CHECKBOX`, radio group, numeric spinner,
  a `BD_COMBO` drop-down over a shared overlay primitive, the `bd_dialog`
  composition helper, `bd_modal_open`/`_close` stacking, and an HSV
  `BD_COLORPICK`.
- OS-integrated file chooser: `bd_filedlg` over the UI-agnostic `bd_fs` model,
  with POSIX/freedesktop and a compile-verified Win32 backend.

MUD client

- MCP 2.1 (MUD Client Protocol) with the `dns-org-mud-moo-simpleedit` package
  (`bd_mcp`): in-band `#$#` handling, a handshake with a client-generated auth
  key, package negotiation, `#$"` quoting, and multiline collection; wired into
  the session line pipeline and an in-app editor (moo-code gets Lua
  highlighting), sending edits back on save.
- Profile script editor (Session > Edit script...) that edits the profile's
  `triggers.lua` with Lua highlighting and a find bar.
- Per-profile inbound encoding (UTF-8 / ISO-8859-1 / Windows-1252 / CP437, the
  last for DOS/BBS art) with an 8-bit to UTF-8 transcode and server CHARSET
  negotiation.
- The connect / edit-profile / app Settings / live trigger-editor dialogs, CSV
  export with per-column filters, and import-collision resolution; GUI-added
  triggers persist to a per-profile `triggers.csv` kept separate from the
  hand-written `triggers.lua`.
- Ludica backend clipboard hooks (Ctrl-C/X/V through the X11 CLIPBOARD).

Build, packaging, and tests

- Per-texture filter choice on the backend texture hooks (see breaking changes).
- `make dist` drift guard (`dist-check` + `scripts/dist-check.sh`): the bundle
  copy-lists must cover every toolkit source, or the build fails.
- Client-core test suites: `test_client`, `test_session`, `test_netloop` (now
  including a real TLS handshake and encrypted round trip against an in-test
  mbedTLS server), and `test_mcp`.

### Changed (breaking)

- Value widgets (slider, toggle, wheel, jog, knob, X-Y pad) now take a
  descriptor-struct create signature (`bd_*_desc`).
- Selection accessors standardized on `_selected()` / `_select()` (from the old
  `_active` / `_set_active`).
- Managed-canvas minimize target unified into one enum API
  (`bd_managed_canvas_set_minimize`).
- Backend texture-creation hooks (`load_texture` / `load_texture_mem` /
  `make_texture`) gained a `bd_filter` argument so the client picks NEAREST vs
  LINEAR per texture.

### Fixed

- Ludica backend scissor origin (clipped widgets no longer blank out).
- Window-count overflow, progress-bar aspect guard, CSI parameter overflow, and
  an out-of-bounds IME preedit-caret read.
- All eight gravity points map to distinct anchors.

### Internal

- Trimmed the vendored ludica tree by ~3.6 MB (unused assets, build variants,
  and configs) and extended the re-vendor prune list.
- Reconciled the `make dist` bundle and examples toolkit source lists.

<!-- Made by a machine. PUBLIC DOMAIN (CC0-1.0) -->
