#ifndef BD_DRAW_H
#define BD_DRAW_H

#include "bd_backend.h"
#include <stdint.h>

/*
 * Toolkit renderer. Builds chrome and text on top of the backend GPU
 * interface (bd_backend.h): one default shader (textured * vertex color), one
 * dynamic quad batch, one stb_truetype glyph atlas. Colors are packed RGBA8
 * (0xRRGGBBAA), matching the widget layer. Coordinates are pixels, origin
 * top-left.
 *
 * Extension widgets that want custom effects (e.g. shaded knobs) bypass these
 * helpers and talk to the backend GPU interface directly via bd_backend_get().
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* Create the renderer on a backend. font_path is a TTF/OTF baked at font_px
 * for chrome text; pass NULL to run without text (text calls become no-ops).
 * Returns 1 on success, 0 if the shader or mesh could not be created. */
int  bd_draw_init(const bd_backend *be, const char *font_path, float font_px);
void bd_draw_shutdown(void);

/* Frame: begin a batch for a window of win_w x win_h pixels, end flushes it.
 * Quads emitted between them are batched and drawn in order. */
void bd_draw_begin(int win_w, int win_h);
void bd_draw_end(void);

/* Flush the pending batch now. An extension widget calls this before issuing
 * its own custom-shader draws (via bd_backend_get()) so the chrome drawn so
 * far lands beneath them; the toolkit resumes batching afterward. */
void bd_draw_flush(void);

/* The window size passed to the current bd_draw_begin (for widgets setting
 * their own u_res-style uniform). */
int bd_draw_win_w(void);
int bd_draw_win_h(void);

/* Solid fill / one-pixel outline. */
void bd_draw_rect(float x, float y, float w, float h, uint32_t rgba);
void bd_draw_rect_lines(float x, float y, float w, float h, uint32_t rgba);

/* Filled convex quad from four corners (in order), e.g. a folder-tab
 * trapezoid. Solid color. */
void bd_draw_quad(float x0, float y0, float x1, float y1,
                  float x2, float y2, float x3, float y3, uint32_t rgba);

/* Tinted textured quad. Source rect is normalized texture coords [0,1]. */
void bd_draw_sprite(bd_texture tex, float dx, float dy, float dw, float dh,
                    float u0, float v0, float u1, float v1, uint32_t rgba);

/* Chrome text at the baseline-independent top-left pen position (x,y is the
 * top of the line). No-op if the renderer was created without a font. */
void  bd_draw_text(const char *s, float x, float y, uint32_t rgba);
float bd_draw_text_width(const char *s);
float bd_draw_line_height(void);   /* baked font ascent+descent+gap, pixels */
float bd_draw_ascent(void);        /* baked font ascent, pixels */

/* Font style: a bitmask selecting one of eight baked faces — bold/italic in a
 * proportional or a fixed-width (BD_FONT_MONO) family. A variant whose TTF was
 * not found falls back to the regular proportional face. */
enum {
	BD_FONT_BOLD   = 1 << 0,
	BD_FONT_ITALIC = 1 << 1,
	BD_FONT_MONO   = 1 << 2,
};
void  bd_draw_text_styled(const char *s, float x, float y, uint32_t rgba, int style);
float bd_draw_text_width_styled(const char *s, int style);
float bd_draw_line_height_styled(int style); /* per-face line height, px */
float bd_draw_ascent_styled(int style);      /* per-face ascent, px */

#endif
