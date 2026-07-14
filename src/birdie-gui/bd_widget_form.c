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
