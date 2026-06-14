#ifndef BD_BACKEND_GLES_H
#define BD_BACKEND_GLES_H

/*
 * Raw OpenGL ES 3 backend for birdie-gui, paired with the window.h windowing
 * layer. The non-ludica reference backend used by the widget gallery.
 *
 * Seeded from the smoltrek X11/EGL/GLES backend; adopted into birdie-gui.
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_backend.h"
#include "window.h"

/* The backend vtable. Pass &bd_backend_gles to bd_gui_init() after win_open()
 * has created the window and made its GL context current. */
extern const bd_backend bd_backend_gles;

/* Translate one neutral window event into a bd_event. Returns 1 if *out was
 * written, 0 for events the toolkit does not consume (close/resize/key-up). */
int bd_event_from_win(const win_event *ev, bd_event *out);

#endif
