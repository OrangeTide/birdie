/*
 * bd_fs.c -- filesystem model for the birdie-gui file dialog. See bd_fs.h.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#if !defined(_WIN32)
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "bd_fs.h"

#include <stdlib.h>
#include <string.h>

/****************************************************************
 * Case-insensitive helpers (ASCII), so the model needs no libc
 * locale routines and behaves the same on every platform.
 ****************************************************************/

static int
ci_lower(int c)
{
	return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int
ci_strcmp(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		int d = ci_lower((unsigned char)*a) - ci_lower((unsigned char)*b);
		if (d) return d;
	}
	return ci_lower((unsigned char)*a) - ci_lower((unsigned char)*b);
}

/* Case-insensitive substring test. Returns nonzero when hay contains needle. */
static int
ci_contains(const char *hay, const char *needle)
{
	size_t nl = strlen(needle);
	if (nl == 0) return 1;
	for (; *hay; hay++) {
		size_t i = 0;
		while (i < nl && hay[i] &&
		    ci_lower((unsigned char)hay[i]) ==
		    ci_lower((unsigned char)needle[i]))
			i++;
		if (i == nl) return 1;
	}
	return 0;
}

/* Case-insensitive glob match supporting '*' (any run) and '?' (one char).
 * Iterative with backtracking, so it never recurses on adversarial patterns. */
static int
glob_match(const char *pat, const char *str)
{
	const char *star = NULL, *back = NULL;
	while (*str) {
		if (*pat == '?' ||
		    ci_lower((unsigned char)*pat) == ci_lower((unsigned char)*str)) {
			pat++;
			str++;
		} else if (*pat == '*') {
			star = pat++;
			back = str;
		} else if (star) {
			pat = star + 1;
			str = ++back;
		} else {
			return 0;
		}
	}
	while (*pat == '*')
		pat++;
	return *pat == '\0';
}

/****************************************************************
 * Model
 ****************************************************************/

struct bd_fs {
	const bd_fs_platform *plat;
	char   *dir;			/* absolute, normalized current dir */
	bd_fs_entry *all;		/* every entry from the last scan */
	int     nall;
	int    *view;			/* indices into all, filtered + sorted */
	int     nview;
	int     view_cap;
	/* view controls */
	int     sort_key;
	int     sort_desc;
	int     show_hidden;
	int     dirs_only;
	int     recents_mode;		/* view is the OS recents, not a dir */
	const char *const *filters;	/* borrowed */
	int     nfilters;
	char   *search;			/* owned, NULL when unset */
};

static void
free_entries(bd_fs_entry *e, int n)
{
	for (int i = 0; i < n; i++) {
		free(e[i].name);
		free(e[i].path);
	}
	free(e);
}

/* Order: directories before files, then by the active key, with a
 * case-insensitive name tiebreak. A file-scope pointer carries the active
 * model into qsort's fixed comparator signature; the GUI is single-threaded, so
 * this is safe and keeps the sort key out of every entry. */
static bd_fs *g_sort;

static int
entry_cmp(const void *pa, const void *pb)
{
	const bd_fs_entry *a = pa, *b = pb;
	int r;

	if (a->is_dir != b->is_dir)
		return a->is_dir ? -1 : 1;

	switch (g_sort->sort_key) {
	case BD_FS_SORT_SIZE:
		r = (a->size > b->size) - (a->size < b->size);
		break;
	case BD_FS_SORT_MTIME:
		r = (a->mtime > b->mtime) - (a->mtime < b->mtime);
		break;
	default:
		r = 0;
		break;
	}
	if (g_sort->sort_desc)
		r = -r;
	if (r == 0)
		r = ci_strcmp(a->name, b->name);	/* stable tiebreak */
	return r;
}

/* Rebuild the visible view over the already-scanned entries: apply the hidden,
 * filter, and search rules, in the current sort order. */
static void
rebuild_view(bd_fs *fs)
{
	/* The recents view keeps the platform's newest-first order. */
	if (!fs->recents_mode) {
		g_sort = fs;
		qsort(fs->all, (size_t)fs->nall, sizeof fs->all[0], entry_cmp);
		g_sort = NULL;
	}

	if (fs->nall > fs->view_cap) {
		int cap = fs->nall;
		int *v = realloc(fs->view, (size_t)cap * sizeof *v);
		if (!v) {			/* keep the old view on OOM */
			fs->nview = 0;
			return;
		}
		fs->view = v;
		fs->view_cap = cap;
	}

	fs->nview = 0;
	for (int i = 0; i < fs->nall; i++) {
		const bd_fs_entry *e = &fs->all[i];

		if (!fs->show_hidden && e->name[0] == '.')
			continue;
		if (fs->dirs_only && !e->is_dir)
			continue;
		if (fs->search && !ci_contains(e->name, fs->search))
			continue;
		if (!e->is_dir && fs->nfilters > 0) {
			int hit = 0;
			for (int k = 0; k < fs->nfilters && !hit; k++)
				hit = glob_match(fs->filters[k], e->name);
			if (!hit)
				continue;
		}
		fs->view[fs->nview++] = i;
	}
}

static int
is_absolute(const char *p)
{
	if (p[0] == '/')
		return 1;
	/* Windows drive path ("C:\..." or "C:/..."). */
	if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
	    p[1] == ':')
		return 1;
	return 0;
}

