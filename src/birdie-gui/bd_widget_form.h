#ifndef BD_WIDGET_FORM_H
#define BD_WIDGET_FORM_H

#include "widget.h"
#include "bd_widget_value.h"   /* shared BD_HORIZONTAL / BD_VERTICAL enum */

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

/* ---- radio group: one exclusive choice among labeled options ---- */

typedef void (*bd_radio_cb)(bd_id id, void *arg, int index);

typedef struct bd_radio_desc {
    const char *const *labels;   /* option captions, `count` of them (borrowed) */
    int             count;       /* number of options */
    int             selected;    /* initial selection, -1 for none */
    int             orient;      /* BD_HORIZONTAL (default) or BD_VERTICAL */
    bd_radio_cb     cb;          /* fired when the selection changes */
    void           *arg;
} bd_radio_desc;

bd_id bd_radio_create(bd_id parent, const bd_radio_desc *desc, ...);
void  bd_radio_set(bd_id id, int index);   /* no callback */
int   bd_radio_get(bd_id id);

/* ---- numeric spinner: an integer field with up/down steppers ---- */

typedef void (*bd_spinner_cb)(bd_id id, void *arg, int value);

typedef struct bd_spinner_desc {
    int             min;     /* inclusive lower bound (default 0) */
    int             max;     /* inclusive upper bound; <= min means "no cap" */
    int             step;    /* per stepper-click / arrow-key delta (default 1) */
    int             value;   /* initial value (clamped into range) */
    bd_spinner_cb   cb;      /* fired when the value changes */
    void           *arg;
} bd_spinner_desc;

bd_id bd_spinner_create(bd_id parent, const bd_spinner_desc *desc, ...);
void  bd_spinner_set(bd_id id, int value);   /* clamp to range; no callback */
int   bd_spinner_get(bd_id id);

#endif /* BD_WIDGET_FORM_H */
