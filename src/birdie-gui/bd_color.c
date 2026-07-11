/*
 * bd_color.c -- parse human-authored color names / #hex into packed RGBA8.
 *
 * Adopted from the copy that lived in bd_widget_indicator.c and was duplicated
 * verbatim in bd_widget_meter.c; both now call these. See bd_color.h.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_color.h"
#include <string.h>

/* A small spectral-order table: enough to author "green amber red" zones as a
 * convenience; any color is reachable as #hex. */
static const struct { const char *name; uint32_t rgba; } NAMED[] = {
	{ "violet", 0x8B00FFFFu }, /* 400-450 nm */
	{ "blue",   0x1A66FFFFu }, /* 450-495 nm */
	{ "green",  0x22DD22FFu }, /* 495-570 nm */
	{ "yellow", 0xFFD400FFu },
	{ "amber",  0xFFB000FFu }, /* 570-590 nm */
	{ "orange", 0xFF7A00FFu }, /* 590-620 nm */
	{ "red",    0xFF1E10FFu }, /* 620-750 nm */
	{ "white",  0xFFFFFFFFu },
};

static int
str_ieq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		int ca = *a, cb = *b;
		if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
		if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
		if (ca != cb)
			return 0;
	}
	return *a == *b;
}

static int
hex_nibble(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

int
bd_color_parse(const char *tok, uint32_t *out)
{
	if (tok[0] == '#') {
		const char *h = tok + 1;
		int n = (int)strlen(h);
		int d[8], i;
		for (i = 0; i < n && i < 8; i++) {
			d[i] = hex_nibble(h[i]);
			if (d[i] < 0)
				return 0;
		}
		unsigned r, g, b, a = 0xFF;
		if (n == 3) {                 /* #rgb */
			r = d[0] * 0x11; g = d[1] * 0x11; b = d[2] * 0x11;
		} else if (n == 6 || n == 8) {/* #rrggbb[aa] */
			r = d[0] << 4 | d[1];
			g = d[2] << 4 | d[3];
			b = d[4] << 4 | d[5];
			if (n == 8)
				a = d[6] << 4 | d[7];
		} else {
			return 0;
		}
		*out = r << 24 | g << 16 | b << 8 | a;
		return 1;
	}
	for (size_t i = 0; i < sizeof NAMED / sizeof NAMED[0]; i++)
		if (str_ieq(tok, NAMED[i].name)) {
			*out = NAMED[i].rgba;
			return 1;
		}
	return 0;
}

int
bd_color_parse_list(const char *s, uint32_t *out, int max)
{
	int n = 0;
	if (!s || max <= 0)
		return 0;
	char tok[32];
	int tl = 0;
	for (;;) {
		char c = *s;
		int sep = (c == '\0' || c == ' ' || c == ',' ||
		    c == '\t' || c == '\n');
		if (sep) {
			if (tl > 0 && n < max) {
				tok[tl] = '\0';
				if (bd_color_parse(tok, &out[n]))
					n++;
			}
			tl = 0;
			if (c == '\0')
				break;
		} else if (tl < (int)sizeof tok - 1) {
			tok[tl++] = c;
		}
		s++;
	}
	return n;
}
