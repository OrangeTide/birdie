/*
 * bd_widget_tabview -- a tabbed container. See bd_widget_tabview.h.
 *
 * A thin, composite extension widget: it owns a core BD_TAB_BAR strip and a
 * BD_LAYOUT_FIXED content panel, and adds one BD_PANEL pane per tab as a child
 * of that content panel. Because the content panel is FIXED, every pane fills
 * the same rectangle (they overlap); the view keeps just the active pane visible
 * and hides the rest, so the core skips them in render and hit-testing while
 * each keeps its own laid-out contents. The tab strip's change callback and the
 * layout hook both re-sync visibility from the strip's active index, so a tab
 * click, a Left/Right key, or bd_tabview_select all converge.
 *
 * The widget declares BD_WC_CONTAINS_CHILDREN, so the core lays out, renders,
 * and routes input through its children (the strip and the panes) for free; the
 * only custom logic is building the subtree and syncing pane visibility.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_widget_tabview.h"
#include "widget_ext.h"
#include <stdarg.h>
#include <string.h>

#define TV_MAX     16   /* pane cap */
#define TV_TABH    26   /* tab-strip height, px */
#define TV_LABELS 512   /* newline-joined tab-label buffer */

struct tabview {
	bd_id          tabbar;         /* the folder-tab strip (a child) */
	bd_id          content;        /* FIXED panel holding the panes (a child) */
	bd_id          panes[TV_MAX];
	int            npanes;
	int            active;
	bd_callback_fn on_change;
	void          *on_change_data;
	char           labels[TV_LABELS];
	int            labels_len;
};

static int tv_type;

/* keep exactly the active pane visible */
static void
tv_sync(bd_id id, struct tabview *tv)
{
	if (tv->npanes == 0)
		return;
	if (tv->active < 0) tv->active = 0;
	if (tv->active >= tv->npanes) tv->active = tv->npanes - 1;
	for (int i = 0; i < tv->npanes; i++)
		bd_set(tv->panes[i], BD_VISIBLE_B, i == tv->active, BD_END);
	(void)id;
}

/* the tab strip changed the active tab: adopt it, re-sync, notify the host */
static void
tv_tab_changed(bd_id tabbar, void *data)
{
	(void)data;
	bd_id id = bd_parent(tabbar);           /* the strip's parent is the view */
	struct tabview *tv = bd_widget_state(id);
	if (!tv)
		return;
	tv->active = bd_tabbar_selected(tabbar);
	tv_sync(id, tv);
	if (tv->on_change)
		tv->on_change(id, tv->on_change_data);
}

/* ------------------------------------------------------------------ */
/* class hooks                                                        */
/* ------------------------------------------------------------------ */

static void
tv_layout(bd_id id, void *state, int w, int h)
{
	(void)w; (void)h;
	struct tabview *tv = state;
	/* the strip is authoritative for the active index (a click/key may have
	 * moved it); mirror it onto the panes every layout */
	if (tv->tabbar != BD_NONE)
		tv->active = bd_tabbar_selected(tv->tabbar);
	tv_sync(id, tv);
}

static const bd_widget_class tv_class = {
	.name       = "tabview",
	.state_size = sizeof(struct tabview),
	.layout     = tv_layout,
	.flags      = BD_WC_CONTAINS_CHILDREN,
};

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

void
bd_tabview_register(void)
{
	if (!tv_type)
		tv_type = bd_register_widget_class(&tv_class);
}

bd_id
bd_tabview_create(bd_id parent, ...)
{
	bd_tabview_register();
	va_list ap;
	va_start(ap, parent);
	bd_id id = bd_create_va(parent, tv_type, ap);
	va_end(ap);
	struct tabview *tv = bd_widget_state(id);
	if (!tv)
		return id;
	tv->active = 0;
	/* the view stacks its two children vertically: strip over content */
	bd_set(id, BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	tv->tabbar = bd_create(id, BD_TAB_BAR, BD_PREF_H_I, TV_TABH,
	    BD_ON_CLICK_F, tv_tab_changed, BD_END);
	/* FIXED so every pane fills the same rect (panes overlap; one shows) */
	tv->content = bd_create(id, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_FIXED,
	    BD_GROW_I, 1, BD_END);
	return id;
}

bd_id
bd_tabview_add_pane(bd_id tabview, const char *label)
{
	if (bd_widget_type(tabview) != tv_type)
		return BD_NONE;
	struct tabview *tv = bd_widget_state(tabview);
	if (tv->npanes >= TV_MAX)
		return BD_NONE;
	if (!label)
		label = "";

	/* append to the newline-joined label list and push it onto the strip */
	int n = (int)strlen(label);
	if (tv->labels_len + (tv->labels_len ? 1 : 0) + n < TV_LABELS) {
		if (tv->labels_len)
			tv->labels[tv->labels_len++] = '\n';
		memcpy(tv->labels + tv->labels_len, label, (size_t)n);
		tv->labels_len += n;
		tv->labels[tv->labels_len] = '\0';
	}
	bd_tabbar_set_tabs(tv->tabbar, tv->labels);

	bd_id pane = bd_create(tv->content, BD_PANEL,
	    BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);
	tv->panes[tv->npanes++] = pane;
	tv_sync(tabview, tv);
	return pane;
}

int
bd_tabview_count(bd_id tabview)
{
	if (bd_widget_type(tabview) != tv_type)
		return 0;
	return ((struct tabview *)bd_widget_state(tabview))->npanes;
}

int
bd_tabview_selected(bd_id tabview)
{
	if (bd_widget_type(tabview) != tv_type)
		return -1;
	struct tabview *tv = bd_widget_state(tabview);
	return tv->npanes ? tv->active : -1;
}

bd_id
bd_tabview_pane(bd_id tabview, int index)
{
	if (bd_widget_type(tabview) != tv_type)
		return BD_NONE;
	struct tabview *tv = bd_widget_state(tabview);
	if (index < 0 || index >= tv->npanes)
		return BD_NONE;
	return tv->panes[index];
}

void
bd_tabview_select(bd_id tabview, int index)
{
	if (bd_widget_type(tabview) != tv_type)
		return;
	struct tabview *tv = bd_widget_state(tabview);
	if (tv->npanes == 0)
		return;
	if (index < 0) index = 0;
	if (index >= tv->npanes) index = tv->npanes - 1;
	tv->active = index;
	bd_tabbar_select(tv->tabbar, index);
	tv_sync(tabview, tv);
}

void
bd_tabview_on_change(bd_id tabview, bd_callback_fn fn, void *data)
{
	if (bd_widget_type(tabview) != tv_type)
		return;
	struct tabview *tv = bd_widget_state(tabview);
	tv->on_change = fn;
	tv->on_change_data = data;
}
