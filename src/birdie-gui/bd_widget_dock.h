#ifndef BD_WIDGET_DOCK_H
#define BD_WIDGET_DOCK_H

#include "widget.h"
#include <stdint.h>

/*
 * Dock widget, built on the extension API (widget_ext.h). A NeXTSTEP /
 * WindowMaker-style strip of fixed-size tiles, one per *minimized* top-level
 * window: clicking a tile restores (and raises) its window. See
 * doc/gui/dock-design.md for the full design.
 *
 * SINGLE-SURFACE ONLY. The dock is a projection of the toolkit's in-surface
 * window manager (minimized floating BD_FRAMEs), which runs only when the
 * backend cannot open native windows (bd_backend.multi_window == 0: ludica,
 * SDL, the headless stub). On a native multi-window backend (the GLES gallery)
 * each frame is a real OS window managed by the host's window manager, there
 * are no in-surface frames to minimize, and this widget stays empty. Extending
 * docks/panels to native windows is a separate design (see the design doc).
 *
 * The dock is *derived state*: its tile set is a projection of which frames are
 * minimized (bd_window_minimize / bd_window_list), never an independent list.
 * It hugs a screen edge or corner chosen with bd_dock_set_gravity (reusing
 * enum bd_gravity), orienting itself into a vertical or horizontal strip; place
 * it in a BD_LAYOUT_FIXED container (typically the desktop root) so it can
 * anchor to that edge. Tiles render exactly like bd_widget_inventory slots
 * (shared bd_draw_tile primitive), so the two widgets match; the icon is a
 * bd_texture the host may update each frame to animate (thumbnail / genie).
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* One tile's content, filled by the model's get() for that call only (mirrors
 * bd_inventory_item). Borrowed strings need only survive the get() call. */
typedef struct bd_dock_item {
	uint64_t    key;      /* the frame's bd_id, echoed back (unused by v1) */
	const char *label;    /* caption (default: the frame's BD_LABEL_S) */
	bd_texture  icon;     /* tile image; id 0 = empty square. Update its pixels
	                         each frame to animate a thumbnail or genie. */
	int         count;    /* > 1 draws a small "xN" badge */
	int         enabled;  /* 0 = dimmed */
} bd_dock_item;

/* Optional per-frame tile content override. get() may be NULL (or the whole
 * model NULL) to use the defaults (empty icon + frame title). */
typedef struct bd_dock_model {
	void (*get)(void *ctx, bd_id frame, bd_dock_item *out);
	void  *ctx;
} bd_dock_model;

/* Register the dock class (idempotent). Called by bd_dock_create. */
void bd_dock_register(void);

/*
 * Create a dock. `model` is optional (NULL = defaults) and copied by value.
 * Trailing args are BD_* attributes ending in BD_END. The dock sizes itself to
 * its tiles and anchors to its gravity edge, so it needs no explicit size; set
 * its gravity with bd_dock_set_gravity (default BD_GRAVITY_LEFT).
 */
bd_id bd_dock_create(bd_id parent, const bd_dock_model *model, ...);

/* Which edge/corner the dock hugs (enum bd_gravity). LEFT/RIGHT and the four
 * corners make a vertical strip; TOP/BOTTOM make a horizontal one. */
void bd_dock_set_gravity(bd_id dock, int gravity);

/* Tile (icon) edge length in pixels. Default 64. */
void bd_dock_set_tile_size(bd_id dock, int px);

/* Number of tiles currently shown (== minimized top-level windows). */
int  bd_dock_count(bd_id dock);

#endif
