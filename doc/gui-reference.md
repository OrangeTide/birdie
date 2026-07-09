# birdie-gui reference

A catalog of the widgets and public API. For the *why* (design rationale,
retained-mode reasoning, roadmap), see [gui.md](gui.md); this file is the
*what*. Signatures are copied from the headers; when in doubt the header wins.

## Header map

| header                  | provides                                                        |
|-------------------------|-----------------------------------------------------------------|
| `widget.h`              | core: tree, attributes, lifecycle, layout, the v1.0 widget set, overlays, the in-surface window manager |
| `widget_ext.h`          | `bd_widget_class` — register your own widget type               |
| `bd_backend.h`          | `bd_backend` vtable + neutral `bd_event` a host implements       |
| `bd_theme.h`            | `bd_theme` colors/metrics; `bd_theme_default()`                  |
| `bd_draw.h`             | the immediate renderer widgets are drawn with (rects, text, sprites) |
| `bd_asset.h`            | register fonts/images as in-binary blobs instead of files        |
| `bd_widget_value.h`     | slider, knob, toggle, wheel, jog, X-Y pad                        |
| `bd_widget_vt.h`        | `BD_TERMINAL` (libvt-backed terminal)                            |
| `bd_widget_explorer.h`  | icon/grid browser (Explorer / Finder style)                     |
| `bd_widget_editor.h`    | rich-text, row-oriented editor                                  |
| `bd_widget_table.h`     | sortable multi-column table                                     |
| `bd_widget_inventory.h` | fixed grid of icon cells (game inventory)                       |
| `bd_widget_canvas.h`    | pressure-sensitive drawing canvas                               |
| `bd_backend_ludica.h` / `bd_backend_sdl3.h` | reference backends                            |

## Core lifecycle

Driven by the host's main loop.

```c
void  bd_gui_init(const bd_backend *backend, const bd_theme *theme); /* theme NULL = default */
void  bd_gui_init_fonts(const bd_backend *backend, const bd_theme *theme,
                        const bd_font_set *fonts);   /* bake a custom family */
void  bd_gui_cleanup(void);
void  bd_gui_layout(int win_w, int win_h);           /* place the tree */
void  bd_gui_render(void);                           /* draw a frame */
int   bd_gui_event(const bd_event *ev);              /* feed one neutral event; 1 = consumed */
bd_id bd_focused(void);                              /* the Tab-focused widget */
```

## Widget tree & attributes

```c
bd_id       bd_create(bd_id parent, int type, ...);       /* attrs as BD_*, value pairs, BD_END */
bd_id       bd_create_v(bd_id parent, int type, const bd_attr *attrs);  /* non-varargs builder */
void        bd_destroy(bd_id id);
void        bd_set(bd_id id, ...);                        /* BD_*, value, ..., BD_END */
void        bd_set_v(bd_id id, const bd_attr *attrs);
int         bd_get_i(bd_id id, int attr);
const char *bd_get_s(bd_id id, int attr);
bd_id       bd_parent(bd_id id);
bd_id       bd_first_child(bd_id id);
bd_id       bd_next_sibling(bd_id id);
```

`BD_NONE` (0) is the null id; a top-level widget has parent `BD_NONE`.

### Attributes

Each id encodes its value type in the low nibble (`_I` int, `_S` string,
`_P` pointer, `_F` callback, `_C` color, `_B` bool), so the varargs reader
knows the width to pull. Terminate a list with `BD_END`.

