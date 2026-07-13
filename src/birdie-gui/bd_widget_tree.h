#ifndef BD_WIDGET_TREE_H
#define BD_WIDGET_TREE_H

#include "widget.h"
#include <stdint.h>

/*
 * Indented hierarchy list, built on the extension API (widget_ext.h). A
 * BD_LIST cousin that renders an expand/collapse outline over a model that
 * yields children on demand: a file/project tree, a class hierarchy, or any
 * nested structure. Model-driven like BD_TABLE, the widget owns scrolling,
 * selection, keyboard navigation (up/down, left/right collapse/expand,
 * type-ahead), and the twisty toggles.
 *
 * Nodes are named by an app-chosen uint64_t key (0 is reserved: it means the
 * invisible root, so `child_count(ctx, 0)` returns the top-level nodes, and no
 * real node may use key 0). The widget holds no tree structure of its own; it
 * re-walks the visible nodes from the model on each layout (respecting the
 * expand state it owns), so mutating the model then calling bd_tree_refresh()
 * repaints. Expand/collapse state lives in the widget (seed it with
 * bd_tree_set_expanded); an optional `expand` callback lets the app lazily
 * populate children the first time a node opens.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* One node, filled by the model's get() for the duration of that call only
 * (the label is borrowed and copied nowhere). */
typedef struct bd_tree_item {
	const char *label;        /* row text */
	bd_texture  icon;         /* optional; id 0 = no icon */
	int         has_children; /* draw a twisty (a folder, even if empty now) */
	int         enabled;      /* 0 = dimmed, not activatable */
	void       *user;         /* opaque, passed to the callbacks */
} bd_tree_item;

/* Data source. child_count/child enumerate a node's children (parent key 0 =
 * the roots); get() describes one node. get() is required. */
typedef struct bd_tree_model {
	int      (*child_count)(void *ctx, uint64_t parent);
	uint64_t (*child)(void *ctx, uint64_t parent, int index);
	void     (*get)(void *ctx, uint64_t node, bd_tree_item *out);
	void      *ctx;
} bd_tree_model;

/* App callbacks; all optional. `user` is the node's bd_tree_item.user. */
typedef struct bd_tree_cb {
	void (*select)  (bd_id w, uint64_t node, void *user);          /* cursor moved */
	void (*activate)(bd_id w, uint64_t node, void *user);          /* dbl-click / Enter on a leaf */
	void (*expand)  (bd_id w, uint64_t node, int expanded, void *user);
} bd_tree_cb;

/* Create a tree. model is required (copied by value; ctx must outlive the
 * widget); cb may be NULL. Trailing args are BD_* attributes ending in BD_END. */
bd_id bd_tree_create(bd_id parent, const bd_tree_model *model,
                     const bd_tree_cb *cb, ...);

/* Re-walk the model (after its structure changed). Keeps the selection and
 * expand state by key. */
void bd_tree_refresh(bd_id id);

/* Selected node key, or 0 if none. */
uint64_t bd_tree_selected(bd_id id);

/* Select a node (0 clears). Scrolls it into view if it is currently visible. */
void bd_tree_select(bd_id id, uint64_t node);

/* Open/close a node programmatically (does not fire the expand callback). */
void bd_tree_set_expanded(bd_id id, uint64_t node, int open);
int  bd_tree_is_expanded(bd_id id, uint64_t node);

/* Pixels of indent per depth level (default 16). */
void bd_tree_set_indent(bd_id id, int px);

#endif
