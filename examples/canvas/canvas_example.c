/*
 * canvas_example.c -- the smallest birdie-gui GLES-background (passthrough)
 * canvas. A spinning triangle is drawn with raw GLES3 into the rect of a
 * passthrough BD_MANAGED_CANVAS, and the toolkit's widgets are composited on
 * top. Drag the bare scene to spin it, wheel to resize it, and drag the item
 * out of the "Bag" window onto the scene to drop a marker "into the world".
 *
 * This is the minimal counterpart to sdl3_example.c. That demo routes a
 * whole-window scene the Option A way (the host peeks at bd_gui_event's return
 * value); this one shows the Option B passthrough API on a sub-rectangle:
 *
 *   - bd_managed_canvas_set_passthrough(canvas, cb): the canvas paints no
 *     backdrop (the scene shows through) and forwards bare-scene input to
 *     cb.on_input and dropped items to cb.on_drop, in canvas-local coordinates.
 *   - bd_managed_canvas_rect(canvas, ...): the rect to scissor the scene into.
 *   - the host owns the clear + scene render; bd_gui_render composites on top
 *     (the SDL3 backend leaves clear NULL, so the scene survives underneath).
 *
 * Windows that float over a passthrough canvas must be the canvas's own managed
 * frames (parented to it), so their clicks are handled before the passthrough
 * sees them; the "Bag" window below is one.
 *
 * Build: cd examples && make ; run examples/_out/<triplet>/bin/canvas_example
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "widget.h"
#include "widget_ext.h"
#include "bd_backend_sdl3.h"
#include "bd_widget_inventory.h"

#include <SDL3/SDL.h>
#include <GLES3/gl3.h>

#include <math.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* the "scene": a flat triangle spun and scaled by the canvas callbacks */
/* ------------------------------------------------------------------ */

static const char *TRI_VERT =
	"#version 300 es\n"
	"layout(location=0) in vec2 a_pos;\n"
	"layout(location=1) in vec3 a_col;\n"
	"uniform float u_angle;\n"
	"uniform float u_scale;\n"
	"uniform float u_aspect;\n"
	"out vec3 v_col;\n"
	"void main(){\n"
	"    float c = cos(u_angle), s = sin(u_angle);\n"
	"    vec2 p = mat2(c, -s, s, c) * a_pos * u_scale;\n"
	"    p.x /= u_aspect;\n"
	"    gl_Position = vec4(p, 0.0, 1.0);\n"
	"    v_col = a_col;\n"
	"}\n";

static const char *TRI_FRAG =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec3 v_col;\n"
	"out vec4 frag;\n"
	"void main(){ frag = vec4(v_col, 1.0); }\n";

/* Point-sprite shader for the "dropped into the world" markers. */
static const char *MARK_VERT =
	"#version 300 es\n"
	"layout(location=0) in vec2 a_pos;\n"        /* window pixels, top-left */
	"layout(location=1) in vec3 a_col;\n"
	"uniform vec2 u_vp;\n"
	"out vec3 v_col;\n"
	"void main(){\n"
	"    vec2 ndc = vec2(a_pos.x / u_vp.x * 2.0 - 1.0,\n"
	"                    1.0 - a_pos.y / u_vp.y * 2.0);\n"
	"    gl_Position = vec4(ndc, 0.0, 1.0);\n"
	"    gl_PointSize = 16.0;\n"
	"    v_col = a_col;\n"
	"}\n";

static const char *MARK_FRAG =
	"#version 300 es\n"
	"precision mediump float;\n"
	"in vec3 v_col;\n"
	"out vec4 frag;\n"
	"void main(){\n"
	"    float r = length(gl_PointCoord - vec2(0.5));\n"
	"    frag = vec4(v_col, smoothstep(0.5, 0.15, r));\n"
	"}\n";

static struct { bd_shader prog; GLuint vao, vbo; } tri, mark;

static bd_id canvas;
static float scene_angle, scene_scale = 0.7f;
static int   scene_drag, scene_lx;      /* left-drag spin tracking */

#define MAX_MARKERS 32
static struct { int x, y; float r, g, b; } markers[MAX_MARKERS];
static int marker_count;

