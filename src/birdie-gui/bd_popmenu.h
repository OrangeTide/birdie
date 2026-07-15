/* bd_popmenu.h : a free-floating popup / context menu over the overlay layer */

#ifndef BD_POPMENU_H
#define BD_POPMENU_H

#include "widget.h"

/*
 * bd_popmenu -- a transient popup menu (a right-click context menu, or any
 * app-driven command list) floated top-most through the shared overlay
 * primitive (bd_overlay_*).
 *
 * It is a MECHANISM, not a policy: the toolkit does not attach context menus to
 * widgets. The app decides when to open one -- from a right-click handler, an
 * editor-mode gesture, a toolbar button, whatever -- and where. This matters
 * for a passthrough canvas or a GLES game view that owns every click: such an
 * app forwards input to its own handlers and may raise a context menu only in a
 * special editor mode, entirely on its own terms.
 *
 * Open it at a screen position with a flat array of items; a click (or Enter)
 * runs the chosen item's action and closes the menu, a click outside or Escape
 * dismisses it. Only one popup or overlay is open at a time, so opening a menu
 * replaces any open combo drop-down and vice versa.
 *
 * Lifetime: the items array is copied on open (so the caller's array need not
 * persist), but the label strings and the action `user` pointers are borrowed
 * and must stay valid until the menu closes.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

enum {
    BD_POPMENU_SEPARATOR = 1u << 0,  /* a non-selectable divider (label/action ignored) */
    BD_POPMENU_DISABLED  = 1u << 1,  /* shown dimmed, not selectable */
};

typedef struct bd_popmenu_item {
    const char *label;                  /* borrowed, like BD_LABEL_S */
    void      (*action)(void *user);    /* run when chosen; NULL for none */
    void       *user;                   /* passed to action */
    unsigned    flags;                  /* BD_POPMENU_* */
} bd_popmenu_item;

/* Open a popup menu with its top-left near screen (x, y). The position is
 * clamped so the menu stays on screen. `items`/`count` describe the rows; the
 * array is copied, its strings and user pointers borrowed. A count <= 0 (or a
 * NULL array) is a no-op. */
void bd_popmenu_open(int x, int y, const bd_popmenu_item *items, int count);

/* Dismiss the popup if this module's menu is open (no-op otherwise). */
void bd_popmenu_close(void);

/* 1 while this module's popup menu is the open overlay. */
int  bd_popmenu_is_open(void);

#endif /* BD_POPMENU_H */
