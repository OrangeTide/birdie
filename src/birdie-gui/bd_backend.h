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
typedef struct { unsigned id; } bd_shader;

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
	BD_EV_KEY_UP,
	BD_EV_TEXT_COMMIT,  /* committed text (IME / compose / dead keys): `text` */
	BD_EV_TEXT_PREEDIT, /* in-progress composition: `text` + `caret` (bytes) */
	BD_EV_TOUCH_DOWN,   /* a finger touched down: `touch` id at `x`,`y` */
	BD_EV_TOUCH_MOVE,
	BD_EV_TOUCH_UP,
	BD_EV_PEN_HOVER,    /* stylus moving in proximity, not touching the surface */
	BD_EV_PEN_DOWN,     /* stylus tip contacted the surface */
	BD_EV_PEN_MOVE,     /* stylus moving while in contact */
	BD_EV_PEN_UP,       /* stylus tip lifted off the surface */
	BD_EV_DROP,         /* toolkit-synthesized: a cross-widget drag was released
	                       over this widget; read the payload with bd_dnd_get().
	                       Not injected by the host; the toolkit delivers it to an
	                       extension widget's event() at `x`,`y` on mouse-up. */
	BD_EV_FOCUS_IN,     /* the window `window` gained OS input focus */
	BD_EV_FOCUS_OUT,    /* the window `window` lost OS input focus. The toolkit
	                       tracks these so an app can throttle its render loop
	                       when unfocused (bd_gui_focused / bd_window_focused). */
};

/* Pen flag bitmask (bd_event.pen_flags), valid for the BD_EV_PEN_* events. */
enum {
	BD_PEN_INRANGE = 1 << 0, /* stylus is within sensing proximity */
	BD_PEN_BARREL  = 1 << 1, /* barrel (side) button held */
	BD_PEN_ERASER  = 1 << 2, /* the eraser end is in use, not the tip */
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
	BD_KEY_F2,
};

/* Flattened event. Only the fields relevant to `type` are valid. */
typedef struct {
	int      type;          /* enum bd_ev_type */
	int      mods;          /* BD_MOD_* bitmask */
	int      x, y;          /* mouse / touch position */
	int      button;        /* BD_MOUSE_* (mouse down / up) */
	int      touch;         /* touch-point id (BD_EV_TOUCH_*) */
	float    pressure;      /* stylus tip pressure 0..1 (BD_EV_PEN_*) */
	float    tilt_x, tilt_y;/* stylus tilt in degrees, ~-90..90 (BD_EV_PEN_*) */
	int      pen_flags;     /* BD_PEN_* bitmask (BD_EV_PEN_*) */
	float    scroll_dy;     /* wheel delta (scroll) */
	int      key;           /* BD_KEY_* (key down / up) */
	int      repeat;        /* key down: 1 if an auto-repeat, else 0 */
	unsigned codepoint;     /* Unicode codepoint (char) */
	const char *text;       /* UTF-8 for TEXT_COMMIT / TEXT_PREEDIT; valid for
	                           the dispatch only */
	int      caret;         /* TEXT_PREEDIT: caret byte offset within `text` */
	int      window;        /* originating window id (0 = primary). Backends
	                           without multi_window leave this 0. */
} bd_event;

/* ------------------------------------------------------------------ */
/* renderer backend                                                   */
/* ------------------------------------------------------------------ */

/*
 * The backend provides a window + GLES-class GPU surface: shaders, vertex
 * draws, uniforms, textures, and scissor clipping. It is the level ludica
 * (lud_make_shader / lud_uniform_* / lud_scissor) and raw GLES both provide.
 * The toolkit's renderer (bd_draw.c) builds chrome and text on top of it, and
 * extension widgets can drop to a custom shader for effects like shaded knobs.
 *
 * Coordinates are pixels, origin top-left. Shaders consume the bd_vertex
 * attributes by name at locations 0,1,2: a_pos (vec2), a_uv (vec2),
 * a_col (vec4). The backend establishes the 2D UI render state (alpha blend
 * on, depth test and face culling off) as part of draw_verts, not as a side
 * effect of clear, so a host that owns the frame can leave clear NULL.
 */
