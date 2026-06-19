#ifndef BD_PROFILE_H
#define BD_PROFILE_H

#include <stddef.h>

/*
 * bd_profile -- the MUD list (doc/profiles.md).
 *
 * A profile is a property list: an unordered bag of (key, value) string pairs
 * describing one MUD. Values are always strings (callers parse ints/bools on
 * read), which matches the CSV wire format and means unknown columns from an
 * imported list round-trip losslessly. A bd_profiles store holds many profiles
 * keyed by their unique "name".
 *
 * The store is the import/export format for sharing lists and the persistence
 * record on disk; "save" is "export with no filter".
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* The default safe column set: what may leave or enter the client unless the
 * user opts custom columns in. Passed as the filter for foreign import/export;
 * sensitive fields (character, notes, on_connect, password) are absent. */
#define BD_PROFILE_SAFE_COLUMNS \
	"name,host,port,tls,encoding,description,url,tags"

typedef struct bd_profile bd_profile;
typedef struct bd_profiles bd_profiles;

/* ---- single profile (property list) ---- */

/* Value for key, or NULL if unset. Valid until the next set on this profile. */
const char *bd_profile_get(const bd_profile *p, const char *key);
/* Set key to val (copied). val == NULL removes the key. */
void bd_profile_set(bd_profile *p, const char *key, const char *val);
/* Iterate keys in insertion order. */
int bd_profile_count(const bd_profile *p);
const char *bd_profile_key(const bd_profile *p, int i);
const char *bd_profile_val(const bd_profile *p, int i);

/* ---- the store ---- */

bd_profiles *bd_profiles_new(void);
void bd_profiles_free(bd_profiles *ps);

int bd_profiles_count(const bd_profiles *ps);
bd_profile *bd_profiles_at(bd_profiles *ps, int i);
/* Find by "name" (case-sensitive), or NULL. */
bd_profile *bd_profiles_find(bd_profiles *ps, const char *name);
/* Find or create a profile with the given name. */
bd_profile *bd_profiles_add(bd_profiles *ps, const char *name);
/* Remove a profile by name. Returns 1 if removed, 0 if not found. */
int bd_profiles_remove(bd_profiles *ps, const char *name);

/* ---- CSV import / export ---- */

/*
 * Import CSV text (first row is the title header). Rows are merged into the
 * store by their "name" column; an existing profile of the same name is
 * updated. Columns are accepted only if listed in filter (comma-separated);
 * pass NULL to accept every column (used when loading our own file). Rows with
 * no "name" are skipped. Returns the number of profiles imported/updated, or
 * -1 on a parse/allocation error.
 */
int bd_profiles_import_csv(bd_profiles *ps, const char *data, size_t len,
                           const char *filter);

/*
 * Export the store as CSV. The header is the union of keys (across all
 * profiles) that pass filter, in a stable order with the safe columns first.
 * Pass NULL to export every column (used when saving our own file). Returns a
 * malloc'd NUL-terminated string (caller frees); *len gets the byte length.
 * Returns NULL on allocation failure.
 */
char *bd_profiles_export_csv(const bd_profiles *ps, const char *filter,
                             size_t *len);

/* Load from / save to a file path. Load accepts all columns; save writes all
 * columns (no filter). Return 0 on success, -1 on error. */
int bd_profiles_load(bd_profiles *ps, const char *path);
int bd_profiles_save(const bd_profiles *ps, const char *path);

#endif /* BD_PROFILE_H */
