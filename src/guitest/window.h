/*
 * window.h : thin platform window + GL ES context, flattened to a neutral
 * event stream so callers never include Xlib. Backs birdie-gui's GLES
 * reference backend (x11_window.c on Linux).
 *
 * Seeded from the smoltrek windowing layer; adopted into birdie-gui.
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */
#ifndef BD_WINDOW_H
#define BD_WINDOW_H

/*
 * One top-level window with an OpenGL ES 3 context. The host owns the main
 * loop: drain win_poll() each frame, render, then win_swap(). Native windowing
 * events are flattened into the neutral win_event below so the rest of the
 * program never includes Xlib. A second backend (Win32 + ANGLE) can implement
 * the same interface later.
 */

/* Event kinds delivered by win_poll(). */
enum win_ev_type {
    WIN_EV_NONE = 0,
    WIN_EV_CLOSE,           /* window manager close request */
    WIN_EV_RESIZE,          /* width/height changed */
    WIN_EV_MOUSE_MOVE,
    WIN_EV_MOUSE_DOWN,
    WIN_EV_MOUSE_UP,
    WIN_EV_MOUSE_SCROLL,
    WIN_EV_KEY_DOWN,
    WIN_EV_KEY_UP,
    WIN_EV_CHAR,
};

/* Mouse buttons. */
enum {
    WIN_MOUSE_LEFT = 1,
    WIN_MOUSE_RIGHT,
    WIN_MOUSE_MIDDLE,
};

/* Modifier bitmask. */
enum {
    WIN_MOD_SHIFT = 1 << 0,
    WIN_MOD_CTRL  = 1 << 1,
    WIN_MOD_ALT   = 1 << 2,
};

/*
 * Key codes. Printable letters use their ASCII uppercase value so a block of
 * A..Z maps arithmetically; non-printable keys live above 256. This matches
 * the birdie bd_backend scheme so the GUI backend translation stays trivial.
 */
enum {
    WIN_KEY_UNKNOWN = 0,
    WIN_KEY_A = 65,             /* 'A'..'Z' == 65..90 */
    WIN_KEY_Z = 90,

    WIN_KEY_LEFT = 256,
    WIN_KEY_RIGHT,
    WIN_KEY_UP,
    WIN_KEY_DOWN,
    WIN_KEY_HOME,
    WIN_KEY_END,
    WIN_KEY_BACKSPACE,
    WIN_KEY_DELETE,
    WIN_KEY_ENTER,
    WIN_KEY_ESCAPE,
    WIN_KEY_TAB,
};

/* Flattened event. Only the fields relevant to `type` are valid. */
typedef struct {
    int      type;          /* enum win_ev_type */
    int      mods;          /* WIN_MOD_* bitmask */
    int      x, y;          /* mouse position (move / button) */
    int      button;        /* WIN_MOUSE_* (mouse down / up) */
    float    scroll_dy;     /* wheel delta (scroll) */
    int      key;           /* WIN_KEY_* (key down / up) */
    unsigned codepoint;     /* Unicode codepoint (char) */
    int      width, height; /* new size (resize) */
} win_event;

/* Open the window and make its GL ES 3 context current. Returns 0 on success,
 * -1 on failure. */
int win_open(const char *title, int width, int height);

/* Tear down the context, surface, and window. Safe to call after a failed
 * win_open(). */
void win_close(void);

/* Current drawable size in pixels. */
int win_width(void);
int win_height(void);

/* Monotonic time in seconds since an unspecified epoch. */
double win_time(void);

/* Pop one pending event into *ev. Returns 1 if an event was written, 0 when
 * the queue is empty. */
int win_poll(win_event *ev);

/* Present the rendered frame. Blocks for vsync, so it also paces the loop. */
void win_swap(void);

#endif
