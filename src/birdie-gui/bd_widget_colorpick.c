/*
 * bd_widget_colorpick -- an HSV colour picker (BD_COLORPICK).
 *
 * A saturation/value square, a hue bar, a live swatch, and a preset row, drawn
 * as flat-cell gradients (no shader, so it is backend-neutral) in one leaf
 * extension widget. See bd_widget_colorpick.h and doc/gui/dialogs.md.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_widget_colorpick.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include <stdarg.h>
#include <math.h>

#define HUE_W     20   /* hue bar width */
#define SWATCH_W  28   /* swatch width */
#define PRESET_H  22   /* preset row height */
#define GAP        6
#define SVN       24   /* saturation/value grid resolution */
#define HUEN      24   /* hue bar strip count */

enum { DRAG_NONE, DRAG_SV, DRAG_HUE };

struct colorpick {
    float           h, s, v;   /* 0..1 */
    uint8_t         a;         /* alpha, preserved through set/get */
    int             drag;
    bd_colorpick_cb cb;
    void           *arg;
};

static int colorpick_type;

static const uint32_t presets[] = {
    0x000000FFu, 0xFFFFFFFFu, 0x808080FFu, 0xFF0000FFu,
    0x00FF00FFu, 0x0000FFFFu, 0xFFFF00FFu, 0xFF00FFFFu,
    0x00FFFFFFu, 0xFF8000FFu, 0x8000FFFFu, 0x008000FFu,
};
#define N_PRESETS ((int)(sizeof presets / sizeof presets[0]))

/* ---- colour maths ---- */

static float
clamp01(float x)
{
    return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
}

/* hsv (each 0..1) -> packed 0xRRGGBBAA with alpha `a` */
static uint32_t
hsv_rgba(float h, float s, float v, uint8_t a)
{
    float r = v, g = v, b = v;
    if (s > 0.0f) {
        float hh = (h - floorf(h)) * 6.0f;
        int   i = (int)hh;
        float f = hh - (float)i;
        float p = v * (1.0f - s);
        float q = v * (1.0f - s * f);
        float t = v * (1.0f - s * (1.0f - f));
        switch (i) {
        case 0:  r = v; g = t; b = p; break;
        case 1:  r = q; g = v; b = p; break;
        case 2:  r = p; g = v; b = t; break;
        case 3:  r = p; g = q; b = v; break;
        case 4:  r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
        }
    }
    uint32_t R = (uint32_t)(r * 255.0f + 0.5f);
    uint32_t G = (uint32_t)(g * 255.0f + 0.5f);
    uint32_t B = (uint32_t)(b * 255.0f + 0.5f);
    return (R << 24) | (G << 16) | (B << 8) | a;
}

/* packed rgb -> hsv (alpha ignored) */
static void
rgba_hsv(uint32_t rgba, float *h, float *s, float *v)
{
    float r = ((rgba >> 24) & 0xFF) / 255.0f;
    float g = ((rgba >> 16) & 0xFF) / 255.0f;
    float b = ((rgba >> 8) & 0xFF) / 255.0f;
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    float d = mx - mn;
    *v = mx;
    *s = mx > 0.0f ? d / mx : 0.0f;
    if (d <= 0.0f) {
        *h = 0.0f;
        return;
    }
    float hh;
    if (mx == r)      hh = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) hh = (b - r) / d + 2.0f;
    else              hh = (r - g) / d + 4.0f;
    *h = hh / 6.0f;
}

static uint32_t
current_rgba(struct colorpick *c)
{
    return hsv_rgba(c->h, c->s, c->v, c->a);
}

/* ---- layout ---- */

struct regions {
    int svx, svy, svw, svh;
    int hux, huy, huw, huh;
    int swx, swy, sww, swh;
    int prx, pry, prw, prh;
};

static void
regions_of(bd_id id, struct regions *r)
{
    int x, y, w, h;
    bd_widget_rect(id, &x, &y, &w, &h);
    int prh = PRESET_H;
    int toph = h - prh - GAP;
    if (toph < 24) { toph = h; prh = 0; }   /* too short: drop the preset row */
    r->prx = x; r->pry = y + h - prh; r->prw = w; r->prh = prh;
    r->sww = SWATCH_W; r->swx = x + w - SWATCH_W; r->swy = y; r->swh = toph;
    r->huw = HUE_W; r->hux = r->swx - GAP - HUE_W; r->huy = y; r->huh = toph;
    r->svx = x; r->svy = y; r->svw = r->hux - GAP - x; r->svh = toph;
    if (r->svw < 8) r->svw = 8;
}

/* ---- render ---- */

static void
draw_ring(float cx, float cy, float rad, uint32_t inner, uint32_t outer)
{
    bd_draw_rect_lines(cx - rad - 1, cy - rad - 1, rad * 2 + 2, rad * 2 + 2, outer);
    bd_draw_rect_lines(cx - rad, cy - rad, rad * 2, rad * 2, inner);
}

