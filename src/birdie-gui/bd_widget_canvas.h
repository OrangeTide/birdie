#ifndef BD_WIDGET_CANVAS_H
#define BD_WIDGET_CANVAS_H

#include "widget.h"
#include <stdint.h>

/*
 * Drawing canvas — a pressure-sensitive sketch surface built on the extension
 * API (widget_ext.h) and the stylus event routing in core (BD_EV_PEN_*). It
 * turns pen input into variable-width ink strokes: tip pressure sets the nib
 * width, tilt widens it, the barrel button switches to a second ink, and the
 * eraser end rubs out strokes it passes over. While the pen hovers in proximity
 * (before contact) a nib-sized cursor previews where ink will land.
 *
 * It works with a mouse too: a left-drag paints at full pressure, so the widget
 * is usable on backends with no tablet.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* Create a drawing canvas. Trailing args are BD_* attributes ending in BD_END
 * (e.g. BD_GROW_I, BD_PREF_H_I). */
bd_id bd_canvas_create(bd_id parent, ...);

/* Ink color (0xRRGGBBAA) used for new strokes drawn with the tip. The barrel
 * button paints with bd_canvas_set_alt_ink instead. */
void bd_canvas_set_ink(bd_id id, uint32_t rgba);
void bd_canvas_set_alt_ink(bd_id id, uint32_t rgba);

/* Nib width in pixels at full pressure (default 6). */
void bd_canvas_set_nib(bd_id id, float px);

/* Erase every stroke. */
void bd_canvas_clear(bd_id id);

/* Number of completed strokes on the canvas (for tests / status). */
int  bd_canvas_stroke_count(bd_id id);

#endif
