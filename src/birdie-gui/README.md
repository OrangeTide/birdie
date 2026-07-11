# birdie-gui

A small retained-mode GUI toolkit in C. You build a tree of widgets with a
tagged varargs API, and the toolkit lays them out, draws them, and routes
input. Drawing runs on a small GPU interface (shaders, vertex draws, uniforms,
textures, scissor) that ludica, SDL, raylib, or GLFW can all provide through
GLES, so the same widget code runs on any of them once a backend exists.

It has two API tiers:

- **App API** (`widget.h`) for building UIs out of widgets.
- **Extension API** (`widget_ext.h`) for defining new widget *types*. The VT
  terminal (`bd_widget_vt.h`), the value widgets (`bd_widget_value.h`), and the
  explorer/icon browser (`bd_widget_explorer.h`), and the rich-text editor
  (`bd_widget_editor.h`) are built on it; `bd_widget_vt.c` is the reference.

## Vendoring into your project

birdie-gui is distributed as a source ZIP, one per tagged release. The
`get-birdie-gui.sh` script (shipped in this bundle) downloads a release and
drops it into a vendor directory in your project, so you only ever vendor a
tagged, CI-built version. It pulls the widget library only, not the birdie MUD
client it lives in.

To add it to a project, run the script straight from the repo (it defaults to
the canonical GitHub release assets, so no configuration is needed):

```sh
curl -fsSL https://raw.githubusercontent.com/OrangeTide/birdie/main/scripts/get-birdie-gui.sh \
    | sh -s -- 0.7.0 third_party/birdie-gui
```

That vendors birdie-gui 0.7.0 into `third_party/birdie-gui/`. Re-run it with a
newer version to update in place; it refuses to overwrite a directory that is
not a prior birdie-gui checkout unless you pass `--force`. To pull from a fork
or mirror, set `BIRDIE_GUI_REPO`. Commit the script to your repo so updates are
a one-liner:

```sh
scripts/get-birdie-gui.sh 0.7.0            # into third_party/birdie-gui by default
scripts/get-birdie-gui.sh --help           # options and environment variables
```

## Building

