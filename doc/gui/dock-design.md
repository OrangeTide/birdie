# birdie-gui Dock: Design Notes

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

A NeXTSTEP / WindowMaker-style dock for birdie-gui: a cluster of fixed-size
tiles, one per **minimized** top-level window, that the user clicks to restore.

This document is adopted from a prior, language-neutral design (a Leptos/Rust
WebRPG implementation, `webrpg/doc/dock-design.md`) and rewritten for
birdie-gui's C toolkit and its in-surface window manager. The proven ideas
(derive tiles from window state, integer snap-grid, click-vs-drag threshold,
ghost preview, push-windows-clear) carry over; the concrete data model, API,
and staging are birdie-gui's. It is a **living document**: as each increment
lands, the "Status" markers below move from *planned* to *done*, and any design
that changed in implementation is corrected here.

## 0. Decisions (this port)

| decision            | choice                                                        |
|---------------------|---------------------------------------------------------------|
| Tile source         | **Minimized windows only** — one tile per minimized top-level `BD_FRAME`; click restores. |
| Where it lives      | **Extension widget** `BD_DOCK` (like explorer/inventory), driven by new public WM hooks -- not baked into the core. |
| Arrangement (v1)    | **Auto-packed strip** — tiles pack along the dock's axis; no hand-arrangement yet. Full 2D snap-grid is a roadmap item (§10). |
| Placement           | **Relocatable via window gravity** — the dock hugs an edge/corner using `enum bd_gravity`; its axis follows the edge. |
| Persistence         | Host-owned; only arrangement is ever persisted (roadmap, §8). |
| Backend             | **Single-surface only** (`multi_window == 0`). On native multi-window backends the OS manages windows; see §14. |

> **Backend applicability.** The dock, minimize, and the whole in-surface
> window manager exist only when the backend cannot open native windows
> (`bd_backend.multi_window == 0`: ludica, SDL, the headless stub). There, the
> toolkit composites floating `BD_FRAME`s itself and can hide/dock them. On a
> native multi-window backend (the GLES gallery) each frame is a real OS window
> the host's window manager owns; there are no in-surface frames to minimize,
> so `BD_DOCK` stays empty and is not exhibited in the gallery. Bridging the two
> is the open design in §14.

## 1. What it is

A dock is a strip (v1) or 2D cluster (roadmap) of fixed-size tiles, each
representing a minimized window. Clicking a tile restores and raises its window.
The dock hugs a screen edge or corner chosen by the user (window gravity) and
its tiles pack along that edge. The dock never overlaps live windows: windows
under its footprint are pushed clear.

This mimics NeXTSTEP/WindowMaker: fixed-size beveled icon tiles and a persistent
edge anchor, as opposed to the macOS Dock's centered magnifying strip.

## 2. Design goals

- **Minimized windows go somewhere visible and manipulable**, not into a hidden
  list.
- **The dock is derived state.** The tile set is a projection of "which windows
  are minimized," never an independent list that can drift out of sync. This is
  the single most important architectural rule (see §5).
- **The dock never overlaps live windows.** When its footprint changes, open
  windows under it are pushed out.
- **The user controls placement** (which edge/corner) and, in the roadmap, the
  2D arrangement of tiles.
- **The layout survives reloads** (roadmap): grid positions persist, membership
  is rebuilt from window state.

## 3. Prerequisite: minimize in the window manager

birdie-gui's in-surface WM (single-surface backends; see `gui.md`) had no
minimize concept: a floating `BD_FRAME` could be dragged, snapped, docked,
locked, or closed. The dock needs a fourth title-bar action and a flag.

**Status: DONE.**

WM additions (`widget.c`, public API in `widget.h`):

- `int` field `minimized` on a frame. A minimized frame is skipped by the WM in
  layout, render, and hit-testing, so it vanishes from the surface.
- A **minimize** glyph on the floating title bar (a short bar), left of the
  lock and close buttons. Clicking it minimizes the frame.