typedef struct bd_backend {
	/* frame / window */
	int    (*width)(void);
	int    (*height)(void);
	/* Monotonic seconds. Optional: leave NULL and the toolkit uses its own
	 * monotonic clock (bd_time()). Supply this only when the host has a
	 * frame-synced clock the toolkit should share. */
	double (*time)(void);
	void   (*viewport)(int x, int y, int w, int h);
	/* Clear the frame to the given color at the start of each rendered
	 * window. Optional: leave NULL when the host clears the framebuffer
	 * itself (e.g. it draws a 3D background first and composites the UI on
	 * top). draw_verts sets the render state regardless, so a NULL clear
	 * costs nothing but the buffer clear. */
	void   (*clear)(float r, float g, float b, float a);

	/* shaders */
	bd_shader (*make_shader)(const char *vert_glsl, const char *frag_glsl);
	void      (*destroy_shader)(bd_shader sh);
	void      (*use_shader)(bd_shader sh);
	void (*set_uniform_int)  (bd_shader sh, const char *name, int v);
	void (*set_uniform_float)(bd_shader sh, const char *name, float v);
	void (*set_uniform_vec2) (bd_shader sh, const char *name, float x, float y);
	void (*set_uniform_vec3) (bd_shader sh, const char *name, float x, float y, float z);
	void (*set_uniform_vec4) (bd_shader sh, const char *name,
	                          float x, float y, float z, float w);
	void (*set_uniform_mat4) (bd_shader sh, const char *name, const float m[16]);

	/* draw a triangle list of `count` vertices with the bound shader and
	 * texture unit 0. The backend owns vertex-buffer management and sets the
	 * 2D UI render state (alpha blend on, depth test and cull off) here, so
	 * the toolkit renders correctly whether or not clear ran this frame. */
	void (*draw_verts)(const bd_vertex *verts, int count);

	/* textures */
	bd_texture (*load_texture)(const char *path);
	/* Decode an image (PNG) from an in-memory buffer, same filtering as
	 * load_texture. Optional (may be NULL): needed only to serve textures
	 * registered as embedded data through bd_asset. */
	bd_texture (*load_texture_mem)(const unsigned char *data, int len);
	bd_texture (*make_texture)(int w, int h, const void *rgba); /* rgba NULL = blank */
	void       (*update_texture)(bd_texture t, int x, int y, int w, int h,
	                             const void *rgba);
	void       (*bind_texture)(bd_texture t, int unit);
	void       (*destroy_texture)(bd_texture tex);

	/* scissor clip rectangle (pixels, same space as viewport). */
	void (*scissor)(int x, int y, int w, int h);
	void (*scissor_off)(void);

	/* ---- multiple native windows (optional) ----
	 *
	 * A backend that can host more than one OS window sets multi_window to 1
	 * and provides the window_* hooks. The toolkit then maps each top-level
	 * BD_FRAME to a backend window: it renders each window's tree between
	 * window_begin()/window_swap() and tags input events with the window id
	 * the event came from.
	 *
	 * Window id 1 is the primary window the host already opened before
	 * bd_gui_init(); the toolkit adopts it for the first top-level frame and
	 * calls window_open() for any further frames. The id is also what appears
	 * in bd_event.window.
	 *
	 * When multi_window is 0 these hooks are unused (leave them NULL): the
	 * toolkit renders a single window and the host presents it as before. */
	int multi_window;
	int  (*window_open)(const char *title, int w, int h); /* >0 id, 0 = fail */
	void (*window_close)(int id);
	void (*window_begin)(int id);          /* make current + set draw target */
	void (*window_swap)(int id);           /* present the rendered frame */
	int  (*window_width)(int id);
	int  (*window_height)(int id);
	void (*window_set_title)(int id, const char *title);
	/* Iconify / de-iconify a native window through the host's window manager
	 * (optional; NULL = unsupported). The toolkit calls these from
	 * bd_window_minimize / bd_window_restore on a multi_window backend, mapping
	 * minimize to the platform's iconify (e.g. XIconifyWindow). window_restore
	 * also raises the window. On a single-surface backend these are unused; the
	 * toolkit's in-surface window manager handles minimize itself. */
	void (*window_minimize)(int id);
	void (*window_restore)(int id);

	/* ---- clipboard (optional; NULL = no clipboard) ----
	 * set copies the UTF-8 string to the system clipboard; get returns the
	 * current clipboard text (a pointer owned by the backend, valid until the
	 * next clipboard call) or NULL if empty/unsupported. */
	void        (*clipboard_set)(const char *utf8);
	const char *(*clipboard_get)(void);

	/* ---- input method (IME / compose; optional) ----
	 * The toolkit enables the IME when a text widget gains focus and disables
	 * it otherwise (so it does not swallow keys elsewhere), and reports the
	 * caret rectangle so the platform places its own candidate window there.
	 * The platform delivers finished text as BD_EV_TEXT_COMMIT (and, where
	 * supported, in-progress text as BD_EV_TEXT_PREEDIT). */
	void (*ime_set_enabled)(int on);
	void (*ime_set_cursor_rect)(int x, int y, int w, int h);
} bd_backend;

#endif
