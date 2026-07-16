#ifndef BD_WIDGET_TABLE_H
#define BD_WIDGET_TABLE_H

#include "widget.h"

/*
 * Multi-column table / data grid, built on the extension API (widget_ext.h).
 * Model-driven like the explorer: the app supplies a row count and a cell
 * accessor, the widget owns scrolling, selection, keyboard navigation, and
 * click-to-sort by column. Rows are referred to by their model index; sorting
 * reorders a private visual permutation, so a selection survives a re-sort.
 *
 * Built for the MUD list, the trigger #list, and any tabular dialog. The
 * scrolling body is scissor-clipped; long cells are ellipsized to their column.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

enum { BD_TABLE_LEFT, BD_TABLE_RIGHT, BD_TABLE_CENTER };

/* per-column flags */
enum {
	BD_TABLE_COL_NUMERIC = 1 << 0,  /* sort this column as numbers, not text */
	BD_TABLE_COL_NOSORT  = 1 << 1   /* clicking the header does not sort */
};

/* A column. The array passed to bd_table_create is copied (titles are
 * borrowed and must outlive the widget). width 0 means "share the leftover
 * space" with the other zero-width columns. */
typedef struct bd_table_column {
	const char *title;
	int width;      /* fixed pixel width, or 0 = grow */
	int align;      /* BD_TABLE_LEFT / RIGHT / CENTER */
	int flags;      /* BD_TABLE_COL_* */
} bd_table_column;

/* Optional per-row visual style, filled by model.row_style. All fields are
 * optional: draw the row bold, override its text colour, tint its background. A
 * zero fg means the theme default text colour; a zero bg means no tint. */
typedef struct bd_table_row_style {
	int      bold;
	uint32_t fg;
	uint32_t bg;
} bd_table_row_style;

/* Data source. rows() returns the row count; cell() returns the text for a
 * (row, col), valid for the duration of the call (NULL = empty). The optional
 * icon() and row_style() hooks add rich cells: a small per-cell glyph (for a
 * status / priority / attachment column) and per-row bold/colour/tint. */
typedef struct bd_table_model {
	int (*rows)(void *ctx);
	const char *(*cell)(void *ctx, int row, int col);
	void *ctx;
	/* Optional: a small icon drawn at the left of a cell (texture id 0 = none). */
	bd_texture (*icon)(void *ctx, int row, int col);
	/* Optional: per-row bold / colour / background tint. */
	void (*row_style)(void *ctx, int row, bd_table_row_style *out);
} bd_table_model;

/* App callbacks; all optional. Row indices are model rows. */
typedef struct bd_table_cb {
	void (*activate)(bd_id w, int row, void *ctx);          /* dbl-click/Enter */
	void (*context)(bd_id w, int row, int sx, int sy, void *ctx); /* right-click */
	void (*selection_changed)(bd_id w, void *ctx);
	void *ctx;
} bd_table_cb;

/* Create a table. cols/ncols define the columns (copied); model is required;
 * cb may be NULL. Trailing args are BD_* attributes ending in BD_END. */
bd_id bd_table_create(bd_id parent, const bd_table_column *cols, int ncols,
                      const bd_table_model *model, const bd_table_cb *cb, ...);

/* Re-query the model after its data changed. Keeps the selection when the row
 * count is unchanged; resets it otherwise. Re-applies the active sort. */
void bd_table_refresh(bd_id id);

/* Copy up to max selected model-row indices into rows; returns the selection
 * count (which may exceed max). Pass rows=NULL/max=0 to query the count. */
int bd_table_selection(bd_id id, int *rows, int max);

/* Select model row. add=0 replaces the selection, add=1 adds to it. */
void bd_table_select(bd_id id, int row, int add);

/* The cursor's model row, or -1 if none. */
int bd_table_current(bd_id id);

#endif
