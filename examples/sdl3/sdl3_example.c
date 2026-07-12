/*
 * sdl3_example.c -- birdie-gui on an SDL3 window + OpenGL ES 3 context, mixing
 * the toolkit's 2D UI with a hand-written 3D scene.
 *
 * The window shows a rotatable 3D tetrahedron drawn with raw GLES3 as the
 * background, and a birdie-gui UI composited on top: a floating "terminal"
 * subwindow you can drag by its title bar and minimize, and an inventory grid
 * whose "Relic" cell shows the same spinning model rendered into an FBO texture
 * each frame (the "host renders to a texture" path a backend-neutral widget
 * relies on). A floating action bar (a CRPG hotbar) sits at the bottom: drag an
 * item off the inventory (or a minimized-window tile off the dock) onto a slot
 * to bind it, then click the slot or press its Q W E R T Y hotkey to fire it.
 * A tabbed "Panels" window flips between plain widgets, the spinning relic (a
 * live GLES animation), and the rich-text editor, one full pane per tab.
 * Drag anywhere over the background to rotate it; the wheel zooms.
 *
 * It is the demo host for the SDL3 backend (bd_backend_sdl3.c, in src/birdie-gui
 * alongside the ludica backend): this file owns the window/frame loop and the
 * 3D scene, and drives the toolkit through &bd_backend_sdl3 + bd_event_from_sdl.
 *
 * Layering: the toolkit renders the whole UI in one pass that normally begins
 * by clearing the framebuffer. Here the *host* owns the frame; each iteration
 * it clears, draws the 3D scene, then lets the toolkit composite the UI. The
 * SDL3 backend leaves clear NULL (the toolkit skips it) and the UI draws over
 * the 3D. The toolkit's root frame is transparent (bg alpha 0), so only the
 * opaque subwindow and text land on top of the tetrahedron.
 *
 * The examples are a separate modular-make project (examples/ has its own copy
 * of GNUmakefile) so the main birdie build never depends on SDL3. Build and run
 * (needs SDL3 via pkg-config; libvt for the terminal widget is built from the
 * parent tree):
 *
 *   cd examples && make
 *   examples/_out/<triplet>/bin/sdl3_example
 *
 * The examples build stages the fonts next to the binary (the
 * SDL3 backend finds them via SDL_GetBasePath), so it runs from any directory.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "widget.h"
#include "widget_ext.h"     /* bd_widget_rect */
#include "bd_backend_sdl3.h"
#include "bd_widget_vt.h"
#include "bd_widget_inventory.h"
#include "bd_widget_dock.h"
#include "bd_widget_actionbar.h"
#include "bd_widget_tabview.h"
#include "bd_widget_editor.h"

#include <SDL3/SDL.h>
#include <GLES3/gl3.h>

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ================================================================== */
/* 3D tetrahedron (raw GLES3, drawn as the background)                */
/* ================================================================== */

/* Minimal column-major 4x4 matrix math. M(m,row,col) indexes an element. */
#define M(m, r, c) ((m)[(c) * 4 + (r)])

static void
mat4_identity(float *m)
{
	memset(m, 0, 16 * sizeof(float));
	M(m, 0, 0) = M(m, 1, 1) = M(m, 2, 2) = M(m, 3, 3) = 1.0f;
}

/* out = a * b (out may alias neither a nor b). */
static void
mat4_mul(float *out, const float *a, const float *b)
{
	float r[16];
	for (int c = 0; c < 4; c++)
		for (int row = 0; row < 4; row++) {
			float s = 0.0f;
			for (int k = 0; k < 4; k++)
				s += M(a, row, k) * M(b, k, c);
			r[c * 4 + row] = s;
		}
	memcpy(out, r, sizeof r);
}

static void
mat4_perspective(float *m, float fovy, float aspect, float n, float f)
{
	float t = tanf(fovy * 0.5f);
	memset(m, 0, 16 * sizeof(float));
	M(m, 0, 0) = 1.0f / (aspect * t);
	M(m, 1, 1) = 1.0f / t;
	M(m, 2, 2) = (f + n) / (n - f);
	M(m, 3, 2) = -1.0f;
	M(m, 2, 3) = (2.0f * f * n) / (n - f);
}

