/*
 * bd_replay -- scrollback replay from NDJSON logs. See bd_replay.h.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>

/* ---- minimal JSON string-field extraction ---- */

/* Append the UTF-8 encoding of code point `cp` to out[*o] (cap-bounded). */
static void
put_utf8(char *out, size_t cap, size_t *o, unsigned cp)
{
	if (cp < 0x80) {
		if (*o + 1 < cap) out[(*o)++] = (char)cp;
	} else if (cp < 0x800) {
		if (*o + 2 < cap) {
			out[(*o)++] = (char)(0xc0 | (cp >> 6));
			out[(*o)++] = (char)(0x80 | (cp & 0x3f));
		}
	} else {
		if (*o + 3 < cap) {
			out[(*o)++] = (char)(0xe0 | (cp >> 12));
			out[(*o)++] = (char)(0x80 | ((cp >> 6) & 0x3f));
			out[(*o)++] = (char)(0x80 | (cp & 0x3f));
		}
	}
}

/* Decode a JSON string starting at `p` (just past the opening quote) into
 * out[cap]. Returns a pointer just past the closing quote, or NULL on error. */
static const char *
decode_json_string(const char *p, char *out, size_t cap)
{
	size_t o = 0;
	while (*p) {
		unsigned char c = (unsigned char)*p;
		if (c == '"') {
			out[o < cap ? o : cap - 1] = '\0';
			return p + 1;
		}
		if (c == '\\') {
			p++;
			switch (*p) {
			case 'n': if (o + 1 < cap) out[o++] = '\n'; break;
			case 't': if (o + 1 < cap) out[o++] = '\t'; break;
			case 'r': if (o + 1 < cap) out[o++] = '\r'; break;
			case 'b': if (o + 1 < cap) out[o++] = '\b'; break;
			case 'f': if (o + 1 < cap) out[o++] = '\f'; break;
			case '/': if (o + 1 < cap) out[o++] = '/'; break;
			case '"': if (o + 1 < cap) out[o++] = '"'; break;
			case '\\': if (o + 1 < cap) out[o++] = '\\'; break;
			case 'u': {
				unsigned cp = 0;
				int k;
				for (k = 1; k <= 4; k++) {
					char h = p[k];
					cp <<= 4;
					if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
					else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
					else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
					else { cp = '?'; break; }
				}
				put_utf8(out, cap, &o, cp);
				p += 4;
				break;
			}
			default:
				if (*p && o + 1 < cap) out[o++] = *p;
				break;
			}
			if (!*p)
				break;
			p++;
			continue;
		}
		if (o + 1 < cap)
			out[o++] = (char)c;
		p++;
	}
	out[o < cap ? o : cap - 1] = '\0';
	return NULL;            /* unterminated */
}

int
bd_replay_json_str(const char *line, const char *key, char *out, size_t cap)
{
	char pat[64];
	const char *p;
	size_t kl;

	if (!line || !key || !out || cap == 0)
		return 0;
	out[0] = '\0';
	/* search for "<key>" followed by optional ws, ':', optional ws, '"' */
	kl = (size_t)snprintf(pat, sizeof pat, "\"%s\"", key);
	for (p = line; (p = strstr(p, pat)) != NULL; p += kl) {
		const char *q = p + kl;
		while (*q == ' ' || *q == '\t')
			q++;
		if (*q != ':')
			continue;
		q++;
		while (*q == ' ' || *q == '\t')
			q++;
		if (*q != '"')
			return 0;       /* value is not a string */
		return decode_json_string(q + 1, out, cap) != NULL;
	}
	return 0;
}

/* ---- bucket-file discovery ---- */

struct strvec {
	char **v;
	int n, cap;
};

static int
sv_push(struct strvec *s, const char *str)
{
	if (s->n == s->cap) {
		int nc = s->cap ? s->cap * 2 : 16;
		char **nv = realloc(s->v, (size_t)nc * sizeof *nv);
		if (!nv)
			return -1;
		s->v = nv;
		s->cap = nc;
	}
	s->v[s->n] = strdup(str);
	if (!s->v[s->n])
		return -1;
	s->n++;
	return 0;
}

static void
sv_free(struct strvec *s)
{
	int i;
	for (i = 0; i < s->n; i++)
		free(s->v[i]);
	free(s->v);
}

static int
cmp_str(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int
is_ndjson(const char *name)
{
	size_t n = strlen(name);
	return n > 7 && strcmp(name + n - 7, ".ndjson") == 0;
}

/* Collect full paths of *.ndjson bucket files for (mud, character) under root,
 * sorted ascending (chronological, since the path embeds year then bucket). */
static void
collect_buckets(const char *root, const char *mud, const char *character,
                struct strvec *out)
{
	DIR *d = opendir(root);
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d)) != NULL) {       /* each entry is a year dir */
		char dir[1024];
		DIR *cd;
		struct dirent *fe;

		if (e->d_name[0] == '.')
			continue;
		snprintf(dir, sizeof dir, "%s/%s/%s/%s",
		    root, e->d_name, mud, character);
		cd = opendir(dir);
		if (!cd)
			continue;
		while ((fe = readdir(cd)) != NULL) {
			char path[1024 + 260];
			if (!is_ndjson(fe->d_name))
				continue;
			snprintf(path, sizeof path, "%s/%s", dir, fe->d_name);
			sv_push(out, path);
		}
		closedir(cd);
	}
	closedir(d);
	if (out->n > 1)
		qsort(out->v, (size_t)out->n, sizeof *out->v, cmp_str);
}

/* ---- replay ---- */

int
bd_replay_recv(const char *root, const char *mud, const char *character,
               int max, bd_replay_emit_fn emit, void *ctx)
{
	struct strvec buckets = {0};
	struct strvec lines = {0};      /* collected newest-first */
	int fi, i, count;

	if (!root || !emit || max <= 0)
		return -1;
	if (!mud || !*mud)
		mud = "_";
	if (!character || !*character)
		character = "_";

	collect_buckets(root, mud, character, &buckets);

	/* Walk files newest-first; within each, take recv `raw` lines from the
	 * tail. Stop once we have `max`. */
	for (fi = buckets.n - 1; fi >= 0 && lines.n < max; fi--) {
		FILE *f = fopen(buckets.v[fi], "rb");
		struct strvec file_raws = {0};
		char *buf = NULL;
		size_t bcap = 0;
		ssize_t got;

		if (!f)
			continue;
		while ((got = getline(&buf, &bcap, f)) != -1) {
			char kind[16], raw[8192];
			if (!bd_replay_json_str(buf, "kind", kind, sizeof kind))
				continue;
			if (strcmp(kind, "recv") != 0)
				continue;
			if (bd_replay_json_str(buf, "raw", raw, sizeof raw))
				sv_push(&file_raws, raw);
		}
		free(buf);
		fclose(f);
		/* append this file's lines newest-first to the result */
		for (i = file_raws.n - 1; i >= 0 && lines.n < max; i--)
			sv_push(&lines, file_raws.v[i]);
		sv_free(&file_raws);
	}

	/* `lines` is newest-first; emit oldest-first */
	count = lines.n;
	for (i = count - 1; i >= 0; i--)
		emit(lines.v[i], strlen(lines.v[i]), ctx);

	sv_free(&lines);
	sv_free(&buckets);
	return count;
}
