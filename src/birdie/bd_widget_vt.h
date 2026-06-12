#ifndef BD_WIDGET_VT_H
#define BD_WIDGET_VT_H

#include "widget.h"

/*
 * VT terminal — an extension widget built on the toolkit's widget-class API
 * (widget_ext.h). It is the reference example of "define a new kind of widget"
 * and the one widget that depends on libvt; core widget.c knows nothing about
 * it.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* Register the terminal widget class. Safe to call more than once; the first
 * call registers, later calls are no-ops. bd_terminal_create() also registers
 * lazily, so calling this explicitly is optional. */
void bd_terminal_register(void);

/* Create a terminal widget. Accepts the same BD_* attribute varargs as
 * bd_create(), terminated by BD_END. */
bd_id bd_terminal_create(bd_id parent, ...);

/* Feed bytes (escape sequences and text) to a terminal widget. len < 0 means
 * data is NUL-terminated. No-op if id is not a terminal. */
void bd_terminal_write(bd_id id, const char *data, int len);

#endif
