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
| `bd_color.h`            | parse color names / `#hex` into RGBA8 (widget color lists)       |
| `bd_widget_value.h`     | slider, knob, toggle, wheel, jog, X-Y pad                        |
| `bd_widget_vt.h`        | `BD_TERMINAL` (libvt-backed terminal)                            |
| `bd_widget_explorer.h`  | icon/grid browser (Explorer / Finder style)                     |
| `bd_widget_editor.h`    | rich-text, row-oriented editor                                  |
| `bd_syntax.h`           | runtime-configurable syntax highlighter (state machine)         |
| `bd_widget_table.h`     | sortable multi-column table                                     |
| `bd_widget_tree.h`      | indented expand/collapse hierarchy list                         |
| `bd_widget_icon.h`      | shared icon cell + standalone icon (app launcher / desktop icon) |
| `bd_widget_inventory.h` | fixed grid of icon cells (game inventory)                       |
| `bd_widget_sketch.h`    | pressure-sensitive sketch pad                                    |
| `bd_widget_indicator.h` | panel-mount LED indicator lamp                                  |
| `bd_widget_meter.h`     | 0..1 meters (bar / VU / magic eye / pie / liquid vial)          |
| `bd_widget_progress.h`  | determinate / indeterminate progress bar                        |
| `bd_widget_chart.h`     | scrolling multi-series time-series strip chart                   |
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
| `BD_TIP_S`      | str  | any               | hover tooltip text (dwell-triggered bubble)|
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
| `BD_TEXT_FIELD`       | single-line text input                 | `BD_LABEL_S`, `BD_PASSWORD_B`   |
| `BD_TEXT_AREA`  | multi-line text input                  | `BD_LABEL_S`                     |
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
int  bd_tabbar_count(bd_id);  int bd_tabbar_selected(bd_id);  void bd_tabbar_select(bd_id, int);
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

### Context / popup menu

`bd_popmenu` (`bd_popmenu.h`) is a transient popup menu floated top-most through
the overlay primitive. It is a mechanism, not a policy: the toolkit never
attaches context menus to widgets. The app decides when and where to open one
(a right-click handler, an editor-mode gesture, a toolbar button). A passthrough
canvas or GLES game view that owns every click opens it entirely on its own
terms, e.g. only in an in-game editor mode.

```c
enum { BD_POPMENU_SEPARATOR = 1u<<0, BD_POPMENU_DISABLED = 1u<<1 };
typedef struct bd_popmenu_item {
    const char *label;                /* borrowed */
    void      (*action)(void *user);  /* run when chosen; NULL for none */
    void       *user;
    unsigned    flags;                /* BD_POPMENU_* */
} bd_popmenu_item;

/* open at (x,y), clamped on screen; the item array is copied, strings/user borrowed */
void bd_popmenu_open(int x, int y, const bd_popmenu_item *items, int count);
void bd_popmenu_close(void);
int  bd_popmenu_is_open(void);
```

A click or Enter runs the chosen item's action and closes; a click outside or
Escape dismisses; a click on a separator or disabled row keeps it open. The
`BD_TABLE`, inventory, and explorer widgets report a right-click through their
`context` callback (with screen coords) for the app to open one from.

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
void  bd_window_minimize(bd_id frame);   void bd_window_restore(bd_id frame);   int bd_window_minimized(bd_id frame);
/* embedded WM: a BD_MANAGED_CANVAS hosts floating frames inside a widget rect */
bd_id bd_managed_canvas_create(bd_id parent, ...);   bd_id bd_managed_canvas_of(bd_id descendant);
void  bd_managed_canvas_set_icon_minimize(bd_id canvas, int on); /* minimized frame -> desktop icon */
void  bd_managed_canvas_set_backdrop(bd_id canvas, bd_texture wallpaper, bd_shader effect); /* textured wallpaper backdrop; effect=={0} reverts to solid */
```

Behavior: drag the title bar to move; release near an edge/corner to snap and
dock (gravity set); drag away to float. A docked window re-snaps on resize.
Locking pins it (no drag) while keeping its gravity. Set the floating size
with `BD_PREF_W_I`/`BD_PREF_H_I` and the floating position with `BD_X_I`/`BD_Y_I`.
A minimized frame vanishes to a `BD_DOCK` tile; with
`bd_managed_canvas_set_icon_minimize` a canvas instead (or also) shows it as a
free-floating desktop icon (double-click restores, drag repositions).
`bd_managed_canvas_set_backdrop` gives the canvas a textured wallpaper drawn by
a custom effect shader (paired with `BD_SHADER_QUAD_VERT`, sampling the texture
on unit 0, animated via `u_time`) in place of the solid `BD_BG_C`; pass an
`effect` of `{0}` to revert. The gallery's Desktop tab toggles it from a dialog.

## Extension widgets

Built on `widget_ext.h`; each has its own header and a `*_create()`.

### Value widgets (`bd_widget_value.h`)

```c
enum { BD_HORIZONTAL, BD_VERTICAL };
typedef void (*bd_value_cb)(bd_id, void *arg, float t);   /* t in [0,1] unless noted */

