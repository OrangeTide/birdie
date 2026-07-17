#ifndef BD_FINDBAR_H
#define BD_FINDBAR_H

#include "widget.h"
#include "bd_widget_editor.h"   /* BD_FIND_* flags + the editor find API */

/*
 * bd_findbar -- a composed find/replace bar bound to a bd_editor, in the spirit
 * of bd_dialog: a thin helper that builds ordinary widgets, not a new widget
 * class. It lays out a compact bar under `parent`: a search field (live -- it
 * re-searches as you type, Enter jumps to the next match), a match counter,
 * previous / next / close buttons, and, when with_replace is set, a replacement
 * field with Replace and Replace All.
 *
 * Editing the search field drives bd_editor_find on the target editor, so the
 * highlight lives on the editor's find layer and composes with syntax and app
 * styling. Escape in the search field or the close button dismisses the bar and
 * fires the on_close callback, so the app can hide or free it.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

typedef struct bd_findbar bd_findbar;

/* Build a bar under `parent` bound to `editor`. with_replace adds the second
 * (replace) row. Returns NULL on allocation failure. */
bd_findbar *bd_findbar_create(bd_id parent, bd_id editor, int with_replace);

/* Free the handle. Destroys the bar's widget subtree too, so an app that keeps
 * the bar in a togglable slot can create/free it on demand. */
void bd_findbar_free(bd_findbar *fb);

bd_id bd_findbar_widget(const bd_findbar *fb);   /* bar root (to show/hide) */
bd_id bd_findbar_editor(const bd_findbar *fb);

/* Focus the search field and (re)run the current search. Call when revealing. */
void bd_findbar_open(bd_findbar *fb);

/* Match flags (BD_FIND_ICASE / BD_FIND_WORD); re-runs the search. */
void     bd_findbar_set_flags(bd_findbar *fb, unsigned flags);
/* Set the search text and run the search (e.g. seed from the selection). */
void     bd_findbar_set_query(bd_findbar *fb, const char *needle);
unsigned bd_findbar_flags(const bd_findbar *fb);

/* Fired when the user dismisses the bar (Escape in the field or close button).
 * The callback receives the bar's root widget id. */
void bd_findbar_on_close(bd_findbar *fb, bd_callback_fn fn, void *user);

#endif