| attribute       | type | applies to        | meaning                                  |
|-----------------|------|-------------------|-------------------------------------------|
| `BD_WIDTH_I` / `BD_HEIGHT_I` | int | any  | alias of `BD_PREF_W_I` / `BD_PREF_H_I`     |
| `BD_PREF_W_I` / `BD_PREF_H_I` | int | any | preferred size (main-axis hint in flex)    |
| `BD_GROW_I`     | int  | flex child        | weight for distributing slack space        |
| `BD_X_I` / `BD_Y_I` | int | FIXED child, floating frame | position offset             |
| `BD_LAYOUT_I`   | int  | container         | `BD_LAYOUT_ROW` / `_COL` / `_FIXED`        |
| `BD_ROLE_I`     | int  | any               | accessibility role hint                    |
| `BD_PAD_I`      | int  | container         | inner padding (px)                         |
| `BD_GAP_I`      | int  | flex container    | gap between children (px)                   |
| `BD_ANCHOR_I`   | int  | child             | `enum bd_anchor` — cell alignment / edge-anchoring |
| `BD_PACK_I`     | int  | flex container    | `enum bd_pack` — main-axis distribution     |
| `BD_GRAVITY_I`  | int  | floating frame    | `enum bd_gravity` edge/corner dock         |
| `BD_LABEL_S`    | str  | most              | text / items (`\n`-separated for list/tabs)|
| `BD_NAME_S`     | str  | any               | accessibility name                         |
| `BD_ON_CLICK_F` / `BD_ON_CLOSE_F` | cb | button, menu, frame | `void(bd_id, void*)` handler   |
| `BD_ON_CLICK_P` / `BD_ON_CLOSE_P` | ptr | ″ | user data passed to the handler             |
| `BD_FG_C` / `BD_BG_C` | color | any         | foreground / background RGBA (`0xRRGGBBAA`)|
| `BD_VISIBLE_B` / `BD_ENABLED_B` | bool | any     | shown / interactive                        |
| `BD_MENU_PIN_B` | bool | menu              | open pinned (olvwm pushpin)                |
| `BD_PASSWORD_B` | bool | text field        | mask input                                 |
| `BD_LOCKED_B`   | bool | floating frame    | pin in place, keep gravity                 |

### Layout

Containers arrange children by their `BD_LAYOUT_I`:

- `BD_LAYOUT_COL` (default) / `BD_LAYOUT_ROW` — flexbox: children take their
  preferred main-axis size; `BD_GROW_I` shares leftover space; the cross axis
  fills. `BD_GAP_I` spaces them, `BD_PAD_I` insets the container.
- `BD_LAYOUT_FIXED` — children are placed at their `BD_X_I`/`BD_Y_I` offset
  with their preferred size (or the content box when a dimension is 0).

The layout engine never measures contents; a content panel needs an explicit
`BD_PREF_W_I`/`BD_PREF_H_I` or `BD_GROW_I` or it collapses.

### Anchor & packing

`BD_ANCHOR_I` (`enum bd_anchor`, on a child) controls how the child sits in
the cell the layout hands it; `BD_ANCHOR_FILL` (default) stretches to fill it,
matching the pre-anchor behavior.

```c
enum bd_anchor { BD_ANCHOR_FILL, BD_ANCHOR_CENTER,
    BD_ANCHOR_N, BD_ANCHOR_S, BD_ANCHOR_E, BD_ANCHOR_W,
    BD_ANCHOR_NE, BD_ANCHOR_NW, BD_ANCHOR_SE, BD_ANCHOR_SW };
```

- **In `FIXED`** the anchor pins the child (at its preferred size, filling any
  axis whose preferred size is 0) to that edge/corner of the parent's content
  box and re-tracks it on resize; `BD_X_I`/`BD_Y_I` become inward margins from
  the anchored edge(s). `BD_ANCHOR_CENTER` centers; `BD_ANCHOR_FILL` keeps the
  legacy top-left `X/Y` placement.
- **In `ROW`/`COL`** only the cross-axis component matters: a non-`FILL` child
  takes its preferred cross size and aligns start/center/end within the cross
  extent instead of stretching (e.g. `BD_ANCHOR_W` left-aligns a fixed-width
  child in a column; `BD_ANCHOR_E` right-aligns it).

`BD_PACK_I` (`enum bd_pack`, on a `ROW`/`COL` container) distributes leftover
main-axis space when no child grows to consume it. Ignored when any child has
`BD_GROW_I` (grow already eats the slack).