bd_id bd_slider_create(bd_id parent, const bd_slider_desc *desc, ...);  /* orient/value + cb */
void  bd_slider_set(bd_id, float);   float bd_slider_get(bd_id);

bd_id bd_knob_create(bd_id parent, const bd_knob_desc *desc, ...);  /* min/max/step/dial + cb */
void  bd_knob_set(bd_id, float);     float bd_knob_get(bd_id);      /* value in [min,max] */

typedef void (*bd_toggle_cb)(bd_id, void *arg, int on);
bd_id bd_toggle_create(bd_id parent, const bd_toggle_desc *desc, ...);  /* on + cb */
void  bd_toggle_set(bd_id, int on);  int bd_toggle_get(bd_id);

bd_id bd_wheel_create(bd_id parent, const bd_wheel_desc *desc, ...);   /* orient + cb; endless jog */

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
set_pos callbacks); keys are `uint64_t`. Three view modes: icons (free-arranged),
list, and details (rows under a sticky column header, columns from `model.cell`).

```c
bd_id bd_explorer_create(bd_id parent, const bd_explorer_model *model, const bd_explorer_cb *cb, ...);
void  bd_explorer_refresh(bd_id);
int   bd_explorer_selection(bd_id, uint64_t *keys, int max);   void bd_explorer_select(bd_id, uint64_t key, int add);
void  bd_explorer_set_icon_size(bd_id, int px);               void bd_explorer_begin_rename(bd_id, uint64_t key);
void  bd_explorer_set_view(bd_id, int view);   /* BD_EXPLORER_ICONS|LIST|DETAILS */   int bd_explorer_view(bd_id);
void  bd_explorer_set_columns(bd_id, const bd_explorer_column *cols, int ncol); /* details columns */
```

### Editor (`bd_widget_editor.h`)

Row-oriented rich-text with per-span styles, an optional submit hook, and an
autocomplete popup (install a completer; a floating list of text+detail
suggestions appears under the caret as you type, Up/Down + Enter to accept).

Style runs are layered (syntax / app / find / mark) and composed attribute by
attribute, so a find highlight tints the background without erasing the colour
under it. `bd_editor_find` highlights every match, cycles the current one, and
scrolls it into view; `bd_editor_replace` / `_replace_all` edit through it. See
`doc/gui/editor-highlight.md` for the styling model and the syntax-highlighter
design.

