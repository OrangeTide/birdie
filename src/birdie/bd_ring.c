/*
 * bd_ring -- lock-free SPSC byte ring. See bd_ring.h for the contract.
 *
 * head and tail are free-running counters (never reset); the buffer index is
 * counter & mask. The difference tail - head is the fill level and stays
 * correct under unsigned wraparound, so the counters need no special handling
 * even if they eventually wrap. The producer owns tail, the consumer owns
 * head; each publishes its own index with release and reads the other's with
 * acquire, which is the standard SPSC fence pairing.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_ring.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct bd_ring {
	unsigned char *buf;
	size_t mask;            /* cap - 1; cap is a power of two */
	_Atomic size_t head;    /* consumer cursor */
	_Atomic size_t tail;    /* producer cursor */
};

static size_t
round_up_pow2(size_t n)
{
	size_t p = 1;
	while (p < n)
		p <<= 1;
	return p;
}

bd_ring *
bd_ring_new(size_t cap)
{
	bd_ring *r = calloc(1, sizeof *r);
	if (!r)
		return NULL;
	cap = round_up_pow2(cap ? cap : 1);
	r->buf = malloc(cap);
	if (!r->buf) {
		free(r);
		return NULL;
	}
	r->mask = cap - 1;
	atomic_init(&r->head, 0);
	atomic_init(&r->tail, 0);
	return r;
}

void
bd_ring_free(bd_ring *r)
{
	if (!r)
		return;
	free(r->buf);
	free(r);
}

size_t
bd_ring_write_avail(const bd_ring *r)
{
	size_t cap = r->mask + 1;
	size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
	size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
	return cap - (tail - head);
}

/* Copy len bytes into the buffer starting at counter position pos (wrapping). */
static void
put_at(bd_ring *r, size_t pos, const unsigned char *src, size_t len)
{
	size_t off = pos & r->mask;
	size_t first = (r->mask + 1) - off;
	if (first > len)
		first = len;
	memcpy(r->buf + off, src, first);
	if (len > first)
		memcpy(r->buf, src + first, len - first);
}

int
bd_ring_writev(bd_ring *r, const void *a, size_t alen,
               const void *b, size_t blen)
{
	size_t cap = r->mask + 1;
	size_t total = alen + blen;
	size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
	size_t head = atomic_load_explicit(&r->head, memory_order_acquire);

	if (total > cap - (tail - head))
		return -1;      /* would not fit as a whole */

	if (alen)
		put_at(r, tail, a, alen);
	if (blen)
		put_at(r, tail + alen, b, blen);

	/* Publish only after both segments are in place. */
	atomic_store_explicit(&r->tail, tail + total, memory_order_release);
	return 0;
}

int
bd_ring_write(bd_ring *r, const void *data, size_t len)
{
	return bd_ring_writev(r, data, len, NULL, 0);
}

size_t
bd_ring_read_avail(const bd_ring *r)
{
	size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
	size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
	return tail - head;
}

size_t
bd_ring_peek(const bd_ring *r, void *out, size_t len)
{
	size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
	size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
	size_t avail = tail - head;
	size_t off, first;

	if (len > avail)
		len = avail;
	off = head & r->mask;
	first = (r->mask + 1) - off;
	if (first > len)
		first = len;
	memcpy(out, r->buf + off, first);
	if (len > first)
		memcpy((unsigned char *)out + first, r->buf, len - first);
	return len;
}

size_t
bd_ring_skip(bd_ring *r, size_t len)
{
	size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
	size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
	size_t avail = tail - head;

	if (len > avail)
		len = avail;
	atomic_store_explicit(&r->head, head + len, memory_order_release);
	return len;
}

size_t
bd_ring_read(bd_ring *r, void *out, size_t len)
{
	len = bd_ring_peek(r, out, len);
	bd_ring_skip(r, len);
	return len;
}
