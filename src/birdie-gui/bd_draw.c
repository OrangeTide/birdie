#include "bd_draw.h"
#include "bd_asset.h"
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

/* fonts: eight baked faces indexed by
 * BD_FONT_BOLD|BD_FONT_ITALIC|BD_FONT_MONO. A missing variant TTF falls back
 * to the regular proportional face (index 0). */
#define FONT_FACES 8
struct face {
	int              have;
	bd_texture       atlas;
	stbtt_packedchar packed[FONT_COUNT];
	float            ascent;
	float            line_h;
};
static struct face faces[FONT_FACES];
static float        font_ascent;   /* regular proportional face (back-compat) */
static float        font_line_h;

/* default face paths; override with -DBD_ASSET_GUI_FONT_* at build time, or
 * replace a face at runtime by id (BD_ASSET_FONT_*) via bd_asset_register_*. */
#ifndef BD_ASSET_GUI_FONT
#define BD_ASSET_GUI_FONT            "src/birdie-gui/assets/fonts/DejaVuSans.ttf"
#endif
#ifndef BD_ASSET_GUI_FONT_BOLD
#define BD_ASSET_GUI_FONT_BOLD       "src/birdie-gui/assets/fonts/DejaVuSans-Bold.ttf"
#endif
#ifndef BD_ASSET_GUI_FONT_ITALIC
#define BD_ASSET_GUI_FONT_ITALIC     "src/birdie-gui/assets/fonts/DejaVuSans-Oblique.ttf"
#endif
#ifndef BD_ASSET_GUI_FONT_BOLDITALIC
#define BD_ASSET_GUI_FONT_BOLDITALIC "src/birdie-gui/assets/fonts/DejaVuSans-BoldOblique.ttf"
#endif
#ifndef BD_ASSET_GUI_FONT_MONO
#define BD_ASSET_GUI_FONT_MONO           "src/birdie-gui/assets/fonts/DejaVuSansMono.ttf"
#endif
#ifndef BD_ASSET_GUI_FONT_MONO_BOLD
#define BD_ASSET_GUI_FONT_MONO_BOLD      "src/birdie-gui/assets/fonts/DejaVuSansMono-Bold.ttf"
#endif
#ifndef BD_ASSET_GUI_FONT_MONO_ITALIC
#define BD_ASSET_GUI_FONT_MONO_ITALIC    "src/birdie-gui/assets/fonts/DejaVuSansMono-Oblique.ttf"
#endif
#ifndef BD_ASSET_GUI_FONT_MONO_BOLDITALIC
#define BD_ASSET_GUI_FONT_MONO_BOLDITALIC "src/birdie-gui/assets/fonts/DejaVuSansMono-BoldOblique.ttf"
#endif

/* Per-face parallel arrays, indexed by BD_FONT_BOLD|ITALIC|MONO. Each face is
 * resolved as: an explicit bd_font_face from the caller, else a source
 * registered under its generic id, else its default file. */
static const char *const font_asset_id[FONT_FACES] = {
	BD_ASSET_FONT_REGULAR,   BD_ASSET_FONT_BOLD,
	BD_ASSET_FONT_ITALIC,    BD_ASSET_FONT_BOLD_ITALIC,
	BD_ASSET_FONT_MONO,      BD_ASSET_FONT_MONO_BOLD,
	BD_ASSET_FONT_MONO_ITALIC, BD_ASSET_FONT_MONO_BOLD_ITALIC,
};
static const char *const font_default_path[FONT_FACES] = {
	BD_ASSET_GUI_FONT,       BD_ASSET_GUI_FONT_BOLD,
	BD_ASSET_GUI_FONT_ITALIC, BD_ASSET_GUI_FONT_BOLDITALIC,
	BD_ASSET_GUI_FONT_MONO,  BD_ASSET_GUI_FONT_MONO_BOLD,
	BD_ASSET_GUI_FONT_MONO_ITALIC, BD_ASSET_GUI_FONT_MONO_BOLDITALIC,
};

/* Resolve one face: an explicit caller face wins, else a registered source for
 * `id`, else the default file path. */
static bd_font_face
resolve_face(const bd_font_face *given, const char *id, const char *default_path)
{
	if (given && (given->data || given->path))
		return *given;

	bd_asset a;
	if (bd_asset_lookup(id, &a)) {
		if (a.data)
			return (bd_font_face){ .data = a.data, .len = (long)a.len };
		if (a.path)
			return (bd_font_face){ .path = a.path };
	}
	return (bd_font_face){ .path = default_path };
}

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

