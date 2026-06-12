#ifndef BD_BACKEND_LUDICA_H
#define BD_BACKEND_LUDICA_H

#include "bd_backend.h"
#include "ludica_input.h"

/*
 * ludica binding for the widget toolkit's bd_backend interface.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* The ludica-backed renderer vtable. Pass &bd_backend_ludica to bd_gui_init. */
extern const bd_backend bd_backend_ludica;

/*
 * Translate a native ludica event into the toolkit's neutral bd_event.
 * Returns 1 and fills *out for events the toolkit cares about; returns 0 for
 * events it ignores (resize, gamepad, key-up, ...), leaving *out untouched.
 */
int bd_event_from_lud(const lud_event_t *ev, bd_event *out);

#endif