```c
bd_id bd_editor_create(bd_id parent, ...);
void  bd_editor_set_text(bd_id, const char *);    int bd_editor_text(bd_id, char *out, int cap);
int   bd_editor_row_count(bd_id);                 int bd_editor_row_text(bd_id, int row, char *out, int cap);
void  bd_editor_insert_row(bd_id, int row, const char *);   void bd_editor_replace_row(bd_id, int row, const char *);
void  bd_editor_delete_row(bd_id, int row);
void  bd_editor_on_submit(bd_id, bd_callback_fn, void *data);
void  bd_editor_set_enter_submits(bd_id, int);    int bd_editor_enter_submits(bd_id);
/* autocomplete: a popup appears as you type; completer returns text+detail items */
void  bd_editor_set_completer(bd_id, bd_completer_fn, void *user);  /* fn NULL = off */
void  bd_editor_set_complete_min(bd_id, int chars);   /* auto-trigger prefix len (default 2) */
void  bd_editor_set_locked(bd_id, int);           int bd_editor_locked(bd_id);
void  bd_editor_set_monospace(bd_id, int);
void  bd_editor_clear_styles(bd_id);
void  bd_editor_style_span(bd_id, int start, int end, bd_rich_style);
void  bd_editor_highlight_row(bd_id, int row, bd_rich_style);
void  bd_editor_highlight_span(bd_id, int row, int col0, int col1, bd_rich_style);
/* find / replace: highlight matches, cycle current, edit through it */
enum { BD_FIND_ICASE = 1 << 0, BD_FIND_WORD = 1 << 1 };
int   bd_editor_find(bd_id, const char *needle, unsigned flags);  /* -> count */
int   bd_editor_find_next(bd_id);  int bd_editor_find_prev(bd_id);
int   bd_editor_find_count(bd_id); int bd_editor_find_current(bd_id);
void  bd_editor_find_clear(bd_id);
int   bd_editor_replace(bd_id, const char *repl);      /* current match */
int   bd_editor_replace_all(bd_id, const char *repl);  /* -> replaced */
/* syntax highlighting: install a bd_syntax language; NULL = off */
void  bd_editor_set_syntax(bd_id, const bd_syntax_lang *);
const bd_syntax_lang *bd_editor_syntax(bd_id);
```

### Syntax highlighting (`bd_syntax.h`)

A runtime-configurable highlighter: a language is a state machine (named states,
each with ordered character-class or literal rules and an optional keyword map)
that the tokenizer walks byte by byte, carrying state across lines so multi-line
comments and strings work. It emits `(start, end, flags, fg, bg)` spans and owns
no widget. Parse a definition from an in-memory text buffer, take a compiled-in
default (`lua`, `abc`), or autodetect by extension; install one on an editor with
`bd_editor_set_syntax`. The text format is documented in
`doc/gui/editor-highlight.md`.

```c
bd_syntax_lang *bd_syntax_parse(const char *text, int len);   /* NULL on error */
void            bd_syntax_free(bd_syntax_lang *);
const bd_syntax_lang *bd_syntax_builtin(const char *name);    /* "lua" | "abc" */
const bd_syntax_lang *bd_syntax_for_name(const char *filename);   /* by ext */
void bd_syntax_register(const char *name, const char *const *exts,
                        const bd_syntax_lang *);
int  bd_syntax_run(const bd_syntax_lang *, const char *buf, int len,
                   bd_syntax_span *out, int max);             /* -> span count */
```

### Table (`bd_widget_table.h`)

Model-driven, sortable, multi-select. Optional model hooks give rich cells: a
per-cell `icon()` glyph (status/priority/attachment columns) and a per-row
`row_style()` for bold text, a foreground colour, or a background tint.

```c
enum { BD_TABLE_LEFT, BD_TABLE_RIGHT, BD_TABLE_CENTER };     /* column alignment */
bd_id bd_table_create(bd_id parent, const bd_table_column *cols, int ncols, const bd_table_model *, ...);
void  bd_table_refresh(bd_id);
int   bd_table_selection(bd_id, int *rows, int max);   void bd_table_select(bd_id, int row, int add);
int   bd_table_current(bd_id);                         /* cursor row */
```

### Tree (`bd_widget_tree.h`)

An indented expand/collapse outline over a model that yields children on
demand (a file/project tree, a class hierarchy). Node identity is an app-chosen
`uint64_t` key (0 = the invisible root: `child_count(ctx, 0)` returns the top
level). The widget owns scrolling, selection, keyboard nav (up/down, left/right
collapse-expand, type-ahead), and the expand state (seed it with
`bd_tree_set_expanded`); an optional `expand` callback lets the app lazily fill
children the first time a node opens. Each node may carry an optional `detail`
string drawn right-aligned and dimmed (an unread count, a size), separate from
the label.

```c
/* model: child_count(ctx, parent) + child(ctx, parent, i) -> key + get(ctx, node, *item) */
/* item: label, detail, icon, has_children, enabled, user  cb: select / activate / expand */
bd_id    bd_tree_create(bd_id parent, const bd_tree_model *, const bd_tree_cb *, ...);
void     bd_tree_refresh(bd_id);
uint64_t bd_tree_selected(bd_id);        void bd_tree_select(bd_id, uint64_t node);
void     bd_tree_set_expanded(bd_id, uint64_t node, int open);   int bd_tree_is_expanded(bd_id, uint64_t);
void     bd_tree_set_indent(bd_id, int px);
```

