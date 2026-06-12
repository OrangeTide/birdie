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
} bd_backend;

#endif
