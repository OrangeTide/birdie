/*
 * bd_findbar -- a composed find/replace bar over bd_editor_find. See
 * bd_findbar.h. The bar is a column of one or two rows of ordinary widgets; the
 * search field's on_change drives a live search, its on_click (Enter) steps to
 * the next match, and its on_close (Escape) dismisses the bar.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_findbar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct bd_findbar {
	bd_id  root;
	bd_id  editor;
	bd_id  search;
	bd_id  replace;
	bd_id  count;
	unsigned flags;
	int    with_replace;
	char   countbuf[24];
	bd_callback_fn on_close;
	void  *on_close_data;
};

/* ---- internals ---- */

static void
update_count(bd_findbar *fb)
{
	int n = bd_editor_find_count(fb->editor);
	int cur = bd_editor_find_current(fb->editor);
	if (n <= 0 || cur < 0)
		snprintf(fb->countbuf, sizeof fb->countbuf, "0/0");
	else
		snprintf(fb->countbuf, sizeof fb->countbuf, "%d/%d", cur + 1, n);
	bd_set(fb->count, BD_LABEL_S, fb->countbuf, BD_END);
}

static void
run_search(bd_findbar *fb)
{
	const char *t = bd_get_s(fb->search, BD_LABEL_S);
	bd_editor_find(fb->editor, t ? t : "", fb->flags);
	update_count(fb);
}

static const char *
replace_text(bd_findbar *fb)
{
	const char *r = fb->replace != BD_NONE ? bd_get_s(fb->replace, BD_LABEL_S)
	    : NULL;
	return r ? r : "";
}

/* callbacks: the on_*_data is always the bd_findbar handle */
static void cb_changed(bd_id id, void *u) { (void)id; run_search(u); }

static void
cb_enter(bd_id id, void *u)
{
	(void)id;
	bd_findbar *fb = u;
	bd_editor_find_next(fb->editor);
	update_count(fb);
}

static void
cb_next(bd_id id, void *u)
{
	(void)id;
	bd_findbar *fb = u;
	bd_editor_find_next(fb->editor);
	update_count(fb);
}

static void
cb_prev(bd_id id, void *u)
{
	(void)id;
	bd_findbar *fb = u;
	bd_editor_find_prev(fb->editor);
	update_count(fb);
}

static void
cb_replace(bd_id id, void *u)
{
	(void)id;
	bd_findbar *fb = u;
	bd_editor_replace(fb->editor, replace_text(fb));
	update_count(fb);
}

static void
cb_replace_all(bd_id id, void *u)
{
	(void)id;
	bd_findbar *fb = u;
	bd_editor_replace_all(fb->editor, replace_text(fb));
	run_search(fb);            /* re-highlight what remains (usually nothing) */
}

static void
cb_close(bd_id id, void *u)
{
	(void)id;
	bd_findbar *fb = u;
	if (fb->on_close)
		fb->on_close(fb->root, fb->on_close_data);
}

/* ---- public ---- */

bd_findbar *
bd_findbar_create(bd_id parent, bd_id editor, int with_replace)
{
	bd_findbar *fb = calloc(1, sizeof *fb);
	if (!fb)
		return NULL;
	fb->editor = editor;
	fb->replace = BD_NONE;
	fb->with_replace = with_replace;

	fb->root = bd_create(parent, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_COL,
	    BD_PAD_I, 3, BD_GAP_I, 3,
	    BD_PREF_H_I, with_replace ? 56 : 28, BD_END);

	bd_id r1 = bd_create(fb->root, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_ROW,
	    BD_GAP_I, 4, BD_GROW_I, 1, BD_END);
	bd_create(r1, BD_LABEL, BD_LABEL_S, "Find:", BD_PREF_W_I, 36, BD_END);
	fb->search = bd_create(r1, BD_TEXT_FIELD, BD_GROW_I, 1,
	    BD_TIP_S, "Search (Enter = next, Esc = close)",
	    BD_ON_CHANGE_F, cb_changed, BD_ON_CHANGE_P, fb,
	    BD_ON_CLICK_F, cb_enter, BD_ON_CLICK_P, fb,
	    BD_ON_CLOSE_F, cb_close, BD_ON_CLOSE_P, fb, BD_END);
	fb->count = bd_create(r1, BD_LABEL, BD_LABEL_S, "0/0",
	    BD_PREF_W_I, 44, BD_END);
	bd_create(r1, BD_BUTTON, BD_LABEL_S, "<", BD_PREF_W_I, 26,
	    BD_TIP_S, "Previous match",
	    BD_ON_CLICK_F, cb_prev, BD_ON_CLICK_P, fb, BD_END);
	bd_create(r1, BD_BUTTON, BD_LABEL_S, ">", BD_PREF_W_I, 26,
	    BD_TIP_S, "Next match",
	    BD_ON_CLICK_F, cb_next, BD_ON_CLICK_P, fb, BD_END);
	bd_create(r1, BD_BUTTON, BD_LABEL_S, "x", BD_PREF_W_I, 26,
	    BD_TIP_S, "Close (Esc)",
	    BD_ON_CLICK_F, cb_close, BD_ON_CLICK_P, fb, BD_END);

	if (with_replace) {
		bd_id r2 = bd_create(fb->root, BD_PANEL, BD_LAYOUT_I,
		    BD_LAYOUT_ROW, BD_GAP_I, 4, BD_GROW_I, 1, BD_END);
		bd_create(r2, BD_LABEL, BD_LABEL_S, "Repl:",
		    BD_PREF_W_I, 36, BD_END);
		fb->replace = bd_create(r2, BD_TEXT_FIELD, BD_GROW_I, 1,
		    BD_TIP_S, "Replacement text", BD_END);
		bd_create(r2, BD_BUTTON, BD_LABEL_S, "Replace", BD_PREF_W_I, 64,
		    BD_TIP_S, "Replace the current match",
		    BD_ON_CLICK_F, cb_replace, BD_ON_CLICK_P, fb, BD_END);
		bd_create(r2, BD_BUTTON, BD_LABEL_S, "All", BD_PREF_W_I, 40,
		    BD_TIP_S, "Replace every match",
		    BD_ON_CLICK_F, cb_replace_all, BD_ON_CLICK_P, fb, BD_END);
	}
	return fb;
}

void
bd_findbar_free(bd_findbar *fb)
{
	if (!fb)
		return;
	if (fb->root != BD_NONE)
		bd_destroy(fb->root);
	free(fb);
}

bd_id
bd_findbar_widget(const bd_findbar *fb)
{
	return fb ? fb->root : BD_NONE;
}

bd_id
bd_findbar_editor(const bd_findbar *fb)
{
	return fb ? fb->editor : BD_NONE;
}

void
bd_findbar_open(bd_findbar *fb)
{
	if (!fb)
		return;
	bd_focus(fb->search);
	run_search(fb);
}

void
bd_findbar_set_flags(bd_findbar *fb, unsigned flags)
{
	if (!fb)
		return;
	fb->flags = flags;
	run_search(fb);
}

void
bd_findbar_set_query(bd_findbar *fb, const char *needle)
{
	if (!fb)
		return;
	bd_set(fb->search, BD_LABEL_S, needle ? needle : "", BD_END);
	run_search(fb);
}

unsigned
bd_findbar_flags(const bd_findbar *fb)
{
	return fb ? fb->flags : 0;
}

void
bd_findbar_on_close(bd_findbar *fb, bd_callback_fn fn, void *user)
{
	if (!fb)
		return;
	fb->on_close = fn;
	fb->on_close_data = user;
}
