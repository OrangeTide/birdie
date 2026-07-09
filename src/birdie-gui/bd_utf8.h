/*
 * bd_utf8.h — UTF-8 encoding and decoding for birdie-gui.
 *
 * A small, length-bounded UTF-8 codec. Decoding takes an explicit byte count
 * so it can never read past the end of a buffer on a truncated sequence, which
 * makes it the safe primitive for laying out arbitrary (possibly malformed)
 * text. The toolkit renderer and the app both build on it.
 *
 * The bd_utf8_ prefix keeps these symbols distinct from libvt's identical
 * utf8_* routines: the app links both, and the toolkit core stays independent
 * of libvt (the terminal widget, hence libvt, is optional).
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#ifndef BD_UTF8_H
#define BD_UTF8_H

#include <stddef.h>
#include <stdint.h>

/* Maximum bytes per UTF-8 encoded codepoint (RFC 3629: 4 bytes). */
#define BD_UTF8_MAX 4

/* Replacement character returned on decode errors. */
#define BD_UTF8_RUNE_ERROR 0xFFFDu

/* Maximum valid Unicode codepoint. */
#define BD_UTF8_RUNE_MAX 0x10FFFFu

/* Decode one codepoint from a UTF-8 byte sequence. Reads up to len bytes from
 * s, stores the codepoint in *rune, and returns the number of bytes consumed
 * (1-4). On invalid or incomplete input, stores BD_UTF8_RUNE_ERROR in *rune and
 * returns 1 (to advance past the bad byte); on len == 0 it returns 0. Rejects
 * overlong forms, surrogates, and out-of-range values. */
int bd_utf8_decode(uint32_t *rune, const unsigned char *s, size_t len);

/* Encode one codepoint as UTF-8. Writes up to BD_UTF8_MAX bytes into buf and
 * returns the number of bytes written (1-4), or 0 if the codepoint is invalid
 * (a surrogate or above BD_UTF8_RUNE_MAX). */
int bd_utf8_encode(unsigned char *buf, uint32_t rune);

/* Count the bytes needed to encode a codepoint as UTF-8: 1-4 for valid
 * codepoints, 0 for invalid ones. */
int bd_utf8_runelen(uint32_t rune);

/* Truncate a UTF-8 string to fit in maxbytes (including the NUL). Returns the
 * largest byte count <= maxbytes-1 that does not split a multibyte sequence;
 * the caller NUL-terminates the result. */
size_t bd_utf8_trunc(const char *s, size_t maxbytes);

#endif /* BD_UTF8_H */
