/*
 * bd_popmenu -- a transient popup / context menu floated through the shared
 * overlay primitive (bd_overlay_*). See bd_popmenu.h.
 *
 * The menu is owned by a single reusable, invisible orphan widget (parent
 * BD_NONE, so register_window never treats it as a window and the render walk
 * never reaches it): it exists only to carry the current menu state and to be
 * the overlay's owner. The overlay's render/event hooks draw the box and route
 * input; the owner widget has no hooks of its own.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_popmenu.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include <stdlib.h>
#include <string.h>

#define PADX     14   /* text inset on each side */
#define MIN_W    96   /* minimum menu width */
#define ROW_PAD   6   /* extra height added to a text row */
#define SEP_H     7   /* separator row height */

struct popmenu {
    bd_popmenu_item *items;        /* owned copy of the item array */
    int              count;
    int              hi;           /* highlighted row, -1 for none */
    int              x, y, w, h;   /* menu rect, fixed at open time */
};

static int   popmenu_type;
static bd_id popmenu_owner = BD_NONE;

static int
row_h(void)
{
    return (int)bd_draw_line_height() + ROW_PAD;
}

static int
item_h(const bd_popmenu_item *it)
{
    return (it->flags & BD_POPMENU_SEPARATOR) ? SEP_H : row_h();
}

static int
selectable(const bd_popmenu_item *it)
{
    return !(it->flags & (BD_POPMENU_SEPARATOR | BD_POPMENU_DISABLED));
}

/* which row a screen-y falls on, or -1 (also -1 over a separator/disabled) */
static int
row_at(struct popmenu *m, int py)
{
    int ry = m->y + 1;
    for (int i = 0; i < m->count; i++) {
        int h = item_h(&m->items[i]);
        if (py >= ry && py < ry + h)
            return selectable(&m->items[i]) ? i : -1;
        ry += h;
    }
    return -1;
}

static void
choose(bd_id id, struct popmenu *m, int idx)
{
    /* idx is a valid, selectable row (row_at filtered the rest) */
    void (*action)(void *) = m->items[idx].action;
    void *user = m->items[idx].user;
    /* close before running the action, so the action may open another menu */
    bd_overlay_close(id);
    if (action)
        action(user);
}

/* move the highlight to the next selectable row in `dir` (+1 down, -1 up) */
static void
move_hi(struct popmenu *m, int dir)
{
    if (m->count == 0)
        return;
    int i = m->hi;
    for (int n = 0; n < m->count; n++) {
        i += dir;
        if (i < 0)
            i = m->count - 1;
        else if (i >= m->count)
            i = 0;
        if (selectable(&m->items[i])) {
            m->hi = i;
            return;
        }
    }
}

static void
popmenu_overlay_render(bd_id id)
{
    struct popmenu *m = bd_widget_state(id);
    if (!m)
        return;
    const bd_theme *th = bd_gui_theme();
    bd_draw_rect((float)m->x, (float)m->y, (float)m->w, (float)m->h, th->panel);
    bd_draw_rect_lines((float)m->x, (float)m->y, (float)m->w, (float)m->h,
        th->border);
    float lh = bd_draw_line_height();
    int ry = m->y + 1;
    for (int i = 0; i < m->count; i++) {
        const bd_popmenu_item *it = &m->items[i];
        int h = item_h(it);
        if (it->flags & BD_POPMENU_SEPARATOR) {
            bd_draw_rect((float)(m->x + PADX / 2), (float)(ry + h / 2),
                (float)(m->w - PADX), 1.0f, th->border);
        } else {
            if (i == m->hi)
                bd_draw_rect((float)(m->x + 1), (float)ry,
                    (float)(m->w - 2), (float)h, th->select);
            uint32_t col;
            if (it->flags & BD_POPMENU_DISABLED)
                col = (th->text & 0xFFFFFF00u) | 0x70u;  /* translucent = dimmed */
            else
                col = (i == m->hi) ? th->text_hi : th->text;
            if (it->label && it->label[0])
                bd_draw_text(it->label, (float)(m->x + PADX),
                    ry + (h - lh) * 0.5f, col);
        }
        ry += h;
    }
}

