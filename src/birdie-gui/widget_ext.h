#ifndef BD_WIDGET_EXT_H
#define BD_WIDGET_EXT_H

#include "widget.h"
#include "bd_backend.h"
#include <stdarg.h>

/*
 * Extension API -- the second consumer tier of the toolkit. Where widget.h is
 * for apps that *use* widgets, this header is for code that *defines new kinds
 * of widgets*, the way the VT terminal does. An extension fills a
 * bd_widget_class, registers it to obtain a type id, then builds its own
 * typed create/accessor functions on top of the core create calls.
 *
 * Ownership conventions (uniform across the extension widgets; a widget's own
 * header notes any exception):
 *
 *   - Descriptor / model structs (bd_*_desc, bd_*_model, bd_*_cb) are read and
 *     copied field-by-field at the create call. The struct itself is never
 *     retained, so it may be a stack temporary or compound literal:
 *         bd_knob_create(p, &(bd_knob_desc){ .value = 0.5f, .cb = f });
 *
 *   - Strings in a descriptor (labels, captions) are BORROWED by default: the
 *     widget stores the pointer, so the string must outlive the widget. This
 *     matches the core BD_LABEL_S attribute. Exceptions that COPY into owned
 *     storage (safe to pass a temporary) are noted in their headers: BD_ICON,
 *     bd_actionbar, and bd_chart series labels.
 *
 *   - Model-driven widgets (tree, explorer, inventory, dock, table) re-walk the
 *     model on every layout and never cache per-item strings, so an item label
 *     returned from model.get()/cell() is borrowed only for that call and may be
 *     a shared scratch buffer the caller overwrites next call.
 *
 *   - Two distinct context pointers: a model/callback `ctx` is one widget-wide
 *     pointer passed to every model and callback invocation; a per-item `user`
 *     field is item-specific data echoed back on activate/select. Both are
 *     borrowed (the widget stores, never frees them).
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Capability flags for a widget class (bd_widget_class.flags). Behavioral
 * toggles, OR'd together. Keep them additive across versions (assign new bits;
 * never repurpose an old one) so a class can compose a default set from a macro
 * and pick up later additions on recompile.
 */
typedef uint64_t bd_widget_flags;
enum {
	/* core recurses into the widget's children (lays them out / renders /
	 * routes events); the default (bit clear) treats the widget as a leaf. */
	BD_WC_CONTAINS_CHILDREN = 1ull << 0,

	/* also deliver BD_EV_MOUSE_MOVE while the widget is merely hovered, not
	 * only during a captured drag, so it can do hover effects / tooltips. The
	 * move whose coords leave the widget is delivered too, so it can clear the
	 * hover. */
	BD_WC_WANTS_HOVER       = 1ull << 1,
};

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

	bd_widget_flags flags;   /* BD_WC_* capability bits, OR'd together */
	void           *reserved;/* future: custom-attribute hook */
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

/* Monotonic time in seconds. Uses the backend's time() hook when it supplies
 * one, else the toolkit's own monotonic clock. Extensions should call this
 * rather than bd_backend_get()->time() so a NULL time hook is handled. */
double bd_time(void);

/* va_list form of bd_create(), so extensions can wrap it in variadic helpers. */
bd_id bd_create_va(bd_id parent, int type, va_list ap);

/*
 * Cross-widget drag-and-drop.
 *
 * The toolkit's pointer capture keeps a drag glued to its source widget, so the
 * source never sees the release land on another widget. This facility bridges
 * that gap: a source widget (inventory, dock, ...) that has grabbed the pointer
 * calls bd_dnd_begin() once the drag starts to advertise what is being carried.
 * The toolkit then draws a ghost tile trailing the pointer and, when the button
 * is released over a *different* extension widget, synthesizes a BD_EV_DROP
 * event to that target's event() hook. The target reads the payload with
 * bd_dnd_get() and returns 1 to accept the drop.
 *
 * The payload is copied into the toolkit (label bytes included, up to a bounded
 * length), so the source may pass borrowed strings. A drag ends when the button
 * is released; there is no separate cancel (a release over the source or empty
 * space just discards it).
 */
typedef struct bd_dnd_payload {
	bd_id       source;   /* widget the drag started from */
	uint64_t    key;      /* app identity of the dragged item */
	const char *label;    /* caption shown on the ghost (copied) */
	bd_texture  icon;     /* ghost image; id 0 = a plain marker */
	void       *user;     /* opaque app payload, passed through unchanged */
} bd_dnd_payload;

/* Source: begin (or refresh) the active drag with this payload. No-op unless a
 * pointer drag is in flight (the toolkit ties the drag's lifetime to it). */
void bd_dnd_begin(const bd_dnd_payload *p);

/* The payload of the drag in flight, or NULL when none. A drop target queries
 * this from its event() hook on a BD_EV_DROP; a source may query it to suppress
 * its own in-widget ghost while the toolkit draws the shared one. */
const bd_dnd_payload *bd_dnd_get(void);

/*
 * Top-most overlay -- the shared pop-up primitive (drop-down lists, and in time
 * the editor's autocomplete). One overlay is open at a time, owned by an
 * extension widget. While open the toolkit:
 *   - draws it last, above every frame / modal / notice (below only the drag
 *     ghost), by calling its render() hook;
 *   - routes input to it first: pointer events inside its rect and all key/char
 *     events go to its event() hook; a press outside dismisses it; Escape the
 *     event() hook does not consume dismisses it.
 * The rect is in the primary window's coordinate space (the same space a
 * widget's bd_widget_rect reports). Opening a second overlay replaces the first.
 */
typedef struct bd_overlay {
	bd_id owner;                                /* widget that owns it */
	int   x, y, w, h;                           /* screen rect */
	void (*render)(bd_id owner);                /* paint contents (top-most) */
	int  (*event) (bd_id owner, const bd_event *ev); /* 1 = consumed */
} bd_overlay;

void  bd_overlay_open(const bd_overlay *ov); /* open/replace (owner must be set) */
void  bd_overlay_close(bd_id owner);         /* no-op unless owner matches */
bd_id bd_overlay_owner(void);                /* BD_NONE when none is open */

#endif
