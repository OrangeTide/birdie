#ifndef BD_CSV_H
#define BD_CSV_H

#include <stddef.h>

/*
 * bd_csv -- a small in-memory CSV reader and writer (RFC 4180 style).
 *
 * Parses from a memory buffer (so files, clipboard text, and fetched gist
 * blobs all funnel through the same path) and handles quoted fields with
 * embedded commas, quotes (doubled ""), and newlines. The writer quotes
 * fields only when they need it. Used by bd_profile for the MUD list.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* ---- reader ---- */

typedef struct bd_csv bd_csv;

/* Parse len bytes of CSV. Returns a document (free with bd_csv_free) or NULL
 * on allocation failure. A trailing newline does not create an empty row. */
bd_csv *bd_csv_parse(const char *data, size_t len);
void bd_csv_free(bd_csv *c);

int bd_csv_rows(const bd_csv *c);
int bd_csv_cols(const bd_csv *c, int row);
/* Field text (NUL-terminated), or NULL if row/col is out of range. */
const char *bd_csv_get(const bd_csv *c, int row, int col);

/* ---- writer ---- */

typedef struct bd_csv_w bd_csv_w;

bd_csv_w *bd_csv_w_new(void);
void bd_csv_w_free(bd_csv_w *w);

/* Append one field to the current row, quoting/escaping as needed. */
void bd_csv_w_field(bd_csv_w *w, const char *s);
/* End the current row (emits CRLF). */
void bd_csv_w_endrow(bd_csv_w *w);

/* The accumulated CSV text, owned by the writer (valid until free). If len is
 * non-NULL it receives the byte length. */
const char *bd_csv_w_str(bd_csv_w *w, size_t *len);

#endif /* BD_CSV_H */
