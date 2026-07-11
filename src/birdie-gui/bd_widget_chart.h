#ifndef BD_WIDGET_CHART_H
#define BD_WIDGET_CHART_H

#include "widget.h"
#include <stdint.h>

/*
 * Time-series strip chart, built on the extension API (widget_ext.h). A
 * scrolling multi-series line graph in the style of xload / a system monitor:
 * the horizontal axis is time (newest sample at the right), several series are
 * overlaid as colored ink traces on a graph-paper grid, and each series
 * autoscales over its window. Output-only, like the meters.
 *
 * The widget owns one ring buffer per series (`window` samples). The app feeds
 * it with bd_chart_push()/bd_chart_push_row() on whatever cadence it samples;
 * the widget keeps the most recent `window` values and plots them across the
 * full width. A series' `unit` of "%" pins its scale to 0..100 and shares the
 * grid; any other unit autoscales to the series min/max, and up to two such
 * non-percentage series get a labeled value axis (left, then right) so their
 * absolute range is readable. Percentages stay unlabeled on the shared grid.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* A series definition. label/unit are copied. color 0 picks from a palette.
 * unit "%" special-cases the scale to 0..100; NULL/"" autoscales without a
 * value axis; any other unit autoscales and may claim a labeled axis. */
typedef struct bd_chart_series {
	const char *label;
	const char *unit;
	uint32_t    color;
} bd_chart_series;

/* Create a chart keeping `window` samples per series (clamped to a sane range;
 * <= 0 uses a default). Trailing args are BD_* attributes ending in BD_END. */
bd_id bd_chart_create(bd_id parent, int window, ...);

/* Add a series; returns its index (0-based) or -1 if full. */
int bd_chart_add_series(bd_id id, const bd_chart_series *s);

/* Append one sample to a series' ring (the oldest falls off at capacity). */
void bd_chart_push(bd_id id, int series, float value);

/* Append one sample to each of the first `n` series at once (a time column). */
void bd_chart_push_row(bd_id id, const float *values, int n);

/* Drop all samples (keeps the series definitions). */
void bd_chart_clear(bd_id id);

/* Toggles (both on by default). */
void bd_chart_set_grid(bd_id id, int on);
void bd_chart_set_legend(bd_id id, int on);

#endif
