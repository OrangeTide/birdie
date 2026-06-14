/*
 * x11_window.c : window.h backend for X11 + EGL + OpenGL ES 3.
 *
 * Holds a small table of top-level windows that share one EGLDisplay,
 * EGLConfig, and EGLContext (so textures/shaders made on one window work on
 * all). win_open() creates the primary window as id 1; win_window_open() adds
 * more. win_poll() drains events from every window and tags each with the id
 * it came from.
 *
 * Seeded from the smoltrek windowing layer; adopted into birdie-gui.
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "window.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#ifndef EGL_OPENGL_ES3_BIT_KHR
#  define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif

#define MAX_WINDOWS 16

static const EGLint config_attribs[] = {
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
    EGL_RED_SIZE,        8,
    EGL_GREEN_SIZE,      8,
    EGL_BLUE_SIZE,       8,
    EGL_ALPHA_SIZE,      8,
    EGL_DEPTH_SIZE,      16,
    EGL_STENCIL_SIZE,    8,
    EGL_NONE,
};

static const EGLint context_attribs[] = {
    EGL_CONTEXT_CLIENT_VERSION, 3,
    EGL_NONE,
};

/* One top-level window. id 0 means an empty slot. */
struct win_slot {
    int        id;
    Window     xwindow;
    XIC        xic;
    EGLSurface egl_surface;
    int        width, height;
};

/* Display + GL context shared by every window. */
static struct {
    Display   *xdisplay;
    Atom       wm_delete_window;
    Atom       a_clipboard, a_utf8, a_targets, a_prop;
    XIM        xim;
    EGLDisplay egl_display;
    EGLConfig  egl_config;
    EGLContext egl_context;
    struct win_slot windows[MAX_WINDOWS];   /* slot 0 is window id 1 */
    int        next_id;
    char       clip[4096];                  /* our owned clipboard text */
} g;

/* ------------------------------------------------------------------ */
/* slot lookup                                                        */
/* ------------------------------------------------------------------ */

static struct win_slot *
slot_by_id(int id)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g.windows[i].id == id)
            return &g.windows[i];
    return NULL;
}

static struct win_slot *
slot_by_xwindow(Window w)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g.windows[i].id && g.windows[i].xwindow == w)
            return &g.windows[i];
    return NULL;
}

