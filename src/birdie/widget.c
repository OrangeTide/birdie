#include "widget.h"
#include "widget_ext.h"
#include "bd_backend.h"
#include "bd_draw.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* constants                                                          */
/* ------------------------------------------------------------------ */

#define MAX_WIDGETS 256
#define TYPEMASK    0xF
#define TYPE_I      1
#define TYPE_S      2
#define TYPE_P      3
#define TYPE_F      4
#define TYPE_C      5
#define TYPE_B      6

/*
 * Asset paths — compile-time configurable. Override any of these with
 * -DBD_ASSET_xxx='"path"' at build time; the defaults assume the source
 * tree layout.
 */
#ifndef BD_ASSET_GUI_FONT
#define BD_ASSET_GUI_FONT    "src/birdie/assets/fonts/DejaVuSans.ttf"
#endif
#ifndef BD_ASSET_PIN_OUT
#define BD_ASSET_PIN_OUT     "src/birdie/assets/pushpin/pushpin-out-14.png"
#endif
#ifndef BD_ASSET_PIN_IN
#define BD_ASSET_PIN_IN      "src/birdie/assets/pushpin/pushpin-in-14.png"
#endif

/* fallback intrinsic size for a child with no preferred dimension */
#define DEFAULT_MIN_W   64
#define DEFAULT_MIN_H   16

/*
 * Chrome theme, set by bd_gui_init(). Initialized to the compile-time
 * defaults so widgets created before init (and the CHROME_* metric macros
 * below) still resolve. CHROME_FONT_SZ / CHROME_BASELINE alias the live
 * theme so existing call sites and derived macros read runtime values.
 */
static bd_theme theme = BD_THEME_DEFAULTS;
#define CHROME_FONT_SZ  (theme.font_size)
#define CHROME_BASELINE (theme.baseline)

/* ------------------------------------------------------------------ */
/* internal widget structure                                          */
/* ------------------------------------------------------------------ */

struct widget {
	int alive;
	int type;
	bd_id parent;
	bd_id first_child, last_child;
	bd_id next_sib, prev_sib;

	int pref_w, pref_h, grow;
	int layout, pad, gap;
	int user_x, user_y;

	int x, y, w, h;

	const char *label;
	uint32_t fg, bg;
	int visible, enabled;
	int role;
	const char *acc_name;

	bd_callback_fn on_click;
	void *on_click_data;
	bd_callback_fn on_close;
	void *on_close_data;

	int hover, pressed;

	int menu_open, menu_pinned;
	int popup_x, popup_y, popup_w, popup_h;

	char text_buf[1024];
	int text_len;
	int cursor;
	int sel_anchor;
	float scroll_x;
	float scroll_y;         /* BD_MULTILINE vertical scroll */

	int win_id;             /* backend window id for a top-level frame; 0 = none */

	void *state;            /* per-instance data for extension widget types */
};

/* ------------------------------------------------------------------ */
/* module state                                                       */
/* ------------------------------------------------------------------ */

#define MAX_WINDOWS 16

static struct widget pool[MAX_WIDGETS];
static int pool_next = 1;
static bd_id root = BD_NONE;
/* Top-level frames, in creation order; windows[0] == root is the primary. With
 * a multi_window backend each maps to a native window (pool[].win_id). */
static bd_id windows[MAX_WINDOWS];
static int   window_count;
static bd_id active_press = BD_NONE;
static bd_id pointer_capture = BD_NONE; /* extension widget grabbing a drag */
/* per-finger capture: each active touch id grabs the widget it landed on, so
 * several widgets (e.g. knobs) can be dragged at once */
#define MAX_TOUCHES 10
static struct { int active; int id; bd_id widget; } touches[MAX_TOUCHES];
static bd_id pen_capture = BD_NONE;     /* extension widget grabbing the stylus */
static bd_id active_menu = BD_NONE;
static bd_id drag_menu = BD_NONE;
static int drag_off_x, drag_off_y;
static int mouse_x, mouse_y;
static const bd_backend *be;    /* renderer/window backend, set by bd_gui_init */
static bd_texture pin_out_tex;
static bd_texture pin_in_tex;
static bd_id focus_id = BD_NONE;
static float cursor_blink;
/* BD_LIST double-click tracking */
static bd_id list_last_id = BD_NONE;
static int   list_last_row = -1;
static double list_last_time;
/* BD_NOTICE modal overlay (one at a time) */
static bd_id        active_notice = BD_NONE;
static bd_notice_cb notice_cb;
static void        *notice_arg;
/* IME: in-progress preedit shown at the focused field's caret, and the last
 * reported enabled state */
static char  preedit_buf[256];
static int   preedit_len;
static int   preedit_caret;
static bd_id preedit_owner = BD_NONE;
static int   ime_enabled;

/* Single-line editable text widgets share the same edit/render/focus paths.
 * BD_INPUT_LINE is the MUD command line (Enter submits and clears); BD_TEXT is
 * a plain form field (Enter commits but keeps the text). */
static int
is_text_field(int type)
{
	return type == BD_INPUT_LINE || type == BD_TEXT || type == BD_MULTILINE;
}

/* ------------------------------------------------------------------ */
/* extension widget-class registry                                    */
/* ------------------------------------------------------------------ */

#define BD_TYPE_CUSTOM_BASE 256
#define MAX_WIDGET_CLASSES  16

static const bd_widget_class *classes[MAX_WIDGET_CLASSES];
static int class_count;

static const bd_widget_class *
class_of(int type)
{
	int i = type - BD_TYPE_CUSTOM_BASE;
	if (i < 0 || i >= class_count)
		return NULL;
	return classes[i];
}

int
bd_register_widget_class(const bd_widget_class *cls)
{
	if (!cls || class_count >= MAX_WIDGET_CLASSES)
		return 0;
	classes[class_count] = cls;
	return BD_TYPE_CUSTOM_BASE + class_count++;
}

void *
bd_widget_state(bd_id id)
{
	if (id == BD_NONE || !pool[id].alive)
		return NULL;
	return pool[id].state;
}

int
bd_widget_type(bd_id id)
{
	if (id == BD_NONE || !pool[id].alive)
		return 0;
	return pool[id].type;
}

void
bd_widget_rect(bd_id id, int *x, int *y, int *w, int *h)
{
	struct widget *wi = &pool[id];
	if (x) *x = wi->x;
	if (y) *y = wi->y;
	if (w) *w = wi->w;
	if (h) *h = wi->h;
}

const bd_backend *
bd_backend_get(void)
{
	return be;
}

const bd_theme *
bd_gui_theme(void)
{
	return &theme;
}

/* ------------------------------------------------------------------ */
/* drawing helpers (toolkit renderer)                                 */
/* ------------------------------------------------------------------ */

static void
fill_rect(int x, int y, int w, int h, uint32_t color)
{
	if (color & 0xFF)
		bd_draw_rect((float)x, (float)y, (float)w, (float)h, color);
}

static void
stroke_rect(int x, int y, int w, int h, uint32_t color)
{
	bd_draw_rect_lines((float)x, (float)y, (float)w, (float)h, color);
}

