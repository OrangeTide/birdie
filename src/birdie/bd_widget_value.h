#ifndef BD_WIDGET_VALUE_H
#define BD_WIDGET_VALUE_H

#include "widget.h"

/*
 * Value-editing widgets, built on the extension API (widget_ext.h) and the
 * pointer-capture event routing in core. The first is the slider; knobs,
 * scroll wheels, and X-Y pads follow on the same value model.
 *
 * Absolute widgets carry a normalized value in [0,1]; the caller maps it to a
 * real range. Drag updates the value and fires the change callback.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

enum { BD_HORIZONTAL = 0, BD_VERTICAL = 1 };

/* Change callback: t is the widget's normalized value in [0,1]. */
typedef void (*bd_value_cb)(bd_id id, void *arg, float t);

/* Create a slider. orient is BD_HORIZONTAL or BD_VERTICAL; value is the
 * initial [0,1] position. Trailing args are BD_* attributes ending in BD_END
 * (e.g. BD_GROW_I, BD_PREF_W_I). */
bd_id bd_slider_create(bd_id parent, int orient, float value,
                       bd_value_cb cb, void *arg, ...);

void  bd_slider_set(bd_id id, float value);   /* clamped to [0,1] */
float bd_slider_get(bd_id id);

#endif
