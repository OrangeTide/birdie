# birdie-gui Dialogs: Plan

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

A plan to make dialogs a first-class, low-friction part of birdie-gui, so the
app's profile / trigger / settings / import / export dialogs are quick to build
and consistent. It is grounded in what already ships: the modal plumbing exists,
what is missing is the form controls, a composition helper, and a few ergonomics.

## 0. What already works

- **`bd_modal_open(dialog)` / `bd_modal_close(dialog)` / `bd_modal_active()`**
  (`widget.c`). A detached widget subtree (usually a `BD_PANEL`) is shown centered
  over a dimmed backdrop, above the main UI and below a `bd_notice` alert. Input
  routes into it as in a frame; everything outside is swallowed; **Escape closes**.
- **`bd_notice_open(message, buttons, cb, arg)`** — a modal alert with a stacked
  button list and a `(notice, button, arg)` callback. The one-shot prompt case.
- **The app already builds dialogs by hand** (`src/birdie/main.c`): the connect
  picker (a `BD_TABLE` + button row) and the add/edit-profile form (labels +
  `BD_INPUT_LINE` + a TLS toggle), each a detached `BD_PANEL` passed to
  `bd_modal_open`.

So the modal layer is solid. The friction is that every dialog is assembled by
hand, and the form controls real dialogs need do not exist yet.

## 1. Gaps

| Gap | Impact |
|-----|--------|
| No combo / checkbox / radio / spinner widgets | Blocks the trigger editor, settings, export column-filter, and import-collision dialogs (see `TODO.md`, `doc/profiles.md`). |
| No dialog-composition helper | Each dialog hand-wires title + content + button row + padding; `main.c` repeats this. |
| No default button (Enter) or initial focus in the modal layer | Only Escape is handled; the opener must focus a field manually; Enter does not confirm. |
| No standard result convention | Each dialog invents its own OK/Cancel callbacks and value-gathering. |

## 2. Decisions

