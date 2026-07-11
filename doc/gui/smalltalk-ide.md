# Widgets for a Smalltalk-style IDE

*Design note answering TODO.md: "what UI elements and composite widgets are
needed to implement a Smalltalk-like class browser" and "every widget necessary
to implement a Smalltalk-like development environment (IDE)."*

This is a **gap analysis**, not an implementation. It decomposes the classic
Smalltalk-80 environment into the primitive widgets it rests on, maps each onto
what birdie-gui already ships, and names the primitives and composites still
missing. It is deliberately backend-neutral: everything here is retained-mode,
model-driven, and drawn through `bd_backend`, consistent with the rest of the
toolkit.

Why Smalltalk is a good target: its whole environment is built from a handful of
list panes, an editor, and resizable dividers, wired together by *models*. If
birdie-gui can host a System Browser it can host almost any tool IDE (a MUD
trigger editor, a scripting console, a settings inspector), which is the real
motivation.

## The tools, and the primitives under each

The Smalltalk-80 "IDE" is a small set of tools, each a fixed arrangement of the
same few widgets over a live model.

| Tool | What it is | Primitive widgets it needs |
|------|------------|----------------------------|
| **System Browser** (class browser) | 5-pane editor of the class library | 4 scrolling **lists**, a 2-way **radio/segment** (instance/class), a **code editor**, 3 vertical + 1 horizontal **splitter**, **context menus** |
| **Workspace** | scratch pane you evaluate text in | **code editor** (multiline), context menu |
| **Transcript** | append-only log / stdout | read-only **terminal** or multiline, **scrollbar** |
| **Inspector** | open an object: fields on the left, value on the right | **list** + a value pane (editor / nested inspector), 1 **splitter** |
| **Debugger** | call stack over live contexts | a **list** (stack), two inspector panes, a **code editor**, splitters |
| **Hierarchy Browser** | superclass/subclass chain of one class | **tree** (or indented list) + code editor |
| **Senders / Implementors / References** | a message set: every method matching a query | a **list** of method refs + code editor |
| **Change Sorter** (nice-to-have) | pending changes, accept/revert | two **lists** + code editor |

Every one of these is *lists + editor + splitters + menus over a model*. Nail
those primitives and the tools are mostly composition.

## Inventory: have / partial / missing

| Need | birdie-gui today | Status |
|------|------------------|--------|
| Frame / panel / label / button | core chrome | **have** |
| Scrolling selectable list | `BD_LIST` | **have** |
| Multi-column list / grid (method list with flags, senders) | `BD_TABLE` (sortable columns) | **have** |
| Code editor with styled runs | `bd_widget_editor` (`bd_editor_style_span` / `_highlight_row`) | **have** (styling hooks exist; see gaps) |
| Scratch/workspace pane | `BD_TEXT_AREA` / editor | **have** |
| Transcript / log pane | `BD_TERMINAL` (or read-only multiline) | **have** |
| Scrollbar | `BD_SCROLLBAR` | **have** |
| Tabbed tool container | `BD_TAB_VIEW` | **have** |
| Floating / minimizable tool windows + an MDI desktop | `BD_MANAGED_CANVAS` + `BD_DOCK` | **have** |
| Context ("yellow button") menus | `BD_MENU` + pushpins | **have** (menus are pull-down/pinnable; a right-click/middle-click *context* menu at the pointer is a thin wrapper still to add) |
| Modal dialogs / confirmers | `BD_NOTICE`, modal API | **have** |
| Icon grid (method categories as icons, tool launcher) | `bd_widget_explorer`, `bd_widget_inventory` | **have** |
| **Resizable split panes (sash)** | — | **missing** — foundational |
| **Tree view (indented, expand/collapse)** | `bd_widget_tree` (`BD_TREE`) | **have** |
| **Radio group / segmented control** (instance \| class) | `bd_toggle` is 2-state on/off, not an N-way exclusive selector | **missing** |
| Editor: line-number gutter | — | **missing** |
| Editor: syntax highlighting *driver* | style-span mechanism exists; no tokenizer hook / re-highlight-on-edit | **partial** |
| Editor: autocomplete popup | — | **missing** (see TODO: "wire up autocomplete to an editor") |
| Editor: find / replace bar | — | **missing** |
| List: type-ahead find | — | **missing** (small add to `BD_LIST`) |

