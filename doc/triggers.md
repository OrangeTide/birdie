# Triggers

Design for Birdie's trigger / alias / timer system. Combines TinTin++'s verb
vocabulary with ZMud's organizing model, with Lua as the scripting substrate.

## Influences

- **TinTin++** — separate verbs: `#action`, `#alias`, `#substitute`, `#gag`,
  `#highlight`, `#tick`. Each verb does one thing; the grammar is small and
  learnable.
- **ZMud / CMud** — nestable **classes** (groups) that toggle as a unit,
  **multi-state triggers** (state N arms only after state N-1 fires), and a
  uniform trigger table covering pattern / prompt / MXP / GMCP / timer / event
  / expression types.
- **Mudlet** — trigger chains are the same idea as ZMud states, reinforcing
  that this feature is non-optional for combat scripting.

## Surface syntax

Use TinTin++-style verbs for the common cases so existing MUDers feel at home:

    #action {<pattern>} {<body>} [class]
    #alias  {<pattern>} {<body>} [class]
    #substitute {<pattern>} {<replacement>} [class]
    #gag    {<pattern>} [class]
    #highlight {<pattern>} {<color>} [class]
    #tick   {<name>} {<body>} <seconds> [class]

`<body>` can be either MUD command text (with `%1..%9` captures) or a Lua
expression prefixed with `@`:

    #action {You are hungry} {@eat_something()} combat

## Organizing model: classes

Every binding lives in a **class** (default: `default`). Classes are
dot-nestable and toggle as a unit:

    #class combat.melee on
    #class combat.melee off
    #class shopping off

This is the single most important feature to preserve from ZMud. It is what
lets users keep a library of reusable script modules without them interfering
with each other.

Classes are persisted per-MUD — each profile has its own class tree and
enabled-set.

## Multi-state triggers (chains)

A trigger may declare named states; only state `1` is armed initially, and
firing state N arms state N+1.

    #action {You begin casting (.+)} {...} combat/cast:1
    #action {Your spell fizzles}     {...} combat/cast:2
    #action {Your spell succeeds}    {...} combat/cast:2

States reset on timeout (configurable per chain) or on explicit `#reset`.
This is how combat scripts, quest walkthroughs, and multi-line parse work.

## Trigger types

One underlying table, one `type` field. The dispatcher routes by type:

| type         | fires on                                   |
|--------------|--------------------------------------------|
| `line`       | a line of MUD output (default)             |
| `prompt`     | text identified as a prompt                |
| `mxp`        | an MXP tag                                 |
| `gmcp`       | a GMCP message (matched by package.name)   |
| `timer`      | wall-clock interval or one-shot delay      |
| `event`      | user-fired event (`#event name args...`)   |
| `expression` | a watched Lua expression changes value     |

All types share class membership, enable/disable, priority, and state.

## Priority

Within a class, triggers fire in **priority order, highest first**. Default
priority is 5 on a 0–9 scale. Priority only orders within a class; across
classes the order is class-load order (stable, user-visible in `#list`).

A trigger may declare `#action ... {... #stop}` to halt further matching for
that line.

## When to drop to Lua

The `@`-prefixed body is the escape hatch. Full Lua is also available via:

    #script {<lua source>}

Lua has first-class access to:

- `mud.send(text)`, `mud.sendraw(bytes)`
- `mud.gmcp(package, data)` — send GMCP
- `on.line`, `on.prompt`, `on.gmcp[pkg]`, `on.timer`, `on.connect`,
  `on.disconnect`, `on.mxp` — hook tables (append a function, it runs in
  addition to declared triggers)
- `class.enable(name)`, `class.disable(name)`, `class.toggle(name)`
- `log.note(text)` — write an annotation into the NDJSON log (see
  `logging.md`)
- `var` — persistent per-profile variable table

The rule of thumb: **use `#action` for one-liners, use Lua for anything with
state, control flow, or data structures**. The trigger verbs are a
convenience layer over the Lua hook tables; nothing you can do in the verbs
is inaccessible from Lua.

## Scripting engine: Lua 5.4 + LPeg

**Lua 5.4 is the scripting language. LPeg is the primary pattern-matching
library.** Lua patterns are available for quick one-liners; LPeg is the
expected tool for anything non-trivial. PCRE is not shipped — add it in a
fork if you want it.

Reasons this was chosen over alternatives (notably Wren):

- **Pattern matching is the hot path.** MUD scripting is mostly text
  matching against MUD output. LPeg is the gold standard for this and
  has no real equivalent in other embeddable languages.
- **User-script portability.** Mudlet, MUSHclient, TinTin++, TinyFugue
  all speak Lua. Trigger snippets from wikis, forums, and other clients
  drop in with minimal edits.
- **Active ecosystem.** Lua ships everywhere, has LuaJIT when speed
  matters, and is still actively developed.
- **Fits the workload.** MUD scripts are "define handler for event" —
  closures over tables. Lua matches that grain; class-first OO adds
  ceremony without value here.

## VM abstraction: `bd_vm`

The trigger engine, log-note hook, and widget event routing do **not**
talk to `lua_State` directly. They talk to an opaque `bd_vm` handle:

    bd_vm *bd_vm_new(void);
    int    bd_vm_eval(bd_vm *, const char *source);
    int    bd_vm_call(bd_vm *, const char *name, /* args */ ...);
    void   bd_vm_register(bd_vm *, const char *name, bd_host_fn fn);
    void   bd_vm_free(bd_vm *);

This is the same separation-of-concerns seam we maintain between the core
and the GUI (`doc/gui.md`) and between the core and the log formatter
(`doc/logging.md`). The v1.0 backend is Lua 5.4 + LPeg. A null backend
exists for "scripting disabled" builds (constrained targets,
accessibility mode). A recording backend exists for tests. A fork can
swap in another language by implementing `bd_vm` without touching the
trigger engine.

## Open questions

- Do triggers run in the network thread or the UI thread? Leaning UI thread
  for simplicity; network thread only parses and enqueues lines. Matches
  the decision in `doc/gui.md`.
- Rate limiting / recursion cap for trigger cascades. Some ceiling is
  mandatory to prevent script loops from hanging the client.
- Sandboxing posture for imported scripts (untrusted gist URLs etc.):
  default to restricted (`os.execute` stripped, `io` limited to a
  per-profile scratch dir). Opt-in to full standard library.
