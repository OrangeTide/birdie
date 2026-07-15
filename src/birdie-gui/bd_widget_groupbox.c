/* bd_widget_groupbox.c : labeled etched-border fieldset container */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/*
 * A group box is a plain BD_LAYOUT_COL container (BD_WC_CONTAINS_CHILDREN) with
 * one trick: its first child is a fixed-height spacer that reserves the top
 * title band, so the fields the caller adds stack below the caption. The
 * render() hook draws the etched groove around the widget rect with a gap in
 * the top line where the caption sits. Side and bottom insets come from the
 * container's uniform padding; only the top needs the extra band, which the
 * spacer provides, so no asymmetric padding or custom layout is required.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_widget_groupbox.h"
#include "widget_ext.h"
#include "bd_draw.h"
#include "bd_theme.h"
#include <stdarg.h>

#define GB_PAD  8   /* inset from the groove to the content, px */
#define GB_GAP  4   /* gap between stacked fields, px */

struct groupbox {
	const char *title;
};

static int gb_type;

static void
gb_render(bd_id id, void *state)
{
	struct groupbox *g = state;
	int x, y, w, h;
	bd_widget_rect(id, &x, &y, &w, &h);
	if (w <= 2 || h <= 2)
		return;

	const bd_theme *th = bd_gui_theme();
	uint32_t dark  = th->border;   /* groove shadow line */
	uint32_t light = th->hover;    /* inner highlight (the etched look) */

	int lh  = (int)bd_draw_line_height();
	int top = y + lh / 2;          /* the border's top edge runs through the
	                                * vertical middle of the caption */
	int bot = y + h - 1;
	int rgt = x + w - 1;

	/* the caption breaks the top line */
	int tw   = (g->title && g->title[0]) ? (int)bd_draw_text_width(g->title) : 0;
	int gap0 = x + 8;              /* start of the gap */
	int gap1 = tw ? x + 8 + tw + 6 : x + 1;   /* end of the gap */

	/* dark groove: top line (split by the caption gap), then the other three
	 * sides */
	if (gap0 > x + 1)
		bd_draw_rect((float)(x + 1), (float)top,
		    (float)(gap0 - (x + 1)), 1.0f, dark);
	if (rgt > gap1)
		bd_draw_rect((float)gap1, (float)top,
		    (float)(rgt - gap1), 1.0f, dark);
	bd_draw_rect((float)(x + 1), (float)top, 1.0f, (float)(bot - top), dark);
	bd_draw_rect((float)(rgt - 1), (float)top, 1.0f, (float)(bot - top), dark);
	bd_draw_rect((float)(x + 1), (float)(bot - 1), (float)(w - 2), 1.0f, dark);

	/* inner highlight one pixel in on the top and left, so the groove reads as
	 * incised rather than a flat line */
	if (gap0 > x + 2)
		bd_draw_rect((float)(x + 2), (float)(top + 1),
		    (float)(gap0 - (x + 2)), 1.0f, light);
	if (rgt - 1 > gap1)
		bd_draw_rect((float)gap1, (float)(top + 1),
		    (float)(rgt - 1 - gap1), 1.0f, light);
	bd_draw_rect((float)(x + 2), (float)(top + 1), 1.0f,
	    (float)(bot - top - 2), light);

	/* caption text sitting in the gap, over the border line */
	if (tw)
		bd_draw_text(g->title, (float)(x + 11), (float)y, th->text);
}

static const bd_widget_class gb_class = {
	.name       = "groupbox",
	.state_size = sizeof(struct groupbox),
	.render     = gb_render,
	.flags      = BD_WC_CONTAINS_CHILDREN,
};

void
bd_groupbox_register(void)
{
	if (!gb_type)
		gb_type = bd_register_widget_class(&gb_class);
}

bd_id
bd_groupbox_create(bd_id parent, const bd_groupbox_desc *desc, ...)
{
	bd_groupbox_register();
	va_list ap;
	va_start(ap, desc);
	bd_id id = bd_create_va(parent, gb_type, ap);
	va_end(ap);
	struct groupbox *g = bd_widget_state(id);
	if (!g)
		return id;
	g->title = desc ? desc->title : NULL;

	bd_set(id, BD_LAYOUT_I, BD_LAYOUT_COL, BD_PAD_I, GB_PAD, BD_GAP_I, GB_GAP,
	    BD_END);
	/* the title-band spacer reserves the top so fields stack below the caption;
	 * the caller's children append after it */
	bd_create(id, BD_PANEL, BD_PREF_H_I, (int)bd_draw_line_height(), BD_END);
	return id;
}

void
bd_groupbox_set_title(bd_id id, const char *title)
{
	if (bd_widget_type(id) != gb_type)
		return;
	((struct groupbox *)bd_widget_state(id))->title = title;
}

const char *
bd_groupbox_title(bd_id id)
{
	if (bd_widget_type(id) != gb_type)
		return NULL;
	return ((struct groupbox *)bd_widget_state(id))->title;
}
