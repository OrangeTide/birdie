# GUI

Birdie implements a **thin retained-mode widget layer** of its own, drawing
through a small backend abstraction (a GPU interface implemented on `ludica`
or on raw OpenGL ES; see Rendering). No third-party widget toolkit (no ImGui,
no Nuklear, no GTK/FLTK). The API style follows XView: objects are created
with an attribute-list constructor, queried and mutated through get/set calls.

The toolkit has outgrown "birdie's GUI": it is packaged as **birdie-gui**, a
reusable library (`make dist`), with birdie as one consumer. See the
Implementation status section below for what is built today.

> **Looking for the API?** [gui-reference.md](gui-reference.md) is the
> scannable catalog of every widget type and public function, grouped by
> header. This document covers the design rationale and roadmap.

## Why retained-mode, not immediate-mode

Two requirements rule against immediate-mode specifically for this app:

1. **Accessibility seam.** `doc/wishlist.md` commits to keeping the core
   independent of the GUI so a screen-reader front-end can be added later.
   Screen readers consume a *logical tree* of widgets exposed through
   AT-SPI (Linux) and UIA (Windows). Immediate-mode has no such tree —
   the UI is recomputed from scratch each frame. Retained-mode gets the
   tree for free and makes the accessibility front-end a new renderer
   over the same widget tree rather than a rewrite.

