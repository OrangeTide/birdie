#ifndef BD_COLOR_H
#define BD_COLOR_H

#include <stdint.h>

/*
 * Color-name parsing shared by widgets that take human-authored color lists
 * (the indicator's states, the meter's zones, ...). Colors are packed RGBA8
 * (0xRRGGBBAA), matching the rest of the toolkit.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* Parse one already-trimmed, NUL-terminated token into a 0xRRGGBBAA color.
 * Accepts #rgb, #rrggbb, #rrggbbaa, and a small spectral-order named table
 * (violet, blue, green, yellow, amber, orange, red, white). Returns 1 on
 * success, 0 if the token is not a recognized color (out is left untouched). */
int bd_color_parse(const char *tok, uint32_t *out);

/* Split `s` on spaces, commas, tabs, and newlines, parsing each token into
 * out[0..max). Unparsed tokens are skipped. Returns the count written (0 for a
 * NULL/empty/all-unparsed string; the caller supplies any default). */
int bd_color_parse_list(const char *s, uint32_t *out, int max);

#endif
