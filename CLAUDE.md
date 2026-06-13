# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

Birdie is a desktop MUD client written in C, using the ludica rendering framework for its GUI. Design docs live in `doc/` and `concept.md`.

## Build

```sh
make                # debug build
make RELEASE=1      # optimized (-O2, LTO)
make clean
make test           # headless GUI toolkit test (no window/ludica/X11)
make dist           # bundle the GUI toolkit into a versioned ZIP
```

Output: `_out/<triplet>/bin/birdie` (e.g. `_out/x86_64-linux-gnu/bin/birdie`). Also builds ludica tools: `ludica-launcher`, `ludica-mcp`, `ludica-mcp-bridge`, `font2slug`.

`make test` compiles `test/test_gui.c` (a recording stub backend) with the toolkit sources and runs it, linking only libvt. It needs no display, so it runs in CI; a failed check fails the build.

`make dist` stages the GUI toolkit (public headers, `widget.c`, the reference ludica backend, the VT extension, runtime assets, README) into `_out/<triplet>/birdie-gui-$(GUI_VERSION).zip`. Override the version with `make dist GUI_VERSION=x.y.z` (default `0.1.0`). Both targets live in the top-level `module.mk`, so `scripts/update-gnumakefile.sh` won't clobber them.

Build system is [modular-make](https://github.com/OrangeTide/modular-make). Each directory has a `module.mk`; the top-level one controls which SUBDIRS are pulled in.

## Source layout

- `src/birdie/` — birdie application (`main.c`, `module.mk`)
- `src/birdie/assets/` — CP437 font atlases (8x16, 8x8)
- `src/thirdparty/ludica/` — vendored ludica (rendering, input, audio, networking)
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

## Current bootstrap state

`src/birdie/main.c` is a minimal CP437 terminal renderer: loads a glyph atlas, fills the window with blank cells, draws two status lines, Escape quits. Text rendering treats bytes as CP437 codepoints (not UTF-8). The 16-color CGA/EGA palette is hardcoded.

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
