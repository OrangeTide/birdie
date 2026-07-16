# Widget gap analysis: a Eudora-style mail client on birdie-gui

## Purpose

Birdie's GUI toolkit grew up around a MUD client, but its widget set is meant
to be general. This note measures that set against a concrete, demanding
desktop application: a simple graphical mail client (MUA) in the mold of Eudora
Pro. The goal is not to build a mail client. It is to find out whether the
toolkit is "application-complete" for a classic three-pane desktop app, and to
name the specific widgets or capabilities still missing.

Reference: Eudora Pro (late-1990s Windows/Mac MUA). Its defining screen is a
mailbox window with a multi-column message list over an optional preview pane,
a separate mailboxes tree, and a composition window with a header block above a
body editor.

## Anatomy of the application

A Eudora-style MUA is a small number of window types built from a handful of
recurring pieces.

1. Main mailbox window
   - Menu bar and a status bar.
   - A toolbar of icon buttons (check mail, delete, reply, forward, ...).
   - A message list: a sortable, multi-column grid. Eudora's columns are
     status, priority, attachment, label, sender/recipient, date, size, and
     subject. Unread rows are bold; a color label tints the row; the status,
     priority, and attachment columns are small glyphs, not text.
   - An optional preview / reading pane below the list, split by a draggable
     sash, showing the selected message read-only.

2. Mailboxes (folders) window or pane
   - An expand/collapse tree of mailboxes with a folder icon per node and an
     unread count beside each.

3. Message composition window (one per message, so several may be open)
   - A header block: labeled fields for To, From, Subject, Cc, Bcc, plus an
     attachments strip.
   - A body editor (plain text, optionally styled).
   - Its own small toolbar (priority, signature, queue/send).

4. Supporting windows
   - Address book: a list or table of contacts plus a detail form.
   - Filters / rules editor: a list of rules plus an edit form.
   - Find, and progress feedback while checking or sending mail.

## Requirement to widget mapping

Status key: Have (works today), Partial (works with a workaround or is missing
a refinement), Missing (no toolkit support).

| Application element | Capability needed | Existing widget | Status |
| --- | --- | --- | --- |
| Three-pane layout | nestable resizable panes | `BD_SPLIT` | Have |
| Mailboxes tree | expand/collapse, per-node folder icon, keyboard nav | `BD_TREE` (icons, twisties, type-ahead) | Have |
| ...with unread counts | a right-aligned count or badge per node | fold into the label ("Inbox (3)") | Partial |
| Message list | multi-column, sortable, multi-select, scroll, keyboard | `BD_TABLE` | Have |
| ...status/priority/attach glyphs | per-cell icon, not text | `BD_TABLE` model `icon()` hook | Have |
| ...unread bold / color label | per-row text style and background tint | `BD_TABLE` model `row_style()` hook | Have |
| Reading pane | scrollable read-only text, optionally styled | locked `bd_editor` + styled runs (or `BD_TEXT_AREA`) | Have |
| ...HTML mail | an HTML layout engine | out of scope for a widget toolkit | N/A |
| Composer header fields | labeled single-line fields, a combo | `BD_TEXT_FIELD`, `BD_COMBO`, `bd_dialog_field` | Have |
| Composer body | multi-line editor with selection and clipboard | `bd_editor` (cross-line selection, copy/cut/paste) | Have |
| Attachments strip | a compact row of named icons | `bd_explorer` (list view) or a `BD_ICON` row | Have |
| Toolbar | icon buttons with hover hints | `bd_actionbar` / `BD_ICON` + `BD_TIP_S` | Have |
| Menu bar | pull-down menus | `BD_MENU` | Have |
| Context menus | right-click actions | `bd_popmenu` | Have |
| Status bar | text readouts | `BD_LABEL` | Have |
| Check/send progress | a progress bar | `bd_progress` | Have |
| Address book | a table plus a detail form | `BD_TABLE` + `bd_dialog` | Have |
| Filters / rules editor | a list plus an edit form | same pattern as the built trigger editor | Have |
| Find in message | a field plus in-text highlight and scroll-to | `BD_TEXT_FIELD` + editor `style_span` | Partial |
| Several compose windows | multiple top-level windows | multi-window backend (GLES) | Have |
| Drag an attachment in | cross-widget drag and drop | the DnD payload path | Have |