/* Populate fs->all from the platform's recents list (files with full paths). */
static void
load_recents(bd_fs *fs)
{
	bd_fs_place *r = NULL;
	int rn = 0;
	if (!fs->plat->recents || fs->plat->recents(&r, &rn) != 0)
		return;

	bd_fs_entry *e = calloc((size_t)(rn > 0 ? rn : 1), sizeof *e);
	if (!e) {
		bd_fs_places_free(r, rn);
		return;
	}
	int m = 0;
	for (int i = 0; i < rn; i++) {
		e[m].name = strdup(r[i].label);
		e[m].path = strdup(r[i].path);
		if (!e[m].name || !e[m].path) {
			free(e[m].name);
			free(e[m].path);
			continue;
		}
		e[m].size = r[i].size;
		e[m].mtime = r[i].mtime;
		e[m].is_dir = 0;
		m++;
	}
	bd_fs_places_free(r, rn);
	fs->all = e;
	fs->nall = m;
}

void
bd_fs_refresh(bd_fs *fs)
{
	free_entries(fs->all, fs->nall);
	fs->all = NULL;
	fs->nall = 0;

	if (fs->recents_mode) {
		load_recents(fs);
	} else {
		bd_fs_entry *e = NULL;
		int n = 0;
		if (fs->plat->scandir &&
		    fs->plat->scandir(fs->dir, &e, &n) == 0) {
			fs->all = e;
			fs->nall = n;
		}
	}
	rebuild_view(fs);
}

void
bd_fs_chdir(bd_fs *fs, const char *dir)
{
	char *joined;

	if (!dir || !dir[0])
		return;

	if (is_absolute(dir)) {
		joined = strdup(dir);
	} else {
		size_t dl = strlen(fs->dir);
		size_t sl = strlen(dir);
		joined = malloc(dl + 1 + sl + 1);
		if (joined) {
			memcpy(joined, fs->dir, dl);
			size_t p = dl;
			if (p == 0 || joined[p - 1] != '/')
				joined[p++] = '/';
			memcpy(joined + p, dir, sl);
			joined[p + sl] = '\0';
		}
	}
	if (!joined)
		return;

	if (fs->plat->normalize)
		fs->plat->normalize(joined);

	free(fs->dir);
	fs->dir = joined;
	fs->recents_mode = 0;		/* navigating leaves the recents view */
	bd_fs_refresh(fs);
}

void
bd_fs_up(bd_fs *fs)
{
	bd_fs_chdir(fs, "..");
}

bd_fs *
bd_fs_open_ex(const char *start_dir, const bd_fs_platform *plat)
{
	bd_fs *fs = calloc(1, sizeof *fs);
	if (!fs)
		return NULL;
	fs->plat = plat;
	fs->sort_key = BD_FS_SORT_NAME;

	char home[1024];
	if ((!start_dir || !start_dir[0]) && plat->known_folder &&
	    plat->known_folder(BD_FS_HOME, home, sizeof home) == 0)
		start_dir = home;
	if (!start_dir || !start_dir[0])
		start_dir = "/";

	fs->dir = strdup(start_dir);
	if (!fs->dir) {
		free(fs);
		return NULL;
	}
	if (plat->normalize)
		plat->normalize(fs->dir);

	bd_fs_refresh(fs);
	return fs;
}

bd_fs *
bd_fs_open(const char *start_dir)
{
	return bd_fs_open_ex(start_dir, bd_fs_platform_default());
}

void
bd_fs_free(bd_fs *fs)
{
	if (!fs)
		return;
	free_entries(fs->all, fs->nall);
	free(fs->view);
	free(fs->search);
	free(fs->dir);
	free(fs);
}

const char *
bd_fs_dir(const bd_fs *fs)
{
	return fs->recents_mode ? "Recent" : fs->dir;
}

void
bd_fs_show_recents(bd_fs *fs)
{
	fs->recents_mode = 1;
	bd_fs_refresh(fs);
}

int
bd_fs_is_recents(const bd_fs *fs)
{
	return fs->recents_mode;
}

void
bd_fs_sort(bd_fs *fs, int key, int descending)
{
	fs->sort_key = key;
	fs->sort_desc = descending ? 1 : 0;
	rebuild_view(fs);
}

void
bd_fs_set_filter(bd_fs *fs, const char *const *patterns, int n)
{
	fs->filters = (n > 0) ? patterns : NULL;
	fs->nfilters = (n > 0) ? n : 0;
	rebuild_view(fs);
}

void
bd_fs_search(bd_fs *fs, const char *text)
{
	free(fs->search);
	fs->search = (text && text[0]) ? strdup(text) : NULL;
	rebuild_view(fs);
}

void
bd_fs_set_hidden(bd_fs *fs, int show)
{
	fs->show_hidden = show ? 1 : 0;
	rebuild_view(fs);
}

int
bd_fs_show_hidden(const bd_fs *fs)
{
	return fs->show_hidden;
}

void
bd_fs_set_dirs_only(bd_fs *fs, int dirs_only)
{
	fs->dirs_only = dirs_only ? 1 : 0;
	rebuild_view(fs);
}

