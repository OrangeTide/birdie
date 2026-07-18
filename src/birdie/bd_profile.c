/*
 * bd_profile -- the MUD list. See bd_profile.h and doc/profiles.md.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_profile.h"
#include "bd_csv.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct kv {
	char *key;
	char *val;
};

struct bd_profile {
	struct kv *kv;
	int n, cap;
};

struct bd_profiles {
	bd_profile **prof;
	int n, cap;
};

/* ---- single profile ---- */

const char *
bd_profile_get(const bd_profile *p, const char *key)
{
	int i;
	if (!p || !key)
		return NULL;
	for (i = 0; i < p->n; i++)
		if (strcmp(p->kv[i].key, key) == 0)
			return p->kv[i].val;
	return NULL;
}

void
bd_profile_set(bd_profile *p, const char *key, const char *val)
{
	int i;

	if (!p || !key)
		return;
	for (i = 0; i < p->n; i++) {
		if (strcmp(p->kv[i].key, key) != 0)
			continue;
		if (!val) {                     /* remove */
			free(p->kv[i].key);
			free(p->kv[i].val);
			p->kv[i] = p->kv[--p->n];
			return;
		}
		free(p->kv[i].val);
		p->kv[i].val = strdup(val);
		return;
	}
	if (!val)
		return;
	if (p->n == p->cap) {
		int nc = p->cap ? p->cap * 2 : 8;
		struct kv *t = realloc(p->kv, (size_t)nc * sizeof *t);
		if (!t)
			return;
		p->kv = t;
		p->cap = nc;
	}
	p->kv[p->n].key = strdup(key);
	p->kv[p->n].val = strdup(val);
	p->n++;
}

int
bd_profile_count(const bd_profile *p)
{
	return p ? p->n : 0;
}

const char *
bd_profile_key(const bd_profile *p, int i)
{
	return (p && i >= 0 && i < p->n) ? p->kv[i].key : NULL;
}

const char *
bd_profile_val(const bd_profile *p, int i)
{
	return (p && i >= 0 && i < p->n) ? p->kv[i].val : NULL;
}

static void
profile_free(bd_profile *p)
{
	int i;
	if (!p)
		return;
	for (i = 0; i < p->n; i++) {
		free(p->kv[i].key);
		free(p->kv[i].val);
	}
	free(p->kv);
	free(p);
}

/* ---- store ---- */

bd_profiles *
bd_profiles_new(void)
{
	return calloc(1, sizeof(bd_profiles));
}

void
bd_profiles_free(bd_profiles *ps)
{
	int i;
	if (!ps)
		return;
	for (i = 0; i < ps->n; i++)
		profile_free(ps->prof[i]);
	free(ps->prof);
	free(ps);
}

int
bd_profiles_count(const bd_profiles *ps)
{
	return ps ? ps->n : 0;
}

bd_profile *
bd_profiles_at(bd_profiles *ps, int i)
{
	return (ps && i >= 0 && i < ps->n) ? ps->prof[i] : NULL;
}

bd_profile *
bd_profiles_find(bd_profiles *ps, const char *name)
{
	int i;
	if (!ps || !name)
		return NULL;
	for (i = 0; i < ps->n; i++) {
		const char *nm = bd_profile_get(ps->prof[i], "name");
		if (nm && strcmp(nm, name) == 0)
			return ps->prof[i];
	}
	return NULL;
}

bd_profile *
bd_profiles_add(bd_profiles *ps, const char *name)
{
	bd_profile *p;

	if (!ps || !name || !*name)
		return NULL;
	p = bd_profiles_find(ps, name);
	if (p)
		return p;
	p = calloc(1, sizeof *p);
	if (!p)
		return NULL;
	bd_profile_set(p, "name", name);
	if (ps->n == ps->cap) {
		int nc = ps->cap ? ps->cap * 2 : 16;
		bd_profile **t = realloc(ps->prof, (size_t)nc * sizeof *t);
		if (!t) {
			profile_free(p);
			return NULL;
		}
		ps->prof = t;
		ps->cap = nc;
	}
	ps->prof[ps->n++] = p;
	return p;
}

int
bd_profiles_remove(bd_profiles *ps, const char *name)
{
	int i;
	if (!ps || !name)
		return 0;
	for (i = 0; i < ps->n; i++) {
		const char *nm = bd_profile_get(ps->prof[i], "name");
		if (nm && strcmp(nm, name) == 0) {
			profile_free(ps->prof[i]);
			ps->prof[i] = ps->prof[--ps->n];
			return 1;
		}
	}
	return 0;
}

