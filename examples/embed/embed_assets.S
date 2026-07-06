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

	.global emb_pin_out
	.global emb_pin_out_end
	.balign 4
emb_pin_out:
	.incbin EMBED_PIN_OUT
emb_pin_out_end:

	.global emb_pin_in
	.global emb_pin_in_end
	.balign 4
emb_pin_in:
	.incbin EMBED_PIN_IN
emb_pin_in_end:

	.global emb_term_atlas
	.global emb_term_atlas_end
	.balign 4
emb_term_atlas:
	.incbin EMBED_TERM_ATLAS
emb_term_atlas_end:

/* Mark the stack non-executable (silences the GNU ld exec-stack warning). */
	.section .note.GNU-stack,"",@progbits