```c
enum bd_pack { BD_PACK_START, BD_PACK_CENTER, BD_PACK_END,
    BD_PACK_SPACE_BETWEEN, BD_PACK_SPACE_AROUND };
```

`BD_PACK_START` (default) leaves the slack at the end; `CENTER`/`END` shift the
whole group; `SPACE_BETWEEN`/`SPACE_AROUND` insert equal gaps between (and, for
around, outside) the children.

## Widget catalog

Core widgets (`widget.h`) are created with `bd_create(parent, TYPE, ...)`.

| type            | role                                   | notable API                     |
|-----------------|----------------------------------------|----------------------------------|
| `BD_FRAME`      | top-level window / desktop             | window-manager API below         |
| `BD_PANEL`      | layout container (optionally a bg)     | —                                |
| `BD_LABEL`      | read-only text                         | `BD_LABEL_S`                     |
| `BD_BUTTON`     | clickable action; also a menu item     | `BD_ON_CLICK_F/_P`              |
| `BD_TEXT`       | single-line text input                 | `BD_LABEL_S`, `BD_PASSWORD_B`   |
| `BD_MULTILINE`  | multi-line text input                  | `BD_LABEL_S`                     |
| `BD_LIST`       | scrolling single-select list           | `bd_list_*`                     |
| `BD_SCROLLBAR`  | standalone scrollbar                   | `bd_scrollbar_*`                |
| `BD_MENU`       | menu-bar entry / popup (pinnable)      | children are `BD_BUTTON` items; `BD_MENU_PIN_B` |
| `BD_TAB_BAR`    | folder tabs                            | `bd_tabbar_*`                   |
| `BD_INPUT_LINE` | command line (history/completion)      | `BD_ON_CLICK_F` fires on Enter  |
| `BD_NOTICE`     | modal alert (via `bd_notice_open`)     | `bd_notice_*`                   |

A menu is a `BD_MENU` whose children are `BD_BUTTON`s; a menu bar is a
`BD_PANEL`/`BD_LAYOUT_ROW` of `BD_MENU`s.

```c
/* list */
void bd_list_set_items(bd_id, const char *newline_separated);
int  bd_list_count(bd_id);  int bd_list_selected(bd_id);  void bd_list_select(bd_id, int row);
/* tab bar */
void bd_tabbar_set_tabs(bd_id, const char *newline_separated);
int  bd_tabbar_count(bd_id);  int bd_tabbar_active(bd_id);  void bd_tabbar_set_active(bd_id, int);
/* scrollbar */
void  bd_scrollbar_set(bd_id, float pos, float frac);   float bd_scrollbar_value(bd_id);
```

### Overlays

```c
/* transient modal alert: message + `\n`-separated buttons; cb(id, button_index, arg) */
bd_id bd_notice_open(const char *message, const char *buttons, bd_notice_cb cb, void *arg);
void  bd_notice_close(bd_id notice);
/* generic modal dialog: build a detached top-level container, then: */
void  bd_modal_open(bd_id dialog);   void bd_modal_close(bd_id dialog);   bd_id bd_modal_active(void);
```

## Window manager (in-surface)

On a single-surface backend (`bd_backend.multi_window == 0`) the toolkit runs
its own window manager: the first top-level `BD_FRAME` is the full-surface
desktop; every later top-level frame is a floating window with a title bar
(label + lock + close), draggable, raiseable, and snap-dockable. On a native
multi-window backend each frame is a real OS window instead.

```c
enum bd_gravity {                 /* edge = full-length dock strip; corner = pinned at pref size */
    BD_GRAVITY_NONE, BD_GRAVITY_LEFT, BD_GRAVITY_RIGHT, BD_GRAVITY_TOP, BD_GRAVITY_BOTTOM,
    BD_GRAVITY_TOP_LEFT, BD_GRAVITY_TOP_RIGHT, BD_GRAVITY_BOTTOM_LEFT, BD_GRAVITY_BOTTOM_RIGHT,
};
void  bd_window_dock(bd_id frame, int gravity);   /* set edge/corner gravity */
void  bd_window_move(bd_id frame, int x, int y);  /* float at (x,y), clear gravity */
void  bd_window_set_locked(bd_id frame, int locked);   int bd_window_locked(bd_id frame);
int   bd_window_gravity(bd_id frame);
bd_id bd_frame_for_window(int window_id);         /* native backend: OS window id -> frame */
```

