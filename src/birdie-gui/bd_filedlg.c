/*
 * bd_filedlg.c -- file open/save dialog for birdie-gui. See bd_filedlg.h.
 *
 * The dialog is composed, not a registered widget class: bd_filedlg_open builds
 * a bd_dialog, fills it with a table over a bd_fs model plus a path bar, filter
 * combo, and filename field, and wires the buttons back to a per-open instance
 * carried as each callback's arg. The instance owns its bd_fs.
 *
 * Lifetime note: bd_dialog's click handler touches the dialog after the button
 * callback returns, so an instance must not free itself inside a callback.
 * Instead each open tears down the previously retired instance (whose handlers
 * have fully unwound) and a closing dialog only marks itself retired. This also
 * survives a callback that opens another chooser reentrantly.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_filedlg.h"
#include "bd_dialog.h"
#include "bd_fs.h"
#include "bd_widget_table.h"
#include "bd_widget_combo.h"
#include "bd_widget_form.h"
#include "widget.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FD_MAXPAT   16		/* patterns per filter we apply */
#define FD_PATHMAX  4096

typedef struct filedlg {
	bd_dialog *dlg;
	bd_fs     *fs;
	int        mode;

	bd_id      path_field;
	bd_id      sidebar;
	bd_id      table;
	bd_id      name_field;

	bd_fs_place *places;
	int          nplaces;

	const bd_filedlg_filter *filters;
	int        nfilters;

	void     (*on_accept)(const char *path, void *user);
	void     (*on_cancel)(void *user);
	void      *user;

	/* scratch for the active filter, split out of a filter's pattern list */
	char        patbuf[256];
	const char *pats[FD_MAXPAT];

	/* the resolved target, held across an overwrite confirmation */
	char        pending[FD_PATHMAX];
} filedlg;

/* An instance whose dialog has closed, awaiting teardown at the next open. */
static filedlg *g_retired;

static void
teardown(filedlg *fd)
{
	if (!fd)
		return;
	bd_dialog_free(fd->dlg);
	bd_fs_places_free(fd->places, fd->nplaces);
	bd_fs_free(fd->fs);
	free(fd);
}

static void finalize_accept(filedlg *fd);

/* Set the filename field, tolerating its absence in directory-select mode. */
static void
set_name(filedlg *fd, const char *s)
{
	if (fd->name_field != BD_NONE)
		bd_set(fd->name_field, BD_LABEL_S, s, BD_END);
}

/****************************************************************
 * New Folder: a one-field text prompt stacked over the dialog.
 * It follows the same retire-at-next-open lifetime as the chooser,
 * since bd_dialog touches the dialog after a button callback returns.
 ****************************************************************/

struct prompt {
	bd_dialog *dlg;
	bd_id      field;
	void     (*cb)(const char *text, void *user);
	void      *user;
};
static struct prompt *g_prompt_retired;

static void
prompt_teardown(struct prompt *p)
{
	if (!p)
		return;
	bd_dialog_free(p->dlg);
	free(p);
}

static void
prompt_ok(bd_id panel, void *arg)
{
	(void)panel;
	struct prompt *p = arg;
	const char *t = bd_get_s(p->field, BD_LABEL_S);
	char buf[256];
	snprintf(buf, sizeof buf, "%s", t ? t : "");
	bd_dialog_close(p->dlg);
	if (p->cb)
		p->cb(buf, p->user);
	g_prompt_retired = p;
}

static void
prompt_cancel(bd_id panel, void *arg)
{
	(void)panel;
	g_prompt_retired = arg;
}

static void
prompt_open(const char *title, const char *label,
    void (*cb)(const char *, void *), void *user)
{
	prompt_teardown(g_prompt_retired);
	g_prompt_retired = NULL;

	struct prompt *p = calloc(1, sizeof *p);
	if (!p)
		return;
	p->cb = cb;
	p->user = user;
	p->dlg = bd_dialog_create(title, 320, 128);
	if (!p->dlg) {
		free(p);
		return;
	}
	p->field = bd_create(bd_dialog_field(p->dlg, label), BD_INPUT_LINE,
	    BD_GROW_I, 1, BD_PAD_I, 3, BD_END);
	bd_dialog_button(p->dlg, "Cancel", BD_DIALOG_CANCEL, prompt_cancel, p);
	bd_dialog_button(p->dlg, "Create", BD_DIALOG_DEFAULT, prompt_ok, p);
	bd_dialog_open(p->dlg);
}