static void
mat4_rot_x(float *m, float a)
{
	mat4_identity(m);
	float c = cosf(a), s = sinf(a);
	M(m, 1, 1) = c; M(m, 1, 2) = -s;
	M(m, 2, 1) = s; M(m, 2, 2) = c;
}

static void
mat4_rot_y(float *m, float a)
{
	mat4_identity(m);
	float c = cosf(a), s = sinf(a);
	M(m, 0, 0) = c; M(m, 0, 2) = s;
	M(m, 2, 0) = -s; M(m, 2, 2) = c;
}

static const char *TET_VERT =
	"#version 300 es\n"
	"layout(location=0) in vec3 a_pos;\n"
	"layout(location=1) in vec3 a_col;\n"
	"layout(location=2) in vec3 a_norm;\n"
	"uniform mat4 u_mvp;\n"
	"uniform mat4 u_model;\n"
	"out vec3 v_col;\n"
	"out vec3 v_norm;\n"
	"void main(){\n"
	"    gl_Position = u_mvp * vec4(a_pos, 1.0);\n"
	"    v_norm = mat3(u_model) * a_norm;\n"
	"    v_col = a_col;\n"
	"}\n";

static const char *TET_FRAG =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec3 v_col;\n"
	"in vec3 v_norm;\n"
	"out vec4 frag;\n"
	"void main(){\n"
	"    vec3 L = normalize(vec3(0.4, 0.7, 0.6));\n"
	"    float d = abs(dot(normalize(v_norm), L));\n"  /* two-sided */
	"    frag = vec4(v_col * (d * 0.75 + 0.25), 1.0);\n"
	"}\n";

static struct {
	bd_shader prog;
	GLuint    vao, vbo;
	int       verts;
} tet;

/* Append one triangular face (positions + a flat color + face normal). */
static void
tet_face(float *buf, int *n, const float a[3], const float b[3],
    const float c[3], const float col[3])
{
	/* face normal = normalize((b-a) x (c-a)) */
	float u[3] = { b[0]-a[0], b[1]-a[1], b[2]-a[2] };
	float v[3] = { c[0]-a[0], c[1]-a[1], c[2]-a[2] };
	float nrm[3] = {
		u[1]*v[2] - u[2]*v[1],
		u[2]*v[0] - u[0]*v[2],
		u[0]*v[1] - u[1]*v[0],
	};
	float len = sqrtf(nrm[0]*nrm[0] + nrm[1]*nrm[1] + nrm[2]*nrm[2]);
	if (len > 0.0f) { nrm[0]/=len; nrm[1]/=len; nrm[2]/=len; }

	const float *tri[3] = { a, b, c };
	for (int i = 0; i < 3; i++) {
		float *o = buf + (*n) * 9;
		o[0]=tri[i][0]; o[1]=tri[i][1]; o[2]=tri[i][2];
		o[3]=col[0];    o[4]=col[1];    o[5]=col[2];
		o[6]=nrm[0];    o[7]=nrm[1];    o[8]=nrm[2];
		(*n)++;
	}
}

static void
tet_init(void)
{
	/* Regular tetrahedron, scaled to fit the view. */
	const float k = 0.85f;
	float p0[3] = {  k,  k,  k };
	float p1[3] = {  k, -k, -k };
	float p2[3] = { -k,  k, -k };
	float p3[3] = { -k, -k,  k };

	float red[3]    = { 0.90f, 0.28f, 0.30f };
	float green[3]  = { 0.35f, 0.78f, 0.42f };
	float blue[3]   = { 0.32f, 0.55f, 0.95f };
	float yellow[3] = { 0.95f, 0.80f, 0.30f };

	float buf[12 * 9];
	int n = 0;
	tet_face(buf, &n, p0, p1, p2, red);
	tet_face(buf, &n, p0, p3, p1, green);
	tet_face(buf, &n, p0, p2, p3, blue);
	tet_face(buf, &n, p1, p3, p2, yellow);
	tet.verts = n;

	tet.prog = bd_backend_sdl3.make_shader(TET_VERT, TET_FRAG);

	glGenVertexArrays(1, &tet.vao);
	glGenBuffers(1, &tet.vbo);
	glBindVertexArray(tet.vao);
	glBindBuffer(GL_ARRAY_BUFFER, tet.vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 9 * n, buf, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float),
	    (void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float),
	    (void *)(3 * sizeof(float)));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 9 * sizeof(float),
	    (void *)(6 * sizeof(float)));
	glBindVertexArray(0);
}

