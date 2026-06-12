#ifndef BD_WIDGET_EXT_H
#define BD_WIDGET_EXT_H

#include "widget.h"
#include "bd_backend.h"
#include <stdarg.h>

/*
 * Extension API — the second consumer tier of the toolkit. Where widget.h is
 * for apps that *use* widgets, this header is for code that *defines new kinds
 * of widgets*, the way the VT terminal does. An extension fills a
 * bd_widget_class, registers it to obtain a type id, then builds its own
 * typed create/accessor functions on top of the core create calls.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Per-type behavior. All hooks are optional (NULL = no-op). `state` points at
 * the per-instance block the core allocates (state_size bytes, zeroed) and
 * frees; reach it later with bd_widget_state().
 *
 * Call order on create: core defaults -> state alloc -> init -> caller's
 * attributes (so app attributes override class defaults set in init).
 *
 * Drawing (0.1): render runs inside the core's active sprite/quad batch.
 * Extensions draw textured/colored quads through bd_backend_get(); proportional
 * vector text is core-only for now.
 */
typedef struct bd_widget_class {
	const char *name;
	unsigned    state_size;

	void (*init)   (bd_id id, void *state);
	void (*destroy)(bd_id id, void *state);
	void (*render) (bd_id id, void *state);
	void (*layout) (bd_id id, void *state, int w, int h); /* content size */
	int  (*event)  (bd_id id, void *state, const bd_event *ev); /* 1=consumed */

	int   contains_children; /* 0 = core does not recurse into children */
	void *reserved;          /* future: custom-attribute hook */
} bd_widget_class;

/* Register a class; returns a fresh type id to pass to bd_create*(). */
int bd_register_widget_class(const bd_widget_class *cls);

/* Per-instance state block for an extension widget (NULL if none / invalid). */
void *bd_widget_state(bd_id id);

/* Type id of a widget (the value returned by bd_register_widget_class), or 0. */
int bd_widget_type(bd_id id);

/* Resolved on-screen rectangle of a widget (post-layout). */
void bd_widget_rect(bd_id id, int *x, int *y, int *w, int *h);

/* The renderer/window backend the toolkit was initialized with. */
const bd_backend *bd_backend_get(void);

/* va_list form of bd_create(), so extensions can wrap it in variadic helpers. */
bd_id bd_create_va(bd_id parent, int type, va_list ap);

#endif
