#ifndef BD_VERB_H
#define BD_VERB_H

#include "bd_trigger.h"
#include <stddef.h>

/*
 * bd_verb -- the TinTin++-style command-line verb parser (doc/triggers.md).
 *
 * Turns a line of user input that begins with the command character ('#')
 * into trigger-engine operations: it is the thin convenience layer over the
 * bd_trigger API, parsing brace-grouped arguments and dispatching to the
 * engine. Anything the verbs do is reachable through bd_trigger directly.
 *
 * Implemented this increment: #action, #alias, #class. The line-rewriting
 * verbs (#substitute / #gag / #highlight), the timer verb (#tick), the
 * remove/list verbs (#unaction / #list), and a configurable command char are
 * deferred (doc/triggers.md).
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/*
 * If `input` begins with '#', parse it as a verb and apply it to the engine,
 * returning 1 (handled). A short human-readable result is written to
 * `feedback` (e.g. "action added", or an error) when feedback != NULL.
 * Returns 0 if `input` is not a verb (no leading '#'); the caller then treats
 * it as ordinary input (alias / send). "##text" is an escape for a literal
 * leading '#': it is not a verb (returns 0) and `*literal` points past the
 * first '#' so the caller sends "#text".
 */
int bd_verb_exec(bd_triggers *t, const char *input,
                 const char **literal, char *feedback, size_t fbcap);

#endif /* BD_VERB_H */
