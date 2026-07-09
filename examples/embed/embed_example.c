/*
 * embed_example.c -- a self-contained birdie-gui binary with no asset files.
 *
 * The chrome font and the monospace family the body editor renders (regular,
 * bold, italic) are compiled into the executable (see embed_assets.S) and handed
 * to the toolkit through the registry in bd_asset.h. The toolkit asks for each
 * asset by a generic id (BD_ASSET_FONT_REGULAR, BD_ASSET_FONT_MONO_BOLD, ...), so
 * a custom font is registered under BD_ASSET_FONT_REGULAR -- not under the stock
 * font's filename. Nothing here impersonates "DejaVuSans.ttf". This is the same
 * trick a distributed game uses to ship one file. (The pushpins and terminal
 * font are 1-bit bitmaps compiled into the toolkit, so they need no embedding.)
 *
 * Each id can be fed bytes (bd_asset_register_data, as here) or a file path
 * (bd_asset_register_file) -- e.g. to let a user pick their own font at runtime
 * without recompiling. Anything left unregistered falls back to the toolkit's
 * default file, so partial embedding works too.
 *
 * It runs on the raw X11/EGL/GLES backend (no ludica, no SDL3), the same one
 * the toolkit gallery uses, so it builds wherever that does.
 *
 *   ./embed_example           open a window drawn entirely from embedded assets
 *   ./embed_example --check    headless: verify every id is registered, exit
 *
 * The --check mode needs no display, so it is what the build runs to prove the
 * embedding is wired correctly. Run either mode from any directory: nothing is
 * read from disk.
 *
 * No build-machine path leaks into the binary: the .incbin source paths are a
 * build-time detail that never reaches the output, and this example passes them
 * relative. Never point an .incbin path at an $(abspath ...) -- that would bake
 * the build machine's directory layout and username into every shipped copy.
 * The toolkit's built-in asset names are short relative sub-paths
 * ("fonts/DejaVuSans.ttf", "pushpin/...") that this build never reads because
 * every asset is registered by id; they are machine-independent. See README.md.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "widget.h"
#include "bd_widget_vt.h"
#include "bd_widget_editor.h"
#include "bd_asset.h"
#include "bd_backend_gles.h"
#include "window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Blob bounds emitted by embed_assets.S. */
#define EMB(sym) \
	extern const unsigned char sym[]; \
	extern const unsigned char sym##_end[];
EMB(emb_font_ui)
EMB(emb_font_mono)
EMB(emb_font_mono_bold)
EMB(emb_font_mono_italic)

/* Each blob paired with the generic asset id the toolkit requests it by. The id
 * is an identity, not a filename -- the font blob happens to be DejaVu Sans here
 * but is registered as "the regular UI font", so any .ttf/.otf would drop in.
 * The body editor renders the monospace family, so its three faces are embedded
 * too, each under its own id -- that is how a real app ships a styled family. */
static const struct embed {
	const char          *id;
	const unsigned char *data;
	const unsigned char *end;
} embeds[] = {
	{ BD_ASSET_FONT_REGULAR,     emb_font_ui,          emb_font_ui_end          },
	{ BD_ASSET_FONT_MONO,        emb_font_mono,        emb_font_mono_end        },
	{ BD_ASSET_FONT_MONO_BOLD,   emb_font_mono_bold,   emb_font_mono_bold_end   },
	{ BD_ASSET_FONT_MONO_ITALIC, emb_font_mono_italic, emb_font_mono_italic_end },
};

/* Register every embedded blob before the toolkit initializes, so its first
 * font bake and texture load resolve to memory instead of the filesystem. */
static void
register_assets(void)
{
	for (size_t i = 0; i < sizeof embeds / sizeof embeds[0]; i++)
		bd_asset_register_data(embeds[i].id, embeds[i].data,
		    (size_t)(embeds[i].end - embeds[i].data));
}

/* Headless proof: the registry serves each id from memory. Returns exit status. */
static int
run_check(void)
{
	int fails = 0;

	register_assets();
	for (size_t i = 0; i < sizeof embeds / sizeof embeds[0]; i++) {
		const struct embed *e = &embeds[i];
		size_t emb_len = (size_t)(e->end - e->data);
		bd_asset a;
		int ok = bd_asset_lookup(e->id, &a) && a.data == e->data
		    && a.len == emb_len && emb_len > 0;
		printf("%-22s %-5s %zu bytes\n",
		    e->id, ok ? "OK" : "FAIL", emb_len);
		if (!ok)
			fails++;
	}

	if (fails)
		printf("\n%d FAILED\n", fails);
	else
		printf("\nall %zu assets served from memory, none read from disk\n",
		    sizeof embeds / sizeof embeds[0]);
	return fails ? 1 : 0;
}

