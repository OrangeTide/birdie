#ifndef BD_WIDGET_VT_H
#define BD_WIDGET_VT_H

#include "widget.h"
#include <stdint.h>

/*
 * VT terminal -- an extension widget built on the toolkit's widget-class API
 * (widget_ext.h). It is the reference example of "define a new kind of widget"
 * and the one widget that depends on libvt; core widget.c knows nothing about
 * it.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * Terminal palette. ansi[] holds the 16 system colors (palette indices
 * 0-15); the 6x6x6 color cube (16-231) and grayscale ramp (232-255) are
 * derived per xterm and not stored. Defaults are compile-time overridable
 * with -DBD_TERM_xxx=0xRRGGBBAAu and can be set per-terminal at runtime.
 */
#ifndef BD_TERM_ANSI0
#define BD_TERM_ANSI0  0x000000FFu
#endif
#ifndef BD_TERM_ANSI1
#define BD_TERM_ANSI1  0xAA0000FFu
#endif
#ifndef BD_TERM_ANSI2
#define BD_TERM_ANSI2  0x00AA00FFu
#endif
#ifndef BD_TERM_ANSI3
#define BD_TERM_ANSI3  0xAA5500FFu
#endif
#ifndef BD_TERM_ANSI4
#define BD_TERM_ANSI4  0x0000AAFFu
#endif
#ifndef BD_TERM_ANSI5
#define BD_TERM_ANSI5  0xAA00AAFFu
#endif
#ifndef BD_TERM_ANSI6
#define BD_TERM_ANSI6  0x00AAAAFFu
#endif
#ifndef BD_TERM_ANSI7
#define BD_TERM_ANSI7  0xAAAAAAFFu
#endif
#ifndef BD_TERM_ANSI8
#define BD_TERM_ANSI8  0x555555FFu
#endif
#ifndef BD_TERM_ANSI9
#define BD_TERM_ANSI9  0xFF5555FFu
#endif
#ifndef BD_TERM_ANSI10
#define BD_TERM_ANSI10 0x55FF55FFu
#endif
#ifndef BD_TERM_ANSI11
#define BD_TERM_ANSI11 0xFFFF55FFu
#endif
#ifndef BD_TERM_ANSI12
#define BD_TERM_ANSI12 0x5555FFFFu
#endif
#ifndef BD_TERM_ANSI13
#define BD_TERM_ANSI13 0xFF55FFFFu
#endif
#ifndef BD_TERM_ANSI14
#define BD_TERM_ANSI14 0x55FFFFFFu
#endif
#ifndef BD_TERM_ANSI15
#define BD_TERM_ANSI15 0xFFFFFFFFu
#endif
#ifndef BD_TERM_FG
#define BD_TERM_FG      0xAAAAAAFFu
#endif
#ifndef BD_TERM_BG
#define BD_TERM_BG      0x000000FFu
#endif
#ifndef BD_TERM_BOLD_FG
#define BD_TERM_BOLD_FG 0xFFFFFFFFu
#endif

typedef struct bd_palette {
	uint32_t ansi[16];      /* system colors, palette indices 0-15 */
	uint32_t default_fg;    /* color for the terminal's default foreground */
	uint32_t default_bg;    /* color for the terminal's default background */
	uint32_t bold_fg;       /* brightening applied to bold default-fg text */
} bd_palette;

#define BD_PALETTE_DEFAULTS {                                          \
	.ansi = {                                                       \
		BD_TERM_ANSI0,  BD_TERM_ANSI1,  BD_TERM_ANSI2,  BD_TERM_ANSI3, \
		BD_TERM_ANSI4,  BD_TERM_ANSI5,  BD_TERM_ANSI6,  BD_TERM_ANSI7, \
		BD_TERM_ANSI8,  BD_TERM_ANSI9,  BD_TERM_ANSI10, BD_TERM_ANSI11, \
		BD_TERM_ANSI12, BD_TERM_ANSI13, BD_TERM_ANSI14, BD_TERM_ANSI15, \
	},                                                              \
	.default_fg = BD_TERM_FG, .default_bg = BD_TERM_BG,             \
	.bold_fg = BD_TERM_BOLD_FG,                                     \
}

static inline bd_palette
bd_palette_default(void)
{
	bd_palette p = BD_PALETTE_DEFAULTS;
	return p;
}

/* Register the terminal widget class. Safe to call more than once; the first
 * call registers, later calls are no-ops. bd_terminal_create() also registers
 * lazily, so calling this explicitly is optional. */
void bd_terminal_register(void);

/* Create a terminal widget. Accepts the same BD_* attribute varargs as
 * bd_create(), terminated by BD_END. */
bd_id bd_terminal_create(bd_id parent, ...);

/* Feed bytes (escape sequences and text) to a terminal widget. len < 0 means
 * data is NUL-terminated. No-op if id is not a terminal. */
void bd_terminal_write(bd_id id, const char *data, int len);

/* Replace a terminal's color palette (copied). No-op if id is not a terminal.
 * Start from bd_palette_default() and adjust the entries you care about. */
void bd_terminal_set_palette(bd_id id, const bd_palette *pal);

#endif
