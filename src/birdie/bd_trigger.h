#ifndef BD_TRIGGER_H
#define BD_TRIGGER_H

/*
 * bd_trigger -- the trigger / alias engine (doc/triggers.md).
 *
 * Holds a table of triggers, each in a (dot-nestable) class that toggles as a
 * unit, and dispatches incoming lines, prompts, and GMCP packages plus
 * outgoing user input against them in priority order. A matched trigger runs
 * its body: MUD command text (with %0..%9 capture substitution) is emitted
 * through the send callback; a body beginning with '@' is a Lua expression run
 * on the bd_vm. This is the C engine the TinTin++-style verbs (#action,
 * #alias, ...) compile down to; nothing here is inaccessible from Lua.
 *
 * This increment covers action/alias/prompt/gmcp triggers, classes, priority,
 * and #stop. Deferred (doc/triggers.md): multi-state chains, the timer /
 * event / expression / mxp types, and the line-rewriting verbs #gag /
 * #highlight / #substitute.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_vm.h"

typedef enum bd_trigger_type {
	BD_TRIG_ACTION,   /* matches an incoming MUD line (#action) */
	BD_TRIG_ALIAS,    /* matches outgoing user input (#alias) */
	BD_TRIG_PROMPT,   /* matches a prompt line (EOR/GA-marked) */
	BD_TRIG_GMCP,     /* matches a GMCP package by name */
	/* line-rewriting types: applied to a line's display text, not "fired" */
	BD_TRIG_GAG,      /* #gag: drop the line from display */
	BD_TRIG_SUBST,    /* #substitute: replace matched text (body = replacement) */
	BD_TRIG_HILITE    /* #highlight: recolor matched text (body = color) */
} bd_trigger_type;

#define BD_TRIG_PRIO_DEFAULT 5

typedef struct bd_triggers bd_triggers;

/* The engine emits MUD commands through this; it does not own the socket. */
typedef void (*bd_trigger_send_fn)(const char *cmd, void *ctx);

/* Called when an interval timer fires, with the timer's name. Lets the host
 * dispatch the on.timer hook table in addition to the timer's own body. */
typedef void (*bd_trigger_timer_fn)(const char *name, void *ctx);

/* Create an engine. `vm` runs '@' Lua bodies (may be a null/recording VM);
 * `send` emits command bodies. Either may be NULL. */
bd_triggers *bd_triggers_new(bd_vm *vm, bd_trigger_send_fn send, void *ctx);
void bd_triggers_free(bd_triggers *t);

/*
 * Add a trigger. `pattern` is a TinTin++-style pattern for action/alias/prompt
 * (literal text with %1..%9 captures, optional ^ start / $ end anchors) or a
 * GMCP package name for BD_TRIG_GMCP. `body` is command text or, if it begins
 * with '@', a Lua expression; %0 (whole match) and %1..%9 (captures) are
 * substituted first. `class` NULL/"" means "default". `priority` < 0 uses the
 * default. `stop` != 0 halts matching for that input after this fires.
 * Returns a trigger id (>= 0), or -1 on error.
 */
int bd_trigger_add(bd_triggers *t, bd_trigger_type type, const char *pattern,
                   const char *body, const char *class, int priority, int stop);

/* Like bd_trigger_add, but the trigger joins a multi-state chain: it can only
 * fire while the chain is armed at `state` (1..N), and firing advances the
 * chain to the next state (wrapping to 1 past the highest state seen). The
 * chain is identified by `class`+`chain` and resets to state 1 on timeout (see
 * bd_triggers_set_now) or bd_trigger_reset. chain NULL/"" or state <= 0 means
 * an ordinary (always-armed) trigger. Returns the trigger id, or -1. */
int bd_trigger_add_chained(bd_triggers *t, bd_trigger_type type,
                           const char *pattern, const char *body,
                           const char *class, const char *chain, int state,
                           int priority, int stop);

/* Remove a trigger by id. */
void bd_trigger_remove(bd_triggers *t, int id);

/* Remove every trigger of `type` whose pattern equals `pattern` (exact text).
 * If `class` is non-NULL/"" only triggers in that class are removed. Returns
 * the number removed. Backs #unaction / #unalias. */
int bd_trigger_remove_pattern(bd_triggers *t, bd_trigger_type type,
                              const char *pattern, const char *class);

