/*
 * bd_fs.h -- filesystem model for the birdie-gui file dialog.
 *
 * A UI-agnostic model of one directory: the entries in it, a sort order, an
 * extension filter, and a search string. It owns no widgets and draws nothing;
 * a front end (the bd_filedlg widget, or a test) reads the filtered/sorted view
 * and drives navigation. This mirrors the split in rswinkle/file_browser: the
 * model holds state, the host renders it.
 *
 * All OS coupling is funnelled through a small bd_fs_platform vtable (directory
 * enumeration, known folders, lexical path normalization). The default platform
 * is POSIX; a Windows one lands later. Injecting a custom platform makes the
 * model testable with synthetic entries and no disk access.
 *
 * Paths and names are UTF-8. On Windows the platform layer converts to and from
 * the native UTF-16 API at its boundary.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#ifndef BD_FS_H
#define BD_FS_H

#include <stddef.h>
#include <stdint.h>

/* One directory entry. Owned by the bd_fs that produced it. */
typedef struct bd_fs_entry {
	char   *name;		/* basename, UTF-8, heap-owned */
	char   *path;		/* full path, heap-owned, for virtual entries
				 * (the Recent view); NULL for a normal entry,
				 * whose path is the current directory + name */
	int64_t size;		/* size in bytes (files); 0 for directories */
	int64_t mtime;		/* modification time, seconds since epoch */
	int     is_dir;		/* nonzero for directories */
	int     is_link;	/* nonzero for symbolic links */
} bd_fs_entry;

/* Sort keys for bd_fs_sort. */
enum {
	BD_FS_SORT_NAME,
	BD_FS_SORT_SIZE,
	BD_FS_SORT_MTIME,
};

/* Known folders the platform can resolve for the places sidebar. */
enum {
	BD_FS_HOME,
	BD_FS_DESKTOP,
	BD_FS_DOCUMENTS,
	BD_FS_DOWNLOADS,
};

/* A shortcut in the places sidebar. */
enum {
	BD_FS_PLACE_FOLDER,	/* a known user folder (Home, Documents, ...) */
	BD_FS_PLACE_VOLUME,	/* a mounted volume / drive */
	BD_FS_PLACE_RECENT,	/* a recently-used file (from the OS) */
};

typedef struct bd_fs_place {
	char   *label;	/* display text, heap-owned */
	char   *path;	/* absolute target path, heap-owned (empty for Recent) */
	int     kind;	/* BD_FS_PLACE_* */
	int     is_dir;	/* nonzero when path is a directory */
	int64_t size;	/* recent files: size in bytes (0 otherwise) */
	int64_t mtime;	/* recent files: modification time (0 otherwise) */
} bd_fs_place;

/* The narrow OS seam. Every filesystem touch the model makes goes through one
 * of these. A platform implementation fills a static instance of this struct.
 * volumes/recents are added with the places sidebar (a later phase); a NULL
 * function pointer means "unsupported", and the model copes. */
typedef struct bd_fs_platform {
	/* Enumerate dir into a fresh malloc'd array of entries, each with a
	 * malloc'd name, and store the count. Omits "." and "..". Returns 0 on
	 * success, -1 on error (in which case *out is NULL and *n is 0). The
	 * caller (the model) owns and frees the result. */
	int  (*scandir)(const char *dir, bd_fs_entry **out, int *n);
	/* Resolve a BD_FS_* known folder into out (a buffer of cap bytes).
	 * Returns 0 on success, -1 if unknown or unsupported. */
	int  (*known_folder)(int which, char *out, size_t cap);
	/* Canonicalize path in place: collapse "." and ".." segments and
	 * duplicate separators. Lexical only; it does not touch the filesystem
	 * or resolve symlinks. */
	void (*normalize)(char *path);
	/* Enumerate mounted volumes into a fresh malloc'd array of places (each
	 * with malloc'd label and path), storing the count. Returns 0 on
	 * success, -1 on error. NULL means "unsupported". */
	int  (*volumes)(bd_fs_place **out, int *n);
	/* Enumerate the OS recently-used file list, newest first, same
	 * ownership and return convention as volumes. NULL means "unsupported". */
	int  (*recents)(bd_fs_place **out, int *n);
	/* Register path with the OS recent-documents facility. NULL or a no-op
	 * is fine. */
	void (*add_recent)(const char *path);
	/* Create the directory at path. Returns 0 on success, -1 on error.
	 * NULL means "unsupported". */
	int  (*make_dir)(const char *path);
} bd_fs_platform;

