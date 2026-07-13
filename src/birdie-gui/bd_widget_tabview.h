#ifndef BD_WIDGET_TABVIEW_H
#define BD_WIDGET_TABVIEW_H

#include "widget.h"

/*
 * Tab view widget, built on the extension API (widget_ext.h). A tabbed
 * container: a BD_TAB_BAR folder-tab strip on top and a content area below in
 * which exactly one *pane* is shown at a time. Clicking a tab (or Left/Right
 * when the strip has focus) raises that pane.
 *
 * Where BD_TAB_BAR is only the strip (it tracks an active index and fires a
 * callback), a tab view owns the strip AND the panes and swaps them. Each pane
 * is a full container the host fills with any widgets: labels, an inventory, an
 * editor, a texture-backed view of a live GLES animation, another layout, and
 * so on. A pane is as complex as a whole window; only the active one renders and
 * takes input, the rest are hidden (skipped in render and hit-testing).
 *
 * Layout: the panes overlap in one fixed content rectangle, so switching tabs
 * costs nothing and every pane keeps its own laid-out contents. Put the tab view
 * in a container that gives it room (BD_GROW_I or an explicit size); it arranges
 * the strip and content itself.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* Register the tab-view class (idempotent). Called by bd_tabview_create. */
void bd_tabview_register(void);

/*
 * Create a tab view. Trailing args are BD_* attributes ending in BD_END (give
 * it BD_GROW_I or a size so it fills its slot). It starts with no panes; add
 * them with bd_tabview_add_pane.
 */
bd_id bd_tabview_create(bd_id parent, ...);

/*
 * Add a pane titled `label` and return its content container (a BD_PANEL): the
 * host fills it with any widget subtree. The first pane added is active. Returns
 * BD_NONE if the view is full or invalid.
 */
bd_id bd_tabview_add_pane(bd_id tabview, const char *label);

/* Number of panes. */
int  bd_tabview_count(bd_id tabview);

/* The active pane index (-1 if none) and its content container. */
int  bd_tabview_selected(bd_id tabview);
bd_id bd_tabview_pane(bd_id tabview, int index);

/* Switch to pane `index` (clamped). Does not fire the change callback (a
 * programmatic set), matching bd_tabbar_select. */
void bd_tabview_select(bd_id tabview, int index);

/* Optional: called after the active pane changes by a tab click / key, with the
 * tab-view id. Poll bd_tabview_selected() for the new index. */
void bd_tabview_on_change(bd_id tabview, bd_callback_fn fn, void *data);

#endif
