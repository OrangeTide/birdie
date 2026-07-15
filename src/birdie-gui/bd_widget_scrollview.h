/* bd_widget_scrollview.h : a viewport that scrolls a child widget subtree */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef BD_WIDGET_SCROLLVIEW_H
#define BD_WIDGET_SCROLLVIEW_H

#include "widget.h"

/*
 * Scroll-view, built on the extension API (widget_ext.h). A fixed-size viewport
 * that clips a taller content column and scrolls it vertically, for a form with
 * more fields than fit its panel. Unlike the list / tree / editor (which scroll
 * their own drawn content), this scrolls a subtree of real child widgets:
 * labels, fields, checkboxes, group boxes, anything.
 *
 * Add widgets into the content container returned by bd_scrollview_content();
 * they stack in a column. When the column is taller than the viewport a
 * vertical scrollbar appears and the mouse wheel scrolls. Give the scroll-view
 * a size or BD_GROW_I like any container.
 *
 * For the content height to be known, the stacked children must carry a PREF_H
 * (the form controls and bd_dialog_field rows already do). Placing a bare core
 * BD_BUTTON directly in the scrolled content is not recommended -- put action
 * buttons in the dialog's button row, outside the scroll-view.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

typedef struct bd_scrollview_desc {
	int always_bar;   /* keep the scrollbar visible even when content fits */
} bd_scrollview_desc;

/* Register the scroll-view class (idempotent). Called by bd_scrollview_create. */
void bd_scrollview_register(void);

/*
 * Create a scroll-view. Trailing args are BD_* attributes ending in BD_END
 * (give it a size or BD_GROW_I). Fill the content column returned by
 * bd_scrollview_content(). A NULL descriptor takes the defaults.
 */
bd_id bd_scrollview_create(bd_id parent, const bd_scrollview_desc *desc, ...);

/* The content column to add widgets into (a BD_LAYOUT_COL panel). */
bd_id bd_scrollview_content(bd_id id);

/* Scroll so the content is offset by `y_px` from the top (clamped). */
void  bd_scrollview_scroll_to(bd_id id, int y_px);

/* Current scroll offset in pixels, and the full content height. */
int   bd_scrollview_scroll(bd_id id);
int   bd_scrollview_content_height(bd_id id);

#endif /* BD_WIDGET_SCROLLVIEW_H */