- Public hooks the dock widget uses:
  ```c
  void bd_window_minimize(bd_id frame);
  void bd_window_restore(bd_id frame);     /* unminimize + raise to front */
  int  bd_window_minimized(bd_id frame);
  int  bd_window_list(bd_id *out, int max);   /* top-level frames, z-order; -> count */
  ```
  `bd_window_list` gives the dock the enumeration it needs to reconcile without
  the WM knowing anything about docks. Covered by tests in `test/test_gui.c`
  (minimize button hides + flags, hidden window isn't hit-tested, restore
  re-shows and re-raises).

## 4. The BD_DOCK widget

**Status: DONE (v1).** A `widget_ext` widget (`bd_widget_dock.{c,h}`, like
`bd_widget_inventory`), created by the host and placed in the tree:

```c
bd_id bd_dock_create(bd_id parent, const bd_dock_model *model, ...); /* model NULL = defaults */
void  bd_dock_set_gravity(bd_id dock, int gravity);  /* which edge/corner (enum bd_gravity) */
void  bd_dock_set_tile_size(bd_id dock, int px);     /* default 64 */
```

The `model` is optional and mirrors `bd_inventory_model` (§4 item struct): it
supplies each tile's icon/label/badge, defaulting to a generic glyph + the
frame title when NULL. The signature intentionally parallels
`bd_inventory_create` so the two widgets feel like siblings.

Attributes: `BD_GRAVITY_I` (edge/corner it hugs — reusing the enum, though this
is the widget's own placement, distinct from a frame's), tile size, and the
usual size/anchor attributes for its slot in the parent.

The dock's **axis** is derived from its gravity: a LEFT/RIGHT dock is a vertical
strip, TOP/BOTTOM is horizontal, a corner grows along the longer free edge (v1:
pick one; configurable later). Tiles auto-pack along the axis (leveraging the
new `BD_PACK`/anchor machinery conceptually; internally the dock lays its own
tiles since they are fixed-size).

The dock's *membership* is not app-supplied (unlike inventory): it is the WM's
minimized-window set, read through `bd_window_list` + `bd_window_minimized`. But
its *tiles are rendered exactly like inventory slots* (see §9), so a tile's
visual content uses the same item shape:

```c
typedef struct bd_dock_item {   /* mirrors bd_inventory_item */
    uint64_t    key;      /* the frame's bd_id, echoed back on click */
    const char *label;    /* caption (defaults to the frame's BD_LABEL_S) */
    bd_texture  icon;     /* tile image; id 0 = generic glyph. Update its
                             pixels each frame to animate (thumbnail / genie). */
    int         count;    /* optional badge (e.g. unread lines) */
    int         enabled;
} bd_dock_item;
```

An optional host **model** may override a tile's content per frame:

```c
typedef struct bd_dock_model {
    void (*get)(void *ctx, bd_id frame, bd_dock_item *out);  /* NULL = defaults */
    void  *ctx;
} bd_dock_model;
```

Default (no model, v1): the tile shows a generic glyph icon and the frame's
`BD_LABEL_S`. Supplying a model lets the host feed a **live thumbnail** of the
window or an animation into `icon` (a `bd_texture` it updates each frame), which
is the whole point of matching the inventory's texture-based tile: the dock
stays backend-neutral and just blits whatever the host draws into the texture.

## 5. Tiles are derived, not owned

The dock does not keep its own "what is docked" list. Authoritative state is the
WM's `minimized` flag per frame. The dock's tiles are recomputed as: *for every
top-level frame where `minimized` is true, show a tile.*

Reconciliation runs each layout/render pass (cheap; tile count is tiny):

1. `bd_window_list` → for each minimized frame not already represented, add a
   tile (assigned the next slot, v1; `next_available_pos`, roadmap).
2. Drop any tile whose frame is no longer minimized or no longer alive.
3. If the footprint changed, push overlapping live windows clear (§7).

Storing only arrangement (not membership) is what prevents the classic taskbar
desync bug where the button list and the real window list disagree.

## 6. Interaction

**v1 (planned): click to restore.**

- Click a tile → `bd_window_restore(frame)` (unminimize + raise).
- Click/drag on the dock background (roadmap) → relocate the dock to another
  edge (updates its gravity).

**Roadmap: drag-relocate a tile (2D grid).** One mousedown serves click and
drag, split by a 5px movement threshold:

