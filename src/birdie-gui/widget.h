#ifndef BD_WIDGET_H
#define BD_WIDGET_H

#include <stdint.h>
#include "bd_backend.h"
#include "bd_theme.h"
#include "bd_draw.h"   /* bd_font_set, bd_draw_set_font_reader */

typedef unsigned int bd_id;
#define BD_NONE ((bd_id)0)

typedef void (*bd_callback_fn)(bd_id id, void *arg);

/* widget types */
enum {
	BD_FRAME = 1,
	BD_PANEL,
	BD_LABEL,
	BD_BUTTON,
	BD_TEXT,
	BD_MULTILINE,
	BD_LIST,
	BD_SCROLLBAR,
	BD_MENU,
	BD_NOTICE,
	BD_TAB_BAR,
	BD_INPUT_LINE,
};

/* layout modes */
enum {
	BD_LAYOUT_ROW   = 1,
	BD_LAYOUT_COL   = 2,
	BD_LAYOUT_FIXED = 3,
};

/*
 * Window gravity. A secondary top-level BD_FRAME rendered by the in-surface
 * window manager (single-surface backends, see bd_window_dock) sticks to the
 * named edge or corner of the surface and re-snaps there on resize. An edge
 * value stretches the window into a full-length dock strip along that edge; a
 * corner value pins the corner and keeps the window's preferred size.
 * BD_GRAVITY_NONE floats freely at its X/Y position.
 */
enum bd_gravity {
	BD_GRAVITY_NONE = 0,
	BD_GRAVITY_LEFT,
	BD_GRAVITY_RIGHT,
	BD_GRAVITY_TOP,
	BD_GRAVITY_BOTTOM,
	BD_GRAVITY_TOP_LEFT,
	BD_GRAVITY_TOP_RIGHT,
	BD_GRAVITY_BOTTOM_LEFT,
	BD_GRAVITY_BOTTOM_RIGHT,
};

/*
 * Widget anchor (BD_ANCHOR_I) — how a child sits within the cell the layout
 * gives it, by compass point. BD_ANCHOR_FILL (the default) stretches the child
 * to fill the cell, matching the pre-anchor behavior.
 *   - In a FIXED container the anchor pins the child (at its preferred size) to
 *     that edge/corner of the parent's content box and tracks it on resize;
 *     BD_X_I / BD_Y_I act as a margin inward from the anchored edge(s).
 *   - In a ROW/COL container the anchor's cross-axis component aligns the child
 *     on the cross axis at its preferred cross size instead of stretching it
 *     (e.g. BD_ANCHOR_W left-aligns a fixed-width child in a column).
 */
enum bd_anchor {
	BD_ANCHOR_FILL = 0,
	BD_ANCHOR_CENTER,
	BD_ANCHOR_N,
	BD_ANCHOR_S,
	BD_ANCHOR_E,
	BD_ANCHOR_W,
	BD_ANCHOR_NE,
	BD_ANCHOR_NW,
	BD_ANCHOR_SE,
	BD_ANCHOR_SW,
};

/*
 * Container packing (BD_PACK_I) — how a ROW/COL container distributes leftover
 * main-axis space among its children when they do not fill it (i.e. no child
 * grows). BD_PACK_START (the default) leaves the slack at the end. Ignored when
 * any child has BD_GROW_I, since grow already consumes the slack.
 */
enum bd_pack {
	BD_PACK_START = 0,
	BD_PACK_CENTER,
	BD_PACK_END,
	BD_PACK_SPACE_BETWEEN,
	BD_PACK_SPACE_AROUND,
};

/*
 * Attribute IDs — low 4 bits encode the value type so the varargs
 * reader knows which va_arg width to pull:
 *   0=end  1=int  2=string  3=pointer  4=callback  5=color  6=bool
 */
enum {
	BD_END        = 0x000,

