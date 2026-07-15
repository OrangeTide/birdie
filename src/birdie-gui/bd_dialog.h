#ifndef BD_DIALOG_H
#define BD_DIALOG_H

#include "widget.h"

/*
 * bd_dialog -- a thin composition helper over the modal layer (bd_modal_*).
 *
 * It removes the boilerplate every hand-built dialog repeats: a titled panel, a
 * content column with standard padding, and a right-aligned button row, with
 * Enter and Escape wired to the default and cancel buttons. It is convenience,
 * not a new layout engine: bd_dialog_content() hands back the content container
 * so callers add whatever widgets they like, and bd_dialog_field() wraps the
 * common "label on the left, control on the right" row (with the PREF_H the
 * layout needs already set, so forms read top-to-bottom without the pref-size
 * gotcha). See doc/gui/dialogs.md.
 *
 * For bigger forms two more helpers compose the form-support widgets:
 * bd_dialog_group() drops a titled group box (bd_groupbox) into the form so
 * fields can be boxed into sections, and bd_dialog_scrollable() wraps the form
 * in a scroll-view (bd_scrollview) so it can outgrow the panel. See
 * doc/gui/forms-containers.md.
 *
 * The control cannot be reparented after creation, so bd_dialog_field() returns
 * the row for the caller to create the control INTO (with BD_GROW_I), rather
 * than taking an already-built control.
 *
 * Lifetime: bd_dialog_create allocates a handle and a detached widget subtree;
 * bd_dialog_free destroys both. Strings (title, field/button labels) are
 * borrowed, like BD_LABEL_S.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

typedef struct bd_dialog bd_dialog;

enum bd_dialog_role {
    BD_DIALOG_NORMAL = 0,   /* a plain button */
    BD_DIALOG_DEFAULT,      /* Enter triggers it; it does NOT auto-close (the
                               callback validates, then closes on success) */
    BD_DIALOG_CANCEL,       /* Escape triggers it; clicking it closes the dialog
                               after the callback runs */
};

/* Create a titled modal dialog `w` x `h` px. Returns NULL on allocation
 * failure. Must be called after bd_gui_init (it reads the theme). */
bd_dialog *bd_dialog_create(const char *title, int w, int h);

bd_id bd_dialog_panel(bd_dialog *d);     /* the modal panel (e.g. for focus) */
bd_id bd_dialog_content(bd_dialog *d);   /* the column to add widgets into */

/* A labeled field row inside the form. Create the control into the returned row
 * with BD_GROW_I so it fills the space beside the label. */
bd_id bd_dialog_field(bd_dialog *d, const char *label);

/* Add a titled group box (bd_groupbox) to the form and return it, so following
 * fields can be boxed into it. Add fields into a group with bd_dialog_field_in;
 * the group grows to hold them. Nest groups if you like. */
bd_id bd_dialog_group(bd_dialog *d, const char *title);

/* A labeled field row inside a specific `container` (e.g. a group from
 * bd_dialog_group), rather than the top-level form. Same row shape as
 * bd_dialog_field. */
bd_id bd_dialog_field_in(bd_dialog *d, bd_id container, const char *label);

/* Make the form scrollable: subsequent fields and groups go into a scroll-view
 * that scrolls when the form outgrows the panel, keeping the title and button
 * row fixed. Call once, before adding fields. */
void  bd_dialog_scrollable(bd_dialog *d);

/* Add a button to the (right-aligned) button row. `role` is a bd_dialog_role.
 * Add Cancel before the default for the conventional left-to-right order. */
bd_id bd_dialog_button(bd_dialog *d, const char *label, int role,
                       bd_callback_fn cb, void *arg);

void bd_dialog_open(bd_dialog *d);    /* show it modal (first field focused) */
void bd_dialog_close(bd_dialog *d);   /* dismiss (subtree kept; reopenable) */
void bd_dialog_free(bd_dialog *d);    /* destroy the subtree and the handle */

#endif /* BD_DIALOG_H */
