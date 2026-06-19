/*
 * bd_csv -- in-memory CSV reader/writer. See bd_csv.h for the contract.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_csv.h"

#include <stdlib.h>
#include <string.h>

/* ---- a tiny growable byte buffer ---- */

struct buf {
	char *p;
	size_t len, cap;
	int oom;
};

static void
buf_putc(struct buf *b, char c)
{
	if (b->oom)
		return;
	if (b->len + 1 > b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 64;
		char *np = realloc(b->p, nc);
		if (!np) {
			b->oom = 1;
			return;
		}
		b->p = np;
		b->cap = nc;
	}
	b->p[b->len++] = c;
}

static void
buf_puts(struct buf *b, const char *s)
{
	while (*s)
		buf_putc(b, *s++);
}

/* ---- reader ---- */

struct bd_csv {
	char ***rows;           /* rows[r][c] -> field string */
	int *ncols;
	int nrows, cap;
};

static int
csv_add_row(bd_csv *c, char **cols, int ncols)
{
	if (c->nrows == c->cap) {
		int nc = c->cap ? c->cap * 2 : 16;
		char ***nr = realloc(c->rows, (size_t)nc * sizeof *nr);
		int *nn = realloc(c->ncols, (size_t)nc * sizeof *nn);
		if (nr)
			c->rows = nr;
		if (nn)
			c->ncols = nn;
		if (!nr || !nn)
			return -1;
		c->cap = nc;
	}
	c->rows[c->nrows] = cols;
	c->ncols[c->nrows] = ncols;
	c->nrows++;
	return 0;
}

bd_csv *
bd_csv_parse(const char *data, size_t len)
{
	bd_csv *c = calloc(1, sizeof *c);
	size_t i = 0;
	char **cols = NULL;
	int ncols = 0, ccap = 0;
	struct buf field = { 0 };
	int in_quotes = 0, row_started = 0, field_started = 0;

	if (!c)
		return NULL;

	/* push the accumulated field onto the current row */
#define FLUSH_FIELD() do {                                              \
		char *s = malloc(field.len + 1);                       \
		if (!s) goto oom;                                      \
		if (field.len) memcpy(s, field.p, field.len);          \
		s[field.len] = '\0';                                   \
		if (ncols == ccap) {                                   \
			int nn = ccap ? ccap * 2 : 8;                 \
			char **t = realloc(cols, (size_t)nn * sizeof *t); \
			if (!t) { free(s); goto oom; }                \
			cols = t; ccap = nn;                          \
		}                                                     \
		cols[ncols++] = s;                                    \
		field.len = 0; field_started = 0;                     \
	} while (0)

#define END_ROW() do {                                                 \
		if (csv_add_row(c, cols, ncols) < 0) goto oom;        \
		cols = NULL; ncols = 0; ccap = 0;                     \
		row_started = 0;                                      \
	} while (0)

	while (i < len) {
		char ch = data[i];

		if (in_quotes) {
			if (ch == '"') {
				if (i + 1 < len && data[i + 1] == '"') {
					buf_putc(&field, '"');
					i += 2;
				} else {
					in_quotes = 0;
					i++;
				}
			} else {
				buf_putc(&field, ch);
				i++;
			}
			continue;
		}

		if (ch == '"' && !field_started) {
			in_quotes = 1;
			field_started = 1;
			row_started = 1;
			i++;
		} else if (ch == ',') {
			FLUSH_FIELD();
			row_started = 1;
			i++;
		} else if (ch == '\n' || ch == '\r') {
			/* end of record (collapse \r\n) */
			if (row_started || field_started || field.len) {
				FLUSH_FIELD();
				END_ROW();
			}
			if (ch == '\r' && i + 1 < len && data[i + 1] == '\n')
				i++;
			i++;
		} else {
			buf_putc(&field, ch);
			field_started = 1;
			row_started = 1;
			i++;
		}
		if (field.oom)
			goto oom;
	}
	/* trailing field/row with no final newline */
	if (row_started || field_started || field.len || ncols) {
		FLUSH_FIELD();
		END_ROW();
	}

	free(field.p);
	return c;

oom:
	free(field.p);
	{
		int j;
		for (j = 0; j < ncols; j++)
			free(cols[j]);
		free(cols);
	}
	bd_csv_free(c);
	return NULL;
#undef FLUSH_FIELD
#undef END_ROW
}

void
bd_csv_free(bd_csv *c)
{
	int r, k;
	if (!c)
		return;
	for (r = 0; r < c->nrows; r++) {
		for (k = 0; k < c->ncols[r]; k++)
			free(c->rows[r][k]);
		free(c->rows[r]);
	}
	free(c->rows);
	free(c->ncols);
	free(c);
}

int
bd_csv_rows(const bd_csv *c)
{
	return c ? c->nrows : 0;
}

int
bd_csv_cols(const bd_csv *c, int row)
{
	if (!c || row < 0 || row >= c->nrows)
		return 0;
	return c->ncols[row];
}

const char *
bd_csv_get(const bd_csv *c, int row, int col)
{
	if (!c || row < 0 || row >= c->nrows)
		return NULL;
	if (col < 0 || col >= c->ncols[row])
		return NULL;
	return c->rows[row][col];
}

/* ---- writer ---- */

struct bd_csv_w {
	struct buf b;
	int col;        /* fields written on the current row */
};

bd_csv_w *
bd_csv_w_new(void)
{
	return calloc(1, sizeof(bd_csv_w));
}

void
bd_csv_w_free(bd_csv_w *w)
{
	if (!w)
		return;
	free(w->b.p);
	free(w);
}

void
bd_csv_w_field(bd_csv_w *w, const char *s)
{
	int needq;
	const char *t;

	if (!w)
		return;
	if (!s)
		s = "";
	if (w->col)
		buf_putc(&w->b, ',');
	w->col++;

	needq = 0;
	for (t = s; *t; t++) {
		if (*t == ',' || *t == '"' || *t == '\n' || *t == '\r') {
			needq = 1;
			break;
		}
	}
	if (!needq) {
		buf_puts(&w->b, s);
		return;
	}
	buf_putc(&w->b, '"');
	for (t = s; *t; t++) {
		if (*t == '"')
			buf_putc(&w->b, '"');     /* double it */
		buf_putc(&w->b, *t);
	}
	buf_putc(&w->b, '"');
}

void
bd_csv_w_endrow(bd_csv_w *w)
{
	if (!w)
		return;
	buf_putc(&w->b, '\r');
	buf_putc(&w->b, '\n');
	w->col = 0;
}

const char *
bd_csv_w_str(bd_csv_w *w, size_t *len)
{
	if (!w)
		return NULL;
	buf_putc(&w->b, '\0');          /* NUL-terminate for C string use */
	w->b.len--;                     /* but don't count it in the length */
	if (len)
		*len = w->b.len;
	return w->b.p ? w->b.p : "";
}
