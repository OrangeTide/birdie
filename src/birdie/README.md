# birdie-gui

A small retained-mode GUI toolkit in C. You build a tree of widgets with a
tagged varargs API, and the toolkit lays them out, draws them, and routes
input. Rendering and windowing go through a backend interface, so the same
widget code runs on ludica, SDL, raylib, or GLFW once a backend exists for it.

It has two API tiers:

- **App API** (`widget.h`) for building UIs out of widgets.
- **Extension API** (`widget_ext.h`) for defining new widget *types*. The VT
  terminal (`bd_widget_vt.h`) is built entirely on it and is the reference
  example.

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
`BD_FRAME`, `BD_PANEL`, `BD_LABEL`, `BD_BUTTON`, `BD_MENU`, and `BD_INPUT_LINE`.

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

## Defining a widget type

An extension fills a `bd_widget_class` and registers it for a type id, then
wraps the core create call in a typed constructor. Hooks are optional; the
core owns per-instance `state` of the size you declare.

```c
#include "widget_ext.h"

struct gauge { float value; };

static void
gauge_render(bd_id id, void *state)
{
    struct gauge *g = state;
    const bd_backend *be = bd_backend_get();
    int x, y, w, h;
    bd_widget_rect(id, &x, &y, &w, &h);
    be->fill_rect(x, y, w * g->value, h, 0.3f, 0.7f, 1.0f, 1.0f);
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

For a complete example with init/destroy/layout/event hooks and a libvt-backed
implementation, read `bd_widget_vt.c`. As of 0.1 extension widgets draw quads
and textures through the backend; proportional vector text is core-only.

## Porting to another backend

Implement the `bd_backend` vtable in `bd_backend.h` (window/frame, quad batch,
vector font, resource load/destroy), and translate your windowing library's
events into the neutral `bd_event`. Pass your backend to `bd_gui_init`. The
widget code does not change. `bd_backend_ludica.c` is the reference
implementation, around 200 lines.

## Dependencies

- **ludica** is required only by the reference backend
  (`bd_backend_ludica.c`) and ships the proportional GUI font that
  `BD_ASSET_GUI_FONT` points at. Targeting another backend removes this
  dependency.
- **libvt** is required by the terminal widget (`bd_widget_vt.c`).

The assets the default theme loads (`assets/`) must be reachable at the paths
in `widget.c` / `bd_widget_vt.c`, or overridden with `-DBD_ASSET_*`.

## License

Made by a machine. PUBLIC DOMAIN (CC0-1.0)
