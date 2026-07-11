#ifndef BD_WIDGET_EDITOR_H
#define BD_WIDGET_EDITOR_H

#include "widget.h"
#include <stdint.h>

/*
 * Rich-text editor widget, built on the extension API (widget_ext.h). A
 * higher-level, row-oriented text editor with the same multi-line editing
 * model as BD_TEXT_AREA plus a rich-text styling layer, suitable for a small
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

/* ---- submit hook ----
 *
 * The editor is multi-line, so Enter inserts a newline by default and
 * Ctrl+Enter submits (fires the hook). bd_editor_set_enter_submits(id, 1)
 * swaps that: plain Enter submits and Shift/Ctrl+Enter inserts a newline,
 * for single-line-style entry (a command line, a REPL, a rename field). In
 * either mode a submit with no hook installed falls back to inserting a
 * newline, so the key is never dead. The callback fires on the editor id. */
void bd_editor_on_submit(bd_id id, bd_callback_fn fn, void *data);
void bd_editor_set_enter_submits(bd_id id, int on);
int  bd_editor_enter_submits(bd_id id);

/* ---- autocomplete ----
 *
 * Install a completion provider and the editor shows a popup as the user types.
 * Once the identifier fragment before the caret reaches the minimum length
 * (bd_editor_set_complete_min, default 2) the editor calls the provider with
 * that prefix; if it returns any items, a floating list appears under the
 * caret and re-queries on each further keystroke. Up/Down move the highlight,
 * Enter accepts (replacing the fragment with the item's `text`), and Esc, a
 * caret move, or a non-word character dismiss it. A word char is [A-Za-z0-9_].
 * Pass fn = NULL to disable. */
typedef struct bd_completion {
	const char *text;    /* the replacement token inserted on accept */
	const char *detail;  /* optional right-aligned hint (kind/type); NULL = none */
} bd_completion;

/* Fill up to `max` completions for `prefix`; return the count written (0 = no
 * popup). The strings are copied by the editor, so they may be transient. */
typedef int (*bd_completer_fn)(bd_id editor, const char *prefix,
                               bd_completion *out, int max, void *user);

void bd_editor_set_completer(bd_id id, bd_completer_fn fn, void *user);
void bd_editor_set_complete_min(bd_id id, int chars);  /* auto-trigger prefix length */

/* ---- lock (read-only; the row API and styling still work) ---- */
void bd_editor_set_locked(bd_id id, int locked);
int  bd_editor_locked(bd_id id);

/* Fixed-width (monospace) face on/off; on by default (code / ABC). */
void bd_editor_set_monospace(bd_id id, int on);

/* ---- styling / highlight ---- */
void bd_editor_clear_styles(bd_id id);
/* style a byte range [start,end) of the whole text */
void bd_editor_style_span(bd_id id, int start, int end, bd_rich_style s);
/* style a whole row, or byte columns [col0,col1) within a row */
void bd_editor_highlight_row(bd_id id, int row, bd_rich_style s);
void bd_editor_highlight_span(bd_id id, int row, int col0, int col1,
                              bd_rich_style s);

#endif