| Decision | Choice |
|----------|--------|
| Where controls live | **Extension widgets** (`widget_ext.h`), like the value widgets, not core. They compose over the existing event/focus/popup machinery. |
| Create-call shape | **Descriptor structs** (`bd_checkbox_desc`, `bd_combo_desc`, ...) with `on_*` change callbacks and `_get`/`_set`, matching the 0.9 value-widget convention. Do them **in the 0.9 window** so the API breaks land together. |
| Combo / chooser popups | Reuse the existing popup path (the editor's autocomplete popup and `BD_LIST` already do overlay + keyboard nav); a combo is a labeled button that opens a `BD_LIST` popup. |
| Dialog helper weight | **Lightweight helpers + conventions**, not a monolithic dialog class. A `bd_dialog` shell that owns title + content slot + button row, plus a labeled-field-row helper. Callers still add their own controls. |
| Layout constraint | Every content container needs an explicit `PREF_H`/`PREF_W` or `GROW`; the layout never measures contents. Bake this into the field-row helper so callers do not trip on it. |

## 3. Build order

**Status:** Phases 0, 1 and 2 are done. Phase 4 has landed the connect and
edit-profile reworks, the app-wide Settings dialog, the per-profile
autoreconnect/term-type fields, and the live trigger editor; its remaining
net-new dialogs (export column-filter, import-collision) and Phase 3 (choosers)
are still open.

### Phase 0 — modal ergonomics (small, in core `widget.c`) — DONE

Extend the modal layer, not replace it:

- **Default action on Enter.** `bd_modal_open_ex(dialog, { on_accept, on_cancel, focus })` (or attributes on the dialog): Enter fires `on_accept`, Escape fires `on_cancel` then closes. Keep `bd_modal_open` as the zero-config form.
- **Initial focus.** Focus `focus` if given, else the first focusable descendant, so the user can type immediately.
- No change to dimming / centering / routing.

### Phase 1 — form controls (the blocking gap; extension widgets) — DONE

Shipped as `bd_widget_form.{c,h}` (checkbox, radio, spinner) and
`bd_widget_combo.{c,h}` (the drop-down, separate because it opens a popup):

1. **`BD_CHECKBOX`** — a distinct box+tick widget (not a restyled toggle: a
   toggle reads as a switch, a form wants a checkbox). Toggles on click / Space.
2. **Radio group** — one widget owning the mutually-exclusive set (so it is a
   single Tab stop); click or arrow-key to move the selection.
3. **`BD_COMBO` / drop-down** — a closed box with a chevron that opens a floating
   list through the new shared overlay primitive (`bd_overlay_*`, see below), so
   the list floats above siblings and above a dialog the combo sits in.
4. **Numeric spinner / stepper** — an integer field with up/down steppers,
   min/max/step, arrow-key steps, and digit entry.

Each ships with a `bd_*_desc` create, `_get`/`_set`, an `on_change` callback,
keyboard support, a form-row-friendly preferred size, and headless checks in
`test_gui`.

**Shared overlay primitive.** The combo needed a top-most pop-up that escapes
its own widget rect, so Phase 1 also added `bd_overlay_open/close/owner`
(`widget_ext.h`): one overlay at a time, owned by an extension widget, drawn
above every frame / modal / notice and given first crack at input (a press
outside or an unconsumed Escape dismisses it). This is the convergence target
noted in Scope below; the editor autocomplete can move onto it later.

### Phase 2 — dialog composition helper — DONE

Shipped as `bd_dialog.{c,h}`, a thin shell over `bd_modal`:

- `bd_dialog_create(title, w, h)` -> a titled panel with a content column
  (standard pad/gap) and a right-aligned button row.
- `bd_dialog_button(dlg, label, role, cb, arg)` where `role`
  (`BD_DIALOG_DEFAULT` / `BD_DIALOG_CANCEL` / `BD_DIALOG_NORMAL`) wires Enter to
  the default button and Escape to the cancel button. A cancel button also
  closes on click; the default does not (its callback validates, then closes).
- `bd_dialog_field(dlg, label)` — a labeled row with the required `PREF_H`
  already set; the caller creates the control INTO the returned row with
  `BD_GROW_I`. It returns the row rather than taking a pre-built control because
  the toolkit does not reparent.
- `bd_dialog_content(dlg)` hands back the content column for arbitrary widgets.
- `bd_dialog_open` / `bd_dialog_close` wrap the modal calls (open focuses the
  first field); `bd_dialog_free` destroys the subtree and the handle.

Convenience, not a new layout engine: callers still add arbitrary widgets to the
content container. Covered in `test_gui` (Enter/Escape wiring, click roles,
first-field focus) and demoed by the gallery's "Dialog..." button.

### Phase 3 — composite chooser dialogs

Built from the above, no new backend capability:

- **File chooser** — a `BD_TREE` or list of entries + a path field + Open/Cancel.
  For import CSV and the `on_connect` script path.
- **Color chooser** — a palette grid plus an X-Y pad + value slider and a live
  swatch. For `#highlight` color and theme editing.

### Phase 4 — wire the app dialogs (`src/birdie/`) — IN PROGRESS

- [x] Rework the existing **connect** and **edit-profile** dialogs onto the
  shell (`main.c`). The connect picker now composes its table + manage/import
  rows into `bd_dialog_content` with a Connect (default) / Cancel button row;
  the edit form uses `bd_dialog_field` rows (Name/Host/Port inputs, a TLS
  `BD_CHECKBOX` replacing the toggle) with Save (default) / Cancel. Both gain
  Enter-confirms / Escape-cancels and first-field focus for free, and the
  hand-rolled panel/label/button boilerplate is gone (-30 lines net).
- [x] **Settings** — an app-wide preferences dialog (Edit > Settings...): terminal
  grid size (Columns/Rows spinners, applied over NAWS via `bd_session_set_winsize`)
  and a colour-scheme combo (Default / Green phosphor / Amber, applied via
  `bd_terminal_set_palette`), persisted to a new `<data_dir>/settings.csv`
  (flat key,value) loaded at startup. Birdie has little app-wide config (most is
  per-profile), so this is deliberately small; every control has a live effect.
- [x] Per-profile fields on the **edit-profile** dialog: an autoreconnect checkbox
  and a term-type field (both read by `bd_session` at connect). `encoding` was
  left out: it is a safe profile column but is not wired to any behavior yet
  (needs CHARSET/NEW_ENVIRON telopt work), and a dead control is worse than none.
- [x] **Trigger editor** (Session > Triggers...): a `BD_TABLE` of the session's
  triggers plus a compact add form (type combo, pattern/body/class inputs, a
  priority spinner, a stop checkbox) and Remove-selected. It edits the **live**
  session trigger table through the `bd_trigger_*` C API (add /
  remove_pattern / foreach), so it works offline (the engine exists before
  connect) and mirrors the `#action` verbs. It deliberately does **not** persist:
  triggers live in a user-editable `triggers.lua`, and rewriting that from the
  flat table would clobber hand-written Lua. Persisting would need a separate
  per-profile trigger data file plus session load wiring (a future task).
- [ ] **Export** — per-column checkboxes (`doc/profiles.md`).
- **Import-collision** — a radio group (skip / rename / overwrite).

## 4. Testing

- Each new control gets headless `test_gui` checks (construct, synthesize a
  click/key, assert value + callback), like the existing value-widget tests.
- The modal ergonomics and the composition helper are headless-testable too:
  open a dialog, synthesize Enter/Escape, assert the accept/cancel callback and
  that focus landed on the first field.
- The app dialogs, once wired, are exercised through the session/UI the same way
  the gallery exercises widgets.

## 5. Scope notes

- All of Phase 1 is API-additive except any toggle restyle; land the new
  descriptor-based controls **within 0.9** so their signatures settle with the
  rest of the batched API changes.
- `bd_notice` stays the answer for one-shot yes/no prompts; the new shell is for
  multi-field forms. They share the modal layer.
- The combo popup and the editor autocomplete popup should converge on one
  overlay/keyboard-list primitive rather than each rolling their own.
