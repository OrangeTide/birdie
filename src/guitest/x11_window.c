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

#include <ctype.h>
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
#include <X11/extensions/XInput2.h>

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

/* A stylus device learned from XInput2: which valuators carry pressure/tilt
 * and their ranges, plus the live tip/barrel/eraser state. Cached by device
 * id so each XI event need not re-query the server. */
#define MAX_PENDEV 8
struct pen_dev {
    int    id;          /* XInput2 source device id, 0 = empty slot */
    int    is_pen;      /* has a pressure valuator */
    int    eraser;      /* this device is the eraser end */
    int    v_press;     /* valuator number for pressure, -1 = none */
    int    v_tiltx, v_tilty;
    double pmin, pmax;  /* pressure valuator range */
    int    down;        /* tip currently in contact */
    int    barrel;      /* a barrel (side) button is held */
};

/* Display + GL context shared by every window. */
static struct {
    Display   *xdisplay;
    Atom       wm_delete_window;
    Atom       a_clipboard, a_utf8, a_targets, a_prop;
    Atom       a_press, a_tiltx, a_tilty;   /* XInput2 valuator label atoms */
    XIM        xim;
    EGLDisplay egl_display;
    EGLConfig  egl_config;
    EGLContext egl_context;
    struct win_slot windows[MAX_WINDOWS];   /* slot 0 is window id 1 */
    int        next_id;
    char       clip[4096];                  /* our owned clipboard text */
    int        xi_opcode;                   /* XInput2 major opcode, 0 = none */
    struct pen_dev pendev[MAX_PENDEV];
} g;

/* Case-insensitive substring test (avoids the GNU strcasestr). */
static int
name_has(const char *hay, const char *needle)
{
    if (!hay)
        return 0;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*n && tolower((unsigned char)*h) == tolower((unsigned char)*n))
            h++, n++;
        if (!*n)
            return 1;
    }
    return 0;
}

/* Find (or query and cache) the stylus record for an XInput2 source device. */
static struct pen_dev *
pen_dev_get(int sourceid)
{
    struct pen_dev *p = NULL;
    for (int i = 0; i < MAX_PENDEV; i++) {
        if (g.pendev[i].id == sourceid)
            return &g.pendev[i];
        if (!p && g.pendev[i].id == 0)
            p = &g.pendev[i];
    }
    if (!p)
        p = &g.pendev[0];               /* evict slot 0 if the cache is full */

    memset(p, 0, sizeof *p);
    p->id = sourceid;
    p->v_press = p->v_tiltx = p->v_tilty = -1;

    int ndev = 0;
    XIDeviceInfo *info = XIQueryDevice(g.xdisplay, sourceid, &ndev);
    if (info && ndev > 0) {
        if (name_has(info->name, "eras"))
            p->eraser = 1;
        for (int c = 0; c < info->num_classes; c++) {
            if (info->classes[c]->type != XIValuatorClass)
                continue;
            XIValuatorClassInfo *v =
                (XIValuatorClassInfo *)info->classes[c];
            if (v->label == g.a_press) {
                p->is_pen = 1;
                p->v_press = v->number;
                p->pmin = v->min;
                p->pmax = v->max;
            } else if (v->label == g.a_tiltx) {
                p->v_tiltx = v->number;
            } else if (v->label == g.a_tilty) {
                p->v_tilty = v->number;
            }
        }
    }
    if (info)
        XIFreeDeviceInfo(info);
    return p;
}

/* Read valuator `number` out of an XI device event; returns 0 if absent. */
static int
xi_valuator(XIDeviceEvent *de, int number, double *out)
{
    const double *val = de->valuators.values;
    for (int i = 0; i < de->valuators.mask_len * 8; i++) {
        if (!XIMaskIsSet(de->valuators.mask, i))
            continue;
        if (i == number) {
            *out = *val;
            return 1;
        }
        val++;
    }
    return 0;
}

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

    if (g.xi_opcode) {          /* multitouch + pen/tablet events */
        unsigned char mask[XIMaskLen(XI_LASTEVENT)] = { 0 };
        XISetMask(mask, XI_TouchBegin);
        XISetMask(mask, XI_TouchUpdate);
        XISetMask(mask, XI_TouchEnd);
        /* Stylus arrives as XI motion/buttons carrying pressure valuators.
         * Selecting these on the master pointer suppresses core mouse delivery
         * to this client, so win_poll() must translate non-pen XI events into
         * WIN_EV_MOUSE_* itself; it does. */
        XISetMask(mask, XI_Motion);
        XISetMask(mask, XI_ButtonPress);
        XISetMask(mask, XI_ButtonRelease);
        XIEventMask em = { XIAllMasterDevices, sizeof mask, mask };
        XISelectEvents(g.xdisplay, xw, &em, 1);
    }

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

    /* XInput2 for multitouch + pen/tablet (optional) */
    int xi_ev, xi_err;
    if (XQueryExtension(g.xdisplay, "XInputExtension", &g.xi_opcode,
            &xi_ev, &xi_err)) {
        int major = 2, minor = 2;
        if (XIQueryVersion(g.xdisplay, &major, &minor) != Success)
            g.xi_opcode = 0;
    } else {
        g.xi_opcode = 0;
    }
    /* stylus valuator labels (libinput/wacom naming) */
    g.a_press = XInternAtom(g.xdisplay, "Abs Pressure", False);
    g.a_tiltx = XInternAtom(g.xdisplay, "Abs Tilt X", False);
    g.a_tilty = XInternAtom(g.xdisplay, "Abs Tilt Y", False);

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
            /* committed text (ASCII, IME, compose, dead keys): the whole
             * UTF-8 string, not just the first codepoint */
            ev->type = WIN_EV_TEXT_COMMIT;
            ev->mods = mods;
            ev->key = map_key(sym);
            int n = len;
            if (n >= (int)sizeof ev->text)
                n = (int)sizeof ev->text - 1;
            memcpy(ev->text, buf, (size_t)n);
            ev->text[n] = '\0';
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

