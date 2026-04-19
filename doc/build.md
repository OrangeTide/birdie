# Build system

GNU make, using the `modular-make` conventions from
`~/DEVEL/modular-make/GNUmakefile`. Same system `ludica` uses. GNU
Make 4.0+ required (uses `$(file)`).

## Layout

    GNUmakefile                    # copied/adapted from modular-make
    src/
        module.mk                  # root module, lists SUBDIRS
        birdie/                    # main executable
        core/                      # net → parser → triggers → log
        gui/                       # ludica front-end
        thirdparty/
            ludica/
            libvt/
            libutf8/
            libiox/
            mth/
            mbedtls/
            lua/
            lpeg/
    scripts/
        update-<name>.sh           # one per vendored lib
    _build/<triplet>/              # .o and .dep (generated)
    _out/<triplet>/bin/            # binaries (generated)

Each directory with buildable code carries a `module.mk` that appends
to `EXECUTABLES` / `LIBRARIES` / `SHARED_LIBS` and sets per-target
`<name>_DIR` / `<name>_SRCS` / flags. See modular-make's header for
the full variable set.

## Targets

    make              # default: native build
    make debug        # -O0 -g
    make release      # -O2 -g
    make test         # run tests/
    make clean
    make distclean    # also drops _build / _out

Cross-builds set `CC` to a cross-toolchain; the triplet comes from
`$(CC) -dumpmachine` so outputs don't clobber native artifacts.

## Platforms

- **Linux x86-64 / aarch64** — system gcc or clang.
- **Windows x86-64** — mingw-w64 cross from Linux
  (`CC=x86_64-w64-mingw32-gcc`). NSIS installer built from
  `packaging/win/birdie.nsi` in a separate rule.
- **macOS** — stretch goal; native clang, ANGLE for GLES2.
- **Raspberry Pi (aarch64 / armhf)** — native build on the Pi, or
  cross from Linux.

## Third-party libraries

All vendored under `src/thirdparty/<name>/` per `doc/vendoring.md`.
Each has its own `module.mk`; the root pulls them in via `SUBDIRS`.
No system package dependencies for v1.0 beyond libc and OpenGL-ES /
ANGLE loader — mbedTLS, Lua, LPeg, libvt, libiox, MTH all build from
vendored source.

## Release artifacts

- Linux: tarball of `_out/<triplet>/bin/birdie` plus `share/birdie/`
  assets; optional AppImage later.
- Windows: NSIS installer bundling the `.exe`, ANGLE DLLs, CA list,
  default fonts, and sample profiles.

## Open questions

- Do we ship a `compile_commands.json` generator rule (modular-make
  has one)? Lean yes — useful for clangd and for Claude Code.
- Signed Windows installer: deferred until a release channel exists.