int
bd_fs_mkdir(bd_fs *fs, const char *name)
{
	if (!fs->plat->make_dir || !name || !name[0])
		return -1;
	/* Reject a name that is not a single leaf component. */
	if (strchr(name, '/') || strcmp(name, ".") == 0 ||
	    strcmp(name, "..") == 0)
		return -1;

	size_t dl = strlen(fs->dir);
	size_t nl = strlen(name);
	char *path = malloc(dl + 1 + nl + 1);
	if (!path)
		return -1;
	memcpy(path, fs->dir, dl);
	size_t p = dl;
	if (p == 0 || path[p - 1] != '/')
		path[p++] = '/';
	memcpy(path + p, name, nl + 1);

	int r = fs->plat->make_dir(path);
	free(path);
	if (r == 0)
		bd_fs_refresh(fs);
	return r;
}

int
bd_fs_count(const bd_fs *fs)
{
	return fs->nview;
}

const bd_fs_entry *
bd_fs_get(const bd_fs *fs, int index)
{
	if (index < 0 || index >= fs->nview)
		return NULL;
	return &fs->all[fs->view[index]];
}

/****************************************************************
 * Places sidebar
 ****************************************************************/

static int
place_add(bd_fs_place **arr, int *n, int *cap, char *label, char *path,
    int kind, int is_dir)
{
	if (!label || !path) {
		free(label);
		free(path);
		return -1;
	}
	if (*n == *cap) {
		int nc = *cap ? *cap * 2 : 8;
		bd_fs_place *na = realloc(*arr, (size_t)nc * sizeof *na);
		if (!na) {
			free(label);
			free(path);
			return -1;
		}
		*arr = na;
		*cap = nc;
	}
	(*arr)[*n].label = label;
	(*arr)[*n].path = path;
	(*arr)[*n].kind = kind;
	(*arr)[*n].is_dir = is_dir;
	(*n)++;
	return 0;
}

/* Move an owned platform array (volumes or recents) onto the tail of arr,
 * transferring each entry's label/path pointers, then free the source array. */
static void
place_move(bd_fs_place **arr, int *n, int *cap, bd_fs_place *src, int sn)
{
	for (int i = 0; i < sn; i++) {
		if (place_add(arr, n, cap, src[i].label, src[i].path,
		    src[i].kind, src[i].is_dir) != 0) {
			/* place_add freed the moved strings on failure */
		}
	}
	free(src);
}

bd_fs_place *
bd_fs_places(bd_fs *fs, int *n)
{
	static const struct { int which; const char *label; } known[] = {
		{ BD_FS_HOME,      "Home" },
		{ BD_FS_DESKTOP,   "Desktop" },
		{ BD_FS_DOCUMENTS, "Documents" },
		{ BD_FS_DOWNLOADS, "Downloads" },
	};
	const bd_fs_platform *p = fs->plat;
	bd_fs_place *arr = NULL;
	int cnt = 0, cap = 0;
	char buf[1024];

	if (p->known_folder) {
		for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) {
			if (p->known_folder(known[i].which, buf, sizeof buf) == 0)
				place_add(&arr, &cnt, &cap,
				    strdup(known[i].label), strdup(buf),
				    BD_FS_PLACE_FOLDER, 1);
		}
	}
	/* A single Recent shortcut (its own view), not the files themselves. */
	if (p->recents)
		place_add(&arr, &cnt, &cap, strdup("Recent"), strdup(""),
		    BD_FS_PLACE_RECENT, 0);

	if (p->volumes) {
		bd_fs_place *v = NULL;
		int vn = 0;
		if (p->volumes(&v, &vn) == 0)
			place_move(&arr, &cnt, &cap, v, vn);
	}

	*n = cnt;
	return arr;
}

void
bd_fs_places_free(bd_fs_place *places, int n)
{
	for (int i = 0; i < n; i++) {
		free(places[i].label);
		free(places[i].path);
	}
	free(places);
}

void
bd_fs_add_recent(bd_fs *fs, const char *path)
{
	if (fs->plat->add_recent && path && path[0])
		fs->plat->add_recent(path);
}

/****************************************************************
 * Default platform, selected at compile time: a Win32 backend and a
 * Unix (POSIX + freedesktop) backend. Each host compiles only its own
 * branch; the other is skipped by the preprocessor. The cross-build
 * (make windows-check) compiles the Win32 branch.
 ****************************************************************/

/* Append an entry (by value) to a growable array, doubling capacity. Shared by
 * both platforms' scandir. */
static int
push_entry(bd_fs_entry **arr, int *n, int *cap, const bd_fs_entry *e)
{
	if (*n == *cap) {
		int nc = *cap ? *cap * 2 : 32;
		bd_fs_entry *na = realloc(*arr, (size_t)nc * sizeof *na);
		if (!na)
			return -1;
		*arr = na;
		*cap = nc;
	}
	(*arr)[(*n)++] = *e;
	return 0;
}

#if defined(_WIN32)

#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <wchar.h>

/* UTF-16 (native Win32) <-> UTF-8 (the toolkit's encoding). Return 0 on ok. */
static int
w2u(const WCHAR *w, char *out, int cap)
{
	return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL) > 0
	    ? 0 : -1;
}

static int
u2w(const char *u, WCHAR *out, int cap)
{
	return MultiByteToWideChar(CP_UTF8, 0, u, -1, out, cap) > 0 ? 0 : -1;
}

