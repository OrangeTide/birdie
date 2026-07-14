#ifndef BD_WIDGET_COLORPICK_H
#define BD_WIDGET_COLORPICK_H

#include "widget.h"
#include <stdint.h>

/*
 * BD_COLORPICK -- an HSV colour picker (doc/gui/dialogs.md Phase 3). A
 * saturation/value square, a hue bar beside it, a live swatch, and a row of
 * preset swatches, all in one leaf extension widget (no child widgets, no
 * shader: the gradients are drawn as a coarse grid of flat cells so it works on
 * every backend). Drag in the square to set saturation/value, drag the hue bar
 * to set hue, or click a preset.
 *
 * The colour is a packed 0xRRGGBBAA (the alpha byte is preserved, defaulting to
 * 0xFF). Created from a descriptor with an on-change callback, matching the
 * value-widget convention; a NULL descriptor takes the defaults (opaque black).
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

typedef void (*bd_colorpick_cb)(bd_id id, void *arg, uint32_t rgba);

typedef struct bd_colorpick_desc {
    uint32_t        color;   /* initial 0xRRGGBBAA (alpha kept; 0 -> opaque) */
    bd_colorpick_cb cb;      /* fired on every change */
    void           *arg;
} bd_colorpick_desc;

bd_id    bd_colorpick_create(bd_id parent, const bd_colorpick_desc *desc, ...);
uint32_t bd_colorpick_get(bd_id id);           /* current 0xRRGGBBAA */
void     bd_colorpick_set(bd_id id, uint32_t rgba);  /* no callback */

#endif /* BD_WIDGET_COLORPICK_H */
