#ifndef BD_ASSET_H
#define BD_ASSET_H

#include "bd_backend.h"
#include <stddef.h>

/*
 * Backend-neutral registry for the toolkit's runtime assets. The toolkit asks
 * for each asset by a generic, family-neutral IDENTIFIER (the BD_ASSET_* names
 * below), not by a filename. Register a source under an id to replace the
 * built-in default: a custom UI font is just
 *
 *     bd_asset_register_file(BD_ASSET_FONT_REGULAR, "MyFont.otf");
 *
 * with no need to name it after the stock font or to override build macros. A
 * source is EITHER a filesystem path or an in-memory blob, so the same id can be
 * fed a file (loaded on demand) or bytes compiled into the binary (an .incbin or
 * `xxd -i` array). Nothing registered for an id falls back to that asset's
 * default file, so the default build reads its assets from disk unchanged.
 *
 * In-memory blobs and path strings are BORROWED: they must outlive every use,
 * which a .rodata blob satisfies.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* The eight chrome font faces (a proportional family and a fixed-width "mono"
 * family, each Regular/Bold/Italic/BoldItalic), the terminal glyph atlas, and
 * the two pushpin sprites. These names are stable identities, not filenames. */
#define BD_ASSET_FONT_REGULAR          "font.regular"
#define BD_ASSET_FONT_BOLD             "font.bold"
#define BD_ASSET_FONT_ITALIC           "font.italic"
#define BD_ASSET_FONT_BOLD_ITALIC      "font.bold-italic"
#define BD_ASSET_FONT_MONO             "font.mono"
#define BD_ASSET_FONT_MONO_BOLD        "font.mono-bold"
#define BD_ASSET_FONT_MONO_ITALIC      "font.mono-italic"
#define BD_ASSET_FONT_MONO_BOLD_ITALIC "font.mono-bold-italic"
#define BD_ASSET_TERMINAL_FONT         "terminal.font"
#define BD_ASSET_PUSHPIN_OUT           "pushpin.out"
#define BD_ASSET_PUSHPIN_IN            "pushpin.in"

/* A resolved asset source: exactly one of `path` or `data` is set. */
typedef struct {
	const char          *path;   /* file source, else NULL */
	const unsigned char *data;   /* in-memory source, else NULL */
	size_t               len;    /* byte length of data */
} bd_asset;

/* Register an in-memory blob (bytes) or a filesystem path under `id`. `id`, and
 * for the file form `path`, and for the data form `data`, are all borrowed and
 * must outlive every use. Registering the same id again replaces it. */
void bd_asset_register_data(const char *id, const void *data, size_t len);
void bd_asset_register_file(const char *id, const char *path);

/* Look up the source registered for `id`. Returns 1 and fills *out on a hit, or
 * 0 when nothing is registered, so callers fall back to the built-in default. */
int  bd_asset_lookup(const char *id, bd_asset *out);

/* Forget every registered asset. */
void bd_asset_clear(void);

/* Resolve `rel` (an asset-root-relative sub-path, e.g. "fonts/DejaVuSans.ttf")
 * through the backend's resolve_asset hook, writing the located absolute path
 * into the caller-owned `buf` (of `bufsz` bytes) and returning `buf`. Returns
 * `fallback` (unchanged, not copied into buf) when the backend has no hook or
 * does not find the asset. The caller owns `buf`, so there is no shared state
 * or lifetime to track. */
const char *bd_asset_resolve(const bd_backend *be, const char *rel,
    const char *fallback, char *buf, size_t bufsz);

/* Resolve a texture asset and upload it through the backend: registered data
 * (decoded from memory), a registered file, or the default when the id is
 * unregistered -- the default is `rel` located via the backend's resolve_asset
 * hook, falling back to `default_path` (current-directory-relative). Used by
 * the toolkit for the pushpins and terminal atlas. */
bd_texture bd_asset_texture(const bd_backend *be, const char *id,
    const char *rel, const char *default_path);

#endif /* BD_ASSET_H */
