#ifndef BD_WIDGET_H
#define BD_WIDGET_H

#include <stdint.h>

typedef unsigned int bd_id;
#define BD_NONE ((bd_id)0)

typedef void (*bd_callback_fn)(bd_id id, void *arg);

/* widget types */
enum {
	BD_FRAME = 1,
	BD_PANEL,
	BD_LABEL,
	BD_BUTTON,
	BD_TEXT,
	BD_MULTILINE,
	BD_LIST,
	BD_SCROLLBAR,
	BD_MENU,
	BD_NOTICE,
	BD_TAB_BAR,
	BD_TERMINAL,
	BD_INPUT_LINE,
};

/* layout modes */
enum {
	BD_LAYOUT_ROW   = 1,
	BD_LAYOUT_COL   = 2,
	BD_LAYOUT_FIXED = 3,
};

/*
 * Attribute IDs — low 4 bits encode the value type so the varargs
 * reader knows which va_arg width to pull:
 *   0=end  1=int  2=string  3=pointer  4=callback  5=color  6=bool
 */
enum {
	BD_END        = 0x000,

	BD_WIDTH_I    = 0x011,
	BD_HEIGHT_I   = 0x021,
	BD_PREF_W_I   = 0x031,
	BD_PREF_H_I   = 0x041,
	BD_GROW_I     = 0x051,
	BD_X_I        = 0x061,
	BD_Y_I        = 0x071,
	BD_LAYOUT_I   = 0x081,
	BD_ROLE_I     = 0x091,
	BD_PAD_I      = 0x0A1,
	BD_GAP_I      = 0x0B1,

	BD_LABEL_S    = 0x012,
	BD_NAME_S     = 0x022,

	BD_ON_CLICK_P = 0x013,
	BD_ON_CLOSE_P = 0x023,

	BD_ON_CLICK_F = 0x014,
	BD_ON_CLOSE_F = 0x024,

	BD_FG_C       = 0x015,
	BD_BG_C       = 0x025,

	BD_VISIBLE_B  = 0x016,
	BD_ENABLED_B  = 0x026,
	BD_MENU_PIN_B = 0x036,
};

/* tagged-union attribute for bd_create_v / bd_set_v */
typedef struct {
	int id;
	union {
		int i;
		const char *s;
		void *p;
		bd_callback_fn f;
		uint32_t c;
		bd_id wid;
	};
} bd_attr;

/* widget tree */
bd_id       bd_create(bd_id parent, int type, ...);
bd_id       bd_create_v(bd_id parent, int type, const bd_attr *attrs);
void        bd_destroy(bd_id id);
void        bd_set(bd_id id, ...);
void        bd_set_v(bd_id id, const bd_attr *attrs);
int         bd_get_i(bd_id id, int attr);
const char *bd_get_s(bd_id id, int attr);
bd_id       bd_parent(bd_id id);
bd_id       bd_first_child(bd_id id);
bd_id       bd_next_sibling(bd_id id);

/* GUI lifecycle — called from the ludica main loop */
void bd_gui_init(void);
void bd_gui_cleanup(void);
void bd_gui_layout(int win_w, int win_h);
void bd_gui_render(void);
int  bd_gui_event(const void *ev);

#endif
