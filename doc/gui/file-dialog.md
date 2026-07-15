# birdie-gui File Dialog: Plan

Made by a machine. PUBLIC DOMAIN (CC0-1.0)

A plan for a first-class file dialog in birdie-gui: a widget for opening, saving,
and selecting files and folders that looks and works like the rest of the
toolkit, while hooking into the host operating system for known folders, volumes,
and recently-used files. X11/Linux and Windows are the initial targets; macOS,
Android, and iOS are stubbed behind the same platform seam.

It replaces the ad-hoc `dirent` chooser in `src/birdie/main.c` (`open_file_chooser`,
around line 1154), which is a single `BD_LIST` with a name field and no filters,
sorting, places, or save mode.

## 0. Reference reviewed

The design borrows the model/UI split from `rswinkle/file_browser`
(`file_browser.h`). That library is a UI-agnostic model: a `file_browser` struct
holds the current directory, a vector of `file` entries, a selection index, a
sort state, an extension filter, a search buffer, and hidden/select-dir toggles.
The host UI (its Nuklear and terminal front ends) renders that state and feeds
input back. Platform coupling is confined to `fb_scandir` (dirent), `get_homedir`,
and the `home`/`desktop` known paths. Recents are delegated to a host callback.

We keep the model/UI split. We diverge where the reference is limiting:

- It stores size as `int`, which breaks past 2 GB, and uses fixed `MAX_PATH_LEN`
  buffers. We use 64-bit sizes and treat paths carefully against `PATH_MAX`.
- Filenames pass through `bd_utf8` (already in the toolkit). This matters on
  Windows, where the native filesystem API is UTF-16.
- It renders through an immediate-mode GUI. We compose from birdie's own
  retained-mode widgets so the dialog matches the surrounding chrome.

## 1. Decisions

| Decision | Choice |
|----------|--------|
| Model vs. UI | A pure-C model core (`bd_fs`) with no widget dependency, rendered by a composed dialog (`bd_filedlg`). Mirrors the reference's split so the model is headless-testable. |
| File view widget | **`BD_TABLE`**, with sortable Name / Size / Modified columns. Closest to a native detail view and to the reference's column sort; the table is already sortable. |
| Favorites / recents | **OS recents only.** No birdie-managed bookmark store. The places sidebar surfaces OS known folders, volumes, and the OS recently-used list. On accept, the file is registered with the OS recent-documents facility so it appears next time (and in native dialogs). |
| Platform coupling | A narrow vtable inside `bd_fs`: scandir, stat, known folders, volume enumeration, path normalize, recents source. POSIX and Win32 implementations; stubs for macOS/Android/iOS. |
| Native dialogs | Not used by default. The request is a consistent birdie-gui look. A per-platform escape hatch to a true native dialog is deferred and off by default. |
| Where it lives | `src/birdie-gui/bd_fs.{c,h}` and `bd_filedlg.{c,h}`. Both depend only on `birdie_gui` (via `bd_dialog`), stay host-neutral, and ship in the bundle. The Win32 backend is guarded so `make windows-check` cross-compiles it. |

## 2. Layers

### Layer 1: `bd_fs` model core (no widget deps)

The birdie analog of `file_browser.h`. Holds the current directory, an entry
array, selection, sort state, the active extension filter, a search buffer, the
hidden and mode toggles, and cached place lists (known folders, volumes, OS
recents). Pure C, exercised by a headless test in the style of `test_client` /
`test_session`.

```c
typedef struct bd_fs_entry {
    char     *name;        /* basename, UTF-8, owned */
    int64_t   size;        /* bytes; 64-bit */
    int64_t   mtime;       /* seconds since epoch */
    int       is_dir;
    int       is_link;
} bd_fs_entry;

enum { BD_FS_NAME, BD_FS_SIZE, BD_FS_MTIME };   /* sort key */

bd_fs   *bd_fs_open(const char *start_dir);
void     bd_fs_chdir(bd_fs *fs, const char *dir);
void     bd_fs_up(bd_fs *fs);
void     bd_fs_refresh(bd_fs *fs);
void     bd_fs_sort(bd_fs *fs, int key, int descending);
void     bd_fs_set_filter(bd_fs *fs, const char *const *patterns, int n);
void     bd_fs_search(bd_fs *fs, const char *text);
void     bd_fs_set_hidden(bd_fs *fs, int show);
int      bd_fs_count(bd_fs *fs);
const bd_fs_entry *bd_fs_get(bd_fs *fs, int index);
void     bd_fs_free(bd_fs *fs);
```

