/* embed_assets.S : birdie-gui runtime assets baked into this example binary.
 *
 * Each blob is pulled in with .incbin from an absolute path the makefile passes
 * as a -D macro (EMBED_FONT_UI and friends), so the data is part of .rodata at
 * link time and the program reads no asset files at runtime. embed_example.c
 * registers these symbols with bd_asset_register() by basename.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

	.section .rodata

	.global emb_font_ui
	.global emb_font_ui_end
	.balign 4
emb_font_ui:
	.incbin EMBED_FONT_UI
emb_font_ui_end:

/* The monospace family the body editor renders: regular, bold, italic. */
	.global emb_font_mono
	.global emb_font_mono_end
	.balign 4
emb_font_mono:
	.incbin EMBED_FONT_MONO
emb_font_mono_end:

	.global emb_font_mono_bold
	.global emb_font_mono_bold_end
	.balign 4
emb_font_mono_bold:
	.incbin EMBED_FONT_MONO_BOLD
emb_font_mono_bold_end:

	.global emb_font_mono_italic
	.global emb_font_mono_italic_end
	.balign 4
emb_font_mono_italic:
	.incbin EMBED_FONT_MONO_ITALIC
emb_font_mono_italic_end:

/* Mark the stack non-executable (silences the GNU ld exec-stack warning). */
	.section .note.GNU-stack,"",@progbits