static inline int
in_rect(int px, int py, int rx, int ry, int rw, int rh)
{
	return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

/* ------------------------------------------------------------------ */
/* proportional chrome text                                           */
/* ------------------------------------------------------------------ */

static float
chrome_text_w(const char *s)
{
	return s ? bd_draw_text_width(s) : 0.0f;
}

static float
chrome_baseline_y(int wy, int wh)
{
	return (float)wy + (float)wh * CHROME_BASELINE;
}

static float
queue_text(const char *text, float x, float y, uint32_t color)
{
	if (!text)
		return 0.0f;
	/* callers pass y as a baseline; bd_draw_text takes the line top */
	bd_draw_text(text, x, y - bd_draw_ascent(), color);
	return bd_draw_text_width(text);
}

static float
queue_sprite(bd_texture tex, float x, float y,
    float w, float h, float sw, float sh, uint32_t color)
{
	(void)sw;
	(void)sh;
	bd_draw_sprite(tex, x, y, w, h, 0.0f, 0.0f, 1.0f, 1.0f, color);
	return w;
}

/* ------------------------------------------------------------------ */
/* UTF-8 helpers                                                      */
/* ------------------------------------------------------------------ */

static int
utf8_encode(unsigned cp, char *out)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

static int
utf8_prev(const char *s, int pos)
{
	if (pos <= 0)
		return 0;
	pos--;
	while (pos > 0 && (s[pos] & 0xC0) == 0x80)
		pos--;
	return pos;
}

static int
utf8_next(const char *s, int pos, int len)
{
	if (pos >= len)
		return len;
	pos++;
	while (pos < len && (s[pos] & 0xC0) == 0x80)
		pos++;
	return pos;
}

static int
word_left(const char *s, int pos)
{
	while (pos > 0 && s[pos - 1] == ' ')
		pos--;
	while (pos > 0 && s[pos - 1] != ' ')
		pos--;
	return pos;
}

static int
word_right(const char *s, int pos, int len)
{
	while (pos < len && s[pos] != ' ')
		pos++;
	while (pos < len && s[pos] == ' ')
		pos++;
	return pos;
}

static float
input_text_px(const char *buf, int end)
{
	if (end <= 0)
		return 0.0f;
	char tmp[1024];
	if (end >= (int)sizeof(tmp))
		end = (int)sizeof(tmp) - 1;
	memcpy(tmp, buf, end);
	tmp[end] = '\0';
	return chrome_text_w(tmp);
}

/* ------------------------------------------------------------------ */
/* multi-line text helpers (BD_MULTILINE)                             */
/* ------------------------------------------------------------------ */

static int
ml_line_start(const char *s, int pos)
{
	while (pos > 0 && s[pos - 1] != '\n')
		pos--;
	return pos;
}

static int
ml_line_end(const char *s, int len, int pos)
{
	while (pos < len && s[pos] != '\n')
		pos++;
	return pos;
}

static int
ml_line_h(void)
{
	int h = (int)bd_draw_line_height();
	return h > 0 ? h : (int)CHROME_FONT_SZ + 2;
}

/* pixel width of buf[a..b) */
static float
ml_span_px(const char *buf, int a, int b)
{
	char tmp[1024];
	int n = b - a;
	if (n <= 0)
		return 0.0f;
	if (n >= (int)sizeof(tmp))
		n = (int)sizeof(tmp) - 1;
	memcpy(tmp, buf + a, n);
	tmp[n] = '\0';
	return chrome_text_w(tmp);
}

/* byte offset in [start,end] whose x from `start` is nearest `target` px */
static int
ml_col_at_px(const char *buf, int start, int end, float target)
{
	int pos = start;
	float w = 0.0f;
	while (pos < end) {
		int nx = utf8_next(buf, pos, end);
		float cw = ml_span_px(buf, pos, nx);
		if (w + cw * 0.5f >= target)
			return pos;
		w += cw;
		pos = nx;
	}
	return end;
}

/* count of '\n' in buf[0,pos) == the caret's line index */
static int
ml_line_index(const char *buf, int pos)
{
	int n = 0;
	for (int i = 0; i < pos; i++)
		if (buf[i] == '\n')
			n++;
	return n;
}

/* BD_LIST / BD_TAB_BAR: number of '\n'-separated items in text_buf */
static int
list_count(const struct widget *w)
{
	return w->text_len > 0 ? ml_line_index(w->text_buf, w->text_len) + 1 : 0;
}

/* BD_TAB_BAR folder-tab geometry */
#define TAB_SLANT 10    /* angled shoulder width, px */
#define TAB_HPAD  12    /* text padding inside the flat top, px */

/* copy tab `idx`'s label into buf (NUL-terminated); returns its width in px */
static int
tab_label(const struct widget *w, int idx, char *buf, int cap)
{
	int pos = 0;
	for (int i = 0; i < idx; i++) {
		pos = ml_line_end(w->text_buf, w->text_len, pos);
		if (pos < w->text_len) pos++;
	}
	int le = ml_line_end(w->text_buf, w->text_len, pos);
	int n = le - pos;
	if (n > cap - 1) n = cap - 1;
	if (n > 0) memcpy(buf, w->text_buf + pos, (size_t)n);
	buf[n < 0 ? 0 : n] = '\0';
	return (int)chrome_text_w(buf) + 2 * TAB_HPAD + 2 * TAB_SLANT;
}

/* ------------------------------------------------------------------ */
/* BD_SCROLLBAR geometry                                              */
/* ------------------------------------------------------------------ */

/* Orientation derives from the shape: a vertical bar is taller than wide. */
static void
scrollbar_metrics(const struct widget *w, int *vert, int *origin,
    int *track_len, int *thumb_len)
{
	int v = w->h >= w->w;
	int len = v ? w->h : w->w;
	float frac = w->scroll_x;
	if (frac <= 0.0f) frac = 0.3f;
	if (frac > 1.0f) frac = 1.0f;
	int tl = (int)(len * frac);
	if (tl < 12) tl = 12;
	if (tl > len) tl = len;
	*vert = v;
	*origin = v ? w->y : w->x;
	*track_len = len;
	*thumb_len = tl;
}

/* map a pointer to a thumb position [0,1] (thumb centered on the pointer) */
static void
scrollbar_from_pointer(struct widget *w, int mx, int my)
{
	int vert, origin, len, tl;
	scrollbar_metrics(w, &vert, &origin, &len, &tl);
	int travel = len - tl;
	float pos = 0.0f;
	if (travel > 0) {
		int p = (vert ? my : mx) - origin - tl / 2;
		pos = (float)p / (float)travel;
	}
	if (pos < 0.0f) pos = 0.0f;
	if (pos > 1.0f) pos = 1.0f;
	w->scroll_y = pos;
}

/* index of the tab under x (bar-relative bounding boxes), or -1 */
static int
tab_at(const struct widget *w, int mx)
{
	int n = list_count(w);
	int tx = w->x;
	char buf[256];
	for (int i = 0; i < n; i++) {
		int tw = tab_label(w, i, buf, sizeof buf);
		if (mx >= tx && mx < tx + tw)
			return i;
		tx += tw;
	}
	return -1;
}

/* ------------------------------------------------------------------ */
/* attribute application                                              */
/* ------------------------------------------------------------------ */

static void
apply_i(struct widget *w, int attr, int val)
{
	switch (attr) {
	case BD_WIDTH_I:  /* fall through */
	case BD_PREF_W_I: w->pref_w = val; break;
	case BD_HEIGHT_I: /* fall through */
	case BD_PREF_H_I: w->pref_h = val; break;
	case BD_GROW_I:   w->grow = val; break;
	case BD_X_I:      w->user_x = val; break;
	case BD_Y_I:      w->user_y = val; break;
	case BD_LAYOUT_I: w->layout = val; break;
	case BD_ROLE_I:   w->role = val; break;
	case BD_PAD_I:    w->pad = val; break;
	case BD_GAP_I:    w->gap = val; break;
	}
}

static void
apply_s(struct widget *w, int attr, const char *val)
{
	switch (attr) {
	case BD_LABEL_S:
		w->label = val;
		if (is_text_field(w->type) && val) {
			int n = (int)strlen(val);
			if (n >= (int)sizeof(w->text_buf))
				n = (int)sizeof(w->text_buf) - 1;
			memcpy(w->text_buf, val, n);
			w->text_buf[n] = '\0';
			w->text_len = n;
			w->cursor = n;
			w->sel_anchor = -1;
		} else if ((w->type == BD_LIST || w->type == BD_TAB_BAR) && val) {
			/* '\n'-separated items; cursor is the selected row/tab */
			int n = (int)strlen(val);
			if (n >= (int)sizeof(w->text_buf))
				n = (int)sizeof(w->text_buf) - 1;
			memcpy(w->text_buf, val, n);
			w->text_buf[n] = '\0';
			w->text_len = n;
		}
		break;
	case BD_NAME_S:  w->acc_name = val; break;
	}
}

static void
apply_p(struct widget *w, int attr, void *val)
{
	switch (attr) {
	case BD_ON_CLICK_P: w->on_click_data = val; break;
	case BD_ON_CLOSE_P: w->on_close_data = val; break;
	}
}

static void
apply_f(struct widget *w, int attr, bd_callback_fn val)
{
	switch (attr) {
	case BD_ON_CLICK_F: w->on_click = val; break;
	case BD_ON_CLOSE_F: w->on_close = val; break;
	}
}

static void
apply_c(struct widget *w, int attr, uint32_t val)
{
	switch (attr) {
	case BD_FG_C: w->fg = val; break;
	case BD_BG_C: w->bg = val; break;
	}
}

static void
apply_b(struct widget *w, int attr, int val)
{
	switch (attr) {
	case BD_VISIBLE_B:  w->visible = val; break;
	case BD_ENABLED_B:  w->enabled = val; break;
	case BD_MENU_PIN_B: w->menu_pinned = val; break;
	}
}

/* ------------------------------------------------------------------ */
/* varargs / array attribute parsing                                  */
/* ------------------------------------------------------------------ */

static void
parse_va(struct widget *w, va_list ap)
{
	for (;;) {
		int attr = va_arg(ap, int);
		if (attr == BD_END)
			break;
		switch (attr & TYPEMASK) {
		case TYPE_I: apply_i(w, attr, va_arg(ap, int)); break;
		case TYPE_S: apply_s(w, attr, va_arg(ap, const char *)); break;
		case TYPE_P: apply_p(w, attr, va_arg(ap, void *)); break;
		case TYPE_F: apply_f(w, attr, va_arg(ap, bd_callback_fn)); break;
		case TYPE_C: apply_c(w, attr, va_arg(ap, unsigned int)); break;
		case TYPE_B: apply_b(w, attr, va_arg(ap, int)); break;
		default:
			fprintf(stderr, "bd: unknown attr type %d\n",
			    attr & TYPEMASK);
			return;
		}
	}
}

static void
parse_arr(struct widget *w, const bd_attr *a)
{
	for (; a->id != BD_END; a++) {
		switch (a->id & TYPEMASK) {
		case TYPE_I: apply_i(w, a->id, a->i); break;
		case TYPE_S: apply_s(w, a->id, a->s); break;
		case TYPE_P: apply_p(w, a->id, a->p); break;
		case TYPE_F: apply_f(w, a->id, a->f); break;
		case TYPE_C: apply_c(w, a->id, a->c); break;
		case TYPE_B: apply_b(w, a->id, a->i); break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* tree operations                                                    */
/* ------------------------------------------------------------------ */

static void
link_child(bd_id pid, bd_id cid)
{
	struct widget *p = &pool[pid];
	struct widget *c = &pool[cid];

	c->parent = pid;
	c->prev_sib = p->last_child;
	c->next_sib = BD_NONE;
	if (p->last_child != BD_NONE)
		pool[p->last_child].next_sib = cid;
	else
		p->first_child = cid;
	p->last_child = cid;
}

static void
defaults(struct widget *w, int type)
{
	w->type = type;
	w->visible = 1;
	w->enabled = 1;
	w->fg = theme.text;

	switch (type) {
	case BD_FRAME:
		w->bg = theme.bg;
		w->layout = BD_LAYOUT_COL;
		break;
	case BD_PANEL:
		w->layout = BD_LAYOUT_COL;
		break;
	case BD_BUTTON:
		w->bg = theme.widget;
		w->fg = theme.text_hi;
		w->pref_h = (int)CHROME_FONT_SZ + 8;
		break;
	case BD_LABEL:
		w->pref_h = (int)CHROME_FONT_SZ + 4;
		break;
	case BD_MENU:
		w->pref_h = (int)CHROME_FONT_SZ + 4;
		break;
	case BD_TEXT:
	case BD_INPUT_LINE:
		w->bg = theme.press;
		w->fg = theme.text_hi;
		w->pref_h = (int)CHROME_FONT_SZ + 8;
		w->pad = 4;
		w->sel_anchor = -1;
		break;
	case BD_MULTILINE:
		w->bg = theme.press;
		w->fg = theme.text_hi;
		w->pref_h = (int)CHROME_FONT_SZ * 6 + 8;   /* ~6 lines */
		w->pad = 4;
		w->sel_anchor = -1;
		break;
	case BD_LIST:
		w->bg = theme.press;
		w->fg = theme.text;
		w->pref_h = (int)CHROME_FONT_SZ * 6 + 8;
		w->pad = 2;
		w->cursor = -1;       /* selected row index; -1 = none */
		break;
	case BD_TAB_BAR:
		w->bg = theme.bg;
		w->fg = theme.text;
		w->pref_h = (int)CHROME_FONT_SZ + 12;
		w->cursor = 0;        /* active tab index */
		break;
	case BD_SCROLLBAR:
		w->bg = theme.press;
		w->pref_w = 14;
		w->pref_h = 14;       /* app grows the long axis */
		w->scroll_y = 0.0f;   /* thumb position, [0,1] */
		w->scroll_x = 0.3f;   /* thumb size as a fraction of the track */
		break;
	}
}

/* ------------------------------------------------------------------ */
/* public: create / destroy                                           */
/* ------------------------------------------------------------------ */

/*
 * Allocate + default-init a widget and run any extension class init, before
 * the caller's attributes are applied (so app attributes override class
 * defaults). Returns BD_NONE if the pool is full.
 */
static bd_id
create_begin(bd_id parent, int type)
{
	if (pool_next >= MAX_WIDGETS)
		return BD_NONE;

	bd_id id = (bd_id)pool_next++;
	struct widget *w = &pool[id];

	memset(w, 0, sizeof *w);
	w->alive = 1;
	defaults(w, type);

	if (parent != BD_NONE)
		link_child(parent, id);
	else if (root == BD_NONE)
		root = id;

	const bd_widget_class *cls = class_of(type);
	if (cls) {
		if (cls->state_size)
			w->state = calloc(1, cls->state_size);
		if (cls->init)
			cls->init(id, w->state);
	}

	return id;
}

/*
 * Register a top-level frame as a window and, on a multi_window backend, give
 * it a native window. The first frame adopts the primary window (id 1) the
 * host already opened; later frames open their own. Runs after attributes are
 * applied, so the title and preferred size are known.
 */
static void
register_window(bd_id id)
{
	struct widget *w = &pool[id];
	if (w->type != BD_FRAME || w->parent != BD_NONE)
		return;
	if (window_count < MAX_WINDOWS)
		windows[window_count] = id;
	window_count++;

	if (!be || !be->multi_window)
		return;
	if (window_count == 1) {
		w->win_id = 1;                  /* adopt the host's primary window */
		if (be->window_set_title && w->label)
			be->window_set_title(1, w->label);
	} else if (be->window_open) {
		int ww = w->pref_w > 0 ? w->pref_w : 480;
		int wh = w->pref_h > 0 ? w->pref_h : 360;
		w->win_id = be->window_open(w->label ? w->label : "", ww, wh);
	}
}

static void
create_finish(bd_id id)
{
	struct widget *w = &pool[id];
	if (w->type == BD_MENU && w->pref_w == 0 && w->label)
		w->pref_w = (int)(chrome_text_w(w->label) + CHROME_FONT_SZ);
	register_window(id);
}

/* The top-level frame an id belongs to (itself if already a root). */
static bd_id
top_frame_of(bd_id id)
{
	while (id != BD_NONE && pool[id].parent != BD_NONE)
		id = pool[id].parent;
	return id;
}

/* The frame that owns backend window `win`, or root if none matches (single
 * window / window 0). */
static bd_id
frame_for_window(int win)
{
	if (win != 0)
		for (int i = 0; i < window_count; i++) {
			bd_id f = windows[i];
			if (pool[f].alive && pool[f].win_id == win)
				return f;
		}
	return root;
}

bd_id
bd_frame_for_window(int window_id)
{
	for (int i = 0; i < window_count; i++) {
		bd_id f = windows[i];
		if (pool[f].alive && pool[f].win_id == window_id)
			return f;
	}
	return BD_NONE;
}

bd_id
bd_create(bd_id parent, int type, ...)
{
	bd_id id = create_begin(parent, type);
	if (id == BD_NONE)
		return BD_NONE;

	va_list ap;
	va_start(ap, type);
	parse_va(&pool[id], ap);
	va_end(ap);

	create_finish(id);
	return id;
}

bd_id
bd_create_va(bd_id parent, int type, va_list ap)
{
	bd_id id = create_begin(parent, type);
	if (id == BD_NONE)
		return BD_NONE;

	parse_va(&pool[id], ap);
	create_finish(id);
	return id;
}

bd_id
bd_create_v(bd_id parent, int type, const bd_attr *attrs)
{
	bd_id id = create_begin(parent, type);
	if (id == BD_NONE)
		return BD_NONE;

	if (attrs)
		parse_arr(&pool[id], attrs);
	create_finish(id);
	return id;
}

void
bd_destroy(bd_id id)
{
	if (id == BD_NONE || !pool[id].alive)
		return;

	while (pool[id].first_child != BD_NONE)
		bd_destroy(pool[id].first_child);

	struct widget *w = &pool[id];
	if (w->parent != BD_NONE) {
		struct widget *p = &pool[w->parent];
		if (w->prev_sib != BD_NONE)
			pool[w->prev_sib].next_sib = w->next_sib;
		else
			p->first_child = w->next_sib;
		if (w->next_sib != BD_NONE)
			pool[w->next_sib].prev_sib = w->prev_sib;
		else
			p->last_child = w->prev_sib;
	}
	const bd_widget_class *cls = class_of(w->type);
	if (cls && cls->destroy)
		cls->destroy(id, w->state);
	free(w->state);
	w->state = NULL;

	/* drop a top-level frame from the window list and close its native
	 * window (but never close the host-owned primary, id 1) */
	for (int i = 0; i < window_count; i++) {
		if (windows[i] != id)
			continue;
		if (be && be->multi_window && be->window_close &&
		    w->win_id > 1)
			be->window_close(w->win_id);
		for (int j = i; j < window_count - 1; j++)
			windows[j] = windows[j + 1];
		window_count--;
		break;
	}

	if (root == id)
		root = BD_NONE;
	if (active_press == id)
		active_press = BD_NONE;
	if (pointer_capture == id)
		pointer_capture = BD_NONE;
	w->alive = 0;
}

/* ------------------------------------------------------------------ */
/* public: set / get                                                  */
/* ------------------------------------------------------------------ */

void
bd_set(bd_id id, ...)
{
	if (id == BD_NONE || !pool[id].alive)
		return;
	va_list ap;
	va_start(ap, id);
	parse_va(&pool[id], ap);
	va_end(ap);
}

void
bd_set_v(bd_id id, const bd_attr *attrs)
{
	if (id == BD_NONE || !pool[id].alive)
		return;
	parse_arr(&pool[id], attrs);
}

int
bd_get_i(bd_id id, int attr)
{
	if (id == BD_NONE || !pool[id].alive)
		return 0;
	struct widget *w = &pool[id];
	switch (attr) {
	case BD_WIDTH_I:  case BD_PREF_W_I: return w->pref_w;
	case BD_HEIGHT_I: case BD_PREF_H_I: return w->pref_h;
	case BD_GROW_I:   return w->grow;
	case BD_X_I:      return w->user_x;
	case BD_Y_I:      return w->user_y;
	case BD_LAYOUT_I: return w->layout;
	case BD_ROLE_I:   return w->role;
	case BD_PAD_I:    return w->pad;
	case BD_GAP_I:    return w->gap;
	case BD_VISIBLE_B:  return w->visible;
	case BD_ENABLED_B:  return w->enabled;
	case BD_MENU_PIN_B: return w->menu_pinned;
	}
	return 0;
}

const char *
bd_get_s(bd_id id, int attr)
{
	if (id == BD_NONE || !pool[id].alive)
		return NULL;
	switch (attr) {
	case BD_LABEL_S:
		if (is_text_field(pool[id].type))
			return pool[id].text_buf;
		return pool[id].label;
	case BD_NAME_S:  return pool[id].acc_name;
	}
	return NULL;
}

bd_id
bd_parent(bd_id id)
{
	return (id != BD_NONE && pool[id].alive) ? pool[id].parent : BD_NONE;
}

bd_id
bd_first_child(bd_id id)
{
	return (id != BD_NONE && pool[id].alive) ? pool[id].first_child : BD_NONE;
}

bd_id
bd_next_sibling(bd_id id)
{
	return (id != BD_NONE && pool[id].alive) ? pool[id].next_sib : BD_NONE;
}

/* ------------------------------------------------------------------ */
/* layout engine                                                      */
/* ------------------------------------------------------------------ */

static void
layout_children(bd_id id)
{
	struct widget *w = &pool[id];
	if (w->first_child == BD_NONE)
		return;

	int ax = w->x + w->pad;
	int ay = w->y + w->pad;
	int aw = w->w - 2 * w->pad;
	int ah = w->h - 2 * w->pad;
	if (aw < 0) aw = 0;
	if (ah < 0) ah = 0;

	int mode = w->layout ? w->layout : BD_LAYOUT_COL;

	if (mode == BD_LAYOUT_FIXED) {
		bd_id c;
		for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib) {
			struct widget *ch = &pool[c];
			ch->x = ax + ch->user_x;
			ch->y = ay + ch->user_y;
			ch->w = ch->pref_w > 0 ? ch->pref_w : aw;
			ch->h = ch->pref_h > 0 ? ch->pref_h : ah;
			if (ch->type != BD_MENU)
				layout_children(c);
		}
		return;
	}

	/* flexbox: row or col */
	int is_row = (mode == BD_LAYOUT_ROW);
	int total = is_row ? aw : ah;
	int cross = is_row ? ah : aw;
	int sum_pref = 0, sum_grow = 0, n = 0;
	bd_id c;

	for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib) {
		int pref = is_row ? pool[c].pref_w : pool[c].pref_h;
		if (pref <= 0)
			pref = is_row ? DEFAULT_MIN_W : DEFAULT_MIN_H;
		sum_pref += pref;
		sum_grow += pool[c].grow;
		n++;
	}

	int gaps = n > 1 ? (n - 1) * w->gap : 0;
	int remaining = total - sum_pref - gaps;
	if (remaining < 0) remaining = 0;

	int pos = is_row ? ax : ay;
	for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib) {
		struct widget *ch = &pool[c];
		int pref = is_row ? ch->pref_w : ch->pref_h;
		if (pref <= 0)
			pref = is_row ? DEFAULT_MIN_W : DEFAULT_MIN_H;

		int extent = pref;
		if (sum_grow > 0 && ch->grow > 0)
			extent += remaining * ch->grow / sum_grow;

		if (is_row) {
			ch->x = pos; ch->y = ay;
			ch->w = extent; ch->h = cross;
		} else {
			ch->x = ax; ch->y = pos;
			ch->w = cross; ch->h = extent;
		}

		pos += extent + w->gap;
		if (ch->type != BD_MENU)
			layout_children(c);
	}
}

/* ------------------------------------------------------------------ */
/* rendering                                                          */
/* ------------------------------------------------------------------ */

/*
 * Draw one folder tab. The trapezoid (angled shoulders) sits on the content
 * line; the active tab is raised, drawn in the content face, and (because it
 * is drawn after the baseline) merges with the panel below. A light band along
 * the flat top gives the raised, dimensional look.
 */
static void
draw_tab(const struct widget *w, int tx, int tw, const char *label,
    int active, int focused)
{
	int top = w->y + (active ? 2 : 5);
	int bot = w->y + w->h;
	uint32_t face = active ? theme.widget : theme.press;

	bd_draw_quad((float)tx, (float)bot,
	    (float)(tx + TAB_SLANT), (float)top,
	    (float)(tx + tw - TAB_SLANT), (float)top,
	    (float)(tx + tw), (float)bot, face);
	/* highlight band across the flat top */
	uint32_t hi = (active && focused) ? theme.focus : theme.hover;
	fill_rect(tx + TAB_SLANT, top, tw - 2 * TAB_SLANT, 2, hi);

	float twpx = chrome_text_w(label);
	float lx = (float)tx + ((float)tw - twpx) * 0.5f;
	float by = chrome_baseline_y(top, bot - top);
	queue_text(label, lx, by, active ? theme.text_hi : theme.text);
}

static void
render_widget(bd_id id)
{
	struct widget *w = &pool[id];
	if (!w->alive || !w->visible)
		return;

	switch (w->type) {
	case BD_FRAME:
		fill_rect(w->x, w->y, w->w, w->h, w->bg);
		break;

	case BD_PANEL:
		if (w->bg & 0xFF)
			fill_rect(w->x, w->y, w->w, w->h, w->bg);
		break;

	case BD_LABEL:
		if (w->bg & 0xFF)
			fill_rect(w->x, w->y, w->w, w->h, w->bg);
		if (w->label) {
			float tx = (float)(w->x + w->pad);
			float ty = chrome_baseline_y(w->y, w->h);
			queue_text(w->label, tx, ty, w->fg);
		}
		break;

	case BD_BUTTON: {
		uint32_t bg = w->bg;
		if (w->pressed && w->hover)
			bg = theme.press;
		else if (w->hover)
			bg = theme.hover;
		fill_rect(w->x, w->y, w->w, w->h, bg);
		stroke_rect(w->x, w->y, w->w, w->h,
		    (focus_id == id) ? theme.focus : theme.border);
		if (w->label) {
			float tw = chrome_text_w(w->label);
			float tx = (float)w->x + ((float)w->w - tw) * 0.5f;
			float ty = chrome_baseline_y(w->y, w->h);
			queue_text(w->label, tx, ty, w->fg);
		}
		break;
	}

	case BD_MENU: {
		uint32_t bg = 0;
		if (w->menu_open || w->hover)
			bg = theme.hover;
		if (bg)
			fill_rect(w->x, w->y, w->w, w->h, bg);
		if (w->label) {
			float tx = (float)(w->x + w->pad) + CHROME_FONT_SZ * 0.25f;
			float ty = chrome_baseline_y(w->y, w->h);
			queue_text(w->label, tx, ty, w->fg);
		}
		break;
	}

	case BD_TEXT:
	case BD_INPUT_LINE: {
		int focused = (focus_id == id);
		uint32_t border = focused ? theme.focus : theme.border;
		fill_rect(w->x, w->y, w->w, w->h, w->bg);
		stroke_rect(w->x, w->y, w->w, w->h, border);

		float vis_x = (float)(w->x + w->pad);
		float vis_w = (float)(w->w - 2 * w->pad);

		/* keep cursor in view */
		float cursor_px = input_text_px(w->text_buf, w->cursor);
		if (cursor_px - w->scroll_x > vis_w)
			w->scroll_x = cursor_px - vis_w;
		if (cursor_px - w->scroll_x < 0.0f)
			w->scroll_x = cursor_px;

		float base_y = chrome_baseline_y(w->y, w->h);

		/* selection highlight */
		if (focused && w->sel_anchor >= 0 &&
		    w->sel_anchor != w->cursor) {
			int s0 = w->sel_anchor < w->cursor
			    ? w->sel_anchor : w->cursor;
			int s1 = w->sel_anchor < w->cursor
			    ? w->cursor : w->sel_anchor;
			float sx0 = input_text_px(w->text_buf, s0) - w->scroll_x;
			float sx1 = input_text_px(w->text_buf, s1) - w->scroll_x;
			fill_rect((int)(vis_x + sx0), w->y + 2,
			    (int)(sx1 - sx0), w->h - 4, theme.select);
		}

		/* text */
		if (w->text_len > 0)
			queue_text(w->text_buf,
			    vis_x - w->scroll_x, base_y, w->fg);

		/* IME preedit: composing text shown inline at the caret, underlined */
		float caret_x = vis_x + cursor_px - w->scroll_x;
		if (preedit_owner == id && preedit_len > 0) {
			queue_text(preedit_buf, caret_x, base_y, w->fg);
			float pw = chrome_text_w(preedit_buf);
			fill_rect((int)caret_x, w->y + w->h - 3, (int)pw, 1, w->fg);
			caret_x += input_text_px(preedit_buf, preedit_caret);
		}

		/* blinking cursor */
		if (focused) {
			double t = be->time() - (double)cursor_blink;
			if (((int)(t * 2.0)) % 2 == 0)
				fill_rect((int)caret_x, w->y + 2, 2, w->h - 4, w->fg);
		}
		break;
	}

	case BD_MULTILINE: {
		int focused = (focus_id == id);
		uint32_t border = focused ? theme.focus : theme.border;
		fill_rect(w->x, w->y, w->w, w->h, w->bg);
		stroke_rect(w->x, w->y, w->w, w->h, border);

		int pad = w->pad;
		int ix = w->x + pad, iy = w->y + pad;
		int iw = w->w - 2 * pad, ih = w->h - 2 * pad;
		int lh = ml_line_h();

		/* caret position, then keep it in view */
		int cls = ml_line_start(w->text_buf, w->cursor);
		int caret_line = ml_line_index(w->text_buf, w->cursor);
		float caret_px = ml_span_px(w->text_buf, cls, w->cursor);
		int caret_top = caret_line * lh;
		if (caret_top - (int)w->scroll_y < 0)
			w->scroll_y = (float)caret_top;
		if (caret_top + lh - (int)w->scroll_y > ih)
			w->scroll_y = (float)(caret_top + lh - ih);
		if (w->scroll_y < 0.0f)
			w->scroll_y = 0.0f;
		if (caret_px - w->scroll_x > (float)iw)
			w->scroll_x = caret_px - (float)iw;
		if (caret_px - w->scroll_x < 0.0f)
			w->scroll_x = caret_px;
		if (w->scroll_x < 0.0f)
			w->scroll_x = 0.0f;

		/* clip the scrolling text to the interior */
		bd_draw_flush();
		be->scissor(ix, iy, iw, ih);

		char tmp[1024];
		int li = 0, pos = 0;
		for (;;) {
			int le = ml_line_end(w->text_buf, w->text_len, pos);
			int line_top = iy + li * lh - (int)w->scroll_y;
			if (line_top + lh >= iy && line_top <= iy + ih) {
				int n = le - pos;
				if (n >= (int)sizeof(tmp))
					n = (int)sizeof(tmp) - 1;
				if (n > 0) {
					memcpy(tmp, w->text_buf + pos, n);
					tmp[n] = '\0';
					bd_draw_text(tmp, (float)ix - w->scroll_x,
					    (float)line_top, w->fg);
				}
			}
			if (le >= w->text_len)
				break;
			pos = le + 1;
			li++;
		}

		if (focused) {
			double t = be->time() - (double)cursor_blink;
			if (((int)(t * 2.0)) % 2 == 0) {
				int cx = ix + (int)(caret_px - w->scroll_x);
				int cy = iy + caret_top - (int)w->scroll_y;
				fill_rect(cx, cy, 2, lh, w->fg);
			}
		}

		bd_draw_flush();
		be->scissor_off();
		break;
	}

	case BD_LIST: {
		int focused = (focus_id == id);
		fill_rect(w->x, w->y, w->w, w->h, w->bg);
		stroke_rect(w->x, w->y, w->w, w->h,
		    focused ? theme.focus : theme.border);

		int pad = w->pad;
		int ix = w->x + pad, iy = w->y + pad;
		int iw = w->w - 2 * pad, ih = w->h - 2 * pad;
		int lh = ml_line_h();
		int count = list_count(w);

		/* clamp scroll to content */
		int max_scroll = count * lh - ih;
		if (max_scroll < 0) max_scroll = 0;
		if (w->scroll_y > (float)max_scroll) w->scroll_y = (float)max_scroll;
		if (w->scroll_y < 0.0f) w->scroll_y = 0.0f;

		bd_draw_flush();
		be->scissor(ix, iy, iw, ih);

		int li = 0, pos = 0;
		while (li < count) {
			int le = ml_line_end(w->text_buf, w->text_len, pos);
			int top = iy + li * lh - (int)w->scroll_y;
			if (top + lh >= iy && top <= iy + ih) {
				if (li == w->cursor) {
					fill_rect(ix, top, iw, lh,
					    focused ? theme.select : theme.hover);
				}
				char tmp[1024];
				int n = le - pos;
				if (n >= (int)sizeof(tmp)) n = (int)sizeof(tmp) - 1;
				if (n > 0) {
					memcpy(tmp, w->text_buf + pos, n);
					tmp[n] = '\0';
					uint32_t fg = (li == w->cursor) ? theme.text_hi
					    : w->fg;
					bd_draw_text(tmp, (float)(ix + 2),
					    (float)top, fg);
				}
			}
			if (le >= w->text_len) break;
			pos = le + 1;
			li++;
		}

		bd_draw_flush();
		be->scissor_off();
		break;
	}

	case BD_TAB_BAR: {
		int focused = (focus_id == id);
		int n = list_count(w);
		fill_rect(w->x, w->y, w->w, w->h, theme.bg);

		bd_draw_flush();
		be->scissor(w->x, w->y, w->w, w->h);

		/* inactive tabs first, then the content line, then the active tab
		 * on top so it merges with the panel below */
		int tx = w->x;
		int act_tx = 0, act_tw = 0, have_act = 0;
		char act_lbl[256];
		char lbl[256];
		for (int i = 0; i < n; i++) {
			int tw = tab_label(w, i, lbl, sizeof lbl);
			if (i == w->cursor) {
				act_tx = tx; act_tw = tw; have_act = 1;
				memcpy(act_lbl, lbl, sizeof lbl);
			} else {
				draw_tab(w, tx, tw, lbl, 0, focused);
			}
			tx += tw;
		}
		fill_rect(w->x, w->y + w->h - 2, w->w, 2, theme.border);
		if (have_act)
			draw_tab(w, act_tx, act_tw, act_lbl, 1, focused);

		bd_draw_flush();
		be->scissor_off();
		break;
	}

	case BD_SCROLLBAR: {
		int vert, origin, len, tl;
		scrollbar_metrics(w, &vert, &origin, &len, &tl);
		fill_rect(w->x, w->y, w->w, w->h, w->bg);   /* track */

		int off = origin + (int)(w->scroll_y * (float)(len - tl));
		uint32_t face = (w->pressed || w->hover) ? theme.hover : theme.widget;
		int tx, ty, tw_, th_;
		if (vert) {
			tx = w->x + 1; ty = off; tw_ = w->w - 2; th_ = tl;
		} else {
			tx = off; ty = w->y + 1; tw_ = tl; th_ = w->h - 2;
		}
		fill_rect(tx, ty, tw_, th_, face);
		stroke_rect(tx, ty, tw_, th_, theme.border);
		/* skeuomorphic grip: three short lines across the thumb's middle */
		for (int i = -1; i <= 1; i++) {
			if (vert)
				fill_rect(tx + 3, ty + th_ / 2 + i * 3,
				    tw_ - 6, 1, theme.border);
			else
				fill_rect(tx + tw_ / 2 + i * 3, ty + 3,
				    1, th_ - 6, theme.border);
		}
		break;
	}

	default: {
		const bd_widget_class *cls = class_of(w->type);
		if (cls && cls->render)
			cls->render(id, w->state);
		else if (w->bg & 0xFF)
			fill_rect(w->x, w->y, w->w, w->h, w->bg);
		break;
	}
	}

	const bd_widget_class *cls = class_of(w->type);
	int leaf = (w->type == BD_MENU || is_text_field(w->type) ||
	    w->type == BD_LIST || w->type == BD_TAB_BAR ||
	    w->type == BD_SCROLLBAR ||
	    (cls && !cls->contains_children));
	if (!leaf) {
		bd_id c;
		for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib)
			render_widget(c);
	}
}