static void
colorpick_render(bd_id id, void *state)
{
    struct colorpick *c = state;
    const bd_theme *th = bd_gui_theme();
    struct regions r;
    regions_of(id, &r);

    /* saturation/value square: a coarse grid of flat cells */
    float cw = (float)r.svw / SVN, ch = (float)r.svh / SVN;
    for (int i = 0; i < SVN; i++)
        for (int j = 0; j < SVN; j++) {
            float s = (i + 0.5f) / SVN, v = 1.0f - (j + 0.5f) / SVN;
            bd_draw_rect(r.svx + i * cw, r.svy + j * ch, cw + 1.0f, ch + 1.0f,
                hsv_rgba(c->h, s, v, 0xFF));
        }
    bd_draw_rect_lines((float)r.svx, (float)r.svy, (float)r.svw, (float)r.svh,
        th->border);
    /* SV handle */
    draw_ring(r.svx + c->s * r.svw, r.svy + (1.0f - c->v) * r.svh, 4.0f,
        0x000000FFu, 0xFFFFFFFFu);

    /* hue bar */
    float sh = (float)r.huh / HUEN;
    for (int k = 0; k < HUEN; k++)
        bd_draw_rect((float)r.hux, r.huy + k * sh, (float)r.huw, sh + 1.0f,
            hsv_rgba((k + 0.5f) / HUEN, 1.0f, 1.0f, 0xFF));
    bd_draw_rect_lines((float)r.hux, (float)r.huy, (float)r.huw, (float)r.huh,
        th->border);
    /* hue handle: a bar across the strip */
    float hy = r.huy + c->h * r.huh;
    bd_draw_rect_lines((float)r.hux - 1, hy - 2, (float)r.huw + 2, 4, 0x000000FFu);
    bd_draw_rect_lines((float)r.hux, hy - 1, (float)r.huw, 2, 0xFFFFFFFFu);

    /* swatch */
    bd_draw_rect((float)r.swx, (float)r.swy, (float)r.sww, (float)r.swh,
        current_rgba(c));
    bd_draw_rect_lines((float)r.swx, (float)r.swy, (float)r.sww, (float)r.swh,
        th->border);

    /* presets */
    if (r.prh > 0) {
        float pw = (float)r.prw / N_PRESETS;
        for (int i = 0; i < N_PRESETS; i++) {
            bd_draw_rect(r.prx + i * pw, (float)r.pry, pw + 1.0f, (float)r.prh,
                presets[i]);
            bd_draw_rect_lines(r.prx + i * pw, (float)r.pry, pw, (float)r.prh,
                th->border);
        }
    }
}

/* ---- events ---- */

static void
fire(bd_id id, struct colorpick *c)
{
    if (c->cb)
        c->cb(id, c->arg, current_rgba(c));
}

/* returns 1 if the point landed on something we handle */
static int
colorpick_pick(bd_id id, struct colorpick *c, int px, int py, int pressed)
{
    struct regions r;
    regions_of(id, &r);
    if (c->drag == DRAG_SV ||
        (pressed && px >= r.svx && px < r.svx + r.svw &&
         py >= r.svy && py < r.svy + r.svh)) {
        c->drag = DRAG_SV;
        c->s = clamp01((float)(px - r.svx) / r.svw);
        c->v = clamp01(1.0f - (float)(py - r.svy) / r.svh);
        fire(id, c);
        return 1;
    }
    if (c->drag == DRAG_HUE ||
        (pressed && px >= r.hux && px < r.hux + r.huw &&
         py >= r.huy && py < r.huy + r.huh)) {
        c->drag = DRAG_HUE;
        c->h = clamp01((float)(py - r.huy) / r.huh);
        fire(id, c);
        return 1;
    }
    if (pressed && r.prh > 0 && px >= r.prx && px < r.prx + r.prw &&
        py >= r.pry && py < r.pry + r.prh) {
        int i = (int)((px - r.prx) / ((float)r.prw / N_PRESETS));
        if (i >= 0 && i < N_PRESETS) {
            rgba_hsv(presets[i], &c->h, &c->s, &c->v);
            c->a = presets[i] & 0xFF;
            fire(id, c);
        }
        return 1;
    }
    return 0;
}

static int
colorpick_event(bd_id id, void *state, const bd_event *ev)
{
    struct colorpick *c = state;
    switch (ev->type) {
    case BD_EV_MOUSE_DOWN:
        if (ev->button == BD_MOUSE_LEFT)
            return colorpick_pick(id, c, ev->x, ev->y, 1);
        return 0;
    case BD_EV_MOUSE_MOVE:
        if (c->drag != DRAG_NONE)
            return colorpick_pick(id, c, ev->x, ev->y, 0);
        return 0;
    case BD_EV_MOUSE_UP:
        c->drag = DRAG_NONE;
        return 1;
    default:
        return 0;
    }
}

static void
colorpick_init(bd_id id, void *state)
{
    struct colorpick *c = state;
    c->a = 0xFF;
    bd_set(id, BD_PREF_W_I, 220, BD_PREF_H_I, 180, BD_END);
}

static const bd_widget_class colorpick_class = {
    .name = "colorpick",
    .state_size = sizeof(struct colorpick),
    .init = colorpick_init,
    .render = colorpick_render,
    .event = colorpick_event,
};

bd_id
bd_colorpick_create(bd_id parent, const bd_colorpick_desc *desc, ...)
{
    if (colorpick_type == 0)
        colorpick_type = bd_register_widget_class(&colorpick_class);

    va_list ap;
    va_start(ap, desc);
    bd_id id = bd_create_va(parent, colorpick_type, ap);
    va_end(ap);

    struct colorpick *c = bd_widget_state(id);
    if (c && desc) {
        uint32_t col = desc->color ? desc->color : 0x000000FFu;
        rgba_hsv(col, &c->h, &c->s, &c->v);
        c->a = col & 0xFF ? (col & 0xFF) : 0xFF;
        c->cb = desc->cb;
        c->arg = desc->arg;
    }
    return id;
}

uint32_t
bd_colorpick_get(bd_id id)
{
    if (bd_widget_type(id) != colorpick_type)
        return 0;
    return current_rgba(bd_widget_state(id));
}

void
bd_colorpick_set(bd_id id, uint32_t rgba)
{
    if (bd_widget_type(id) != colorpick_type)
        return;
    struct colorpick *c = bd_widget_state(id);
    rgba_hsv(rgba, &c->h, &c->s, &c->v);
    c->a = (rgba & 0xFF) ? (rgba & 0xFF) : 0xFF;
}