/* Build the MVP and submit the tetrahedron to whatever framebuffer/viewport is
 * current. flip_y mirrors clip-space Y, needed when rendering into an FBO
 * texture that the 2D toolkit samples top-left-origin (see relic_render). */
static void
tet_submit(float rot_x, float rot_y, float cam_dist, float aspect, int flip_y)
{
	float rx[16], ry[16], model[16], view[16], proj[16], vm[16], mvp[16];
	mat4_rot_x(rx, rot_x);
	mat4_rot_y(ry, rot_y);
	mat4_mul(model, ry, rx);                 /* model = Ry * Rx (rotation only) */

	mat4_identity(view);
	M(view, 2, 3) = -cam_dist;               /* translate along -Z */

	mat4_perspective(proj, 0.8f, aspect, 0.1f, 100.0f);
	if (flip_y)
		M(proj, 1, 1) = -M(proj, 1, 1);

	mat4_mul(vm, view, model);
	mat4_mul(mvp, proj, vm);

	bd_backend_sdl3.use_shader(tet.prog);
	bd_backend_sdl3.set_uniform_mat4(tet.prog, "u_mvp", mvp);
	bd_backend_sdl3.set_uniform_mat4(tet.prog, "u_model", model);

	glBindVertexArray(tet.vao);
	glDrawArrays(GL_TRIANGLES, 0, tet.verts);
	glBindVertexArray(0);
}

/* Draw the tetrahedron into the full window at the given rotation and camera
 * distance. Leaves depth testing on; the toolkit's clear() turns it back off
 * before the UI draws. */
static void
tet_draw(float rot_x, float rot_y, float cam_dist, int w, int h)
{
	glViewport(0, 0, w, h);
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);   /* two-sided lighting; winding-agnostic */
	tet_submit(rot_x, rot_y, cam_dist, h > 0 ? (float)w / (float)h : 1.0f, 0);
}

/* ------------------------------------------------------------------ */
/* animated 3D "relic": the tetrahedron rendered into an FBO texture   */
/* each frame, so an inventory cell can show a live, spinning model.   */
/* This is the "host renders to a texture" path a backend-neutral      */
/* widget relies on: bd_widget_inventory only ever blits item.icon.    */
/* ------------------------------------------------------------------ */

#define RELIC_SZ 128

static struct {
	GLuint fbo, tex, depth;
} relic;