### Layer 2: platform vtable (inside `bd_fs`)

A small seam so the OS-specific bits are the only thing that changes per platform.

```c
typedef struct bd_fs_platform {
    int  (*scandir)(const char *dir, bd_fs_entry **out, int *n);
    int  (*known_folder)(int which, char *out, size_t cap); /* home/desktop/docs/downloads */
    int  (*volumes)(bd_fs_entry **out, int *n);             /* mounts / drive letters */
    void (*normalize)(char *path);
    int  (*recents)(bd_fs_entry **out, int *n);             /* OS recently-used list */
    void (*add_recent)(const char *path);                   /* register on accept */
} bd_fs_platform;
```

- **Unix / X11** (the POSIX branch of `bd_fs.c`, targeting Linux and the BSDs
  alike): built on portable POSIX and freedesktop.org interfaces, not on any one
  OS. `opendir`/`readdir`/`lstat` for entries. Known folders come from the
  freedesktop XDG user-dirs file (`user-dirs.dirs`, so `Desktop`/`Documents`/etc.
  are the user's localized, configured paths), with a conventional-name fallback.
  Recents use the freedesktop `recently-used.xbel` list under `$XDG_DATA_HOME`.
  The one genuinely OS-divergent piece, enumerating mounts, sits behind a small
  `each_mount` seam: `getmntent` on Linux, `getmntinfo` on FreeBSD / DragonFly /
  macOS, and a graceful "root only" fallback elsewhere. A cross-platform filter
  surfaces real devices mounted under `/media`, `/run/media`, `/mnt`, or
  `/Volumes`. Truly identifying removable media is a UDisks2/dbus (or GIO) job,
  deferred on purpose so the core needs no dbus dependency.
- **Win32** (`bd_fs_win32.c`): `FindFirstFileW`; `SHGetKnownFolderPath`;
  `GetLogicalDrives` for drive letters; the Recent Items folder plus
  `SHAddToRecentDocs` for `add_recent`. UTF-16 converts to and from UTF-8 at the
  boundary via `bd_utf8`. Guarded by `#ifdef _WIN32` so it compiles under the
  mingw cross-build.
- **macOS / Android / iOS**: stub implementations that return the home directory
  and fail the rest gracefully, so the widget is usable everywhere and each OS is
  a later drop-in.

### Layer 3: `bd_filedlg` widget

Composed with the `bd_dialog` helper and filled with existing widgets:

- **File view**: `BD_TABLE` with Name / Size / Modified columns, sorted by column
  header, driven by a `bd_table_model` that reads `bd_fs`.
- **Places sidebar**: a `BD_LIST` of known folders, a single **Recent**
  shortcut, and volumes. A single click navigates (the list's `BD_ON_CHANGE_F`
  selection callback, added for sidebars): a folder or volume does a
  `bd_fs_chdir`; Recent switches the model to the recents view
  (`bd_fs_show_recents`) so the recent files list in the main table (with their
  own paths), rather than cluttering the sidebar as individual rows.
- **Path bar**: a `BD_INPUT_LINE` for typed paths. Breadcrumb buttons can come
  later.
- **Filter**: a `BD_COMBO` of extension patterns, for example "All files" and
  "CSV (*.csv)".
- **Filename**: a `BD_INPUT_LINE` for the selected or typed name.
- **Toolbar and buttons**: `bd_dialog_button` for Open / Save / Cancel, plus Up,
  New Folder, and a "Show hidden" `BD_CHECKBOX`.
- Colors come from `bd_gui_theme()`. File-type glyphs are drawn tinted like the
  existing pushpin and padlock (folder, file, link to start), with room to grow
  through `bd_asset` later.

Consumer API, which replaces `open_file_chooser`:

```c
enum { BD_FILEDLG_OPEN, BD_FILEDLG_SAVE, BD_FILEDLG_DIR };

/* A filter entry: a label and a ";"-separated glob list ("*.png;*.jpg"). */
typedef struct bd_filedlg_filter {
    const char *label;
    const char *patterns;
} bd_filedlg_filter;

typedef struct bd_filedlg_opts {
    const char              *title;
    int                      mode;
    const char              *start_dir;
    const char              *default_name;    /* SAVE */
    const bd_filedlg_filter *filters;         /* borrowed array; NULL for none */
    int                      nfilters;
    void                   (*on_accept)(const char *path, void *user);
    void                   (*on_cancel)(void *user);
    void                    *user;
} bd_filedlg_opts;

void bd_filedlg_open(const bd_filedlg_opts *opts);
```

One implementation note worth recording: `bd_dialog`'s click handler touches the
dialog after a button callback returns, so a chooser instance never frees itself
inside a callback. Each open tears down the previously retired instance, and a
closing dialog only marks itself retired. This also survives an `on_accept` that
opens another chooser reentrantly.

The dialog opens through the existing modal layer, so it stacks over another
dialog. The connect dialog's Browse... already relies on this.

## 3. Build order

- **P0** (done) Model core `bd_fs` plus the POSIX platform, with a `_WIN32`
  stub branch so the cross-build compiles it. Headless unit test (`test_fs`, 23
  checks) driving a fake platform plus a real POSIX scandir.
- **P1** (done) `bd_filedlg` Open mode: `BD_TABLE` detail view, path bar with an
  Up button, filter drop-down, filename field, all through `bd_dialog`. Replaced
  the hand-built `main.c` chooser; File > Import profiles... and the connect
  dialog's Browse... now go through it. Columns are model-ordered (`bd_fs` sorts
  dirs first, then by name), so clickable-header sorting is a later follow-up.