/* ------------------------------------------------------------------ */
/* hit testing                                                        */
/* ------------------------------------------------------------------ */

static void
update_hover(bd_id id, int mx, int my)
{
	struct widget *w = &pool[id];
	if (!w->alive || !w->visible)
		return;
	w->hover = in_rect(mx, my, w->x, w->y, w->w, w->h);
	if (w->type == BD_MENU && !w->menu_open)
		return;
	bd_id c;
	for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib)
		update_hover(c, mx, my);
}

static bd_id
hit_interactive(bd_id id, int mx, int my)
{
	struct widget *w = &pool[id];
	if (!w->alive || !w->visible)
		return BD_NONE;
	if (!in_rect(mx, my, w->x, w->y, w->w, w->h))
		return BD_NONE;

	bd_id found = BD_NONE;
	bd_id c;
	for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib) {
		bd_id r = hit_interactive(c, mx, my);
		if (r != BD_NONE)
			found = r;
	}
	if (found != BD_NONE)
		return found;
	if ((w->on_click || is_text_field(w->type) || w->type == BD_LIST ||
	    w->type == BD_TAB_BAR || w->type == BD_SCROLLBAR) && w->enabled)
		return id;
	return BD_NONE;
}

/*
 * Topmost extension widget (a registered class with an event hook) under the
 * point, children first. Lets value widgets such as sliders, knobs, and X-Y
 * pads take pointer input the way buttons take clicks.
 */