static void
relic_init(void)
{
	glGenTextures(1, &relic.tex);
	glBindTexture(GL_TEXTURE_2D, relic.tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, RELIC_SZ, RELIC_SZ, 0, GL_RGBA,
	    GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenRenderbuffers(1, &relic.depth);
	glBindRenderbuffer(GL_RENDERBUFFER, relic.depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
	    RELIC_SZ, RELIC_SZ);

	glGenFramebuffers(1, &relic.fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, relic.fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_TEXTURE_2D, relic.tex, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
	    GL_RENDERBUFFER, relic.depth);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		fprintf(stderr, "sdl3: relic FBO incomplete\n");
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/* Render the spinning tetrahedron into relic.tex. Y is flipped so the result,
 * sampled by the toolkit as a top-left-origin sprite, appears upright. */
static void
relic_render(float angle)
{
	glBindFramebuffer(GL_FRAMEBUFFER, relic.fbo);
	glViewport(0, 0, RELIC_SZ, RELIC_SZ);
	glDisable(GL_SCISSOR_TEST);
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);   /* transparent: cell bg shows through */
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	tet_submit(angle * 0.5f, angle, 3.0f, 1.0f, 1);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}


/* ================================================================== */
/* demo UI: a floating, minimizable terminal subwindow                */
/* ================================================================== */

enum {
	SUBWIN_W      = 560,
	SUBWIN_FULL_H = 340,
	TITLE_H       = 26,
};

static bd_id root;
static bd_id subwin;     /* the floating window panel */
static bd_id titlebar;   /* its drag handle */
static bd_id min_btn;    /* minimize / restore button */
static bd_id term;       /* terminal body */
static bd_id term_input; /* command line */
static bd_id palette;    /* a real top-level window run by the in-surface WM */
static bd_id invwin;     /* the inventory, hosted as a real WM-managed window */
static bd_id actionbar;  /* CRPG hotbar: drop items onto it, press 1..6 to fire */
static int   minimized;

static void
report(const char *msg)
{
	if (term) {
		bd_terminal_write(term, msg, -1);
		bd_terminal_write(term, "\r\n", 2);
	}
}

static void
on_minimize(bd_id id, void *arg)
{
	(void)id; (void)arg;
	minimized = !minimized;
	bd_set(term,       BD_VISIBLE_B, !minimized, BD_END);
	bd_set(term_input, BD_VISIBLE_B, !minimized, BD_END);
	bd_set(subwin, BD_PREF_H_I, minimized ? TITLE_H : SUBWIN_FULL_H, BD_END);
	bd_set(min_btn, BD_LABEL_S, minimized ? "+" : "_", BD_END);
}

static void
on_dock_left(bd_id id, void *arg)
{
	(void)id; (void)arg;
	bd_window_dock(palette, BD_GRAVITY_LEFT);
	report("palette docked to the left edge");
}

static void
on_palette_float(bd_id id, void *arg)
{
	(void)id; (void)arg;
	bd_window_move(palette, 360, 320);
	report("palette floating");
}

static void
on_palette_minimize(bd_id id, void *arg)
{
	(void)id; (void)arg;
	bd_window_minimize(palette);
	report("palette minimized -> dock tile (click it to restore)");
}

/* The input line fires its click handler on Enter, before it clears, so the
 * submitted text is still readable here. Echo it to the terminal. */
static void
on_submit(bd_id id, void *arg)
{
	(void)arg;
	const char *cmd = bd_get_s(id, BD_LABEL_S);
	if (!cmd || !cmd[0])
		return;
	char line[1024];
	snprintf(line, sizeof line, "> %s", cmd);
	report(line);
}

/* ---- inventory grid: an RPG bag, one cell holding the live 3D relic ---- */

static bd_texture inv_icons[3];   /* static item icons */
static struct bag_slot { const char *name; bd_texture icon; int count; }
    inv_bag[16];

/* a 32x32 solid-color icon with a darker border, uploaded as a texture */
static bd_texture
make_icon(unsigned rgba)
{
	unsigned char px[32 * 32 * 4];
	unsigned edge = ((rgba >> 1) & 0x7F7F7F00u) | 0xFFu;
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++) {
			unsigned c = (x < 2 || x > 29 || y < 2 || y > 29) ? edge : rgba;
			unsigned char *p = px + (y * 32 + x) * 4;
			p[0] = (c >> 24) & 0xFF; p[1] = (c >> 16) & 0xFF;
			p[2] = (c >> 8) & 0xFF;  p[3] = c & 0xFF;
		}
	return bd_backend_sdl3.make_texture(32, 32, px);
}

static void
inv_get(void *ctx, int slot, bd_inventory_item *out)
{
	(void)ctx;
	if (slot < 0 || slot >= 16 || !inv_bag[slot].name)
		return;
	out->key     = (uint64_t)(slot + 1);
	out->label   = inv_bag[slot].name;
	out->icon    = inv_bag[slot].icon;
	out->count   = inv_bag[slot].count;
	out->enabled = 1;
	out->tooltip = inv_bag[slot].name;
}
static void
inv_activate(bd_id w, int slot, uint64_t key, void *ctx)
{
	(void)w; (void)key; (void)ctx;
	char b[64];
	snprintf(b, sizeof b, "use %s (slot %d)",
	    inv_bag[slot].name ? inv_bag[slot].name : "?", slot);
	report(b);
}
static void
inv_context(bd_id w, int slot, uint64_t key, int sx, int sy, void *ctx)
{
	(void)w; (void)key; (void)sx; (void)sy; (void)ctx;
	char b[48];
	snprintf(b, sizeof b, "right-click slot %d", slot);
	report(b);
}
static void
inv_move(bd_id w, int from, int to, void *ctx)
{
	(void)w; (void)ctx;
	struct bag_slot t = inv_bag[to];   /* swap the two slots */
	inv_bag[to] = inv_bag[from];
	inv_bag[from] = t;
	char b[48];
	snprintf(b, sizeof b, "moved slot %d -> %d", from, to);
	report(b);
}

