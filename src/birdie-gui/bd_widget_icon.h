#ifndef BD_WIDGET_ICON_H
#define BD_WIDGET_ICON_H

#include "widget.h"
#include "widget_ext.h"   /* bd_dnd_payload */
#include "bd_backend.h"   /* bd_texture */
#include <stdint.h>

/*
 * Icon cell: the shared "slot" the grid/strip icon widgets are built on, plus a
 * standalone single-icon widget. An icon is a beveled tile (bd_draw_tile) with
 * an image, an optional caption and stack badge, dragged as a cross-widget
 * drag-and-drop payload and activated by a double-click. bd_widget_inventory,
 * bd_widget_dock, and bd_widget_actionbar render their cells and start their
 * drags through the helpers here, so the four widgets share one cell vocabulary.
 *
 * As a standalone widget (bd_icon_create) it is an app launcher or a desktop
 * icon: a free-standing icon the user double-clicks to activate (launch / open
 * / restore a minimized window) and can drag onto any icon-accepting target
 * (an action bar, another icon). A BD_MANAGED_CANVAS uses it for its
 * minimize-to-desktop-icon behavior.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* One icon cell's content: the fields common to every icon widget. Extension
 * widgets wrap this in their own per-slot struct when they need more (the
 * action bar adds a hotkey; the inventory adds a tooltip + user pointer). */
typedef struct bd_icon_desc {
	uint64_t    key;      /* app identity, echoed back in callbacks */
	const char *label;    /* caption; NULL/"" = none */
	bd_texture  icon;     /* tile image; id 0 = empty cell */
	int         count;    /* stack badge; > 1 draws "xN"; 0/1 = none */
	int         enabled;  /* 0 = dimmed */
} bd_icon_desc;

/* ---- shared cell helpers (used by inventory / dock / action bar) ---- */

/* Draw one icon cell's base tile at (rx,ry): the same bd_draw_tile the widgets
 * used before, unpacked from a descriptor. Selection backgrounds and focus /
 * drop rings stay with each widget (their geometry and colors differ). */
void bd_icon_draw(float rx, float ry, int cell_w, int pad, int icon_size,
                  const bd_icon_desc *d, uint32_t bg, uint32_t border,
                  uint32_t fg);

/* Begin a cross-widget drag carrying this cell's content (source + key + label
 * + icon + user). The caller owns the press/threshold tracking. */
void bd_icon_dnd_begin(bd_id source, const bd_icon_desc *d, void *user);

/* ---- standalone icon widget ---- */

/* Fires when the icon is activated (double-click or Enter). */
typedef void (*bd_icon_activate_fn)(bd_id icon, uint64_t key, void *user);
/* Fires when an icon/payload is dropped onto this icon; return 1 to accept
 * (the icon then adopts the payload's content), 0 to reject. */
typedef int  (*bd_icon_drop_fn)(bd_id icon, const bd_dnd_payload *p, void *user);

/* Create a standalone icon. desc may be NULL (empty icon). Trailing args are
 * BD_* attributes ending in BD_END. */
bd_id bd_icon_create(bd_id parent, const bd_icon_desc *desc, ...);

/* Replace the icon's content (label is copied). */
void bd_icon_set(bd_id id, const bd_icon_desc *desc);
void bd_icon_set_texture(bd_id id, bd_texture tex);
uint64_t bd_icon_key(bd_id id);

void bd_icon_on_activate(bd_id id, bd_icon_activate_fn fn, void *user);
/* Installing a drop handler makes the icon a drop target. */
void bd_icon_on_drop(bd_id id, bd_icon_drop_fn fn, void *user);

#endif
