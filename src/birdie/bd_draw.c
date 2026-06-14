#include "bd_draw.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

/*
 * Toolkit renderer on the backend GPU interface. One shader (textured *
 * vertex color), one dynamic quad batch, one stb_truetype glyph atlas. Solid
 * fills sample a 1x1 white texture; glyphs sample the atlas (white, coverage
 * in alpha), so fills, sprites, and text share one shader and one mesh. The
 * batch flushes whenever the bound texture changes or it fills up.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#define MAX_VERTS   8192        /* multiple of 6 (quads) */
#define ATLAS_DIM   512
#define FONT_FIRST  0x20
#define FONT_COUNT  0xE0        /* 0x20..0xFF: Basic Latin + Latin-1 */

static const char *VERT_SRC =
	"#version 300 es\n"
	"layout(location=0) in vec2 a_pos;\n"
	"layout(location=1) in vec2 a_uv;\n"
	"layout(location=2) in vec4 a_col;\n"
	"uniform vec2 u_res;\n"
	"out vec2 v_uv;\n"
	"out vec4 v_col;\n"
	"void main(){\n"
	"    vec2 p = a_pos / u_res * 2.0 - 1.0;\n"
	"    gl_Position = vec4(p.x, -p.y, 0.0, 1.0);\n"
	"    v_uv = a_uv;\n"
	"    v_col = a_col;\n"
	"}\n";

static const char *FRAG_SRC =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec2 v_uv;\n"
	"in vec4 v_col;\n"
	"uniform sampler2D u_tex;\n"
	"out vec4 frag;\n"
	"void main(){ frag = texture(u_tex, v_uv) * v_col; }\n";

static const bd_backend *be;
static bd_shader  shader;
static bd_texture white;

static bd_vertex  verts[MAX_VERTS];
static int        nverts;
static bd_texture cur_tex;
static int        batch_active;
static float      win_w, win_h;

/* font */
static int              have_font;
static bd_texture       atlas;
static stbtt_packedchar packed[FONT_COUNT];
static float            font_ascent;
static float            font_line_h;

static inline void
unpack(uint32_t c, float *r, float *g, float *b, float *a)
{
	*r = ((c >> 24) & 0xFF) / 255.0f;
	*g = ((c >> 16) & 0xFF) / 255.0f;
	*b = ((c >>  8) & 0xFF) / 255.0f;
	*a = ( c        & 0xFF) / 255.0f;
}

static void
flush(void)
{
	if (nverts == 0)
		return;
	be->use_shader(shader);
	be->set_uniform_vec2(shader, "u_res", win_w, win_h);
	be->set_uniform_int(shader, "u_tex", 0);
	be->bind_texture(cur_tex, 0);
	be->draw_verts(verts, nverts);
	nverts = 0;
}

static void
quad(bd_texture tex, float x, float y, float w, float h,
    float u0, float v0, float u1, float v1, uint32_t color)
{
	if (!batch_active)
		return;
	if (tex.id != cur_tex.id) {
		flush();
		cur_tex = tex;
	}
	if (nverts + 6 > MAX_VERTS)
		flush();

	float r, g, b, a;
	unpack(color, &r, &g, &b, &a);
	bd_vertex tl = { x,     y,     u0, v0, r, g, b, a };
	bd_vertex tr = { x + w, y,     u1, v0, r, g, b, a };
	bd_vertex br = { x + w, y + h, u1, v1, r, g, b, a };
	bd_vertex bl = { x,     y + h, u0, v1, r, g, b, a };
	verts[nverts++] = tl; verts[nverts++] = tr; verts[nverts++] = br;
	verts[nverts++] = tl; verts[nverts++] = br; verts[nverts++] = bl;
}

/* ------------------------------------------------------------------ */
/* font baking                                                        */
/* ------------------------------------------------------------------ */