/****************************************************************
 * Table model: format bd_fs entries into cells.
 ****************************************************************/

/* A small ring of scratch buffers so a cell() result survives until the table
 * copies it, even across the few calls it makes per row. */
static char *
scratch(void)
{
	static char buf[6][64];
	static int  i;
	i = (i + 1) % 6;
	return buf[i];
}

static void
human_size(int64_t b, char *out, size_t cap)
{
	if (b < 1024) {
		snprintf(out, cap, "%lld B", (long long)b);
		return;
	}
	static const char *unit[] = { "KB", "MB", "GB", "TB", "PB" };
	double v = (double)b / 1024.0;
	int u = 0;
	while (v >= 1024.0 && u < (int)(sizeof unit / sizeof unit[0]) - 1) {
		v /= 1024.0;
		u++;
	}
	snprintf(out, cap, "%.1f %s", v, unit[u]);
}

static int
model_rows(void *ctx)
{
	return bd_fs_count((bd_fs *)ctx);
}

static const char *
model_cell(void *ctx, int row, int col)
{
	const bd_fs_entry *e = bd_fs_get((bd_fs *)ctx, row);
	if (!e)
		return "";
	char *out = scratch();

	switch (col) {
	case 0:	/* Name (directories get a trailing slash) */
		snprintf(out, 64, "%s%s", e->name, e->is_dir ? "/" : "");
		return out;
	case 1:	/* Size (blank for directories) */
		if (e->is_dir)
			return "";
		human_size(e->size, out, 64);
		return out;
	case 2:	/* Modified */
	{
		time_t t = (time_t)e->mtime;
		struct tm *tm = localtime(&t);
		if (!tm || strftime(out, 64, "%Y-%m-%d %H:%M", tm) == 0)
			return "";
		return out;
	}
	default:
		return "";
	}
}

/****************************************************************
 * View sync
 ****************************************************************/

/* Push the model's current directory into the path field and re-query the
 * table. Called after any navigation or filter change. */
static void
sync_view(filedlg *fd)
{
	bd_set(fd->path_field, BD_LABEL_S, bd_fs_dir(fd->fs), BD_END);
	bd_table_refresh(fd->table);
}

/* Fetch the places list and fill the sidebar with their labels. */
static void
build_sidebar(filedlg *fd)
{
	fd->places = bd_fs_places(fd->fs, &fd->nplaces);
	size_t tot = 1;
	for (int i = 0; i < fd->nplaces; i++)
		tot += strlen(fd->places[i].label) + 1;
	char *items = malloc(tot);
	if (!items)
		return;
	size_t o = 0;
	for (int i = 0; i < fd->nplaces; i++) {
		int w = snprintf(items + o, tot - o, "%s%s",
		    i ? "\n" : "", fd->places[i].label);
		if (w < 0)
			break;
		o += (size_t)w;
	}
	items[o] = '\0';
	bd_list_set_items(fd->sidebar, items);
	free(items);
}

static void
apply_filter(filedlg *fd, int idx)
{
	if (idx < 0 || idx >= fd->nfilters) {
		bd_fs_set_filter(fd->fs, NULL, 0);
		return;
	}
	snprintf(fd->patbuf, sizeof fd->patbuf, "%s", fd->filters[idx].patterns);
	/* Split on ';' in place: replace separators with NUL, collect segments. */
	int n = 0;
	char *p = fd->patbuf;
	while (*p && n < FD_MAXPAT) {
		while (*p == ';')
			p++;
		if (!*p)
			break;
		fd->pats[n++] = p;
		while (*p && *p != ';')
			p++;
		if (*p)
			*p++ = '\0';
	}
	bd_fs_set_filter(fd->fs, fd->pats, n);
}

/****************************************************************
 * Widget callbacks
 ****************************************************************/

static void
on_up(bd_id id, void *arg)
{
	(void)id;
	filedlg *fd = arg;
	bd_fs_up(fd->fs);
	set_name(fd, "");
	sync_view(fd);
}

