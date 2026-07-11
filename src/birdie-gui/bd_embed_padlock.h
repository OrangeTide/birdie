/*
 * bd_embed_padlock.h - embedded padlock glyphs (from XBM sources).
 *
 * The window-manager title bar's lock button is drawn like a text glyph: a
 * 1-bit mask tinted by the theme, sized to the button. Two states -- "closed"
 * (locked / pinned) and "open" (unlocked) -- replace the old procedural
 * fill_rect padlock.
 *
 * The bitmaps are the .xbm files beside this header (X BitMap: bits packed
 * LSB-first, each row padded to a byte). INK IS THE CLEARED BIT, matching the
 * pushpin convention: a 0 bit is the padlock, a 1 bit is background; bd_draw.c
 * sets a pixel where the bit is cleared.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */
#ifndef BD_EMBED_PADLOCK_H
#define BD_EMBED_PADLOCK_H

#include "padlock_closed_14.xbm"
#include "padlock_open_14.xbm"

/* padlock state index */
enum { BD_LOCK_OPEN = 0, BD_LOCK_CLOSED = 1 };

struct bd_padlock_bitmap {
	int                  w, h;   /* pixel size */
	const unsigned char *bits;   /* XBM bytes, LSB-first, stride (w+7)/8; ink = 0 */
};

/* [state]: 0 = open (unlocked), 1 = closed (locked) */
static const struct bd_padlock_bitmap bd_padlocks[2] = {
	{ padlock_open_14_width,   padlock_open_14_height,
	  (const unsigned char *)padlock_open_14_bits },
	{ padlock_closed_14_width, padlock_closed_14_height,
	  (const unsigned char *)padlock_closed_14_bits },
};

#endif /* BD_EMBED_PADLOCK_H */
