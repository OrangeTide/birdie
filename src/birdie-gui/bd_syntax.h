#ifndef BD_SYNTAX_H
#define BD_SYNTAX_H

#include <stdint.h>

/*
 * bd_syntax -- a small, runtime-configurable syntax highlighter. A language is
 * a state machine (named states, each with ordered character-class or literal
 * transition rules and an optional keyword map) that the tokenizer walks one
 * byte at a time, carrying state across lines so multi-line comments and
 * strings work. It emits (start, end, appearance) spans; it owns no widget and
 * no editor state, so anything holding text can highlight with it.
 *
 * A language definition is parsed from an in-memory text buffer
 * (bd_syntax_parse) or taken from a compiled-in default (bd_syntax_builtin).
 * The design and the text format are documented in doc/gui/editor-highlight.md.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* Appearance flags. These match bd_widget_editor.h's BD_RT_* bit values so a
 * span maps straight onto a bd_rich_style. */
enum {
	BD_SYN_BOLD      = 1 << 0,
	BD_SYN_ITALIC    = 1 << 1,
	BD_SYN_UNDERLINE = 1 << 2,
};

/* One emitted span: buf[start,end) drawn with this appearance (fg/bg are RGBA8,
 * 0 = inherit / none). */
typedef struct bd_syntax_span {
	int      start, end;
	unsigned flags;   /* BD_SYN_* */
	uint32_t fg, bg;
} bd_syntax_span;

typedef struct bd_syntax_lang bd_syntax_lang;   /* opaque compiled language */

/* Parse a language from an in-memory definition (see the doc for the format).
 * Returns a heap language to free with bd_syntax_free, or NULL on a hard parse
 * error. len < 0 means strlen(text). */
bd_syntax_lang *bd_syntax_parse(const char *text, int len);
void            bd_syntax_free(bd_syntax_lang *lang);

/* A compiled-in default by name ("lua", "abc"); lazily parsed and cached, so
 * the returned language is owned by the library. NULL if the name is unknown. */
const bd_syntax_lang *bd_syntax_builtin(const char *name);

/* Register a language under `name` plus a NULL-terminated list of filename
 * extensions (without the dot, e.g. {"lua", NULL}) so bd_syntax_for_name can
 * find it. The language is borrowed, not copied; it must outlive the registry.
 * Re-registering a name replaces it. */
void bd_syntax_register(const char *name, const char *const *exts,
                        const bd_syntax_lang *lang);

/* Autodetect a language from a filename's extension, among the built-ins and
 * anything registered. NULL if nothing matches. An app can ignore this and set
 * a language explicitly, or register its own detection via bd_syntax_register. */
const bd_syntax_lang *bd_syntax_for_name(const char *filename);

/* Tokenize buf[0,len) into `out` (up to max spans); returns the span count. */
int bd_syntax_run(const bd_syntax_lang *lang, const char *buf, int len,
                  bd_syntax_span *out, int max);

/* The language's name (as given by `name` in its definition). */
const char *bd_syntax_name(const bd_syntax_lang *lang);

#endif
