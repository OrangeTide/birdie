# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Birdie is a desktop MUD client written in C. Its GUI is **birdie-gui**, a self-contained retained-mode widget toolkit drawn through a backend-neutral GPU interface; ludica is the default backend (rendering, input, audio, networking). Design docs live in `doc/` and `concept.md`.

## Build

```sh
make                # debug build
make RELEASE=1      # optimized (-O2, LTO)
make clean
make test           # headless GUI toolkit test (no window/ludica/X11)
make widget-test    # standalone widget gallery on the raw X11/GLES backend
make dist           # bundle the GUI toolkit into a versioned ZIP
```

Output: `_out/<triplet>/bin/birdie` (e.g. `_out/x86_64-linux-gnu/bin/birdie`). Also builds ludica tools: `ludica-launcher`, `ludica-mcp`, `ludica-mcp-bridge`, `font2slug`.

`make test` compiles `test/test_gui.c` (a recording stub backend) with the toolkit sources and runs it, linking only libvt. It needs no display, so it runs in CI; a failed check fails the build.

`make widget-test` builds a windowed widget gallery (`src/guitest/`) on a raw X11/EGL/GLES backend, independent of ludica, exhibiting every widget. Linux/X11 only and opt-in; run it from the repo root so the default `BD_ASSET_*` paths resolve. birdie runs on ludica and the gallery on GLES, so both backends stay exercised.

`make dist` stages the GUI toolkit (public headers, `widget.c`, the reference ludica backend, the VT extension, runtime assets, README) into `_out/<triplet>/birdie-gui-$(GUI_VERSION).zip`. Override the version with `make dist GUI_VERSION=x.y.z` (default `0.2.0`). The dist file lists in `module.mk` are stale (see `todo.txt`): the v0.2 renderer/value-widget sources, the explorer, and the GLES backend + gallery are not yet bundled. Both targets live in the top-level `module.mk`, so `scripts/update-gnumakefile.sh` won't clobber them.

Build system is [modular-make](https://github.com/OrangeTide/modular-make). Each directory has a `module.mk`; the top-level one controls which SUBDIRS are pulled in.

## Source layout

- `src/birdie/` — birdie app + the birdie-gui toolkit: `main.c`, the widget core (`widget.{c,h}`, `widget_ext.h`), renderer (`bd_draw.{c,h}`), backend interface (`bd_backend.h`) + ludica backend (`bd_backend_ludica.{c,h}`), theme (`bd_theme.h`), and the extension widgets (`bd_widget_vt.*` terminal, `bd_widget_value.*`, `bd_widget_explorer.*`)
- `src/guitest/` — standalone widget gallery (`widget_test.c`) + the raw X11/EGL/GLES backend (`window.h`, `x11_window.c`, `bd_backend_gles.*`); built by `make widget-test`
- `src/birdie/assets/` — chrome TTF (DejaVuSans), CP437 terminal font atlases, pushpin sprites
- `src/thirdparty/ludica/` — vendored ludica (rendering, input, audio, networking)
- `src/thirdparty/stb/` — vendored stb_truetype + stb_image
- `test/test_gui.c` — headless toolkit test (recording stub backend); `make test`
- `module.mk` — top-level build wiring (SUBDIRS, shader rule)
- `GNUmakefile` — modular-make entry point (fetched, not hand-written)
- `scripts/update-ludica.sh` — re-vendor ludica from upstream git
- `scripts/update-gnumakefile.sh` — fetch GNUmakefile from modular-make
- `doc/` — design documents (network, terminal, GUI, triggers, etc.)
- `concept.md` — original requirements and open design questions

## Vendoring

Birdie vendors its dependencies rather than linking to sibling repos. **Never symlink between projects.** Each dependency has an update script under `scripts/`:

- `scripts/update-ludica.sh [ref]` — shallow-clones ludica from GitHub, copies into `src/thirdparty/ludica/`, and merges ludica's Claude Code skills into `.claude/skills/`. Default ref: `main`.
- `scripts/update-gnumakefile.sh [ref]` — fetches GNUmakefile from modular-make GitHub releases.

Provenance is recorded in `src/thirdparty/ludica/UPSTREAM`.

## Top-level module.mk

The top-level `module.mk` deliberately bypasses ludica's own `src/module.mk` (which pulls in samples/imgui we don't need and assumes `tools/glsl2h` at the make root). It SUBDIRs into only the ludica library dirs and tools, and redeclares the shader-to-C rule with the vendored path.

## MCP integration

Ludica provides an MCP server for observing/controlling birdie at runtime. Config is in `.claude/settings.json`. To use it:

1. Build: `make`
2. Start the launcher: `LUDICA_MCP_ALLOWEXEC=$(echo _out/*/bin/* | tr ' ' ':') _out/x86_64-linux-gnu/bin/ludica-launcher &`
3. The bridge (`ludica-mcp-bridge`) connects Claude Code to the launcher on port 4000.

See `.claude/skills/ludica-mcp/SKILL.md` for the full tool reference.

## Current state

The GUI toolkit (now packaged as **birdie-gui**) is the most developed part of the repo. It is a retained-mode widget layer drawn through a backend-neutral GPU interface (`bd_backend`), with two backends: the reference ludica binding (`bd_backend_ludica.c`) and a raw X11/EGL/GLES binding (`src/guitest/`). The renderer is `bd_draw.c` (shader + dynamic quad batch + stb_truetype text). See `doc/gui.md` for the design and an Implementation status section.

`src/birdie/main.c` is a thin MUD-client shell built on the toolkit: a frame with a menu bar, a `BD_TERMINAL` output pane, a `BD_INPUT_LINE`, buttons, and a status bar, running on ludica. Networking, triggers, and the real MUD logic are not wired up yet.

Built today: chrome widgets (frame/panel/label/button/menu+pushpins/input-line/terminal), value widgets (slider, knob, toggle, wheel, jog, X-Y pad), an explorer/icon-browser widget, multiple native windows (GLES backend), and Tab focus traversal. Not yet: `BD_TEXT`/`BD_MULTILINE`/`BD_LIST`/`BD_SCROLLBAR`/`BD_NOTICE`/`BD_TAB_BAR`, IME, clipboard, multitouch, pen (tracked in `doc/gui.md`).

## Target platforms

Portable across Linux (x86-64, aarch64) and Windows (x86-64); macOS is a stretch goal. Windows releases ship with an NSIS installer.

## Reusable code in sibling repos

Grep these before implementing anything from scratch (but vendor via update scripts, never symlink):

- `lumi` (lumimux):
  - `src/libvt` — terminal escape sequence processing
  - `src/libcfg` — config files
  - `src/libutf8` — UTF-8 handling
  - `src/libcore` — misc
- `boris` — MUD telnet processing (server-side reference)
- `jondev/code` — snippets: `csv.[ch]`, `base64.[ch]`, `base26.c`, `base85.c`

## MUD list format

User-managed MUD lists must be shareable via GitHub gist / paste.net links as **flat CSV with a title schema**. Keep the format simple and human-editable.