/* FILETIME (100 ns ticks since 1601) to Unix seconds. */
static int64_t
filetime_unix(FILETIME ft)
{
	ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	return t < 116444736000000000ULL ? 0
	    : (int64_t)((t - 116444736000000000ULL) / 10000000ULL);
}

static int
plat_scandir(const char *dir, bd_fs_entry **out, int *n)
{
	*out = NULL;
	*n = 0;

	char pat[4096];
	snprintf(pat, sizeof pat, "%s/*", dir);
	WCHAR wpat[4096];
	if (u2w(pat, wpat, 4096) != 0)
		return -1;

	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(wpat, &fd);
	if (h == INVALID_HANDLE_VALUE)
		return -1;

	bd_fs_entry *arr = NULL;
	int cnt = 0, cap = 0;
	do {
		if (wcscmp(fd.cFileName, L".") == 0 ||
		    wcscmp(fd.cFileName, L"..") == 0)
			continue;
		char name[1024];
		if (w2u(fd.cFileName, name, sizeof name) != 0)
			continue;
		bd_fs_entry e = {0};
		e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		    ? 1 : 0;
		e.is_link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
		    ? 1 : 0;
		e.size = ((int64_t)fd.nFileSizeHigh << 32) |
		    (int64_t)fd.nFileSizeLow;
		e.mtime = filetime_unix(fd.ftLastWriteTime);
		e.name = strdup(name);
		if (!e.name || push_entry(&arr, &cnt, &cap, &e) != 0) {
			free(e.name);
			continue;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);

	*out = arr;
	*n = cnt;
	return 0;
}

static int
plat_known_folder(int which, char *out, size_t cap)
{
	const KNOWNFOLDERID *id;
	switch (which) {
	case BD_FS_HOME:      id = &FOLDERID_Profile;   break;
	case BD_FS_DESKTOP:   id = &FOLDERID_Desktop;   break;
	case BD_FS_DOCUMENTS: id = &FOLDERID_Documents; break;
	case BD_FS_DOWNLOADS: id = &FOLDERID_Downloads; break;
	default:              return -1;
	}
	PWSTR w = NULL;
	if (SHGetKnownFolderPath(id, 0, NULL, &w) != S_OK) {
		if (w)
			CoTaskMemFree(w);
		return -1;
	}
	int r = w2u(w, out, (int)cap);
	CoTaskMemFree(w);
	return r;
}

/* Lexical normalize: fold '\\' to '/', collapse "." and ".." segments, and
 * preserve an optional "X:" drive prefix. */
static void
plat_normalize(char *path)
{
	for (char *p = path; *p; p++)
		if (*p == '\\')
			*p = '/';

	char drive = 0;
	char *body = path;
	if (((path[0] >= 'A' && path[0] <= 'Z') ||
	     (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':') {
		drive = path[0];
		body = path + 2;
	}
	int absolute = (body[0] == '/');

	char *segs[256];
	int nseg = 0;
	for (char *p = body; *p; ) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		char *start = p;
		while (*p && *p != '/')
			p++;
		size_t len = (size_t)(p - start);
		if (len == 1 && start[0] == '.') {
			/* skip */
		} else if (len == 2 && start[0] == '.' && start[1] == '.') {
			if (nseg > 0 &&
			    !(strncmp(segs[nseg - 1], "..", 2) == 0 &&
			      segs[nseg - 1][2] == '\0'))
				nseg--;
			else if (!absolute && nseg < 256)
				segs[nseg++] = start;
		} else if (nseg < 256) {
			segs[nseg++] = start;
		}
		if (*p)
			*p++ = '\0';
	}

	char *w = path;
	if (drive) {
		*w++ = drive;
		*w++ = ':';
	}
	if (absolute)
		*w++ = '/';
	for (int i = 0; i < nseg; i++) {
		if (i > 0)
			*w++ = '/';
		size_t len = strlen(segs[i]);
		memmove(w, segs[i], len);
		w += len;
	}
	if (w == path)			/* nothing at all: current dir */
		*w++ = '.';
	*w = '\0';
}

static int
plat_volumes(bd_fs_place **out, int *n)
{
	bd_fs_place *arr = NULL;
	int cnt = 0, cap = 0;
	DWORD mask = GetLogicalDrives();
	for (int i = 0; i < 26; i++) {
		if (!(mask & (1u << i)))
			continue;
		char label[3] = { (char)('A' + i), ':', '\0' };
		char path[4]  = { (char)('A' + i), ':', '/', '\0' };
		place_add(&arr, &cnt, &cap, strdup(label), strdup(path),
		    BD_FS_PLACE_VOLUME, 1);
	}
	*out = arr;
	*n = cnt;
	return 0;
}

/* Enumerating the Recent folder's .lnk shortcuts and resolving their targets
 * needs COM (IShellLink), which cannot even be link-verified without a Windows
 * host, so it is a follow-up. It returns empty (the Recent shortcut still
 * appears, just unpopulated); registration below already works. */
static int
plat_recents(bd_fs_place **out, int *n)
{
	*out = NULL;
	*n = 0;
	return 0;
}

static void
plat_add_recent(const char *path)
{
	WCHAR w[4096];
	if (u2w(path, w, 4096) == 0)
		SHAddToRecentDocs(SHARD_PATHW, w);
}

static int
plat_make_dir(const char *path)
{
	WCHAR w[4096];
	if (u2w(path, w, 4096) != 0)
		return -1;
	return CreateDirectoryW(w, NULL) ? 0 : -1;
}

#else /* Unix: X11 desktops (Linux, the BSDs, ...) via POSIX + freedesktop */

#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>
#include <time.h>
#include <ctype.h>

/* Defined further down; declared here so the known-folder and recents code
 * above their definitions can use them. */
static const char *get_home(void);
static char *read_file(const char *path);

static int
plat_scandir(const char *dir, bd_fs_entry **out, int *n)
{
	DIR *d = opendir(dir);
	if (!d) {
		*out = NULL;
		*n = 0;
		return -1;
	}

	bd_fs_entry *arr = NULL;
	int cnt = 0, cap = 0;
	size_t dl = strlen(dir);
	struct dirent *de;

	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;

		/* Build the full path to stat. */
		size_t nl = strlen(de->d_name);
		char *full = malloc(dl + 1 + nl + 1);
		if (!full)
			continue;
		memcpy(full, dir, dl);
		size_t p = dl;
		if (p == 0 || full[p - 1] != '/')
			full[p++] = '/';
		memcpy(full + p, de->d_name, nl + 1);

		bd_fs_entry e = {0};
		struct stat lst, st;
		e.is_link = (lstat(full, &lst) == 0 && S_ISLNK(lst.st_mode));
		if (stat(full, &st) == 0) {		/* follow links */
			e.is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
			e.size = (int64_t)st.st_size;
			e.mtime = (int64_t)st.st_mtime;
		} else if (e.is_link) {			/* dangling link */
			e.size = (int64_t)lst.st_size;
			e.mtime = (int64_t)lst.st_mtime;
		}
		free(full);

		e.name = strdup(de->d_name);
		if (!e.name || push_entry(&arr, &cnt, &cap, &e) != 0) {
			free(e.name);
			continue;
		}
	}
	closedir(d);

	*out = arr;
	*n = cnt;
	return 0;
}

/* Resolve an XDG user directory from the freedesktop user-dirs.dirs file (for
 * example key "XDG_DESKTOP_DIR"). This is the localized, user-configured path,
 * so it is correct regardless of language or layout. Returns 0 on success. */
static int
xdg_user_dir(const char *key, char *out, size_t cap)
{
	const char *home = get_home();
	if (!home)
		return -1;

	const char *cfg = getenv("XDG_CONFIG_HOME");
	char path[1024];
	if (cfg && cfg[0])
		snprintf(path, sizeof path, "%s/user-dirs.dirs", cfg);
	else
		snprintf(path, sizeof path, "%s/.config/user-dirs.dirs", home);

	char *buf = read_file(path);
	if (!buf)
		return -1;

	int found = -1;
	size_t klen = strlen(key);
	for (char *line = buf; line && *line; ) {
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';
		char *s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		if (strncmp(s, key, klen) == 0) {
			char *v = s + klen;
			while (*v == ' ' || *v == '\t')
				v++;
			if (*v == '=') {
				v++;
				if (*v == '"')
					v++;
				for (char *e = v; *e; e++)
					if (*e == '"') { *e = '\0'; break; }
				int r = -1;
				if (strncmp(v, "$HOME", 5) == 0)
					r = snprintf(out, cap, "%s%s", home, v + 5);
				else if (v[0] == '/')
					r = snprintf(out, cap, "%s", v);
				if (r > 0 && (size_t)r < cap)
					found = 0;
			}
			if (found == 0)
				break;
		}
		line = nl ? nl + 1 : NULL;
	}
	free(buf);
	return found;
}

static int
plat_known_folder(int which, char *out, size_t cap)
{
	const char *home = get_home();
	if (!home)
		return -1;

	if (which == BD_FS_HOME) {
		int r = snprintf(out, cap, "%s", home);
		return (r > 0 && (size_t)r < cap) ? 0 : -1;
	}

	const char *key = NULL, *fallback = NULL;
	switch (which) {
	case BD_FS_DESKTOP:	key = "XDG_DESKTOP_DIR";   fallback = "Desktop";   break;
	case BD_FS_DOCUMENTS:	key = "XDG_DOCUMENTS_DIR"; fallback = "Documents"; break;
	case BD_FS_DOWNLOADS:	key = "XDG_DOWNLOAD_DIR";  fallback = "Downloads"; break;
	default:		return -1;
	}

	if (xdg_user_dir(key, out, cap) == 0)
		return 0;

	/* No user-dirs.dirs entry: fall back to the conventional name. */
	int r = snprintf(out, cap, "%s/%s", home, fallback);
	return (r > 0 && (size_t)r < cap) ? 0 : -1;
}

/* Lexical path normalizer: collapse "." and ".." and duplicate slashes without
 * touching the filesystem. An absolute path keeps its leading "/"; ".." at the
 * root is dropped. */
static void
plat_normalize(char *path)
{
	int absolute = (path[0] == '/');
	char *segs[256];
	int nseg = 0;

	for (char *p = path; *p; ) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		char *start = p;
		while (*p && *p != '/')
			p++;
		size_t len = (size_t)(p - start);

		if (len == 1 && start[0] == '.') {
			/* skip */
		} else if (len == 2 && start[0] == '.' && start[1] == '.') {
			if (nseg > 0 &&
			    !(strncmp(segs[nseg - 1], "..", 2) == 0 &&
			      segs[nseg - 1][2] == '\0'))
				nseg--;
			else if (!absolute && nseg < 256)
				segs[nseg++] = start;	/* keep leading ".." */
		} else if (nseg < 256) {
			segs[nseg++] = start;
		}
		/* NUL-terminate this segment in place for the rebuild below. */
		if (*p)
			*p++ = '\0';
	}

	char *w = path;
	if (absolute)
		*w++ = '/';
	for (int i = 0; i < nseg; i++) {
		if (i > 0)
			*w++ = '/';
		size_t len = strlen(segs[i]);
		memmove(w, segs[i], len);
		w += len;
	}
	if (w == path)		/* nothing left: root or cwd */
		*w++ = absolute ? '/' : '.';
	*w = '\0';
}

