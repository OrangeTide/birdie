# GUI

Birdie implements a **thin retained-mode widget layer** of its own, drawing
through a small backend abstraction (a GPU interface implemented on `ludica`
or on raw OpenGL ES; see Rendering). No third-party widget toolkit (no ImGui,
no Nuklear, no GTK/FLTK). The API style follows XView: objects are created
with an attribute-list constructor, queried and mutated through get/set calls.

The toolkit has outgrown "birdie's GUI": it is packaged as **birdie-gui**, a
reusable library (`make dist`), with birdie as one consumer. See the
Implementation status section below for what is built today.

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
  the toolkit never touches a windowing/render library directly. Two backends:
  the reference **ludica** binding (`bd_backend_ludica.c`, what birdie runs on)
  and a raw **OpenGL ES 3 / X11** binding (`src/guitest/`, what the gallery
  runs on). Runtime-verified on both.
- **Renderer** — `bd_draw.c` builds rects, textured sprites, and stb_truetype
  text on the GPU interface; widgets can also drop to a custom fragment shader.
- **Chrome widgets** — `BD_FRAME`, `BD_PANEL`, `BD_LABEL`, `BD_BUTTON`,
  `BD_MENU` (+ pinnable pushpins), `BD_TEXT` (single-line field),
  `BD_MULTILINE` (multi-line editor), `BD_INPUT_LINE`, and the `BD_TERMINAL`
  extension (libvt). Flexbox row/col + fixed layout.
- **Value widgets** (extensions) — slider, shaded knob, sliding toggle, scroll
  wheel, jog dial, X-Y pad.
- **Explorer widget** (extension) — model-driven icon grid with selection
  (single / Ctrl / Shift-range / rubber-band), drag-move, double-click
  activate, right-click context, keyboard nav, in-place rename, scissor clip.
- **Multiple native windows** — on the GLES backend (see the v0.3 section).
- **Keyboard focus** — click- and Tab/Shift-Tab traversal; `bd_focused()`.

Not yet built: `BD_LIST`, `BD_SCROLLBAR`, `BD_NOTICE`, `BD_TAB_BAR` (still
enum-only); the rich-text editor widget; IME/compose; clipboard; multitouch;
pen. These are tracked in the roadmap and widget-set sections.

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
| `BD_LIST`        | scrolling list (MUD list, log sink list)            | no   |
| `BD_SCROLLBAR`   | standalone scrollbar (paired with terminal pane)    | no   |
| `BD_MENU`        | menu bar / popup menu (with `BD_MENU_ITEM`); pinnable | yes |
| `BD_NOTICE`      | modal confirmation / alert                          | no   |
| `BD_TAB_BAR`     | tabs for multiple concurrent MUD sessions           | no   |
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
- textured sprite / quad (glyph atlas, icon textures, SIXEL/MXP later)
- text run (a stb_truetype atlas baked from the chrome TTF; the terminal
  extension keeps its own CP437 atlas)