static bd_id
hit_extension(bd_id id, int mx, int my)
{
	struct widget *w = &pool[id];
	if (!w->alive || !w->visible)
		return BD_NONE;
	if (!in_rect(mx, my, w->x, w->y, w->w, w->h))
		return BD_NONE;
	for (bd_id c = w->first_child; c != BD_NONE; c = pool[c].next_sib) {
		bd_id r = hit_extension(c, mx, my);
		if (r != BD_NONE)
			return r;
	}
	const bd_widget_class *cls = class_of(w->type);
	if (cls && cls->event && w->enabled)
		return id;
	return BD_NONE;
}

static int
ext_event(bd_id id, const bd_event *ev)
{
	const bd_widget_class *cls = class_of(pool[id].type);
	return (cls && cls->event) ? cls->event(id, pool[id].state, ev) : 0;
}

/* ------------------------------------------------------------------ */
/* popup menus                                                        */
/* ------------------------------------------------------------------ */

#define POPUP_PAD     4
#define PIN_ROW_H     ((int)CHROME_FONT_SZ + 6)
#define MENU_ITEM_H   ((int)CHROME_FONT_SZ + 6)
#define POPUP_MIN_W   100
#define PIN_CLICK_W   (PIN_OUT_W + 2 * POPUP_PAD)
#define PIN_OUT_W     29
#define PIN_OUT_H     14
#define PIN_IN_W      15
#define PIN_IN_H      15

static void
menu_open_at(bd_id id)
{
	pool[id].menu_open = 1;
	if (!pool[id].menu_pinned)
		active_menu = id;
}

static void
menu_close(bd_id id)
{
	struct widget *w = &pool[id];
	w->menu_open = 0;
	w->menu_pinned = 0;
	if (active_menu == id)
		active_menu = BD_NONE;
	bd_id c;
	for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib)
		pool[c].hover = 0;
}

static void
position_popup_items(struct widget *w)
{
	int iy = w->popup_y + PIN_ROW_H + 1;
	bd_id c;
	for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib) {
		struct widget *ch = &pool[c];
		ch->x = w->popup_x;
		ch->y = iy;
		ch->w = w->popup_w;
		ch->h = MENU_ITEM_H;
		iy += MENU_ITEM_H;
	}
}

static void
layout_popup(bd_id id)
{
	struct widget *w = &pool[id];
	if (!w->menu_open)
		return;

	int n = 0;
	float max_label = 0;
	bd_id c;
	for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib) {
		float lw = chrome_text_w(pool[c].label);
		if (lw > max_label)
			max_label = lw;
		n++;
	}

	int pw = (int)(max_label + 0.5f) + 2 * POPUP_PAD + (int)CHROME_FONT_SZ;
	if (pw < POPUP_MIN_W)
		pw = POPUP_MIN_W;
	int ph = PIN_ROW_H + 1 + n * MENU_ITEM_H + POPUP_PAD;

	w->popup_w = pw;
	w->popup_h = ph;

	if (!w->menu_pinned) {
		int px = w->x;
		int py = w->y + w->h;

		int ww = be->width(), wh = be->height();
		if (px + pw > ww) px = ww - pw;
		if (px < 0) px = 0;
		if (py + ph > wh) py = w->y - ph;
		if (py < 0) py = 0;

		w->popup_x = px;
		w->popup_y = py;
	}

	position_popup_items(w);
}

