/*
 * bdf2font.c - generate birdie-gui's embedded bitmap fonts from BDF sources.
 *
 * Bakes two fixed-width faces into a single C header (bd_embed_font.h):
 *   EMBED8_*   an 8x8 face   (small UI / dense terminals)
 *   EMBED16_*  an 8x16 face  (default UI / terminals)
 * subset to the codepoint RANGES below (matching REF/subset.sh: Basic Latin,
 * Latin-1, punctuation, super/subscripts, arrows, box drawing, blocks,
 * geometric shapes). Every glyph in those ranges is single-width (8px), so both
 * faces are a fixed 8 columns; glyphs wider than 8 (CJK, outside the ranges) are
 * skipped.
 *
 * Rows are one byte each, top to bottom; bit b (LSB first) is column b -- the
 * convention bd_draw.c's baker reads. BDF stores rows MSB first, so each byte is
 * bit-reversed here.
 *
 * Sources are the unscii bitmap fonts (Viznut, public domain). This is a dev
 * tool, not part of the build; REF/ holds the local BDF sources. Regenerate:
 *
 *   cc -O2 -o /tmp/bdf2font src/birdie-gui/tools/bdf2font.c
 *   /tmp/bdf2font REF/unscii-8.bdf REF/unscii-16.bdf \
 *       > src/birdie-gui/bd_embed_font.h
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { unsigned lo, hi; } RANGES[] = {
	{ 0x0020, 0x007E },  /* Basic Latin */
	{ 0x00A0, 0x00FF },  /* Latin-1 Supplement */
	{ 0x2010, 0x2027 },  /* General Punctuation (dashes, quotes, ellipsis) */
	{ 0x2030, 0x2034 },  /* per-mille, primes */
	{ 0x2039, 0x203A },  /* single angle quotes */
	{ 0x2070, 0x209F },  /* super/subscripts */
	{ 0x2122, 0x2122 },  /* trademark */
	{ 0x2190, 0x2199 },  /* arrows */
	{ 0x2500, 0x257F },  /* box drawing */
	{ 0x2580, 0x259F },  /* block elements */
	{ 0x25A0, 0x25FF },  /* geometric shapes */
};

static int
in_range(unsigned cp)
{
	for (size_t i = 0; i < sizeof RANGES / sizeof RANGES[0]; i++)
		if (cp >= RANGES[i].lo && cp <= RANGES[i].hi)
			return 1;
	return 0;
}

static unsigned char
revbits(unsigned char b)
{
	b = (unsigned char)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
	b = (unsigned char)((b & 0xCC) >> 2 | (b & 0x33) << 2);
	b = (unsigned char)((b & 0xAA) >> 1 | (b & 0x55) << 1);
	return b;
}

struct glyph {
	unsigned      cp;
	unsigned char rows[16];
};

static int
cmp_glyph(const void *a, const void *b)
{
	unsigned x = ((const struct glyph *)a)->cp;
	unsigned y = ((const struct glyph *)b)->cp;
	return x < y ? -1 : x > y ? 1 : 0;
}

/* Parse a BDF, keeping in-range single-width glyphs. Returns the count and
 * fills `out` (caller-sized big enough). `cellh` is the expected row count. */
static int
parse_bdf(const char *path, int cellh, struct glyph *out, int max)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "bdf2font: cannot open %s\n", path);
		exit(1);
	}
	char line[256];
	int n = 0, skipped = 0;
	unsigned cp = 0;
	int bbw = 0, bbh = 0;
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, "ENCODING ", 9)) {
			cp = (unsigned)strtol(line + 9, NULL, 10);
		} else if (!strncmp(line, "BBX ", 4)) {
			int xo, yo;
			sscanf(line + 4, "%d %d %d %d", &bbw, &bbh, &xo, &yo);
		} else if (!strncmp(line, "BITMAP", 6)) {
			/* read bbh rows of hex right after BITMAP */
			int keep = in_range(cp) && bbw <= 8 && bbh == cellh;
			struct glyph g = { cp, { 0 } };
			for (int r = 0; r < bbh; r++) {
				if (!fgets(line, sizeof line, f))
					break;
				unsigned v = (unsigned)strtol(line, NULL, 16);
				/* one byte (bbw<=8); BDF is MSB-first, flip it */
				if (r < 16)
					g.rows[r] = revbits((unsigned char)v);
			}
			if (keep) {
				if (n >= max) {
					fprintf(stderr, "bdf2font: too many glyphs\n");
					exit(1);
				}
				out[n++] = g;
			} else if (in_range(cp) && bbw > 8) {
				skipped++;
			}
		}
	}
	fclose(f);
	if (skipped)
		fprintf(stderr, "bdf2font: %s skipped %d wide (>8px) in-range glyphs\n",
		    path, skipped);
	qsort(out, (size_t)n, sizeof out[0], cmp_glyph);
	return n;
}

static void
emit_face(const char *name, int cellh, const struct glyph *g, int n)
{
	printf("#define %s_NGLYPHS %d\n\n", name, n);
	printf("static const unsigned char %s_GLYPHS[%s_NGLYPHS][%d] = {\n",
	    name, name, cellh);
	for (int i = 0; i < n; i++) {
		printf("\t{");
		for (int r = 0; r < cellh; r++)
			printf("0x%02X%s", g[i].rows[r], r + 1 < cellh ? "," : "");
		printf("}, /* U+%04X */\n", g[i].cp);
	}
	printf("};\n\n");
	printf("static const unsigned %s_CMAP[%s_NGLYPHS] = {\n\t", name, name);
	for (int i = 0; i < n; i++)
		printf("0x%04X,%s", g[i].cp, (i % 8 == 7) ? "\n\t" : " ");
	printf("\n};\n\n");
}

int
main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <8px.bdf> <16px.bdf>\n", argv[0]);
		return 1;
	}
	static struct glyph g8[4096], g16[4096];
	int n8 = parse_bdf(argv[1], 8, g8, 4096);
	int n16 = parse_bdf(argv[2], 16, g16, 4096);

	printf("/*\n");
	printf(" * bd_embed_font.h - embedded bitmap fonts (8x8 and 8x16).\n");
	printf(" *\n");
	printf(" * Two fixed-width faces baked from the unscii bitmap fonts (Viznut,\n");
	printf(" * public domain), subset to the UI/terminal codepoint ranges. One byte\n");
	printf(" * per row, top to bottom; bit b (LSB first) is column b. CMAP[i] is the\n");
	printf(" * ascending Unicode codepoint of glyph i, for codepoint->slot bisection.\n");
	printf(" *\n");
	printf(" * Generated by src/birdie-gui/tools/bdf2font.c (see it to regenerate).\n");
	printf(" * Made by a machine. PUBLIC DOMAIN (CC0-1.0)\n");
	printf(" */\n");
	printf("#ifndef BD_EMBED_FONT_H\n#define BD_EMBED_FONT_H\n\n");
	emit_face("EMBED8", 8, g8, n8);
	emit_face("EMBED16", 16, g16, n16);
	printf("#endif /* BD_EMBED_FONT_H */\n");

	fprintf(stderr, "bdf2font: 8x8 %d glyphs, 8x16 %d glyphs\n", n8, n16);
	return 0;
}
