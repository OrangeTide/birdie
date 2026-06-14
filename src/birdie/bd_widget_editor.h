#ifndef BD_WIDGET_EDITOR_H
#define BD_WIDGET_EDITOR_H

#include "widget.h"
#include <stdint.h>

/*
 * Rich-text editor widget, built on the extension API (widget_ext.h). A
 * higher-level, row-oriented text editor with the same multi-line editing
 * model as BD_MULTILINE plus a rich-text styling layer, suitable for a small
 * code or music (ABC notation) editor.
 *
 * Text is plain UTF-8; styling is carried separately as a list of style runs
 * over byte ranges, so the same mechanism serves both syntax highlighting
 * (a tokenizer emits runs) and transient highlight (mark the playing row).
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* Style flags (a bitmask). Bold is faux-bold (double-struck); italic is
 * recorded but not yet sheared by the renderer; super/subscript shift the
 * baseline. */
enum {
	BD_RT_BOLD      = 1 << 0,
	BD_RT_ITALIC    = 1 << 1,
	BD_RT_UNDERLINE = 1 << 2,
	BD_RT_STRIKE    = 1 << 3,
	BD_RT_SUPER     = 1 << 4,
	BD_RT_SUB       = 1 << 5,
};

/* A run's appearance. fg/bg are RGBA8; fg == 0 uses the theme text color,
 * bg == 0 draws no background. */
typedef struct bd_rich_style {
	unsigned flags;   /* BD_RT_* */
	uint32_t fg;
	uint32_t bg;
} bd_rich_style;

bd_id bd_editor_create(bd_id parent, ...);

/* ---- text ---- */
void bd_editor_set_text(bd_id id, const char *text);   /* replace all; clears styles */
int  bd_editor_text(bd_id id, char *out, int cap);     /* -> total length */
int  bd_editor_row_count(bd_id id);
int  bd_editor_row_text(bd_id id, int row, char *out, int cap); /* -> row length */
void bd_editor_insert_row(bd_id id, int row, const char *s);
void bd_editor_replace_row(bd_id id, int row, const char *s);
void bd_editor_delete_row(bd_id id, int row);

/* ---- lock (read-only; the row API and styling still work) ---- */
void bd_editor_set_locked(bd_id id, int locked);
int  bd_editor_locked(bd_id id);

/* ---- styling / highlight ---- */
void bd_editor_clear_styles(bd_id id);
/* style a byte range [start,end) of the whole text */
void bd_editor_style_span(bd_id id, int start, int end, bd_rich_style s);
/* style a whole row, or byte columns [col0,col1) within a row */
void bd_editor_highlight_row(bd_id id, int row, bd_rich_style s);
void bd_editor_highlight_span(bd_id id, int row, int col0, int col1,
                              bd_rich_style s);

#endif