static void
render_popup(bd_id id)
{
	struct widget *w = &pool[id];
	if (!w->menu_open)
		return;

	int px = w->popup_x, py = w->popup_y;
	int pw = w->popup_w, ph = w->popup_h;

	fill_rect(px, py, pw, ph, theme.panel);
	stroke_rect(px, py, pw, ph, theme.border);

	/* pushpin row — pen advances through inline sprite + text */
	int pin_hover = in_rect(mouse_x, mouse_y,
	    px, py, pw, PIN_ROW_H);
	if (pin_hover)
		fill_rect(px + 1, py + 1, pw - 2, PIN_ROW_H - 1, theme.hover);
	{
		bd_texture pt = w->menu_pinned ? pin_in_tex : pin_out_tex;
		float ptw = w->menu_pinned ? PIN_IN_W : PIN_OUT_W;
		float pth = w->menu_pinned ? PIN_IN_H : PIN_OUT_H;
		float pen_x = (float)(px + POPUP_PAD);
		float base_y = chrome_baseline_y(py, PIN_ROW_H);
		float sprite_y = (float)py + ((float)PIN_ROW_H - pth) * 0.5f;

		pen_x += queue_sprite(pt, pen_x, sprite_y,
		    ptw, pth, ptw, pth, theme.text_hi);
		pen_x += POPUP_PAD;
		queue_text(w->label, pen_x, base_y, theme.text);
	}

	/* separator */
	fill_rect(px + 1, py + PIN_ROW_H, pw - 2, 1, theme.border);

	/* menu items */
	bd_id c;
	for (c = w->first_child; c != BD_NONE; c = pool[c].next_sib) {
		struct widget *ch = &pool[c];
		if (ch->hover)
			fill_rect(ch->x, ch->y, ch->w, ch->h, theme.hover);
		if (ch->label) {
			float tx = (float)(ch->x + POPUP_PAD);
			float ty = chrome_baseline_y(ch->y, ch->h);
			uint32_t fg = ch->enabled ? theme.text_hi : theme.text;
			queue_text(ch->label, tx, ty, fg);
		}
	}
}

static void
render_popups(bd_id frame)
{
	bd_id i;
	for (i = 1; i < (bd_id)pool_next; i++) {
		if (pool[i].alive && pool[i].type == BD_MENU && pool[i].menu_open
		    && top_frame_of(i) == frame)
			render_popup(i);
	}
}

static bd_id
find_menu_sibling(bd_id menu_id, int mx, int my)
{
	struct widget *m = &pool[menu_id];
	if (m->parent == BD_NONE)
		return BD_NONE;
	bd_id c;
	for (c = pool[m->parent].first_child; c != BD_NONE;
	    c = pool[c].next_sib) {
		if (c != menu_id && pool[c].type == BD_MENU &&
		    in_rect(mx, my, pool[c].x, pool[c].y,
		        pool[c].w, pool[c].h))
			return c;
	}
	return BD_NONE;
}

static void
pin_menu(bd_id id)
{
	pool[id].menu_pinned = 1;
	if (active_menu == id)
		active_menu = BD_NONE;
}

static int
handle_menu_event(const bd_event *ev, bd_id frame)
{
	switch (ev->type) {
	case BD_EV_MOUSE_MOVE:
		mouse_x = ev->x;
		mouse_y = ev->y;
		if (drag_menu != BD_NONE) {
			struct widget *dm = &pool[drag_menu];
			dm->popup_x = mouse_x - drag_off_x;
			dm->popup_y = mouse_y - drag_off_y;
			position_popup_items(dm);
			return 1;
		}
		if (active_menu != BD_NONE) {
			bd_id sib = find_menu_sibling(active_menu,
			    mouse_x, mouse_y);
			if (sib != BD_NONE) {
				menu_close(active_menu);
				menu_open_at(sib);
				layout_popup(sib);
			}
		}
		return 0;

	case BD_EV_MOUSE_UP:
		if (drag_menu != BD_NONE) {
			drag_menu = BD_NONE;
			return 1;
		}
		return 0;

	case BD_EV_MOUSE_DOWN: {
		if (ev->button != BD_MOUSE_LEFT)
			return 0;
		int mx = ev->x;
		int my = ev->y;
		bd_id i;

		for (i = 1; i < (bd_id)pool_next; i++) {
			struct widget *m = &pool[i];
			if (!m->alive || m->type != BD_MENU || !m->menu_open)
				continue;
			if (top_frame_of(i) != frame)
				continue;
			if (!in_rect(mx, my, m->popup_x, m->popup_y,
			    m->popup_w, m->popup_h))
				continue;

			/* title bar */
			if (my < m->popup_y + PIN_ROW_H) {
				if (mx < m->popup_x + PIN_CLICK_W) {
					if (m->menu_pinned)
						menu_close(i);
					else
						pin_menu(i);
				} else {
					if (!m->menu_pinned)
						pin_menu(i);
					drag_menu = i;
					drag_off_x = mx - m->popup_x;
					drag_off_y = my - m->popup_y;
				}
				return 1;
			}

			/* menu item */
			bd_id c;
			for (c = m->first_child; c != BD_NONE;
			    c = pool[c].next_sib) {
				struct widget *ch = &pool[c];
				if (in_rect(mx, my, ch->x, ch->y,
				    ch->w, ch->h)) {
					if (ch->on_click && ch->enabled)
						ch->on_click(c, ch->on_click_data);
					if (!m->menu_pinned)
						menu_close(i);
					return 1;
				}
			}
			return 1;
		}

		/* triggers */
		for (i = 1; i < (bd_id)pool_next; i++) {
			struct widget *m = &pool[i];
			if (!m->alive || m->type != BD_MENU)
				continue;
			if (top_frame_of(i) != frame)
				continue;
			if (!in_rect(mx, my, m->x, m->y, m->w, m->h))
				continue;
			if (m->menu_open) {
				if (!m->menu_pinned)
					menu_close(i);
			} else {
				if (active_menu != BD_NONE)
					menu_close(active_menu);
				menu_open_at(i);
				layout_popup(i);
			}
			return 1;
		}

		/* click outside — close active unpinned menu */
		if (active_menu != BD_NONE) {
			menu_close(active_menu);
			return 1;
		}
		return 0;
	}

	default:
		return 0;
	}
}

/* ------------------------------------------------------------------ */
/* input line editing                                                 */
/* ------------------------------------------------------------------ */

static void
input_delete_selection(struct widget *w)
{
	int s0 = w->sel_anchor < w->cursor ? w->sel_anchor : w->cursor;
	int s1 = w->sel_anchor < w->cursor ? w->cursor : w->sel_anchor;
	memmove(w->text_buf + s0, w->text_buf + s1, w->text_len - s1);
	w->text_len -= (s1 - s0);
	w->text_buf[w->text_len] = '\0';
	w->cursor = s0;
	w->sel_anchor = -1;
}

static void
input_insert_char(bd_id id, unsigned codepoint)
{
	struct widget *w = &pool[id];
	char enc[4];
	int n = utf8_encode(codepoint, enc);

	if (w->sel_anchor >= 0 && w->sel_anchor != w->cursor)
		input_delete_selection(w);

	if (w->text_len + n >= (int)sizeof(w->text_buf))
		return;

	memmove(w->text_buf + w->cursor + n,
	    w->text_buf + w->cursor,
	    w->text_len - w->cursor);
	memcpy(w->text_buf + w->cursor, enc, n);
	w->text_len += n;
	w->cursor += n;
	w->text_buf[w->text_len] = '\0';
	w->sel_anchor = -1;
	cursor_blink = (float)be->time();
}

/* copy the current selection (if any) to the system clipboard */
static void
clipboard_copy_selection(struct widget *w)
{
	if (!be->clipboard_set || w->sel_anchor < 0 || w->sel_anchor == w->cursor)
		return;
	int s0 = w->sel_anchor < w->cursor ? w->sel_anchor : w->cursor;
	int s1 = w->sel_anchor < w->cursor ? w->cursor : w->sel_anchor;
	char tmp[1024];
	int n = s1 - s0;
	if (n >= (int)sizeof(tmp)) n = (int)sizeof(tmp) - 1;
	memcpy(tmp, w->text_buf + s0, (size_t)n);
	tmp[n] = '\0';
	be->clipboard_set(tmp);
}

/* insert a UTF-8 string at the cursor, replacing any selection; drops newlines
 * for a single-line field */
static void
input_insert_text(struct widget *w, const char *s, int single_line)
{
	if (!s)
		return;
	if (w->sel_anchor >= 0 && w->sel_anchor != w->cursor)
		input_delete_selection(w);
	for (; *s; s++) {
		char c = *s;
		if (single_line && (c == '\n' || c == '\r'))
			continue;
		if (w->text_len + 1 >= (int)sizeof(w->text_buf))
			break;
		memmove(w->text_buf + w->cursor + 1, w->text_buf + w->cursor,
		    (size_t)(w->text_len - w->cursor));
		w->text_buf[w->cursor] = c;
		w->text_len++;
		w->cursor++;
	}
	w->text_buf[w->text_len] = '\0';
	w->sel_anchor = -1;
	cursor_blink = (float)be->time();
}

static void
input_click(bd_id id, int mx)
{
	struct widget *w = &pool[id];
	float text_x = (float)(w->x + w->pad) - w->scroll_x;
	int best = 0;
	float best_d = (float)mx - text_x;
	if (best_d < 0) best_d = -best_d;

	int pos = 0;
	while (pos < w->text_len) {
		int next = utf8_next(w->text_buf, pos, w->text_len);
		float px = text_x + input_text_px(w->text_buf, next);
		float d = (float)mx - px;
		if (d < 0) d = -d;
		if (d < best_d) {
			best = next;
			best_d = d;
		}
		pos = next;
	}
	w->cursor = best;
	w->sel_anchor = -1;
	cursor_blink = (float)be->time();
}

static int
input_key(bd_id id, int key, unsigned mods)
{
	struct widget *w = &pool[id];
	int shift = mods & BD_MOD_SHIFT;
	int ctrl = mods & BD_MOD_CTRL;
	int old_cursor = w->cursor;

	switch (key) {
	case BD_KEY_LEFT:
		if (w->sel_anchor >= 0 && !shift) {
			w->cursor = w->sel_anchor < w->cursor
			    ? w->sel_anchor : w->cursor;
			w->sel_anchor = -1;
			cursor_blink = (float)be->time();
			return 1;
		}
		if (ctrl)
			w->cursor = word_left(w->text_buf, w->cursor);
		else
			w->cursor = utf8_prev(w->text_buf, w->cursor);
		break;
	case BD_KEY_RIGHT:
		if (w->sel_anchor >= 0 && !shift) {
			w->cursor = w->sel_anchor > w->cursor
			    ? w->sel_anchor : w->cursor;
			w->sel_anchor = -1;
			cursor_blink = (float)be->time();
			return 1;
		}
		if (ctrl)
			w->cursor = word_right(w->text_buf, w->cursor,
			    w->text_len);
		else
			w->cursor = utf8_next(w->text_buf, w->cursor,
			    w->text_len);
		break;
	case BD_KEY_HOME:
		w->cursor = 0;
		break;
	case BD_KEY_END:
		w->cursor = w->text_len;
		break;

	case BD_KEY_BACKSPACE:
		if (w->sel_anchor >= 0 && w->sel_anchor != w->cursor) {
			input_delete_selection(w);
		} else if (w->cursor > 0) {
			int prev = utf8_prev(w->text_buf, w->cursor);
			memmove(w->text_buf + prev,
			    w->text_buf + w->cursor,
			    w->text_len - w->cursor);
			w->text_len -= (w->cursor - prev);
			w->cursor = prev;
			w->text_buf[w->text_len] = '\0';
		}
		w->sel_anchor = -1;
		cursor_blink = (float)be->time();
		return 1;
	case BD_KEY_DELETE:
		if (w->sel_anchor >= 0 && w->sel_anchor != w->cursor) {
			input_delete_selection(w);
		} else if (w->cursor < w->text_len) {
			int next = utf8_next(w->text_buf, w->cursor,
			    w->text_len);
			memmove(w->text_buf + w->cursor,
			    w->text_buf + next,
			    w->text_len - next);
			w->text_len -= (next - w->cursor);
			w->text_buf[w->text_len] = '\0';
		}
		w->sel_anchor = -1;
		cursor_blink = (float)be->time();
		return 1;

	case BD_KEY_ENTER:
		if (w->on_click)
			w->on_click(id, w->on_click_data);
		/* the MUD command line submits and clears; a plain text field
		 * commits but keeps its contents */
		if (w->type == BD_INPUT_LINE) {
			w->text_buf[0] = '\0';
			w->text_len = 0;
			w->cursor = 0;
			w->sel_anchor = -1;
			w->scroll_x = 0.0f;
		}
		cursor_blink = (float)be->time();
		return 1;
	case BD_KEY_ESCAPE:
		focus_id = BD_NONE;
		return 1;
	case BD_KEY_A:
		if (ctrl) {
			w->sel_anchor = 0;
			w->cursor = w->text_len;
			cursor_blink = (float)be->time();
			return 1;
		}
		return 0;
	case 'C':       /* Ctrl-C: copy selection */
		if (ctrl) {
			clipboard_copy_selection(w);
			return 1;
		}
		return 0;
	case 'X':       /* Ctrl-X: cut selection */
		if (ctrl) {
			clipboard_copy_selection(w);
			if (w->sel_anchor >= 0 && w->sel_anchor != w->cursor)
				input_delete_selection(w);
			cursor_blink = (float)be->time();
			return 1;
		}
		return 0;
	case 'V':       /* Ctrl-V: paste at the cursor */
		if (ctrl) {
			if (be->clipboard_get)
				input_insert_text(w, be->clipboard_get(),
				    w->type != BD_MULTILINE);
			return 1;
		}
		return 0;
	default:
		return 0;
	}

	if (shift) {
		if (w->sel_anchor < 0)
			w->sel_anchor = old_cursor;
	} else {
		w->sel_anchor = -1;
	}
	cursor_blink = (float)be->time();
	return 1;
}

