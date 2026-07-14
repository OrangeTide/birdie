#ifndef BD_WIDGET_FORM_H
#define BD_WIDGET_FORM_H

#include "widget.h"

/*
 * Form controls -- the widgets a dialog form is built from, on the extension
 * API (widget_ext.h). See doc/gui/dialogs.md. This header holds the simple
 * ones (checkbox, radio group, numeric spinner); the drop-down combo is its
 * own widget (bd_widget_combo.h) because it opens a popup.
 *
 * All are created from a descriptor struct with an on-change callback, matching
 * the value-widget convention; a NULL descriptor takes the defaults. Labels are
 * borrowed (they must outlive the widget), like the core BD_LABEL_S attribute.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* ---- checkbox: a labeled boolean (a ticked box, not a switch) ---- */

typedef void (*bd_checkbox_cb)(bd_id id, void *arg, int checked);

typedef struct bd_checkbox_desc {
    const char    *label;    /* caption to the right of the box (NULL = none) */
    int            checked;  /* initial state */
    bd_checkbox_cb cb;       /* fired on every change (click / Space) */
    void          *arg;
} bd_checkbox_desc;

bd_id bd_checkbox_create(bd_id parent, const bd_checkbox_desc *desc, ...);
void  bd_checkbox_set(bd_id id, int checked);   /* no callback */
int   bd_checkbox_get(bd_id id);

#endif /* BD_WIDGET_FORM_H */
