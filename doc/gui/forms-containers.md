# Group box and form scroll-view containers

Plan for two form-support containers, the remaining pieces of the
`doc/gui/dialogs.md` form work: a **group box** (a labeled, etched fieldset
border) and a **scroll-view** (a container that scrolls its child widgets when a
form is taller than its panel). Both build on the extension API
(`widget_ext.h`) and match the existing form-control conventions
(`bd_widget_form.*`, `bd_dialog.*`).

Status: planning. Nothing here is built yet.

## Motivation

The dialog helper (`bd_dialog`) composes a titled modal with a content column
and a button row, and `bd_dialog_field` lays out label-plus-control rows. Two
gaps remain:

- **Grouping.** A settings dialog wants related fields boxed together
  ("Connection", "Appearance") with a captioned border, the classic
  Motif / OPEN LOOK etched group box. Today the only grouping is a bare panel.
- **Overflow.** A form with more fields than fit its panel has no way to
  scroll. The terminal, list, tree, and editor scroll their own drawn content,
  but nothing scrolls a subtree of real child widgets (labels, fields,
  checkboxes). A scroll-view fills that gap and is broadly useful beyond forms.

## Toolkit mechanics that shape the design

Findings from the widget core that the design has to respect:

- **Layout recursion is unconditional.** `layout_children` (widget.c:1606)
  recurses into every widget's children regardless of the
  `BD_WC_CONTAINS_CHILDREN` flag. A container can therefore rely on the core to
  lay out its subtree even if the container renders itself as a leaf.
- **The class `layout()` hook runs after the core layout pass** (widget.c:3324),
  so a hook that changes child geometry only takes effect on the *next* frame
  (a one-frame lag). This is the same lag `BD_SPLIT` accepts; it is invisible
  for scrolling and resizing.
- **Layout does not measure content.** Panels need an explicit `PREF_H` or grow;
  the engine never sums a subtree's natural size. A scroll-view must compute its
  content height itself.
- **`FIXED` layout honours a per-child offset.** In `BD_LAYOUT_FIXED` a child at
  the default `BD_ANCHOR_FILL` is placed at `x + user_x, y + user_y` (from
  `BD_X_I` / `BD_Y_I`) and sized to its `PREF_W`/`PREF_H` or the parent's area
  (widget.c:1627). A negative `BD_Y_I` shifts a child up, and the core then lays
  its grandchildren out at that shifted origin. This is the scroll offset.
- **The render walk applies no transform** and has **no post-children hook**:
  `cls->render()` runs, then children recurse (widget.c:2096-2113). A container
  cannot bracket a scissor around its children from an extension hook alone.
- **Events are already viewport-clipped.** `hit_extension` and `hit_interactive`
  reject a point outside a widget's own rect before recursing into its children,
  so a child scrolled outside the viewport is already unhittable. No event-side
  work is needed for clipping.
- **Scissor is a single rect, not a stack** (`bd_backend.scissor` /
  `scissor_off`). Nested clipping regions do not compose; the inner one wins and
  `scissor_off` clears everything.

## Group box

A captioned etched border around a content area. Skeuomorphic OPEN LOOK / Motif:
a two-tone groove (shadow line then highlight line) forms a rectangle whose top
edge is broken by the title text.

**No core change; no custom layout.** The group box is a `CONTAINS_CHILDREN`
container in `BD_LAYOUT_COL` whose first child is a fixed-height title-band
spacer; the caller's widgets append below it. The `render()` hook draws the
groove and the title over the spacer band. Side and bottom insets come from the
container's uniform `BD_PAD_I`; the top title band comes from the spacer, so no
asymmetric padding is needed.

```c
typedef struct bd_groupbox_desc {
    const char *title;   /* caption in the top border (borrowed; NULL = none) */
} bd_groupbox_desc;

/* A group box IS the content container: add fields straight into it. */
bd_id bd_groupbox_create(bd_id parent, const bd_groupbox_desc *desc, ...);
void  bd_groupbox_set_title(bd_id id, const char *title);   /* borrowed */
```

Rendering (via `bd_backend_get()` and `bd_draw_rect`):

- Groove rectangle inset so its top line runs through the title's vertical
  centre: a 1px shadow (`theme.border`) rectangle and a 1px highlight
  (`theme.text` dim) offset by one pixel, the etched look.
- A short gap in the top line where the title sits, title text in `theme.text`
  indented ~10px from the left.
- Optional faint fill (`theme.panel`) inside the groove.

The title-band spacer height is the chrome line height plus a couple of pixels;
the app never sees it (it appends after it). Nesting works because a group box
is just a container.

## Scroll-view

A viewport that clips its content and scrolls it vertically (the common form
case; horizontal is a later option). The content is a single inner column the
app fills; an optional vertical scrollbar appears when the content overflows.

**Structure** (`FIXED` layout, `CONTAINS_CHILDREN`):

- `content` child: a `BD_PANEL` in `BD_LAYOUT_COL` the app fills with fields.
- `bar` child: a `BD_SCROLLBAR` pinned to the right edge, hidden when the
  content fits.

**Scrolling by child offset.** Each layout the scroll-view:

1. Measures the content's natural height by summing its children's `PREF_H`
   (plus gaps and the content's pad). This is the one place the pref-size gotcha
   bites, so the scroll-view walks `bd_first_child`/`bd_next_sibling` and reads
   `bd_get_i(child, BD_PREF_H_I)`. Fields built with `bd_dialog_field` and the
   form controls already carry a `PREF_H`, so they measure correctly.
2. Clamps `scroll_y` to `[0, max(0, content_h - viewport_h)]`.
3. Sets the content child's `BD_PREF_H_I = content_h` and
   `BD_Y_I = -scroll_y`, so the core lays the content (and its grandchildren)
   out at the scrolled origin, taller than the viewport.
4. Sizes `content` to `viewport_w - bar_w` and pins `bar` to the right; shows
   `bar` only when `content_h > viewport_h`, and drives it with
   `bd_scrollbar_set(bar, scroll_y / max, viewport_h / content_h)`.

Steps 1-4 run in the `layout()` hook, so they take effect the next frame (the
accepted one-frame lag). The mouse-wheel and scrollbar-drag paths update
`scroll_y` in the `event()` hook and re-apply the content `BD_Y_I` immediately,
so dragging and wheeling are lag-free.

**Render clipping needs a small core addition.** Because the render walk has no
post-children hook, the scroll-view cannot scissor its children from an
extension. Add one capability flag, honoured by the render walk:

```c
/* widget_ext.h */
enum { ... BD_WC_CLIP_CHILDREN = 1ull << 2 };
```

```c
/* widget.c render_widget(), around line 2109 */
if (!leaf) {
    const bd_backend *be = bd_backend_get();
    int clip = be && be->scissor && (cls->flags & BD_WC_CLIP_CHILDREN);
    if (clip) { bd_draw_flush(); be->scissor(w->x, w->y, w->w, w->h); }
    for (bd_id c = w->first_child; c != BD_NONE; c = pool[c].next_sib)
        render_widget(c);
    if (clip) { bd_draw_flush(); be->scissor_off(); }
}
```

This is a general, reusable primitive (any clipped container benefits), about
five lines, and additive (the flag is a new bit, so existing widgets are
unaffected). Events need no change: the hit walk already rejects points outside
the scroll-view's rect. Caveat: scissor is a single rect, so a scroll-view whose
content itself scissors (a nested list) does not compose cleanly; forms do not
hit this, and it is documented as a limitation for 0.9.

**API:**

```c
typedef struct bd_scrollview_desc {
    int always_bar;   /* keep the scrollbar visible even when content fits */
} bd_scrollview_desc;

/* The scroll-view's content container: add widgets into it (a COL column). */
bd_id bd_scrollview_create(bd_id parent, const bd_scrollview_desc *desc, ...);
bd_id bd_scrollview_content(bd_id id);       /* the inner column to fill */
void  bd_scrollview_scroll_to(bd_id id, int y_px);
int   bd_scrollview_scroll(bd_id id);        /* current offset, px */
```

`bd_scrollview_create` returns the scroll-view; `bd_scrollview_content` returns
the inner column (as `bd_dialog_content` does), so callers add fields into the
column and the scroll-view manages the viewport and bar.

## Phases

- **P0 - Group box.** `bd_widget_groupbox.{c,h}`: the COL container + title-band
  spacer + etched-border render. No core change. Gallery: box two field groups
  in the Controls or a new "Forms" tab. Headless test: create, add children,
  assert the title-band reservation and that children land below it.

- **P1 - Core clip flag.** Add `BD_WC_CLIP_CHILDREN` and the render-walk scissor
  bracket (widget.c). Headless test through an existing `CONTAINS_CHILDREN`
  widget given the flag: assert a child drawn outside the parent rect is
  clipped (via the recording stub backend's scissor calls).

- **P2 - Scroll-view.** `bd_widget_scrollview.{c,h}`: content column + optional
  scrollbar, content-height measurement, `scroll_y` clamp, `BD_Y_I` offset,
  wheel + scrollbar-drag. Uses the P1 flag. Gallery: a tall field list in a
  short viewport, scrolled by wheel and bar. Headless test: measure content
  height, wheel to scroll, assert offset clamps at both ends and the scrollbar
  fraction tracks.

- **P3 - Dialog integration.** An optional `bd_dialog` convenience so a form that
  overflows drops its content column into a scroll-view, and a `bd_dialog_group`
  helper that wraps `bd_groupbox` for the label-row builder. Rework one real
  dialog (the app Settings dialog is the natural candidate) onto grouped,
  scrollable fields.

## Testing and rollout

Each phase adds headless `test_gui` checks (creation, geometry, scroll clamp,
title-band reservation) and a live gallery demo verified on the GLES backend.
The clip flag is exercised both by the scroll-view and by a direct
flagged-container test. All additive; the only public-surface change is the new
`BD_WC_CLIP_CHILDREN` bit, which fits the 0.9 window. Docs updated on landing:
`gui.md`, `gui-reference.md`, this file's status, `CLAUDE.md`, and the dialogs
plan's remaining-items list.

Made by a machine. PUBLIC DOMAIN (CC0-1.0)
