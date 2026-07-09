# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Birdie is a desktop MUD client written in C. Its GUI is **birdie-gui**, a self-contained retained-mode widget toolkit drawn through a backend-neutral GPU interface; ludica is the default backend (rendering, input, audio, networking). Design docs live in `doc/` and `concept.md`.

## AI Policy

- license files created by AI with: Made by a machine. PUBLIC DOMAIN (CC0-1.0)
- don't offer to `git push`, I can do that myself.
- some files are local and not committed, this is noted in `.git/info/exclude`

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

`make test` is a top-level alias for the `test_gui` modular-make target (`test/module.mk`): a recording stub backend (`test/test_gui.c`) linked against the `birdie_gui` library (which drags in libvt), run via its `TEST_TARGET` command. It needs no display, so it runs in CI; a failed check fails the build.

`make widget-test` builds the `birdie-gui-gallery` modular-make executable (`src/guitest/module.mk`): a windowed widget gallery on a raw X11/EGL/GLES backend, independent of ludica, exhibiting every widget. It pairs `birdie_gui` with the shared `birdie_gui_gles_core` library (`bd_backend_gles_core.c`, declared in `src/birdie-gui/module.mk`) plus this directory's windowing glue. Linux/X11 only and opt-in: the top-level `make widget-test` re-enters make with `WIDGET_TEST=1`, which adds `src/guitest` to `SUBDIRS`, so a plain `make` never builds it. Run the binary from the repo root so the default `BD_ASSET_*` paths resolve. birdie runs on ludica and the gallery on GLES, so both backends stay exercised.

`examples/` is a **separate modular-make project** (it carries its own copy of `GNUmakefile`) so the main build never depends on example-only libraries like SDL3. Build the examples with `cd examples && make`; they pull the toolkit sources from `../src`. The SDL3 example (`examples/sdl3/`) needs SDL3 (`pkg-config sdl3`); run its binary from the repo root so `BD_ASSET_*` paths resolve. The embed example (`examples/embed/`) bakes the fonts/PNGs into the binary via `.incbin` + `bd_asset_register`, needs only X11/EGL/GLES, and runs from any directory (`embed_example --check` verifies the embedding headless).

`make dist` stages the full birdie-gui toolkit into `_out/<triplet>/birdie-gui-$(GUI_VERSION).zip`: public headers (`include/`), the implementation + reference ludica and SDL3 backends (`src/`), the raw X11/EGL/GLES backend + standalone gallery (`backend-gles/`), vendored libvt (`libvt/`, so the terminal widget compiles out of the box), vendored stb single-headers (`thirdparty/stb/`), a self-contained `module.mk` (declares the `birdie_gui` + `bd_vt` libraries), and runtime assets (chrome TTF + license, CP437 atlas, pushpins). Override the version with `make dist GUI_VERSION=x.y.z` (default `0.5.0`). The bundle compiles standalone; each backend still needs its own host (ludica / SDL3 / X11+EGL), and the README has the gallery build command. The dist file list lives in the top-level `module.mk`; the bundle's own `module.mk` is `src/birdie-gui/dist-module.mk`. `dist` is a packaging recipe (staging + zip), which has no modular-make primitive, so it stays a custom `.PHONY` rule in the top-level `module.mk` alongside the `test` / `widget-test` aliases; everything that compiles a component is a proper `module.mk` target.

Build system is [modular-make](https://github.com/OrangeTide/modular-make). Each directory has a `module.mk`; the top-level one controls which SUBDIRS are pulled in.

## Source layout

- `src/birdie-gui/` — the birdie-gui toolkit (its own directory, built as the `birdie_gui` library): the widget core (`widget.{c,h}`, `widget_ext.h`), renderer (`bd_draw.{c,h}`), backend interface (`bd_backend.h`), theme (`bd_theme.h`), the embedded-asset registry (`bd_asset.{c,h}`, lets fonts and PNGs load from in-binary blobs instead of disk), the extension widgets (`bd_widget_vt.*` terminal, `bd_widget_value.*`, `bd_widget_explorer.*`, ...), the reference backends (`bd_backend_ludica.*` as `birdie_gui_ludica`, `bd_backend_sdl3.*`), assets, and the `dist-module.mk` / README / LICENSE that ship in the bundle
- `src/birdie/` — the MUD-client app only: `main.c` plus networking, telnet, triggers, profiles, and the scripting VM (`bd_net.*`, `bd_session.*`, `bd_vm.*`, ...); links the toolkit via `birdie_gui_ludica`
- `src/guitest/` — standalone widget gallery (`widget_test.c`) + the raw X11/EGL/GLES backend (`window.h`, `x11_window.c`, `bd_backend_gles.*`); its `module.mk` declares the opt-in `birdie-gui-gallery` executable, built by `make widget-test`
- `examples/` — separate modular-make project (own `GNUmakefile`) for host examples; `examples/sdl3/` hosts the toolkit on an SDL3 window + GLES3 context (`sdl3_example.c`, self-contained backend + demo UI); `examples/embed/` is the embedded-assets reference (`embed_example.c` + `embed_assets.S`, `.incbin` blobs served via `bd_asset`, GLES backend). Build with `cd examples && make`.
- `src/birdie-gui/assets/` — chrome TTF (DejaVuSans), CP437 terminal font atlases, pushpin sprites
- `src/thirdparty/ludica/` — vendored ludica (rendering, input, audio, networking)
- `src/thirdparty/stb/` — vendored stb_truetype + stb_image
- `test/test_gui.c` — headless toolkit test (recording stub backend); its `module.mk` declares the `test_gui` executable + `TEST_TARGET`, run by `make test`
- `module.mk` — top-level build wiring (SUBDIRS, shader-gen rule, the `dist` packaging recipe, and the `test` / `widget-test` aliases)
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

The GUI toolkit (now packaged as **birdie-gui**, in its own `src/birdie-gui/` directory) is the most developed part of the repo. It is a retained-mode widget layer drawn through a backend-neutral GPU interface (`bd_backend`), with three backends: the reference ludica binding (`bd_backend_ludica.c`), an SDL3 binding (`bd_backend_sdl3.c`), and a raw X11/EGL/GLES binding (`src/guitest/`). The renderer is `bd_draw.c` (shader + dynamic quad batch + stb_truetype text). See `doc/gui.md` for the design and an Implementation status section.

`src/birdie/main.c` is a thin MUD-client shell built on the toolkit: a frame with a menu bar, a `BD_TERMINAL` output pane, a `BD_INPUT_LINE`, buttons, and a status bar, running on ludica. Networking, triggers, and the real MUD logic are not wired up yet.

Built: chrome widgets (frame/panel/label/button/menu+pushpins/input-line/terminal), the full v1.0 widget set (`BD_TEXT`/`BD_MULTILINE`/`BD_LIST`/`BD_SCROLLBAR`/`BD_NOTICE`/`BD_TAB_BAR`), value widgets (slider, knob, toggle, wheel, jog, X-Y pad), an explorer/icon-browser widget, a rich-text editor widget, a pressure-sensitive drawing canvas, multiple native windows (GLES backend), Tab focus traversal, clipboard, IME/compose, key-up/repeat, multitouch, and pen/tablet input. The v0.3 roadmap is complete on the GLES backend. Deferred: explorer list/details view, cross-line selection in multiline/editor, and Win32/Wayland/macOS backends (tracked in `doc/gui.md`).

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