- **down** records a drag (`active = false`) + pointer offset in the tile.
- **move** past `DRAG_THRESHOLD` (5px) sets `active = true`.
- **up**: `active` → snap to grid + commit; else → restore the window.
- **leave** cancels (tile returns to origin).

### Visual feedback during a tile drag (roadmap)

1. The original tile is hidden (not drawn twice).
2. A floating copy follows the pointer at `mouse - offset`, semi-transparent
   with a drop shadow.
3. A dashed **ghost** renders at the snap target, computed by running the snap
   search against a copy of the layout with the dragged tile removed.

## 7. Keeping windows clear of the dock

**Status: planned (v1, simple).** After the dock's footprint changes, push any
non-minimized frame whose corner is under the dock the minimum distance out:

```
for each non-minimized top-level frame w overlapping the dock rect:
    push it just past the dock along the shorter axis (right/down/left/up
    depending on which edge the dock hugs).
```

The reference tested only the window's top-left corner against a top-left dock;
with a gravity-relocatable dock the push direction depends on the anchored edge.
v1 may use a full-rectangle intersection since the edge varies.

## 8. Persistence

**Status: roadmap.** Only arrangement is ever persisted (frame identity → grid
position); membership is rebuilt from the WM's minimized state on load. v1
auto-packs, so there is nothing to persist yet. When 2D arrangement lands,
persist a flat list of `(stable_id, col, row)` through birdie's profile/config
layer. Frames without a stable id are excluded.

## 9. Visual design — shared with the inventory widget

The dock renders tiles **the same way `bd_widget_inventory` does**, so the two
look identical and share one code path. `bd_widget_inventory.c` already draws a
slot as: a recessed square (`bg` fill + `border` outline), a `bd_draw_sprite`
of the icon texture, a centered truncated label beneath, and an optional "xN"
count badge (with selection/cursor/drop rings on top).

**Status: DONE.** The tile drawing is factored into `bd_draw_tile()` in
`bd_draw` (recessed square + icon sprite + "xN" badge + centered truncated
caption); `bd_widget_inventory` and `bd_widget_dock` both call it. Benefits:

- **Consistent API** — the dock item mirrors `bd_inventory_item` (§4).
- **Consistent appearance** — one bevel/label/badge implementation.
- **Animation for free** — because the icon is a `bd_texture` the host updates,
  a tile can show a live, spinning, or shrinking image. The dock uses this for
  the minimize→dock and restore genie effects (§6 / roadmap) and for live
  window thumbnails.

Tile specifics:

- Fixed **64×64** tiles by default (configurable via `bd_dock_set_tile_size`,
  as inventory has `bd_inventory_set_cell_size`).
- Beveled/recessed square for the raised NeXTSTEP look; inverts while pressed.
- Icon glyph (or host thumbnail) + short truncated label (the frame title).
- Hit-test tile rects directly; no web `pointer-events` trick needed.

## 10. Roadmap (post-v1)

Ported from the reference once the strip ships:

- **Full 2D snap-grid.** Integer `DockPos { col, row }`; a flat
  `(frame, col, row)` list; pure functions `is_occupied`, `has_adjacent_tile`
  (connectivity — grow a connected blob, no floating islands),
  `next_available_pos`, `snap_to_grid` (expanding-radius search, cap 3),
  `bounds_px`. Store the grid in integers, convert to pixels only at render.
- **Drag-relocate + ghost preview** (§6).
- **Persistence** (§8).
- **Push-out full-rect** and multi-edge awareness.
- **Reorder/insert-and-shift** (reference deferred this).

## 11. Lessons carried from the reference

- **Derive the tile set from window state; never maintain it in parallel.** The
  single most valuable rule — the dock is a pure view of `minimized == true`.
- **Store the grid in integers, convert to pixels only at the edge.** Occupancy,
  adjacency, and equality stay exact.
- **A movement threshold cleanly separates click from drag.** No long-press, no
  modifier, no separate handle.
- **The ghost/snap-preview is worth the extra render.** Snapping without a
  preview feels random.
