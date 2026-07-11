#ifndef BD_WIDGET_PROGRESS_H
#define BD_WIDGET_PROGRESS_H

#include "widget.h"
#include <stdint.h>

/*
 * A progress bar: a plain determinate fill of a 0..1 fraction, the simple
 * sibling of BD_METER_BAR (see bd_widget_meter.h) without zones/peak/ballistics.
 * Output-only. Optionally draws a "NN%" readout over the fill, or runs an
 * indeterminate marquee (a chunk sliding back and forth) when the fraction is
 * unknown. A round ("ball") progress display is BD_METER_VIAL instead.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

typedef struct bd_progress_desc {
	float       value;         /* 0..1 (ignored while indeterminate) */
	int         indeterminate; /* 1 = marquee animation, unknown fraction */
	int         show_percent;  /* 1 = draw "NN%" centered over the bar */
	int         orient;        /* BD_HORIZONTAL (0) | BD_VERTICAL (1) */
	uint32_t    color;         /* fill color; 0 = a theme accent */
	const char *label;         /* optional caption drawn to the left */
} bd_progress_desc;

bd_id bd_progress_create(bd_id parent, const bd_progress_desc *desc, ...);

void  bd_progress_set(bd_id id, float value);   /* clamped to 0..1 */
float bd_progress_get(bd_id id);
void  bd_progress_set_indeterminate(bd_id id, int on);

#endif
