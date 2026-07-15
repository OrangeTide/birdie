/*
 * Headless tests for bd_fs, the file-dialog filesystem model.
 *
 * The model is UI-agnostic and all OS access goes through a bd_fs_platform
 * vtable, so most of it is tested against a FAKE platform that serves a fixed
 * synthetic directory: sort, filter, search, hidden, and navigation are all
 * exercised deterministically with no disk. A small second test drives the real
 * POSIX platform against a temporary directory to prove the scandir binding.
 * Exit code 0 = all checks passed. Run via `make test`.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */
#include "bd_fs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

/* ---- test harness (matches test_client.c) ---- */
static int checks, fails;
static void
check(const char *what, int ok)
{
	checks++;
	if (!ok) fails++;
	printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
}

/* ================================================================== */
/* Fake platform: a fixed synthetic directory, real lexical normalize */
/* ================================================================== */
struct row { const char *name; int64_t size; int64_t mtime; int is_dir; };

static const struct row FAKE[] = {
	{ "banana.txt", 300, 100, 0 },
	{ "Apple.md",   100, 300, 0 },
	{ "cherry.csv", 200, 200, 0 },
	{ "docs",         0,  50, 1 },
	{ "Build",        0,  60, 1 },
	{ ".hidden",     10,  10, 0 },
	{ ".git",         0,  20, 1 },
};
#define FAKE_N ((int)(sizeof FAKE / sizeof FAKE[0]))

static int
fake_scandir(const char *dir, bd_fs_entry **out, int *n)
{
	(void)dir;
	bd_fs_entry *e = calloc(FAKE_N, sizeof *e);
	if (!e) { *out = NULL; *n = 0; return -1; }
	for (int i = 0; i < FAKE_N; i++) {
		e[i].name = strdup(FAKE[i].name);
		e[i].size = FAKE[i].size;
		e[i].mtime = FAKE[i].mtime;
		e[i].is_dir = FAKE[i].is_dir;
	}
	*out = e;
	*n = FAKE_N;
	return 0;
}

static int
fake_home(int which, char *out, size_t cap)
{
	if (which != BD_FS_HOME) return -1;
	int r = snprintf(out, cap, "%s", "/home/user");
	return (r > 0 && (size_t)r < cap) ? 0 : -1;
}

static const char *
name_at(bd_fs *fs, int i)
{
	const bd_fs_entry *e = bd_fs_get(fs, i);
	return e ? e->name : "<null>";
}

static void
test_fake(void)
{
	printf("bd_fs (fake platform):\n");

	bd_fs_platform plat = {
		.scandir = fake_scandir,
		.known_folder = fake_home,
		/* reuse the real lexical normalizer under test */
		.normalize = bd_fs_platform_default()->normalize,
	};

	bd_fs *fs = bd_fs_open_ex(NULL, &plat);	/* NULL start -> HOME */
	check("open succeeds", fs != NULL);
	check("start dir is home", strcmp(bd_fs_dir(fs), "/home/user") == 0);

	/* Default: name-ascending, dirs first, hidden excluded (5 of 7). */
	check("hidden entries excluded by default", bd_fs_count(fs) == 5);
	check("dirs sort before files, case-insensitive",
	    strcmp(name_at(fs, 0), "Build") == 0 &&
	    strcmp(name_at(fs, 1), "docs") == 0 &&
	    strcmp(name_at(fs, 2), "Apple.md") == 0 &&
	    strcmp(name_at(fs, 3), "banana.txt") == 0 &&
	    strcmp(name_at(fs, 4), "cherry.csv") == 0);
	check("out-of-range get is NULL",
	    bd_fs_get(fs, -1) == NULL && bd_fs_get(fs, 5) == NULL);

	/* Sort by size descending: dirs still first, files big-to-small. */
	bd_fs_sort(fs, BD_FS_SORT_SIZE, 1);
	check("size-desc orders files 300,200,100",
	    strcmp(name_at(fs, 2), "banana.txt") == 0 &&
	    strcmp(name_at(fs, 3), "cherry.csv") == 0 &&
	    strcmp(name_at(fs, 4), "Apple.md") == 0);

	/* Filter to CSV: files must match, dirs always pass. */
	const char *csv[] = { "*.csv" };
	bd_fs_set_filter(fs, csv, 1);
	check("filter *.csv keeps 2 dirs + cherry.csv", bd_fs_count(fs) == 3);
	check("filtered file is cherry.csv",
	    strcmp(name_at(fs, 2), "cherry.csv") == 0);
	bd_fs_set_filter(fs, NULL, 0);
	check("clearing filter restores 5", bd_fs_count(fs) == 5);

	/* Search is case-insensitive substring over dirs and files. */
	bd_fs_sort(fs, BD_FS_SORT_NAME, 0);
	bd_fs_search(fs, "a");
	check("search 'a' matches Apple.md + banana.txt", bd_fs_count(fs) == 2);
	bd_fs_search(fs, "");
	check("clearing search restores 5", bd_fs_count(fs) == 5);

	/* Directory-only view (folder picker) drops the files. */
	bd_fs_set_dirs_only(fs, 1);
	check("dirs-only shows just the 2 directories", bd_fs_count(fs) == 2);
	bd_fs_set_dirs_only(fs, 0);
	check("clearing dirs-only restores 5", bd_fs_count(fs) == 5);

	/* Hidden toggle exposes the two dotfiles (.git dir, .hidden file). */
	bd_fs_set_hidden(fs, 1);
	check("show hidden reveals all 7", bd_fs_count(fs) == 7);
	bd_fs_set_hidden(fs, 0);

	/* Navigation + lexical normalization. */
	bd_fs_chdir(fs, "docs");
	check("chdir descends relative", strcmp(bd_fs_dir(fs), "/home/user/docs") == 0);
	bd_fs_up(fs);
	check("up returns to parent", strcmp(bd_fs_dir(fs), "/home/user") == 0);
	bd_fs_chdir(fs, "a/../b/./c");
	check("chdir normalizes . and ..",
	    strcmp(bd_fs_dir(fs), "/home/user/b/c") == 0);
	bd_fs_chdir(fs, "/etc");
	check("absolute chdir replaces the path",
	    strcmp(bd_fs_dir(fs), "/etc") == 0);
	bd_fs_chdir(fs, "..");
	bd_fs_chdir(fs, "..");
	check("up from root stays at root", strcmp(bd_fs_dir(fs), "/") == 0);

	bd_fs_free(fs);
}

