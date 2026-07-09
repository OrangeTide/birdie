#include "bd_asset.h"
#include <string.h>
#include <stdio.h>

/*
 * A small fixed-capacity table of borrowed sources keyed by generic id. Each
 * entry is either data-backed (data != NULL) or file-backed (path != NULL). No
 * allocation and nothing to free: the caller owns the bytes and the strings.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#define BD_ASSET_MAX 32

static struct entry {
	const char          *id;     /* borrowed */
	const char          *path;   /* borrowed, else NULL */
	const unsigned char *data;   /* borrowed, else NULL */
	size_t               len;
} assets[BD_ASSET_MAX];
static int asset_count;

static struct entry *
find(const char *id)
{
	for (int i = 0; i < asset_count; i++)
		if (strcmp(assets[i].id, id) == 0)
			return &assets[i];
	return NULL;
}

/* Reserve (or reuse) the slot for `id`, cleared of any previous source. */
static struct entry *
slot(const char *id)
{
	struct entry *e = find(id);
	if (!e) {
		if (asset_count >= BD_ASSET_MAX) {
			fprintf(stderr, "bd_asset: registry full, dropping '%s'\n",
			    id);
			return NULL;
		}
		e = &assets[asset_count++];
		e->id = id;
	}
	e->path = NULL;
	e->data = NULL;
	e->len = 0;
	return e;
}

void
bd_asset_register_data(const char *id, const void *data, size_t len)
{
	struct entry *e;
	if (!id || !data || !(e = slot(id)))
		return;
	e->data = data;
	e->len = len;
}

void
bd_asset_register_file(const char *id, const char *path)
{
	struct entry *e;
	if (!id || !path || !(e = slot(id)))
		return;
	e->path = path;
}

int
bd_asset_lookup(const char *id, bd_asset *out)
{
	struct entry *e = id ? find(id) : NULL;
	if (!e)
		return 0;
	if (out) {
		out->path = e->path;
		out->data = e->data;
		out->len = e->len;
	}
	return 1;
}

void
bd_asset_clear(void)
{
	asset_count = 0;
}

const char *
bd_asset_resolve(const bd_backend *be, const char *rel, const char *fallback,
    char *buf, size_t bufsz)
{
	if (be && be->resolve_asset && rel && buf && bufsz) {
		const char *r = be->resolve_asset(rel, buf, bufsz);
		if (r)
			return r;
	}
	return fallback;
}

bd_texture
bd_asset_texture(const bd_backend *be, const char *id, const char *rel,
    const char *default_path)
{
	bd_asset a;
	char buf[4096];
	if (bd_asset_lookup(id, &a)) {
		if (a.data)
			return be->load_texture_mem
			    ? be->load_texture_mem(a.data, (int)a.len)
			    : (bd_texture){0};
		if (a.path)
			return be->load_texture(a.path);
	}
	return be->load_texture(
	    bd_asset_resolve(be, rel, default_path, buf, sizeof buf));
}