static void
tri_init(void)
{
	/* one big centered triangle, per-vertex red/green/blue */
	const float v[] = {
		 0.0f,  0.9f,  0.90f, 0.30f, 0.32f,
		-0.8f, -0.6f,  0.36f, 0.80f, 0.44f,
		 0.8f, -0.6f,  0.34f, 0.56f, 0.96f,
	};
	tri.prog = bd_backend_sdl3.make_shader(TRI_VERT, TRI_FRAG);
	glGenVertexArrays(1, &tri.vao);
	glGenBuffers(1, &tri.vbo);
	glBindVertexArray(tri.vao);
	glBindBuffer(GL_ARRAY_BUFFER, tri.vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
	    (void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
	    (void *)(2 * sizeof(float)));
	glBindVertexArray(0);

	mark.prog = bd_backend_sdl3.make_shader(MARK_VERT, MARK_FRAG);
	glGenVertexArrays(1, &mark.vao);
	glGenBuffers(1, &mark.vbo);
	glBindVertexArray(mark.vao);
	glBindBuffer(GL_ARRAY_BUFFER, mark.vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
	    (void *)0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
	    (void *)(2 * sizeof(float)));
	glBindVertexArray(0);
}

/* Draw the scene into the canvas rect (cx,cy top-left in window pixels; win_h
 * flips to GL's bottom-left origin), clipped with the scissor so it never
 * bleeds under the chrome. The host already cleared the whole window. */
static void
scene_draw(int cx, int cy, int cw, int ch, int win_w, int win_h)
{
	if (cw <= 0 || ch <= 0)
		return;
	int gy = win_h - (cy + ch);

	/* the triangle fills the canvas rect: viewport and scissor both = the rect */
	glViewport(cx, gy, cw, ch);
	glScissor(cx, gy, cw, ch);
	glEnable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	bd_backend_sdl3.use_shader(tri.prog);
	bd_backend_sdl3.set_uniform_float(tri.prog, "u_angle", scene_angle);
	bd_backend_sdl3.set_uniform_float(tri.prog, "u_scale", scene_scale);
	bd_backend_sdl3.set_uniform_float(tri.prog, "u_aspect",
	    (float)cw / (float)ch);
	glBindVertexArray(tri.vao);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);

	/* the markers are positioned in whole-window pixels, so their viewport is
	 * the full window while the scissor still clips them to the canvas rect */
	if (marker_count > 0) {
		float verts[MAX_MARKERS * 5];
		for (int i = 0; i < marker_count; i++) {
			float *o = verts + i * 5;
			o[0] = (float)(cx + markers[i].x);
			o[1] = (float)(cy + markers[i].y);
			o[2] = markers[i].r;
			o[3] = markers[i].g;
			o[4] = markers[i].b;
		}
		glViewport(0, 0, win_w, win_h);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		bd_backend_sdl3.use_shader(mark.prog);
		bd_backend_sdl3.set_uniform_vec2(mark.prog, "u_vp",
		    (float)win_w, (float)win_h);
		glBindVertexArray(mark.vao);
		glBindBuffer(GL_ARRAY_BUFFER, mark.vbo);
		glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 5 * marker_count,
		    verts, GL_DYNAMIC_DRAW);
		glDrawArrays(GL_POINTS, 0, marker_count);
		glBindVertexArray(0);
	}
	glDisable(GL_SCISSOR_TEST);
}

/* ------------------------------------------------------------------ */
/* the passthrough callbacks: scene input and drop-into-world          */
/* ------------------------------------------------------------------ */

static int
on_scene_input(bd_id cv, const bd_event *ev, void *u)
{
	(void)cv; (void)u;
	switch (ev->type) {
	case BD_EV_MOUSE_DOWN:
		if (ev->button == BD_MOUSE_LEFT) {
			scene_drag = 1;
			scene_lx = ev->x;
		}
		return 1;
	case BD_EV_MOUSE_MOVE:
		if (scene_drag) {
			scene_angle += (float)(ev->x - scene_lx) * 0.01f;
			scene_lx = ev->x;
		}
		return 1;
	case BD_EV_MOUSE_UP:
		scene_drag = 0;
		return 1;
	case BD_EV_MOUSE_SCROLL:
		scene_scale += ev->scroll_dy * 0.05f;
		if (scene_scale < 0.2f) scene_scale = 0.2f;
		if (scene_scale > 1.5f) scene_scale = 1.5f;
		return 1;
	default:
		return 0;
	}
}

static const float mcol[3][3] = {
	{ 0.95f, 0.85f, 0.30f },
	{ 0.40f, 0.85f, 0.55f },
	{ 0.55f, 0.65f, 1.00f },
};

