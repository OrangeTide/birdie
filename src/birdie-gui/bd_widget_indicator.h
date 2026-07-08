#ifndef BD_WIDGET_INDICATOR_H
#define BD_WIDGET_INDICATOR_H

#include "widget.h"

/*
 * An indicator light: a skeuomorphic panel-mount LED that shows a discrete
 * state, drawn in a fragment shader like the knob and toggle. The lens is a
 * domed 3mm/5mm LED, clear (a bright hotspot over a visible point source),
 * frosted (an even diffuse glow), or a faceted cut-glass jewel (the classic
 * mid-century equipment pilot lens: radial facets that sparkle even unlit).
 *
 * The state model is a list of lit colors. State 0 is always "off" (a dark,
 * faintly color-tinted lens); states 1..N light the Nth listed color. So a
 * single color is a plain on/off lamp, two colors are a bi-color, three are the
 * classic bi-color "off / A / B / both" set (name the blend explicitly rather
 * than let the widget guess), and so on.
 *
 * Colors are given as one string of space- or comma-separated tokens, each a
 * name (violet, blue, green, amber, yellow, orange, red, white) or an HTML hex
 * (#rgb, #rrggbb, #rrggbbaa). The widget parses and owns them, so the caller
 * passes a borrowed string and keeps no array. An empty/NULL string defaults to
 * a single amber state.
 *
 * Output by default. Set `clickable` to make it a lamp-lit button: a press
 * cycles the state (wrapping through off) and fires the callback.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* Lens finish. */
enum { BD_LENS_CLEAR = 0, BD_LENS_FROSTED, BD_LENS_JEWEL };

/* Click callback (clickable indicators): `state` is the new state index. */
typedef void (*bd_indicator_cb)(bd_id id, void *arg, int state);

typedef struct bd_indicator_desc {
	const char     *colors;   /* lit states 1..N; NULL/"" = one amber state */
	int             lens;     /* BD_LENS_CLEAR | BD_LENS_FROSTED | BD_LENS_JEWEL */
	int             diameter; /* lens diameter in px; 0 = default */
	int             state;    /* initial state (0 = off) */
	int             clickable;/* 1 = lamp button, click cycles states */
	const char     *label;    /* optional caption drawn beside the lamp */
	bd_indicator_cb cb;       /* clickable: fires with the new state */
	void           *arg;
} bd_indicator_desc;

bd_id bd_indicator_create(bd_id parent, const bd_indicator_desc *desc, ...);

/* Replace the color list; re-parsed and owned by the widget. The current state
 * is clamped into the new range. */
void  bd_indicator_set_colors(bd_id id, const char *colors);

void  bd_indicator_set(bd_id id, int state);  /* clamped to [0, N] */
int   bd_indicator_get(bd_id id);
int   bd_indicator_states(bd_id id);          /* total states, N+1 (incl. off) */

#endif