/* Click a places entry: the Recent shortcut opens the recents view; a folder or
 * volume navigates to it. */
static void
on_place(bd_id id, void *arg)
{
	(void)id;
	filedlg *fd = arg;
	int i = bd_list_selected(fd->sidebar);
	if (i < 0 || i >= fd->nplaces)
		return;
	const bd_fs_place *pl = &fd->places[i];

	if (pl->kind == BD_FS_PLACE_RECENT)
		bd_fs_show_recents(fd->fs);
	else
		bd_fs_chdir(fd->fs, pl->path);
	set_name(fd, "");
	sync_view(fd);
}

/* Enter in the path field: navigate to the typed path. */
static void
on_path_enter(bd_id id, void *arg)
{
	filedlg *fd = arg;
	const char *p = bd_get_s(id, BD_LABEL_S);
	if (p && p[0])
		bd_fs_chdir(fd->fs, p);
	set_name(fd, "");
	sync_view(fd);
}

/* Double-click / Enter on a row: descend into a directory, accept a recent
 * file (it carries its own path), or fill the name for a normal file. */
static void
on_row_activate(bd_id w, int row, void *ctx)
{
	(void)w;
	filedlg *fd = ctx;
	const bd_fs_entry *e = bd_fs_get(fd->fs, row);
	if (!e)
		return;
	if (e->is_dir) {
		bd_fs_chdir(fd->fs, e->name);
		set_name(fd, "");
		sync_view(fd);
	} else if (e->path) {			/* a recent file: open it */
		snprintf(fd->pending, sizeof fd->pending, "%s", e->path);
		finalize_accept(fd);
	} else {
		set_name(fd, e->name);
	}
}

/* Selecting a file mirrors its name into the field so Open uses it. */
static void
on_selection(bd_id w, void *ctx)
{
	filedlg *fd = ctx;
	int row = bd_table_current(w);
	const bd_fs_entry *e = (row >= 0) ? bd_fs_get(fd->fs, row) : NULL;
	if (e && !e->is_dir)
		set_name(fd, e->name);
}

static void
on_filter(bd_id id, void *arg, int index)
{
	(void)id;
	filedlg *fd = arg;
	apply_filter(fd, index);
	sync_view(fd);
}

static void
on_hidden(bd_id id, void *arg, int checked)
{
	(void)id;
	filedlg *fd = arg;
	bd_fs_set_hidden(fd->fs, checked);
	sync_view(fd);
}

/* Result of the New Folder prompt: create it and reveal it. */
static void
on_newfolder_name(const char *name, void *user)
{
	filedlg *fd = user;
	if (!name || !name[0])
		return;
	if (bd_fs_mkdir(fd->fs, name) == 0)
		sync_view(fd);
	else
		bd_notice_open("Could not create the folder.", "OK", NULL, NULL);
}

static void
on_newfolder(bd_id id, void *arg)
{
	(void)id;
	prompt_open("New Folder", "Name", on_newfolder_name, arg);
}

/* Join the current directory and a name into out (no trailing-slash surprises
 * at the root). */
static void
join_path(const bd_fs *fs, const char *name, char *out, size_t cap)
{
	const char *dir = bd_fs_dir(fs);
	if (dir[0] == '/' && dir[1] == '\0')
		snprintf(out, cap, "/%s", name);
	else
		snprintf(out, cap, "%s/%s", dir, name);
}

/* Does a plain file with this name exist in the current directory's view? */
static int
file_exists_here(filedlg *fd, const char *name)
{
	for (int i = 0, n = bd_fs_count(fd->fs); i < n; i++) {
		const bd_fs_entry *e = bd_fs_get(fd->fs, i);
		if (!e->is_dir && strcmp(e->name, name) == 0)
			return 1;
	}
	return 0;
}

/* Commit fd->pending: register it, close, hand it back, and retire. */
static void
finalize_accept(filedlg *fd)
{
	bd_fs_add_recent(fd->fs, fd->pending);
	bd_dialog_close(fd->dlg);
	if (fd->on_accept)
		fd->on_accept(fd->pending, fd->user);
	g_retired = fd;				/* free at the next open */
}

