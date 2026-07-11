#ifndef BD_WIDGET_METER_H
#define BD_WIDGET_METER_H

#include "widget.h"
#include <stdint.h>

/*
 * A compact meter: shows a single 0..1 quantity (signal strength, battery
 * charge, system load, character hit points, an audio level) as a physical
 * instrument. Output-only, drawn like the knob/indicator. One widget, several
 * appearances chosen with `style`:
 *
 *   BD_METER_BAR  — a level bar (horizontal or vertical), lit up to the value,
 *                   colored by zone; cheap, legible, reusable. `segments` gives
 *                   an LED-bargraph look instead of a smooth fill.
 *   BD_METER_VU   — a cream-faced arc with a swinging needle and a red zone near
 *                   full, the classic D'Arsonval VU/panel meter.
 *   BD_METER_EYE  — a "magic eye" tuning tube: a green fluorescent disc whose
 *                   dark shadow wedge closes as the value rises (a shader).
 *   BD_METER_PIE  — a filled pie/sector that grows with the value; abstract and
 *                   instantly readable.
 *   BD_METER_VIAL — a glass globe of liquid that fills with the value, the
 *                   action-RPG life/mana orb (a shader): colored liquid to a
 *                   bright meniscus inside a glass rim with a specular highlight.
 *
 * Color zones: `zones` is a low->high list of colors (parsed like the indicator:
 * names or #hex, space/comma separated), splitting 0..1 into bands. The band the
 * value falls in colors the moving element (needle/fill/eye/liquid); a
 * BD_METER_BAR lights each band in its own color. `stops` optionally gives the
 * N-1 ascending break points in 0..1 (e.g. "0.7 0.9" for green/amber/red); NULL
 * splits evenly. So hit points pass "red amber green", a load meter
 * "green amber red".
 *
 * Peak marker: set `peak` to hold a marker at the recent maximum (it decays
 * slowly back toward the live value), like a PPM's peak hold.
 *
 * Ballistics: `ballistic` picks how the moving element chases a new value —
 * BD_METER_EXACT jumps to it (deterministic; the default), BD_METER_VU_BALLISTIC
 * eases over ~300 ms like a real VU movement, BD_METER_PEAK_HOLD snaps up and
 * falls slowly. The eased modes animate, so they need the backend clock and a
 * per-frame redraw (the app/gallery loops already redraw); EXACT needs neither.
 *
 * A plain progress bar is a separate, simpler widget: see bd_widget_progress.h.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* Appearance. */
enum { BD_METER_BAR = 0, BD_METER_VU, BD_METER_EYE, BD_METER_PIE, BD_METER_VIAL };

/* Needle/fill response to a new value. */
enum { BD_METER_EXACT = 0, BD_METER_VU_BALLISTIC, BD_METER_PEAK_HOLD };

typedef struct bd_meter_desc {
	int         style;      /* BD_METER_* (default BD_METER_BAR) */
	float       value;      /* initial value, 0..1 */
	const char *zones;      /* low->high color list; NULL = one accent color */
	const char *stops;      /* N-1 ascending break points; NULL = even split */
	int         peak;       /* 1 = show a peak-hold marker */
	int         ballistic;  /* BD_METER_EXACT | _VU_BALLISTIC | _PEAK_HOLD */
	int         orient;     /* BAR only: BD_HORIZONTAL (0) | BD_VERTICAL (1) */
	int         segments;   /* BAR only: 0 = smooth; >0 = that many LED segments */
	int         size;       /* px: bar thickness / round diameter; 0 = default */
	const char *label;      /* optional caption drawn beside/under the meter */
	uint32_t    color;      /* accent when no zones given; 0 = a sensible default */
} bd_meter_desc;

/* Create a meter. Trailing args are BD_* attributes ending in BD_END (give a bar
 * BD_GROW_I or a length so it fills its slot). */
bd_id bd_meter_create(bd_id parent, const bd_meter_desc *desc, ...);

/* Set/read the target value (clamped to 0..1). bd_meter_get returns the
 * currently displayed value, which trails the target under a ballistic mode. */
void  bd_meter_set(bd_id id, float value);
float bd_meter_get(bd_id id);

/* Replace the zone colors / break points (re-parsed and owned by the widget). */
void  bd_meter_set_zones(bd_id id, const char *zones, const char *stops);

/* Drop the peak marker back to the live value. */
void  bd_meter_reset_peak(bd_id id);

#endif