The bundle is a self-contained copy of the toolkit's in-tree directory: sources
and public headers sit flat in the bundle root next to its `module.mk` for
[modular-make](https://github.com/OrangeTide/modular-make), with a vendored
`thirdparty/stb/`, the terminal under `bd_vt/`, and the reference backends as
loose source. It is the *same* `module.mk` the toolkit is built with in its own
tree, so there is nothing bundle-specific to drift out of date.

`module.mk` declares one library, `birdie_gui` — the backend-agnostic toolkit
(widget core, renderer, extension widgets, UTF-8 codec; no backend, no
terminal). Add the vendored directory to your project's `SUBDIRS` and list
`birdie_gui` in your executable's `LIBS`, plus a backend of your own:

```makefile
SUBDIRS += third_party/birdie-gui

myapp_SRCS = main.c bd_backend_ludica.c   # your host + a backend
myapp_LIBS = birdie_gui
```

The terminal is a separate library, `birdie_gui_vt` (the VT escape-sequence
engine plus the `BD_TERMINAL` widget), declared by `bd_vt/module.mk`. It is
**opt-in**: add its directory as a second `SUBDIR` only when you want a
terminal, so a terminal-free UI never compiles the VT engine or its Unicode
width tables. `birdie_gui_vt` depends on `birdie_gui` transitively:

```makefile
SUBDIRS += third_party/birdie-gui third_party/birdie-gui/bd_vt

myapp_LIBS = birdie_gui_vt        # pulls in birdie_gui
```

`module.mk` exports the bundle root on your include path, so `#include
"widget.h"` and the rest of the public API resolve with no extra `-I`;
`birdie_gui_vt` likewise exports `bd_vt/`, so `#include "bd_widget_vt.h"`
resolves when you link it. Neither builds a backend, so compile one of your own
into your target: the bundle ships `bd_backend_ludica.c` and
`bd_backend_sdl3.c` flat in the root, and the raw X11/EGL/GLES one in
`backend-gles/`; see "Porting to another backend" below.

Not using modular-make? The toolkit is the `bd_*.c` / `widget.c` files in the
bundle root (skip the `bd_backend_*` backends). Compile them with `-I.
-Ithirdparty/stb`. For the terminal, add the `bd_vt/*.c` files and `-Ibd_vt`.
The gallery command under "Bundled X11/GLES backend and gallery" shows the full
compile of the library plus a backend in one line.

## Usage

A minimal app using the reference ludica backend. The host owns the main loop
and feeds the toolkit three things each frame: a layout pass, a render pass,
and translated input events.

```c
#include "widget.h"
#include "bd_widget_vt.h"
#include "bd_backend_ludica.h"
#include "ludica.h"

static bd_id terminal;

static void
on_quit(bd_id id, void *arg)
{
    (void)id; (void)arg;
    lud_quit();
}

static void
init(void)
{
    bd_gui_init(&bd_backend_ludica, NULL);   /* NULL theme = defaults */

    bd_id frame = bd_create(BD_NONE, BD_FRAME,
        BD_LABEL_S,  "Demo",
        BD_LAYOUT_I, BD_LAYOUT_COL,
        BD_END);

    terminal = bd_terminal_create(frame, BD_GROW_I, 1, BD_END);
    bd_terminal_write(terminal, "hello from birdie-gui\r\n", -1);

    bd_create(frame, BD_BUTTON,
        BD_LABEL_S,    "Quit",
        BD_PREF_H_I,   28,
        BD_ON_CLICK_F, on_quit,
        BD_END);
}

static void
frame(float dt)
{
    (void)dt;
    bd_gui_layout(lud_width(), lud_height());
    bd_gui_render();
}

static int
on_event(const lud_event_t *ev)
{
    bd_event bev;
    if (bd_event_from_lud(ev, &bev) && bd_gui_event(&bev))
        return 1;   /* consumed by the GUI */
    return 0;
}

static void
cleanup(void)
{
    bd_gui_cleanup();
}

int
main(int argc, char **argv)
{
    return lud_run(&(lud_desc_t){
        .app_name = "demo", .width = 800, .height = 500, .resizable = 1,
        .gles_version = 3,   /* the toolkit's shaders are #version 300 es */
        .argc = argc, .argv = argv,
        .init = init, .frame = frame, .cleanup = cleanup, .event = on_event,
    });
}
```

`bd_create(parent, type, ...)` takes a `BD_END`-terminated list of attribute
id / value pairs. Pass `BD_NONE` as the parent for the root. Attributes cover
geometry (`BD_PREF_W_I`, `BD_GROW_I`, `BD_PAD_I`, `BD_GAP_I`), layout
(`BD_LAYOUT_I` with `BD_LAYOUT_ROW`/`COL`/`FIXED`), text (`BD_LABEL_S`), color
(`BD_FG_C`, `BD_BG_C`), and callbacks (`BD_ON_CLICK_F`). Built-in types include
`BD_FRAME`, `BD_PANEL`, `BD_LABEL`, `BD_BUTTON`, `BD_MENU`, `BD_TEXT_FIELD`,
`BD_TEXT_AREA`, `BD_LIST`, `BD_TAB_BAR`, `BD_SCROLLBAR`, `BD_NOTICE`, and
`BD_INPUT_LINE`. `BD_NOTICE` is a modal alert/confirm:
`bd_notice_open(message, "Yes\nNo", cb, arg)` shows a centered panel over a
dimmed backdrop and blocks the rest of the UI; the callback gets the chosen
button index (-1 for Escape) and it closes. `BD_SCROLLBAR` is a standalone
scrollbar (orientation follows its shape;
`bd_scrollbar_set(id, pos, frac)` / `bd_scrollbar_value`; drag fires
`BD_ON_CLICK_F`). `BD_TAB_BAR` is a
row of skeuomorphic folder tabs (labels via `BD_LABEL_S`/`bd_tabbar_set_tabs`;
`bd_tabbar_active`/`bd_tabbar_set_active`; `BD_ON_CLICK_F` fires on tab change).
`BD_LIST` is a
scrolling/selectable list of `\n`-separated items (`BD_LABEL_S` or
`bd_list_set_items`); `bd_list_selected`/`bd_list_select` read/set the row and
`BD_ON_CLICK_F` fires on activation (double-click or Enter). `BD_TEXT_FIELD` is a
single-line field:
set/read its contents with `bd_set`/`bd_get_s(id, BD_LABEL_S)`; Enter fires
`BD_ON_CLICK_F` without clearing (`BD_INPUT_LINE` clears, for a command line).
`BD_TEXT_AREA` is the multi-line editor (Enter inserts a newline; Up/Down,
line-relative Home/End, vertical scroll, click-to-caret); same
`BD_LABEL_S`/`bd_get_s` for its `\n`-separated text.

## Theme and palette

The chrome theme and the terminal palette are runtime-configurable, defaulting
to compile-time values you can override with `-DBD_TH_*` / `-DBD_TERM_*`.

```c
bd_theme th = bd_theme_default();
th.focus = 0x88CCFFFFu;          /* RGBA8 */
bd_gui_init(&bd_backend_ludica, &th);

bd_palette pal = bd_palette_default();
pal.ansi[1] = 0xE06C75FFu;       /* remap palette index 1 (red) */
bd_terminal_set_palette(terminal, &pal);
```

## Value widgets

The toolkit ships a family of interactive controls in `bd_widget_value.h`,
built on the extension API. They drag to change, fire a callback, and match the
chrome theme; the rotary ones are drawn in fragment shaders sharing a
brushed-aluminum look.

```c
#include "bd_widget_value.h"

/* slider — horizontal or vertical, value normalized to [0,1] */
bd_slider_create(row, BD_HORIZONTAL, 0.5f, on_change, NULL, BD_GROW_I, 1, BD_END);

/* knob — a value range, optional step, and a dial plate */
bd_knob_create(row, &(bd_knob_desc){
    .min = 0, .max = 127, .value = 64,
    .dial = BD_DIAL_LABELS, .hex = 1,    /* MIDI-CC style hex labels */
    .cb = on_change,
}, BD_PREF_W_I, 120, BD_END);

/* jog dial — the knob's endless/relative mode (emits drag deltas, dimples,
   no indicator) */
bd_knob_create(row, &(bd_knob_desc){ .relative = 1, .dimples = 3,
    .cb = on_jog }, BD_PREF_W_I, 84, BD_END);

/* toggle — a skeuomorphic on/off slide switch */
bd_toggle_create(row, 1, on_toggle, NULL, BD_PREF_W_I, 56, BD_END);

/* scroll wheel — a ribbed jog cylinder, relative output */
bd_wheel_create(row, BD_VERTICAL, on_spin, NULL, BD_PREF_W_I, 30, BD_END);

/* X-Y pad — two values in [0,1], square or circular (joystick) limit */
bd_xypad_create(row, &(bd_xypad_desc){ .shape = BD_XY_CIRCLE, .spring = 1,
    .x = 0.5f, .y = 0.5f, .cb = on_xy }, BD_PREF_W_I, 76, BD_END);
```

| Widget | Create | Output |
|---|---|---|
| Slider | `bd_slider_create(parent, orient, value, cb, arg, ...)` | absolute `[0,1]`; `bd_slider_get/set` |
| Knob | `bd_knob_create(parent, &bd_knob_desc, ...)` | absolute `[min,max]`; `bd_knob_get/set`. `.step` = N-way rotary switch; `.dial` = `BD_DIAL_DOTS`/`BALANCE`/`LABELS`/`NONE` |
| Jog dial | `bd_knob_create` with `.relative = 1` | relative deltas; `.dimples` finger grips, no indicator |
| Toggle | `bd_toggle_create(parent, on, cb, arg, ...)` | boolean; `bd_toggle_get/set` |
| Scroll wheel | `bd_wheel_create(parent, orient, cb, arg, ...)` | relative spin deltas |
| X-Y pad | `bd_xypad_create(parent, &bd_xypad_desc, ...)` | two values `[0,1]`; `bd_xypad_get/set`. `.shape` square/circle, `.spring` |

The knob is drawn in a fragment shader: a brushed-aluminum face with an
anisotropic circular highlight and an orange-red indicator over a 270-degree
sweep. The toggle, wheel, X-Y pad puck, and jog dial reuse that metal shading
(`bd_widget_value.c` is the reference for shader-drawn widgets).

## Explorer widget

`bd_widget_explorer.h` is a model-driven icon browser (Explorer / PROGMAN /
Finder style): an arrangeable grid of labeled icons. It is not tied to files;
you supply a model (item count, per-item fetch, optional position/name
persistence) and the widget renders and edits a view of it, re-querying on
`bd_explorer_refresh()` so dynamic data stays in sync.

```c
#include "bd_widget_explorer.h"

static int   srv_count(void *c) { (void)c; return n_servers; }
static void  srv_get(void *c, int i, bd_explorer_item *out) {
    (void)c;
    out->key = servers[i].id;        /* stable id; selection/state key on this */
    out->label = servers[i].name;
    out->icon = servers[i].icon;     /* 0 = default placeholder */
    out->enabled = 1;
    out->x = servers[i].x; out->y = servers[i].y;  /* <0 = auto-place */
    out->user = &servers[i];
}
static void  srv_set_pos(void *c, uint64_t key, int x, int y)  { /* persist */ }
static void  srv_set_name(void *c, uint64_t key, const char *s){ /* rename  */ }

bd_explorer_create(parent,
    &(bd_explorer_model){ .count = srv_count, .get = srv_get,
        .set_pos = srv_set_pos, .set_name = srv_set_name },
    &(bd_explorer_cb){ .activate = on_open, .context = on_menu },
    BD_GROW_I, 1, BD_END);
```

Interaction: click / Ctrl-click / Shift-range / rubber-band selection,
drag-move (commits via `set_pos`), double-click `activate`, right-click
`context` (screen coords, for the app to pop a menu), wheel scroll, keyboard
nav (focus it, then arrows / Home / End / Ctrl-A / Enter), and in-place rename
(F2 or `bd_explorer_begin_rename`, committing via `set_name`). Content is
scissor-clipped to the panel. Query/poke selection with
`bd_explorer_selection` / `bd_explorer_select`.

## Editor widget (rich text)

`bd_widget_editor.h` is a row-oriented text editor with the `BD_TEXT_AREA`
editing model plus a rich-text styling layer, for a small code or ABC-notation
music editor. Text is plain UTF-8; styling is a separate list of style runs
over byte ranges, so a syntax highlighter (emit runs on change) and a transient
highlight (mark the playing row) use the same mechanism.

```c
#include "bd_widget_editor.h"

bd_id ed = bd_editor_create(parent, BD_GROW_I, 1, BD_END);
bd_editor_set_text(ed, "X:1\nT:Tune\nK:C\nCDEF GABc|");

/* row API */
int  rows = bd_editor_row_count(ed);
char line[128]; bd_editor_row_text(ed, 3, line, sizeof line);
bd_editor_replace_row(ed, 3, "GFED CBAG|");
bd_editor_insert_row(ed, 1, "T:Subtitle");
bd_editor_delete_row(ed, 1);

bd_editor_set_locked(ed, 1);             /* read-only (styling still works) */

/* styling: fg/bg + bold/italic/underline/strikeout/super/subscript */
bd_editor_highlight_row(ed, 3,
    (bd_rich_style){ BD_RT_BOLD, 0x202020FFu, 0xFFD54AFFu });   /* playing row */
bd_editor_style_span(ed, 0, 3,
    (bd_rich_style){ BD_RT_BOLD | BD_RT_UNDERLINE, 0x7FB2FFFFu, 0 });
bd_editor_clear_styles(ed);
```

The renderer draws runs segment by segment: per-run fg/bg, underline,
strikeout, true bold/italic (separate baked faces; see Dependencies), and
super/subscript. The editor renders in a fixed-width face by default (good for
code/ABC column alignment); `bd_editor_set_monospace(ed, 0)` switches to the
proportional face.

## Defining a widget type

An extension fills a `bd_widget_class` and registers it for a type id, then
wraps the core create call in a typed constructor. Hooks are optional; the
core owns per-instance `state` of the size you declare and routes pointer
events (with drag capture) to the `event` hook.

```c
#include "widget_ext.h"
#include "bd_draw.h"

struct gauge { float value; };

static void
gauge_render(bd_id id, void *state)
{
    struct gauge *g = state;
    int x, y, w, h;
    bd_widget_rect(id, &x, &y, &w, &h);
    bd_draw_rect(x, y, w * g->value, h, 0x4DB2FFFFu);
}

static const bd_widget_class gauge_class = {
    .name = "gauge", .state_size = sizeof(struct gauge), .render = gauge_render,
};

static int gauge_type;

bd_id
gauge_create(bd_id parent, ...)
{
    if (!gauge_type) gauge_type = bd_register_widget_class(&gauge_class);
    va_list ap; va_start(ap, parent);
    bd_id id = bd_create_va(parent, gauge_type, ap);
    va_end(ap);
    return id;
}
```

Render with the toolkit helpers in `bd_draw.h` (`bd_draw_rect`,
`bd_draw_rect_lines`, `bd_draw_sprite`, `bd_draw_text`). For a fully custom look
a widget can drop to the GPU: call `bd_draw_flush()` to land the batched chrome,
then issue its own shader through `bd_backend_get()`
(`make_shader` / `use_shader` / `set_uniform_*` / `draw_verts`). The knob in
`bd_widget_value.c` is the reference for a shader-drawn widget; `bd_widget_vt.c`
is the reference for a stateful, libvt-backed one.

## Porting to another backend

Implement the `bd_backend` GPU interface in `bd_backend.h`:

- window/frame: `width`, `height`, `time`, `viewport`, `clear` (optional: leave
  NULL to clear the frame yourself, e.g. to composite the UI over a 3D scene)
- shaders: `make_shader` / `destroy_shader` / `use_shader` and the
  `set_uniform_*` setters
- geometry: `draw_verts` (a triangle list of `bd_vertex`, layout
  `a_pos`/`a_uv`/`a_col` at locations 0/1/2); it also sets the 2D render state
  (blend on, depth/cull off), so the UI renders correctly whether or not
  `clear` ran
- textures: `load_texture` / `make_texture` / `update_texture` / `bind_texture`
  / `destroy_texture`
- clipping: `scissor` / `scissor_off`

Then translate your windowing library's input into the neutral `bd_event`. Pass
your backend to `bd_gui_init`. The toolkit owns the renderer
(`bd_draw.c`: one default shader, a dynamic quad batch, and an stb_truetype
glyph atlas), so a backend supplies GPU plumbing, not drawing primitives.
Shaders are GLES3 (`#version 300 es`); alpha blending is on.
The bundle ships three reference backends to read or reuse: `bd_backend_ludica.c`
(ludica), `bd_backend_sdl3.c` (SDL3 + GLES3, a good copy-and-adapt starting
point for a new windowing library; its demo host is `sdl3_example.c` in the
birdie repo), and the raw X11/EGL/GLES one in `backend-gles/`, which also drives
the standalone widget gallery.

**Clipboard (optional).** Provide `clipboard_set(utf8)` / `clipboard_get()`
and the text fields gain Ctrl-C/X/V; the GLES backend implements them on the
X11 CLIPBOARD selection. Leave them NULL for no clipboard (a safe no-op).

**IME / compose (optional).** Deliver finished text as `BD_EV_TEXT_COMMIT`
(`bd_event.text`, the full UTF-8 string) so multi-codepoint commits, dead
keys, and compose work, and provide `ime_set_enabled(on)` /
`ime_set_cursor_rect(x,y,w,h)` so the toolkit can focus the input method only
on text fields and place the candidate window at the caret. Optionally emit
`BD_EV_TEXT_PREEDIT` for inline composition. The GLES backend does this on
X11/XIM (the IME draws its own candidate window). With no hooks, plain
`BD_EV_CHAR` still types ASCII.

**Multitouch (optional).** Emit `BD_EV_TOUCH_DOWN/MOVE/UP` with a per-pointer
`bd_event.touch` id and the toolkit captures each finger independently,
synthesizing mouse events to the widget it landed on — so several knobs/sliders
drag at once. The GLES backend sources touches from X11 XInput2 (`-lXi`).

**Pen / tablet (optional).** Emit `BD_EV_PEN_HOVER/DOWN/MOVE/UP` with
`bd_event.pressure`, `tilt_x`/`tilt_y`, and a `pen_flags` bitmask
(`BD_PEN_INRANGE`/`BD_PEN_BARREL`/`BD_PEN_ERASER`). Contact captures the
extension widget under the tip for the whole stroke; hover tracks it without
capturing. `bd_widget_sketch` (`bd_sketch_create`) is a drawing surface that
turns this into variable-width ink (pressure → width), with the barrel button
as a second ink and the eraser end removing strokes; it falls back to mouse
drags at full pressure. The GLES backend sources the stylus from X11 XInput2
valuators (`-lXi`).

**Multiple windows (optional).** Set `multi_window = 1` and provide the
`window_*` hooks (`window_open` / `close` / `begin` / `swap` / `width` /
`height` / `set_title`). Each top-level `BD_FRAME` then maps to a native
window: the toolkit renders each between `window_begin`/`window_swap` and tags
events with the originating window id (`bd_event.window`); `bd_frame_for_window`
maps an id back to its frame. Leave the hooks NULL and `multi_window = 0` for a
single-window host, the default. Keyboard focus moves on click and on
Tab / Shift-Tab; `bd_focused()` reports it.

## Bundled X11/GLES backend and gallery

`backend-gles/` is a ready-to-use raw X11 + EGL + OpenGL ES 3 backend
(`window.h`, `x11_window.c`, `bd_backend_gles.{c,h}`) plus a standalone widget
gallery (`widget_test.c`) that exhibits every widget — a working non-ludica
example. Its GPU code (shaders, quad batch, textures, scissor) lives in the
shared `bd_backend_gles_core.{c,h}` (bundle root), which `bd_backend_sdl3.c`
uses too; compile `bd_backend_gles_core.c` alongside whichever GLES backend you
pick, plus `bd_gl.c` for the built-in GL loader (see "Built-in GL loader"
below). Build it (Linux) from the bundle root, with the toolkit sources and
`bd_vt/` on the include path (`bd_vt/*.c` supplies the terminal):

```sh
cc -I. -Ibackend-gles -Ithirdparty/stb -Ithirdparty/khronos -Ibd_vt \
   -DBD_GL_LOADER_BUILTIN \
   backend-gles/widget_test.c backend-gles/x11_window.c \
   backend-gles/bd_backend_gles.c bd_backend_gles_core.c bd_gl.c \
   widget.c bd_draw.c bd_asset.c bd_utf8.c bd_color.c \
   bd_widget_value.c bd_widget_explorer.c bd_widget_editor.c \
   bd_widget_sketch.c bd_widget_table.c bd_widget_tree.c bd_widget_inventory.c \
   bd_widget_icon.c bd_widget_dock.c bd_widget_actionbar.c bd_widget_tabview.c \
   bd_widget_indicator.c bd_widget_meter.c bd_widget_progress.c bd_widget_chart.c \
   bd_vt/*.c \
   -lX11 -lXi -lEGL -lGLESv2 -lm -o assets/gallery
```

For the external-loader variant (link GL symbols directly instead of loading
them at runtime), drop `bd_gl.c` and `-Ithirdparty/khronos`, and swap
`-DBD_GL_LOADER_BUILTIN` for `-DBD_GL_LOADER_EXTERNAL`.

The toolkit finds its fonts by their relative sub-path (`fonts/…`), located
next to the executable or in the current directory. The bundle keeps them under
`assets/`, so build the binary into `assets/` (as above) and run it from there:
`cd assets && ./gallery`. (The pushpins are 1-bit glyphs compiled into the
toolkit, no file needed.)

## Built-in GL loader

The GLES-core backend (`bd_backend_gles_core.c`, shared by the SDL3 and X11
backends) calls OpenGL ES 3.0 entry points. How those `gl*` symbols are made
available is a compile-time choice, because it differs by platform. On Windows
`opengl32.dll` exports only GL 1.1, so ES 3.0 entry points **must** be resolved
at runtime; you cannot link them.

Two modes, selected by a preprocessor define:

- **`-DBD_GL_LOADER_BUILTIN`** (recommended, and the in-tree default) — compile
  `bd_gl.c` too. It resolves every `gl*` the backend uses through a getproc
  callback (`SDL_GL_GetProcAddress`, `eglGetProcAddress`, or your own). The
  backend's `GLES3/gl3.h` shim redirects the unchanged `gl*` call sites to the
  loaded pointers, so no GL library is linked. Call `bd_gles_load_gl(getproc)`
  once after making the GL context current, before any draw (the SDL3 and X11
  backends already do this; a custom backend must). Put `-Ithirdparty/khronos`
  on the include path **after** `-I.` so the shim's `#include_next` finds the
  vendored Khronos headers — this is what lets it compile on Windows, which
  ships no `GLES3/gl3.h`.
- **`-DBD_GL_LOADER_EXTERNAL`** — you already have the `gl*` symbols (GLEW,
  GLAD, Galogen, or direct linking such as `-lGLESv2` on Linux). `bd_gl.c` is
  not compiled, the shim passes through to the real header, and
  `bd_gles_load_gl()` is a no-op returning success, so call sites stay
  unconditional.

The vendored `thirdparty/khronos/` holds the Khronos ES 3.0 core headers
(MIT / Apache-2.0; see its `UPSTREAM`), needed only by builtin mode.

## Dependencies

- A **GLES-capable backend**. The bundle ships three: ludica
  (`bd_backend_ludica.c`), SDL3 (`bd_backend_sdl3.c`), and raw
  X11/EGL/GLES (`backend-gles/`). SDL, raylib, GLFW, or ANGLE can host it too.
- **The terminal engine** (adopted from libvt) backs the terminal widget
  (`bd_widget_vt.c`) and ships under `bd_vt/`, built together with the widget as
  the `birdie_gui_vt` library; no need to supply it yourself. A terminal-free UI
  links `birdie_gui` alone and skips it.
- Vendored: **stb_truetype** (text rasterization) and eight chrome TTFs the
  toolkit bakes — a proportional family (DejaVu Sans) and a fixed-width family
  (DejaVu Sans Mono), each in regular / bold / oblique / bold-oblique.
  `bd_draw_text_styled(s, x, y, rgba, BD_FONT_BOLD|BD_FONT_ITALIC|BD_FONT_MONO)`
  selects a face; a missing variant TTF falls back to regular. The editor uses
  the mono family by default (`bd_editor_set_monospace`).

The assets the toolkit loads on disk (the chrome TTFs) are named only by their
asset-root-relative sub-path (`fonts/…`; the terminal font and the pushpins
need none — they are 1-bit bitmaps compiled into the toolkit). The backend's
`resolve_asset` hook locates them next to the executable (an installed layout),
and otherwise they are read relative to the current directory. Stage the
`assets/` tree next to your binary, or compile the fonts into it and register
them so no files are read at all -- see "Using your own fonts" and
"Embedding assets" below.

## Using your own fonts

You do not have to place font files on disk at all to ship a custom family:
fill a `bd_font_set` and pass it once. Each of the eight faces is a
`bd_font_face` that is either a filesystem path or an in-memory TTF/OTF buffer;
a face left zeroed falls back to `regular` at draw time.

```c
bd_font_set fonts = {
    .regular          = { .path = "fonts/Inter-Regular.otf" },
    .bold             = { .path = "fonts/Inter-Bold.otf" },
    .italic           = { .path = "fonts/Inter-Italic.otf" },
    .bold_italic      = { .path = "fonts/Inter-BoldItalic.otf" },
    .mono             = { .path = "fonts/JetBrainsMono-Regular.ttf" },
    /* ...mono_bold, mono_italic, mono_bold_italic... */
};
bd_gui_init_fonts(backend, theme, &fonts);   /* or NULL for the defaults */
```

To make the binary self-contained, point the faces at bytes compiled into it
(e.g. via an `.incbin` blob) instead of paths — no files are read:

```c
extern const unsigned char font_regular[];   /* embedded blob */
extern const unsigned long font_regular_len;
bd_font_set fonts = {
    .regular = { .data = font_regular, .len = (long)font_regular_len },
    /* ...other faces... */
};
bd_gui_init_fonts(backend, theme, &fonts);
```

If you would rather keep paths but resolve them from an embedded asset store
(matching how the backend's `load_texture` works), install a reader before
init; it is consulted first and `fopen` is the fallback:

```c
/* return a malloc'd buffer the renderer frees, or NULL to fall back to fopen */
unsigned char *my_reader(const char *path, long *len) { ... }

bd_draw_set_font_reader(my_reader);
bd_gui_init(backend, theme);   /* faces given by path now go through my_reader */
```

The lower-level renderer entry points (`bd_draw_init_fonts`,
`bd_draw_set_font_reader`) are in `bd_draw.h` if you are not using the widget
layer. `bd_draw_init(be, path, px)` remains and is unchanged: it bakes `path`
as the regular face and resolves the seven variants by id / built-in
relative name.

## Embedding fonts, and custom fonts

The toolkit requests each font by a **generic identifier**, not a filename:
`BD_ASSET_FONT_REGULAR` and the seven other `BD_ASSET_FONT_*` faces (all in
`bd_asset.h`). Register a source under an id and the toolkit uses it instead of
the built-in default. A source is **either a file path or in-memory data**, so
the same mechanism covers "use a different font on disk" and "ship a
self-contained binary". (The pushpins and terminal font are 1-bit bitmaps
compiled into the toolkit, so they are never loaded or embedded.)

Point an id at a file (no recompiling, no embedding) -- e.g. a user-chosen font:

```c
#include "bd_asset.h"
bd_asset_register_file(BD_ASSET_FONT_REGULAR, "/home/me/.fonts/Inter.otf");
bd_asset_register_file(BD_ASSET_FONT_BOLD,    "/home/me/.fonts/Inter-Bold.otf");
bd_gui_init(backend, theme);   /* the rest fall back to the built-in faces */
```

Or embed the font blobs for a **single self-contained binary**, all through one
registry:

```c
extern const unsigned char font_ui[];    extern const unsigned char font_ui_end[];

/* register BEFORE bd_gui_init*; keyed by identity, not by any filename */
bd_asset_register_data(BD_ASSET_FONT_REGULAR,  font_ui,    font_ui_end    - font_ui);

bd_gui_init(backend, theme);
```

There is no need to name your font "DejaVuSans.ttf" or override any build macro;
you register it as *the regular UI font*. Anything left unregistered falls back
to the default file, so partial embedding is fine (embed the fonts, load others
from disk, or vice versa). Registered data and path strings are **borrowed**:
they must outlive every use, which a `.rodata` blob satisfies. (For fonts you can
still pass a whole `bd_font_set` to `bd_gui_init_fonts` instead; the registry is
the lighter route and also covers the textures.) Serving embedded PNG data needs
the backend's optional `load_texture_mem` hook -- the three bundled backends
implement it.

A minimal way to produce the blobs with GNU `as` (no codegen tool needed):

```asm
    .section .rodata
    .global font_ui
    .global font_ui_end
    .balign 4
font_ui:     .incbin "assets/fonts/MyFont.otf"
font_ui_end:
```

### Build paths stay out of the binary

There is nothing to configure here. The registry keys are the fixed `BD_ASSET_*`
id strings (short, generic), and the only file names baked in are the built-in
fonts' short relative sub-paths (`fonts/DejaVuSans.ttf`) -- no absolute paths,
no build-machine layout, identical in every build. The backend's
`resolve_asset` hook locates those next to the executable at runtime; keep an
`.incbin` source path (a build-time detail) machine-independent and it never
reaches the binary either. The bundled `examples/embed/` demonstrates registering
every asset by id so a fully self-contained binary reads no files at all.

## License

Made by a machine. PUBLIC DOMAIN (CC0-1.0)