static int running = 1;

static void
on_quit(bd_id id, void *arg)
{
	(void)id; (void)arg;
	running = 0;
}

/* A tiny UI: a menu bar (the pinnable menu draws the toolkit's built-in pushpin
 * glyph), a label (embedded font), and a terminal (drawn from birdie-gui's
 * built-in bitmap font). */
static void
build_ui(void)
{
	bd_id frame = bd_create(BD_NONE, BD_FRAME,
		BD_LABEL_S, "birdie-gui: assets embedded in the binary",
		BD_LAYOUT_I, BD_LAYOUT_COL, BD_END);

	bd_id menu = bd_create(frame, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_ROW,
		BD_PREF_H_I, 22, BD_BG_C, 0x3C3F41FFu,
		BD_PAD_I, 2, BD_GAP_I, 16, BD_END);
	bd_id m_file = bd_create(menu, BD_MENU, BD_LABEL_S, "File", BD_END);
	bd_create(m_file, BD_BUTTON, BD_LABEL_S, "Quit",
		BD_ON_CLICK_F, on_quit, BD_END);
	/* pinnable menu: opening it pinned draws the toolkit's built-in pushpin */
	bd_id m_view = bd_create(menu, BD_MENU, BD_LABEL_S, "View (pinnable)",
		BD_END);
	bd_create(m_view, BD_BUTTON, BD_LABEL_S, "Nothing here", BD_END);

	bd_id body = bd_create(frame, BD_PANEL, BD_LAYOUT_I, BD_LAYOUT_COL,
		BD_GROW_I, 1, BD_PAD_I, 8, BD_GAP_I, 6, BD_END);
	bd_create(body, BD_LABEL,
		BD_LABEL_S, "This label is drawn from a font baked into the "
		"executable. No .ttf or .png is read from disk.",
		BD_PREF_H_I, 20, BD_END);

	/* A rich-text editor renders the embedded monospace family: the regular,
	 * bold, and italic mono faces each resolve from their own embedded blob,
	 * which is why the example bakes in three mono faces, not just one. */
	bd_id ed = bd_editor_create(body, BD_PREF_H_I, 60, BD_END);
	bd_editor_set_monospace(ed, 1);
	bd_editor_set_text(ed,
		"This editor uses the embedded monospace font.\n"
		"Bold and italic spans use the mono-bold and mono-italic faces.");
	bd_editor_highlight_span(ed, 1, 0, 4, (bd_rich_style){ .flags = BD_RT_BOLD });
	bd_editor_highlight_span(ed, 1, 9, 15, (bd_rich_style){ .flags = BD_RT_ITALIC });
	bd_editor_set_locked(ed, 1);

	bd_id term = bd_terminal_create(body, BD_GROW_I, 1, BD_END);
	bd_terminal_write(term,
		"\033[1membed_example\033[0m -- terminal uses the built-in bitmap font.\r\n"
		"Run this from any directory; it needs no asset files.\r\n", -1);

	bd_create(body, BD_BUTTON, BD_LABEL_S, "Quit", BD_PREF_W_I, 80,
		BD_ON_CLICK_F, on_quit, BD_END);
}

int
main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "--check") == 0)
		return run_check();

	/* Register BEFORE bd_gui_init: the toolkit bakes the fonts during init,
	 * and they must resolve to the embedded blobs. */
	register_assets();

	if (win_open("birdie-gui embedded assets", 900, 600) != 0) {
		fprintf(stderr, "embed_example: cannot open window\n");
		return 1;
	}
	bd_gui_init(&bd_backend_gles, NULL);
	build_ui();

	while (running) {
		win_event wev;
		while (win_poll(&wev)) {
			if (wev.type == WIN_EV_CLOSE && wev.window == 1)
				running = 0;
			else if (wev.type == WIN_EV_KEY_DOWN
			    && wev.key == WIN_KEY_ESCAPE)
				running = 0;

			bd_event bev;
			if (bd_event_from_win(&wev, &bev))
				bd_gui_event(&bev);
		}
		bd_gui_layout(win_width(), win_height());
		bd_gui_render();
	}

	bd_gui_cleanup();
	win_close();
	return 0;
}