/* Overwrite confirmation for Save mode (button 0 = Overwrite). */
static void
on_confirm_overwrite(bd_id notice, int button, void *arg)
{
	(void)notice;
	if (button == 0)
		finalize_accept(arg);
}

/* Open / Save / Choose: resolve the target and hand it to the caller. */
static void
on_accept(bd_id panel, void *arg)
{
	(void)panel;
	filedlg *fd = arg;

	/* In the recents view, the target is the selected recent file's path. */
	if (bd_fs_is_recents(fd->fs)) {
		int row = bd_table_current(fd->table);
		const bd_fs_entry *e = (row >= 0) ? bd_fs_get(fd->fs, row) : NULL;
		if (e && e->path) {
			snprintf(fd->pending, sizeof fd->pending, "%s", e->path);
			finalize_accept(fd);
		}
		return;
	}

	/* Directory-select: choose the selected folder, else the current one. */
	if (fd->mode == BD_FILEDLG_DIR) {
		int row = bd_table_current(fd->table);
		const bd_fs_entry *e = (row >= 0) ? bd_fs_get(fd->fs, row) : NULL;
		if (e && e->is_dir)
			join_path(fd->fs, e->name, fd->pending, sizeof fd->pending);
		else
			snprintf(fd->pending, sizeof fd->pending, "%s",
			    bd_fs_dir(fd->fs));
		finalize_accept(fd);
		return;
	}

	const char *name = bd_get_s(fd->name_field, BD_LABEL_S);
	if (!name || !name[0]) {		/* no typed name: use the row */
		int row = bd_table_current(fd->table);
		const bd_fs_entry *e = (row >= 0) ? bd_fs_get(fd->fs, row) : NULL;
		if (!e)
			return;
		if (e->is_dir) {
			bd_fs_chdir(fd->fs, e->name);
			sync_view(fd);
			return;
		}
		name = e->name;
	}

	/* A typed name that matches a subdirectory descends into it. */
	for (int i = 0, n = bd_fs_count(fd->fs); i < n; i++) {
		const bd_fs_entry *e = bd_fs_get(fd->fs, i);
		if (e->is_dir && strcmp(e->name, name) == 0) {
			bd_fs_chdir(fd->fs, name);
			set_name(fd, "");
			sync_view(fd);
			return;
		}
	}

	join_path(fd->fs, name, fd->pending, sizeof fd->pending);

	/* Save over an existing file asks first. */
	if (fd->mode == BD_FILEDLG_SAVE && file_exists_here(fd, name)) {
		bd_notice_open("A file with that name exists. Overwrite it?",
		    "Overwrite\nCancel", on_confirm_overwrite, fd);
		return;
	}
	finalize_accept(fd);
}

static void
on_cancel(bd_id panel, void *arg)
{
	(void)panel;
	filedlg *fd = arg;
	if (fd->on_cancel)
		fd->on_cancel(fd->user);
	g_retired = fd;				/* CANCEL role closes the dialog */
}

/****************************************************************
 * Build + open
 ****************************************************************/

