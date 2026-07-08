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
    WIN_EV_TEXT_COMMIT,   /* committed text (IME / compose): `text` */
    WIN_EV_TOUCH_DOWN,    /* touch point `touch` at `x`,`y` */
    WIN_EV_TOUCH_MOVE,
    WIN_EV_TOUCH_UP,
    WIN_EV_PEN_HOVER,     /* stylus in proximity, not touching */
    WIN_EV_PEN_DOWN,      /* stylus tip contact */
    WIN_EV_PEN_MOVE,      /* stylus moving while in contact */
    WIN_EV_PEN_UP,        /* stylus tip lift */
};

/* Pen flag bitmask (win_event.pen_flags). */
enum {
    WIN_PEN_INRANGE = 1 << 0,
    WIN_PEN_BARREL  = 1 << 1,
    WIN_PEN_ERASER  = 1 << 2,
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
    WIN_KEY_F2,
};

/* Flattened event. Only the fields relevant to `type` are valid. */
typedef struct {
    int      type;          /* enum win_ev_type */
    int      mods;          /* WIN_MOD_* bitmask */
    int      x, y;          /* mouse position (move / button) */
    int      button;        /* WIN_MOUSE_* (mouse down / up) */
    int      touch;         /* touch-point id (WIN_EV_TOUCH_*) */
    float    pressure;      /* stylus pressure 0..1 (WIN_EV_PEN_*) */
    float    tilt_x, tilt_y;/* stylus tilt in degrees (WIN_EV_PEN_*) */
    int      pen_flags;     /* WIN_PEN_* bitmask (WIN_EV_PEN_*) */
    float    scroll_dy;     /* wheel delta (scroll) */
    int      key;           /* WIN_KEY_* (key down / up) */
    int      repeat;        /* key down: 1 if an auto-repeat */
    unsigned codepoint;     /* Unicode codepoint (char) */
    char     text[64];      /* UTF-8 committed text (WIN_EV_TEXT_COMMIT) */
    int      width, height; /* new size (resize) */
    int      window;        /* originating window id (1 = primary) */
} win_event;

/* Open the primary window (id 1) and make its GL ES 3 context current. The
 * EGL display/context/config created here are shared by any further windows.
 * Returns 0 on success, -1 on failure. */
int win_open(const char *title, int width, int height);

/* Tear down every window plus the context and display. Safe to call after a
 * failed win_open(). */
void win_close(void);

/* Drawable size of the primary window in pixels. */
int win_width(void);
int win_height(void);

/* Monotonic time in seconds since an unspecified epoch. */
double win_time(void);

/* Pop one pending event (from any window) into *ev; ev->window is the id of
 * the window it came from. Returns 1 if an event was written, 0 when the
 * queue is empty. */
int win_poll(win_event *ev);

/* Present the primary window's rendered frame (== win_window_swap(1)). */
void win_swap(void);

/* ---- additional windows ----
 * Each shares the primary window's GL context, so resources made on one are
 * usable on all. win_window_begin() makes a window's surface current before
 * drawing into it; win_window_swap() presents it. */
int  win_window_open(const char *title, int w, int h); /* >0 id, 0 = fail */
void win_window_close(int id);
void win_window_begin(int id);
void win_window_swap(int id);
int  win_window_width(int id);
int  win_window_height(int id);
void win_window_set_title(int id, const char *title);
void win_window_minimize(int id);   /* iconify (XIconifyWindow) */
void win_window_restore(int id);    /* de-iconify + raise */

/* system clipboard (X11 CLIPBOARD). get returns a pointer valid until the next
 * clipboard call, or NULL if empty. */
void        win_clipboard_set(const char *utf8);
const char *win_clipboard_get(void);

/* input method: focus/unfocus the X input context (so it only intercepts keys
 * while a text field is focused), and place its candidate window at the caret. */
void win_ime_set_enabled(int on);
void win_ime_set_cursor_rect(int x, int y, int w, int h);

#endif