/* -------- places: volumes and recently-used files -------- */

static const char *
get_home(void)
{
	const char *home = getenv("HOME");
	if (!home || !home[0]) {
		struct passwd *pw = getpwuid(getuid());
		home = pw ? pw->pw_dir : NULL;
	}
	return (home && home[0]) ? home : NULL;
}

static char *
basename_dup(const char *path)
{
	const char *slash = strrchr(path, '/');
	const char *base = slash ? slash + 1 : path;
	return strdup(base[0] ? base : path);
}

static void
url_decode(const char *s, char *out, size_t cap)
{
	size_t o = 0;
	while (*s && o + 1 < cap) {
		if (s[0] == '%' && isxdigit((unsigned char)s[1]) &&
		    isxdigit((unsigned char)s[2])) {
			char hex[3] = { s[1], s[2], 0 };
			out[o++] = (char)strtol(hex, NULL, 16);
			s += 3;
		} else {
			out[o++] = *s++;
		}
	}
	out[o] = '\0';
}

static void
url_encode(const char *s, char *out, size_t cap)
{
	static const char *hex = "0123456789ABCDEF";
	size_t o = 0;
	for (; *s && o + 1 < cap; s++) {
		unsigned char c = (unsigned char)*s;
		if (isalnum(c) || c == '-' || c == '_' || c == '.' ||
		    c == '~' || c == '/') {
			out[o++] = (char)c;
		} else if (o + 3 < cap) {
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 0xF];
		} else {
			break;
		}
	}
	out[o] = '\0';
}