/* ------------------------------------------------------------------ */
/* input method                                                       */
/* ------------------------------------------------------------------ */

void
win_ime_set_enabled(int on)
{
    struct win_slot *s = slot_by_id(1);
    if (!s || !s->xic)
        return;
    if (on)
        XSetICFocus(s->xic);
    else
        XUnsetICFocus(s->xic);
}

void
win_ime_set_cursor_rect(int x, int y, int w, int h)
{
    struct win_slot *s = slot_by_id(1);
    if (!s || !s->xic)
        return;
    (void)w;
    XPoint spot = { (short)x, (short)(y + h) };   /* baseline of the caret */
    XVaNestedList pre = XVaCreateNestedList(0, XNSpotLocation, &spot, NULL);
    if (pre) {
        XSetICValues(s->xic, XNPreeditAttributes, pre, (void *)NULL);
        XFree(pre);
    }
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
        /* XInput2 multitouch */
        if (xev.type == GenericEvent && g.xi_opcode
            && xev.xcookie.extension == g.xi_opcode
            && XGetEventData(g.xdisplay, &xev.xcookie)) {
            XIDeviceEvent *de = xev.xcookie.data;
            int t = xev.xcookie.evtype;
            struct win_slot *s = slot_by_xwindow(de->event);
            int handled = 0;
            if (s && (t == XI_TouchBegin || t == XI_TouchUpdate
                || t == XI_TouchEnd)) {
                memset(ev, 0, sizeof(*ev));
                ev->window = s->id;
                ev->x = (int)de->event_x;
                ev->y = (int)de->event_y;
                ev->touch = (int)de->detail;
                ev->type = (t == XI_TouchBegin) ? WIN_EV_TOUCH_DOWN
                    : (t == XI_TouchUpdate) ? WIN_EV_TOUCH_MOVE
                    : WIN_EV_TOUCH_UP;
                handled = 1;
            } else if (s && (t == XI_Motion || t == XI_ButtonPress
                || t == XI_ButtonRelease)) {
                /* A device with a pressure valuator is a stylus. Selecting XI2
                 * button/motion on the master pointer makes the X server stop
                 * delivering core ButtonPress/ButtonRelease/MotionNotify to
                 * this client for that device, so the XI path is now the sole
                 * source of plain mouse input too: translate non-pen events
                 * into WIN_EV_MOUSE_* here rather than dropping them. */
                struct pen_dev *p = pen_dev_get(de->sourceid);
                if (!p->is_pen) {
                    memset(ev, 0, sizeof(*ev));
                    ev->window = s->id;
                    ev->mods = map_mods(de->mods.effective);
                    ev->x = (int)de->event_x;
                    ev->y = (int)de->event_y;
                    if (t == XI_Motion) {
                        ev->type = WIN_EV_MOUSE_MOVE;
                        handled = 1;
                    } else if (de->detail == 1 || de->detail == 2
                        || de->detail == 3) {
                        ev->button = (de->detail == 1) ? WIN_MOUSE_LEFT
                            : (de->detail == 2) ? WIN_MOUSE_MIDDLE
                            : WIN_MOUSE_RIGHT;
                        ev->type = (t == XI_ButtonPress) ? WIN_EV_MOUSE_DOWN
                            : WIN_EV_MOUSE_UP;
                        handled = 1;
                    } else if (t == XI_ButtonPress
                        && (de->detail == 4 || de->detail == 5)) {
                        ev->type = WIN_EV_MOUSE_SCROLL;
                        ev->scroll_dy = (de->detail == 4) ? 1.0f : -1.0f;
                        handled = 1;
                    }
                } else {
                    double raw;
                    if (t == XI_ButtonPress && de->detail == 1)
                        p->down = 1;
                    else if (t == XI_ButtonRelease && de->detail == 1)
                        p->down = 0;
                    else if (t == XI_ButtonPress && de->detail >= 2)
                        p->barrel = 1;
                    else if (t == XI_ButtonRelease && de->detail >= 2)
                        p->barrel = 0;

                    memset(ev, 0, sizeof(*ev));
                    ev->window = s->id;
                    ev->x = (int)de->event_x;
                    ev->y = (int)de->event_y;
                    ev->pressure = 1.0f;
                    if (p->v_press >= 0 && p->pmax > p->pmin
                        && xi_valuator(de, p->v_press, &raw))
                        ev->pressure = (float)((raw - p->pmin)
                            / (p->pmax - p->pmin));
                    if (p->v_tiltx >= 0 && xi_valuator(de, p->v_tiltx, &raw))
                        ev->tilt_x = (float)raw;
                    if (p->v_tilty >= 0 && xi_valuator(de, p->v_tilty, &raw))
                        ev->tilt_y = (float)raw;
                    ev->pen_flags = WIN_PEN_INRANGE
                        | (p->barrel ? WIN_PEN_BARREL : 0)
                        | (p->eraser ? WIN_PEN_ERASER : 0);
                    if (t == XI_ButtonPress && de->detail == 1)
                        ev->type = WIN_EV_PEN_DOWN;
                    else if (t == XI_ButtonRelease && de->detail == 1)
                        ev->type = WIN_EV_PEN_UP;
                    else
                        ev->type = p->down ? WIN_EV_PEN_MOVE
                            : WIN_EV_PEN_HOVER;
                    handled = 1;
                }
            }
            XFreeEventData(g.xdisplay, &xev.xcookie);
            if (handled)
                return 1;
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