Behavior: drag the title bar to move; release near an edge/corner to snap and
dock (gravity set); drag away to float. A docked window re-snaps on resize.
Locking pins it (no drag) while keeping its gravity. Set the floating size
with `BD_PREF_W_I`/`BD_PREF_H_I` and the floating position with `BD_X_I`/`BD_Y_I`.

## Extension widgets

Built on `widget_ext.h`; each has its own header and a `*_create()`.

### Value widgets (`bd_widget_value.h`)

```c
enum { BD_HORIZONTAL, BD_VERTICAL };
typedef void (*bd_value_cb)(bd_id, void *arg, float t);   /* t in [0,1] unless noted */

bd_id bd_slider_create(bd_id parent, int orient, float value, bd_value_cb, void *arg, ...);
void  bd_slider_set(bd_id, float);   float bd_slider_get(bd_id);

bd_id bd_knob_create(bd_id parent, const bd_knob_desc *desc, ...);  /* min/max/step/dial + cb */
void  bd_knob_set(bd_id, float);     float bd_knob_get(bd_id);      /* value in [min,max] */

typedef void (*bd_toggle_cb)(bd_id, void *arg, int on);
bd_id bd_toggle_create(bd_id parent, int on, bd_toggle_cb, void *arg, ...);
void  bd_toggle_set(bd_id, int on);  int bd_toggle_get(bd_id);

bd_id bd_wheel_create(bd_id parent, int orient, bd_value_cb, void *arg, ...);   /* endless jog */

enum { BD_XY_SQUARE, BD_XY_CIRCLE };
bd_id bd_xypad_create(bd_id parent, const bd_xypad_desc *desc, ...);
void  bd_xypad_get(bd_id, float *x, float *y);   void bd_xypad_set(bd_id, float x, float y);
```

### Terminal (`bd_widget_vt.h`)

```c
void  bd_terminal_register(void);                 /* once, before creating one */
bd_id bd_terminal_create(bd_id parent, ...);
void  bd_terminal_write(bd_id, const char *data, int len);   /* len<0 = strlen; parses escapes */
void  bd_terminal_set_palette(bd_id, const bd_palette *pal); /* 16-color ANSI palette */
```

### Explorer (`bd_widget_explorer.h`)

Icon/grid browser driven by a caller-supplied `bd_explorer_model` (count/get/
set_pos callbacks); keys are `uint64_t`.

```c
bd_id bd_explorer_create(bd_id parent, const bd_explorer_model *model, const bd_explorer_cb *cb, ...);
void  bd_explorer_refresh(bd_id);
int   bd_explorer_selection(bd_id, uint64_t *keys, int max);   void bd_explorer_select(bd_id, uint64_t key, int add);
void  bd_explorer_set_icon_size(bd_id, int px);               void bd_explorer_begin_rename(bd_id, uint64_t key);
```

### Editor (`bd_widget_editor.h`)

Row-oriented rich-text with per-span styles.

```c
bd_id bd_editor_create(bd_id parent, ...);
void  bd_editor_set_text(bd_id, const char *);    int bd_editor_text(bd_id, char *out, int cap);
int   bd_editor_row_count(bd_id);                 int bd_editor_row_text(bd_id, int row, char *out, int cap);
void  bd_editor_insert_row(bd_id, int row, const char *);   void bd_editor_replace_row(bd_id, int row, const char *);
void  bd_editor_delete_row(bd_id, int row);
void  bd_editor_set_locked(bd_id, int);           int bd_editor_locked(bd_id);
void  bd_editor_set_monospace(bd_id, int);
void  bd_editor_clear_styles(bd_id);
void  bd_editor_style_span(bd_id, int start, int end, bd_rich_style);
void  bd_editor_highlight_row(bd_id, int row, bd_rich_style);
void  bd_editor_highlight_span(bd_id, int row, int col0, int col1, bd_rich_style);
```

