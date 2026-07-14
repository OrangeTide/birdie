/*
 * bd_widget_combo -- a drop-down selector (BD_COMBO).
 *
 * The closed control is a labeled box with a chevron; clicking it opens a
 * floating list through the shared overlay primitive (bd_overlay_*), which
 * draws top-most and feeds input back through the two hooks below. See
 * bd_widget_combo.h and doc/gui/dialogs.md.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_widget_combo.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include <stdarg.h>
#include <stdlib.h>

#define PADX     6    /* left text inset / list row inset */
#define CHEVW    16   /* chevron column width on the right of the closed box */

struct combo {
    const char **items;      /* owned array; the strings inside are borrowed */
    int          count;
    int          sel;        /* selected index, -1 for none */
    int          hi;         /* highlighted row while the list is open */
    const char  *placeholder;
    bd_combo_cb  cb;
    void        *arg;
    /* geometry of the open list, fixed at open time so render and hit agree */
    int          ox, oy, ow, oh, rh;
};

static int combo_type;

static int
combo_rowh(void)
{
    return (int)bd_draw_line_height() + 4;
}

/* -------- the open list, driven through the overlay hooks -------- */

/* which list row a point falls on, or -1 */
static int
list_row(struct combo *c, int py)
{
    if (py < c->oy + 1 || py >= c->oy + c->oh - 1)
        return -1;
    int r = (py - c->oy - 1) / c->rh;
    return (r >= 0 && r < c->count) ? r : -1;
}

static void
combo_overlay_render(bd_id id)
{
    struct combo *c = bd_widget_state(id);
    if (!c)
        return;
    const bd_theme *th = bd_gui_theme();
    bd_draw_rect((float)c->ox, (float)c->oy, (float)c->ow, (float)c->oh, th->panel);
    bd_draw_rect_lines((float)c->ox, (float)c->oy, (float)c->ow, (float)c->oh,
        th->border);
    float lh = bd_draw_line_height();
    for (int i = 0; i < c->count; i++) {
        int ry = c->oy + 1 + i * c->rh;
        if (i == c->hi)
            bd_draw_rect((float)(c->ox + 1), (float)ry,
                (float)(c->ow - 2), (float)c->rh, th->select);
        const char *s = c->items[i];
        if (s && s[0])
            bd_draw_text(s, (float)(c->ox + PADX), ry + (c->rh - lh) * 0.5f,
                i == c->hi ? th->text_hi : th->text);
    }
}

static void
combo_choose(bd_id id, struct combo *c, int idx)
{
    if (idx >= 0 && idx < c->count && idx != c->sel) {
        c->sel = idx;
        if (c->cb)
            c->cb(id, c->arg, idx);
    }
    bd_overlay_close(id);
}

static int
combo_overlay_event(bd_id id, const bd_event *ev)
{
    struct combo *c = bd_widget_state(id);
    if (!c)
        return 0;
    switch (ev->type) {
    case BD_EV_MOUSE_MOVE: {
        int r = list_row(c, ev->y);
        if (r >= 0)
            c->hi = r;
        return 1;
    }
    case BD_EV_MOUSE_DOWN: {
        int r = list_row(c, ev->y);
        if (r >= 0)
            combo_choose(id, c, r);
        else
            bd_overlay_close(id);
        return 1;
    }
    case BD_EV_KEY_DOWN:
        if (ev->key == BD_KEY_UP) {
            if (c->hi > 0) c->hi--;
            return 1;
        }
        if (ev->key == BD_KEY_DOWN) {
            if (c->hi < c->count - 1) c->hi++;
            return 1;
        }
        if (ev->key == BD_KEY_ENTER) {
            combo_choose(id, c, c->hi);
            return 1;
        }
        return 0;   /* Escape (and the rest) falls through: the core closes */
    default:
        return 0;
    }
}

/* -------- the closed control -------- */

static void
combo_open(bd_id id, struct combo *c)
{
    int x, y, w, h;
    bd_widget_rect(id, &x, &y, &w, &h);
    c->rh = combo_rowh();
    c->ow = w;
    c->oh = c->count * c->rh + 2;
    c->ox = x;
    /* drop below; flip above if it would fall off the bottom */
    c->oy = y + h;
    if (c->oy + c->oh > bd_draw_win_h() && y - c->oh >= 0)
        c->oy = y - c->oh;
    c->hi = c->sel >= 0 ? c->sel : 0;
    bd_overlay_open(&(bd_overlay){ .owner = id,
        .x = c->ox, .y = c->oy, .w = c->ow, .h = c->oh,
        .render = combo_overlay_render, .event = combo_overlay_event });
}

