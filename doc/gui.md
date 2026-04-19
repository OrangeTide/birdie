# GUI

Birdie implements a **thin retained-mode widget layer** of its own, drawing
through `ludica` primitives. No third-party widget toolkit (no ImGui, no
Nuklear, no GTK/FLTK). The API style follows XView: objects are created with
an attribute-list constructor, queried and mutated through get/set calls.

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

## v1.0 widget set

Kept intentionally small. If it is not on this list, it is not in v1.0.

| widget           | role                                                |
|------------------|-----------------------------------------------------|
| `BD_FRAME`       | top-level window                                    |
| `BD_PANEL`       | layout container                                    |
| `BD_LABEL`       | read-only text                                      |
| `BD_BUTTON`      | clickable action                                    |
| `BD_TEXT`        | single-line text input                              |
| `BD_MULTILINE`   | multi-line text input (prefs notes, script edit)    |
| `BD_LIST`        | scrolling list (MUD list, log sink list)            |
| `BD_SCROLLBAR`   | standalone scrollbar (paired with terminal pane)    |
| `BD_MENU`        | menu bar / popup menu (with `BD_MENU_ITEM`); pinnable |
| `BD_NOTICE`      | modal confirmation / alert                          |
| `BD_TAB_BAR`     | tabs for multiple concurrent MUD sessions           |
| `BD_TERMINAL`    | the MUD output widget (custom renderer)             |
| `BD_INPUT_LINE`  | the MUD command input (history, completion)         |

`BD_TERMINAL` and `BD_INPUT_LINE` are the only two widgets birdie couldn't
have built against an off-the-shelf toolkit anyway; they will be the
largest widgets by code volume. The rest are small.

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

All drawing goes through `ludica`. Widgets render themselves via a small
set of primitives:

- filled / stroked rect, rounded rect (OPEN LOOK obround buttons)
- line, polyline
- triangle (for pull-down / pull-right glyphs, in the OPEN LOOK style)
- textured quad (glyph atlas, SIXEL tiles, MXP images later)
- text run (using the CP437 atlas from `ludica/samples/ansiview` for the
  terminal; a separate proportional atlas for chrome)

A dirty-region list drives per-frame redraws; when the dirty list is
empty the main loop blocks on the event queue (network fd + input) with
no GPU work.

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

Implementation sketch: a pinned menu is just a menu whose lifetime is
decoupled from the SELECT release that opened it. The widget gains
`BD_MENU_PIN_B` (a `BD_B` boolean attribute) that the user can read or
set programmatically. The pin glyph is drawn with our existing
primitives (circle + rect stem). No new widget type.

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

## Open questions

- Text shaping for chrome: stick with bitmap atlas (fits GLES2 cleanly,
  matches the terminal aesthetic, skips HarfBuzz) or pull in a shaper?
  Lean bitmap for v1.0.
- IME / dead-key handling on Windows and Linux. Non-trivial; v1.0 may
  only support ASCII in the input line, with full IME deferred.
- How far to push the accessibility tree in v1.0. Minimum: every widget
  carries a `BD_ROLE` and `BD_NAME` attribute from day one, even if no
  platform accessibility bridge is wired up yet. Free future work.