## Missing primitive widgets (the deliverable list)

In dependency order. The first three unlock every tool above.

1. **`BD_SPLIT` — split pane / sash.** A container of two (or N) children with a
   draggable divider that repartitions them, horizontal or vertical, with a
   minimum size per pane and an optional collapse-to-zero. *Foundational*: the
   System Browser, Inspector, and Debugger are all just nested splits. Proposed
   shape: `bd_split_create(parent, BD_HORIZONTAL|BD_VERTICAL, ...)` then add
   children; store the split ratio(s) as widget state, drag the sash to adjust,
   re-snap on resize. Nestable, so a browser is a vertical split (panes over
   code) whose top is a horizontal split of four lists.

2. **`BD_TREE` — indented hierarchy list.** *Built* (`bd_widget_tree.{c,h}`). A
   `BD_LIST` cousin that renders an indented, expand/collapse outline over a
   model that yields children on demand (`child_count` / `child` / `get` with
   `label`, `has_children`, `enabled`, `user`; nodes keyed by an app
   `uint64_t`). Drives the class Hierarchy Browser, the project/file tree (also
   a separate TODO), and any nested structure. Owns scrolling, selection,
   keyboard (up/down, left/right collapse-expand, type-ahead), the twisty
   toggles, and the expand state (the widget owns it; seed with
   `bd_tree_set_expanded`, react via the `expand` callback for lazy loading);
   select / activate / expand callbacks.

3. **`BD_RADIO` / segmented control.** An N-way exclusive selector for the
   browser's `instance | class` toggle (and message-category filters). Either a
   classic radio column or a segmented button strip; the segmented form reuses
   button chrome and fits the skeuomorphic aim. Small: it is a row of buttons
   with one-hot selection state and a change callback.

4. **Editor enhancements** (extend `bd_widget_editor`, do not fork it):
   - a **line-number gutter** (toggle; the styling machinery already positions
     rows, so this is a fixed left margin the editor paints);
   - a **syntax-highlight hook**: a per-widget tokenizer callback the editor
     calls on edited rows to emit style spans, so highlighting stays current
     instead of being a one-shot `bd_editor_style_span` at load;
   - an **autocomplete popup** (shared with the standalone autocomplete TODO): a
     floating `BD_LIST` at the caret, fed by a completion callback, Tab/Enter to
     accept;
   - a **find / replace bar**: a thin input strip the editor can host, with
     next/prev and match highlighting via existing `_highlight_span`.

5. **`BD_LIST` type-ahead** (minor): typing while a list is focused jumps to the
   first matching row. The browser panes are long; this is expected behavior.

## `BD_COLUMN_BROWSER` — the pane-container primitive