static unsigned char *
read_file(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *buf = malloc(n > 0 ? (size_t)n : 1);
	if (buf && n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	if (buf)
		*len = n;
	return buf;
}

static void
bake_font(const char *path, float px)
{
	long len;
	unsigned char *ttf = read_file(path, &len);
	if (!ttf) {
		fprintf(stderr, "bd_draw: cannot read font '%s'\n", path);
		return;
	}

	stbtt_fontinfo info;
	if (stbtt_InitFont(&info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0))) {
		int asc, desc, gap;
		stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
		float s = stbtt_ScaleForPixelHeight(&info, px);
		font_ascent = asc * s;
		font_line_h = (asc - desc + gap) * s;
	}

	unsigned char *cov = malloc(ATLAS_DIM * ATLAS_DIM);
	unsigned char *rgba = malloc(ATLAS_DIM * ATLAS_DIM * 4);
	if (cov && rgba) {
		stbtt_pack_context pc;
		stbtt_PackBegin(&pc, cov, ATLAS_DIM, ATLAS_DIM, 0, 1, NULL);
		stbtt_PackSetOversampling(&pc, 1, 1);
		if (!stbtt_PackFontRange(&pc, ttf, 0, px, FONT_FIRST,
		    FONT_COUNT, packed))
			fprintf(stderr, "bd_draw: glyph atlas overflow\n");
		stbtt_PackEnd(&pc);

		for (int i = 0; i < ATLAS_DIM * ATLAS_DIM; i++) {
			rgba[i*4+0] = 255; rgba[i*4+1] = 255;
			rgba[i*4+2] = 255; rgba[i*4+3] = cov[i];
		}
		atlas = be->make_texture(ATLAS_DIM, ATLAS_DIM, rgba);
		have_font = (atlas.id != 0);
	}
	free(cov);
	free(rgba);
	free(ttf);
}

/* ------------------------------------------------------------------ */
/* public                                                             */
/* ------------------------------------------------------------------ */

int
bd_draw_init(const bd_backend *backend, const char *font_path, float font_px)
{
	be = backend;

	shader = be->make_shader(VERT_SRC, FRAG_SRC);
	if (shader.id == 0)
		return 0;

	uint32_t wpix = 0xFFFFFFFFu;
	white = be->make_texture(1, 1, &wpix);
	cur_tex = white;

	if (font_path)
		bake_font(font_path, font_px);
	return 1;
}

void
bd_draw_shutdown(void)
{
	if (!be)
		return;
	if (have_font)
		be->destroy_texture(atlas);
	be->destroy_texture(white);
	be->destroy_shader(shader);
	have_font = 0;
	be = NULL;
}

void
bd_draw_begin(int w, int h)
{
	win_w = (float)w;
	win_h = (float)h;
	nverts = 0;
	cur_tex = white;
	batch_active = 1;
}

void
bd_draw_end(void)
{
	flush();
	batch_active = 0;
}

void
bd_draw_rect(float x, float y, float w, float h, uint32_t rgba)
{
	quad(white, x, y, w, h, 0, 0, 1, 1, rgba);
}

void
bd_draw_rect_lines(float x, float y, float w, float h, uint32_t rgba)
{
	quad(white, x,         y,         w, 1, 0, 0, 1, 1, rgba);
	quad(white, x,         y + h - 1, w, 1, 0, 0, 1, 1, rgba);
	quad(white, x,         y,         1, h, 0, 0, 1, 1, rgba);
	quad(white, x + w - 1, y,         1, h, 0, 0, 1, 1, rgba);
}

void
bd_draw_sprite(bd_texture tex, float dx, float dy, float dw, float dh,
    float u0, float v0, float u1, float v1, uint32_t rgba)
{
	quad(tex, dx, dy, dw, dh, u0, v0, u1, v1, rgba);
}

void
bd_draw_text(const char *s, float x, float y, uint32_t rgba)
{
	if (!have_font || !s)
		return;
	float px = x, py = y + font_ascent;
	for (; *s; s++) {
		int cp = (unsigned char)*s;
		if (cp < FONT_FIRST || cp >= FONT_FIRST + FONT_COUNT)
			cp = '?';
		stbtt_aligned_quad q;
		stbtt_GetPackedQuad(packed, ATLAS_DIM, ATLAS_DIM,
		    cp - FONT_FIRST, &px, &py, &q, 1);
		quad(atlas, q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0,
		    q.s0, q.t0, q.s1, q.t1, rgba);
	}
}

float
bd_draw_text_width(const char *s)
{
	if (!have_font || !s)
		return 0.0f;
	float px = 0, py = 0;
	for (; *s; s++) {
		int cp = (unsigned char)*s;
		if (cp < FONT_FIRST || cp >= FONT_FIRST + FONT_COUNT)
			cp = '?';
		stbtt_aligned_quad q;
		stbtt_GetPackedQuad(packed, ATLAS_DIM, ATLAS_DIM,
		    cp - FONT_FIRST, &px, &py, &q, 1);
	}
	return px;
}

float bd_draw_line_height(void) { return font_line_h; }
float bd_draw_ascent(void)      { return font_ascent; }
