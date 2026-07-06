#ifndef BD_THEME_H
#define BD_THEME_H

#include <stdint.h>

/*
 * GUI chrome theme. Colors are RGBA8 (0xRRGGBBAA). Defaults are compile-time
 * overridable: pass -DBD_TH_xxx=0x... at build time. At runtime, hand a
 * bd_theme to bd_gui_init() (NULL selects the defaults).
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#ifndef BD_TH_BG
#define BD_TH_BG        0x2B2B2BFFu
#endif
#ifndef BD_TH_PANEL
#define BD_TH_PANEL     0x313335FFu
#endif
#ifndef BD_TH_WIDGET
#define BD_TH_WIDGET    0x3C3F41FFu
#endif
#ifndef BD_TH_HOVER
#define BD_TH_HOVER     0x4C5052FFu
#endif
#ifndef BD_TH_PRESS
#define BD_TH_PRESS     0x27292AFFu
#endif
#ifndef BD_TH_TEXT
#define BD_TH_TEXT      0xBBBBBBFFu
#endif
#ifndef BD_TH_TEXT_HI
#define BD_TH_TEXT_HI   0xFFFFFFFFu
#endif
#ifndef BD_TH_BORDER
#define BD_TH_BORDER    0x555555FFu
#endif
#ifndef BD_TH_FOCUS
#define BD_TH_FOCUS     0x5599DDFFu
#endif
#ifndef BD_TH_SELECT
#define BD_TH_SELECT    0x264F78FFu
#endif
#ifndef BD_TH_FONT_SIZE
#define BD_TH_FONT_SIZE 14.0f
#endif
#ifndef BD_TH_BASELINE
#define BD_TH_BASELINE  0.75f
#endif

typedef struct bd_theme {
	uint32_t bg;        /* window background */
	uint32_t panel;     /* panel / popup background */
	uint32_t widget;    /* button / control face */
	uint32_t hover;     /* hovered control */
	uint32_t press;     /* pressed control / input field */
	uint32_t text;      /* normal text */
	uint32_t text_hi;   /* emphasized text */
	uint32_t border;    /* outlines */
	uint32_t focus;     /* focus ring */
	uint32_t select;    /* text selection */
	float    font_size; /* chrome text size, px */
	float    baseline;  /* text baseline as a fraction of widget height */
} bd_theme;

#define BD_THEME_DEFAULTS {                                          \
	.bg = BD_TH_BG, .panel = BD_TH_PANEL, .widget = BD_TH_WIDGET, \
	.hover = BD_TH_HOVER, .press = BD_TH_PRESS, .text = BD_TH_TEXT, \
	.text_hi = BD_TH_TEXT_HI, .border = BD_TH_BORDER,            \
	.focus = BD_TH_FOCUS, .select = BD_TH_SELECT,               \
	.font_size = BD_TH_FONT_SIZE, .baseline = BD_TH_BASELINE,    \
}

static inline bd_theme
bd_theme_default(void)
{
	bd_theme t = BD_THEME_DEFAULTS;
	return t;
}

#endif