/* Read a whole (small) file into a fresh NUL-terminated buffer, or NULL. */
static char *
read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	if (sz < 0 || sz > 4L * 1024 * 1024) {
		fclose(f);
		return NULL;
	}
	fseek(f, 0, SEEK_SET);
	char *b = malloc((size_t)sz + 1);
	if (b) {
		size_t rd = fread(b, 1, (size_t)sz, f);
		b[rd] = '\0';
	}
	fclose(f);
	return b;
}

static int
recent_xbel_path(char *out, size_t cap)
{
	const char *xdg = getenv("XDG_DATA_HOME");
	int r;
	if (xdg && xdg[0]) {
		r = snprintf(out, cap, "%s/recently-used.xbel", xdg);
	} else {
		const char *home = get_home();
		if (!home)
			return 0;
		r = snprintf(out, cap, "%s/.local/share/recently-used.xbel", home);
	}
	return (r > 0 && (size_t)r < cap);
}

/* Value of attribute `key` (e.g. "href=\"") within [start,end), copied into
 * out up to the closing quote. Returns out or NULL when absent. */
static char *
attr_val(const char *start, const char *end, const char *key,
    char *out, size_t cap)
{
	size_t klen = strlen(key);
	for (const char *p = start; p + klen <= end; p++) {
		if (strncmp(p, key, klen) != 0)
			continue;
		p += klen;
		size_t i = 0;
		while (p < end && *p != '"' && i + 1 < cap)
			out[i++] = *p++;
		out[i] = '\0';
		return out;
	}
	return NULL;
}

/* Strip "file://" and any authority, returning the path portion of an href. */
static const char *
file_uri_path(const char *href)
{
	const char *p = href + 7;		/* past "file://" */
	if (*p != '/') {			/* skip an authority component */
		const char *slash = strchr(p, '/');
		if (slash)
			p = slash;
	}
	return p;
}

struct rec { char *label; char *path; char mod[40]; int64_t size, mtime; };

static int
rec_cmp_desc(const void *pa, const void *pb)
{
	const struct rec *a = pa, *b = pb;
	return strcmp(b->mod, a->mod);		/* newest modified first */
}