/*
 * Key handling for BD_MULTILINE. Home/End are line-relative, Up/Down move
 * across lines preserving the caret's x, and Enter inserts a newline; every
 * other key (Left/Right/Backspace/Delete/Ctrl-A/Escape) reuses input_key,
 * which edits the shared buffer and treats '\n' as an ordinary byte.
 */
static int
multiline_key(bd_id id, int key, unsigned mods)
{
	struct widget *w = &pool[id];
	int ls = ml_line_start(w->text_buf, w->cursor);

	switch (key) {
	case BD_KEY_HOME:
		w->cursor = ls;
		break;
	case BD_KEY_END:
		w->cursor = ml_line_end(w->text_buf, w->text_len, w->cursor);
		break;
	case BD_KEY_UP: {
		if (ls == 0) {
			w->cursor = 0;
			break;
		}
		float x = ml_span_px(w->text_buf, ls, w->cursor);
		int pstart = ml_line_start(w->text_buf, ls - 1);
		w->cursor = ml_col_at_px(w->text_buf, pstart, ls - 1, x);
		break;
	}
	case BD_KEY_DOWN: {
		int le = ml_line_end(w->text_buf, w->text_len, w->cursor);
		if (le >= w->text_len) {
			w->cursor = w->text_len;
			break;
		}
		float x = ml_span_px(w->text_buf, ls, w->cursor);
		int nstart = le + 1;
		int nend = ml_line_end(w->text_buf, w->text_len, nstart);
		w->cursor = ml_col_at_px(w->text_buf, nstart, nend, x);
		break;
	}
	case BD_KEY_ENTER:
		input_insert_char(id, '\n');
		return 1;
	default:
		return input_key(id, key, mods);
	}
	w->sel_anchor = -1;
	cursor_blink = (float)be->time();
	return 1;
}

/* Place the caret at the clicked line and column. */
static void
multiline_click(bd_id id, int mx, int my)
{
	struct widget *w = &pool[id];
	int pad = w->pad;
	int ix = w->x + pad, iy = w->y + pad;
	int lh = ml_line_h();

	int line = (my - iy + (int)w->scroll_y) / (lh > 0 ? lh : 1);
	if (line < 0)
		line = 0;

	int pos = 0, li = 0;
	while (li < line && pos < w->text_len) {
		pos = ml_line_end(w->text_buf, w->text_len, pos);
		if (pos < w->text_len)
			pos++;
		li++;
	}
	int le = ml_line_end(w->text_buf, w->text_len, pos);
	float target = (float)(mx - ix) + w->scroll_x;
	w->cursor = ml_col_at_px(w->text_buf, pos, le, target);
	w->sel_anchor = -1;
	cursor_blink = (float)be->time();
}

/* ------------------------------------------------------------------ */
/* BD_LIST selection                                                  */
/* ------------------------------------------------------------------ */

/* scroll so the selected row is visible */
static void
list_ensure_visible(struct widget *w)
{
	if (w->cursor < 0)
		return;
	int lh = ml_line_h();
	int ih = w->h - 2 * w->pad;
	int top = w->cursor * lh;
	if (top - (int)w->scroll_y < 0)
		w->scroll_y = (float)top;
	if (top + lh - (int)w->scroll_y > ih)
		w->scroll_y = (float)(top + lh - ih);
	if (w->scroll_y < 0.0f)
		w->scroll_y = 0.0f;
}

/* select the row under my; returns the row, or -1 if past the last item */
static int
list_click(bd_id id, int my)
{
	struct widget *w = &pool[id];
	int lh = ml_line_h();
	int iy = w->y + w->pad;
	int row = (my - iy + (int)w->scroll_y) / (lh > 0 ? lh : 1);
	if (row < 0 || row >= list_count(w))
		return -1;
	w->cursor = row;
	return row;
}

/* keyboard: move/activate the selection. Returns 1 if consumed. */
static int
list_key(bd_id id, int key, unsigned mods)
{
	(void)mods;
	struct widget *w = &pool[id];
	int count = list_count(w);
	if (count == 0)
		return 0;

	switch (key) {
	case BD_KEY_UP:
		w->cursor = w->cursor <= 0 ? 0 : w->cursor - 1;
		break;
	case BD_KEY_DOWN:
		w->cursor = w->cursor < 0 ? 0
		    : (w->cursor + 1 >= count ? count - 1 : w->cursor + 1);
		break;
	case BD_KEY_HOME:
		w->cursor = 0;
		break;
	case BD_KEY_END:
		w->cursor = count - 1;
		break;
	case BD_KEY_ENTER:
		if (w->on_click && w->cursor >= 0)
			w->on_click(id, w->on_click_data);
		return 1;
	default:
		return 0;
	}
	list_ensure_visible(w);
	return 1;
}

/* ------------------------------------------------------------------ */
/* BD_TAB_BAR                                                         */
/* ------------------------------------------------------------------ */

/* set the active tab (clamped) and fire the change callback */
static void
tabbar_activate(bd_id id, int idx)
{
	struct widget *w = &pool[id];
	int n = list_count(w);
	if (n == 0)
		return;
	if (idx < 0) idx = 0;
	if (idx >= n) idx = n - 1;
	if (idx == w->cursor)
		return;
	w->cursor = idx;
	if (w->on_click)
		w->on_click(id, w->on_click_data);
}

/* ------------------------------------------------------------------ */
/* public: lifecycle                                                  */
/* ------------------------------------------------------------------ */

void
bd_gui_init_fonts(const bd_backend *backend, const bd_theme *th,
    const bd_font_set *fonts)
{
	be = backend;
	if (th)
		theme = *th;

	if (fonts) {
		if (!bd_draw_init_fonts(be, fonts, theme.font_size))
			fprintf(stderr, "bd: renderer init failed\n");
	} else if (!bd_draw_init(be, BD_ASSET_GUI_FONT, theme.font_size)) {
		fprintf(stderr, "bd: renderer init failed\n");
	}
	pin_out_tex = be->load_texture(BD_ASSET_PIN_OUT);
	pin_in_tex = be->load_texture(BD_ASSET_PIN_IN);
}

void
bd_gui_init(const bd_backend *backend, const bd_theme *th)
{
	bd_gui_init_fonts(backend, th, NULL);
}

void
bd_gui_cleanup(void)
{
	bd_draw_shutdown();
	if (pin_out_tex.id != 0)
		be->destroy_texture(pin_out_tex);
	if (pin_in_tex.id != 0)
		be->destroy_texture(pin_in_tex);
	for (int i = 1; i < pool_next; i++) {
		if (!pool[i].alive)
			continue;
		const bd_widget_class *cls = class_of(pool[i].type);
		if (cls && cls->destroy)
			cls->destroy(i, pool[i].state);
		free(pool[i].state);
		pool[i].state = NULL;
	}
	memset(pool, 0, sizeof pool);
	pool_next = 1;
	root = BD_NONE;
	window_count = 0;
	active_menu = BD_NONE;
	active_press = BD_NONE;
	pointer_capture = BD_NONE;
	pen_capture = BD_NONE;
	drag_menu = BD_NONE;
	focus_id = BD_NONE;
	active_notice = BD_NONE;
	notice_cb = NULL;
	notice_arg = NULL;
	memset(touches, 0, sizeof touches);
}

/* Place one top-level frame and its tree at the origin of the given size. */
static void
layout_frame(bd_id frame, int w, int h)
{
	struct widget *r = &pool[frame];
	r->x = 0;
	r->y = 0;
	r->w = w;
	r->h = h;
	layout_children(frame);
}

void
bd_gui_layout(int win_w, int win_h)
{
	if (root == BD_NONE)
		return;

	if (be && be->multi_window) {
		for (int i = 0; i < window_count; i++) {
			bd_id f = windows[i];
			if (!pool[f].alive)
				continue;
			layout_frame(f, be->window_width(pool[f].win_id),
			    be->window_height(pool[f].win_id));
		}
	} else {
		layout_frame(root, win_w, win_h);
	}

	/* shared pass: popups and extension-widget layout over the whole pool */
	bd_id i;
	for (i = 1; i < (bd_id)pool_next; i++) {
		struct widget *wi = &pool[i];
		if (!wi->alive)
			continue;
		if (wi->type == BD_MENU && wi->menu_open)
			layout_popup(i);
		const bd_widget_class *cls = class_of(wi->type);
		if (cls && cls->layout)
			cls->layout(i, wi->state, wi->w, wi->h);
	}
}

/* ------------------------------------------------------------------ */
/* BD_NOTICE modal overlay                                            */
/* ------------------------------------------------------------------ */

#define NOTICE_PAD 16
#define NOTICE_GAP 12

/* Lay the notice and its buttons out centered in a w x h window. */
static void
layout_notice(int w, int h)
{
	struct widget *nw = &pool[active_notice];
	int lh = ml_line_h();

	/* message size (text_buf, '\n'-separated) */
	int msg_w = 0, lines = 0, pos = 0;
	int mlen = nw->text_len;
	while (pos <= mlen) {
		int le = ml_line_end(nw->text_buf, mlen, pos);
		float lw = ml_span_px(nw->text_buf, pos, le);
		if ((int)lw > msg_w) msg_w = (int)lw;
		lines++;
		if (le >= mlen) break;
		pos = le + 1;
	}
	int msg_h = lines * lh;

	/* button sizes */
	int bh = (int)CHROME_FONT_SZ + 10;
	int btn_total = 0, nbtn = 0;
	for (bd_id c = nw->first_child; c != BD_NONE; c = pool[c].next_sib) {
		int bw = (int)chrome_text_w(pool[c].label) + 28;
		pool[c].w = bw;
		pool[c].h = bh;
		btn_total += bw;
		nbtn++;
	}
	if (nbtn > 1)
		btn_total += (nbtn - 1) * 8;

	int content_w = msg_w > btn_total ? msg_w : btn_total;
	int nwid = content_w + 2 * NOTICE_PAD;
	int nhei = NOTICE_PAD + msg_h + NOTICE_GAP + bh + NOTICE_PAD;
	int nx = (w - nwid) / 2, ny = (h - nhei) / 2;
	nw->x = nx; nw->y = ny; nw->w = nwid; nw->h = nhei;

	/* lay the buttons in a centered row at the bottom */
	int bx = nx + (nwid - btn_total) / 2;
	int by = ny + nhei - NOTICE_PAD - bh;
	for (bd_id c = nw->first_child; c != BD_NONE; c = pool[c].next_sib) {
		pool[c].x = bx;
		pool[c].y = by;
		bx += pool[c].w + 8;
	}
}

