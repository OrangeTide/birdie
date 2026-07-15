/*
 * bd_filedlg.h -- a file open/save dialog for birdie-gui.
 *
 * A modal file chooser composed from the toolkit's own widgets, so it matches
 * the rest of the chrome: a BD_TABLE detail view (name / size / modified) over
 * the bd_fs model, an editable path bar with an Up button, an extension-filter
 * drop-down, and a filename field, wired through bd_dialog. It draws on no
 * native OS dialog; the OS is reached only for filesystem services, through
 * bd_fs.
 *
 * One call opens it: bd_filedlg_open(opts). The chooser runs modal (it stacks
 * over another dialog, like the connect dialog's Browse...), and hands the
 * chosen path to opts.on_accept, or calls opts.on_cancel on dismissal. The opts
 * struct and the strings it points at (title, default_name, filter labels and
 * patterns) are borrowed and must stay valid until one of those callbacks runs;
 * pointing them at static data is the simplest way.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#ifndef BD_FILEDLG_H
#define BD_FILEDLG_H

/* Dialog mode. OPEN is implemented; SAVE and DIR share the OPEN flow for now
 * (only the primary button label differs) and gain their own semantics later. */
enum {
	BD_FILEDLG_OPEN,	/* pick an existing file */
	BD_FILEDLG_SAVE,	/* name a file to write */
	BD_FILEDLG_DIR,		/* pick a directory */
};

/* One entry in the filter drop-down: a human label and a ";"-separated list of
 * globs applied to filenames (for example "*.csv" or "*.png;*.jpg"). A pattern
 * of "*" matches everything. */
typedef struct bd_filedlg_filter {
	const char *label;
	const char *patterns;
} bd_filedlg_filter;

typedef struct bd_filedlg_opts {
	const char *title;		/* NULL uses a mode-appropriate default */
	int         mode;		/* BD_FILEDLG_* */
	const char *start_dir;		/* NULL or "" starts at the home directory */
	const char *default_name;	/* prefilled filename (SAVE); may be NULL */
	const bd_filedlg_filter *filters;	/* borrowed array; NULL for none */
	int         nfilters;
	/* Called with the chosen absolute path when the user accepts. */
	void      (*on_accept)(const char *path, void *user);
	/* Called when the user cancels or dismisses; may be NULL. */
	void      (*on_cancel)(void *user);
	void       *user;
} bd_filedlg_opts;

/* Open the chooser modal. Requires bd_gui to be initialized (it reads the
 * theme). Does nothing if opts is NULL or allocation fails. */
void bd_filedlg_open(const bd_filedlg_opts *opts);

#endif /* BD_FILEDLG_H */
