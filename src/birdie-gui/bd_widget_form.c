/*
 * bd_widget_form -- simple form controls (checkbox, and later radio + spinner).
 * See bd_widget_form.h and doc/gui/dialogs.md.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_widget_form.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

#define BOX_GAP  6   /* px between the box and its label */

/* a thick line segment as a quad (for the checkmark), like bd_widget_chart's */
static void
seg(float x0, float y0, float x1, float y1, float w, uint32_t c)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = dx * dx + dy * dy;
    if (len <= 0.0f) {
        bd_draw_rect(x0 - w * 0.5f, y0 - w * 0.5f, w, w, c);
        return;
    }
    len = 1.0f / sqrtf(len);
    float nx = -dy * len * w * 0.5f, ny = dx * len * w * 0.5f;
    bd_draw_quad(x0 + nx, y0 + ny, x1 + nx, y1 + ny,
                 x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
}

/* ================================================================== */
/* checkbox                                                           */
/* ================================================================== */

struct checkbox {
    int            on;
    bd_checkbox_cb cb;
    void          *arg;
};

static int checkbox_type;

/* the square box side, from the line height, capped to the widget height */
static int
box_side(bd_id id)
{
    int h;
    bd_widget_rect(id, NULL, NULL, NULL, &h);
    int s = (int)bd_draw_line_height();
    if (s > h) s = h;
    if (s < 8) s = 8;
    return s;
}

static void
checkbox_init(bd_id id, void *state)
{
    (void)state;
    bd_set(id, BD_PREF_H_I, (int)bd_draw_line_height() + 4, BD_END);
}

static void
checkbox_toggle(bd_id id, struct checkbox *c)
{
    c->on = !c->on;
    if (c->cb)
        c->cb(id, c->arg, c->on);
}

static void
checkbox_render(bd_id id, void *state)
{
    struct checkbox *c = state;
    const bd_theme *th = bd_gui_theme();
    int x, y, w, h;
    bd_widget_rect(id, &x, &y, &w, &h);
    int s = box_side(id);
    int by = y + (h - s) / 2;

    /* the box: recessed field with a border, focus-ringed when focused */
    bd_draw_rect((float)x, (float)by, (float)s, (float)s, th->press);
    bd_draw_rect_lines((float)x, (float)by, (float)s, (float)s,
        bd_focused() == id ? th->focus : th->border);

    if (c->on) {                        /* a check mark inside the box */
        float t = s * 0.14f + 1.0f;     /* stroke thickness */
        float lx = x + s * 0.22f, mx = x + s * 0.42f, rx = x + s * 0.80f;
        float my = by + s * 0.70f, ly = by + s * 0.50f, ry = by + s * 0.26f;
        seg(lx, ly, mx, my, t, th->text_hi);
        seg(mx, my, rx, ry, t, th->text_hi);
    }

    const char *label = bd_get_s(id, BD_LABEL_S);
    if (label && label[0]) {
        uint32_t fg = (uint32_t)bd_get_i(id, BD_FG_C);
        float ty = y + (h - bd_draw_line_height()) * 0.5f;
        bd_draw_text(label, (float)(x + s + BOX_GAP), ty, fg ? fg : th->text);
    }
}

static int
checkbox_event(bd_id id, void *state, const bd_event *ev)
{
    struct checkbox *c = state;
    if (ev->type == BD_EV_MOUSE_DOWN && ev->button == BD_MOUSE_LEFT) {
        checkbox_toggle(id, c);
        return 1;
    }
    if (ev->type == BD_EV_CHAR && ev->codepoint == ' ') {  /* Space toggles */
        checkbox_toggle(id, c);
        return 1;
    }
    return 0;
}

static const bd_widget_class checkbox_class = {
    .name = "checkbox",
    .state_size = sizeof(struct checkbox),
    .init = checkbox_init,
    .render = checkbox_render,
    .event = checkbox_event,
};