- **A connectivity rule keeps the layout coherent** with almost no code.
- **Small-N means simple data structures win.** A flat list beats a hash map on
  every axis that matters here.

## 12. Open questions (to resolve while building)

- **Tile rendering reuse mechanism** — factor a shared `bd_draw_tile` primitive
  (cleanest, small refactor of inventory) vs the dock copying the tile-draw
  code. Leaning shared primitive.
- **Icon/animation source** — v1 ships a generic glyph with the `bd_texture`
  hook in place; what does the host animate first: a live window thumbnail, or a
  minimize→dock "genie" shrink? (The texture path supports either.)
- Corner-gravity axis: which direction does a corner-anchored dock grow first?
- Should a minimized frame keep its z-order for restore, or always restore to
  front? (v1: restore to front.)

## 13. Implementation checklist

1. **DONE** — WM: `minimized` flag; skip minimized frames in layout/render/hit;
   minimize title-bar glyph; `bd_window_minimize/restore/minimized/list`. (§3)
2. **DONE** — shared `bd_draw_tile` primitive; inventory repointed at it. (§9)
3. **DONE** — `BD_DOCK` extension widget: reconcile from `bd_window_list`,
   gravity-anchored auto-packed strip of shared tiles, click-to-restore;
   `bd_dock_create/set_gravity/set_tile_size/count`. (§4–6)
4. **DONE** — headless-stub tests (minimize hides + tiles, hidden window not
   hit-tested, restore re-shows, tile count is derived, click restores, dead
   tile dropped) + SDL3 example demo (minimize → dock tile → restore).
5. **Deferred (roadmap)** — push windows clear (§7); 2D snap-grid +
   drag-relocate + ghost; persistence; per-frame icon/animation. (§10)

## 14. Extending docks/panels to native windows (open discussion)

The v1 dock only works single-surface because it *is* the in-surface window
manager's projection. This section frames how docks and panels could work on a
native multi-window backend (X11/Win32/Wayland/macOS), where the host's window
manager owns placement, decoration, and minimize. It records the design space;
nothing here is built.

### The core tension

