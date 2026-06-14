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
`BD_FRAME`, `BD_PANEL`, `BD_LABEL`, `BD_BUTTON`, `BD_MENU`, `BD_TEXT`,
`BD_MULTILINE`, `BD_LIST`, and `BD_INPUT_LINE`. `BD_LIST` is a
scrolling/selectable list of `\n`-separated items (`BD_LABEL_S` or
`bd_list_set_items`); `bd_list_selected`/`bd_list_select` read/set the row and
`BD_ON_CLICK_F` fires on activation (double-click or Enter). `BD_TEXT` is a
single-line field:
set/read its contents with `bd_set`/`bd_get_s(id, BD_LABEL_S)`; Enter fires
`BD_ON_CLICK_F` without clearing (`BD_INPUT_LINE` clears, for a command line).
`BD_MULTILINE` is the multi-line editor (Enter inserts a newline; Up/Down,
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

`bd_widget_editor.h` is a row-oriented text editor with the `BD_MULTILINE`
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

- window/frame: `width`, `height`, `time`, `viewport`, `clear`
- shaders: `make_shader` / `destroy_shader` / `use_shader` and the
  `set_uniform_*` setters
- geometry: `draw_verts` (a triangle list of `bd_vertex`, layout
  `a_pos`/`a_uv`/`a_col` at locations 0/1/2)
- textures: `load_texture` / `make_texture` / `update_texture` / `bind_texture`
  / `destroy_texture`
- clipping: `scissor` / `scissor_off`

Then translate your windowing library's input into the neutral `bd_event`. Pass
your backend to `bd_gui_init`. The toolkit owns the renderer
(`bd_draw.c`: one default shader, a dynamic quad batch, and an stb_truetype
glyph atlas), so a backend supplies GPU plumbing, not drawing primitives.
Shaders are GLES3 (`#version 300 es`); alpha blending is on.
`bd_backend_ludica.c` is the reference; a raw X11/EGL/GLES backend lives in
`src/guitest/` (which also has a standalone widget gallery).

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
example. Build it (Linux; needs libvt for the terminal widget) with the asset paths
pointed at the bundle's `assets/` (the compiled-in defaults assume birdie's
source tree):

```sh
cc -Iinclude -Ibackend-gles -Ithirdparty/stb -I<libvt> \
   -DBD_ASSET_GUI_FONT='"assets/fonts/DejaVuSans.ttf"' \
   -DBD_ASSET_GUI_FONT_BOLD='"assets/fonts/DejaVuSans-Bold.ttf"' \
   -DBD_ASSET_GUI_FONT_ITALIC='"assets/fonts/DejaVuSans-Oblique.ttf"' \
   -DBD_ASSET_GUI_FONT_BOLDITALIC='"assets/fonts/DejaVuSans-BoldOblique.ttf"' \
   -DBD_ASSET_GUI_FONT_MONO='"assets/fonts/DejaVuSansMono.ttf"' \
   -DBD_ASSET_GUI_FONT_MONO_BOLD='"assets/fonts/DejaVuSansMono-Bold.ttf"' \
   -DBD_ASSET_GUI_FONT_MONO_ITALIC='"assets/fonts/DejaVuSansMono-Oblique.ttf"' \
   -DBD_ASSET_GUI_FONT_MONO_BOLDITALIC='"assets/fonts/DejaVuSansMono-BoldOblique.ttf"' \
   -DBD_ASSET_TERM_FONT='"assets/font8x16.png"' \
   -DBD_ASSET_PIN_OUT='"assets/pushpin/pushpin-out-14.png"' \
   -DBD_ASSET_PIN_IN='"assets/pushpin/pushpin-in-14.png"' \
   backend-gles/widget_test.c backend-gles/x11_window.c \
   backend-gles/bd_backend_gles.c \
   src/widget.c src/bd_draw.c src/bd_widget_vt.c src/bd_widget_value.c \
   src/bd_widget_explorer.c src/bd_widget_editor.c <libvt.a> \
   -lX11 -lEGL -lGLESv2 -lm -o gallery
```

Run `./gallery` from the bundle root so those relative asset paths resolve.

## Dependencies

- A **GLES-capable backend**. ludica (the reference, `bd_backend_ludica.c`)
  provides the window and GLES context; SDL, raylib, GLFW, or ANGLE can too.
- **libvt** is required by the terminal widget (`bd_widget_vt.c`).
- Vendored: **stb_truetype** (text rasterization) and eight chrome TTFs the
  toolkit bakes — a proportional family (DejaVu Sans) and a fixed-width family
  (DejaVu Sans Mono), each in regular / bold / oblique / bold-oblique.
  `bd_draw_text_styled(s, x, y, rgba, BD_FONT_BOLD|BD_FONT_ITALIC|BD_FONT_MONO)`
  selects a face; a missing variant TTF falls back to regular. The editor uses
  the mono family by default (`bd_editor_set_monospace`).

The assets the toolkit loads (`assets/` — the chrome TTFs, the terminal's CP437
atlas, the pushpin sprites) must be reachable at the paths in `widget.c` /
`bd_draw.c` / `bd_widget_vt.c`, or overridden with `-DBD_ASSET_*` (the eight font faces are
`BD_ASSET_GUI_FONT`[`_BOLD`/`_ITALIC`/`_BOLDITALIC`] and the same with a
`_MONO` prefix on the suffix, e.g. `BD_ASSET_GUI_FONT_MONO_BOLD`).

## License

Made by a machine. PUBLIC DOMAIN (CC0-1.0)