/* ================================================================== */
/* Places sidebar over a fake platform                                */
/* ================================================================== */
static int fake_add_recent_calls;
static char fake_last_recent[256];

static int
fake_volumes(bd_fs_place **out, int *n)
{
	bd_fs_place *a = calloc(1, sizeof *a);
	if (!a) { *out = NULL; *n = 0; return -1; }
	a[0].label = strdup("Data");
	a[0].path = strdup("/mnt/data");
	a[0].kind = BD_FS_PLACE_VOLUME;
	a[0].is_dir = 1;
	*out = a;
	*n = 1;
	return 0;
}

static int
fake_recents(bd_fs_place **out, int *n)
{
	bd_fs_place *a = calloc(1, sizeof *a);
	if (!a) { *out = NULL; *n = 0; return -1; }
	a[0].label = strdup("notes.txt");
	a[0].path = strdup("/home/user/notes.txt");
	a[0].kind = BD_FS_PLACE_RECENT;
	a[0].is_dir = 0;
	*out = a;
	*n = 1;
	return 0;
}

static void
fake_add_recent(const char *path)
{
	fake_add_recent_calls++;
	snprintf(fake_last_recent, sizeof fake_last_recent, "%s", path);
}

static void
test_places(void)
{
	printf("bd_fs places (fake platform):\n");

	bd_fs_platform plat = {
		.scandir = fake_scandir,
		.known_folder = fake_home,	/* only HOME resolves */
		.normalize = bd_fs_platform_default()->normalize,
		.volumes = fake_volumes,
		.recents = fake_recents,
		.add_recent = fake_add_recent,
	};
	bd_fs *fs = bd_fs_open_ex("/home/user", &plat);

	int n = 0;
	bd_fs_place *pl = bd_fs_places(fs, &n);
	check("assembles home + recent shortcut + volume", n == 3);
	check("first place is Home folder",
	    pl && pl[0].kind == BD_FS_PLACE_FOLDER &&
	    strcmp(pl[0].label, "Home") == 0 &&
	    strcmp(pl[0].path, "/home/user") == 0);
	check("a single Recent shortcut, not the files",
	    n >= 2 && pl[1].kind == BD_FS_PLACE_RECENT &&
	    strcmp(pl[1].label, "Recent") == 0);
	check("volume follows",
	    n >= 3 && pl[2].kind == BD_FS_PLACE_VOLUME &&
	    strcmp(pl[2].path, "/mnt/data") == 0);
	bd_fs_places_free(pl, n);

	/* The Recent view lists the recent files (with their full paths). */
	bd_fs_show_recents(fs);
	check("recents view is active", bd_fs_is_recents(fs));
	check("dir label reads Recent", strcmp(bd_fs_dir(fs), "Recent") == 0);
	check("recents view lists the recent file",
	    bd_fs_count(fs) == 1 &&
	    strcmp(bd_fs_get(fs, 0)->name, "notes.txt") == 0 &&
	    bd_fs_get(fs, 0)->path != NULL &&
	    strcmp(bd_fs_get(fs, 0)->path, "/home/user/notes.txt") == 0);
	bd_fs_chdir(fs, "/tmp");
	check("navigating leaves the recents view", !bd_fs_is_recents(fs));

	bd_fs_add_recent(fs, "/home/user/session.log");
	check("add_recent forwards to the platform",
	    fake_add_recent_calls == 1 &&
	    strcmp(fake_last_recent, "/home/user/session.log") == 0);

	bd_fs_free(fs);
}

