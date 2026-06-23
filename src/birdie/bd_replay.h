#ifndef BD_REPLAY_H
#define BD_REPLAY_H

#include <stddef.h>

/*
 * bd_replay -- scrollback replay from the NDJSON logs (doc/logging.md,
 * doc/network.md).
 *
 * NDJSON is the source of truth and keeps each inbound line's `raw` bytes with
 * ANSI intact, so the last N lines of history can be rendered identically to
 * live output by re-feeding `raw` from `recv` records through the terminal.
 * This backs "show last N lines on reconnect" and a future scrollback viewer.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* Called once per replayed line, oldest-first, with the unescaped raw bytes
 * (NUL-terminated; `len` excludes the NUL). */
typedef void (*bd_replay_emit_fn)(const char *raw, size_t len, void *ctx);

/*
 * Emit up to `max` most-recent `recv` lines logged under `root` for the
 * (mud, character) pair, oldest-first, via `emit`. The bucket layout matches
 * bd_log's default path template
 * (`<root>/<year>/<mud>/<character>/<...>.ndjson`); NULL/"" mud or character
 * map to "_" as the writer does. Returns the number emitted, or -1 on error.
 */
int bd_replay_recv(const char *root, const char *mud, const char *character,
                   int max, bd_replay_emit_fn emit, void *ctx);

/*
 * Extract the string value of top-level field `key` from one NDJSON object
 * `line` into out[cap], JSON-unescaping it. Returns 1 if found, 0 otherwise.
 * Exposed for tests.
 */
int bd_replay_json_str(const char *line, const char *key, char *out,
                       size_t cap);

#endif /* BD_REPLAY_H */
