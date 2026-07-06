#ifndef BD_BACKEND_SDL3_H
#define BD_BACKEND_SDL3_H

#include "bd_backend.h"

#include <SDL3/SDL.h>

/*
 * SDL3 binding for the widget toolkit's bd_backend interface.
 *
 * A reference backend alongside ludica (bd_backend_ludica.c) and the raw
 * X11/EGL/GLES gallery (backend-gles/). The toolkit and renderer are untouched;
 * only this vtable and the SDL event translation change. Needs SDL3 (resolved
 * through pkg-config) and an OpenGL ES 3 loader.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * The SDL3-backed renderer vtable. Pass &bd_backend_sdl3 to bd_gui_init.
 *
 * Its clear hook is NULL: the host owns the frame clear, so the UI composites
 * over whatever the host drew that frame (e.g. a 3D scene). If you want the
 * toolkit to own the frame, clear it yourself at the top of each iteration.
 */
extern const bd_backend bd_backend_sdl3;

/*
 * Create the SDL window and OpenGL ES 3.0 context the backend draws through,
 * make the context current, and enable vsync. Initializes SDL's video
 * subsystem if it is not already up. Returns the window (also retained
 * internally, for the vtable) or NULL on failure. Call before
 * bd_gui_init(&bd_backend_sdl3, ...); the caller still owns the frame loop and
 * SDL_GL_SwapWindow.
 */
SDL_Window *bd_backend_sdl3_open(const char *title, int w, int h);

/*
 * Destroy the context and window created by bd_backend_sdl3_open and shut SDL
 * down.
 */
void bd_backend_sdl3_close(void);

/*
 * Translate one native SDL event into the toolkit's neutral bd_event. Returns 1
 * and fills *out for events the toolkit cares about; returns 0 for events it
 * ignores (SDL_EVENT_QUIT, unmapped keys, ...), leaving *out untouched.
 */
int bd_event_from_sdl(const SDL_Event *ev, bd_event *out);

#endif
