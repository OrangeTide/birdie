/* bd_widget_split.h : resizable split-pane (sash) container widget */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef BD_WIDGET_SPLIT_H
#define BD_WIDGET_SPLIT_H

#include "widget.h"

/*
 * Split pane, built on the extension API (widget_ext.h). A container of two
 * panes separated by a draggable sash that repartitions them, laid out either
 * side-by-side (BD_SPLIT_HORIZONTAL, a vertical sash) or stacked
 * (BD_SPLIT_VERTICAL, a horizontal sash).
 *
 * Each pane is a full container (a BD_PANEL) the host fills with any widget
 * subtree, exactly like a tab-view pane. Dragging the sash adjusts the split
 * ratio; a window resize preserves that ratio. Splits nest: a pane may itself
 * hold another split, so the classic four-list-over-code browser is a vertical
 * split whose top pane is a horizontal split.
 *
 * The ratio is the first pane's fraction of the splittable extent (the widget
 * width or height minus the sash), clamped so neither pane falls below the
 * per-pane minimum size. Put the split in a container that gives it room
 * (BD_GROW_I or an explicit size); it arranges the panes and sash itself.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* Orientation passed to bd_split_create. HORIZONTAL puts the panes side by
 * side (a vertical sash between them); VERTICAL stacks them (a horizontal
 * sash). */
enum {
	BD_SPLIT_HORIZONTAL = 1,
	BD_SPLIT_VERTICAL,
};

/* Register the split (and its private sash) class, idempotent. Called by
 * bd_split_create. */
void bd_split_register(void);

/*
 * Create a split. `orient` is BD_SPLIT_HORIZONTAL or BD_SPLIT_VERTICAL.
 * Trailing args are BD_* attributes ending in BD_END (give it BD_GROW_I or a
 * size so it fills its slot). Both panes are created immediately; reach them
 * with bd_split_pane and fill them with widgets. The initial ratio is 0.5.
 */
bd_id bd_split_create(bd_id parent, int orient, ...);

/* The pane content container: index 0 = first (left / top), 1 = second (right
 * / bottom). Returns BD_NONE for a bad index or a non-split id. */
bd_id bd_split_pane(bd_id split, int index);

/* The split ratio, the first pane's fraction of the splittable extent [0,1]. */
float bd_split_ratio(bd_id split);

/* Set the split ratio (clamped to keep both panes >= the minimum size). Does
 * not fire the change callback, matching the programmatic-set convention. */
void bd_split_set_ratio(bd_id split, float ratio);

/* Minimum size in pixels for each pane (default 32). The sash cannot be
 * dragged past this on either side. */
void bd_split_set_min_size(bd_id split, int px);

/* Sash thickness in pixels (default 6). */
void bd_split_set_sash_size(bd_id split, int px);

/* Optional: called after the ratio changes by a sash drag, with the split id.
 * Poll bd_split_ratio for the new value. A programmatic bd_split_set_ratio does
 * not fire it. */
void bd_split_on_change(bd_id split, bd_callback_fn fn, void *data);

#endif /* BD_WIDGET_SPLIT_H */
