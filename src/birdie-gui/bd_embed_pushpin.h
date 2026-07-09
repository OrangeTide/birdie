/*
 * bd_embed_pushpin.h - embedded pushpin glyphs (from XBM sources).
 *
 * The window/menu pushpin is drawn like a text glyph: a 1-bit mask tinted by
 * the theme, sized to the chrome font. Two size tiers pair with the two
 * embedded font faces -- the "10" tier (OPEN LOOK point size; the glyphs are
 * ~12-13px tall) with the 8x8 face, the "14" tier (~14-15px) with 8x16 -- each
 * with an "out" (unpinned) and "in" (pinned) state.
 *
 * The bitmaps are the .xbm files beside this header (X BitMap: bits packed
 * LSB-first, each row padded to a byte). INK IS THE CLEARED BIT: a 0 bit is the
 * pin, a 1 bit is background. That polarity was verified against the original
 * pushpin PNGs' alpha; bd_draw.c inverts accordingly when baking.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */
#ifndef BD_EMBED_PUSHPIN_H
#define BD_EMBED_PUSHPIN_H

#include "pushpin_out_10.xbm"
#include "pushpin_in_10.xbm"
#include "pushpin_out_14.xbm"
#include "pushpin_in_14.xbm"

/* pushpin state index */
enum { BD_PIN_OUT = 0, BD_PIN_IN = 1 };

struct bd_pushpin_bitmap {
	int                  w, h;   /* pixel size */
	const unsigned char *bits;   /* XBM bytes, LSB-first, stride (w+7)/8; ink = 0 */
};

/* [tier][state]: tier 0 = "10" (pairs with the 8x8 font), tier 1 = "14" (8x16) */
static const struct bd_pushpin_bitmap bd_pushpins[2][2] = {
	{ { pushpin_out_10_width, pushpin_out_10_height,
	    (const unsigned char *)pushpin_out_10_bits },
	  { pushpin_in_10_width,  pushpin_in_10_height,
	    (const unsigned char *)pushpin_in_10_bits } },
	{ { pushpin_out_14_width, pushpin_out_14_height,
	    (const unsigned char *)pushpin_out_14_bits },
	  { pushpin_in_14_width,  pushpin_in_14_height,
	    (const unsigned char *)pushpin_in_14_bits } },
};

#endif /* BD_EMBED_PUSHPIN_H */
