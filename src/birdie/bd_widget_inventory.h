#ifndef BD_WIDGET_INVENTORY_H
#define BD_WIDGET_INVENTORY_H

#include "widget.h"
#include <stdint.h>

/*
 * Inventory grid widget, built on the extension API (widget_ext.h). A
 * scrollable, fixed cols x rows grid of slots for a roleplaying-game bag:
 * each slot shows a square icon with a caption beneath, like an Explorer icon
 * view locked to a grid. The grid capacity (cols x rows) is set at create and
 * can be changed with bd_inventory_set_dims(); it scrolls vertically when the
 * viewport is shorter than the grid.
 *
 * Cell content is a bd_texture, so a slot can hold a static image or an
 * animated 3D model: to animate, the host renders the model into that texture
 * and updates its pixels each frame (backend update_texture, or its own
 * render-to-texture). The widget stays backend-neutral and just blits it.
 *
 * The widget is not the source of truth: a model supplies each slot's content
 * on demand (queried only for visible slots), and drag-and-drop fires a move()
 * callback so the app performs the actual swap. Selection is keyed on slot
 * index, which is stable for a fixed grid.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* One slot's content, filled by the model's get() for that call only. The
 * widget copies nothing; borrowed strings need only survive the get() call. */
typedef struct bd_inventory_item {
	uint64_t    key;      /* app-side identity, echoed back in callbacks */
	const char *label;    /* caption under the icon (NULL = none) */
	bd_texture  icon;     /* cell image; id 0 = empty slot. Update its pixels
	                         each frame to animate a 3D model. */
	int         count;    /* stack size; > 1 draws a small "xN" badge */
	int         enabled;  /* 0 = dimmed and not activatable */
	const char *tooltip;  /* hover text (NULL = fall back to label) */
	void       *user;     /* app payload (unused by the widget) */
} bd_inventory_item;

/* The data source. get() is required and must be cheap: it is called per
 * visible slot each frame. Leave *out zeroed (icon.id == 0) for an empty slot. */
typedef struct bd_inventory_model {
	void (*get)(void *ctx, int slot, bd_inventory_item *out);
	void  *ctx;
} bd_inventory_model;

/* Per-slot events delivered to the app. Any hook may be NULL. */
typedef struct bd_inventory_cb {
	void (*activate)(bd_id w, int slot, uint64_t key, void *ctx);   /* dbl-click / Enter */
	void (*context) (bd_id w, int slot, uint64_t key,
	                 int sx, int sy, void *ctx);                    /* right-click; screen coords */
	void (*move)    (bd_id w, int from_slot, int to_slot, void *ctx); /* drag-drop between slots */
	void (*selection_changed)(bd_id w, void *ctx);
	void (*hover)   (bd_id w, int slot, uint64_t key, void *ctx);   /* hovered slot changed; slot < 0 = none */
	void  *ctx;
} bd_inventory_cb;

/*
 * Create an inventory grid of cols x rows slots. `model` is required and copied
 * by value (its ctx and function pointer are retained, so a stack temporary is
 * fine). `cb` is optional (NULL for none) and likewise copied. Trailing args
 * are BD_* attributes ending in BD_END (e.g. BD_GROW_I, BD_PREF_H_I). The
 * preferred width is derived from cols and the cell size; set the height (or
 * BD_GROW_I) yourself.
 */
bd_id bd_inventory_create(bd_id parent, int cols, int rows,
                          const bd_inventory_model *model,
                          const bd_inventory_cb *cb, ...);

/* Change the grid capacity (cheap; re-sizes the selection and re-clamps
 * scroll). Values < 1 are clamped to 1. */
void bd_inventory_set_dims(bd_id id, int cols, int rows);

/* Set the icon edge length in pixels (cell size scales with it). Default 48. */
void bd_inventory_set_cell_size(bd_id id, int px);

/* Re-query the model after the underlying data changed (re-clamps scroll and
 * clears hover). Content is queried live, so this is only needed to drop a
 * hover/tooltip or after a dims change. */
void bd_inventory_refresh(bd_id id);

int  bd_inventory_cols(bd_id id);
int  bd_inventory_rows(bd_id id);

/* First selected slot, or -1 if none. */
int  bd_inventory_selected(bd_id id);

/* Copy up to `max` selected slot indices into `slots`; returns the selection
 * count (which may exceed `max`). Pass slots=NULL/max=0 to just count. */
int  bd_inventory_selection(bd_id id, int *slots, int max);

/* Select `slot`. add=0 replaces the selection with just this slot; add=1 adds
 * to it. Out-of-range slots are ignored. */
void bd_inventory_select(bd_id id, int slot, int add);

#endif