Single-surface: birdie-gui *is* the window manager. It positions, decorates
(title bars), minimizes (hides the frame's render), docks (gravity), and draws
the dock over its own surface. Total control.

Native: the OS WM owns all of that. birdie-gui cannot draw title bars (the WM
does), cannot freely reposition (the WM may fight or ignore requests), and
"minimize" means OS-iconify. The OS also already has a taskbar/dock for
minimized windows, so an in-app dock risks duplicating it.

The scoping question is therefore **whose windows the dock manages**. For a MUD
client the answer is *its own* sub-windows (session panes, palettes,
inspectors), never other apps'. That narrows the options.

### What "minimize" needs from a native backend

Minimize on a native window is not close. It needs the backend to hide and
re-show a window without destroying its GL resources. New optional vtable hooks:

```c
void (*window_hide)(int id);   /* unmap / iconify, keep the window + context */
void (*window_show)(int id);   /* re-map / de-iconify + raise */
```

On X11 (the GLES backend) these are `XUnmapWindow` / `XMapWindow` (app-managed
hide), or `XIconifyWindow` + `_NET_WM_STATE` for true OS-iconify. `bd_window_
minimize/restore` would, when `multi_window`, route through these instead of
just flipping the in-surface render skip.

### Three options

- **A. OS-delegated (CHOSEN).** Minimize maps to OS iconify; there is *no*
  birdie dock on native backends — the OS taskbar shows minimized windows. Least
  code (just the two hooks + iconify). Loses a consistent in-app look and the
  tile thumbnails. This is the decided direction: on a native backend the dock
  is deliberately absent and window management is left to the host's WM; `BD_DOCK`
  remains a single-surface widget.

- **B. In-app dock via hide/show (not chosen).** Add `window_hide/show`.
  `bd_window_minimize` unmaps the native sub-window and the `minimized` flag goes
  true exactly as today; the **dock widget is unchanged** — it is still the
  projection of the minimized set. The dock lives as a normal child widget in
  one host window (e.g. the main window) and shows tiles for the hidden
  sub-windows. This unifies the dock across backends: the *only* backend-specific
  part is how a minimized frame is hidden (skip-render vs unmap). It also revives
  the gallery demo: the gallery's "New Window" sub-windows could minimize into a
  dock in the main window.

- **C. Panel/strut.** An edge-docked *panel* that reserves screen space (a
  taskbar-like strip) is a different mechanism: on X11 it is a borderless,
  always-on-top window that sets `_NET_WM_STRUT`. This is the "birdie-gui is a
  panel program" case, orthogonal to the minimized-window dock. Only worth it if
  we want a persistent always-visible strip, not just a minimized-window tray.

### Decision — Option A (implemented)

On native multi-window backends, window management is the host WM's job:
minimize maps to OS iconify, and the OS taskbar is the tray. birdie-gui does
**not** ship an in-app dock on native backends; `BD_DOCK` stays a single-surface
widget. This keeps the toolkit out of reimplementing a window manager where one
already exists, at the cost of a consistent in-app dock look across backends.

**Status: DONE.** The `bd_backend` vtable gained optional `window_minimize` /
`window_restore` hooks. On a `multi_window` backend, `bd_window_minimize` /
`bd_window_restore` call them with the frame's native window id;
`bd_window_minimized` and the in-surface flag are advisory there (the OS owns
the real iconified state). The GLES/X11 backend implements them with
`XIconifyWindow` and `XMapRaised` (`win_window_minimize/restore` in
`x11_window.c`). A recording multi-window stub in `test/test_gui.c` verifies the
delegation. Win32/Wayland/macOS backends map the same hooks to their platform
iconify when they land; a backend that leaves the hooks NULL simply keeps the
window mapped (no-op minimize).

Option B (a unified in-app dock via `window_hide/show`) and Option C (a
strut-reserving panel) are recorded above as the alternatives we considered and
declined; revisit only if a future host genuinely needs an in-app tray on a
native backend.

## 15. The managed-canvas minimize target (dock vs WM desktop icons)

A `BD_MANAGED_CANVAS` is its own in-surface WM host (embedded MDI), so it needs
somewhere for *its* minimized frames to go. Two receivers can each project the
canvas's minimized set: the WM's own free-floating **desktop icons**
(double-click to restore, drag to reposition) and a `BD_DOCK` scoped to the
canvas. Because both derive from the same `minimized` flag (§5), enabling both
draws two icons per minimized window. That is a bug, not a feature.

So a canvas has exactly **one minimize target**:

- **Default: the WM owns it and draws desktop icons.** Icons are on by default,
  so a minimized frame is always reachable in an MDI. `bd_managed_canvas_set_
  icon_minimize(canvas, 0)` suppresses the WM icon layer entirely (a minimized
  frame then only vanishes, unless a dock is attached).
- **A dock claims it and becomes the exclusive receiver.** While a live dock
  owns the target the WM draws no desktop icons, so a minimize yields one dock
  tile. A `BD_DOCK` created inside a canvas **auto-claims** the target if it is
  still free (dropping a dock into an MDI just works); the WM reclaims it if the
  owning dock dies.

```c
void  bd_managed_canvas_set_minimize_dock(bd_id canvas, bd_id dock); /* claim / detach (BD_NONE) */
bd_id bd_managed_canvas_minimize_dock(bd_id canvas);                 /* current owner, or BD_NONE */
```

Mutual exclusion lives in one predicate (`mcanvas_shows_icons` in `widget.c`)
that gates both the icon render and the icon hit-test: the WM shows icons only
when icon-minimize is on *and* no live dock has claimed the target. This keeps
the dock unchanged — it is still the pure projection of the minimized set (§5);
only the *WM's own* icon layer steps aside.

Note this is orthogonal to the native-backend decision (§14): the target
concerns which in-surface receiver a **canvas** uses on a single-surface host,
whereas §14 is about surface-level frames on a native multi-window backend.

**Status: DONE.** Covered by tests in `test/test_gui.c` (auto-claim, a fresh
canvas leaves the target with the WM, a minimized frame lands in the attached
dock, detach returns the target to the WM). The gallery's Desktop tab attaches
its top-left dock as the canvas's minimize target.
