#ifndef BD_WIDGET_EXPLORER_H
#define BD_WIDGET_EXPLORER_H

#include "widget.h"
#include <stdint.h>

/*
 * Explorer / icon-browser widget, built on the extension API (widget_ext.h).
 * An arrangeable grid of labeled icons modeled on Explorer / PROGMAN.EXE /
 * Finder.
 *
 * It is not tied to files: a model interface supplies the items, so they can
 * be any collection (a MUD server list, a DAW plugin list, ...). The widget
 * caches nothing authoritative, it re-queries the model on layout; the app
 * calls bd_explorer_refresh() when the data changes. Items carry a stable key
 * so selection and saved positions survive a refresh when indices shift.
 *
 * Status: selection (single / Ctrl-toggle / Shift-range), drag-move,
 * rubber-band, keyboard navigation, and in-place rename work; list/details
 * view modes are still to come (see bd_widget_explorer.c).
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* One item, filled by the model's get() for the duration of that call only.
 * The widget copies nothing but `key` and (when moved) the position. */
typedef struct bd_explorer_item {
	uint64_t    key;      /* stable identity; selection/state keyed on this */
	const char *label;    /* caption under the icon */
	bd_texture  icon;     /* 0 = default placeholder icon */
	int         enabled;  /* 0 = dimmed and not activatable */
	int         x, y;     /* saved position; <0 = auto-place on the grid */
	void       *user;     /* app payload */
} bd_explorer_item;

/* The data source. count()/get() are required; set_pos() and set_name() are
 * optional. set_pos persists a dragged icon's position; set_name receives the
 * new label after an in-place rename (NULL = rename disabled). */
typedef struct bd_explorer_model {
	int  (*count)(void *ctx);
	void (*get)(void *ctx, int index, bd_explorer_item *out);
	void (*set_pos)(void *ctx, uint64_t key, int x, int y);
	void (*set_name)(void *ctx, uint64_t key, const char *name);
	void *ctx;
} bd_explorer_model;

/* Per-item events delivered to the app. Any hook may be NULL. */
typedef struct bd_explorer_cb {
	void (*activate)(bd_id w, uint64_t key, void *user);            /* dbl-click / Enter */
	void (*context)(bd_id w, uint64_t key, int sx, int sy, void *); /* right-click; screen coords */
	void (*selection_changed)(bd_id w, void *ctx);
	void (*moved)(bd_id w, uint64_t key, int x, int y, void *);     /* after a drag */
	void *ctx;
} bd_explorer_cb;

/*
 * Create an explorer. `model` is required and is copied by value (its ctx and
 * function pointers are retained, so the model struct itself may be a stack
 * temporary). `cb` is optional (NULL for none) and likewise copied. Trailing
 * args are BD_* attributes ending in BD_END (e.g. BD_GROW_I, BD_PREF_W_I).
 */
bd_id bd_explorer_create(bd_id parent, const bd_explorer_model *model,
                         const bd_explorer_cb *cb, ...);

/* Re-query the model after the underlying data changed. Selection keys that no
 * longer exist are dropped. */
void bd_explorer_refresh(bd_id id);

/* Copy up to `max` selected keys into `keys`; returns the selection count
 * (which may exceed `max`). Pass keys=NULL/max=0 to just query the count. */
int bd_explorer_selection(bd_id id, uint64_t *keys, int max);

/* Select `key`. add=0 replaces the selection with just this key; add=1 adds to
 * the current selection. */
void bd_explorer_select(bd_id id, uint64_t key, int add);

/* Set the icon edge length in pixels (cell size scales with it). */
void bd_explorer_set_icon_size(bd_id id, int px);

/* Begin an in-place rename of the item with `key`: an editable field opens
 * over its label, pre-filled with the current name. Enter commits (via
 * model.set_name), Escape cancels, and clicking elsewhere commits. No-op if
 * the model has no set_name hook or the key is not present. The widget also
 * starts a rename of the cursor item on F2. */
void bd_explorer_begin_rename(bd_id id, uint64_t key);

#endif