### Split (`bd_widget_split.h`)

A nestable binary split: two panes (a `BD_PANEL` each) separated by a draggable
sash, laid out side by side (`BD_SPLIT_HORIZONTAL`) or stacked
(`BD_SPLIT_VERTICAL`). The panes divide the space by the split ratio (the first
pane's fraction of the extent minus the sash), enforced through the core flex
engine's grow weights, so a resize preserves the ratio. Dragging the sash
adjusts it, clamped to a per-pane minimum. The sash is a private extension
widget, not part of the split's own class, so a press on pane content still
reaches that content (only a press on the sash starts a drag). Splits nest: a
pane may hold another split.

```c
enum { BD_SPLIT_HORIZONTAL = 1, BD_SPLIT_VERTICAL };
bd_id bd_split_create(bd_id parent, int orient, ...);
bd_id bd_split_pane(bd_id split, int index);   /* 0 = first, 1 = second */
float bd_split_ratio(bd_id split);             void bd_split_set_ratio(bd_id split, float);
void  bd_split_set_min_size(bd_id split, int px);   void bd_split_set_sash_size(bd_id split, int px);
void  bd_split_on_change(bd_id split, bd_callback_fn, void *data);  /* fired on a drag */
```


### Group box (`bd_widget_groupbox.h`)

A captioned etched-border fieldset (OPEN LOOK / Motif) for grouping related form
fields. The group box IS the content container: add fields straight into it and
they stack in a column below the caption band. Group boxes nest. The title is
borrowed. No core capability; a plain container with a title-band spacer and a
groove-drawing render hook.

```c
typedef struct bd_groupbox_desc { const char *title; } bd_groupbox_desc;
bd_id       bd_groupbox_create(bd_id parent, const bd_groupbox_desc *, ...);
void        bd_groupbox_set_title(bd_id, const char *title);
const char *bd_groupbox_title(bd_id);
```

### Scroll-view (`bd_widget_scrollview.h`)

A fixed-size viewport that clips a taller content column and scrolls it, for a
form with more fields than fit its panel. Unlike the list / tree / editor (which
scroll their own drawn content), it scrolls a subtree of real child widgets. Add
fields into the content column returned by `bd_scrollview_content`; a vertical
scrollbar appears and the wheel scrolls when the column overflows. The wheel
bubbles, so it works even when the pointer is over an inner field.

It works by offsetting the single content child (`BD_Y_I = -scroll_y`) and
measuring the content height from the children's `PREF_H`; the stacked children
must carry a `PREF_H` (the form controls and `bd_dialog_field` rows do). It uses
the `BD_WC_CLIP_CHILDREN` class flag (widget_ext.h), a general capability the
render walk honours by scissoring a container's children to its rect.

```c
typedef struct bd_scrollview_desc { int always_bar; } bd_scrollview_desc;
bd_id bd_scrollview_create(bd_id parent, const bd_scrollview_desc *, ...);
bd_id bd_scrollview_content(bd_id);          /* the column to fill */
void  bd_scrollview_scroll_to(bd_id, int y_px);
int   bd_scrollview_scroll(bd_id);           int bd_scrollview_content_height(bd_id);
```
### Icon (`bd_widget_icon.h`)

The shared icon "cell" the dock, action bar, and inventory render and drag
through (`bd_icon_desc` = key/label/icon/count/enabled; `bd_icon_draw` +
`bd_icon_dnd_begin`), plus a standalone single-icon widget used as an app
launcher or a desktop icon (double-click / Enter activates; drag it onto any
icon-accepting target).

```c
typedef struct bd_icon_desc { uint64_t key; const char *label; bd_texture icon; int count, enabled; } bd_icon_desc;
void  bd_icon_draw(float rx, float ry, int cell_w, int pad, int icon_size, const bd_icon_desc *, uint32_t bg, uint32_t border, uint32_t fg);
void  bd_icon_dnd_begin(bd_id source, const bd_icon_desc *, void *user);
/* standalone widget */
bd_id bd_icon_create(bd_id parent, const bd_icon_desc *desc, ...);
void  bd_icon_set(bd_id, const bd_icon_desc *);   void bd_icon_set_texture(bd_id, bd_texture);
uint64_t bd_icon_key(bd_id);
void  bd_icon_on_activate(bd_id, bd_icon_activate_fn, void *user);   /* dbl-click / Enter */
void  bd_icon_on_drop(bd_id, bd_icon_drop_fn, void *user);           /* makes it a drop target */
```

### Inventory (`bd_widget_inventory.h`)

Icon, list, or details view; drag-and-drop and context menu via callbacks.

```c
bd_id bd_inventory_create(bd_id parent, int cols, int rows, const bd_inventory_model *, const bd_inventory_cb *, ...);
void  bd_inventory_set_dims(bd_id, int cols, int rows);   void bd_inventory_set_cell_size(bd_id, int px);
void  bd_inventory_refresh(bd_id);
int   bd_inventory_cols(bd_id);   int bd_inventory_rows(bd_id);
int   bd_inventory_selected(bd_id);   int bd_inventory_selection(bd_id, int *slots, int max);   void bd_inventory_select(bd_id, int slot, int add);
```

### Sketch pad (`bd_widget_sketch.h`)

Pressure-sensitive freehand drawing.

```c
bd_id bd_sketch_create(bd_id parent, ...);
void  bd_sketch_set_ink(bd_id, uint32_t rgba);   void bd_sketch_set_alt_ink(bd_id, uint32_t rgba);
void  bd_sketch_set_nib(bd_id, float px);        void bd_sketch_clear(bd_id);
int   bd_sketch_stroke_count(bd_id);
```

### Meters (`bd_widget_meter.h`)

A 0..1 quantity as a physical instrument, output-only. `style` picks the look;
`zones` is a low->high color list (names or `#hex`) split by `stops` (or evenly)
into bands that color the moving element; `peak` holds a marker at the recent
max; `ballistic` eases the value (`BD_METER_EXACT` / `_VU_BALLISTIC` / `_PEAK_HOLD`).

```c
enum { BD_METER_BAR, BD_METER_VU, BD_METER_EYE, BD_METER_PIE, BD_METER_VIAL };
enum { BD_METER_EXACT, BD_METER_VU_BALLISTIC, BD_METER_PEAK_HOLD };
bd_id bd_meter_create(bd_id parent, const bd_meter_desc *desc, ...);
void  bd_meter_set(bd_id, float value);          float bd_meter_get(bd_id);
void  bd_meter_set_zones(bd_id, const char *zones, const char *stops);
void  bd_meter_reset_peak(bd_id);
/* desc: style, value, zones, stops, peak, ballistic, orient, segments, size, label, color */
```

### Progress bar (`bd_widget_progress.h`)

The simple sibling of `BD_METER_BAR`: a determinate fill, an optional `NN%`
readout, or an indeterminate marquee. Set `glass` for a liquid-in-glass tube
matching `BD_METER_VIAL` (the bar-form life/mana bar). (A round "ball" progress
is `BD_METER_VIAL` itself.)

```c
bd_id bd_progress_create(bd_id parent, const bd_progress_desc *desc, ...);
void  bd_progress_set(bd_id, float value);       float bd_progress_get(bd_id);
void  bd_progress_set_indeterminate(bd_id, int on);
/* desc: value, indeterminate, show_percent, glass, orient, color, label */
```

### Chart (`bd_widget_chart.h`)

A scrolling time-series strip chart (xload / system-monitor style): overlaid
colored line traces on a graph-paper grid, newest sample at the right. The
widget owns a ring buffer per series; the app pushes samples. Each series
autoscales over its window; a `"%"` unit pins it to 0..100 and shares the grid,
and up to two other units get a labeled value axis (left, then right).

```c
/* series: label, unit ("%" = 0..100 scale), color (0 = palette) */
bd_id bd_chart_create(bd_id parent, int window, ...);   /* window = samples kept */
int   bd_chart_add_series(bd_id, const bd_chart_series *);   /* -> index or -1 */
void  bd_chart_push(bd_id, int series, float value);
void  bd_chart_push_row(bd_id, const float *values, int n); /* one per series */
void  bd_chart_clear(bd_id);
void  bd_chart_set_grid(bd_id, int);   void bd_chart_set_legend(bd_id, int);
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
