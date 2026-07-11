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

/*
 * A rotary knob rendered with a custom fragment shader: a brushed-aluminum
 * 1970s-hifi body with an orange-red indicator over a 270-degree sweep.
 * Vertical drag turns it (drag up to increase).
 */

/* Dial plate: the markings drawn around the knob. */
enum {
	BD_DIAL_NONE = 0,   /* no markings */
	BD_DIAL_DOTS,       /* evenly spaced dots (one per detent if step set,
	                       else 11) -- the default */
	BD_DIAL_BALANCE,    /* three dots: both ends and the center */
	BD_DIAL_LABELS,     /* numeric labels at the ends and round values
	                       between, decimal or hex (see .hex) */
};

typedef struct bd_knob_desc {
	float       min, max;   /* value range; both 0 defaults to 0..1 */
	float       step;       /* >0 quantizes the value into detents, turning
	                           the knob into an N-way rotary switch; 0 = smooth */
	float       value;      /* initial value, in [min,max] */
	int         dial;       /* BD_DIAL_* dial-plate style */
	int         hex;        /* BD_DIAL_LABELS in hexadecimal (e.g. MIDI CC) */
	int         relative;   /* endless jog dial: emits drag deltas instead of an
	                           absolute value, with finger dimples and no
	                           indicator. min/max/step/dial are ignored; the
	                           callback receives the delta. */
	int         dimples;    /* jog finger-dimples when relative (default 3) */
	bd_value_cb cb;         /* change callback; the value in [min,max], or the
	                           delta when relative */
	void       *arg;
} bd_knob_desc;

bd_id bd_knob_create(bd_id parent, const bd_knob_desc *desc, ...);
void  bd_knob_set(bd_id id, float value);   /* clamped to range, snapped to step */
float bd_knob_get(bd_id id);                /* value in [min,max] */

/*
 * A sliding on/off switch (modern mobile style): a rounded pill track with a
 * circular thumb that slides between the ends, the track tinting to the theme
 * accent when on. Serves the role of both a checkbox and a switch. Click flips
 * it; the thumb animates.
 */
typedef void (*bd_toggle_cb)(bd_id id, void *arg, int on);

bd_id bd_toggle_create(bd_id parent, int on, bd_toggle_cb cb, void *arg, ...);
void  bd_toggle_set(bd_id id, int on);
int   bd_toggle_get(bd_id id);

/*
 * A relative scroll/jog wheel: a ribbed cylinder you drag to spin. The
 * callback receives the spin delta (not an absolute value); a BD_VERTICAL wheel
 * spins up/down, a BD_HORIZONTAL one left/right.
 */
bd_id bd_wheel_create(bd_id parent, int orient, bd_value_cb cb, void *arg, ...);

/*
 * An X-Y pad: a recessed surface with a draggable chrome puck giving two
 * values in [0,1] (0.5,0.5 = center). The limit shape is a square or a circle
 * (joystick); a joystick can spring back to center on release.
 */
enum { BD_XY_SQUARE = 0, BD_XY_CIRCLE };

typedef void (*bd_xy_cb)(bd_id id, void *arg, float x, float y);

typedef struct bd_xypad_desc {
	int      shape;    /* BD_XY_SQUARE or BD_XY_CIRCLE */
	int      spring;   /* return the puck to center on release (joystick) */
	float    x, y;     /* initial position, each [0,1] */
	bd_xy_cb cb;
	void    *arg;
} bd_xypad_desc;

bd_id bd_xypad_create(bd_id parent, const bd_xypad_desc *desc, ...);
void  bd_xypad_get(bd_id id, float *x, float *y);
void  bd_xypad_set(bd_id id, float x, float y);

#endif