static bd_font_reader font_reader;

void
bd_draw_set_font_reader(bd_font_reader fn)
{
	font_reader = fn;
}

static unsigned char *
read_file(const char *path, long *len)
{
	if (font_reader) {
		unsigned char *buf = font_reader(path, len);
		if (buf)
			return buf;
	}
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

/* Bake one face into faces[slot]. The face is either an in-memory buffer
 * (face->data) or a path to read; an unset face is skipped. metrics=1 sets the
 * shared ascent/line height (use the regular face). Silent if a variant is
 * missing; warns only when the required regular face cannot be loaded. */
static void
bake_font(const bd_font_face *face, float px, int slot, int metrics)
{
	long len = 0;
	unsigned char *ttf;
	int owned = 0;             /* read_file allocated ttf; free it after baking */

	if (face->data) {
		ttf = (unsigned char *)face->data;
		len = face->len;
	} else if (face->path) {
		ttf = read_file(face->path, &len);
		owned = 1;
	} else {
		ttf = NULL;
	}
	if (!ttf) {
		if (metrics)   /* the regular face is required; variants are optional */
			fprintf(stderr, "bd_draw: cannot read font '%s'\n",
			    face->path ? face->path : "(none)");
		return;
	}

	{
		stbtt_fontinfo info;
		if (stbtt_InitFont(&info, ttf,
		    stbtt_GetFontOffsetForIndex(ttf, 0))) {
			int asc, desc, gap;
			stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
			float s = stbtt_ScaleForPixelHeight(&info, px);
			faces[slot].ascent = asc * s;
			faces[slot].line_h = (asc - desc + gap) * s;
			if (metrics) {       /* the regular face sets the globals */
				font_ascent = faces[slot].ascent;
				font_line_h = faces[slot].line_h;
			}
		}
	}

	unsigned char *cov = malloc(ATLAS_DIM * ATLAS_DIM);
	unsigned char *rgba = malloc(ATLAS_DIM * ATLAS_DIM * 4);
	if (cov && rgba) {
		stbtt_pack_context pc;
		stbtt_PackBegin(&pc, cov, ATLAS_DIM, ATLAS_DIM, 0, 1, NULL);
		stbtt_PackSetOversampling(&pc, 1, 1);
		if (!stbtt_PackFontRange(&pc, ttf, 0, px, FONT_FIRST,
		    FONT_COUNT, faces[slot].packed))
			fprintf(stderr, "bd_draw: glyph atlas overflow\n");
		stbtt_PackEnd(&pc);

		for (int i = 0; i < ATLAS_DIM * ATLAS_DIM; i++) {
			rgba[i*4+0] = 255; rgba[i*4+1] = 255;
			rgba[i*4+2] = 255; rgba[i*4+3] = cov[i];
		}
		faces[slot].atlas = be->make_texture(ATLAS_DIM, ATLAS_DIM, rgba);
		faces[slot].have = (faces[slot].atlas.id != 0);
	}
	free(cov);
	free(rgba);
	if (owned)
		free(ttf);
}

/* the baked face for a style, falling back to the regular proportional face */
static const struct face *
face_for(int style)
{
	int i = style & (BD_FONT_BOLD | BD_FONT_ITALIC | BD_FONT_MONO);
	if (i < 0 || i >= FONT_FACES || !faces[i].have)
		i = 0;
	return &faces[i];
}

/* ------------------------------------------------------------------ */
/* public                                                             */
/* ------------------------------------------------------------------ */

int
bd_draw_init_fonts(const bd_backend *backend, const bd_font_set *fonts,
    float font_px)
{
	be = backend;

	shader = be->make_shader(VERT_SRC, FRAG_SRC);
	if (shader.id == 0)
		return 0;

	uint32_t wpix = 0xFFFFFFFFu;
	white = be->make_texture(1, 1, &wpix);
	cur_tex = white;

	/* face order matches the BD_FONT_BOLD|ITALIC|MONO style index. Each face
	 * resolves from the caller's set (if any), then the bd_asset registry by
	 * id, then its default file. A NULL set means "all defaults". */
	const bd_font_face *f[FONT_FACES] = {0};
	if (fonts) {
		f[0] = &fonts->regular;          f[1] = &fonts->bold;
		f[2] = &fonts->italic;           f[3] = &fonts->bold_italic;
		f[4] = &fonts->mono;             f[5] = &fonts->mono_bold;
		f[6] = &fonts->mono_italic;      f[7] = &fonts->mono_bold_italic;
	}
	for (int i = 0; i < FONT_FACES; i++) {
		bd_font_face face = resolve_face(f[i], font_asset_id[i],
		    font_default_path[i]);
		bake_font(&face, font_px, i, i == 0); /* regular sets metrics */
	}
	return 1;
}

int
bd_draw_init(const bd_backend *backend, const char *font_path, float font_px)
{
	/* legacy entry: font_path is the regular face; the seven variants resolve
	 * by id/default like bd_draw_init_fonts. NULL font_path = all defaults. */
	if (!font_path)
		return bd_draw_init_fonts(backend, NULL, font_px);

	bd_font_set fs = {0};
	fs.regular.path = font_path;
	return bd_draw_init_fonts(backend, &fs, font_px);
}

void
bd_draw_shutdown(void)
{
	if (!be)
		return;
	for (int i = 0; i < FONT_FACES; i++)
		if (faces[i].have) {
			be->destroy_texture(faces[i].atlas);
			faces[i].have = 0;
		}
	be->destroy_texture(white);
	be->destroy_shader(shader);
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
bd_draw_flush(void)
{
	flush();
}

int bd_draw_win_w(void) { return (int)win_w; }
int bd_draw_win_h(void) { return (int)win_h; }

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

/* Filled convex quad from four arbitrary corners (e.g. a folder-tab
 * trapezoid). Corners in order; solid color via the 1x1 white texture. */
void
bd_draw_quad(float x0, float y0, float x1, float y1,
    float x2, float y2, float x3, float y3, uint32_t rgba)
{
	if (!batch_active)
		return;
	if (cur_tex.id != white.id) {
		flush();
		cur_tex = white;
	}
	if (nverts + 6 > MAX_VERTS)
		flush();
	float r, g, b, a;
	unpack(rgba, &r, &g, &b, &a);
	bd_vertex v0 = { x0, y0, 0, 0, r, g, b, a };
	bd_vertex v1 = { x1, y1, 0, 0, r, g, b, a };
	bd_vertex v2 = { x2, y2, 0, 0, r, g, b, a };
	bd_vertex v3 = { x3, y3, 0, 0, r, g, b, a };
	verts[nverts++] = v0; verts[nverts++] = v1; verts[nverts++] = v2;
	verts[nverts++] = v0; verts[nverts++] = v2; verts[nverts++] = v3;
}

void
bd_draw_sprite(bd_texture tex, float dx, float dy, float dw, float dh,
    float u0, float v0, float u1, float v1, uint32_t rgba)
{
	quad(tex, dx, dy, dw, dh, u0, v0, u1, v1, rgba);
}

void
bd_draw_text_styled(const char *s, float x, float y, uint32_t rgba, int style)
{
	const struct face *f = face_for(style);
	if (!f->have || !s)
		return;
	float px = x, py = y + f->ascent;
	for (; *s; s++) {
		int cp = (unsigned char)*s;
		if (cp < FONT_FIRST || cp >= FONT_FIRST + FONT_COUNT)
			cp = '?';
		stbtt_aligned_quad q;
		stbtt_GetPackedQuad(f->packed, ATLAS_DIM, ATLAS_DIM,
		    cp - FONT_FIRST, &px, &py, &q, 1);
		quad(f->atlas, q.x0, q.y0, q.x1 - q.x0, q.y1 - q.y0,
		    q.s0, q.t0, q.s1, q.t1, rgba);
	}
}

float
bd_draw_text_width_styled(const char *s, int style)
{
	const struct face *f = face_for(style);
	if (!f->have || !s)
		return 0.0f;
	float px = 0, py = 0;
	for (; *s; s++) {
		int cp = (unsigned char)*s;
		if (cp < FONT_FIRST || cp >= FONT_FIRST + FONT_COUNT)
			cp = '?';
		stbtt_aligned_quad q;
		stbtt_GetPackedQuad(f->packed, ATLAS_DIM, ATLAS_DIM,
		    cp - FONT_FIRST, &px, &py, &q, 1);
	}
	return px;
}

void  bd_draw_text(const char *s, float x, float y, uint32_t rgba)
{ bd_draw_text_styled(s, x, y, rgba, 0); }
float bd_draw_text_width(const char *s)
{ return bd_draw_text_width_styled(s, 0); }

float bd_draw_line_height(void) { return font_line_h; }
float bd_draw_ascent(void)      { return font_ascent; }

float bd_draw_line_height_styled(int style) { return face_for(style)->line_h; }
float bd_draw_ascent_styled(int style)      { return face_for(style)->ascent; }