- **P2.5** (done) Verification fixes from a live smoke on the ludica backend:
  (a) the ludica backend's `be_scissor` was missing the top-left→bottom-left flip
  the GLES backend does, so every clipped widget (input lines, text areas, and
  the list/table content regions) rendered wrong: input text spilled past its
  field and lists/tables dropped their first row. Fixed in `bd_backend_ludica.c`,
  and `BD_INPUT_LINE` now clips its text like `BD_TEXT_AREA`. (b) Recents moved
  from individual sidebar rows into a single Recent shortcut plus a recents view
  (see Layer 3). (c) The export dialog was enlarged so its new Save As... button
  fits. (d) `BD_LIST` gained a `BD_ON_CHANGE_F` single-click selection callback
  so the sidebar navigates on one click (activation stays double-click).
  `test_fs` was also added to the `make test` alias.
- **P2** (done) Places sidebar (`bd_fs_places`): known folders, mounted volumes
  (via `getmntent` on Linux, `getmntinfo` on the BSDs/macOS), and the OS
  recently-used files (the Unix branch parses the freedesktop
  `recently-used.xbel`). Accepted files are registered back into that
  list via `bd_fs_add_recent`, so they surface next time and in native dialogs.
  The platform seam gained `volumes` / `recents` / `add_recent`, stubbed for
  `_WIN32`.
- **P3** (done) Save mode with an overwrite-confirm alert (`bd_notice`, stacked
  over the chooser), a New Folder prompt (`bd_fs_mkdir` + a one-field sub-dialog),
  a Hidden toggle (`BD_CHECKBOX`), and directory-select mode (`bd_fs_set_dirs_only`,
  no filename/filter, accept returns the chosen or current folder). The platform
  seam gained `make_dir`. The app's profile export now has a Save As... button
  that opens the Save chooser and drops the path into the export field.
- **P4** Win32 platform backend and `make windows-check` coverage; macOS /
  Android / iOS stubs behind the vtable.
- **P5** (optional) A native-dialog escape hatch per platform.

## 4. Testing

- The `bd_fs` model core gets a headless test (scandir, sort, filter, search,
  navigation), which runs in CI like `test_client`.
- A gallery entry in `make widget-test` exercises the dialog visually on the raw
  X11/GLES backend, independent of ludica.
```