/* ---- filter helper ---- */

/* Is column `name` allowed by `filter` (a comma-separated list)? NULL filter
 * allows everything. Surrounding spaces on a token are ignored. */
static int
filter_allows(const char *filter, const char *name)
{
	size_t nl;
	const char *p;

	if (!filter)
		return 1;
	nl = strlen(name);
	p = filter;
	while (*p) {
		const char *start, *end;
		while (*p == ' ' || *p == ',')
			p++;
		start = p;
		while (*p && *p != ',')
			p++;
		end = p;
		while (end > start && end[-1] == ' ')
			end--;
		if ((size_t)(end - start) == nl &&
		    strncmp(start, name, nl) == 0)
			return 1;
	}
	return 0;
}

/* ---- import ---- */

int
bd_profiles_import_csv(bd_profiles *ps, const char *data, size_t len,
                       const char *filter)
{
	bd_csv *csv;
	int r, c, ncols, name_col = -1, imported = 0;

	if (!ps || !data)
		return -1;
	csv = bd_csv_parse(data, len);
	if (!csv)
		return -1;
	if (bd_csv_rows(csv) < 1) {
		bd_csv_free(csv);
		return 0;
	}

	ncols = bd_csv_cols(csv, 0);
	for (c = 0; c < ncols; c++) {
		const char *t = bd_csv_get(csv, 0, c);
		if (t && strcmp(t, "name") == 0) {
			name_col = c;
			break;
		}
	}
	if (name_col < 0) {             /* no key column: nothing to import */
		bd_csv_free(csv);
		return 0;
	}

	for (r = 1; r < bd_csv_rows(csv); r++) {
		const char *name = bd_csv_get(csv, r, name_col);
		bd_profile *p;
		if (!name || !*name)
			continue;
		p = bd_profiles_add(ps, name);
		if (!p)
			continue;
		for (c = 0; c < ncols; c++) {
			const char *title = bd_csv_get(csv, 0, c);
			const char *val = bd_csv_get(csv, r, c);
			if (!title || !*title || c == name_col)
				continue;
			if (!filter_allows(filter, title))
				continue;
			if (val && *val)
				bd_profile_set(p, title, val);
		}
		imported++;
	}

	bd_csv_free(csv);
	return imported;
}

/* ---- export ---- */

static int
any_profile_has(const bd_profiles *ps, const char *key)
{
	int i;
	for (i = 0; i < ps->n; i++)
		if (bd_profile_get(ps->prof[i], key))
			return 1;
	return 0;
}

static int
strlist_has(char **list, int n, const char *s)
{
	int i;
	for (i = 0; i < n; i++)
		if (strcmp(list[i], s) == 0)
			return 1;
	return 0;
}

char *
bd_profiles_export_csv(const bd_profiles *ps, const char *filter, size_t *len)
{
	bd_csv_w *w;
	char **cols = NULL;
	int ncols = 0, ccap = 0, i, k;
	const char *safe = BD_PROFILE_SAFE_COLUMNS;
	char *safedup, *tok, *save;
	const char *out;
	char *ret = NULL;
	size_t outlen;

	if (!ps)
		return NULL;
	w = bd_csv_w_new();
	if (!w)
		return NULL;

#define ADD_COL(name) do {                                             \
		if (filter_allows(filter, (name)) &&                   \
		    any_profile_has(ps, (name)) &&                     \
		    !strlist_has(cols, ncols, (name))) {               \
			if (ncols == ccap) {                           \
				int nc = ccap ? ccap * 2 : 16;         \
				char **t = realloc(cols,               \
				    (size_t)nc * sizeof *t);           \
				if (t) { cols = t; ccap = nc; }        \
			}                                              \
			if (ncols < ccap)                              \
				cols[ncols++] = strdup(name);          \
		}                                                     \
	} while (0)

	/* safe columns first, in their canonical order */
	safedup = strdup(safe);
	if (safedup) {
		for (tok = strtok_r(safedup, ",", &save); tok;
		     tok = strtok_r(NULL, ",", &save))
			ADD_COL(tok);
		free(safedup);
	}
	/* then any remaining (custom) columns, in first-seen order */
	for (i = 0; i < ps->n; i++) {
		const bd_profile *p = ps->prof[i];
		for (k = 0; k < bd_profile_count(p); k++)
			ADD_COL(bd_profile_key(p, k));
	}

	/* header */
	for (i = 0; i < ncols; i++)
		bd_csv_w_field(w, cols[i]);
	bd_csv_w_endrow(w);

	/* rows */
	for (i = 0; i < ps->n; i++) {
		for (k = 0; k < ncols; k++) {
			const char *v = bd_profile_get(ps->prof[i], cols[k]);
			bd_csv_w_field(w, v ? v : "");
		}
		bd_csv_w_endrow(w);
	}

	out = bd_csv_w_str(w, &outlen);
	if (out) {
		ret = malloc(outlen + 1);
		if (ret) {
			memcpy(ret, out, outlen + 1);
			if (len)
				*len = outlen;
		}
	}

	for (i = 0; i < ncols; i++)
		free(cols[i]);
	free(cols);
	bd_csv_w_free(w);
	return ret;