	BD_WIDTH_I    = 0x011,
	BD_HEIGHT_I   = 0x021,
	BD_PREF_W_I   = 0x031,
	BD_PREF_H_I   = 0x041,
	BD_GROW_I     = 0x051,
	BD_X_I        = 0x061,
	BD_Y_I        = 0x071,
	BD_LAYOUT_I   = 0x081,
	BD_ROLE_I     = 0x091,
	BD_PAD_I      = 0x0A1,
	BD_GAP_I      = 0x0B1,
	BD_GRAVITY_I  = 0x0C1,   /* top-level frame: enum bd_gravity */
	BD_ANCHOR_I   = 0x0D1,   /* child: enum bd_anchor (cell alignment) */
	BD_PACK_I     = 0x0E1,   /* ROW/COL container: enum bd_pack (main-axis) */

	BD_LABEL_S    = 0x012,
	BD_NAME_S     = 0x022,

	BD_ON_CLICK_P = 0x013,
	BD_ON_CLOSE_P = 0x023,

	BD_ON_CLICK_F = 0x014,
	BD_ON_CLOSE_F = 0x024,

	BD_FG_C       = 0x015,
	BD_BG_C       = 0x025,

	BD_VISIBLE_B  = 0x016,
	BD_ENABLED_B  = 0x026,
	BD_MENU_PIN_B = 0x036,
	BD_PASSWORD_B = 0x046,   /* text field: mask input (password entry) */
	BD_LOCKED_B   = 0x056,   /* top-level frame: pin position, keep gravity */
};

/* tagged-union attribute for bd_create_v / bd_set_v */
typedef struct {
	int id;
	union {
		int i;
		const char *s;
		void *p;
		bd_callback_fn f;
		uint32_t c;
		bd_id wid;
	};
} bd_attr;

/* widget tree */
bd_id       bd_create(bd_id parent, int type, ...);
bd_id       bd_create_v(bd_id parent, int type, const bd_attr *attrs);
void        bd_destroy(bd_id id);
void        bd_set(bd_id id, ...);
void        bd_set_v(bd_id id, const bd_attr *attrs);
int         bd_get_i(bd_id id, int attr);
const char *bd_get_s(bd_id id, int attr);
bd_id       bd_parent(bd_id id);
bd_id       bd_first_child(bd_id id);
bd_id       bd_next_sibling(bd_id id);

/* GUI lifecycle — driven by the host's main loop. bd_gui_init() takes the
 * renderer/window backend the toolkit will draw through and an optional chrome
 * theme (NULL = bd_theme_default()); bd_gui_event() consumes neutral events
 * the host translates from its native ones. */
void bd_gui_init(const bd_backend *backend, const bd_theme *theme);

/* As bd_gui_init, but baking an explicit font set (each face an in-memory
 * buffer or a path) instead of the built-in DejaVu defaults. Pass NULL for
 * `fonts` for the defaults. Use this to ship a custom family, e.g. from blobs
 * embedded in the binary; call bd_draw_set_font_reader() first if faces are
 * given by path and must be resolved from embedded assets. */
void bd_gui_init_fonts(const bd_backend *backend, const bd_theme *theme,
                       const bd_font_set *fonts);
void bd_gui_cleanup(void);
void bd_gui_layout(int win_w, int win_h);
void bd_gui_render(void);
int  bd_gui_event(const bd_event *ev);

/* The active chrome theme, so extension widgets can match the chrome. */
const bd_theme *bd_gui_theme(void);

/* The widget that currently holds keyboard focus, or BD_NONE. Focus moves on
 * click and on Tab / Shift-Tab traversal. */
bd_id bd_focused(void);

/* BD_LIST: a scrolling, selectable list of newline-separated items. Set the
 * items with BD_LABEL_S (or bd_list_set_items); the selection callback runs on
 * activation (double-click or Enter) via BD_ON_CLICK_F. The host reads the
 * selected row index (-1 if none) and can set it. */
void bd_list_set_items(bd_id id, const char *newline_separated);
int  bd_list_count(bd_id id);
int  bd_list_selected(bd_id id);
void bd_list_select(bd_id id, int row);

/* BD_TAB_BAR: a row of skeuomorphic folder tabs for multiple sessions. Set the
 * tab labels with BD_LABEL_S (or bd_tabbar_set_tabs); the active tab is index
 * 0 by default. Clicking a tab or Left/Right (when focused) changes it and
 * fires BD_ON_CLICK_F; the host reads/sets the active index. */