static void
combo_init(bd_id id, void *state)
{
    (void)state;
    bd_set(id, BD_PREF_H_I, (int)bd_draw_line_height() + 8, BD_END);
}

static void
combo_render(bd_id id, void *state)
{
    struct combo *c = state;
    const bd_theme *th = bd_gui_theme();
    int x, y, w, h;
    bd_widget_rect(id, &x, &y, &w, &h);
    int open = bd_overlay_owner() == id;

    /* the field: a raised widget face with a border, focus-ringed */
    bd_draw_rect((float)x, (float)y, (float)w, (float)h, th->widget);
    bd_draw_rect_lines((float)x, (float)y, (float)w, (float)h,
        (bd_focused() == id || open) ? th->focus : th->border);

    /* current selection, or the placeholder in the dim text tone */
    const char *label = (c->sel >= 0 && c->sel < c->count)
                          ? c->items[c->sel] : c->placeholder;
    float ty = y + (h - bd_draw_line_height()) * 0.5f;
    if (label && label[0])
        bd_draw_text(label, (float)(x + PADX), ty,
            c->sel >= 0 ? th->text_hi : th->text);

    /* chevron in its column on the right */
    float cx = x + w - CHEVW * 0.5f, cy = y + h * 0.5f;
    bd_draw_quad(cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3, cx, cy + 3,
        th->text);
}

static int
combo_event(bd_id id, void *state, const bd_event *ev)
{
    struct combo *c = state;
    if (ev->type == BD_EV_MOUSE_DOWN && ev->button == BD_MOUSE_LEFT) {
        if (bd_overlay_owner() == id)
            bd_overlay_close(id);        /* click the closed box: toggle shut */
        else if (c->count > 0)
            combo_open(id, c);
        return 1;
    }
    /* keyboard open: Space or Enter (Space arrives as a CHAR like the button) */
    if (ev->type == BD_EV_CHAR && ev->codepoint == ' ' && c->count > 0) {
        combo_open(id, c);
        return 1;
    }
    if (ev->type == BD_EV_KEY_DOWN && ev->key == BD_KEY_ENTER && c->count > 0) {
        combo_open(id, c);
        return 1;
    }
    return 0;
}

static void
combo_destroy(bd_id id, void *state)
{
    (void)id;
    free(((struct combo *)state)->items);
}

static const bd_widget_class combo_class = {
    .name = "combo",
    .state_size = sizeof(struct combo),
    .init = combo_init,
    .destroy = combo_destroy,
    .render = combo_render,
    .event = combo_event,
};

bd_id
bd_combo_create(bd_id parent, const bd_combo_desc *desc, ...)
{
    if (combo_type == 0)
        combo_type = bd_register_widget_class(&combo_class);

    va_list ap;
    va_start(ap, desc);
    bd_id id = bd_create_va(parent, combo_type, ap);
    va_end(ap);

    struct combo *c = bd_widget_state(id);
    if (c && desc) {
        if (desc->count > 0 && desc->items) {
            c->items = malloc((size_t)desc->count * sizeof(*c->items));
            if (c->items) {
                c->count = desc->count;
                for (int i = 0; i < desc->count; i++)
                    c->items[i] = desc->items[i];
            }
        }
        c->sel = (desc->selected >= 0 && desc->selected < c->count)
                   ? desc->selected : -1;
        c->placeholder = desc->placeholder;
        c->cb = desc->cb;
        c->arg = desc->arg;
    }
    return id;
}

void
bd_combo_set(bd_id id, int index)
{
    if (bd_widget_type(id) != combo_type)
        return;
    struct combo *c = bd_widget_state(id);
    if (index < -1)
        index = -1;
    if (index >= c->count)
        index = c->count - 1;
    c->sel = index;
}

int
bd_combo_get(bd_id id)
{
    if (bd_widget_type(id) != combo_type)
        return -1;
    return ((struct combo *)bd_widget_state(id))->sel;
}