The reusable engine under the class browser is the **NeXTSTEP column browser**
(`NSBrowser`, a.k.a. Miller columns; still Finder's column view): a horizontal
strip of panes where selecting an item in pane *N* fills pane *N+1* to its
right, navigating a hierarchy left→right, with a wider trailing **leaf** pane
(a preview, or here the code editor). This is *not* Smalltalk-specific — it is a
generic navigation widget, so it gets its own primitive.

It is a **pane container: each pane is any widget the host adds**, not a fixed
list type. A pane is usually a `BD_LIST`, but it can be a `BD_TABLE` (a method
list with flag columns), a `BD_TREE`, or a custom widget; the leaf is typically
the `bd_widget_editor`. The browser owns only what is generic across pane types:

- **Layout** — equal-width columns with horizontal scroll when they overflow
  (NeXT behavior), or fixed panes with `BD_SPLIT` sashes (Smalltalk behavior);
  the leaf spans the trailing width.
- **Cascade wiring** — a pane emits a *selection-changed → key* signal (both
  `BD_LIST` and `BD_TABLE` can); the browser clears every downstream pane and
  calls the host to refill pane *N+1*, in order, so one click ripples right.

Sketch (the pane-container API the answer picked):

```c
bd_id cb = bd_column_browser_create(pane, BD_GROW_I, 1, BD_END);
bd_column_browser_add(cb, categories_list);   /* pane 0 -- any widget */
bd_column_browser_add(cb, classes_list);      /* pane 1 */
bd_column_browser_add(cb, protocols_list);    /* pane 2 */
bd_column_browser_add(cb, methods_table);     /* pane 3 -- e.g. a BD_TABLE */
bd_column_browser_leaf(cb, source_editor);    /* trailing leaf pane */
/* host fills pane i+1 from the key selected in pane i: */
bd_column_browser_on_select(cb, on_column_select, ctx);
```

Because panes are host-owned widgets, the browser never needs to know class
semantics; the *fields that show and edit the hierarchy and names* — the lists,
a rename `BD_INPUT_LINE`, the `instance|class` `BD_RADIO`, the code editor — are
all ordinary widgets the host places into (or beside) the columns.

## Composite widgets to build on the primitives

Fixed arrangements over a shared model, packaged so a host instantiates one call.

- **The class browser (`bd_class_browser`) — the full System Browser, turnkey.**
  Built on `BD_COLUMN_BROWSER`: a vertical `BD_SPLIT` whose top is the column
  browser with four `BD_LIST` panes (class category, class, method protocol,
  method) plus the `instance|class` `BD_RADIO` under the class column and a
  rename `BD_INPUT_LINE`, and whose bottom (the leaf) is the code editor. The
  composite bakes in the wiring: selecting a row in pane *N* narrows pane *N+1*
  (`category -> classes -> protocols -> methods -> source`), the editor shows the
  selected method's source, and Accept compiles it back. The host supplies one
  **class-library model** — `categories()`, `classes(cat)`, `protocols(cls,
  meta)`, `methods(cls, proto, meta)`, `source(cls, sel)`, `compile(cls, text,
  meta)`, plus rename hooks — and gets the whole tool from one
  `bd_class_browser_create(pane, &model, ...)`. This is the payoff of the
  primitives above.

- **Inspector composite.** A horizontal `BD_SPLIT`: a `BD_LIST` of the object's
  fields on the left, a value pane on the right (a read-only editor, or another
  inspector for drill-down). Model: `field_count()`, `field_name(i)`,
  `field_value(i)`, `inspect(i)`. (A two-pane inspector is a degenerate column
  browser, so it can also be a `BD_COLUMN_BROWSER` with one column + leaf.)

- **Debugger composite.** A `BD_SPLIT` stack: a `BD_LIST` of stack frames at the
  top, two inspector panes for receiver/context in the middle, a code editor
  showing the failing method at the bottom, with a proceed/restart/step button
  row. Reuses the inspector composite twice.

- **Message-set browser** (Senders/Implementors/References): a `BD_LIST` (or
  `BD_TABLE`) of method references over a code editor, in a small `BD_SPLIT`. A
  degenerate `BD_COLUMN_BROWSER` with one pane + editor leaf.

## Build order and cross-references

1. `BD_SPLIT` — unblocks every tool; also broadly useful outside the IDE.
2. `BD_TREE` — **built** (`bd_widget_tree`). The Hierarchy Browser and the
   project tree (TODO: "tree browser for projects") share it.
3. `BD_RADIO` / segmented control — small, needed by the browser.
4. Editor autocomplete + syntax-highlight hook + find bar — shared with the TODO
   items "wire up autocomplete to an editor" and the IDE-on-an-editor note.
5. `BD_COLUMN_BROWSER` (the generic pane container), then the `bd_class_browser`,
   Inspector, and Debugger composites on top of it.

Related TODO.md items that feed the same effort: the tree browser for projects,
autocomplete wiring, and the `BD_ICON` unification of dock/actionbar slots (a
tool launcher of IDE tools would use those icons). None of the missing items
requires new backend capability: `BD_SPLIT` is layout + a draggable sash,
`BD_TREE` is `BD_LIST` with indentation and a child model, and
`BD_COLUMN_BROWSER` plus the composites are pure composition over `widget_ext`.

*Made by a machine. PUBLIC DOMAIN (CC0-1.0)*