static void
render_notice(int w, int h)
{
	if (active_notice == BD_NONE || !pool[active_notice].alive)
		return;
	layout_notice(w, h);
	struct widget *nw = &pool[active_notice];

	bd_draw_begin(w, h);
	bd_draw_rect(0, 0, w, h, 0x00000099u);                 /* dim backdrop */
	fill_rect(nw->x, nw->y, nw->w, nw->h, theme.panel);    /* panel */
	stroke_rect(nw->x, nw->y, nw->w, nw->h, theme.border);

	/* message lines */
	int lh = ml_line_h();
	int pos = 0, li = 0, mlen = nw->text_len;
	while (pos <= mlen) {
		int le = ml_line_end(nw->text_buf, mlen, pos);
		char tmp[1024];
		int n = le - pos;
		if (n >= (int)sizeof(tmp)) n = (int)sizeof(tmp) - 1;
		if (n > 0) {
			memcpy(tmp, nw->text_buf + pos, n);
			tmp[n] = '\0';
			bd_draw_text(tmp, (float)(nw->x + NOTICE_PAD),
			    (float)(nw->y + NOTICE_PAD + li * lh), theme.text);
		}
		if (le >= mlen) break;
		pos = le + 1; li++;
	}

	for (bd_id c = nw->first_child; c != BD_NONE; c = pool[c].next_sib)
		render_widget(c);
	bd_draw_end();
}

/* On-screen caret rectangle of a focused text field (for IME positioning). */
static int
caret_rect(bd_id id, int *cx, int *cy, int *cw, int *ch)
{
	struct widget *w = &pool[id];
	int pad = w->pad;
	if (w->type == BD_MULTILINE) {
		int ls = ml_line_start(w->text_buf, w->cursor);
		int line = ml_line_index(w->text_buf, w->cursor);
		float px = ml_span_px(w->text_buf, ls, w->cursor);
		*cx = w->x + pad + (int)(px - w->scroll_x);
		*cy = w->y + pad + line * ml_line_h() - (int)w->scroll_y;
		*cw = 2;
		*ch = ml_line_h();
		return 1;
	}
	if (w->type == BD_INPUT_LINE || w->type == BD_TEXT) {
		float px = input_text_px(w->text_buf, w->cursor);
		*cx = w->x + pad + (int)(px - w->scroll_x);
		*cy = w->y;
		*cw = 2;
		*ch = w->h;
		return 1;
	}
	return 0;
}

/* Drive the backend IME: enable only while a text field is focused, and report
 * the caret so the platform places its candidate window there. */
static void
handle_ime_state(void)
{
	int want = (focus_id != BD_NONE && pool[focus_id].alive &&
	    is_text_field(pool[focus_id].type));
	if (be->ime_set_enabled && want != ime_enabled) {
		be->ime_set_enabled(want);
		ime_enabled = want;
	}
	if (want && be->ime_set_cursor_rect) {
		int x, y, w, h;
		if (caret_rect(focus_id, &x, &y, &w, &h))
			be->ime_set_cursor_rect(x, y, w, h);
	}
	if (!want) {
		preedit_len = 0;
		preedit_owner = BD_NONE;
	}
}

/* Render one top-level frame's tree plus the popups it owns into the currently
 * bound draw target of size w x h. */
static void
render_frame(bd_id frame, int w, int h)
{
	be->viewport(0, 0, w, h);
	be->clear(0.0f, 0.0f, 0.0f, 1.0f);

	/* layer 1: main widget tree */
	bd_draw_begin(w, h);
	render_widget(frame);
	bd_draw_end();

	/* layer 2: popup overlays owned by this frame */
	bd_draw_begin(w, h);
	render_popups(frame);
	bd_draw_end();
}

void
bd_gui_render(void)
{
	if (root == BD_NONE)
		return;

	if (be->multi_window) {
		for (int i = 0; i < window_count; i++) {
			bd_id f = windows[i];
			if (!pool[f].alive)
				continue;
			int id = pool[f].win_id;
			be->window_begin(id);
			render_frame(f, be->window_width(id),
			    be->window_height(id));
			if (i == 0)   /* modal notice overlays the primary window */
				render_notice(be->window_width(id),
				    be->window_height(id));
			be->window_swap(id);
		}
	} else {
		render_frame(root, be->width(), be->height());
		render_notice(be->width(), be->height());
	}

	handle_ime_state();
}

/* A widget that can hold keyboard focus via Tab. */
static int
is_focusable(bd_id id)
{
	struct widget *w = &pool[id];
	if (!w->alive || !w->visible || !w->enabled)
		return 0;
	if (is_text_field(w->type) || w->type == BD_LIST ||
	    w->type == BD_TAB_BAR)
		return 1;
	if (w->type == BD_BUTTON)
		return w->on_click != NULL;
	const bd_widget_class *cls = class_of(w->type);
	return cls && cls->event != NULL; /* extension widgets (explorer, ...) */
}

/* Append focusable widgets under `id` to list[], depth-first in child order.
 * Does not descend into menus (their items are not in the Tab order). */
static void
collect_focusable(bd_id id, bd_id *list, int *n, int max)
{
	struct widget *w = &pool[id];
	if (!w->alive || !w->visible)
		return;
	if (is_focusable(id) && *n < max)
		list[(*n)++] = id;
	if (w->type == BD_MENU)
		return;
	for (bd_id c = w->first_child; c != BD_NONE; c = pool[c].next_sib)
		collect_focusable(c, list, n, max);
}

/* Move keyboard focus to the next (dir +1) or previous (dir -1) focusable
 * widget within `frame`, wrapping around. */
static void
focus_advance(bd_id frame, int dir)
{
	static bd_id list[MAX_WIDGETS];
	int n = 0;
	collect_focusable(frame, list, &n, MAX_WIDGETS);
	if (n == 0) {
		focus_id = BD_NONE;
		return;
	}
	int cur = -1;
	for (int i = 0; i < n; i++)
		if (list[i] == focus_id) {
			cur = i;
			break;
		}
	int next = (cur < 0) ? (dir > 0 ? 0 : n - 1) : ((cur + dir + n) % n);
	focus_id = list[next];
}

bd_id
bd_focused(void)
{
	return focus_id;
}

void
bd_list_set_items(bd_id id, const char *items)
{
	if (id == BD_NONE || !pool[id].alive || pool[id].type != BD_LIST)
		return;
	bd_set(id, BD_LABEL_S, items, BD_END);
}

int
bd_list_count(bd_id id)
{
	if (id == BD_NONE || !pool[id].alive || pool[id].type != BD_LIST)
		return 0;
	return list_count(&pool[id]);
}

int
bd_list_selected(bd_id id)
{
	if (id == BD_NONE || !pool[id].alive || pool[id].type != BD_LIST)
		return -1;
	return pool[id].cursor;
}

void
bd_list_select(bd_id id, int row)
{
	if (id == BD_NONE || !pool[id].alive || pool[id].type != BD_LIST)
		return;
	int n = list_count(&pool[id]);
	if (row < -1) row = -1;
	if (row >= n) row = n - 1;
	pool[id].cursor = row;
	list_ensure_visible(&pool[id]);
}

void
bd_tabbar_set_tabs(bd_id id, const char *tabs)
{
	if (id == BD_NONE || !pool[id].alive || pool[id].type != BD_TAB_BAR)
		return;
	bd_set(id, BD_LABEL_S, tabs, BD_END);
}

int
bd_tabbar_count(bd_id id)
{
	if (id == BD_NONE || !pool[id].alive || pool[id].type != BD_TAB_BAR)
		return 0;
	return list_count(&pool[id]);
}

int
bd_tabbar_active(bd_id id)
{
	if (id == BD_NONE || !pool[id].alive || pool[id].type != BD_TAB_BAR)
		return -1;
	return pool[id].cursor;
}

void
bd_tabbar_set_active(bd_id id, int index)
{
	if (id == BD_NONE || !pool[id].alive || pool[id].type != BD_TAB_BAR)
		return;
	int n = list_count(&pool[id]);
	if (index < 0) index = 0;
	if (index >= n) index = n - 1;
	pool[id].cursor = index;   /* programmatic set: no callback */
}

void
bd_scrollbar_set(bd_id id, float pos, float frac)
{
	if (id == BD_NONE || !pool[id].alive || pool[id].type != BD_SCROLLBAR)
		return;
	if (pos < 0.0f) pos = 0.0f;
	if (pos > 1.0f) pos = 1.0f;
	if (frac < 0.0f) frac = 0.0f;
	if (frac > 1.0f) frac = 1.0f;
	pool[id].scroll_y = pos;
	pool[id].scroll_x = frac;
}

float
bd_scrollbar_value(bd_id id)
{
	if (id == BD_NONE || !pool[id].alive || pool[id].type != BD_SCROLLBAR)
		return 0.0f;
	return pool[id].scroll_y;
}

void
bd_notice_close(bd_id notice)
{
	if (notice == BD_NONE)
		return;
	if (active_notice == notice) {
		active_notice = BD_NONE;
		notice_cb = NULL;
		notice_arg = NULL;
	}
	bd_destroy(notice);
}

/* internal: a notice button carries its index in on_click_data */
static void
notice_button_clicked(bd_id btn, void *data)
{
	int idx = (int)(intptr_t)data;
	bd_id n = pool[btn].parent;
	bd_notice_cb cb = notice_cb;
	void *arg = notice_arg;
	bd_notice_close(n);            /* clears globals, destroys the subtree */
	if (cb)
		cb(n, idx, arg);
}

bd_id
bd_notice_open(const char *message, const char *buttons,
    bd_notice_cb cb, void *arg)
{
	bd_id n = bd_create(BD_NONE, BD_NOTICE, BD_END);
	if (n == BD_NONE)
		return BD_NONE;

	struct widget *nw = &pool[n];
	int mlen = message ? (int)strlen(message) : 0;
	if (mlen >= (int)sizeof(nw->text_buf))
		mlen = (int)sizeof(nw->text_buf) - 1;
	if (mlen > 0)
		memcpy(nw->text_buf, message, (size_t)mlen);
	nw->text_buf[mlen] = '\0';
	nw->text_len = mlen;

	const char *bs = (buttons && buttons[0]) ? buttons : "OK";
	int blen = (int)strlen(bs), p = 0, i = 0;
	while (p <= blen) {
		int e = p;
		while (e < blen && bs[e] != '\n') e++;
		bd_id b = bd_create(n, BD_BUTTON, BD_END);
		if (b != BD_NONE) {
			int ln = e - p;
			if (ln >= (int)sizeof(pool[b].text_buf))
				ln = (int)sizeof(pool[b].text_buf) - 1;
			memcpy(pool[b].text_buf, bs + p, (size_t)ln);
			pool[b].text_buf[ln] = '\0';
			pool[b].label = pool[b].text_buf;   /* owned, persistent */
			bd_set(b, BD_ON_CLICK_F, notice_button_clicked,
			    BD_ON_CLICK_P, (void *)(intptr_t)i, BD_END);
		}
		i++;
		if (e >= blen) break;
		p = e + 1;
	}

	notice_cb = cb;
	notice_arg = arg;
	active_notice = n;
	return n;
}

/* Route a touch to a per-finger captured widget, synthesizing mouse events so
 * existing extension widgets (knobs, sliders, ...) work unchanged. Several
 * fingers can drive several widgets at once. */
static int
handle_touch(const bd_event *ev, bd_id frame)
{
	bd_event m;
	int i;

	if (ev->type == BD_EV_TOUCH_DOWN) {
		int s = -1;
		for (i = 0; i < MAX_TOUCHES; i++)
			if (!touches[i].active) { s = i; break; }
		if (s < 0)
			return 1;
		touches[s].active = 1;
		touches[s].id = ev->touch;
		touches[s].widget = hit_extension(frame, ev->x, ev->y);
		if (touches[s].widget != BD_NONE) {
			m = (bd_event){0};
			m.type = BD_EV_MOUSE_DOWN;
			m.button = BD_MOUSE_LEFT;
			m.x = ev->x; m.y = ev->y; m.window = ev->window;
			ext_event(touches[s].widget, &m);
		}
		return 1;
	}

	for (i = 0; i < MAX_TOUCHES; i++)
		if (touches[i].active && touches[i].id == ev->touch)
			break;
	if (i == MAX_TOUCHES)
		return 1;

	if (touches[i].widget != BD_NONE) {
		m = (bd_event){0};
		m.type = (ev->type == BD_EV_TOUCH_UP) ? BD_EV_MOUSE_UP
		    : BD_EV_MOUSE_MOVE;
		m.button = BD_MOUSE_LEFT;
		m.x = ev->x; m.y = ev->y; m.window = ev->window;
		ext_event(touches[i].widget, &m);
	}
	if (ev->type == BD_EV_TOUCH_UP) {
		touches[i].active = 0;
		touches[i].widget = BD_NONE;
	}
	return 1;
}

