# Build system

GNU make, using the [modular-make](https://github.com/OrangeTide/modular-make)
conventions. Same system `ludica` uses. GNU Make 4.0+ required (uses
`$(file)`). The `GNUmakefile` is fetched, not hand-written
(`scripts/update-gnumakefile.sh`); the project's build data lives in the
`module.mk` files.

The quick command reference is in the top-level `CLAUDE.md` ("## Build");
this document is the design-level overview.

## Layout

    GNUmakefile                    # modular-make entry point (fetched)
    module.mk                      # top-level: SUBDIRS, shader rule, dist recipe,
                                   #   test/widget-test aliases
    src/
        birdie/                    # the MUD-client app + its module.mk
            module.mk
        birdie-gui/                # the birdie-gui toolkit (library) + backends
            module.mk
        guitest/                   # standalone widget gallery (opt-in)
            module.mk
        libvt/                     # terminal escape-sequence engine (library)
            module.mk
        thirdparty/
            ludica/                # rendering/input/audio/net (bundles libiox)
            mbedtls/
            miniz/
            lua/
            lpeg/
            stb/                   # stb_truetype + stb_image single-headers
    test/
        module.mk                  # headless toolkit test (test_gui)
    scripts/
        update-<name>.sh           # one per vendored dependency
    _build/<triplet>/              # .o, .dep, and .a archives (generated)
    _out/<triplet>/bin/            # binaries (generated)

There is a top-level `module.mk` (not `src/module.mk`), so it is responsible
for adding the source directories to `SUBDIRS`. Each directory with buildable
code carries a `module.mk` that appends to `EXECUTABLES` / `LIBRARIES` and sets
per-target `<name>_DIR` / `<name>_SRCS` / `<name>_LIBS` / flags. Libraries
declared: `birdie_gui`, `birdie_gui_ludica`, `birdie_gui_gles_core`, `bd_vt`
(libvt), plus the vendored ludica/mbedtls/miniz/lua/lpeg libraries. See
modular-make's header comment for the full variable set.

The top-level `module.mk` deliberately bypasses ludica's own `src/module.mk`
(which pulls in samples/imgui and assumes `tools/glsl2h` at the make root) and
SUBDIRs into just the ludica library dirs and tools it needs, redeclaring the
shader-to-C rule with the vendored `glsl2h` path.

## Targets

    make              # default: debug build of the app + ludica tools + test_gui
    make RELEASE=1    # optimized: -O2, LTO, -DNDEBUG
    make clean        # remove generated objects, archives, and binaries
    make test         # build + run the headless GUI toolkit test (no display)
    make widget-test  # build the opt-in X11/EGL/GLES widget gallery
    make dist         # stage the birdie-gui toolkit into a versioned source ZIP

`make` also (re)generates `compile_commands.json` for clangd. `make test` and
`make widget-test` are thin aliases in the top-level `module.mk` for the
`test_gui` and `birdie-gui-gallery` modular-make targets; the gallery is
opt-in (loaded into `SUBDIRS` only under `WIDGET_TEST`) because it needs X11 +
EGL + GLESv2 and is Linux-only. Cross-builds set `CC` to a cross-toolchain; the
triplet comes from `$(CC) -dumpmachine` so outputs don't clobber native ones.

The `examples/` tree is a **separate** modular-make project (its own
`GNUmakefile`) so the main build never depends on example-only libraries like
SDL3. Build it with `cd examples && make`.

## Platforms

- **Linux x86-64 / aarch64** — system gcc or clang.
- **Windows x86-64** — mingw-w64 cross from Linux
  (`CC=x86_64-w64-mingw32-gcc`). Releases ship with an NSIS installer.
- **macOS** — stretch goal.

## Third-party libraries

Dependencies are vendored, not linked to sibling repos, and never symlinked.
ludica, mbedTLS, miniz, Lua, and LPeg live under `src/thirdparty/<name>/`;
libvt lives at `src/libvt/`; libiox ships inside ludica. Each vendored
dependency has an update script under `scripts/` (`update-ludica.sh`,
`update-lua.sh`, `update-mbedtls.sh`, `update-miniz.sh`) and the GNUmakefile
itself is refreshed with `update-gnumakefile.sh`. See `doc/vendoring.md` for
the policy and provenance tracking.

The UTF-8 codec (`src/birdie-gui/bd_utf8.c`) and the embedded fallback font
(`bd_fallback_font.h`) are adopted as first-class birdie-gui code, not tracked
as external dependencies.

## Release artifacts

- **birdie-gui toolkit** — `make dist` produces
  `_out/<triplet>/birdie-gui-$(GUI_VERSION).zip`, a self-contained source
  bundle (public headers, implementation, reference backends, libvt, stb, and
  assets). The GitHub release workflow publishes it.
- **Linux app** — tarball of `_out/<triplet>/bin/birdie` plus its runtime
  assets.
- **Windows app** — NSIS installer bundling the `.exe`, GLES loader, CA list,
  default fonts, and sample profiles.