void
bd_filedlg_open(const bd_filedlg_opts *opts)
{
	if (!opts)
		return;

	/* Reclaim the previously closed instance now that its handlers unwound. */
	teardown(g_retired);
	g_retired = NULL;

	filedlg *fd = calloc(1, sizeof *fd);
	if (!fd)
		return;
	fd->mode = opts->mode;
	fd->filters = opts->filters;
	fd->nfilters = (opts->filters && opts->nfilters > 0) ? opts->nfilters : 0;
	fd->on_accept = opts->on_accept;
	fd->on_cancel = opts->on_cancel;
	fd->user = opts->user;

	fd->fs = bd_fs_open(opts->start_dir);
	if (!fd->fs) {
		free(fd);
		return;
	}

	const char *title = opts->title;
	const char *primary = "Open";
	if (fd->mode == BD_FILEDLG_SAVE)
		primary = "Save";
	else if (fd->mode == BD_FILEDLG_DIR)
		primary = "Choose";
	if (!title)
		title = (fd->mode == BD_FILEDLG_SAVE) ? "Save File" : "Open File";

	fd->dlg = bd_dialog_create(title, 640, 440);
	if (!fd->dlg) {
		bd_fs_free(fd->fs);
		free(fd);
		return;
	}
	bd_id content = bd_dialog_content(fd->dlg);

	/* Toolbar row: Up, New Folder, the editable path, and a hidden toggle. */
	bd_id bar = bd_create(content, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_ROW,
	    BD_PREF_H_I, 26, BD_GAP_I, 8, BD_END);
	bd_create(bar, BD_BUTTON, BD_LABEL_S, "Up", BD_PREF_W_I, 44,
	    BD_ON_CLICK_F, on_up, BD_ON_CLICK_P, fd, BD_END);
	bd_create(bar, BD_BUTTON, BD_LABEL_S, "New Folder", BD_PREF_W_I, 92,
	    BD_ON_CLICK_F, on_newfolder, BD_ON_CLICK_P, fd, BD_END);
	fd->path_field = bd_create(bar, BD_INPUT_LINE, BD_GROW_I, 1, BD_PAD_I, 3,
	    BD_ON_CLICK_F, on_path_enter, BD_ON_CLICK_P, fd, BD_END);
	bd_checkbox_create(bar, &(bd_checkbox_desc){
		.label = "Hidden", .checked = 0, .cb = on_hidden, .arg = fd,
	    }, BD_PREF_W_I, 84, BD_END);

	/* Middle row: a places sidebar beside the detail table. */
	bd_id mid = bd_create(content, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_ROW,
	    BD_GROW_I, 1, BD_GAP_I, 8, BD_END);
	fd->sidebar = bd_create(mid, BD_LIST, BD_PREF_W_I, 160,
	    BD_ON_CHANGE_F, on_place, BD_ON_CHANGE_P, fd, BD_END);

	/* The detail table. Columns are model-ordered (bd_fs sorts dirs first,
	 * then by name), so none are click-sortable here. */
	static const bd_table_column cols[] = {
		{ "Name",     0,   BD_TABLE_LEFT,  BD_TABLE_COL_NOSORT },
		{ "Size",     90,  BD_TABLE_RIGHT, BD_TABLE_COL_NOSORT },
		{ "Modified", 150, BD_TABLE_LEFT,  BD_TABLE_COL_NOSORT },
	};
	bd_table_model model = {
		.rows = model_rows,
		.cell = model_cell,
		.ctx = fd->fs,
	};
	bd_table_cb tcb = {
		.activate = on_row_activate,
		.selection_changed = on_selection,
		.ctx = fd,
	};
	fd->table = bd_table_create(mid, cols, 3, &model, &tcb,
	    BD_GROW_I, 1, BD_END);

	if (fd->mode == BD_FILEDLG_DIR) {
		/* Folder picker: no filename or filter, list directories only. */
		bd_fs_set_dirs_only(fd->fs, 1);
	} else {
		/* Filename field. */
		fd->name_field = bd_create(bd_dialog_field(fd->dlg, "File"),
		    BD_INPUT_LINE, BD_GROW_I, 1, BD_PAD_I, 3, BD_END);
		if (opts->default_name)
			bd_set(fd->name_field, BD_LABEL_S, opts->default_name, BD_END);

		/* Filter drop-down, only when the caller supplied filters. */
		if (fd->nfilters > 0) {
			int n = fd->nfilters < FD_MAXPAT ? fd->nfilters : FD_MAXPAT;
			const char *labels[FD_MAXPAT];
			for (int i = 0; i < n; i++)
				labels[i] = fd->filters[i].label;
			bd_combo_create(bd_dialog_field(fd->dlg, "Filter"),
			    &(bd_combo_desc){
				.items = labels, .count = n, .selected = 0,
				.cb = on_filter, .arg = fd,
			    }, BD_GROW_I, 1, BD_END);
			apply_filter(fd, 0);
		}
	}

	bd_dialog_button(fd->dlg, "Cancel", BD_DIALOG_CANCEL, on_cancel, fd);
	bd_dialog_button(fd->dlg, primary, BD_DIALOG_DEFAULT, on_accept, fd);

	build_sidebar(fd);
	sync_view(fd);
	bd_dialog_open(fd->dlg);
}