#undef ADD_COL
}

/* ---- file I/O ---- */

int
bd_profiles_load(bd_profiles *ps, const char *path)
{
	FILE *f;
	char *data;
	long sz;
	size_t got;
	int rc;

	if (!ps || !path)
		return -1;
	f = fopen(path, "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz < 0) {
		fclose(f);
		return -1;
	}
	data = malloc((size_t)sz + 1);
	if (!data) {
		fclose(f);
		return -1;
	}
	got = fread(data, 1, (size_t)sz, f);
	fclose(f);
	data[got] = '\0';
	rc = bd_profiles_import_csv(ps, data, got, NULL);
	free(data);
	return rc < 0 ? -1 : 0;
}

int
bd_profiles_save(const bd_profiles *ps, const char *path)
{
	FILE *f;
	char *csv;
	size_t len;
	size_t wr;

	if (!ps || !path)
		return -1;
	csv = bd_profiles_export_csv(ps, NULL, &len);
	if (!csv)
		return -1;
	f = fopen(path, "wb");
	if (!f) {
		free(csv);
		return -1;
	}
	wr = fwrite(csv, 1, len, f);
	fclose(f);
	free(csv);
	return wr == len ? 0 : -1;
}

/* ---- merging one store into another ---- */

/* Copy every key from src into dst. force_name (if set) replaces src's name,
 * for the rename policy. */
static void
copy_profile_keys(bd_profile *dst, const bd_profile *src, const char *force_name)
{
	int i, n = bd_profile_count(src);
	for (i = 0; i < n; i++) {
		const char *k = bd_profile_key(src, i);
		if (force_name && !strcmp(k, "name"))
			continue;
		bd_profile_set(dst, k, bd_profile_val(src, i));
	}
	if (force_name)
		bd_profile_set(dst, "name", force_name);
}

/* "base (2)", "base (3)", ... : the first that no profile in ps already uses. */
static void
unique_profile_name(bd_profiles *ps, const char *base, char *out, size_t outsz)
{
	int i;
	for (i = 2; i < 100000; i++) {
		snprintf(out, outsz, "%s (%d)", base, i);
		if (!bd_profiles_find(ps, out))
			return;
	}
}

int
bd_profiles_count_collisions(bd_profiles *dst, bd_profiles *src)
{
	int i, c = 0, n = bd_profiles_count(src);
	for (i = 0; i < n; i++) {
		const char *name = bd_profile_get(bd_profiles_at(src, i), "name");
		if (name && *name && bd_profiles_find(dst, name))
			c++;
	}
	return c;
}

int
bd_profiles_merge(bd_profiles *dst, bd_profiles *src, int policy)
{
	int i, changed = 0, n = bd_profiles_count(src);
	for (i = 0; i < n; i++) {
		bd_profile *sp = bd_profiles_at(src, i);
		const char *name = bd_profile_get(sp, "name");
		bd_profile *ex, *np;
		if (!name || !*name)
			continue;
		ex = bd_profiles_find(dst, name);
		if (!ex) {                          /* no clash: add as-is */
			np = bd_profiles_add(dst, name);
			if (np) { copy_profile_keys(np, sp, NULL); changed++; }
		} else if (policy == BD_IMPORT_SKIP) {
			continue;                       /* keep the existing one */
		} else if (policy == BD_IMPORT_RENAME) {
			char nm[160];
			unique_profile_name(dst, name, nm, sizeof nm);
			np = bd_profiles_add(dst, nm);
			if (np) { copy_profile_keys(np, sp, nm); changed++; }
		} else {                            /* BD_IMPORT_OVERWRITE (merge) */
			copy_profile_keys(ex, sp, NULL);
			changed++;
		}
	}
	return changed;
}