typedef struct bd_fs bd_fs;

/* The built-in platform for the host this was compiled for (POSIX today). */
const bd_fs_platform *bd_fs_platform_default(void);

/* Open a model rooted at start_dir (NULL or "" means the home directory), using
 * the default platform. Returns NULL on allocation failure. */
bd_fs *bd_fs_open(const char *start_dir);

/* As bd_fs_open, but with a caller-supplied platform. plat must outlive the
 * model. Used by tests to feed synthetic entries. */
bd_fs *bd_fs_open_ex(const char *start_dir, const bd_fs_platform *plat);

void   bd_fs_free(bd_fs *fs);

/* Navigation. chdir resolves dir relative to the current directory when it is
 * not absolute, normalizes it, and re-scans. up moves to the parent. refresh
 * re-scans the current directory (for external changes). Each rebuilds the
 * filtered/sorted view. */
void   bd_fs_chdir(bd_fs *fs, const char *dir);
void   bd_fs_up(bd_fs *fs);
void   bd_fs_refresh(bd_fs *fs);

/* The absolute, normalized current directory (UTF-8), or "Recent" while the
 * recents view is active. */
const char *bd_fs_dir(const bd_fs *fs);

/* Switch the view to the OS recently-used files (newest first) instead of a
 * directory listing. Each entry carries its full path (bd_fs_entry.path). Any
 * navigation (chdir/up) leaves this view. */
void   bd_fs_show_recents(bd_fs *fs);
int    bd_fs_is_recents(const bd_fs *fs);

/* View controls. Each rebuilds the view over the already-scanned entries
 * without hitting the filesystem. sort orders directories first, then by key
 * (descending when descending is nonzero). set_filter keeps a borrowed array of
 * glob patterns ("*.csv", "*"); a file is shown when it matches any pattern,
 * directories are always shown, and n == 0 shows everything. search keeps only
 * entries whose name contains text (case-insensitive); NULL or "" clears it.
 * set_hidden toggles dotfile visibility and re-scans. */
void   bd_fs_sort(bd_fs *fs, int key, int descending);
void   bd_fs_set_filter(bd_fs *fs, const char *const *patterns, int n);
void   bd_fs_search(bd_fs *fs, const char *text);
void   bd_fs_set_hidden(bd_fs *fs, int show);
int    bd_fs_show_hidden(const bd_fs *fs);

/* When set, the view hides plain files and shows only directories (for a
 * folder-picker). Off by default. */
void   bd_fs_set_dirs_only(bd_fs *fs, int dirs_only);

/* Create directory `name` inside the current directory and re-scan on success.
 * name is a single leaf component. Returns 0 on success, -1 on failure or when
 * the platform cannot create directories. */
int    bd_fs_mkdir(bd_fs *fs, const char *name);

/* The filtered/sorted view: count of visible entries and access by view index
 * (0 <= index < count). get returns NULL when index is out of range. The
 * pointer is valid until the next navigation or view-control call. */
int    bd_fs_count(const bd_fs *fs);
const bd_fs_entry *bd_fs_get(const bd_fs *fs, int index);

/* Assemble the places sidebar: the known user folders the platform resolves
 * (Home, Desktop, Documents, Downloads), then mounted volumes, then the OS
 * recently-used files. Returns a fresh array (owned by the caller) and stores
 * the count in *n; free it with bd_fs_places_free. Returns NULL with *n == 0
 * when nothing is available. */
bd_fs_place *bd_fs_places(bd_fs *fs, int *n);
void         bd_fs_places_free(bd_fs_place *places, int n);

/* Register path with the OS recent-documents facility (no-op if unsupported).
 * Called when the user opens or saves a file, so it appears in recents next
 * time. */
void         bd_fs_add_recent(bd_fs *fs, const char *path);

#endif /* BD_FS_H */