/* Number of triggers currently in the table. */
int bd_trigger_count(const bd_triggers *t);

/* Enumerate triggers in dispatch order (priority, then insertion). The
 * callback gets the type, pattern, body, class, chain key (NULL if none),
 * state (0 if none), priority, and the per-class enabled flag. Backs #list. */
typedef void (*bd_trigger_iter_fn)(bd_trigger_type type, const char *pattern,
                                   const char *body, const char *cls,
                                   const char *chain, int state, int priority,
                                   int enabled, void *ctx);
void bd_trigger_foreach(bd_triggers *t, bd_trigger_iter_fn fn, void *ctx);

/* The scripting VM the engine runs '@' bodies on (for #script and the
 * event-hook dispatch). May be NULL. */
bd_vm *bd_triggers_vm(bd_triggers *t);

/* ---- classes (dot-nested: disabling "combat" disables "combat.melee") ---- */

void bd_class_enable(bd_triggers *t, const char *name);
void bd_class_disable(bd_triggers *t, const char *name);
/* 1 if `name`'s class (and all its ancestors) are enabled. */
int  bd_class_enabled(bd_triggers *t, const char *name);

/* ---- dispatch (all return 1 if at least one trigger fired) ---- */

int bd_triggers_line(bd_triggers *t, const char *line);    /* BD_TRIG_ACTION */
int bd_triggers_prompt(bd_triggers *t, const char *text);  /* BD_TRIG_PROMPT */
int bd_triggers_gmcp(bd_triggers *t, const char *pkg, const char *json);

/* ---- line rewriting (#gag / #substitute / #highlight) ---- */

typedef struct bd_line_edit {
	int gag;                /* 1 -> drop the line from display */
	int changed;            /* 1 -> `text` differs from the input line */
	char text[4096];        /* rewritten display text (valid unless gagged) */
} bd_line_edit;

/* Apply the gag / substitute / highlight triggers (in priority order, class
 * gated) to `line`, filling `*e`. A gag wins immediately. Substitute replaces
 * all non-overlapping matches with its body (%0..%9 expanded); highlight wraps
 * each match in an ANSI color (its body is a color name or raw SGR params).
 * The input is the line's plain display text; highlight/substitute output is
 * plain text plus any color this layer adds (original server color on a
 * rewritten line is not preserved). */
void bd_triggers_rewrite(bd_triggers *t, const char *line, bd_line_edit *e);

/*
 * Run outgoing user input through the aliases. Returns 1 if an alias fired
 * (the caller should NOT send the original input -- the alias body emitted
 * whatever should go instead), 0 if no alias matched (send the input as-is).
 */
int bd_triggers_input(bd_triggers *t, const char *cmd);

/* ---- interval timers (#tick) ---- */

/* Add or replace a repeating timer named `name`: fire `body` (command text, or
 * '@' Lua) every `seconds`. `class` gates it like other triggers (NULL/"" ->
 * "default"). seconds <= 0 is rejected. Returns 0 on success, -1 on error. */
int bd_trigger_add_tick(bd_triggers *t, const char *name, const char *body,
                        double seconds, const char *class);

/* Remove a timer by name. */
void bd_trigger_remove_tick(bd_triggers *t, const char *name);

/* Register a callback fired (on the same thread as run_timers) for each timer
 * that elapses, after its body runs. Used to drive the on.timer hook table. */
void bd_triggers_set_timer_cb(bd_triggers *t, bd_trigger_timer_fn fn, void *ctx);

/* Fire any timers whose deadline has passed at monotonic time `now_ms`. Call
 * regularly (e.g. once per frame from bd_session_drain). A timer reschedules
 * from now, so a stalled frame fires it once, not a catch-up burst. Returns
 * the number of timers fired. */
int bd_triggers_run_timers(bd_triggers *t, double now_ms);

/* ---- multi-state chains ---- */

/* Tell the engine the current monotonic time (ms), used to time out stalled
 * chains. Call once per frame before dispatch. */
void bd_triggers_set_now(bd_triggers *t, double now_ms);

/* Reset chains to state 1. `chain` matches the full key "class/chain" or just
 * the chain name; NULL/"" resets every chain. */
void bd_trigger_reset(bd_triggers *t, const char *chain);

#endif /* BD_TRIGGER_H */
