/*
 * birdie — desktop MUD client. Bootstrap: CP437 glyph atlas draws a
 * static splash string. Forked from ludica/samples/ansiview.
 *
 * Escape: quit
 */

#include "ludica.h"
#include "ludica_gfx.h"
#include <stdio.h>

#define FONT_COLS 32

static lud_texture_t font_tex;
static int font_w = 8, font_h = 16;

static const float palette[16][3] = {
	{ 0.000f, 0.000f, 0.000f }, { 0.000f, 0.000f, 0.667f },
	{ 0.000f, 0.667f, 0.000f }, { 0.000f, 0.667f, 0.667f },
	{ 0.667f, 0.000f, 0.000f }, { 0.667f, 0.000f, 0.667f },
	{ 0.667f, 0.333f, 0.000f }, { 0.667f, 0.667f, 0.667f },
	{ 0.333f, 0.333f, 0.333f }, { 0.333f, 0.333f, 1.000f },
	{ 0.333f, 1.000f, 0.333f }, { 0.333f, 1.000f, 1.000f },
	{ 1.000f, 0.333f, 0.333f }, { 1.000f, 0.333f, 1.000f },
	{ 1.000f, 1.000f, 0.333f }, { 1.000f, 1.000f, 1.000f },
};

static void
draw_glyph(unsigned char ch, int col, int row, int fg, int bg)
{
	float dx = (float)(col * font_w);
	float dy = (float)(row * font_h);
	int gcol = ch % FONT_COLS;
	int grow = ch / FONT_COLS;
	float sx = (float)(gcol * font_w);
	float sy = (float)(grow * font_h);

	lud_sprite_rect(dx, dy, font_w, font_h,
	                palette[bg][0], palette[bg][1], palette[bg][2], 1.0f);
	lud_sprite_draw_tinted(font_tex, dx, dy, font_w, font_h,
	                       sx, sy, font_w, font_h,
	                       palette[fg][0], palette[fg][1], palette[fg][2], 1.0f);
}

static void
draw_text(const char *s, int col, int row, int fg, int bg)
{
	for (; *s; s++, col++)
		draw_glyph((unsigned char)*s, col, row, fg, bg);
}

static int
on_event(const lud_event_t *ev)
{
	if (ev->type == LUD_EV_KEY_DOWN && ev->key.keycode == LUD_KEY_ESCAPE) {
		lud_quit();
		return 1;
	}
	return 0;
}

static void
init(void)
{
	font_tex = lud_load_texture("src/birdie/assets/font8x16.png",
	                            LUD_FILTER_NEAREST, LUD_FILTER_NEAREST);
	if (font_tex.id == 0)
		fprintf(stderr, "birdie: failed to load font atlas\n");
}

static void
frame(float dt)
{
	int w = lud_width(), h = lud_height();
	int cols = w / font_w, rows = h / font_h;
	int r, c;

	(void)dt;

	lud_viewport(0, 0, w, h);
	lud_clear(0.0f, 0.0f, 0.0f, 1.0f);

	lud_sprite_begin(0, 0, w, h);

	/* terminal fills the background — blank cells. */
	for (r = 0; r < rows; r++)
		for (c = 0; c < cols; c++)
			draw_glyph(' ', c, r, 7, 0);

	draw_text("birdie v0.0 - bootstrap", 2, 1, 15, 0);
	draw_text("Escape to quit.",         2, 3,  7, 0);

	lud_sprite_end();
}

static void
cleanup(void)
{
	lud_destroy_texture(font_tex);
}

int
main(int argc, char **argv)
{
	return lud_run(&(lud_desc_t){
		.app_name  = "birdie",
		.width     = 800,
		.height    = 500,
		.resizable = 1,
		.argc      = argc,
		.argv      = argv,
		.init      = init,
		.frame     = frame,
		.cleanup   = cleanup,
		.event     = on_event,
	});
}
