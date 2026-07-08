#ifndef BD_WIDGET_ACTIONBAR_H
#define BD_WIDGET_ACTIONBAR_H

#include "widget.h"
#include "widget_ext.h"   /* bd_dnd_payload */
#include <stdint.h>

/*
 * Action bar widget, built on the extension API (widget_ext.h). A strip of
 * fixed-size action tiles, like a game hotbar or a floating tool palette. It is
 * the mutable sibling of BD_DOCK: it renders the same beveled tiles
 * (bd_draw_tile) and hugs a screen edge/corner using enum bd_gravity, but where
 * the dock is a read-only projection of the minimized-window set, the action
 * bar OWNS its slots and the host (or a drag-and-drop) fills them.
 *
 * Placement. Set a gravity edge/corner with bd_actionbar_set_gravity and the bar
 * anchors itself against that edge, orienting into a vertical strip (LEFT/RIGHT
 * and the corners) or a horizontal one (TOP/BOTTOM); put it in a
 * BD_LAYOUT_FIXED parent (typically the desktop root) so it can anchor. With
 * BD_GRAVITY_NONE the bar floats at its BD_X_I/BD_Y_I position and the user can
 * drag it around by the grip at its leading edge; its orientation then follows
 * bd_actionbar_set_orientation (default horizontal).
 *
 * Drag-and-drop. The bar is a drop target for the toolkit's cross-widget drag
 * (widget_ext.h): drag an item out of a bd_widget_inventory or a bd_widget_dock
 * tile and release it over a slot to bind it there. By default the dropped
 * payload's icon/label/key are copied into the slot; a drop callback may veto or
 * customize that. Slots can also be rearranged by dragging one onto another
 * within the bar (fires the move callback). A plain click on a filled slot fires
 * the activate callback.
 *
 * Hotkeys. Each slot can carry a keyboard binding (an ASCII key plus Shift/Ctrl/
 * Alt modifiers), like a CRPG hotbar (World of Warcraft, Neverwinter Nights).
 * The binding belongs to the slot, not the action, so it survives a slot being
 * refilled by a drop. Its label (e.g. "^A", "⇧⌥5") is drawn in the slot's
 * corner when the slot is empty or hovered, using ⇧ Shift, ^ Control, ⌥ Alt.
 * Feed key presses to bd_actionbar_key() to fire the matching slot's action.
 *
 * SINGLE-SURFACE friendly. Like the dock, edge gravity is most useful on the
 * in-surface window manager, but the action bar itself is a plain leaf widget
 * and works on any backend; only the whole-bar drag-move assumes a
 * BD_LAYOUT_FIXED parent.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* One action slot's content. bd_actionbar_set_slot copies the label, so a stack
 * temporary is fine; icon/key/user are stored by value. */
typedef struct bd_action {
	uint64_t    key;      /* app identity, echoed back in callbacks */
	const char *label;    /* caption under the icon (copied; NULL = none) */
	bd_texture  icon;     /* tile image; id 0 = empty square */
	int         enabled;  /* 0 = dimmed and not activatable */
	void       *user;     /* app payload (unused by the widget) */
} bd_action;

/* Per-bar events. Any hook may be NULL. */
typedef struct bd_actionbar_cb {
	/* click on a filled, enabled slot */
	void (*activate)(bd_id w, int slot, uint64_t key, void *ctx);
	/* right-click a slot; screen coords for a context menu */
	void (*context) (bd_id w, int slot, uint64_t key, int sx, int sy, void *ctx);
	/* a cross-widget drag was released over `slot`. Return nonzero to accept
	 * (the bar then copies the payload into the slot); return 0 to reject and
	 * leave the slot unchanged. NULL hook = accept-and-bind by default. */
	int  (*drop)    (bd_id w, int slot, const bd_dnd_payload *p, void *ctx);
	/* a slot was dragged onto another slot within the bar (reorder) */
	void (*move)    (bd_id w, int from_slot, int to_slot, void *ctx);
	void  *ctx;
} bd_actionbar_cb;

/* Register the action-bar class (idempotent). Called by bd_actionbar_create. */
void bd_actionbar_register(void);

/*
 * Create an action bar with `slots` empty slots (clamped to [1, 32]). `cb` is
 * optional (NULL for none) and copied by value. Trailing args are BD_*
 * attributes ending in BD_END. The bar sizes itself to its slots; set its
 * placement with bd_actionbar_set_gravity (default BD_GRAVITY_NONE = floating).
 */
bd_id bd_actionbar_create(bd_id parent, int slots, const bd_actionbar_cb *cb, ...);

/* Which edge/corner the bar hugs (enum bd_gravity). BD_GRAVITY_NONE floats. */
void bd_actionbar_set_gravity(bd_id bar, int gravity);

/* Orientation of a floating (BD_GRAVITY_NONE) bar: nonzero = vertical, 0 =
 * horizontal (default). Ignored when a gravity edge already fixes the axis. */
void bd_actionbar_set_orientation(bd_id bar, int vertical);

/* Tile (icon) edge length in pixels. Default 48. */
void bd_actionbar_set_tile_size(bd_id bar, int px);

/* Change the slot count (clamped to [1, 32]); extra slots are cleared. */
void bd_actionbar_set_slots(bd_id bar, int slots);
int  bd_actionbar_slots(bd_id bar);

/* Set (a != NULL) or clear (a == NULL) a slot's action. Out-of-range slots are
 * ignored. The label is copied. */
void bd_actionbar_set_slot(bd_id bar, int slot, const bd_action *a);

/* Read a slot into *out. Returns 1 if the slot is filled, 0 if empty/invalid
 * (out is left untouched when 0). */
int  bd_actionbar_get_slot(bd_id bar, int slot, bd_action *out);

/*
 * Bind (or clear) a slot's hotkey. `key` is an ASCII code or a BD_KEY_* value
 * (0 clears the binding); `mods` is a BD_MOD_* bitmask (Shift/Ctrl/Alt). The
 * binding is a property of the slot and is kept when the slot is refilled by a
 * drop or bd_actionbar_set_slot. The label is shown when the slot is empty or
 * hovered.
 */
void bd_actionbar_set_hotkey(bd_id bar, int slot, int key, int mods);

/* Read a slot's hotkey into *key / *mods (either may be NULL). Returns 1 if the
 * slot has a binding, else 0. */
int  bd_actionbar_get_hotkey(bd_id bar, int slot, int *key, int *mods);

/*
 * Dispatch a key press to the bar: if a slot's hotkey matches (`key` and exactly
 * `mods`) and that slot is filled and enabled, its activate callback fires.
 * Returns 1 if a bound slot matched (the press was consumed), else 0. Call this
 * from the host's key handling to drive the bar like a CRPG hotbar; the bar does
 * not grab global keys itself.
 */
int  bd_actionbar_key(bd_id bar, int key, int mods);

#endif