bd_id
bd_checkbox_create(bd_id parent, const bd_checkbox_desc *desc, ...)
{
    if (checkbox_type == 0)
        checkbox_type = bd_register_widget_class(&checkbox_class);

    va_list ap;
    va_start(ap, desc);
    bd_id id = bd_create_va(parent, checkbox_type, ap);
    va_end(ap);

    struct checkbox *c = bd_widget_state(id);
    if (c && desc) {
        c->on = desc->checked ? 1 : 0;
        c->cb = desc->cb;
        c->arg = desc->arg;
        if (desc->label)
            bd_set(id, BD_LABEL_S, desc->label, BD_END);
    }
    return id;
}

void
bd_checkbox_set(bd_id id, int checked)
{
    if (bd_widget_type(id) != checkbox_type)
        return;
    ((struct checkbox *)bd_widget_state(id))->on = checked ? 1 : 0;
}

int
bd_checkbox_get(bd_id id)
{
    if (bd_widget_type(id) != checkbox_type)
        return 0;
    return ((struct checkbox *)bd_widget_state(id))->on;
}

/* ================================================================== */
/* radio group                                                        */
/* ================================================================== */

#define DISC_SEGS 20   /* triangle-fan segments approximating the bullet */

struct radio {
    const char **labels;   /* owned array; the strings inside are borrowed */
    int          count;
    int          sel;
    int          orient;   /* BD_VERTICAL / BD_HORIZONTAL */
    bd_radio_cb  cb;
    void        *arg;
};

static int radio_type;

/* a filled disc as a fan of triangles (no circle primitive in bd_draw) */
static void
disc(float cx, float cy, float r, uint32_t c)
{
    float px = cx + r, py = cy;
    for (int i = 1; i <= DISC_SEGS; i++) {
        float a = (float)i / DISC_SEGS * 6.28318530718f;
        float nx = cx + r * cosf(a), ny = cy + r * sinf(a);
        bd_draw_quad(cx, cy, px, py, nx, ny, nx, ny, c);  /* tri as quad */
        px = nx; py = ny;
    }
}

/* one option's row height, and the bullet diameter within it */
static int radio_rh(void)   { return (int)bd_draw_line_height() + 4; }
static int radio_bull(void) { int d = (int)bd_draw_line_height() - 2; return d < 8 ? 8 : d; }

/* width an option occupies in a horizontal group (bullet + gap + label) */
static int
radio_opt_w(const char *label)
{
    int w = radio_bull() + BOX_GAP;
    if (label && label[0])
        w += (int)bd_draw_text_width(label);
    return w + 12;   /* trailing spacing between options */
}

/* preferred size from the options, set once the state is populated */
static void
radio_size(bd_id id, struct radio *r)
{
    int rh = radio_rh();
    if (r->orient == BD_HORIZONTAL) {
        int w = 0;
        for (int i = 0; i < r->count; i++)
            w += radio_opt_w(r->labels[i]);
        bd_set(id, BD_PREF_H_I, rh, BD_PREF_W_I, w, BD_END);
    } else {
        int w = radio_bull() + BOX_GAP;
        for (int i = 0; i < r->count; i++) {
            int lw = r->labels[i] ? (int)bd_draw_text_width(r->labels[i]) : 0;
            if (lw > w - radio_bull() - BOX_GAP)
                w = radio_bull() + BOX_GAP + lw;
        }
        bd_set(id, BD_PREF_H_I, rh * (r->count > 0 ? r->count : 1),
               BD_PREF_W_I, w, BD_END);
    }
}

static void
radio_destroy(bd_id id, void *state)
{
    (void)id;
    struct radio *r = state;
    free(r->labels);
}

static void
radio_select(bd_id id, struct radio *r, int idx)
{
    if (idx < 0 || idx >= r->count || idx == r->sel)
        return;
    r->sel = idx;
    if (r->cb)
        r->cb(id, r->arg, idx);
}