## Gaps, in priority order

### 1. Rich cells in BD_TABLE (IMPLEMENTED)

Resolved: `bd_table_model` gained an optional `icon(ctx, row, col)` accessor (a
per-cell glyph, id 0 = none) and a `row_style(ctx, row, out)` hook (bold,
foreground colour, background tint). The message list is now composable. The
rest of this section records the original analysis.

The message list is the defining widget of a mail client, and it is the one
thing the toolkit cannot render today. `BD_TABLE` is sortable, multi-select,
and scroll-clipped, but every cell is a plain string. A mail list needs:

- Per-cell icons in narrow columns: a read/unread/replied/forwarded status
  glyph, an attachment paperclip, a priority marker.
- Per-row styling: unread rows drawn bold, a color label tinting the row
  background.

Recommendation: extend `BD_TABLE`, do not build a bespoke message-list widget.
The reuse-first change is additive and benefits every tabular view (the MUD
list and the address book included):

- Add an optional per-cell adornment to the model, for example an
  `icon(ctx, row, col)` accessor returning a `bd_texture` (0 = none) drawn
  before the text, respecting the column alignment.
- Add an optional per-row style hook, for example
  `row_style(ctx, row, bd_table_row_style *out)` filling a bold flag, a
  foreground color, and a background tint.

Both are new optional fields on `bd_table_model` (or a paired struct), so they
cost nothing for existing callers. The 0.9 breaking-change window is open, so
this can land cleanly. This is the single enhancement that unblocks the MUA.

### 2. Tree secondary text or count badge (minor)

An unread count reads best right-aligned and dimmed, separate from the folder
name. Today the app must bake it into the label. A `bd_tree_item.detail`
string (drawn right-aligned) would be a small, general improvement. Not a
blocker: "Inbox (3)" in the label works now.

### 3. In-message find (minor)

The editor already supports transient highlight via `style_span`, so an app can
color matches itself, but there is no built-in find that scrolls the first
match into view. A small `bd_editor_find` / reveal helper would remove
boilerplate. Not a blocker.

### 4. Reading-pane refinements (optional)

Read-only styled text works via the locked editor. Quoted-text coloring and URL
detection are app-level uses of styled runs. HTML rendering is deliberately out
of scope: it needs a layout engine, not a widget, and belongs to the
application if it is ever wanted.

## Minimal build path for a skeleton

Everything structural exists. A working three-pane shell is pure composition:

1. A vertical `BD_SPLIT`: left pane a `BD_TREE` of mailboxes; right pane a
   horizontal `BD_SPLIT` whose top is the message `BD_TABLE` and whose bottom
   is a locked `bd_editor` reading pane.
2. A `BD_MENU` menu bar, a `bd_actionbar` toolbar, and `BD_LABEL` status text.
3. Compose as a new top-level window: a panel of labeled `BD_TEXT_FIELD`
   header rows above a `bd_editor` body.
4. Right-click menus via `bd_popmenu`; send/receive progress via `bd_progress`.

The only piece that cannot be composed from what exists is the rich message
list, which needs gap 1.

## Verdict

The toolkit is close to application-complete for a classic desktop MUA.
Every structural and interactive piece a three-pane mail client needs already
exists: nested splits, a tree with icons, a sortable multi-select table, a
rich-text editor with selection and clipboard, dialogs and form controls,
toolbars, menus, context menus, multiple native windows, drag and drop, and
tooltips. The one substantive gap was **rich cells in `BD_TABLE` (per-cell
icons and per-row styling)**, and it is now implemented (`icon()` +
`row_style()` model hooks). The message list, the defining widget of a mail
client, is now composable from the existing widgets. Everything else on the
list is composition or minor polish.

<!-- Made by a machine. PUBLIC DOMAIN (CC0-1.0) -->