static int
on_scene_drop(bd_id cv, const bd_dnd_payload *p, int lx, int ly, void *u)
{
	(void)cv; (void)u;
	if (marker_count < MAX_MARKERS) {
		int ci = marker_count % 3;
		markers[marker_count].x = lx;
		markers[marker_count].y = ly;
		markers[marker_count].r = mcol[ci][0];
		markers[marker_count].g = mcol[ci][1];
		markers[marker_count].b = mcol[ci][2];
		marker_count++;
	}
	printf("dropped \"%s\" into the world at (%d, %d)\n",
	    p && p->label ? p->label : "?", lx, ly);
	return 1;
}

/* ------------------------------------------------------------------ */
/* a one-item bag, the drag source                                     */
/* ------------------------------------------------------------------ */

static bd_texture item_icon;

static void
bag_get(void *ctx, int slot, bd_inventory_item *out)
{
	(void)ctx;
	if (slot != 0)
		return;
	out->key     = 7;
	out->label   = "Cube";
	out->icon    = item_icon;
	out->count   = 1;
	out->enabled = 1;
}

/* a 32x32 solid-color icon uploaded as a texture */
static bd_texture
make_icon(unsigned rgba)
{
	unsigned char px[32 * 32 * 4];
	for (int i = 0; i < 32 * 32; i++) {
		px[i * 4 + 0] = (rgba >> 24) & 0xFF;
		px[i * 4 + 1] = (rgba >> 16) & 0xFF;
		px[i * 4 + 2] = (rgba >> 8) & 0xFF;
		px[i * 4 + 3] = rgba & 0xFF;
	}
	return bd_backend_sdl3.make_texture(32, 32, px);
}

static void
build_ui(void)
{
	/* Transparent column: a header strip over the passthrough canvas, which
	 * fills the rest and hosts the bag window. */
	bd_id root = bd_create(BD_NONE, BD_FRAME,
	    BD_LAYOUT_I, BD_LAYOUT_COL, BD_PAD_I, 0, BD_GAP_I, 0,
	    BD_BG_C, 0x00000000u, BD_END);

	bd_create(root, BD_LABEL,
	    BD_LABEL_S, "passthrough canvas  -  drag the triangle to spin it, wheel "
	                "to resize, drop the Cube onto it",
	    BD_PREF_H_I, 22, BD_PAD_I, 8,
	    BD_BG_C, 0x11151CFFu, BD_FG_C, 0xE8ECF0FFu, BD_END);

	canvas = bd_managed_canvas_create(root, BD_GROW_I, 1, BD_END);
	bd_managed_canvas_set_passthrough(canvas,
	    &(bd_canvas_cb){ .on_input = on_scene_input, .on_drop = on_scene_drop });

	/* The bag: a canvas-managed frame (decorated and floated by the canvas's
	 * window manager) holding a one-cell inventory to drag from. */
	item_icon = make_icon(0xE0803CFFu);
	bd_id bag = bd_create(canvas, BD_FRAME, BD_LABEL_S, "Bag",
	    BD_LAYOUT_I, BD_LAYOUT_COL, BD_PAD_I, 6,
	    BD_X_I, 40, BD_Y_I, 40, BD_PREF_W_I, 120, BD_PREF_H_I, 120,
	    BD_BG_C, 0x222A33FFu, BD_END);
	bd_id inv = bd_inventory_create(bag, 1, 1,
	    &(bd_inventory_model){ .get = bag_get }, NULL, BD_GROW_I, 1, BD_END);
	bd_inventory_set_cell_size(inv, 64);
}

int
main(void)
{
	SDL_Window *win = bd_backend_sdl3_open("birdie-gui passthrough canvas",
	    800, 600);
	if (!win)
		return 1;

	bd_gui_init(&bd_backend_sdl3, NULL);
	tri_init();
	build_ui();

	int running = 1;
	while (running) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_EVENT_QUIT) {
				running = 0;
				continue;
			}
			bd_event bev;
			if (bd_event_from_sdl(&ev, &bev))
				bd_gui_event(&bev);   /* the canvas forwards bare-scene input */
		}

		int w = bd_backend_sdl3.width(), h = bd_backend_sdl3.height();

		/* Lay out first so the canvas rect is current, then clear the whole
		 * window, draw the scene into the canvas rect, and composite the UI. */
		bd_gui_layout(w, h);
		glDisable(GL_SCISSOR_TEST);
		glClearColor(0.06f, 0.07f, 0.09f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

		int cx, cy, cw, ch;
		if (bd_managed_canvas_rect(canvas, &cx, &cy, &cw, &ch))
			scene_draw(cx, cy, cw, ch, w, h);

		bd_gui_render();
		SDL_GL_SwapWindow(win);
	}

	bd_gui_cleanup();
	bd_backend_sdl3_close();
	return 0;
}