static int
plat_recents(bd_fs_place **out, int *n)
{
	*out = NULL;
	*n = 0;

	char xbel[1024];
	if (!recent_xbel_path(xbel, sizeof xbel))
		return -1;
	char *buf = read_file(xbel);
	if (!buf)
		return 0;			/* no recents yet: empty, not error */

	struct rec *rs = NULL;
	int rc = 0, rcap = 0;

	for (char *p = strstr(buf, "<bookmark"); p; p = strstr(p, "<bookmark")) {
		char *end = strchr(p, '>');
		if (!end)
			break;
		char href[2048];
		if (attr_val(p, end, "href=\"", href, sizeof href) &&
		    strncmp(href, "file://", 7) == 0) {
			char decoded[2048];
			url_decode(file_uri_path(href), decoded, sizeof decoded);
			struct stat st;
			int dup = 0;
			for (int i = 0; i < rc; i++)
				if (strcmp(rs[i].path, decoded) == 0) {
					dup = 1;
					break;
				}
			if (!dup && stat(decoded, &st) == 0 &&
			    !S_ISDIR(st.st_mode)) {
				if (rc == rcap) {
					int nc = rcap ? rcap * 2 : 16;
					struct rec *nr = realloc(rs,
					    (size_t)nc * sizeof *nr);
					if (!nr)
						break;
					rs = nr;
					rcap = nc;
				}
				rs[rc].label = basename_dup(decoded);
				rs[rc].path = strdup(decoded);
				rs[rc].size = (int64_t)st.st_size;
				rs[rc].mtime = (int64_t)st.st_mtime;
				rs[rc].mod[0] = '\0';
				attr_val(p, end, "modified=\"", rs[rc].mod,
				    sizeof rs[rc].mod);
				if (rs[rc].label && rs[rc].path)
					rc++;
				else {
					free(rs[rc].label);
					free(rs[rc].path);
				}
			}
		}
		p = end;
	}
	free(buf);

	qsort(rs, (size_t)rc, sizeof *rs, rec_cmp_desc);

	int keep = rc < 24 ? rc : 24;		/* newest 24 */
	bd_fs_place *arr = keep ? malloc((size_t)keep * sizeof *arr) : NULL;
	for (int i = 0; i < rc; i++) {
		if (arr && i < keep) {
			arr[i].label = rs[i].label;
			arr[i].path = rs[i].path;
			arr[i].kind = BD_FS_PLACE_RECENT;
			arr[i].is_dir = 0;
			arr[i].size = rs[i].size;
			arr[i].mtime = rs[i].mtime;
		} else {			/* trimmed or OOM: release */
			free(rs[i].label);
			free(rs[i].path);
		}
	}
	free(rs);

	*out = arr;
	*n = arr ? keep : 0;
	return 0;
}

static int
under(const char *path, const char *prefix)
{
	size_t pl = strlen(prefix);
	return strncmp(path, prefix, pl) == 0 && path[pl] != '\0';
}

/* A user-facing volume is a real device mounted where desktops surface
 * removable media, kept OS-neutral so the same rule serves Linux, the BSDs, and
 * macOS. Truly identifying "removable" needs UDisks2/dbus, deferred by design. */
static int
is_user_volume(const char *dev, const char *mnt)
{
	return strncmp(dev, "/dev/", 5) == 0 &&
	    (under(mnt, "/media/") || under(mnt, "/run/media/") ||
	     under(mnt, "/mnt/") || under(mnt, "/Volumes/"));
}

/* Enumerate mounted filesystems, invoking cb(ctx, dev, mountpoint) for each.
 * The enumeration source is the one OS-divergent piece: Linux exposes it
 * through getmntent, the BSDs and macOS through getmntinfo. Anywhere else we
 * simply offer no removable volumes rather than guess. */
typedef void (*mount_cb)(void *ctx, const char *dev, const char *mnt);

#if defined(__linux__)

#include <mntent.h>

/* Mount points in /proc/mounts use \NNN octal escapes for spaces and such. */
static void
unescape_octal(char *s)
{
	char *w = s;
	for (char *r = s; *r; ) {
		if (r[0] == '\\' && r[1] >= '0' && r[1] <= '7' &&
		    r[2] >= '0' && r[2] <= '7' && r[3] >= '0' && r[3] <= '7') {
			*w++ = (char)((r[1] - '0') * 64 + (r[2] - '0') * 8 +
			    (r[3] - '0'));
			r += 4;
		} else {
			*w++ = *r++;
		}
	}
	*w = '\0';
}

static void
each_mount(mount_cb cb, void *ctx)
{
	FILE *f = setmntent("/proc/self/mounts", "r");
	if (!f)
		f = setmntent("/etc/mtab", "r");
	if (!f)
		return;
	struct mntent *m;
	while ((m = getmntent(f)) != NULL) {
		char dir[1024];
		snprintf(dir, sizeof dir, "%s", m->mnt_dir);
		unescape_octal(dir);
		cb(ctx, m->mnt_fsname, dir);
	}
	endmntent(f);
}

#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__APPLE__)

#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>

static void
each_mount(mount_cb cb, void *ctx)
{
	struct statfs *mnt = NULL;
	int cnt = getmntinfo(&mnt, MNT_NOWAIT);
	for (int i = 0; i < cnt; i++)
		cb(ctx, mnt[i].f_mntfromname, mnt[i].f_mntonname);
}

#else /* other Unix (NetBSD, OpenBSD, ...): no removable-media enumeration */

static void
each_mount(mount_cb cb, void *ctx)
{
	(void)cb;
	(void)ctx;
}

#endif

struct vol_acc { bd_fs_place *arr; int cnt, cap; };

static void
vol_collect(void *ctx, const char *dev, const char *mnt)
{
	struct vol_acc *a = ctx;
	if (is_user_volume(dev, mnt))
		place_add(&a->arr, &a->cnt, &a->cap, basename_dup(mnt),
		    strdup(mnt), BD_FS_PLACE_VOLUME, 1);
}

