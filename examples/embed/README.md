# birdie-gui with assets embedded in the binary

A small example host that ships as a **single self-contained executable**: the
chrome font, the pushpin sprites, the terminal's CP437 atlas, and the monospace
family the body editor renders (regular + bold + italic) are all compiled into
the binary instead of read from disk at runtime. It is the worked reference for
`bd_asset.h`, the backend-neutral embedded-asset registry.

It runs on the raw X11/EGL/GLES backend (`src/guitest/`), the same one the
toolkit gallery uses, so it needs no ludica and no SDL3, only X11 + EGL +
GLESv2.

The window shows a menu bar (the pinnable menu draws the embedded pushpin), a
label and a terminal (each drawing an embedded font/atlas), and a **rich-text
editor** whose bold and italic spans pull the embedded `mono-bold` and
`mono-italic` faces -- so every face the UI touches comes from a baked-in blob.

## How it works

Three pieces:

1. **`embed_assets.S`** bakes each asset into `.rodata` with the GNU assembler's
   `.incbin`. The source paths come from `-D` macros the makefile sets
   (relative). Each blob is bracketed by a `sym` / `sym_end` label pair. The
   `.incbin` path is a build-time detail; it never reaches the binary.

2. **`embed_example.c`** pairs each symbol with the **generic id** the toolkit
   requests that asset by and, before `bd_gui_init()`, calls
   `bd_asset_register_data(id, data, len)` for each. That is the whole
   integration:

   ```c
   bd_asset_register_data(BD_ASSET_FONT_REGULAR,  emb_font_ui,    emb_font_ui_end    - emb_font_ui);
   bd_asset_register_data(BD_ASSET_PUSHPIN_OUT,   emb_pin_out,    emb_pin_out_end    - emb_pin_out);
   bd_asset_register_data(BD_ASSET_TERMINAL_FONT, emb_term_atlas, emb_term_atlas_end - emb_term_atlas);
   bd_asset_register_data(BD_ASSET_FONT_MONO,      emb_font_mono,      ...);  /* editor body */
   bd_asset_register_data(BD_ASSET_FONT_MONO_BOLD, emb_font_mono_bold, ...);  /* bold spans   */
   /* ...and BD_ASSET_FONT_MONO_ITALIC */
   ```

   Note the regular font is registered as `BD_ASSET_FONT_REGULAR` -- *the regular
   UI font*, an identity -- not as `"DejaVuSans.ttf"`. Any `.ttf`/`.otf` drops in
   the same way; the id never ties you to a font family. A full styled family is
   just one `bd_asset_register_data` per face, each under its own id -- the editor
   here embeds the three mono faces it renders.

3. The toolkit does the rest. It resolves each asset by id: a registered source
   (here, embedded data) wins over the built-in default file. `bd_draw.c`
   resolves the font faces and `bd_asset_texture` resolves the pushpins and
   atlas. Nothing is read from disk; anything left unregistered would fall back
   to a file.

The registered blobs are **borrowed** (a `.rodata` blob satisfies "must outlive
use"), so there is nothing to free. Register only the faces you draw: the chrome
uses the regular proportional face, the editor the three mono faces, and the
terminal its own atlas -- unregistered faces would fall back to the regular one.
Each id can equally take a file path (`bd_asset_register_file`), e.g. to load a
user-chosen font at runtime.

## Build paths and the binary

No **absolute** build path leaks into this example: the `.incbin` source paths
are passed relative and never reach the output anyway (`.incbin` copies file
*contents*). The one rule to remember when you embed in your own project: never
point `BD_ASSET_*` or an `.incbin` path at an `$(abspath ...)`, which would bake
the build machine's directory layout and username into every shipped copy.

The registry keys are the fixed `BD_ASSET_*` id strings (short, generic), so
they leak nothing. The only paths that appear in `strings embed_example` are the
toolkit's **default fallback** file paths (`src/birdie-gui/assets/...`), which
this build never reads because every asset is registered by id. They show up
because this example shares its compiled toolkit objects (`widget.o`,
`bd_draw.o`, ...) with the SDL3 example in the same modular-make project, so it
cannot recompile them with its own macro overrides. A real consumer compiles
birdie-gui as its own single executable and overrides those default paths to
short strings (`-DBD_ASSET_GUI_FONT='"font.ttf"'`, and the same for the variants,
`BD_ASSET_TERM_FONT`, and `BD_ASSET_PIN_*`), leaving no `src/birdie-gui/assets/...`
in the binary at all. See the toolkit README's "Keeping build paths out of the
binary".

## Build and run

```sh
cd examples && make embed_example
./_out/<triplet>/bin/embed_example            # opens a window, all assets embedded
./_out/<triplet>/bin/embed_example --check     # headless: verify the embedding, then exit
```

Run it **from any directory** -- unlike the rest of the toolkit's hosts, this
one depends on no `BD_ASSET_*` files being reachable. `--check` needs no display:
it confirms every blob is registered and its baked length matches the original
asset, printing a line per asset. It reads the original files only to compare
sizes; the windowed mode reads nothing.

## Why the raw GLES backend

The point is to show embedding, not a window system, so the example uses the
smallest dependency-free backend. The same three-line registration works
identically on the ludica and SDL3 backends: every backend's `load_texture`
routes through `bd_asset_lookup`, so no per-backend change is needed.

## License

Made by a machine. PUBLIC DOMAIN (CC0-1.0)