static void
radio_render(bd_id id, void *state)
{
    struct radio *r = state;
    const bd_theme *th = bd_gui_theme();
    int x, y, w, h;
    bd_widget_rect(id, &x, &y, &w, &h);
    (void)w; (void)h;
    int rh = radio_rh(), d = radio_bull();
    int horiz = (r->orient == BD_HORIZONTAL);
    float lh = bd_draw_line_height();
    int ox = x;

    for (int i = 0; i < r->count; i++) {
        int oy = horiz ? y : y + i * rh;
        float cx = ox + d * 0.5f, cy = oy + rh * 0.5f;

        /* recessed bullet: a border ring around a sunken field */
        disc(cx, cy, d * 0.5f, bd_focused() == id && i == r->sel
                                 ? th->focus : th->border);
        disc(cx, cy, d * 0.5f - 1.5f, th->press);
        if (i == r->sel)                          /* filled center dot */
            disc(cx, cy, d * 0.28f, th->text_hi);

        const char *label = r->labels[i];
        if (label && label[0]) {
            uint32_t fg = (uint32_t)bd_get_i(id, BD_FG_C);
            bd_draw_text(label, (float)(ox + d + BOX_GAP),
                         oy + (rh - lh) * 0.5f, fg ? fg : th->text);
        }
        ox = horiz ? ox + radio_opt_w(label) : x;
    }
}

/* which option a point falls on, or -1 */
static int
radio_hit(struct radio *r, int x, int y, int px, int py)
{
    int rh = radio_rh();
    if (r->orient == BD_HORIZONTAL) {
        if (py < y || py >= y + rh)
            return -1;
        int ox = x;
        for (int i = 0; i < r->count; i++) {
            int nx = ox + radio_opt_w(r->labels[i]);
            if (px >= ox && px < nx)
                return i;
            ox = nx;
        }
        return -1;
    }
    if (py < y || py >= y + rh * r->count)
        return -1;
    return (py - y) / rh;
}

static int
radio_event(bd_id id, void *state, const bd_event *ev)
{
    struct radio *r = state;
    if (ev->type == BD_EV_MOUSE_DOWN && ev->button == BD_MOUSE_LEFT) {
        int x, y, w, h;
        bd_widget_rect(id, &x, &y, &w, &h);
        int idx = radio_hit(r, x, y, ev->x, ev->y);
        if (idx >= 0)
            radio_select(id, r, idx);
        return 1;
    }
    if (ev->type == BD_EV_KEY_DOWN) {
        int sel = r->sel < 0 ? 0 : r->sel;
        if (ev->key == BD_KEY_UP || ev->key == BD_KEY_LEFT) {
            radio_select(id, r, sel > 0 ? sel - 1 : sel);
            return 1;
        }
        if (ev->key == BD_KEY_DOWN || ev->key == BD_KEY_RIGHT) {
            radio_select(id, r, sel < r->count - 1 ? sel + 1 : sel);
            return 1;
        }
    }
    return 0;
}

static const bd_widget_class radio_class = {
    .name = "radio",
    .state_size = sizeof(struct radio),
    .destroy = radio_destroy,
    .render = radio_render,
    .event = radio_event,
};

bd_id
bd_radio_create(bd_id parent, const bd_radio_desc *desc, ...)
{
    if (radio_type == 0)
        radio_type = bd_register_widget_class(&radio_class);

    va_list ap;
    va_start(ap, desc);
    bd_id id = bd_create_va(parent, radio_type, ap);
    va_end(ap);

    struct radio *r = bd_widget_state(id);
    if (r && desc && desc->count > 0 && desc->labels) {
        r->labels = malloc((size_t)desc->count * sizeof(*r->labels));
        if (r->labels) {
            r->count = desc->count;
            for (int i = 0; i < desc->count; i++)
                r->labels[i] = desc->labels[i];
        }
        r->orient = desc->orient == BD_VERTICAL ? BD_VERTICAL : BD_HORIZONTAL;
        r->sel = (desc->selected >= 0 && desc->selected < r->count)
                   ? desc->selected : -1;
        r->cb = desc->cb;
        r->arg = desc->arg;
        radio_size(id, r);
    }
    return id;
}

void
bd_radio_set(bd_id id, int index)
{
    if (bd_widget_type(id) != radio_type)
        return;
    struct radio *r = bd_widget_state(id);
    if (index >= -1 && index < r->count)
        r->sel = index;
}

int
bd_radio_get(bd_id id)
{
    if (bd_widget_type(id) != radio_type)
        return -1;
    return ((struct radio *)bd_widget_state(id))->sel;
}
