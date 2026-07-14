#ifndef BD_WIDGET_COMBO_H
#define BD_WIDGET_COMBO_H

#include "widget.h"

/*
 * BD_COMBO -- a drop-down selector: a closed box showing the current item that
 * opens a floating list of options on click; pick one of N. Built on the
 * extension API (widget_ext.h) over the shared top-most overlay primitive
 * (bd_overlay_*), so its list floats above sibling widgets, and above a dialog
 * when the combo sits inside one.
 *
 * Created from a descriptor with an on-change callback, matching the value- and
 * form-widget convention. The items array and the strings in it are BORROWED
 * (the array is copied into owned storage, the strings are not), so the strings
 * must outlive the widget. A NULL descriptor takes the defaults.
 *
 * Keyboard: Up/Down move the highlight while open, Enter picks it, Escape
 * dismisses. Closed, Space or Enter opens the list.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

typedef void (*bd_combo_cb)(bd_id id, void *arg, int index);

typedef struct bd_combo_desc {
    const char *const *items;       /* option captions, `count` of them */
    int             count;          /* number of options */
    int             selected;       /* initial selection, -1 for none */
    const char     *placeholder;    /* shown while nothing is selected */
    bd_combo_cb     cb;             /* fired when the selection changes */
    void           *arg;
} bd_combo_desc;

bd_id bd_combo_create(bd_id parent, const bd_combo_desc *desc, ...);
void  bd_combo_set(bd_id id, int index);   /* clamp to [-1,count); no callback */
int   bd_combo_get(bd_id id);              /* selected index, -1 if none */

#endif /* BD_WIDGET_COMBO_H */