### Table (`bd_widget_table.h`)

```c
enum { BD_TABLE_LEFT, BD_TABLE_RIGHT, BD_TABLE_CENTER };     /* column alignment */
bd_id bd_table_create(bd_id parent, const bd_table_column *cols, int ncols, const bd_table_model *, ...);
void  bd_table_refresh(bd_id);
int   bd_table_selection(bd_id, int *rows, int max);   void bd_table_select(bd_id, int row, int add);
int   bd_table_current(bd_id);                         /* cursor row */
```

### Inventory (`bd_widget_inventory.h`)

Fixed grid of icon cells; drag-and-drop and context menu via callbacks.

```c
bd_id bd_inventory_create(bd_id parent, int cols, int rows, const bd_inventory_model *, const bd_inventory_cb *, ...);
void  bd_inventory_set_dims(bd_id, int cols, int rows);   void bd_inventory_set_cell_size(bd_id, int px);
void  bd_inventory_refresh(bd_id);
int   bd_inventory_cols(bd_id);   int bd_inventory_rows(bd_id);
int   bd_inventory_selected(bd_id);   int bd_inventory_selection(bd_id, int *slots, int max);   void bd_inventory_select(bd_id, int slot, int add);
```

### Canvas (`bd_widget_canvas.h`)

Pressure-sensitive freehand drawing.

```c
bd_id bd_canvas_create(bd_id parent, ...);
void  bd_canvas_set_ink(bd_id, uint32_t rgba);   void bd_canvas_set_alt_ink(bd_id, uint32_t rgba);
void  bd_canvas_set_nib(bd_id, float px);        void bd_canvas_clear(bd_id);
int   bd_canvas_stroke_count(bd_id);
```

## Theme (`bd_theme.h`)

`bd_theme_default()` returns the built-in theme; pass a copy with tweaked
fields to `bd_gui_init`. Fields: `bg, panel, widget, hover, press, text,
text_hi, border, focus, select` (RGBA) plus font metrics.

## Assets (`bd_asset.h`)

Serve fonts/images from in-binary blobs (via `.incbin`) instead of disk, so a
build has no external `BD_ASSET_*` path dependency.

```c
void       bd_asset_register_data(const char *id, const void *data, size_t len);
void       bd_asset_register_file(const char *id, const char *path);
int        bd_asset_lookup(const char *id, bd_asset *out);   void bd_asset_clear(void);
```

## Renderer (`bd_draw.h`)

The immediate-mode drawing layer widgets render through; extension widgets can
call it directly between `bd_draw_begin`/`bd_draw_end`.

```c
void  bd_draw_rect(float x, float y, float w, float h, uint32_t rgba);
void  bd_draw_rect_lines(float x, float y, float w, float h, uint32_t rgba);
void  bd_draw_quad(float x0,float y0, float x1,float y1, float x2,float y2, float x3,float y3, uint32_t rgba);
void  bd_draw_sprite(bd_texture, float dx,float dy,float dw,float dh, float u0,float v0,float u1,float v1, uint32_t rgba);
void  bd_draw_text(const char *s, float x, float y, uint32_t rgba);   /* +_styled variants for bold/italic/mono */
float bd_draw_text_width(const char *s);   float bd_draw_line_height(void);   float bd_draw_ascent(void);
```

## Backend (`bd_backend.h`)

A host implements the `bd_backend` vtable (window/GPU: `width/height/viewport/
clear`, shaders, `draw_verts`, textures, `scissor`, optional multi-window,
clipboard, IME) and translates native input into the neutral `bd_event`, then
injects it via `bd_gui_init` and `bd_gui_event`. See `bd_backend_ludica.c`,
`bd_backend_sdl3.c`, and `src/guitest/bd_backend_gles.c` for references.