These batch into one dynamic vertex buffer that flushes on texture change or
on `bd_draw_flush()`. A widget that needs an effect (the shaded knob, and the
explorer's scissor clip) flushes the batch and issues its own shader / scissor
through the backend, then resumes batching.

A dirty-region list driving per-frame redraws (idle to 0 Hz) is still planned;
the loop currently redraws each frame.

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
a textured sprite (`pushpin-out`/`pushpin-in` PNGs; replacing these with
scalable vector art is a todo). No new widget type.

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
  readout. Built by `make widget-test` (Linux/X11, opt-in), run from the
  repo root.

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

Still to do:

- **ludica / single-surface compositing.** `multi_window` is 0 on ludica,
  so only the primary frame shows; secondary frames need an in-surface
  window manager (the "backend composites frames itself" path). Deferred.
- Window decorations/raise/move/focus policy, and per-window hover-leave
  (moving in one window leaves stale hover in others).
- Popups still use global menu state; one open menu at a time across all
  windows.

### Explorer / icon-browser widget

A new widget type (v0.3, built as a `widget_ext` extension) modeled on
Explorer / PROGMAN.EXE (Win 3.x) / Finder: an arrangeable grid of labeled
icons with persistent positional state. It is
**not** tied to files; a callback/model interface lets the items be any
collection (a MUD client's server list, a DAW's known-plugin list), so the
view refreshes from live data without the app rebuilding it.

In `src/birdie/bd_widget_explorer.{c,h}`. Working: model query, grid/free
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

### IME, compose, and dead keys

Today the only text channel is `BD_EV_CHAR`, which carries a single
codepoint. That cannot represent a multi-codepoint commit, a compose/dead-key
sequence, or an in-progress composition, so CJK input, accented-letter
compose, and emoji variants are all broken or truncated at the source. The
gap is structural, not an X11 quirk: `bd_event` has no way to carry a string
or a preedit, so every backend hits the same wall. (A reference X11 backend
got as far as an `XIC` with `Xutf8LookupString` but had to disable preedit
and forward only the first codepoint of each commit.)

The complete solution is four seams, identical across X11 (XIM/IBus), Win32
(IMM/TSF), macOS (`NSTextInputClient`), and Wayland (text-input-v3):

- **Commit as a UTF-8 string.** Add `BD_EV_TEXT_COMMIT` carrying a
  `const char *` valid for the dispatch only. This also makes dead keys,
  compose, and paste fall out for free. `BD_EV_CHAR` may stay as a
  convenience for the ASCII path or be retired.
- **Preedit event.** `BD_EV_TEXT_PREEDIT` carries the in-progress string
  plus a caret offset (and optionally an underline/attribute range). The
  text widget renders it inline but does **not** insert it into the buffer
  until commit.
- **Caret-rect reporting (toolkit → backend).** The reverse channel that is
  missing today: `ime_set_cursor_rect(x, y, w, h)` so the platform positions
  its candidate window at the caret (XNSpotLocation / ImmSetCompositionWindow
  / `set_cursor_rectangle`).
- **Enable/disable on focus.** `ime_set_enabled(on)` as focus enters or
  leaves a text widget, so the IME does not swallow keys in games or other
  non-text widgets.

**Decision:** the platform IME draws its own candidate/conversion window;
the toolkit only reports the caret rect. Native candidate UI is the default
for XIM/IMM/TSF/macOS, so this is the smallest portable surface that is still
complete for CJK, compose, and dead keys. A toolkit-drawn candidate window
is only needed for a fully custom IME, which is out of scope.

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

### Editor widget (rich-text, row-oriented)

A higher-level text editor built as a **composition over `BD_MULTILINE`** (the
plain multi-line field above, still to do): the multiline supplies editable
text with caret, selection, and scrolling; the editor adds an API suited to
programmatic editing plus a rich-text styling layer. An extension
(`widget_ext`), not a core widget.

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

**Rendering implications.** The text renderer must draw per-run fg/bg and
styles. fg/bg, underline, strikeout, and super/subscript are cheap (vertex
color, extra line quads, a baseline offset + scale). **Bold and italic** need
either additional baked font faces (a bold and an oblique TTF) or synthetic
emboldening / shearing; the chrome currently bakes a single face, so rich text
is what introduces font-variant baking — decide whether to ship bold/italic
TTFs or synthesize them. A fixed-width face is desirable for code and ABC so
columns line up (super/subscript aside).

**Ordering.** (1) `BD_MULTILINE` — plain multi-line editing. (2) style-run
support in the renderer (per-run fg/bg + styles; font-variant baking for
bold/italic). (3) the editor widget — row API, lock, and highlight on top.

### Clipboard

Copy / cut / paste, backed by per-platform hooks (X11 selections, Win32
clipboard, macOS pasteboard, Wayland data-device). Paste reuses the same
string channel as IME commit, so it lands naturally alongside it. A text
editor without paste is incomplete.

### Focus traversal, key-up, and repeat

- **Tab / Shift-Tab** traversal between focusable widgets — **done**: cycles
  the current frame's input lines, buttons, and extension widgets (menus
  excluded), wrapping; focused buttons get a ring and Enter/Space activation;
  `bd_focused()` exposes the focus. Per-window scoping uses `ev->window`.
- **Key-up events and a repeat flag** on `bd_event`. Cheap to add while the
  struct is already being broken, and useful for held-key interactions. Still
  to do.

### Multitouch gestures

`bd_event` carries a single pointer position today. Multitouch adds touch
events with a **per-pointer id** (`BD_EV_TOUCH_DOWN/MOVE/UP`, each with an
id and position) so simultaneous contacts route independently. This extends
the existing single-global pointer-capture to **per-pointer capture**: the
headline use case is turning several knobs at once, one finger per knob, with
each contact captured by the widget it landed on. Baseline is raw per-finger
delivery; pinch/rotate gesture recognition can sit on top as optional
toolkit helpers or be left to the application.

Backends surface touches from their platform source (X11 XInput2 touch,
Win32 pointer input, Wayland touch, macOS).

### Pen tablet input and a drawing canvas

Support a pressure-sensitive pen (MPP 2.0 baseline: pressure, tilt X/Y,
barrel button, eraser, and hover/proximity). The pointer/touch event grows
pen fields: `pressure` (0..1), `tilt_x` / `tilt_y`, and a pen-flags field
(eraser, barrel button, in-range/hover). Backends source these from XInput2
valuators (X11), the Windows Ink / Pointer API (Win32), or tablet-v2
(Wayland).

The compelling consumer is a **drawing-canvas widget** that turns pen data
into variable-width strokes (pressure → width, tilt → nib shape), with hover
shown before contact and the eraser end recognized. It is a natural
`widget_ext` that renders through the v0.2 GPU interface.

## Open questions

- Text shaping for chrome: stick with bitmap atlas (fits GLES2 cleanly,
  matches the terminal aesthetic, skips HarfBuzz) or pull in a shaper?
  Lean bitmap for v1.0.
- IME / dead-key handling on Windows and Linux. Non-trivial; v1.0 may
  only support ASCII in the input line, with full IME deferred.
- How far to push the accessibility tree in v1.0. Minimum: every widget
  carries a `BD_ROLE` and `BD_NAME` attribute from day one, even if no
  platform accessibility bridge is wired up yet. Free future work.