static int
popmenu_overlay_event(bd_id id, const bd_event *ev)
{
    struct popmenu *m = bd_widget_state(id);
    if (!m)
        return 0;
    switch (ev->type) {
    case BD_EV_MOUSE_MOVE:
        m->hi = row_at(m, ev->y);
        return 1;
    case BD_EV_MOUSE_DOWN: {
        int r = row_at(m, ev->y);
        if (r >= 0)
            choose(id, m, r);
        return 1;   /* a click on a non-selectable row keeps the menu open */
    }
    case BD_EV_KEY_DOWN:
        if (ev->key == BD_KEY_UP)    { move_hi(m, -1); return 1; }
        if (ev->key == BD_KEY_DOWN)  { move_hi(m, +1); return 1; }
        if (ev->key == BD_KEY_ENTER) {
            if (m->hi >= 0 && selectable(&m->items[m->hi]))
                choose(id, m, m->hi);
            return 1;
        }
        return 0;   /* Escape (and the rest) falls through: the core closes */
    default:
        return 0;
    }
}

static void
popmenu_init(bd_id id, void *state)
{
    (void)state;
    bd_set(id, BD_VISIBLE_B, 0, BD_END);   /* the owner never draws itself */
}

static void
popmenu_destroy(bd_id id, void *state)
{
    (void)id;
    free(((struct popmenu *)state)->items);
}

static const bd_widget_class popmenu_class = {
    .name = "popmenu",
    .state_size = sizeof(struct popmenu),
    .init = popmenu_init,
    .destroy = popmenu_destroy,
    /* no render/event hooks: the overlay drives both */
};

/* The single reusable owner. Revalidated by type so it survives a gui
 * init/cleanup cycle: the pool is wiped (the old id goes stale) but the
 * registered class id persists, so a type mismatch re-creates the orphan. */
static bd_id
ensure_owner(void)
{
    if (popmenu_type == 0)
        popmenu_type = bd_register_widget_class(&popmenu_class);
    if (popmenu_owner == BD_NONE ||
        bd_widget_type(popmenu_owner) != popmenu_type)
        popmenu_owner = bd_create(BD_NONE, popmenu_type, BD_END);
    return popmenu_owner;
}

void
bd_popmenu_open(int x, int y, const bd_popmenu_item *items, int count)
{
    if (count <= 0 || !items)
        return;
    bd_id id = ensure_owner();
    struct popmenu *m = bd_widget_state(id);
    if (!m)
        return;

    /* copy the item array (labels and user pointers stay borrowed) */
    free(m->items);
    m->items = malloc((size_t)count * sizeof(*m->items));
    if (!m->items) {
        m->count = 0;
        return;
    }
    memcpy(m->items, items, (size_t)count * sizeof(*m->items));
    m->count = count;

    /* geometry: width from the widest label, height from the rows */
    int w = MIN_W, h = 2;
    for (int i = 0; i < count; i++) {
        const bd_popmenu_item *it = &m->items[i];
        if (!(it->flags & BD_POPMENU_SEPARATOR) && it->label) {
            int tw = (int)bd_draw_text_width(it->label) + 2 * PADX;
            if (tw > w)
                w = tw;
        }
        h += item_h(it);
    }

    /* clamp the box on screen */
    int W = bd_draw_win_w(), H = bd_draw_win_h();
    if (x + w > W)
        x = W - w;
    if (x < 0)
        x = 0;
    if (y + h > H)
        y = H - h;
    if (y < 0)
        y = 0;
    m->x = x;
    m->y = y;
    m->w = w;
    m->h = h;

    /* highlight the first selectable row */
    m->hi = -1;
    for (int i = 0; i < count; i++)
        if (selectable(&m->items[i])) {
            m->hi = i;
            break;
        }

    bd_overlay_open(&(bd_overlay){ .owner = id, .x = x, .y = y, .w = w, .h = h,
        .render = popmenu_overlay_render, .event = popmenu_overlay_event });
}

void
bd_popmenu_close(void)
{
    if (popmenu_owner != BD_NONE && bd_overlay_owner() == popmenu_owner)
        bd_overlay_close(popmenu_owner);
}

int
bd_popmenu_is_open(void)
{
    return popmenu_owner != BD_NONE && bd_overlay_owner() == popmenu_owner;
}