static struct win_slot *
slot_free(void)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g.windows[i].id == 0)
            return &g.windows[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* window create / destroy                                            */
/* ------------------------------------------------------------------ */

static int
window_create(const char *title, int width, int height)
{
    struct win_slot *s = slot_free();
    if (!s)
        return 0;

    Screen *screen = DefaultScreenOfDisplay(g.xdisplay);
    Window root = RootWindowOfScreen(screen);
    unsigned long black = BlackPixelOfScreen(screen);

    XSetWindowAttributes wattr = {
        .event_mask = StructureNotifyMask | KeyPressMask | KeyReleaseMask
            | ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
        .background_pixel = black,
        .border_pixel = black,
    };

    int x = (WidthOfScreen(screen) - width) / 2;
    int y = (HeightOfScreen(screen) - height) / 2;
    Window xw = XCreateWindow(g.xdisplay, root, x, y, width, height, 0,
        CopyFromParent, InputOutput, CopyFromParent,
        CWBorderPixel | CWEventMask, &wattr);

    XSetWMProtocols(g.xdisplay, xw, &g.wm_delete_window, 1);
    XStoreName(g.xdisplay, xw, title);
    XMapWindow(g.xdisplay, xw);

    EGLSurface surf = eglCreateWindowSurface(g.egl_display, g.egl_config,
        xw, NULL);
    if (surf == EGL_NO_SURFACE) {
        XDestroyWindow(g.xdisplay, xw);
        return 0;
    }

    XIC xic = NULL;
    if (g.xim) {
        xic = XCreateIC(g.xim,
            XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
            XNClientWindow, xw, XNFocusWindow, xw, (void *)NULL);
        if (xic)
            XSetICFocus(xic);
    }

    s->id = g.next_id++;
    s->xwindow = xw;
    s->xic = xic;
    s->egl_surface = surf;
    s->width = width;
    s->height = height;
    return s->id;
}

static void
window_destroy(struct win_slot *s)
{
    if (!s || !s->id)
        return;
    /* never leave a destroyed surface current */
    eglMakeCurrent(g.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
        g.egl_context);
    if (s->xic)
        XDestroyIC(s->xic);
    if (s->egl_surface != EGL_NO_SURFACE)
        eglDestroySurface(g.egl_display, s->egl_surface);
    if (s->xwindow)
        XDestroyWindow(g.xdisplay, s->xwindow);
    memset(s, 0, sizeof(*s));
}

/* ------------------------------------------------------------------ */
/* setup / teardown                                                   */
/* ------------------------------------------------------------------ */

int
win_open(const char *title, int width, int height)
{
    setlocale(LC_ALL, "");
    memset(&g, 0, sizeof(g));
    g.next_id = 1;

    g.xdisplay = XOpenDisplay(NULL);
    if (!g.xdisplay)
        return -1;

    g.wm_delete_window = XInternAtom(g.xdisplay, "WM_DELETE_WINDOW", False);
    g.a_clipboard = XInternAtom(g.xdisplay, "CLIPBOARD", False);
    g.a_utf8      = XInternAtom(g.xdisplay, "UTF8_STRING", False);
    g.a_targets   = XInternAtom(g.xdisplay, "TARGETS", False);
    g.a_prop      = XInternAtom(g.xdisplay, "BD_CLIPBOARD", False);

    g.egl_display = eglGetDisplay(g.xdisplay);
    if (g.egl_display == EGL_NO_DISPLAY)
        goto fail;
    if (!eglInitialize(g.egl_display, NULL, NULL))
        goto fail;
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint num_config;
    if (!eglChooseConfig(g.egl_display, config_attribs, &g.egl_config, 1,
            &num_config) || num_config < 1)
        goto fail;

    g.egl_context = eglCreateContext(g.egl_display, g.egl_config,
        EGL_NO_CONTEXT, context_attribs);
    if (g.egl_context == EGL_NO_CONTEXT)
        goto fail;

    /* Optional X input method for UTF-8 / compose / IME text entry. Each
     * window gets its own input context in window_create(). */
    XSetLocaleModifiers("");
    g.xim = XOpenIM(g.xdisplay, NULL, NULL, NULL);

    if (window_create(title, width, height) == 0)
        goto fail;

    win_window_begin(1);
    eglSwapInterval(g.egl_display, 1);
    return 0;

fail:
    win_close();
    return -1;
}

void
win_close(void)
{
    if (g.egl_display != EGL_NO_DISPLAY) {
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (g.windows[i].id)
                window_destroy(&g.windows[i]);
        eglMakeCurrent(g.egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE,
            EGL_NO_CONTEXT);
        if (g.egl_context != EGL_NO_CONTEXT)
            eglDestroyContext(g.egl_display, g.egl_context);
        eglTerminate(g.egl_display);
    }
    if (g.xim)
        XCloseIM(g.xim);
    if (g.xdisplay)
        XCloseDisplay(g.xdisplay);
    memset(&g, 0, sizeof(g));
}

/* ------------------------------------------------------------------ */
/* per-window operations                                              */
/* ------------------------------------------------------------------ */

int
win_window_open(const char *title, int w, int h)
{
    if (!g.xdisplay)
        return 0;
    return window_create(title, w, h);
}

void
win_window_close(int id)
{
    window_destroy(slot_by_id(id));
}

void
win_window_begin(int id)
{
    struct win_slot *s = slot_by_id(id);
    if (s)
        eglMakeCurrent(g.egl_display, s->egl_surface, s->egl_surface,
            g.egl_context);
}

void
win_window_swap(int id)
{
    struct win_slot *s = slot_by_id(id);
    if (s)
        eglSwapBuffers(g.egl_display, s->egl_surface);
}

int
win_window_width(int id)
{
    struct win_slot *s = slot_by_id(id);
    return s ? s->width : 0;
}

int
win_window_height(int id)
{
    struct win_slot *s = slot_by_id(id);
    return s ? s->height : 0;
}

void
win_window_set_title(int id, const char *title)
{
    struct win_slot *s = slot_by_id(id);
    if (s)
        XStoreName(g.xdisplay, s->xwindow, title);
}

/* primary-window (id 1) conveniences */
int    win_width(void)  { return win_window_width(1); }
int    win_height(void) { return win_window_height(1); }
void   win_swap(void)   { win_window_swap(1); }

double
win_time(void)
{
    struct timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    return tp.tv_sec + tp.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------ */
/* event translation                                                  */
/* ------------------------------------------------------------------ */

static int
map_mods(unsigned state)
{
    int m = 0;
    if (state & ShiftMask)   m |= WIN_MOD_SHIFT;
    if (state & ControlMask) m |= WIN_MOD_CTRL;
    if (state & Mod1Mask)    m |= WIN_MOD_ALT;
    return m;
}

static int
map_key(KeySym sym)
{
    if (sym >= XK_a && sym <= XK_z)
        return WIN_KEY_A + (int)(sym - XK_a);
    if (sym >= XK_A && sym <= XK_Z)
        return WIN_KEY_A + (int)(sym - XK_A);
    switch (sym) {
    case XK_Left:      return WIN_KEY_LEFT;
    case XK_Right:     return WIN_KEY_RIGHT;
    case XK_Up:        return WIN_KEY_UP;
    case XK_Down:      return WIN_KEY_DOWN;
    case XK_Home:      return WIN_KEY_HOME;
    case XK_End:       return WIN_KEY_END;
    case XK_BackSpace: return WIN_KEY_BACKSPACE;
    case XK_Delete:    return WIN_KEY_DELETE;
    case XK_Return:    return WIN_KEY_ENTER;
    case XK_Escape:    return WIN_KEY_ESCAPE;
    case XK_Tab:       return WIN_KEY_TAB;
    case XK_F2:        return WIN_KEY_F2;
    default:           return WIN_KEY_UNKNOWN;
    }
}

/* Decode the first UTF-8 codepoint of a byte buffer; returns the raw lead byte
 * on malformed input. */
static unsigned
utf8_first(const char *s, int len)
{
    if (len <= 0)
        return 0;
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80)
        return c;
    int n = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    if (n == 0 || n + 1 > len)
        return c;
    unsigned cp = c & (0x3F >> n);
    for (int i = 1; i <= n; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            return c;
        cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
    }
    return cp;
}

/* Translate one XEvent into *ev. Returns 1 if it produced an event, else 0. */
static int
translate(XEvent *xev, win_event *ev)
{
    struct win_slot *s = slot_by_xwindow(xev->xany.window);
    if (!s)
        return 0;

    memset(ev, 0, sizeof(*ev));
    ev->window = s->id;

    switch (xev->type) {
    case ConfigureNotify:
        if (xev->xconfigure.width == s->width
            && xev->xconfigure.height == s->height)
            return 0;
        s->width = xev->xconfigure.width;
        s->height = xev->xconfigure.height;
        ev->type = WIN_EV_RESIZE;
        ev->width = s->width;
        ev->height = s->height;
        return 1;

    case ClientMessage:
        if ((Atom)xev->xclient.data.l[0] == g.wm_delete_window) {
            ev->type = WIN_EV_CLOSE;
            return 1;
        }
        return 0;

    case MotionNotify:
        ev->type = WIN_EV_MOUSE_MOVE;
        ev->mods = map_mods(xev->xmotion.state);
        ev->x = xev->xmotion.x;
        ev->y = xev->xmotion.y;
        return 1;

    case ButtonPress:
    case ButtonRelease: {
        XButtonEvent *b = &xev->xbutton;
        ev->mods = map_mods(b->state);
        ev->x = b->x;
        ev->y = b->y;
        switch (b->button) {
        case Button1: ev->button = WIN_MOUSE_LEFT;   break;
        case Button2: ev->button = WIN_MOUSE_MIDDLE; break;
        case Button3: ev->button = WIN_MOUSE_RIGHT;  break;
        case Button4: /* wheel up */
        case Button5: /* wheel down */
            if (xev->type != ButtonPress)
                return 0;
            ev->type = WIN_EV_MOUSE_SCROLL;
            ev->scroll_dy = (b->button == Button4) ? 1.0f : -1.0f;
            return 1;
        default:
            return 0;
        }
        ev->type = (xev->type == ButtonPress) ? WIN_EV_MOUSE_DOWN
            : WIN_EV_MOUSE_UP;
        return 1;
    }

    case KeyRelease: {
        KeySym sym = XLookupKeysym(&xev->xkey, 0);
        ev->type = WIN_EV_KEY_UP;
        ev->mods = map_mods(xev->xkey.state);
        ev->key = map_key(sym);
        return 1;
    }

    case KeyPress: {
        char buf[32];
        KeySym sym = NoSymbol;
        Status status = XLookupNone;
        int len, mods = map_mods(xev->xkey.state);

        if (s->xic)
            len = Xutf8LookupString(s->xic, &xev->xkey, buf, sizeof(buf) - 1,
                &sym, &status);
        else
            len = XLookupString(&xev->xkey, buf, sizeof(buf) - 1, &sym, NULL);
        buf[len < 0 ? 0 : len] = '\0';

        int got_text = s->xic
            ? (status == XLookupChars || status == XLookupBoth)
            : (len > 0);

        if (got_text && (unsigned char)buf[0] >= 0x20
            && !(mods & (WIN_MOD_CTRL | WIN_MOD_ALT))) {
            ev->type = WIN_EV_CHAR;
            ev->mods = mods;
            ev->key = map_key(sym);
            ev->codepoint = utf8_first(buf, len);
            return 1;
        }
        ev->type = WIN_EV_KEY_DOWN;
        ev->mods = mods;
        ev->key = map_key(sym);
        return 1;
    }

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* clipboard (X11 CLIPBOARD selection)                                */
/* ------------------------------------------------------------------ */

static Window
clip_owner_window(void)
{
    struct win_slot *s = slot_by_id(1);
    return s ? s->xwindow : 0;
}

/* serve a SelectionRequest from our owned clipboard text */
static void
clip_serve(XSelectionRequestEvent *req)
{
    XSelectionEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SelectionNotify;
    ev.display = req->display;
    ev.requestor = req->requestor;
    ev.selection = req->selection;
    ev.target = req->target;
    ev.time = req->time;
    ev.property = None;

    if (req->target == g.a_utf8 || req->target == XA_STRING) {
        XChangeProperty(g.xdisplay, req->requestor, req->property,
            req->target, 8, PropModeReplace,
            (const unsigned char *)g.clip, (int)strlen(g.clip));
        ev.property = req->property;
    } else if (req->target == g.a_targets) {
        Atom targets[] = { g.a_utf8, XA_STRING };
        XChangeProperty(g.xdisplay, req->requestor, req->property,
            XA_ATOM, 32, PropModeReplace,
            (const unsigned char *)targets, 2);
        ev.property = req->property;
    }
    XSendEvent(g.xdisplay, req->requestor, False, 0, (XEvent *)&ev);
}

void
win_clipboard_set(const char *utf8)
{
    if (!g.xdisplay)
        return;
    snprintf(g.clip, sizeof g.clip, "%s", utf8 ? utf8 : "");
    XSetSelectionOwner(g.xdisplay, g.a_clipboard, clip_owner_window(),
        CurrentTime);
}

const char *
win_clipboard_get(void)
{
    if (!g.xdisplay)
        return NULL;
    Window me = clip_owner_window();
    Window owner = XGetSelectionOwner(g.xdisplay, g.a_clipboard);
    if (owner == None)
        return NULL;
    if (owner == me)
        return g.clip;          /* we own it: return our copy directly */

    XConvertSelection(g.xdisplay, g.a_clipboard, g.a_utf8, g.a_prop, me,
        CurrentTime);
    XFlush(g.xdisplay);

    for (int i = 0; i < 200; i++) {     /* wait up to ~200ms */
        XEvent xev;
        if (XCheckTypedWindowEvent(g.xdisplay, me, SelectionNotify, &xev)) {
            if (xev.xselection.property == None)
                return NULL;
            Atom type; int fmt;
            unsigned long nitems, after;
            unsigned char *data = NULL;
            XGetWindowProperty(g.xdisplay, me, g.a_prop, 0, 65536, True,
                AnyPropertyType, &type, &fmt, &nitems, &after, &data);
            if (!data)
                return NULL;
            unsigned long n = nitems;
            if (n >= sizeof g.clip) n = sizeof g.clip - 1;
            memcpy(g.clip, data, n);
            g.clip[n] = '\0';
            XFree(data);
            return g.clip;
        }
        /* keep serving our own clipboard to other apps while we wait */
        if (XCheckTypedWindowEvent(g.xdisplay, me, SelectionRequest, &xev))
            clip_serve(&xev.xselectionrequest);
        usleep(1000);
    }
    return NULL;
}

int
win_poll(win_event *ev)
{
    while (g.xdisplay && XPending(g.xdisplay)) {
        XEvent xev;
        XNextEvent(g.xdisplay, &xev);
        if (XFilterEvent(&xev, None))
            continue;
        if (xev.type == SelectionRequest) {
            clip_serve(&xev.xselectionrequest);
            continue;
        }
        /* X11 auto-repeat is a KeyRelease immediately followed by a KeyPress
         * with the same keycode and time: collapse the pair into one repeat
         * key-down, dropping the release. */
        if (xev.type == KeyRelease && XPending(g.xdisplay)) {
            XEvent nx;
            XPeekEvent(g.xdisplay, &nx);
            if (nx.type == KeyPress && nx.xkey.keycode == xev.xkey.keycode
                && nx.xkey.time == xev.xkey.time) {
                XNextEvent(g.xdisplay, &nx);
                if (translate(&nx, ev)) {
                    ev->repeat = 1;
                    return 1;
                }
                continue;
            }
        }
        if (translate(&xev, ev))
            return 1;
    }
    return 0;
}
