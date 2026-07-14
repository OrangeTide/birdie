/*
 * bd_encoding.h -- decode legacy 8-bit MUD text to UTF-8.
 *
 * MUDs predate UTF-8 and many still send ISO-8859-1 or Windows-1252 bytes with
 * no CHARSET negotiation. The terminal is UTF-8, so those high bytes must be
 * transcoded first or they render as mojibake. This maps a per-profile
 * encoding selection to a stateless byte decoder. UTF-8 is a passthrough, so
 * the default path is unchanged.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#ifndef BD_ENCODING_H
#define BD_ENCODING_H

#include <stddef.h>

typedef enum bd_encoding {
    BD_ENC_UTF8 = 0,    /* passthrough (default) */
    BD_ENC_LATIN1,      /* ISO-8859-1 */
    BD_ENC_CP1252       /* Windows-1252 */
} bd_encoding;

/* Map a charset name to an encoding (case-insensitive, common aliases:
 * "utf-8"/"utf8"; "iso-8859-1"/"latin1"/"latin-1"/"8859-1";
 * "windows-1252"/"cp1252"/"1252"). NULL/empty/unknown -> BD_ENC_UTF8. */
bd_encoding bd_encoding_parse(const char *name);

/* Canonical display name for an encoding (e.g. "UTF-8", "ISO-8859-1"). */
const char *bd_encoding_name(bd_encoding e);

/* Worst-case UTF-8 output bytes for `n` input bytes: each legacy byte expands
 * to at most 3 UTF-8 bytes. */
#define BD_ENCODING_MAX_OUT(n) ((n) * 3)

/* Decode `len` bytes of `src` in encoding `e` as UTF-8 into `dst` (capacity
 * `dstcap`, ideally at least BD_ENCODING_MAX_OUT(len)). Returns the number of
 * bytes written; output is truncated to `dstcap` rather than overrunning. For
 * BD_ENC_UTF8 this copies the bytes verbatim. Stateless: each input byte
 * decodes independently, so it is safe to call per received chunk (legacy
 * 8-bit sets have no multibyte sequences to split across chunks). */
size_t bd_encoding_decode(bd_encoding e, const unsigned char *src, size_t len,
                          unsigned char *dst, size_t dstcap);

#endif /* BD_ENCODING_H */
