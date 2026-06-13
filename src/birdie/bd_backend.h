#ifndef BD_BACKEND_H
#define BD_BACKEND_H

/*
 * bd_backend — thin portability layer between the widget toolkit and the
 * underlying windowing/rendering library. The toolkit (widget.c) never
 * touches ludica, SDL, raylib, or GLFW directly; it speaks only to a
 * bd_backend vtable and the neutral bd_event below. A host implements the
 * vtable, translates its native events into bd_event, and injects the
 * backend via bd_gui_init().
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* Opaque resource handles. Values are meaningful only to the backend that
 * produced them; the toolkit just stores and passes them back. */
typedef struct { unsigned id; } bd_texture;
typedef struct { unsigned id; } bd_font;
typedef struct { unsigned id; } bd_shader;   /* GPU interface (v0.2) */
typedef struct { unsigned id; } bd_mesh;     /* GPU interface (v0.2) */

/* Interleaved UI vertex: position in pixels, texcoord, RGBA in [0,1]. The one
 * vertex format the GPU interface and the toolkit renderer share. */
typedef struct {
	float x, y;
	float u, v;
	float r, g, b, a;
} bd_vertex;

/* ------------------------------------------------------------------ */
/* input events                                                       */
/* ------------------------------------------------------------------ */

enum bd_ev_type {
	BD_EV_NONE = 0,
	BD_EV_MOUSE_MOVE,
	BD_EV_MOUSE_DOWN,
	BD_EV_MOUSE_UP,
	BD_EV_MOUSE_SCROLL,
	BD_EV_KEY_DOWN,
	BD_EV_CHAR,
};

/* Mouse buttons. */
enum {
	BD_MOUSE_LEFT = 1,
	BD_MOUSE_RIGHT,
	BD_MOUSE_MIDDLE,
};

/* Modifier bitmask. */
enum {
	BD_MOD_SHIFT = 1 << 0,
	BD_MOD_CTRL  = 1 << 1,
	BD_MOD_ALT   = 1 << 2,
};

/*
 * Key codes. Printable letters use their ASCII uppercase value so backends
 * can map a contiguous A..Z block arithmetically; non-printable keys live in
 * a private block above 256.
 */
enum {
	BD_KEY_UNKNOWN = 0,
	BD_KEY_A = 65,           /* 'A'..'Z' == 65..90 */
	BD_KEY_Z = 90,

	BD_KEY_LEFT = 256,
	BD_KEY_RIGHT,
	BD_KEY_UP,
	BD_KEY_DOWN,
	BD_KEY_HOME,
	BD_KEY_END,
	BD_KEY_BACKSPACE,
	BD_KEY_DELETE,
	BD_KEY_ENTER,
	BD_KEY_ESCAPE,
	BD_KEY_TAB,
};

/* Flattened event. Only the fields relevant to `type` are valid. */
typedef struct {
	int      type;          /* enum bd_ev_type */
	int      mods;          /* BD_MOD_* bitmask */
	int      x, y;          /* mouse position (move / button) */
	int      button;        /* BD_MOUSE_* (mouse down / up) */
	float    scroll_dy;     /* wheel delta (scroll) */
	int      key;           /* BD_KEY_* (key down) */
	unsigned codepoint;     /* Unicode codepoint (char) */
} bd_event;

/* ------------------------------------------------------------------ */
/* renderer backend                                                   */
/* ------------------------------------------------------------------ */

/*
 * All colors are float r,g,b,a in [0,1]. Coordinates are pixels with the
 * origin at the top-left of the window. The toolkit brackets quad drawing
 * with sprite_begin/sprite_end and proportional text with vfont_begin/
 * vfont_end, so a backend can batch within each pass.
 */
typedef struct bd_backend {
	/* frame / window */
	int    (*width)(void);
	int    (*height)(void);
	double (*time)(void);                       /* monotonic seconds */
	void   (*viewport)(int x, int y, int w, int h);
	void   (*clear)(float r, float g, float b, float a);

	/* quad / sprite batch */
	void   (*sprite_begin)(float x, float y, float w, float h);
	void   (*sprite_end)(void);
	void   (*fill_rect)(float x, float y, float w, float h,
	                    float r, float g, float b, float a);
	void   (*stroke_rect)(float x, float y, float w, float h,
	                      float r, float g, float b, float a);
	void   (*draw_tinted)(bd_texture tex,
	                      float dx, float dy, float dw, float dh,
	                      float sx, float sy, float sw, float sh,
	                      float r, float g, float b, float a);

	/* proportional vector font */
	void   (*vfont_begin)(float vx, float vy, float vw, float vh);
	void   (*vfont_end)(void);
	void   (*vfont_draw)(bd_font font, float x, float y, float size,
	                     float r, float g, float b, float a,
	                     const char *text);
	float  (*vfont_text_width)(bd_font font, float size, const char *text);

	/* resources */
	bd_texture (*load_texture)(const char *path);
	void       (*destroy_texture)(bd_texture tex);
	bd_font    (*load_font)(const char *path);
	void       (*destroy_font)(bd_font font);

	/*
	 * GPU interface (v0.2). A shader + mesh + uniform + texture surface,
	 * the level ludica (lud_make_shader/lud_make_mesh/lud_uniform_*) and raw
	 * GLES both provide. The toolkit's renderer (bd_draw.c) builds chrome and
	 * text on top of this, and extension widgets can drop to a custom shader
	 * for effects like shaded knobs. Coordinates are pixels; the backend
	 * supplies the pixel->clip projection to shaders as the "u_proj" mat4.
	 */
	bd_shader  (*make_shader)(const char *vert_glsl, const char *frag_glsl);
	void       (*destroy_shader)(bd_shader sh);
	bd_mesh    (*make_mesh)(const bd_vertex *verts, int count, int dynamic);
	void       (*update_mesh)(bd_mesh m, const bd_vertex *verts, int count);
	void       (*destroy_mesh)(bd_mesh m);
	bd_texture (*make_texture)(int w, int h, const void *rgba);  /* rgba NULL = blank */
	void       (*update_texture)(bd_texture t, int x, int y, int w, int h,
	                             const void *rgba);

	void (*use_shader)(bd_shader sh);
	void (*set_uniform_int)  (bd_shader sh, const char *name, int v);
	void (*set_uniform_float)(bd_shader sh, const char *name, float v);
	void (*set_uniform_vec2) (bd_shader sh, const char *name, float x, float y);
	void (*set_uniform_vec3) (bd_shader sh, const char *name, float x, float y, float z);
	void (*set_uniform_vec4) (bd_shader sh, const char *name,
	                          float x, float y, float z, float w);
	void (*set_uniform_mat4) (bd_shader sh, const char *name, const float m[16]);
	void (*bind_texture)(bd_texture t, int unit);
	void (*draw_mesh)(bd_mesh m);
} bd_backend;

#endif