void bd_tabbar_set_tabs(bd_id id, const char *newline_separated);
int  bd_tabbar_count(bd_id id);
int  bd_tabbar_active(bd_id id);
void bd_tabbar_set_active(bd_id id, int index);

/* BD_SCROLLBAR: a standalone scrollbar. Orientation follows the shape (a bar
 * taller than wide is vertical). pos is the thumb position in [0,1]; frac is
 * the thumb size as a fraction of the track (visible/total). Dragging fires
 * BD_ON_CLICK_F; the host reads bd_scrollbar_value. */
void  bd_scrollbar_set(bd_id id, float pos, float frac);
float bd_scrollbar_value(bd_id id);

/* BD_NOTICE: a modal confirmation/alert. bd_notice_open() shows a centered
 * panel over a dimmed backdrop with `message` and one button per '\n'-separated
 * label in `buttons` (NULL/"" defaults to "OK"); while it is up, input to the
 * rest of the UI is blocked. The callback runs with the chosen button index
 * (or -1 for Escape) and the notice is then closed automatically. */
typedef void (*bd_notice_cb)(bd_id notice, int button, void *arg);
bd_id bd_notice_open(const char *message, const char *buttons,
                     bd_notice_cb cb, void *arg);
void  bd_notice_close(bd_id notice);

/* Generic modal dialog. Build a top-level container (parent BD_NONE) with a
 * BD_PREF_W / BD_PREF_H and fill it with any widgets (panels, tables, buttons,
 * inputs); bd_modal_open() then shows it centered over a dimmed backdrop and
 * routes all input to it through the normal dispatch, so its widgets behave
 * exactly as in a frame. Escape or bd_modal_close() dismisses it (the dialog
 * subtree is not destroyed, so it can be reopened). One modal at a time; it
 * layers above the main UI and below a bd_notice alert. The dialog is detached
 * from the main frame, so the app keeps and reuses the bd_id. */
void  bd_modal_open(bd_id dialog);
void  bd_modal_close(bd_id dialog);
bd_id bd_modal_active(void);   /* the open dialog, or BD_NONE */

/* Multiple windows: each top-level BD_FRAME (parent BD_NONE) is a window. On a
 * backend with multi-window support the toolkit gives each a native window and
 * tags events with its id (bd_event.window). bd_frame_for_window() maps a
 * backend window id back to its frame so a host can, e.g., destroy the right
 * frame on a window-close event. Returns BD_NONE if no frame owns that id. */
bd_id bd_frame_for_window(int window_id);

/*
 * In-surface window manager. On a single-surface backend (multi_window == 0,
 * e.g. ludica or SDL) the first top-level BD_FRAME is the full-surface desktop
 * and every later top-level BD_FRAME is a floating window the toolkit draws,
 * decorates with a title bar, and lets the user drag, raise, snap, and dock.
 * (On a native multi-window backend each frame is a real OS window instead and
 * these calls set the same gravity/lock state, ignored until such a backend
 * grows client-side move support.)
 *
 * Gravity/snap/lock semantics:
 *   - Dragging a window's title bar moves it. Released near a surface edge or
 *     corner it snaps and docks there (gravity is set); dragged away it floats.
 *   - A docked window re-snaps to its edge/corner whenever the surface resizes.
 *   - Locking pins the window: it can no longer be dragged but keeps its
 *     gravity and still re-snaps on resize (a fixed dock). Unlock to move again.
 *
 * These are thin wrappers over bd_set/bd_get_i with BD_GRAVITY_I / BD_LOCKED_B;
 * position a floating window with BD_X_I / BD_Y_I and size it with
 * BD_PREF_W_I / BD_PREF_H_I.
 */
void bd_window_dock(bd_id frame, int gravity);      /* enum bd_gravity */
void bd_window_move(bd_id frame, int x, int y);     /* float to (x,y), undock */
void bd_window_set_locked(bd_id frame, int locked);
int  bd_window_locked(bd_id frame);
int  bd_window_gravity(bd_id frame);

#endif