2. **Low-end targets.** Raspberry Pi Zero and Pi 3 are real targets
   (they're the reason for the GLES2-only decision). Birdie chrome is
   overwhelmingly static — the MUD list, prefs dialog, tab bar, menu bar
   change on user action, not per-frame. Immediate-mode redraws all of
   that at 60 Hz regardless. Retained-mode redraws dirty regions only
   and can drop to 0 Hz when idle.

The third-party C option (Nuklear) is immediate-mode, so it loses on both
points. ImGui is additionally C++, which contradicts the C decision.

## API shape: XView-style attribute lists — with portability fixes

Widget construction uses an attribute-value list terminated by a typed
sentinel, **not** `NULL`:

    bd_id frame = bd_create(BD_NONE, BD_FRAME,
        BD_LABEL_S,  "Birdie",
        BD_WIDTH_I,  960,
        BD_HEIGHT_I, 600,
        BD_END);

    bd_id panel = bd_create(frame, BD_PANEL, BD_END);

    bd_id btn = bd_create(panel, BD_BUTTON,
        BD_LABEL_S,      "Connect",
        BD_ON_CLICK_F,   on_connect,
        BD_ON_CLICK_P,   profile,
        BD_END);

    bd_set(btn, BD_LABEL_S, "Disconnect", BD_END);
    const char *label = bd_get_s(btn, BD_LABEL_S);

Prior art for this shape:

- XView (`xv_create`, `xv_set`, `xv_get`) — the original.
- `libquaoar` (`~/research/programmable-display-protocols/demo/libquaoar.h`)
  — Jon's own C distillation. Birdie's widget vocabulary is a superset.

Widgets are identified by opaque **`bd_id`** integers, not pointers. This
keeps the event-handling story simple (integers are trivially safe to
compare, stash in Lua, and serialize into NDJSON log notes) and leaves
pointer lifetime a purely internal concern.

### Portability: avoiding XView's varargs trap

Classic XView attribute lists broke in subtle ways once 32-bit and 64-bit
systems started sharing headers (and they will share headers on a
Raspberry Pi running both armhf and aarch64 userlands side-by-side). The
failure modes:

1. The `NULL` terminator. `NULL` is sometimes `(void*)0`, sometimes `0L`,
   sometimes plain `0`. Varargs readers pulling a pointer-width value when
   the caller passed an `int`-width `0` read garbage on LP64.
2. Attribute values typed inconsistently. An attribute whose value is
   sometimes an `int` and sometimes a pointer (XView did this) forces the
   reader to guess the width. Default argument promotion makes this worse
   — `int` promotes to `int`, but `short` / `char` enums don't promote to
   pointer width.
3. Bit-flag attributes passed as untyped integer literals were sometimes
   read as 32-bit, sometimes 64-bit, depending on compiler and platform.

Rules we adopt to avoid all three:

- **Sentinel is typed, not `NULL`.** `BD_END` is a distinct enum value
  (e.g. `0`) read as the same type as every other attribute ID.
- **Type is encoded in the attribute ID suffix**, so the reader never
  guesses. Suffixes:
  - `_I` — `int` (promoted to `int` in varargs)
  - `_U` — `unsigned int`
  - `_L` — `long` / `int64_t` (explicit width via `int64_t` internally)
  - `_S` — `const char *` (nul-terminated UTF-8)
  - `_P` — `void *` (opaque user pointer)
  - `_F` — function pointer (declared as a specific typedef per attribute
    group so the reader knows the exact signature)
  - `_ID` — `bd_id`
  - `_C` — `uint32_t` color (RGBA8)
- **Varargs reader pulls each value as `uintptr_t`**, then casts to the
  declared type. This is the one width that is safe to read on every
  supported ABI. Callers pass integer values via macros that perform the
  cast:

      #define BD_I(v)  ((uintptr_t)(intptr_t)(int)(v))
      #define BD_S(v)  ((uintptr_t)(const char *)(v))
      #define BD_P(v)  ((uintptr_t)(void *)(v))

  Usage: `bd_set(btn, BD_WIDTH_I, BD_I(200), BD_END);` — verbose, but
  every value is pointer-wide in the varargs and every cast is explicit.
  Attribute IDs alternate with wrapped values; mismatches are a
  compile-time error from the cast, not a silent ABI bug.
- **No bare integer literals in attribute lists.** Enforced by convention
  and, where practical, by making the `_I`/`_S`/etc. macros mandatory.
- **Function pointers are never cast through `void *`.** Each `_F`
  attribute group has a dedicated typedef and the reader pulls through
  that typedef. Avoids the POSIX `dlsym`-style UB on platforms where
  function and data pointers differ in width.

### Non-varargs alternative: the builder

For callers that prefer strict type checking (and for the Lua binding,
which can't sensibly construct varargs at runtime), an array-based
alternative exists:

    bd_attr attrs[] = {
        { BD_LABEL_S,  .s = "Connect"   },
        { BD_WIDTH_I,  .i = 120         },
        { BD_ON_CLICK_F, .f = on_connect },
        { BD_END, {0} },
    };
    bd_id btn = bd_create_v(panel, BD_BUTTON, attrs);

`bd_attr` is a tagged-union struct; the compiler checks the designator
against the declared member type. The varargs form calls into the same
internal builder, so both paths share one implementation.

### Testing the ABI

A unit test exercises `bd_create_v` / `bd_create` with every attribute
type, compiled both 32-bit and 64-bit, and diffs the resulting widget
tree. Anyone adding a new attribute must extend this test. The Pi dual-
userland scenario (armhf + aarch64 sharing `/usr/include`) is the
canonical target.

## Implementation status

As of 2026-06 the toolkit is at GUI v0.3 (multi-window, explorer, and Tab
focus traversal have landed; the remaining v0.3 items are noted below).
What is built:

- **Backend abstraction** — the `bd_backend` GPU vtable + neutral `bd_event`;
  the toolkit never touches a windowing/render library directly. Three backends:
  the reference **ludica** binding (`bd_backend_ludica.c`, what birdie runs on),
  a raw **OpenGL ES 3 / X11** binding (`src/guitest/`, what the gallery runs on),
  and an **SDL3** binding (`bd_backend_sdl3.c`, the examples/sdl3 demo host).
  Runtime-verified on ludica and GLES.
- **Renderer** — `bd_draw.c` builds rects, textured sprites, and stb_truetype
  text on the GPU interface; widgets can also drop to a custom fragment shader.
- **Chrome widgets** — `BD_FRAME`, `BD_PANEL`, `BD_LABEL`, `BD_BUTTON`,
  `BD_MENU` (+ pinnable pushpins), `BD_TEXT` (single-line field),
  `BD_MULTILINE` (multi-line editor), `BD_LIST` (scrolling/selectable list),
  `BD_TAB_BAR` (skeuomorphic folder tabs), `BD_SCROLLBAR`, `BD_NOTICE` (modal
  alert/confirm), `BD_INPUT_LINE`, and the `BD_TERMINAL` extension (libvt).
  Flexbox row/col + fixed layout. The v1.0 core widget set is complete.
- **Value widgets** (extensions) — slider, shaded knob, sliding toggle, scroll
  wheel, jog dial, X-Y pad.
- **Indicator lamp** (extension) — a panel-mount LED indicator (clear, frosted,
  or faceted cut-glass jewel lens) drawn in a fragment shader. Its state is an
  index into a string-parsed color list (names or `#hex`), state 0 = off;
  optionally a clickable lamp button that cycles. `bd_widget_indicator.{c,h}`.
- **Editor widget** (extension) — rich-text, row-oriented text editor (style
  runs: fg/bg/underline/strike/bold/super-sub) for code or ABC-notation music.
- **Explorer widget** (extension) — model-driven icon grid with selection
  (single / Ctrl / Shift-range / rubber-band), drag-move, double-click
  activate, right-click context, keyboard nav, in-place rename, scissor clip.
- **Multiple native windows** — on the GLES backend (see the v0.3 section).
- **Keyboard focus** — click- and Tab/Shift-Tab traversal; `bd_focused()`.
- **Clipboard** — Ctrl-C/X/V in text fields via backend `clipboard_set`/`get`
  (X11 CLIPBOARD on the GLES backend; ludica backend NULL for now).
- **IME / compose** — `BD_EV_TEXT_COMMIT`/`BD_EV_TEXT_PREEDIT` + backend
  `ime_set_enabled`/`ime_set_cursor_rect`; X11 XIM on the GLES backend (native
  candidate window). Key-up + an auto-repeat flag on `bd_event`.
- **Multitouch** — `BD_EV_TOUCH_DOWN/MOVE/UP` with per-pointer ids + per-finger
  capture (several knobs at once); X11 XInput2 on the GLES backend.
- **Pen / tablet** — `BD_EV_PEN_HOVER/DOWN/MOVE/UP` with pressure/tilt/flags +
  a `bd_widget_canvas` drawing surface; X11 XInput2 valuators on the GLES
  backend.
- **Window focus** — `BD_EV_FOCUS_IN`/`BD_EV_FOCUS_OUT` (X11 `FocusIn`/`FocusOut`
  on the GLES backend, `LUD_EV_FOCUS`/`UNFOCUS` on ludica, SDL window
  focus-gained/lost on SDL3). The toolkit tracks focus per top-level window;
  `bd_gui_focused()` / `bd_window_focused(frame)` let an app throttle its render
  loop (pause animation, drop framerate, lower quality) while in the background.
- **Reduced motion** — a toolkit-wide flag widgets read (`bd_reduced_motion()`)
  to skip animation while still drawing the final state: the cursor stops
  blinking and the toggle thumb snaps instead of sliding. Policy is
  split from the widgets: `bd_set_reduced_motion(mode)` forces or leaves it on
  AUTO, and in AUTO it is on when unfocused or when `bd_reduced_motion_hint()`
  is raised (an accessibility preference or a future low-framerate watchdog).
- **Asset path resolution** — an optional `bd_backend.resolve_asset(rel, buf,
  bufsz)` hook lets an *installed* app find its runtime assets next to the
  executable instead of relative to the current directory. The backend writes
  the located absolute path into the caller-owned buffer and returns it, or
  `NULL` to fall back to the plain relative name (read from the cwd). Resolvers: SDL3
  (`SDL_GetBasePath`), GLES and ludica (Linux `/proc/self/exe` dir,
  `../share/birdie`, `$HOME/.local/share/birdie`). See Rendering, "Custom and
  embedded assets".

The v0.3 input roadmap (multi-window, explorer, rich text, the full v1.0
widget set, clipboard, key-up/repeat, IME, multitouch, pen) is complete on the
GLES backend. Deferred polish is tracked in the roadmap section: explorer
list/details view, cross-line selection in the multiline/editor widgets, and
the Win32 / Wayland / macOS backends.

## v1.0 widget set

Kept intentionally small. If it is not on this list, it is not in v1.0. The
"done" column marks what is implemented today; value widgets and the explorer
are extensions (`widget_ext.h`) and so are not in this core set.

| widget           | role                                                | done |
|------------------|-----------------------------------------------------|------|
| `BD_FRAME`       | top-level window                                    | yes  |
| `BD_PANEL`       | layout container                                    | yes  |
| `BD_LABEL`       | read-only text                                      | yes  |
| `BD_BUTTON`      | clickable action                                    | yes  |
| `BD_TEXT`        | single-line text input                              | yes  |
| `BD_MULTILINE`   | multi-line text input (prefs notes, script edit)    | yes  |
| `BD_LIST`        | scrolling list (MUD list, log sink list)            | yes  |
| `BD_SCROLLBAR`   | standalone scrollbar (paired with terminal pane)    | yes  |
| `BD_MENU`        | menu bar / popup menu (with `BD_MENU_ITEM`); pinnable | yes |
| `BD_NOTICE`      | modal confirmation / alert                          | yes  |
| `BD_TAB_BAR`     | tabs for multiple concurrent MUD sessions           | yes  |
| `BD_TERMINAL`    | the MUD output widget (custom renderer)             | yes  |
| `BD_INPUT_LINE`  | the MUD command input (history, completion)         | yes  |

`BD_TERMINAL` and `BD_INPUT_LINE` are the only two widgets birdie couldn't
have built against an off-the-shelf toolkit anyway; they are the largest
widgets by code volume. The rest are small.

Explicitly deferred to later versions: drop targets, file chooser, color
chooser, property sheets, split views inside the terminal pane, dockable
panels. Handle each with a modal or plain widget if encountered.

## Layout

Flexbox-style row/column with optional absolute positioning inside a
panel. Two layout directives per container:

- `BD_LAYOUT_ROW` / `BD_LAYOUT_COL` — auto-packing in one direction.
- `BD_LAYOUT_FIXED` — children place themselves with `BD_X`, `BD_Y`.

Children advertise a preferred size (`BD_PREF_W`, `BD_PREF_H`) and an
optional grow weight (`BD_GROW`). This covers every dialog birdie ships
in v1.0 without needing constraint solvers.

Placement within a cell is tuned by two more directives (see the
[reference](gui-reference.md) for the enums):

- `BD_ANCHOR_I` on a child (`enum bd_anchor`, compass points). In `FIXED` it
  pins the child to a parent edge/corner and tracks it on resize, with `BD_X`/
  `BD_Y` as inward margins; in `ROW`/`COL` it aligns the child on the cross
  axis at its preferred size instead of stretching. Default `BD_ANCHOR_FILL`
  keeps the original fill/top-left behavior.
- `BD_PACK_I` on a `ROW`/`COL` container (`enum bd_pack`): start/center/end/
  space-between/space-around distribution of leftover main-axis space when no
  child grows. Default `BD_PACK_START`.

**A container is never measured against its contents.** Layout is a
single pass: a box's size is its explicit `BD_PREF_W`/`BD_PREF_H` (or a
small `DEFAULT_MIN_*` fallback) plus a share of the leftover space when it
has `BD_GROW`. It does not look at what is inside. So a content-holding
panel with neither a pref size nor grow collapses to the minimum, and a
sibling that does grow eats the rest, clipping the collapsed panel's
children.

Therefore: **any container holding content must set an explicit
`BD_PREF_W`/`BD_PREF_H` or a `BD_GROW`.** Nested panels do not size
themselves. For a fixed-content panel, set the pref to the computed
content height, e.g. `2*pad + rows*row_h + gaps`. (Reported by the
smoltrek consumer, where an unspecified param panel collapsed under a
sibling editor that had grow.)

## Event model

One dispatch entry point per widget: `BD_ON_<event>` attributes (e.g.
`BD_ON_CLICK`, `BD_ON_TEXT_CHANGE`, `BD_ON_CLOSE`) accept a function
pointer plus an opaque user arg. Lua callbacks are wrapped by the
scripting layer and registered through the same attributes.

The terminal widget emits line events upstream into the trigger engine
(`doc/triggers.md`); the trigger engine does not know GUI details.

## Rendering

All drawing goes through the **`bd_backend` GPU interface**, not any specific
library. That interface is a GLES-class subset: compile a shader, set
uniforms, bind a texture, draw a triangle list of `bd_vertex` (pos / uv /
rgba), and a scissor rectangle. ludica and raw OpenGL ES both provide it; a
host implements the vtable and injects it via `bd_gui_init()`.

On top of that, the toolkit's renderer **`bd_draw.c`** offers the higher-level
primitives widgets actually call:

- filled / stroked rect (chrome panels, borders, selection)
- filled convex quad from four corners (`bd_draw_quad`) for trapezoids like
  the folder tabs
- textured sprite / quad (glyph atlas, icon textures, SIXEL/MXP later)
- text run (stb_truetype atlases baked from the chrome TTFs — a proportional
  and a fixed-width family, each in regular/bold/italic/bold-italic, selected
  by `bd_draw_text_styled` via `BD_FONT_BOLD|ITALIC|MONO`; the terminal
  extension draws its cells from an embedded fixed-cell bitmap font via
  `bd_draw_cell`, carrying no texture of its own)

These batch into one dynamic vertex buffer that flushes on texture change or
on `bd_draw_flush()`. A widget that needs an effect (the shaded knob, and the
explorer's scissor clip) flushes the batch and issues its own shader / scissor
through the backend, then resumes batching.

A dirty-region list driving per-frame redraws (idle to 0 Hz) is still planned;
the loop currently redraws each frame.

### Custom and embedded assets (`bd_asset`)

The toolkit requests each runtime asset (the chrome font faces) by a **generic
identifier**, not a filename: `BD_ASSET_FONT_REGULAR` and the seven other
`BD_ASSET_FONT_*` faces (in **`bd_asset.h`**). The terminal font and the
pushpins need no asset; they are 1-bit bitmaps compiled into the toolkit.
Register a source under an id to override its built-in default. A source is
**either a file path or in-memory data**, so one mechanism covers both "use a
different font" and "ship a self-contained binary":

```c
/* redirect an asset to another file (loaded on demand) */
bd_asset_register_file(BD_ASSET_FONT_REGULAR, "/home/me/.fonts/Inter.otf");

/* or serve it from bytes compiled in (an .incbin / xxd -i blob) */
bd_asset_register_data(BD_ASSET_FONT_MONO, mono_ttf, mono_ttf_len);
```

Register before `bd_gui_init*`. The key is the asset's identity, so a custom
font is registered as `BD_ASSET_FONT_REGULAR` -- no need to name it after the
stock font or override a build macro. Anything unregistered falls back to that
asset's default file, so the default build is unchanged. Registered data and
path strings are **borrowed** and must outlive use (a `.rodata` blob fits).

Resolution lives in the toolkit, not the backends: `bd_draw.c` resolves each of
the eight faces (explicit `bd_font_set` face, else registry id, else default
file). Fonts can still be supplied wholesale as a
`bd_font_set` (`bd_gui_init_fonts`); the registry is the lighter route and the
only one for the texture assets. `bd_draw_set_font_reader` remains as a
lower-level per-path hook for a host with its own resolver.

**Keeping build paths out of the binary.** The registry keys are the fixed
`BD_ASSET_*` id strings (short, generic) and expose nothing. The only file
names baked in are the built-in fonts' short **relative sub-paths**
(`fonts/DejaVuSans.ttf`) — no absolute paths, nothing machine-specific,
identical in every build. The build copies the fonts into `$(BINDIR)` next to
the executable, where `resolve_asset` finds them; keep any
`.incbin` source path (a build-time detail) machine-independent and it never
reaches the binary either. The `examples/embed/` example registers every asset
by id so a self-contained binary reads no files at all.

**Finding assets on disk (`resolve_asset`).** Registration covers "use my
bytes/file"; the remaining case is an *installed* app whose assets sit next to
the executable rather than in the current working directory. For that the
toolkit asks the backend's optional `resolve_asset(rel, buf, bufsz)` hook to
locate a runtime asset by its **asset-root-relative** sub-path (e.g.
`"fonts/DejaVuSans.ttf"`).
The backend searches the locations that fit its platform, writes the located
absolute path into the caller-owned `buf`, and returns it; it returns `NULL`
when the asset is not found there, and the toolkit then uses the built-in
default path resolved against the current directory (the historical dev-tree
behavior). The buffer is caller-owned, so there is no shared state or lifetime
to track. `bd_asset_resolve()` wraps the hook with the fallback. This runs only
for *unregistered* assets built from their default file: an explicit
`bd_asset_register_*` source (path or blob) is used as given and bypasses the
hook. Reference resolvers: SDL3 uses `SDL_GetBasePath`; the GLES and ludica
backends search, on Linux, the executable's own directory, a sibling
`../share/birdie` (FHS install layout), then `$HOME/.local/share/birdie`.

## Pinnable menus (olvwm-style pushpins)

Popup menus — both the menu-bar pull-downs and context menus — carry a
**pushpin** in their title area. Clicking the pin pins the menu open at
its current on-screen position; it stays until dismissed explicitly. An
unpinned menu auto-dismisses on selection as usual.

Why: batch operations on similar GUI actions are the killer use case
(olvwm's original motivation). In a MUD client the natural uses are:
- triggering the same trigger-class toggle repeatedly during testing;
- replaying a set of scripted commands while tuning them;
- running the same log-viewer filter against several session files.

Implemented: a pinned menu is a menu whose lifetime is decoupled from the
SELECT release that opened it. The widget carries `BD_MENU_PIN_B` (a `BD_B`
boolean attribute) the user can read or set programmatically. The pin glyph is
a 1-bit bitmap compiled into the toolkit (from `pushpin_*.xbm`), drawn tinted
and sized to the chrome font like a text glyph via `bd_draw_pushpin`. No new
widget type.

Attribute suffix `_B` — `int` used as boolean; added to the suffix list
above for completeness.

## OPEN LOOK visual cues (optional, cheap)

The XView/OPEN LOOK look is distinctive and inexpensive to replicate
with our primitive set:

- Obround buttons — rounded-rect with semicircular ends.
- Triangle glyphs on menu buttons — single filled triangle.
- Three shading tones computed from one base color per widget (OLGX-style)
  so the theme is one tunable instead of a palette.

None of this is load-bearing; a flat theme also works. Call it a stretch
stylistic goal.

## Threading

All widget calls happen on the UI thread. The network thread enqueues
received bytes into a lock-free ring; the UI thread parses (libvt),
appends to the terminal widget's scrollback, and walks the trigger engine.
This matches the decision left open in `doc/triggers.md`.

## Roadmap: proposed v0.3 features

The v0.3 line, built on the v0.2 GPU-drawing/value-widget work. Multi-window,
the explorer widget, and Tab focus traversal have landed (marked below); the
rest are recorded here so today's design does not foreclose them.

Several of these break the input ABI (`bd_event` and the `bd_backend`
vtable): multi-window adds a window id, IME needs a string-carrying commit
event and a reverse caret channel, multitouch needs per-pointer ids, and
the pen needs pressure/tilt fields. Land them together as one v0.3 ABI
break rather than churning the struct repeatedly.

The prototyping vehicle is the **second backend** (below): ludica exposes
none of the per-pointer/valuator input these features need, but the owned
GLES backend talks X11 directly and can grow XInput2 touch/pen and a real
IME path. The widget gallery on that backend is where the new input is
exercised, while birdie keeps running on ludica.

### Reference GLES backend and widget gallery

birdie the MUD client runs on ludica. To keep a second backend honest and
to have somewhere to exhibit and exercise widgets, the toolkit also ships a
raw OpenGL ES 3 backend plus a standalone widget gallery:

- `src/guitest/window.h` + `x11_window.c` — a neutral window/event layer
  over X11 + EGL (no Xlib leaks past the header), adopted from smoltrek.
- `src/guitest/bd_backend_gles.{c,h}` — the `bd_backend` GPU vtable on raw
  GLES3 (shader compile, streaming vertex buffer, textures, scissor).
- `src/guitest/widget_test.c` — the gallery: menus (incl. a pinnable one),
  terminal, input line, every value widget, sliders, buttons, with an event
  readout. Built by `make widget-test` (Linux/X11, opt-in); the build stages
  the fonts next to the binary, so it runs from any directory.

Owning this backend means birdie-gui has a non-ludica reference
implementation. The plan is to **bundle both the GLES backend and the
gallery into the `make dist` ZIP** so downstream consumers get a working
example backend and a widget showcase, not just headers and sources.

### Multiple native windows (implemented on the GLES backend)

Open more than one top-level window so pop-up dialogs and genuinely
multi-window applications are possible. This is the natural companion to
pinnable menus: a pinned menu wants to outlive its parent and float as its
own window, and tear-off palettes/inspectors follow the same model.

Not every backend can reasonably create multiple OS windows. Backends that
can (X11, Win32, Wayland, SDL) map each `bd_frame` to a real window.
Backends that cannot (a single-surface or embedded host) must run their
own in-surface window manager that lays out and decorates birdie-gui
windows inside the one surface they own. The toolkit therefore treats
"a window" as a backend capability, not a guarantee.

What landed:

- The `bd_backend` vtable gained a `multi_window` capability flag plus
  `window_open`/`window_close`/`window_begin`/`window_swap`/`window_width`/
  `window_height`/`window_set_title`. `bd_event` gained a `window` id.
- Each top-level `BD_FRAME` (parent `BD_NONE`) is a window. The first frame
  adopts the primary window the host opened before `bd_gui_init` (so the GL
  context exists for shader/atlas init); later frames call `window_open`.
  `bd_frame_for_window()` maps an id back to its frame (e.g. for the host to
  destroy the right frame on a WM close).
- `bd_gui_render`/`bd_gui_layout`/`bd_gui_event` take a dual path on
  `be->multi_window`: in multi-window mode render iterates windows
  (`window_begin` → draw tree + the popups that window owns → `window_swap`)
  and input routes to the frame matching `ev->window`. When the flag is 0
  (ludica, the headless stub) the old single-window path is used unchanged,
  so birdie and `make test` are unaffected.
- The GLES backend (`src/guitest/`) implements it: one `EGLDisplay`/
  `EGLContext`/`EGLConfig` shared across per-window `Window`+`EGLSurface`,
  `win_poll` tags events with the originating window id. The gallery's "New
  Window" button opens a real second OS window with its own widgets.
- Routing/registration/render-all/destroy are covered by `make test` (a
  multi-window recording stub); two independent native windows verified on a
  real display.

**In-surface window manager (single-surface backends).** When `multi_window`
is 0 (ludica, SDL, the headless stub) the toolkit runs its own window manager
so secondary frames are usable, not just the primary one:

- `windows[0]` (the first top-level `BD_FRAME`) is the full-surface **desktop**
  and is drawn exactly as before, so existing single-frame hosts (birdie on
  ludica, `make test`) are unaffected.
- Every later top-level `BD_FRAME` is a **floating window** the toolkit lays
  out at its own rectangle, decorates with a title bar (label + a lock and a
  close button), and composites over the desktop. The `windows[]` array order
  is the z-order; a press raises a window to the front and routes input to the
  topmost window under the pointer (`wm_dispatch` / `wm_frame_at` in
  `widget.c`).

**Window gravity, snapping, and docking.** A floating window carries a
`BD_GRAVITY_I` (`enum bd_gravity`) and a `BD_LOCKED_B` flag:

- Dragging the title bar moves the window (`BD_X_I`/`BD_Y_I`). Released within
  `WM_SNAP_DIST` of a surface edge or corner it **snaps and docks** there
  (gravity is set); an edge dock becomes a full-length strip at the window's
  preferred width/height, a corner dock keeps the preferred size. Dragging away
  clears gravity and the window floats again. A translucent preview shows the
  snap target during the drag.
- A docked window **re-snaps to its edge/corner on every layout**, so it stays
  glued when the surface resizes (a stable dock/palette).
- **Locking** pins the window: it can no longer be dragged but keeps its
  gravity and still re-snaps on resize. The title-bar padlock toggles it; the
  host can also call `bd_window_set_locked()` / `bd_window_dock()` /
  `bd_window_move()`.

The `examples/sdl3` demo carries a real floating "Palette" frame exercising
this; `test/test_gui.c` covers desktop-fills-surface, drag, snap/dock, lock,
resize re-snap, and raise on the single-surface stub.

**Any content widget can be a window.** The WM decorates and manages *top-level
frames*, not particular widget types, so an extension widget (an inventory, a
table, an editor) becomes a first-class floating window simply by making its
parent a top-level `BD_FRAME` (parent `BD_NONE`) with a `BD_LABEL_S` title and a
`BD_PREF_W`/`BD_PREF_H`: it then gets the title bar, drag, snap/dock, lock,
minimize-to-dock, and close for free, and the WM routes body input into it. The
SDL3 demo hosts its inventory this way (a "Bag" window you drag and minimize),
and `test/test_gui.c` verifies an inventory in a floating frame lays out below
the title bar, takes body clicks, drags by its title, and stops receiving input
while minimized. The frame does not auto-size to its content (layout never
measures children), so give the frame an explicit preferred size.

Still to do:

- **ludica / single-surface compositing** for genuinely separate render
  targets. The in-surface WM above draws floating frames into the one surface;
  a backend that composites frames as independent targets is still deferred.
- Native (`multi_window == 1`) client-side move/snap: gravity/lock state is
  stored but not yet applied to real OS windows (the OS manages those).
- Per-window hover-leave (moving off a floating window can leave stale hover),
  window resize handles, and per-window menu state (one open menu at a time
  across all windows).

### Cross-widget drag-and-drop — implemented

The toolkit's pointer capture keeps a drag glued to the widget it started on, so
a source never sees the release land elsewhere. A small facility in the
extension API (`widget_ext.h`) bridges that: a source widget mid-drag calls
`bd_dnd_begin()` with a `bd_dnd_payload` (source id, item key, label, icon, opaque
user pointer). The toolkit copies it, draws a translucent ghost tile trailing the
pointer, and on release over a **different** extension widget synthesizes a
`BD_EV_DROP` event to that target's `event()` hook. The target reads the payload
with `bd_dnd_get()` and returns 1 to accept. A release over the source or empty
space just discards the drag; there is no separate cancel. It is mouse-driven
(the touch/pen capture paths are unaffected) and the payload's lifetime is tied
to the active pointer capture, so a destroyed source cleanly drops the drag.

`bd_widget_inventory` and `bd_widget_dock` are both drag sources now (a grid item
or a minimized-window tile), and `bd_widget_actionbar` (below) is the reference
drop target. Verified headlessly in `test/test_gui.c` (drag an inventory item
onto an action-bar slot; the ghost renders without crashing) and demoed live in
the SDL3 example.

### Action bar widget — implemented

A CRPG-style hotbar / floating tool palette (`bd_widget_actionbar.{c,h}`, a
`widget_ext` extension), the mutable sibling of the dock. It renders the same
beveled tiles (`bd_draw_tile`) and hugs a screen edge/corner with `enum
bd_gravity` (vertical for LEFT/RIGHT and corners, horizontal for TOP/BOTTOM),
but where the dock is a read-only projection of the minimized-window set, the
action bar **owns** its slots. With `BD_GRAVITY_NONE` it floats at its
`BD_X_I`/`BD_Y_I` position and the user drags it by the grip at its leading edge;
place it in a `BD_LAYOUT_FIXED` parent (the desktop root) so it can anchor.

- **Filled by drag-and-drop.** Drop an inventory item or a dock tile onto a slot
  (via the cross-widget DND above) and its icon/label/key bind there; a `drop`
  callback can veto or customize. Slots also reorder by dragging one onto
  another (fires `move`), and a plain click fires `activate`.
- **Hotkeys.** Each slot carries an optional keyboard binding (ASCII key +
  Shift/Ctrl/Alt), like a WoW/Neverwinter Nights hotbar. The binding belongs to
  the slot, not the action, so it survives a re-drop. Its label (`⇧` Shift, `^`
  Control, `⌥` Alt, then the key) is drawn in the slot corner when the slot is
  empty or hovered. Feed presses to `bd_actionbar_key()` from the host's key
  handling to fire the matching slot; the bar does not grab global keys itself.

Covered by `test/test_gui.c` (drop-binds, click-activate, hotkey dispatch with
modifier matching, internal reorder) and exercised in the SDL3 example (a
six-slot floating bar bound to Q W E R T Y).

### Tab view widget — implemented

A tabbed container (`bd_widget_tabview.{c,h}`, a `widget_ext` extension): a
`BD_TAB_BAR` folder-tab strip over a content area in which exactly one *pane*
shows at a time. Where `BD_TAB_BAR` is only the strip (it tracks an active index
and fires a callback), the tab view owns the strip **and** the panes and swaps
them. `bd_tabview_add_pane(tv, label)` returns a `BD_PANEL` the host fills with
any widget subtree, so a pane is as complex as a whole window: labels, an
inventory, an editor, a texture-backed view of a live GLES animation, another
layout.

It is a thin composite: a `BD_TAB_BAR` child over a `BD_LAYOUT_FIXED` content
child, one `BD_PANEL` pane per tab. Because the content is FIXED, every pane
fills the **same** rectangle (they overlap), so switching tabs is free and each
pane keeps its own laid-out contents; the view just keeps the active pane
`BD_VISIBLE_B` and hides the rest, which the core already skips in render and
hit-testing. The class sets `BD_WC_CONTAINS_CHILDREN`, so the core lays out,
renders, and routes input through the strip and panes for free; the only custom
logic is building the subtree and syncing pane visibility from the strip's active
index (done in the layout hook and the strip's change callback, so a tab click,
Left/Right, or `bd_tabview_set_active` all converge).

Covered by `test/test_gui.c` (three panes, only the active one visible and
hittable, tab switch flips visibility and input routing, programmatic switch is
silent) and shown in the SDL3 example (a "Panels" window flipping between plain
widgets, the spinning relic as a live GLES animation, and the editor).

Because `MAX_WIDGET_CLASSES` had to grow to seat this alongside the recording
test stubs, the extension-class registry cap is now 32 (was 16).

### Explorer / icon-browser widget

A new widget type (v0.3, built as a `widget_ext` extension) modeled on
Explorer / PROGMAN.EXE (Win 3.x) / Finder: an arrangeable grid of labeled
icons with persistent positional state. It is
**not** tied to files; a callback/model interface lets the items be any
collection (a MUD client's server list, a DAW's known-plugin list), so the
view refreshes from live data without the app rebuilding it.

In `src/birdie-gui/bd_widget_explorer.{c,h}`. Working: model query, grid/free
layout, rendering, click selection (replace / Ctrl-toggle / **Shift-range**,
the latter additive with Ctrl), double-click activate, right-click context,
wheel scroll, **drag-move** (commit via `set_pos` + `moved()`),
**rubber-band** selection (Ctrl = additive), and **keyboard navigation**
(click to focus, then arrows / Home / End to move the cursor, Shift to extend
the range, Ctrl-A select all, Enter activate; a focus ring marks the cursor),
and **in-place rename** (F2 or `bd_explorer_begin_rename()` opens a small
UTF-8 line editor over the label; Enter commits via `model.set_name`, Escape
cancels, clicking away commits). The scrolling content is **scissor-clipped**
to the panel interior. Still to come: list/details view modes. Exhibited in
the widget gallery's "New Window" dialog (drag to arrange, F2 to rename).

Keyboard focus: the toolkit routes key events to a focused extension widget
and gives a widget keyboard focus when it is clicked (the same path that
already focused `BD_INPUT_LINE`). **Tab / Shift-Tab** now traverse the
focusable widgets of the current frame (input lines, buttons, and extension
widgets), wrapping around; a focused button shows a focus ring and is
activated by Enter or Space. `bd_focused()` reports the focused widget. Menus
are excluded from the Tab order. (A latent bug where focus survived
`bd_gui_cleanup` was fixed in passing.)

**Model — the widget owns no data.** Items carry a *stable key* so selection
and saved positions survive a refresh when indices shift:

    typedef struct {
        uint64_t    key;      /* stable identity; selection/state keyed on this */
        const char *label;    /* caption; valid for the get() call only */
        bd_texture  icon;     /* 0 = default placeholder */
        int         enabled;  /* 0 = dimmed, not activatable */
        int         x, y;     /* saved position; <0 = auto-place on the grid */
        void       *user;     /* app payload */
    } bd_explorer_item;

    typedef struct {
        int  (*count)(void *ctx);
        void (*get)(void *ctx, int index, bd_explorer_item *out);
        void (*set_pos)(void *ctx, uint64_t key, int x, int y); /* optional persist */
        void *ctx;
    } bd_explorer_model;

The widget caches nothing authoritative: it re-queries `count`/`get` on
layout, and the app calls `bd_explorer_refresh()` when data changes. Label
and icon need only be valid during `get()`; the widget keeps only keys and
positions.

**Per-item events** go back to the app:

    typedef struct {
        void (*activate)(bd_id w, uint64_t key, void *user);            /* dbl-click/Enter */
        void (*context)(bd_id w, uint64_t key, int sx, int sy, void *); /* right-click */
        void (*selection_changed)(bd_id w, void *ctx);
        void (*moved)(bd_id w, uint64_t key, int x, int y, void *);     /* after a drag */
        void *ctx;
    } bd_explorer_cb;

`context()` hands the app screen coords so it can pop a `BD_MENU` — the hook
for app-specific right-click menus, tying into pinnable menus and (for a
torn-off menu) multi-window.

**Create / accessors**, in the value-widget style:

    bd_id bd_explorer_create(bd_id parent, const bd_explorer_model *m,
                             const bd_explorer_cb *cb, ...);   /* attrs, BD_END */
    void  bd_explorer_refresh(bd_id id);
    int   bd_explorer_selection(bd_id id, uint64_t *keys, int max); /* -> count */
    void  bd_explorer_select(bd_id id, uint64_t key, int add);
    void  bd_explorer_set_icon_size(bd_id id, int px);

**Interaction (Windows-Explorer semantics):** left-click selects one (sets
the anchor); Ctrl+click toggles; Shift+click range-selects from the anchor;
drag on empty space rubber-bands (Ctrl = additive); drag on a selected icon
moves the whole selection and commits via `set_pos` + `moved()`; double-click
activates; right-click selects-then-`context()`; wheel scrolls; arrows/Enter/
Ctrl-A drive grid nav (needs focus traversal). This rides the existing
pointer-capture path (down/move/up routed to the captured extension, held
through the drag) that the value widgets already use.

**Rendering** through `bd_draw` + `scissor`: a recessed bevelled panel; per
cell a translucent selection highlight (`theme.focus`), the icon quad
(`bd_draw_sprite`, dimmed when disabled), a centred truncated label, and a
focus ring; a translucent rubber-band rect; ghost copies of dragged icons.
Scissor clips the content while `scroll_y` offsets the cells.

**Layout & persistence:** cell = icon + label + pad; columns = view width /
cell width. Items with `x,y >= 0` are placed there (free desktop-icon
layout); `x < 0` auto-places row-major and may be written back via `set_pos`.
The model owns persistence (CSV for the MUD server list per this project's
MUD-list format, prefs for a DAW), keyed by the stable key.

**Scope.** First cut: icon grid, single/multi-select (rubber-band +
Ctrl/Shift), drag-move, double-click activate, right-click context,
enabled/disabled, wheel scroll, refresh. Later: list/details view modes,
in-place rename (needs the real `BD_TEXT` widget), type-to-find, drag-drop
between explorers, a `BD_SCROLLBAR`, per-icon accessibility roles. Sensible
build order is mouse-only core first, then rename, keyboard nav, and a richer
context menu as those dependencies (text widget, focus traversal,
popup-anywhere) land.

### IME, compose, and dead keys — implemented

The four seams (identical across X11 (XIM/IBus), Win32 (IMM/TSF), macOS
(`NSTextInputClient`), and Wayland (text-input-v3)) are in place:

- **Commit as a UTF-8 string.** `BD_EV_TEXT_COMMIT` carries `bd_event.text`
  (valid for the dispatch only). The text widgets insert it (replacing the
  selection; newlines kept in `BD_MULTILINE`); the editor handles it too. This
  is what makes multi-codepoint commits, dead keys, and compose work. `BD_CHAR`
  stays for the simple single-codepoint path (ludica still uses it).
- **Preedit event.** `BD_EV_TEXT_PREEDIT` carries the in-progress string plus
  a caret offset; the focused single-line field renders it inline (underlined
  at the caret) without inserting until commit. (The X11 backend below uses the
  IME's own preedit window, so it does not emit this yet; the ABI + rendering
  are ready for an over-the-spot backend.)
- **Caret-rect reporting (toolkit → backend).** `ime_set_cursor_rect(x,y,w,h)`
  is called each frame for the focused field so the platform places its
  candidate window at the caret (X11 `XNSpotLocation`).
- **Enable/disable on focus.** `ime_set_enabled(on)` fires as focus enters or
  leaves a text field, so the IME does not swallow keys elsewhere.

**Decision (kept):** the platform IME draws its own candidate window; the
toolkit only reports the caret rect.

The raw GLES backend implements it on X11/XIM: an `XIC` (PreeditNothing, so the
IME shows its own preedit/candidates) focused via `XSetICFocus`, with
`Xutf8LookupString` emitting the full committed UTF-8 string as
`BD_EV_TEXT_COMMIT`. Verified headlessly (commit insert incl. multi-byte CJK,
preedit, enable/disable on focus). The ludica backend leaves the IME hooks
NULL and keeps the `BD_EV_CHAR` path. Still to do: an over-the-spot X11 preedit
(emit `BD_EV_TEXT_PREEDIT` via `XIMPreeditCallbacks`); Win32/macOS/Wayland.

### Real text and multiline widgets

Both **done**. `BD_TEXT` (single-line field) shares `BD_INPUT_LINE`'s editor
(`is_text_field()` covers all three) and differs only in that Enter commits
without clearing. `BD_MULTILINE` (prefs notes, script editing) holds `\n`-
separated text in the same buffer: it reuses `input_key` for
Left/Right/Backspace/Delete and the char path, adds line-relative Home/End,
Up/Down (preserving caret x), Enter-inserts-newline, vertical scroll, and
scissor-clipped multi-line rendering; click positions the caret by line/column.
Initial text via `BD_LABEL_S`, read back with `bd_get_s(id, BD_LABEL_S)`, set
with `bd_set`. Both will pick up the IME preedit/commit path above when it
lands; cross-line selection in `BD_MULTILINE` is still to do, and it is the
base the rich-text editor widget composes over.

### Editor widget (rich-text, row-oriented) — implemented

`src/birdie-gui/bd_widget_editor.{c,h}`. A higher-level text editor with the same
multi-line editing model as `BD_MULTILINE` plus a rich-text styling layer. An
extension (`widget_ext`), not a core widget. (It reimplements the editing
model rather than embedding a `BD_MULTILINE`, since the multiline renders one
color and rich text needs per-run drawing.)

Driving use case (smoltrek): a window to edit a small text-based music file
(**ABC notation**) and play it back, with the styling layer marking the row or
note currently sounding. The same layer gives **code syntax highlighting** for
free, which is the reason to base highlighting on rich text rather than a
one-off highlight channel.

**API (sketch).** Row-oriented, since both ABC and code are line structured:

- set text: `set_text(all)`, `insert_row(n, s)`, `replace_row(n, s)`,
  `delete_row(n)`
- read text: `text()`, `row_count()`, `row_text(n)`
- lock: `set_locked(on)` — read-only (caret/selection and highlighting still
  work; keystroke edits and the row mutators are rejected). Lets the host edit
  programmatically while the user only views, e.g. during playback.
- highlight: apply a style to a whole row, or to a `[row, col0..col1]` span;
  `clear_highlights()`. Highlighting is just setting style runs (below).

**Rich-text model.** Styles are a small set — bold, italic, underline,
strikeout, superscript, subscript, foreground color, background color — encoded
as a flag bitmask plus two colors. They are stored as **style runs** layered
over the plain text: a sorted list of `(start, end, style)` spans, so the text
buffer itself stays plain (cheap editing; runs shift with inserts/deletes or
are simply recomputed). The application owns styling: a syntax highlighter
tokenizes on change and re-emits runs; the ABC player sets a run on the active
row/note. The editor's `highlight()` calls are a convenience over the run list.

**Rendering implications.** The text renderer draws per-run fg/bg and styles.
fg/bg, underline, strikeout, and super/subscript are cheap (vertex color, extra
line quads, a baseline offset). **Bold and italic** are true variant faces:
`bd_draw.c` bakes eight atlases — a proportional family (DejaVu Sans) and a
fixed-width family (DejaVu Sans Mono), each in regular/bold/oblique/
bold-oblique — and `bd_draw_text_styled(..., BD_FONT_BOLD|BD_FONT_ITALIC|
BD_FONT_MONO)` picks one (a missing variant TTF falls back to regular). The
editor uses the **fixed-width family by default** so code and ABC columns line
up (`bd_editor_set_monospace`); chrome stays proportional.

**Custom fonts.** An app can replace the whole family in one call: fill a
`bd_font_set` (each face a path *or* an in-memory TTF/OTF buffer) and pass it to
`bd_gui_init_fonts` (or `bd_draw_init_fonts` at the renderer layer). Or replace
individual faces without threading a set through, by registering a source under
the face's generic id -- `bd_asset_register_file(BD_ASSET_FONT_REGULAR, "MyFont.otf")`
or `bd_asset_register_data(...)` (see Rendering, "Custom and embedded assets").
Either way the face is named by identity, not by the stock filename.
`bd_draw_set_font_reader` remains the lower-level per-path hook. See README
"Embedding assets (fonts and images), and custom fonts".

**Status.** Implemented: the row API (`bd_editor_set_text` / `text` /
`row_count` / `row_text` / `insert_row` / `replace_row` / `delete_row`), lock
(`bd_editor_set_locked`), styling (`bd_editor_clear_styles` /
`style_span` / `highlight_row` / `highlight_span` with `bd_rich_style`), the
multi-line editor (caret nav, newline, backspace/delete, click-to-caret,
scroll), and styled rendering segment-by-segment: per-run fg/bg, underline,
strikeout, **true bold and italic** (separate baked faces, see Rendering),
super/subscript (baseline shift); it renders in a **fixed-width face by
default** (`bd_editor_set_monospace`). Style runs are byte ranges that shift across
edits; an app re-emits them after big changes (a syntax highlighter) or sets
them on a locked buffer (ABC playback). Deferred: cross-line selection, and
Tab-inserts-a-tab (Tab currently traverses focus).

### Clipboard — implemented

Copy / cut / paste via two optional backend hooks, `clipboard_set(utf8)` and
`clipboard_get()`. The text fields handle Ctrl-C / Ctrl-X (copy/cut the
selection) and Ctrl-V (paste, replacing the selection; newlines kept in
`BD_MULTILINE`, stripped in single-line fields); the editor widget pastes too.
The raw GLES backend implements it on the X11 CLIPBOARD selection (owns the
selection and serves `SelectionRequest`; reads via `XConvertSelection`),
verified interoperating with `xclip` both directions. The ludica backend
leaves the hooks NULL for now, so birdie itself has no clipboard until ludica
exposes one (see `todo.txt`); the hooks being NULL is a safe no-op. Per-window
copy/cut still needs a selection, so it is single-line-field-only until
`BD_MULTILINE`/editor selection lands.

### Focus traversal, key-up, and repeat

- **Tab / Shift-Tab** traversal between focusable widgets — **done**: cycles
  the current frame's input lines, buttons, and extension widgets (menus
  excluded), wrapping; focused buttons get a ring and Enter/Space activation;
  `bd_focused()` exposes the focus. Per-window scoping uses `ev->window`.
- **Key-up events and a repeat flag** on `bd_event` — **done**. `BD_EV_KEY_UP`
  plus `bd_event.repeat` (1 on an auto-repeat key-down). Both backends emit
  them (the GLES backend collapses X11's release/press auto-repeat pair into
  one repeat key-down). Key-up routes to the focused extension widget; core
  widgets act on key-down only.

### Multitouch — implemented

`BD_EV_TOUCH_DOWN/MOVE/UP` each carry a **per-pointer id** (`bd_event.touch`)
plus position. The toolkit keeps a small per-finger capture table: a touch-down
grabs the extension widget it landed on, and that finger's moves/up route to it
until release — so several widgets drag at once. Each finger is delivered to its
widget as a synthesized mouse event, so existing value widgets (knobs, sliders,
X-Y pads) work unchanged: the headline case, **turning several knobs at once
with one finger each**, just works. Pinch/rotate gesture recognition can sit on
top later (app- or helper-side).

The raw GLES backend sources touches from **X11 XInput2** (selects
`XI_TouchBegin/Update/End` per window; reads the touch id and coords from the
`XIDeviceEvent`). Verified headlessly (two fingers driving two widgets with
interleaved down/move/up); not live-tested for want of a touchscreen. ludica
exposes no touch, so birdie has none there. Win32 pointer input / Wayland touch
/ macOS are future.

### Pen tablet input and a drawing canvas — implemented

`bd_event` carries pen fields: `pressure` (0..1), `tilt_x` / `tilt_y`
(degrees), and `pen_flags` (`BD_PEN_INRANGE` / `BD_PEN_BARREL` /
`BD_PEN_ERASER`), delivered as `BD_EV_PEN_HOVER/DOWN/MOVE/UP`. Contact
(`PEN_DOWN`) captures the extension widget under the tip so the whole stroke
routes there even if it strays past the edge; `PEN_HOVER` tracks the widget
under the cursor without capturing, so a canvas can preview the nib before
touching down. Pen events are delivered verbatim (not synthesized to mouse),
since the consumer wants pressure and tilt.

The consumer is `bd_widget_canvas` (`bd_widget_canvas.{c,h}`): a
drawing-canvas `widget_ext` that turns pen data into variable-width ink
strokes (pressure → nib width, tilt → broader nib), with the barrel button
switching to a second ink and the eraser end rubbing out strokes it crosses.
A hover ring previews the nib in proximity. It renders strokes as a run of
convex quads with a square dab at each sample (caps + joins), scissor-clipped
to the surface, through the v0.2 GPU interface. It also accepts plain mouse
drags at full pressure, so it is usable with no tablet.

The raw GLES backend sources the stylus from **X11 XInput2** valuators: it
selects `XI_Motion`/`ButtonPress`/`ButtonRelease`, identifies a device as a
pen by an `Abs Pressure` valuator (caching its pressure/tilt valuator numbers
and ranges per device), reads pressure/tilt off each event, and maps the tip
button to down/up, side buttons to the barrel flag, and an "eraser"-named
device to the eraser flag. Plain mouse XI events are ignored so the core
pointer path is untouched. The stroke routing, building, pressure→width, and
eraser are headless-verified, and the renderer is verified live (a
pressure-ramped sine drawn through the real event path, `GALLERY_AUTODRAW=1`);
the XInput2 *emit* path is unverified for want of a connected tablet. ludica
exposes no tablet. Windows Ink / Pointer API (Win32) and tablet-v2 (Wayland)
are future.

## Open questions

- Text shaping for chrome: stick with bitmap atlas (fits GLES2 cleanly,
  matches the terminal aesthetic, skips HarfBuzz) or pull in a shaper?
  Lean bitmap for v1.0.
- IME / dead-key handling on Windows and Linux. Non-trivial; v1.0 may
  only support ASCII in the input line, with full IME deferred.
- How far to push the accessibility tree in v1.0. Minimum: every widget
  carries a `BD_ROLE` and `BD_NAME` attribute from day one, even if no
  platform accessibility bridge is wired up yet. Free future work.
