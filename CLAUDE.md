# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project status

Birdie is a **planned desktop MUD client**. The repository currently contains only `concept.md` — no source code, build system, or tests exist yet. Implementation language (C vs Delphi) and GUI toolkit are still open questions (see `concept.md`).

Before writing new code, read `concept.md` for the current requirements and open design questions.

## Target platforms

Portable across Linux (x86-64, aarch64) and Windows (x86-64); macOS is a stretch goal. Windows releases are intended to ship with an NSIS installer. Keep portability in mind from the first line of code.

## Reusable code in sibling repos

The concept doc explicitly points to existing code in other local repos that should be reused rather than rewritten. **Grep these before implementing anything from scratch:**

- `/home/jon/DEVEL/modular-make` — starting point for the `GNUmakefile`
- `/home/jon/DEVEL/lumi` (lumimux) — substantial reusable libraries:
  - `src/libvt` — terminal escape sequence processing
  - `src/libiox` — networking
  - `src/libcfg` — config files
  - `src/libutf8` — UTF-8 handling
  - `src/libcore` — misc
- `/home/jon/boris` — MUD telnet processing (server-side, but useful reference)
- `/home/jon/DEVEL/rust/webrpg` — complete windowed GUI in Wasm+JS (reference for browser-based UI option)
- `/home/jon/jondev/code` — snippets including `csv.[ch]`, `base64.[ch]`, `base26.c`, `base85.c`. Grep this directory before writing new utility code.

## MUD list format

User-managed MUD lists must be shareable via GitHub gist / paste.net links as **flat CSV with a title schema**. Keep the format simple and human-editable.
