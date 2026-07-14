/*
 * bd_dialog -- composition helper over the modal layer. See bd_dialog.h and
 * doc/gui/dialogs.md.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_dialog.h"
#include <stdlib.h>

#define DLG_PAD     12    /* panel inset */
#define DLG_GAP      8    /* gap between rows */
#define TITLE_H     20    /* title label height */
#define FIELD_H     26    /* labeled field row height */
#define LABEL_W     90    /* field label column width */
#define BTNROW_H    32    /* button row height */
#define BTN_W       84    /* button width */
#define MAX_BTN      8

struct bd_dialog {
    bd_id panel;      /* the top-level modal panel */
    bd_id content;    /* column container the caller fills */
    bd_id btnrow;     /* right-aligned button row */
    int   nbtn;
    int   default_idx, cancel_idx;
    struct {
        bd_callback_fn cb;
        void          *arg;
        int            role;
    } btn[MAX_BTN];
    /* stable per-button back-references handed to BD_ON_CLICK_F */
    struct btn_ref { bd_dialog *d; int idx; } ref[MAX_BTN];
};

/* one internal click handler for every dialog button: run the user callback,
 * and for a cancel button close the dialog afterward */
static void
dialog_btn_click(bd_id btn, void *data)
{
    (void)btn;
    struct btn_ref *r = data;
    bd_dialog *d = r->d;
    bd_callback_fn cb = d->btn[r->idx].cb;
    void *arg = d->btn[r->idx].arg;
    if (cb)
        cb(d->panel, arg);
    if (d->btn[r->idx].role == BD_DIALOG_CANCEL)
        bd_modal_close(d->panel);
}

/* Enter: fire the default button's callback (no auto-close, like the modal). */
static void
dialog_accept(bd_id panel, void *arg)
{
    (void)panel;
    bd_dialog *d = arg;
    if (d->default_idx >= 0 && d->btn[d->default_idx].cb)
        d->btn[d->default_idx].cb(d->panel, d->btn[d->default_idx].arg);
}

/* Escape: fire the cancel button's callback (the modal closes itself after). */
static void
dialog_cancel(bd_id panel, void *arg)
{
    (void)panel;
    bd_dialog *d = arg;
    if (d->cancel_idx >= 0 && d->btn[d->cancel_idx].cb)
        d->btn[d->cancel_idx].cb(d->panel, d->btn[d->cancel_idx].arg);
}

bd_dialog *
bd_dialog_create(const char *title, int w, int h)
{
    bd_dialog *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->default_idx = d->cancel_idx = -1;
    const bd_theme *th = bd_gui_theme();

    d->panel = bd_create(BD_NONE, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_COL,
        BD_PREF_W_I, w, BD_PREF_H_I, h, BD_BG_C, th->panel,
        BD_PAD_I, DLG_PAD, BD_GAP_I, DLG_GAP, BD_END);
    if (title && title[0])
        bd_create(d->panel, BD_LABEL, BD_LABEL_S, title, BD_PREF_H_I, TITLE_H,
            BD_FG_C, th->text_hi, BD_END);
    /* content grows to fill, pushing the button row to the bottom edge */
    d->content = bd_create(d->panel, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_COL,
        BD_GROW_I, 1, BD_GAP_I, DLG_GAP, BD_BG_C, th->panel, BD_END);
    /* button row: a leading spacer right-aligns the buttons */
    d->btnrow = bd_create(d->panel, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_ROW,
        BD_PREF_H_I, BTNROW_H, BD_GAP_I, DLG_GAP, BD_BG_C, th->panel, BD_END);
    bd_create(d->btnrow, BD_LABEL, BD_LABEL_S, "", BD_GROW_I, 1, BD_END);
    return d;
}

bd_id
bd_dialog_panel(bd_dialog *d)
{
    return d ? d->panel : BD_NONE;
}

bd_id
bd_dialog_content(bd_dialog *d)
{
    return d ? d->content : BD_NONE;
}

bd_id
bd_dialog_field(bd_dialog *d, const char *label)
{
    if (!d)
        return BD_NONE;
    const bd_theme *th = bd_gui_theme();
    bd_id row = bd_create(d->content, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_ROW,
        BD_PREF_H_I, FIELD_H, BD_GAP_I, DLG_GAP, BD_BG_C, th->panel, BD_END);
    bd_create(row, BD_LABEL, BD_LABEL_S, label ? label : "",
        BD_PREF_W_I, LABEL_W, BD_FG_C, th->text, BD_END);
    return row;
}

bd_id
bd_dialog_button(bd_dialog *d, const char *label, int role,
                 bd_callback_fn cb, void *arg)
{
    if (!d || d->nbtn >= MAX_BTN)
        return BD_NONE;
    int i = d->nbtn++;
    d->btn[i].cb = cb;
    d->btn[i].arg = arg;
    d->btn[i].role = role;
    d->ref[i].d = d;
    d->ref[i].idx = i;
    if (role == BD_DIALOG_DEFAULT)
        d->default_idx = i;
    else if (role == BD_DIALOG_CANCEL)
        d->cancel_idx = i;
    return bd_create(d->btnrow, BD_BUTTON, BD_LABEL_S, label ? label : "",
        BD_PREF_W_I, BTN_W, BD_ON_CLICK_F, dialog_btn_click,
        BD_ON_CLICK_P, &d->ref[i], BD_END);
}

void
bd_dialog_open(bd_dialog *d)
{
    if (!d)
        return;
    bd_modal_open_ex(d->panel, &(bd_modal_opts){
        .focus = BD_NONE,   /* first focusable: the first field */
        .on_accept = d->default_idx >= 0 ? dialog_accept : NULL,
        .on_cancel = dialog_cancel,
        .arg = d,
    });
}

void
bd_dialog_close(bd_dialog *d)
{
    if (d)
        bd_modal_close(d->panel);
}

void
bd_dialog_free(bd_dialog *d)
{
    if (!d)
        return;
    bd_destroy(d->panel);
    free(d);
}