/* Route a stylus event to the extension widget under it. The pen carries
 * pressure/tilt/eraser, so unlike touch it is delivered verbatim (not as a
 * synthesized mouse event) for a drawing-canvas to consume. Contact (DOWN)
 * captures the widget so the rest of the stroke goes to it even if the tip
 * strays past the edge; HOVER tracks the widget under the cursor without
 * capturing, so a canvas can preview the nib before touching down. */
static int
handle_pen(const bd_event *ev, bd_id frame)
{
	if (ev->type == BD_EV_PEN_DOWN) {
		pen_capture = hit_extension(frame, ev->x, ev->y);
		if (pen_capture != BD_NONE)
			ext_event(pen_capture, ev);
		return 1;
	}
	if (ev->type == BD_EV_PEN_HOVER) {
		bd_id ext = hit_extension(frame, ev->x, ev->y);
		if (ext != BD_NONE)
			ext_event(ext, ev);
		return 1;
	}
	/* MOVE / UP follow the captured widget */
	if (pen_capture != BD_NONE)
		ext_event(pen_capture, ev);
	if (ev->type == BD_EV_PEN_UP)
		pen_capture = BD_NONE;
	return 1;
}

int
bd_gui_event(const bd_event *ev)
{
	if (root == BD_NONE)
		return 0;

	/* a modal notice swallows all input except its own buttons */
	if (active_notice != BD_NONE && pool[active_notice].alive) {
		switch (ev->type) {
		case BD_EV_MOUSE_MOVE:
			update_hover(active_notice, ev->x, ev->y);
			return 1;
		case BD_EV_MOUSE_DOWN:
			if (ev->button == BD_MOUSE_LEFT) {
				bd_id hit = hit_interactive(active_notice,
				    ev->x, ev->y);
				if (hit != BD_NONE) {
					pool[hit].pressed = 1;
					active_press = hit;
				}
			}
			return 1;
		case BD_EV_MOUSE_UP:
			if (ev->button == BD_MOUSE_LEFT && active_press != BD_NONE) {
				struct widget *b = &pool[active_press];
				bd_id bid = active_press;
				b->pressed = 0;
				active_press = BD_NONE;
				if (b->on_click && in_rect(ev->x, ev->y,
				    b->x, b->y, b->w, b->h))
					b->on_click(bid, b->on_click_data);
			}
			return 1;
		case BD_EV_KEY_DOWN:
			if (ev->key == BD_KEY_ESCAPE) {
				bd_id n = active_notice;
				bd_notice_cb cb = notice_cb;
				void *arg = notice_arg;
				bd_notice_close(n);
				if (cb)
					cb(n, -1, arg);
			}
			return 1;
		default:
			return 1;
		}
	}

	/* the top-level frame this event is destined for (its own window) */
	bd_id frame = frame_for_window(ev->window);
	if (frame == BD_NONE || !pool[frame].alive)
		return 0;

	if (handle_menu_event(ev, frame))
		return 1;

	/* IME text: a finished commit inserts; a preedit updates the inline
	 * composition shown at the caret */
	if (ev->type == BD_EV_TEXT_COMMIT || ev->type == BD_EV_TEXT_PREEDIT) {
		if (focus_id == BD_NONE || !pool[focus_id].alive)
			return 0;
		if (is_text_field(pool[focus_id].type)) {
			if (ev->type == BD_EV_TEXT_PREEDIT) {
				int n = ev->text ? (int)strlen(ev->text) : 0;
				if (n >= (int)sizeof(preedit_buf))
					n = (int)sizeof(preedit_buf) - 1;
				if (n > 0)
					memcpy(preedit_buf, ev->text, (size_t)n);
				preedit_buf[n] = '\0';
				preedit_len = n;
				preedit_caret = ev->caret;
				preedit_owner = n > 0 ? focus_id : BD_NONE;
			} else {
				preedit_len = 0;
				preedit_owner = BD_NONE;
				input_insert_text(&pool[focus_id], ev->text,
				    pool[focus_id].type != BD_MULTILINE);
			}
			return 1;
		}
		/* extension widgets (e.g. the editor) handle text themselves */
		const bd_widget_class *cls = class_of(pool[focus_id].type);
		if (cls && cls->event && ext_event(focus_id, ev))
			return 1;
		return 0;
	}

	if (ev->type == BD_EV_TOUCH_DOWN || ev->type == BD_EV_TOUCH_MOVE ||
	    ev->type == BD_EV_TOUCH_UP)
		return handle_touch(ev, frame);

	if (ev->type == BD_EV_PEN_DOWN || ev->type == BD_EV_PEN_MOVE ||
	    ev->type == BD_EV_PEN_UP || ev->type == BD_EV_PEN_HOVER)
		return handle_pen(ev, frame);

	/* Tab / Shift-Tab cycles keyboard focus among the frame's widgets,
	 * before the event reaches whatever currently has focus */
	if (ev->type == BD_EV_KEY_DOWN && ev->key == BD_KEY_TAB) {
		focus_advance(frame, (ev->mods & BD_MOD_SHIFT) ? -1 : 1);
		return 1;
	}

	/* keyboard events for focused input line */
	if (focus_id != BD_NONE &&
	    pool[focus_id].alive &&
	    is_text_field(pool[focus_id].type)) {
		if (ev->type == BD_EV_CHAR && ev->codepoint >= 32) {
			input_insert_char(focus_id, ev->codepoint);
			return 1;
		}
		if (ev->type == BD_EV_KEY_DOWN)
			return pool[focus_id].type == BD_MULTILINE
			    ? multiline_key(focus_id, ev->key, ev->mods)
			    : input_key(focus_id, ev->key, ev->mods);
	}

	/* keyboard events for a focused list */
	if (focus_id != BD_NONE && pool[focus_id].alive &&
	    pool[focus_id].type == BD_LIST && ev->type == BD_EV_KEY_DOWN) {
		if (list_key(focus_id, ev->key, ev->mods))
			return 1;
	}

	/* Left/Right switch tabs on a focused tab bar */
	if (focus_id != BD_NONE && pool[focus_id].alive &&
	    pool[focus_id].type == BD_TAB_BAR && ev->type == BD_EV_KEY_DOWN) {
		if (ev->key == BD_KEY_LEFT) {
			tabbar_activate(focus_id, pool[focus_id].cursor - 1);
			return 1;
		}
		if (ev->key == BD_KEY_RIGHT) {
			tabbar_activate(focus_id, pool[focus_id].cursor + 1);
			return 1;
		}
	}

	/* Enter or Space activates a focused button */
	if (focus_id != BD_NONE && pool[focus_id].alive &&
	    pool[focus_id].type == BD_BUTTON) {
		int activate = (ev->type == BD_EV_KEY_DOWN && ev->key == BD_KEY_ENTER)
		    || (ev->type == BD_EV_CHAR && ev->codepoint == ' ');
		if (activate) {
			struct widget *b = &pool[focus_id];
			if (b->on_click && b->enabled)
				b->on_click(focus_id, b->on_click_data);
			return 1;
		}
	}

	/* keyboard events (incl. key-up) for a focused extension widget */
	if (focus_id != BD_NONE && pool[focus_id].alive &&
	    (ev->type == BD_EV_KEY_DOWN || ev->type == BD_EV_KEY_UP ||
	     ev->type == BD_EV_CHAR)) {
		const bd_widget_class *cls = class_of(pool[focus_id].type);
		if (cls && cls->event && ext_event(focus_id, ev))
			return 1;
	}

	switch (ev->type) {
	case BD_EV_MOUSE_MOVE:
		/* a captured widget keeps the pointer through a drag */
		if (pointer_capture != BD_NONE) {
			if (pool[pointer_capture].type == BD_SCROLLBAR) {
				struct widget *sb = &pool[pointer_capture];
				scrollbar_from_pointer(sb, ev->x, ev->y);
				if (sb->on_click)
					sb->on_click(pointer_capture,
					    sb->on_click_data);
			} else {
				ext_event(pointer_capture, ev);
			}
			return 1;
		}
		update_hover(frame, ev->x, ev->y);
		return 0;

	case BD_EV_MOUSE_SCROLL: {
		bd_id li = hit_interactive(frame, mouse_x, mouse_y);
		if (li != BD_NONE && pool[li].type == BD_LIST) {
			pool[li].scroll_y -= ev->scroll_dy * (float)ml_line_h();
			if (pool[li].scroll_y < 0.0f)
				pool[li].scroll_y = 0.0f;
			return 1;
		}
		bd_id ext = hit_extension(frame, mouse_x, mouse_y);
		if (ext != BD_NONE && ext_event(ext, ev))
			return 1;
		return 0;
	}

	case BD_EV_MOUSE_DOWN:
		/* non-left press on an extension widget (e.g. right-click for an
		 * explorer context menu): deliver without grabbing a drag */
		if (ev->button != BD_MOUSE_LEFT) {
			bd_id ext = hit_extension(frame, ev->x, ev->y);
			if (ext != BD_NONE) {
				focus_id = ext;
				return ext_event(ext, ev);
			}
			return 0;
		}
		if (ev->button == BD_MOUSE_LEFT) {
			int mx = ev->x;
			int my = ev->y;
			bd_id hit = hit_interactive(frame, mx, my);

			if (hit != BD_NONE &&
			    is_text_field(pool[hit].type)) {
				focus_id = hit;
				if (pool[hit].type == BD_MULTILINE)
					multiline_click(hit, mx, my);
				else
					input_click(hit, mx);
				return 1;
			}

			if (hit != BD_NONE && pool[hit].type == BD_LIST) {
				focus_id = hit;
				int row = list_click(hit, my);
				double now = be->time();
				if (row >= 0 && hit == list_last_id &&
				    row == list_last_row &&
				    now - list_last_time < 0.4) {
					/* double-click activates */
					struct widget *l = &pool[hit];
					if (l->on_click)
						l->on_click(hit, l->on_click_data);
					list_last_id = BD_NONE;
				} else {
					list_last_id = hit;
					list_last_row = row;
					list_last_time = now;
				}
				return 1;
			}

			if (hit != BD_NONE && pool[hit].type == BD_TAB_BAR) {
				focus_id = hit;
				int t = tab_at(&pool[hit], mx);
				if (t >= 0)
					tabbar_activate(hit, t);
				return 1;
			}

			if (hit != BD_NONE && pool[hit].type == BD_SCROLLBAR) {
				struct widget *sb = &pool[hit];
				pointer_capture = hit;
				sb->pressed = 1;
				scrollbar_from_pointer(sb, mx, my);
				if (sb->on_click)
					sb->on_click(hit, sb->on_click_data);
				return 1;
			}

			focus_id = BD_NONE;

			/* a value widget grabs the pointer for the drag; an explorer
			 * also takes keyboard focus for arrow-key navigation */
			bd_id ext = hit_extension(frame, mx, my);
			if (ext != BD_NONE) {
				pointer_capture = ext;
				focus_id = ext;
				ext_event(ext, ev);
				return 1;
			}

			if (hit != BD_NONE) {
				pool[hit].pressed = 1;
				active_press = hit;
				return 1;
			}
		}
		return 0;

	case BD_EV_MOUSE_UP:
		if (pointer_capture != BD_NONE) {
			if (pool[pointer_capture].type == BD_SCROLLBAR)
				pool[pointer_capture].pressed = 0;
			else
				ext_event(pointer_capture, ev);
			pointer_capture = BD_NONE;
			return 1;
		}
		if (ev->button == BD_MOUSE_LEFT &&
		    active_press != BD_NONE) {
			struct widget *w = &pool[active_press];
			w->pressed = 0;
			if (w->on_click &&
			    in_rect(ev->x, ev->y,
			        w->x, w->y, w->w, w->h))
				w->on_click(active_press, w->on_click_data);
			active_press = BD_NONE;
			return 1;
		}
		return 0;

	default:
		return 0;
	}
}