/* ---- action bar: a CRPG hotbar, filled by dragging items onto it ---- */
static void
act_activate(bd_id w, int slot, uint64_t key, void *ctx)
{
	(void)w; (void)key; (void)ctx;
	char b[48];
	snprintf(b, sizeof b, "fire action in slot %d", slot);
	report(b);
}
static int
act_drop(bd_id w, int slot, const bd_dnd_payload *p, void *ctx)
{
	(void)w; (void)ctx;
	char b[80];
	snprintf(b, sizeof b, "bound \"%s\" to action slot %d (hotkey %d)",
	    p->label ? p->label : "?", slot, slot + 1);
	report(b);
	return 1;   /* accept: the bar copies the item's icon/label into the slot */
}

/* one-cell model for the tab-view "Relic" pane: shows the live FBO texture, so
 * a tab pane can hold a running GLES animation as readily as static widgets */
static void
relic_get(void *ctx, int slot, bd_inventory_item *out)
{
	(void)ctx; (void)slot;
	out->key = 1;
	out->label = "Relic";
	out->icon = (bd_texture){ relic.tex };
	out->enabled = 1;
}

static void
build_ui(void)
{
	/* Transparent, fixed-layout root: children float at absolute X/Y and the
	 * 3D background shows through everywhere they don't paint. */
	root = bd_create(BD_NONE, BD_FRAME,
	    BD_LAYOUT_I, BD_LAYOUT_FIXED,
	    BD_PAD_I,    0,
	    BD_BG_C,     0x00000000u,     /* alpha 0 => nothing drawn */
	    BD_END);

	bd_create(root, BD_LABEL,
	    BD_LABEL_S, "Drag the tetrahedron to rotate  -  wheel to zoom  -  "
	                "the Relic cell shows the same model rendered to a texture",
	    BD_X_I, 12, BD_Y_I, 10, BD_PREF_W_I, 900, BD_PREF_H_I, 18,
	    BD_BG_C, 0x00000000u, BD_FG_C, 0xE8ECF0FFu,
	    BD_END);

	/* The floating terminal subwindow: an opaque panel laid out as a column
	 * of title bar / terminal / command line. */
	subwin = bd_create(root, BD_PANEL,
	    BD_LAYOUT_I, BD_LAYOUT_COL,
	    BD_X_I, 90, BD_Y_I, 70,
	    BD_PREF_W_I, SUBWIN_W, BD_PREF_H_I, SUBWIN_FULL_H,
	    BD_PAD_I, 0, BD_GAP_I, 0,
	    BD_END);

	titlebar = bd_create(subwin, BD_PANEL,
	    BD_LAYOUT_I, BD_LAYOUT_ROW,
	    BD_PREF_H_I, TITLE_H, BD_PAD_I, 3, BD_GAP_I, 4,
	    BD_BG_C, 0x2A3340FFu,
	    BD_END);
	bd_create(titlebar, BD_LABEL,
	    BD_LABEL_S, "Terminal", BD_GROW_I, 1,
	    BD_BG_C, 0x00000000u, BD_FG_C, 0xDCE3EAFFu,
	    BD_END);
	min_btn = bd_create(titlebar, BD_BUTTON,
	    BD_LABEL_S, "_", BD_PREF_W_I, 34,
	    BD_ON_CLICK_F, on_minimize,
	    BD_END);

	term = bd_terminal_create(subwin, BD_GROW_I, 1, BD_END);
	bd_terminal_write(term,
	    "birdie-gui + SDL3, 2D UI over a 3D scene.\r\n"
	    "This terminal is a draggable, minimizable subwindow.\r\n"
	    "Type below and press Enter.\r\n", -1);

	term_input = bd_create(subwin, BD_INPUT_LINE,
	    BD_PREF_H_I, 26, BD_ON_CLICK_F, on_submit,
	    BD_END);

	/* Inventory window. A genuine top-level frame (parent BD_NONE), so the
	 * in-surface window manager treats it exactly like the palette below: a
	 * real title bar to drag it by, and the minimize / lock / close buttons.
	 * Minimize it (the "_" button) and it collapses to a tile on the left dock;
	 * click the tile to restore it. Slot 0 holds the live 3D relic (its icon is
	 * the FBO texture updated each frame in the main loop); the rest are static
	 * items. Drag between slots, right-click, wheel to scroll, hover for a
	 * tooltip, or drag an item onto the action bar. */
	inv_icons[0] = make_icon(0xD24B4BFFu);   /* potion  */
	inv_icons[1] = make_icon(0xE8C24AFFu);   /* gold    */
	inv_icons[2] = make_icon(0x6FB36FFFu);   /* herb    */
	inv_bag[0] = (struct bag_slot){ "Relic",  (bd_texture){ relic.tex }, 1  };
	inv_bag[1] = (struct bag_slot){ "Potion", inv_icons[0], 5  };
	inv_bag[2] = (struct bag_slot){ "Gold",   inv_icons[1], 99 };
	inv_bag[4] = (struct bag_slot){ "Herb",   inv_icons[2], 12 };

	invwin = bd_create(BD_NONE, BD_FRAME,
	    BD_LABEL_S, "Inventory",
	    BD_LAYOUT_I, BD_LAYOUT_COL, BD_PAD_I, 4, BD_GAP_I, 0,
	    BD_X_I, 720, BD_Y_I, 70,
	    BD_PREF_W_I, 260, BD_PREF_H_I, 300,
	    BD_BG_C, 0x222A33FFu,
	    BD_END);
	bd_id inv = bd_inventory_create(invwin, 4, 4,
	    &(bd_inventory_model){ .get = inv_get },
	    &(bd_inventory_cb){ .activate = inv_activate, .context = inv_context,
	        .move = inv_move },
	    BD_GROW_I, 1, BD_END);
	bd_inventory_set_cell_size(inv, 44);

	/* A genuine second top-level frame (parent BD_NONE). Because the SDL3
	 * backend is single-surface, the toolkit's in-surface window manager
	 * draws and decorates it: drag the title bar to move it, drag it against
	 * a screen edge to dock it, and click the padlock to pin it in place.
	 * (subwin above is only a panel faking a window; this one is the real
	 * thing the WM manages.) */
	palette = bd_create(BD_NONE, BD_FRAME,
	    BD_LABEL_S, "Palette",
	    BD_LAYOUT_I, BD_LAYOUT_COL, BD_PAD_I, 8, BD_GAP_I, 6,
	    BD_X_I, 360, BD_Y_I, 320,
	    BD_PREF_W_I, 190, BD_PREF_H_I, 200,
	    BD_BG_C, 0x222A33FFu,
	    BD_END);
	bd_create(palette, BD_LABEL,
	    BD_LABEL_S, "Drag my title bar;", BD_PREF_H_I, 18,
	    BD_BG_C, 0x00000000u, BD_FG_C, 0xDCE3EAFFu, BD_END);
	bd_create(palette, BD_LABEL,
	    BD_LABEL_S, "snap me to an edge.", BD_PREF_H_I, 18,
	    BD_BG_C, 0x00000000u, BD_FG_C, 0xDCE3EAFFu, BD_END);
	bd_create(palette, BD_BUTTON, BD_LABEL_S, "Dock left", BD_PREF_H_I, 26,
	    BD_ON_CLICK_F, on_dock_left, BD_END);
	bd_create(palette, BD_BUTTON, BD_LABEL_S, "Float", BD_PREF_H_I, 26,
	    BD_ON_CLICK_F, on_palette_float, BD_END);
	bd_create(palette, BD_BUTTON, BD_LABEL_S, "Minimize", BD_PREF_H_I, 26,
	    BD_ON_CLICK_F, on_palette_minimize, BD_END);

	/* A NeXTSTEP-style dock in the top-left corner. It is derived state: a tile
	 * appears for each minimized window (click the title-bar "_" button), and
	 * clicking a tile restores that window. Empty until something is minimized. */
	bd_id dock = bd_dock_create(root, NULL, BD_END);
	bd_dock_set_gravity(dock, BD_GRAVITY_TOP_LEFT);
	bd_dock_set_tile_size(dock, 48);

	/* A CRPG-style action bar (hotbar): a floating strip of six empty slots.
	 * Drag an item out of the inventory (or a minimized-window tile off the
	 * dock) and drop it on a slot to bind it there; click a bound slot to fire
	 * it. Each slot is bound to a letter hotkey (Q W E R T Y), shown in the slot
	 * corner while it is empty or hovered; press the key to trigger it (wired in
	 * the main loop). Drag the grip at its left edge to move the whole bar. */
	static const char hotkeys[6] = { 'Q', 'W', 'E', 'R', 'T', 'Y' };
	bd_id abar = bd_actionbar_create(root, 6,
	    &(bd_actionbar_cb){ .activate = act_activate, .drop = act_drop },
	    BD_X_I, 300, BD_Y_I, 650, BD_END);
	bd_actionbar_set_tile_size(abar, 44);
	for (int i = 0; i < 6; i++)
		bd_actionbar_set_hotkey(abar, i, hotkeys[i], 0);
	actionbar = abar;

	/* A tabbed window. Each pane is a full container, as complex as a window:
	 * folder tabs across the top flip between "Notes" (plain widgets), "Relic"
	 * (a live GLES animation, the spinning tetrahedron in an inventory cell),
	 * and "Editor" (the rich-text editor). Hosted in a top-level frame, so it is
	 * itself a draggable / minimizable WM window. */
	bd_id tabwin = bd_create(BD_NONE, BD_FRAME, BD_LABEL_S, "Panels",
	    BD_LAYOUT_I, BD_LAYOUT_COL, BD_PAD_I, 4,
	    BD_X_I, 60, BD_Y_I, 360, BD_PREF_W_I, 300, BD_PREF_H_I, 260,
	    BD_BG_C, 0x222A33FFu, BD_END);
	bd_id tabs = bd_tabview_create(tabwin, BD_GROW_I, 1, BD_END);

	bd_id notes = bd_tabview_add_pane(tabs, "Notes");
	bd_create(notes, BD_LABEL, BD_LABEL_S, "Each tab holds a whole", BD_PREF_H_I, 18,
	    BD_BG_C, 0x00000000u, BD_FG_C, 0xDCE3EAFFu, BD_END);
	bd_create(notes, BD_LABEL, BD_LABEL_S, "widget subtree.", BD_PREF_H_I, 18,
	    BD_BG_C, 0x00000000u, BD_FG_C, 0xDCE3EAFFu, BD_END);
	bd_create(notes, BD_BUTTON, BD_LABEL_S, "A button", BD_PREF_H_I, 26,
	    BD_ON_CLICK_F, on_palette_float, BD_END);

	bd_id relicpane = bd_tabview_add_pane(tabs, "Relic");
	bd_id relicinv = bd_inventory_create(relicpane, 1, 1,
	    &(bd_inventory_model){ .get = relic_get }, NULL, BD_GROW_I, 1, BD_END);
	bd_inventory_set_cell_size(relicinv, 128);

	bd_id edpane = bd_tabview_add_pane(tabs, "Editor");
	bd_id ed = bd_editor_create(edpane, BD_GROW_I, 1, BD_END);
	bd_editor_set_text(ed, "The editor lives in a tab.\n"
	    "Type here; switch tabs and back\nand your text is kept.");
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

static int
in_rect(int px, int py, int x, int y, int w, int h)
{
	return px >= x && px < x + w && py >= y && py < y + h;
}

int
main(void)
{
	/* The SDL3 backend owns the window, GLES3 context, and SDL lifecycle. */
	SDL_Window *win = bd_backend_sdl3_open("birdie-gui on SDL3", 1024, 720);
	if (!win)
		return 1;

	bd_gui_init(&bd_backend_sdl3, NULL);   /* NULL theme = defaults */
	tet_init();
	relic_init();
	build_ui();

	/* Host-side drag state: the toolkit consumes events over its widgets; an
	 * unconsumed left-drag either moves the subwindow (started on the title
	 * bar) or rotates the tetrahedron (started over the 3D background). */
	enum { DRAG_NONE, DRAG_ROTATE, DRAG_MOVE } drag = DRAG_NONE;
	int drag_off_x = 0, drag_off_y = 0, last_x = 0, last_y = 0;
	float rot_x = 0.5f, rot_y = 0.7f, cam_dist = 4.0f;
	double last_time = bd_backend_sdl3.time();

	int running = 1;
	while (running) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_EVENT_QUIT) {
				running = 0;
				continue;
			}

			bd_event bev;
			if (!bd_event_from_sdl(&ev, &bev))
				continue;

			if (bev.type == BD_EV_MOUSE_DOWN &&
			    bev.button == BD_MOUSE_LEFT) {
				if (bd_gui_event(&bev))
					continue;   /* a widget took it */
				int tx, ty, tw, th;
				bd_widget_rect(titlebar, &tx, &ty, &tw, &th);
				if (in_rect(bev.x, bev.y, tx, ty, tw, th)) {
					int sx, sy;
					bd_widget_rect(subwin, &sx, &sy, NULL, NULL);
					drag = DRAG_MOVE;
					drag_off_x = bev.x - sx;
					drag_off_y = bev.y - sy;
				} else {
					drag = DRAG_ROTATE;
					last_x = bev.x;
					last_y = bev.y;
				}
			} else if (bev.type == BD_EV_MOUSE_MOVE && drag == DRAG_ROTATE) {
				rot_y += (float)(bev.x - last_x) * 0.01f;
				rot_x += (float)(bev.y - last_y) * 0.01f;
				if (rot_x >  1.55f) rot_x =  1.55f;
				if (rot_x < -1.55f) rot_x = -1.55f;
				last_x = bev.x;
				last_y = bev.y;
			} else if (bev.type == BD_EV_MOUSE_MOVE && drag == DRAG_MOVE) {
				int nx = bev.x - drag_off_x;
				int ny = bev.y - drag_off_y;
				int ww = bd_backend_sdl3.width(), wh = bd_backend_sdl3.height();
				if (nx < 0) nx = 0;
				if (ny < 0) ny = 0;
				if (nx > ww - 40)     nx = ww - 40;
				if (ny > wh - TITLE_H) ny = wh - TITLE_H;
				bd_set(subwin, BD_X_I, nx, BD_Y_I, ny, BD_END);
			} else if (bev.type == BD_EV_MOUSE_UP &&
			    bev.button == BD_MOUSE_LEFT) {
				drag = DRAG_NONE;
				bd_gui_event(&bev);
			} else if (bev.type == BD_EV_MOUSE_SCROLL) {
				if (!bd_gui_event(&bev)) {   /* not over the terminal */
					cam_dist -= bev.scroll_dy * 0.4f;
					if (cam_dist < 2.5f)  cam_dist = 2.5f;
					if (cam_dist > 12.0f) cam_dist = 12.0f;
				}
			} else if (bev.type == BD_EV_KEY_DOWN &&
			    bd_focused() == BD_NONE &&
			    bd_actionbar_key(actionbar, bev.key, bev.mods)) {
				/* a hotbar key fired its action (only when no text
				 * field has focus, so typing is never stolen) */
			} else {
				bd_gui_event(&bev);
			}
		}

		/* gentle idle spin unless the user is rotating by hand */
		double now = bd_backend_sdl3.time();
		float dt = (float)(now - last_time);
		last_time = now;
		if (drag != DRAG_ROTATE)
			rot_y += dt * 0.3f;

		int w = bd_backend_sdl3.width(), h = bd_backend_sdl3.height();

		/* Update the inventory relic's texture: render the spinning model into
		 * its FBO before the UI draws, so the inventory cell blits a fresh
		 * frame. */
		relic_render((float)now);

		/* The host owns the frame: clear, draw the 3D background, then let
		 * the toolkit composite the UI on top (its clear() is a no-op). */
		glDisable(GL_SCISSOR_TEST);
		glClearColor(0.05f, 0.06f, 0.08f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		tet_draw(rot_x, rot_y, cam_dist, w, h);

		bd_gui_layout(w, h);
		bd_gui_render();
		SDL_GL_SwapWindow(win);
	}

	bd_gui_cleanup();
	glDeleteFramebuffers(1, &relic.fbo);
	glDeleteTextures(1, &relic.tex);
	glDeleteRenderbuffers(1, &relic.depth);
	bd_backend_sdl3_close();
	return 0;
}
