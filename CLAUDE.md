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
make windows-check  # cross-compile the GUI libs for Windows (mingw-w64)
make dist           # bundle the GUI toolkit into a versioned ZIP
```

`make windows-check` cross-compiles the GUI libraries (`birdie_gui` + `birdie_gui_gles_core`, built with the default built-in GL loader `BD_GL_LOADER=builtin`) with mingw-w64, validating the actual Windows target the loader exists for: `opengl32.dll` exports only GL 1.1, so the ES 3.0 entry points must be resolved at runtime (`bd_gl_load` via a getproc). It is a **compile+archive check only** (no `.exe`, no GL runtime, no import lib, since the loader defers `gl*` to runtime), so it needs no X11/GL dev libs, only the toolchain (`apt install gcc-mingw-w64-x86-64`). It catches Windows-only compile breaks the Linux build cannot. The Khronos GLES3 headers are vendored (`src/birdie-gui/thirdparty/khronos/`) because mingw ships `KHR/khrplatform.h` but no `GLES3/gl3.h`; the built-in loader's `GLES3/gl3.h` shim `#include_next`s them. It scopes the sub-make to the library archive, so ludica and the app are not cross-built. Override the toolchain with `make windows-check WIN_CC=i686-w64-mingw32-gcc`. The Linux gallery remains the loader's runtime exercise. See `doc/gl-loader.md`.

Output: `_out/<triplet>/bin/birdie` (e.g. `_out/x86_64-linux-gnu/bin/birdie`). Also builds ludica tools: `ludica-launcher`, `ludica-mcp`, `ludica-mcp-bridge`, `font2slug`.

`make test` is a top-level alias for the `test_gui` modular-make target (`test/module.mk`): a recording stub backend (`test/test_gui.c`) linked against the `birdie_gui_vt` terminal library (which drags in `birdie_gui`), run via its `TEST_TARGET` command. It needs no display, so it runs in CI; a failed check fails the build.

`make widget-test` builds the `birdie-gui-gallery` modular-make executable (`src/guitest/module.mk`): a windowed widget gallery on a raw X11/EGL/GLES backend, independent of ludica, exhibiting every widget. It pairs `birdie_gui` with the shared `birdie_gui_gles_core` library (`bd_backend_gles_core.c`, declared in `src/birdie-gui/module.mk`) plus this directory's windowing glue. Linux/X11 only and opt-in: the top-level `make widget-test` re-enters make with `WIDGET_TEST=1`, which adds `src/guitest` to `SUBDIRS`, so a plain `make` never builds it. The build stages the fonts into `$(BINDIR)` next to the binary, so it runs from any directory. birdie runs on ludica and the gallery on GLES, so both backends stay exercised.

`examples/` is a **separate modular-make project** (it carries its own copy of `GNUmakefile`) so the main build never depends on example-only libraries like SDL3. Build the examples with `cd examples && make`; they pull the toolkit sources from `../src`. The SDL3 example (`examples/sdl3/`) needs SDL3 (`pkg-config sdl3`); the examples build stages the toolkit's fonts next to the example binaries, so it runs from any directory. The embed example (`examples/embed/`) bakes the fonts into the binary via `.incbin` + `bd_asset_register`, needs only X11/EGL/GLES, and runs from any directory (`embed_example --check` verifies the embedding headless).

`make dist` stages the full birdie-gui toolkit into `_out/<triplet>/birdie-gui-$(GUI_VERSION).zip` as a **self-contained copy of `src/birdie-gui/`**: the same flat layout (sources + public headers together, its own `thirdparty/stb/`, the `bd_vt/` terminal sublibrary, assets) shipping the **same `module.mk` the in-tree build uses** (no separate dist-module.mk to drift). The reference ludica/SDL3/GLES-core backends ship as loose source to compile into your own target; the raw X11/EGL/GLES backend + gallery ship under `backend-gles/`. Override the version with `make dist GUI_VERSION=x.y.z` (default `0.8.2`). The bundle compiles standalone (a consumer adds the dir to SUBDIRS and links `birdie_gui`; add `bd_vt/` + `birdie_gui_vt` for a terminal), each backend still needs its own host (ludica / SDL3 / X11+EGL), and the README has the gallery build command. What makes this work: `src/birdie-gui/module.mk` is host-neutral and declares only `birdie_gui` — the ludica/GLES-core backend libraries are declared in the **top-level** `module.mk` instead, because `all` builds `compile_commands.json` over every declared library's objects, so declaring a backend beside the toolkit would force a bundle consumer to compile it (and satisfy its host deps). The dist staging is a packaging recipe (copy + zip), which has no modular-make primitive, so it stays a custom `.PHONY` rule in the top-level `module.mk` alongside the `test` / `widget-test` aliases; everything that compiles a component is a proper `module.mk` target.

