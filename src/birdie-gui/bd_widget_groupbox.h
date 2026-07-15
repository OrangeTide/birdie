/* bd_widget_groupbox.h : labeled etched-border fieldset container */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

#ifndef BD_WIDGET_GROUPBOX_H
#define BD_WIDGET_GROUPBOX_H

#include "widget.h"

/*
 * Group box, built on the extension API (widget_ext.h). A container that draws
 * a captioned etched border (OPEN LOOK / Motif fieldset) around its children,
 * for grouping related form fields ("Connection", "Appearance") in a dialog.
 *
 * The group box IS the content container: add fields straight into it, the way
 * you would a panel. It lays them out in a column below a reserved title band,
 * and draws the groove with the caption inset in the top edge. Give it a size
 * or BD_GROW_I like any container. Group boxes nest.
 *
 * The title string is borrowed (it must outlive the widget), like the core
 * BD_LABEL_S attribute.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

typedef struct bd_groupbox_desc {
	const char *title;   /* caption in the top border (borrowed; NULL = none) */
} bd_groupbox_desc;

/* Register the group-box class (idempotent). Called by bd_groupbox_create. */
void bd_groupbox_register(void);

/*
 * Create a group box. Trailing args are BD_* attributes ending in BD_END (give
 * it a size or BD_GROW_I). Add fields into the returned id; they stack in a
 * column below the caption band. A NULL descriptor means no title.
 */
bd_id bd_groupbox_create(bd_id parent, const bd_groupbox_desc *desc, ...);

/* Change / read the caption (borrowed). */
void        bd_groupbox_set_title(bd_id id, const char *title);
const char *bd_groupbox_title(bd_id id);

#endif /* BD_WIDGET_GROUPBOX_H */