static int
plat_volumes(bd_fs_place **out, int *n)
{
	struct vol_acc a = {0};

	/* The root filesystem is always a place. */
	place_add(&a.arr, &a.cnt, &a.cap, strdup("File System"), strdup("/"),
	    BD_FS_PLACE_VOLUME, 1);

	each_mount(vol_collect, &a);

	*out = a.arr;
	*n = a.cnt;
	return 0;
}

/* Create the directory holding `file`, and its parent, best-effort. */
static void
ensure_parent_dirs(const char *file)
{
	char dir[1024];
	snprintf(dir, sizeof dir, "%s", file);
	char *slash = strrchr(dir, '/');
	if (!slash)
		return;
	*slash = '\0';
	/* Walk and create each component. */
	for (char *p = dir + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(dir, 0700);
			*p = '/';
		}
	}
	mkdir(dir, 0700);
}

static void
now_iso(char *out, size_t cap)
{
	time_t t = time(NULL);
	struct tm *tm = gmtime(&t);
	if (!tm || strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", tm) == 0)
		snprintf(out, cap, "1970-01-01T00:00:00Z");
}

/* Cut every <bookmark ...>...</bookmark> element whose href equals `hrefattr`,
 * so re-registering a file does not accumulate duplicates. */
static void
remove_bookmark(char *buf, const char *hrefattr)
{
	char needle[2200];
	snprintf(needle, sizeof needle, "href=\"%s\"", hrefattr);
	for (;;) {
		char *h = strstr(buf, needle);
		if (!h)
			return;
		char *bstart = h;
		while (bstart > buf && strncmp(bstart, "<bookmark", 9) != 0)
			bstart--;
		if (strncmp(bstart, "<bookmark", 9) != 0)
			return;			/* malformed; leave it be */
		char *bend = strstr(h, "</bookmark>");
		char *tail;
		if (bend) {
			tail = bend + strlen("</bookmark>");
		} else {			/* self-closing */
			char *gt = strchr(h, '>');
			if (!gt)
				return;
			tail = gt + 1;
		}
		while (*tail == '\n')
			tail++;
		memmove(bstart, tail, strlen(tail) + 1);
	}
}

static void
plat_add_recent(const char *path)
{
	if (path[0] != '/')			/* only absolute local paths */
		return;

	char xbel[1024];
	if (!recent_xbel_path(xbel, sizeof xbel))
		return;
	ensure_parent_dirs(xbel);

	char enc[2048], href[2100];
	url_encode(path, enc, sizeof enc);
	snprintf(href, sizeof href, "file://%s", enc);

	static const char *empty_doc =
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<xbel version=\"1.0\"\n"
	    "      xmlns:bookmark=\"http://www.freedesktop.org/standards/desktop-bookmarks\"\n"
	    "      xmlns:mime=\"http://www.freedesktop.org/standards/shared-mime-info\">\n"
	    "</xbel>\n";

	char *doc = read_file(xbel);
	if (!doc || !strstr(doc, "</xbel>")) {
		free(doc);
		doc = strdup(empty_doc);
		if (!doc)
			return;
	}
	remove_bookmark(doc, href);

	char ts[40];
	now_iso(ts, sizeof ts);
	char block[4096];
	snprintf(block, sizeof block,
	    "  <bookmark href=\"%s\" added=\"%s\" modified=\"%s\" visited=\"%s\">\n"
	    "    <info>\n"
	    "      <metadata owner=\"http://freedesktop.org\">\n"
	    "        <mime:mime-type type=\"application/octet-stream\"/>\n"
	    "        <bookmark:applications>\n"
	    "          <bookmark:application name=\"birdie\" exec=\"&apos;birdie %%u&apos;\" modified=\"%s\" count=\"1\"/>\n"
	    "        </bookmark:applications>\n"
	    "      </metadata>\n"
	    "    </info>\n"
	    "  </bookmark>\n",
	    href, ts, ts, ts, ts);

	char *at = strstr(doc, "</xbel>");
	if (!at) {				/* should not happen after the guard */
		free(doc);
		return;
	}
	size_t head = (size_t)(at - doc);
	char *merged = malloc(strlen(doc) + strlen(block) + 1);
	if (merged) {
		memcpy(merged, doc, head);
		size_t o = head;
		size_t bl = strlen(block);
		memcpy(merged + o, block, bl);
		o += bl;
		strcpy(merged + o, doc + head);	/* the rest, from </xbel> on */

		char tmp[1088];
		snprintf(tmp, sizeof tmp, "%s.tmp", xbel);
		FILE *w = fopen(tmp, "wb");
		if (w) {
			fwrite(merged, 1, strlen(merged), w);
			fclose(w);
			rename(tmp, xbel);	/* atomic replace */
		}
		free(merged);
	}
	free(doc);
}

static int
plat_make_dir(const char *path)
{
	return mkdir(path, 0777) == 0 ? 0 : -1;	/* umask trims the mode */
}

#endif /* platform */

const bd_fs_platform *
bd_fs_platform_default(void)
{
	static const bd_fs_platform plat = {
		.scandir = plat_scandir,
		.known_folder = plat_known_folder,
		.normalize = plat_normalize,
		.volumes = plat_volumes,
		.recents = plat_recents,
		.add_recent = plat_add_recent,
		.make_dir = plat_make_dir,
	};
	return &plat;
}