/* ================================================================== */
/* Real POSIX platform against a temporary directory                  */
/* ================================================================== */
#ifndef _WIN32
static void
write_file(const char *dir, const char *name, const char *body)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		ssize_t r = write(fd, body, strlen(body));
		(void)r;
		close(fd);
	}
}

static void
rm_file(const char *dir, const char *name)
{
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	unlink(path);
}

static void
test_posix(void)
{
	printf("bd_fs (real POSIX platform):\n");

	char tmpl[] = "/tmp/bd_fs_testXXXXXX";
	char *dir = mkdtemp(tmpl);
	if (!dir) {
		printf("  [SKIP] mkdtemp failed; skipping POSIX scandir test\n");
		return;
	}

	write_file(dir, "alpha.txt", "hello");
	write_file(dir, "beta.csv", "a,b,c");
	write_file(dir, ".dot", "x");
	char sub[1024];
	snprintf(sub, sizeof sub, "%s/nested", dir);
	mkdir(sub, 0755);

	bd_fs *fs = bd_fs_open(dir);
	check("open real dir", fs != NULL);
	check("scans 2 files + 1 dir (dotfile hidden)", bd_fs_count(fs) == 3);
	check("directory sorts first", bd_fs_get(fs, 0)->is_dir == 1 &&
	    strcmp(bd_fs_get(fs, 0)->name, "nested") == 0);

	const char *csv[] = { "*.csv" };
	bd_fs_set_filter(fs, csv, 1);
	check("filter keeps nested + beta.csv", bd_fs_count(fs) == 2);
	check("beta.csv size is 5 bytes",
	    bd_fs_get(fs, 1)->size == 5);
	bd_fs_set_filter(fs, NULL, 0);

	bd_fs_set_hidden(fs, 1);
	check("show hidden reveals .dot (4 total)", bd_fs_count(fs) == 4);

	/* Create a directory through the model and confirm it appears. */
	check("mkdir succeeds", bd_fs_mkdir(fs, "newdir") == 0);
	int seen = 0;
	for (int i = 0, c = bd_fs_count(fs); i < c; i++) {
		const bd_fs_entry *e = bd_fs_get(fs, i);
		if (e->is_dir && strcmp(e->name, "newdir") == 0)
			seen = 1;
	}
	check("the new directory appears in the listing", seen);

	bd_fs_free(fs);

	/* Real recents round-trip: register a file through the XBEL writer, with
	 * XDG_DATA_HOME pointed at the temp dir, then read it back. */
	setenv("XDG_DATA_HOME", dir, 1);
	const bd_fs_platform *P = bd_fs_platform_default();
	char alpha[1024];
	snprintf(alpha, sizeof alpha, "%s/alpha.txt", dir);
	P->add_recent(alpha);
	bd_fs_place *rec = NULL;
	int nrec = 0;
	int ok = (P->recents(&rec, &nrec) == 0);
	int found = 0;
	for (int i = 0; i < nrec; i++)
		if (strcmp(rec[i].path, alpha) == 0 && !rec[i].is_dir)
			found = 1;
	check("recents round-trips the registered file", ok && found);
	/* Registering the same file again must not duplicate it. */
	P->add_recent(alpha);
	bd_fs_place *rec2 = NULL;
	int nrec2 = 0;
	P->recents(&rec2, &nrec2);
	check("re-registering does not duplicate", nrec2 == nrec);
	bd_fs_places_free(rec, nrec);
	bd_fs_places_free(rec2, nrec2);
	rm_file(dir, "recently-used.xbel");

	rm_file(dir, "alpha.txt");
	rm_file(dir, "beta.csv");
	rm_file(dir, ".dot");
	char nd[1024];
	snprintf(nd, sizeof nd, "%s/newdir", dir);
	rmdir(nd);
	rmdir(sub);
	rmdir(dir);
}
#endif

int
main(void)
{
	test_fake();
	test_places();
#ifndef _WIN32
	test_posix();
#endif
	printf("\n%d checks, %d failures\n", checks, fails);
	return fails ? 1 : 0;
}
