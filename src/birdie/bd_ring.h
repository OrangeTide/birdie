#ifndef BD_RING_H
#define BD_RING_H

#include <stddef.h>

/*
 * bd_ring -- a lock-free single-producer / single-consumer byte ring.
 *
 * One thread writes, one thread reads; no locks. Used by bd_net to hand a
 * decoded byte stream from the network thread to the UI thread and to hand
 * commands back the other way. Writes are all-or-nothing so a reader never
 * observes a half-written record. Safe concurrency requires exactly one
 * producer thread and one consumer thread.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

typedef struct bd_ring bd_ring;

/* Create a ring with at least cap bytes of usable capacity (rounded up to a
 * power of two). Returns NULL on failure. */
bd_ring *bd_ring_new(size_t cap);
void bd_ring_free(bd_ring *r);

/* --- producer side --- */

/* Bytes that can currently be written. */
size_t bd_ring_write_avail(const bd_ring *r);

/* Write a + b as one atomic unit (b may be NULL/0). All-or-nothing: returns 0
 * on success, -1 if the combined length does not fit. Publishing happens once,
 * after both segments are in place, so the consumer sees the whole record or
 * none of it. */
int bd_ring_writev(bd_ring *r, const void *a, size_t alen,
                   const void *b, size_t blen);

/* Convenience: single-segment all-or-nothing write. */
int bd_ring_write(bd_ring *r, const void *data, size_t len);

/* --- consumer side --- */

/* Bytes that can currently be read. */
size_t bd_ring_read_avail(const bd_ring *r);

/* Copy up to len bytes without consuming them. Returns bytes copied. */
size_t bd_ring_peek(const bd_ring *r, void *out, size_t len);

/* Discard up to len bytes. Returns bytes discarded. */
size_t bd_ring_skip(bd_ring *r, size_t len);

/* Copy and consume up to len bytes. Returns bytes copied. */
size_t bd_ring_read(bd_ring *r, void *out, size_t len);

#endif /* BD_RING_H */