Build system is [modular-make](https://github.com/OrangeTide/modular-make). Each directory has a `module.mk`; the top-level one controls which SUBDIRS are pulled in.

## Source layout

- `src/birdie-gui/` — the birdie-gui toolkit (its own directory, built as the `birdie_gui` library): the widget core (`widget.{c,h}`, `widget_ext.h`), renderer (`bd_draw.{c,h}`), UTF-8 codec (`bd_utf8.{c,h}`), backend interface (`bd_backend.h`), theme (`bd_theme.h`), the embedded-asset registry (`bd_asset.{c,h}`, lets fonts and PNGs load from in-binary blobs instead of disk), the extension widgets (`bd_widget_value.*`, `bd_widget_explorer.*`, ...), the reference backends (`bd_backend_ludica.*`, `bd_backend_sdl3.*`; the `birdie_gui_ludica` / `birdie_gui_gles_core` *libraries* are declared in the top-level `module.mk`, not here, so this dir's `module.mk` stays host-neutral and ships verbatim as the bundle), its own vendored `thirdparty/stb/`, assets, and the `module.mk` / README / LICENSE that ship in the bundle
- `src/birdie-gui/bd_vt/` — the terminal, built as the separate `birdie_gui_vt` library (a terminal-free UI links `birdie_gui` alone): the embedded VT escape-sequence engine (`vt_*.{c,h}`, `rune_width.*`, `width_tables.c`; adopted from lumi's libvt, cord cut, uses `bd_utf8`) plus the `BD_TERMINAL` widget (`bd_widget_vt.*`)
- `src/birdie/` — the MUD-client app only: `main.c` plus networking, telnet, triggers, profiles, and the scripting VM (`bd_net.*`, `bd_session.*`, `bd_vm.*`, ...); links the toolkit via `birdie_gui_ludica`
- `src/guitest/` — standalone widget gallery (`widget_test.c`) + the raw X11/EGL/GLES backend (`window.h`, `x11_window.c`, `bd_backend_gles.*`); its `module.mk` declares the opt-in `birdie-gui-gallery` executable, built by `make widget-test`
- `examples/` — separate modular-make project (own `GNUmakefile`) for host examples; `examples/sdl3/` hosts the toolkit on an SDL3 window + GLES3 context (`sdl3_example.c`, self-contained backend + demo UI); `examples/embed/` is the embedded-assets reference (`embed_example.c` + `embed_assets.S`, `.incbin` blobs served via `bd_asset`, GLES backend). Build with `cd examples && make`.
- `src/birdie-gui/assets/` — chrome TTF (DejaVuSans) only; the terminal font, the pushpins, and the WM lock-button padlock are 1-bit bitmaps compiled into the toolkit (`bd_embed_font.h` from unscii, `bd_embed_pushpin.h` from the `pushpin_*.xbm` sources, `bd_embed_padlock.h` from the `padlock_*_14.xbm` sources; drawn tinted via `bd_draw_pushpin` / `bd_draw_padlock`)
- `src/thirdparty/ludica/` — vendored ludica (rendering, input, audio, networking)
- `src/birdie-gui/thirdparty/stb/` — vendored stb_truetype + stb_image (birdie-gui's only third-party dep; kept inside the toolkit dir so it ships self-contained in the bundle)
- `src/birdie-gui/thirdparty/khronos/` — vendored Khronos GLES3 headers (`GLES3/gl3.h`, `gl3platform.h`, `KHR/khrplatform.h`) that the built-in GL loader's shim needs to compile where the system ships none (Windows); ships in the bundle. Re-vendor with `scripts/update-khronos.sh`
- `test/test_gui.c` — headless toolkit test (recording stub backend); its `module.mk` declares the `test_gui` executable + `TEST_TARGET`, run by `make test`
- `module.mk` — top-level build wiring (SUBDIRS, shader-gen rule, the `dist` packaging recipe, and the `test` / `widget-test` aliases)
- `GNUmakefile` — modular-make entry point (fetched, not hand-written)
- `scripts/update-ludica.sh` — re-vendor ludica from upstream git
- `scripts/update-gnumakefile.sh` — fetch GNUmakefile from modular-make
- `scripts/update-khronos.sh` — re-vendor the Khronos GLES3 headers from the registries
- `doc/` — design documents (network, terminal, GUI, triggers, etc.)
- `concept.md` — original requirements and open design questions

## Vendoring

Birdie vendors its dependencies rather than linking to sibling repos. **Never symlink between projects.** Each dependency has an update script under `scripts/`:

- `scripts/update-ludica.sh [ref]` — shallow-clones ludica from GitHub, copies into `src/thirdparty/ludica/`, and merges ludica's Claude Code skills into `.claude/skills/`. Default ref: `main`.
- `scripts/update-gnumakefile.sh [ref]` — fetches GNUmakefile from modular-make GitHub releases.
- `scripts/update-khronos.sh [gl_ref] [egl_ref]` — fetches the Khronos GLES3 headers (`GLES3/gl3.h` + `gl3platform.h` from OpenGL-Registry, `KHR/khrplatform.h` from EGL-Registry) into `src/birdie-gui/thirdparty/khronos/`. Two separate repos: neither publishes tags, so an immutable pin is a commit SHA and you must give both refs (a GL SHA doesn't resolve in the EGL repo). Default both: `main`.

Provenance is recorded in each dependency's `UPSTREAM` file (`src/thirdparty/ludica/UPSTREAM`, `src/birdie-gui/thirdparty/khronos/UPSTREAM`).

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

`src/birdie/main.c` is a MUD-client shell on the toolkit: a menu bar, a `BD_TERMINAL` output pane, a `BD_INPUT_LINE`, buttons, and a status bar, running on ludica. The client plumbing behind it is substantial and wired through `bd_session`: TCP networking with async resolve (`bd_net`), telnet option negotiation (`bd_telopt`), MXP plus GMCP/MSDP package handling (`bd_mxp`), a trigger engine (actions, aliases, prompt, gmcp, timers, events in `bd_trigger`) fed from the session's line pipeline, profile storage with title-schema CSV import/export (`bd_profile`), session log sinks (`bd_log`), and a scripting VM (`bd_vm`, with a Lua backend in `bd_vm_lua` and a null fallback). What is still thin is the UI that exposes all this: the profile/trigger/settings dialogs and surrounding UX are not built out, and there are no automated tests for the client core yet.

Built: chrome widgets (frame/panel/label/button/menu+pushpins/input-line/terminal), the full v1.0 widget set (`BD_TEXT_FIELD`/`BD_TEXT_AREA`/`BD_LIST`/`BD_SCROLLBAR`/`BD_NOTICE`/`BD_TAB_BAR`), value widgets (slider, knob, toggle, wheel, jog, X-Y pad; all created via `bd_*_desc` structs), form controls for dialogs (`BD_CHECKBOX`, radio group, numeric spinner in `bd_widget_form.*`; a `BD_COMBO` drop-down in `bd_widget_combo.*`), an explorer/icon-browser widget, a `BD_TREE` expand/collapse hierarchy list, a `BD_TABLE` sortable multi-column data grid, a multi-state `BD_INDICATOR` (LED/annunciator), an `bd_actionbar`, a shared icon cell + standalone `BD_ICON` (app-launcher/desktop icon; the dock/actionbar/inventory render and drag through it), a rich-text editor widget (with an autocomplete popup), a pressure-sensitive sketch pad (`bd_sketch`), a tab-view container, a NeXTSTEP-style dock, an embedded-WM canvas (`BD_MANAGED_CANVAS`: floating frames + a scoped dock inside a widget, giving primitive MDI on any backend; a per-canvas minimize target selects where minimized frames go via `bd_managed_canvas_set_minimize`: WM desktop icons, an attached dock, or none), 0..1 meters (`bd_meter`: bar/VU/magic-eye/pie/liquid-vial styles with color zones, peak-hold, and ballistics) and a progress bar (`bd_progress`, with a `glass` liquid-tube style matching the vial), a scrolling time-series strip chart (`bd_chart`, xload/system-monitor style), a top-most overlay primitive (`bd_overlay_*`) for pop-ups that escape their widget rect (the combo list uses it), multiple native windows (GLES backend), Tab focus traversal, clipboard, IME/compose, key-up/repeat, multitouch, and pen/tablet input. The v0.3 roadmap is complete on the GLES backend. Deferred: explorer list/details view, cross-line selection in multiline/editor, and Win32/Wayland/macOS backends (tracked in `doc/gui.md`).

The toolkit is at `GUI_VERSION` 0.8.2 (module.mk) heading to a 0.9 release that deliberately batches breaking public-API changes while breakage is still cheap. Landed 0.9 breaks so far: the value-widget `bd_*_desc` create signatures, the `_selected()`/`_select()` selection accessors (from `_active`/`_set_active`), and the unified `bd_managed_canvas_set_minimize`. Prefer API correctness over compatibility until 0.9 is cut.

## Target platforms

Portable across Linux (x86-64, aarch64) and Windows (x86-64); macOS is a stretch goal. Windows releases ship with an NSIS installer.

## Reusable code in sibling repos

Grep these before implementing anything from scratch (but vendor via update scripts, never symlink):

- `lumi` (lumimux):
  - `src/libvt` — terminal escape sequence processing (already adopted into birdie as `src/birdie-gui/bd_vt/`; the cord is cut, so re-vendor only to cherry-pick a specific upstream fix)
  - `src/libcfg` — config files
  - `src/libutf8` — UTF-8 handling
  - `src/libcore` — misc
- `boris` — MUD telnet processing (server-side reference)
- `jondev/code` — snippets: `csv.[ch]`, `base64.[ch]`, `base26.c`, `base85.c`

## MUD list format

User-managed MUD lists must be shareable via GitHub gist / paste.net links as **flat CSV with a title schema**. Keep the format simple and human-editable.
