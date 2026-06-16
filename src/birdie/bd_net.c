/*
 * bd_net -- birdie's network connection layer.
 *
 * A dedicated network thread runs a libiox poll loop and owns the socket:
 * resolution (getaddrinfo), connect, recv/send, and telnet IAC filtering all
 * happen there, so the UI thread never blocks on the network. Two lock-free
 * SPSC rings carry framed messages between the threads:
 *
 *   tx (UI -> net):  CONNECT / SEND / CLOSE commands
 *   rx (net -> UI):  DATA (clean bytes) / STATE (transition) records
 *
 * The UI wakes the net thread by writing one byte to a self-pipe the loop
 * watches. bd_net_poll() drains rx on the UI thread and fires the data/state
 * callbacks there, so callers still receive everything on the UI thread.
 *
 * Plain TCP for now. Deferred (doc/network.md): TLS (mbedTLS), the MTH telopt
 * set (CHARSET/ECHO/EOR/GMCP/MCCP/MSDP), Happy Eyeballs, reconnect, encoding
 * transcode, NDJSON logging, and a Winsock path.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_net.h"
#include "bd_ring.h"
#include "iox_loop.h"
#include "iox_fd.h"

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* telnet protocol bytes (RFC 854 + friends) */
#define TN_SE   240
#define TN_GA   249
#define TN_SB   250
#define TN_WILL 251
#define TN_WONT 252
#define TN_DO   253
#define TN_DONT 254
#define TN_IAC  255

/* cross-thread message types (one byte, framed as [type][u32 len][payload]) */
enum {
	MSG_CONNECT,    /* tx: payload = host '\0' port '\0' */
	MSG_SEND,       /* tx: payload = bytes to transmit */
	MSG_CLOSE,      /* tx: no payload */
	MSG_DATA,       /* rx: payload = clean received bytes */
	MSG_STATE       /* rx: payload = [u8 state][detail string, no NUL] */
};

#define RX_CAP   (1u << 20)     /* 1 MiB inbound buffer */
#define TX_CAP   (1u << 16)     /* 64 KiB command buffer */
#define CHUNK    4096           /* recv size and max per-frame payload */

/* inbound telnet parser states */
enum tn_state {
	TS_DATA, TS_IAC, TS_OPT, TS_SB, TS_SB_IAC
};

struct bd_net {
	bd_net_data_cb on_data;
	bd_net_state_cb on_state;
	void *arg;

	_Atomic int state;      /* bd_net_state; written by net thread */
	_Atomic int shutdown;   /* set by bd_net_free to stop the thread */

	bd_ring *rx;            /* net -> UI */
	bd_ring *tx;            /* UI -> net */
	int wake_r, wake_w;     /* self-pipe; UI writes wake_w, net reads wake_r */

	pthread_t thread;
	int have_thread;
};

/* Net-thread-only working state. */
struct net_ctx {
	bd_net *n;
	struct iox_loop *loop;
	int fd;                 /* socket, -1 when none */
	int connecting;         /* awaiting non-blocking connect completion */
	unsigned want;          /* iox events currently registered on fd */
	int rx_blocked;         /* read disabled: rx ring was full */
	int quit;

	enum tn_state ts;
	unsigned char tn_cmd;

	unsigned char *out;     /* bytes owed to the socket */
	size_t out_len, out_cap, out_sent;
};

/* ---- framed-message helpers over a ring -------------------------------- */

static int
ring_put(bd_ring *r, uint8_t type, const void *payload, uint32_t len)
{
	unsigned char hdr[5];
	hdr[0] = type;
	memcpy(hdr + 1, &len, sizeof len);
	return bd_ring_writev(r, hdr, sizeof hdr, payload, len);
}

/* Read one whole frame if present. Returns 1 and fills out and len (truncated
 * to cap), or 0 if no complete frame is buffered. */
static int
ring_get(bd_ring *r, uint8_t *type, unsigned char *buf, uint32_t cap,
         uint32_t *len)
{
	unsigned char hdr[5];
	uint32_t l, n;

	if (bd_ring_read_avail(r) < sizeof hdr)
		return 0;
	bd_ring_peek(r, hdr, sizeof hdr);
	memcpy(&l, hdr + 1, sizeof l);
	if (bd_ring_read_avail(r) < sizeof hdr + (size_t)l)
		return 0;       /* frames publish atomically, so this is rare */

	bd_ring_skip(r, sizeof hdr);
	*type = hdr[0];
	n = l < cap ? l : cap;
	bd_ring_read(r, buf, n);
	if (l > n)
		bd_ring_skip(r, l - n);
	*len = n;
	return 1;
}

static void
wake(bd_net *n)
{
	unsigned char b = 1;
	ssize_t rc = write(n->wake_w, &b, 1);
	(void)rc;       /* EAGAIN just means a wake is already pending */
}

/* ---- net thread: socket + telnet ------------------------------------- */

static void
push_state(struct net_ctx *c, bd_net_state st, const char *msg)
{
	unsigned char frame[1 + 200];
	uint32_t len = 1;
	frame[0] = (unsigned char)st;
	if (msg) {
		size_t m = strlen(msg);
		if (m > sizeof frame - 1)
			m = sizeof frame - 1;
		memcpy(frame + 1, msg, m);
		len += (uint32_t)m;
	}
	atomic_store(&c->n->state, (int)st);
	ring_put(c->n->rx, MSG_STATE, frame, len);
}

static int
out_push(struct net_ctx *c, const unsigned char *p, size_t len)
{
	if (c->out_sent == c->out_len)
		c->out_sent = c->out_len = 0;
	if (c->out_len + len > c->out_cap) {
		size_t cap = c->out_cap ? c->out_cap * 2 : 256;
		unsigned char *nb;
		while (cap < c->out_len + len)
			cap *= 2;
		nb = realloc(c->out, cap);
		if (!nb)
			return -1;
		c->out = nb;
		c->out_cap = cap;
	}
	memcpy(c->out + c->out_len, p, len);
	c->out_len += len;
	return 0;
}

static void
tn_reply(struct net_ctx *c, unsigned char cmd, unsigned char opt)
{
	unsigned char r[3] = { TN_IAC, cmd, opt };
	out_push(c, r, sizeof r);
}

/* Filter raw socket bytes: strip/answer telnet negotiation, push the clean
 * remainder to the UI as one DATA frame. Caller guarantees rx has room. */
static void
tn_feed(struct net_ctx *c, const unsigned char *p, int len)
{
	unsigned char clean[CHUNK];
	int cn = 0, i;

	for (i = 0; i < len; i++) {
		unsigned char b = p[i];
		switch (c->ts) {
		case TS_DATA:
			if (b == TN_IAC)
				c->ts = TS_IAC;
			else
				clean[cn++] = b;
			break;
		case TS_IAC:
			if (b == TN_IAC) {
				clean[cn++] = TN_IAC;
				c->ts = TS_DATA;
			} else if (b == TN_WILL || b == TN_WONT ||
			           b == TN_DO || b == TN_DONT) {
				c->tn_cmd = b;
				c->ts = TS_OPT;
			} else if (b == TN_SB) {
				c->ts = TS_SB;
			} else {
				c->ts = TS_DATA;        /* GA/EOR/NOP/... */
			}
			break;
		case TS_OPT:
			if (c->tn_cmd == TN_DO)
				tn_reply(c, TN_WONT, b);
			else if (c->tn_cmd == TN_WILL)
				tn_reply(c, TN_DONT, b);
			c->ts = TS_DATA;
			break;
		case TS_SB:
			if (b == TN_IAC)
				c->ts = TS_SB_IAC;
			break;
		case TS_SB_IAC:
			if (b == TN_SE)
				c->ts = TS_DATA;
			else
				c->ts = TS_SB;
			break;
		}
	}
	if (cn)
		ring_put(c->n->rx, MSG_DATA, clean, (uint32_t)cn);
}

static int
set_nonblock(int fd)
{
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl < 0)
		return -1;
	return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void
update_interest(struct net_ctx *c)
{
	unsigned ev = 0;

	if (c->fd < 0)
		return;
	if (c->connecting) {
		ev = IOX_WRITE;
	} else {
		if (!c->rx_blocked)
			ev |= IOX_READ;
		if (c->out_sent < c->out_len)
			ev |= IOX_WRITE;
	}
	if (ev != c->want) {
		iox_fd_mod(c->loop, c->fd, ev);
		c->want = ev;
	}
}

static void
close_socket(struct net_ctx *c)
{
	if (c->fd >= 0) {
		iox_fd_remove(c->loop, c->fd);
		close(c->fd);
		c->fd = -1;
	}
	c->connecting = 0;
	c->want = 0;
	c->rx_blocked = 0;
	c->ts = TS_DATA;
	c->out_len = c->out_sent = 0;
}

static void sock_cb(struct iox_loop *loop, int fd, unsigned events, void *arg);

static void
do_connect(struct net_ctx *c, const unsigned char *payload, uint32_t len)
{
	struct addrinfo hints, *res, *ai;
	const char *host, *port;
	size_t hl;
	int rc;

	close_socket(c);

	/* payload is host '\0' port '\0' */
	host = (const char *)payload;
	hl = strnlen(host, len);
	if (hl >= len) {
		push_state(c, BD_NET_ERROR, "bad connect target");
		return;
	}
	port = host + hl + 1;

	push_state(c, BD_NET_CONNECTING, NULL);

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	rc = getaddrinfo(host, port, &hints, &res);   /* blocking; net thread */
	if (rc != 0) {
		push_state(c, BD_NET_ERROR, gai_strerror(rc));
		return;
	}

	for (ai = res; ai; ai = ai->ai_next) {
		int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (set_nonblock(fd) < 0) {
			close(fd);
			continue;
		}
		rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
		if (rc == 0) {
			c->fd = fd;
			c->connecting = 0;
			iox_fd_add(c->loop, fd, IOX_READ, sock_cb, c);
			c->want = IOX_READ;
			freeaddrinfo(res);
			push_state(c, BD_NET_CONNECTED, NULL);
			return;
		}
		if (rc < 0 && errno == EINPROGRESS) {
			c->fd = fd;
			c->connecting = 1;
			iox_fd_add(c->loop, fd, IOX_WRITE, sock_cb, c);
			c->want = IOX_WRITE;
			freeaddrinfo(res);
			return;         /* CONNECTED/ERROR follows in sock_cb */
		}
		close(fd);
	}
	freeaddrinfo(res);
	push_state(c, BD_NET_ERROR, "could not connect");
}

static void
do_send(struct net_ctx *c, const unsigned char *p, uint32_t len)
{
	uint32_t i, start;

	if (c->fd < 0 || c->connecting)
		return;         /* not connected; drop */

	/* escape 0xFF -> IAC IAC so payload is not read as telnet commands */
	for (i = 0, start = 0; i < len; i++) {
		if (p[i] == TN_IAC) {
			unsigned char esc[2] = { TN_IAC, TN_IAC };
			if (i > start)
				out_push(c, p + start, i - start);
			out_push(c, esc, 2);
			start = i + 1;
		}
	}
	if (len > start)
		out_push(c, p + start, len - start);
	update_interest(c);
}

static void
flush_out(struct net_ctx *c)
{
	while (c->out_sent < c->out_len) {
		ssize_t w = send(c->fd, c->out + c->out_sent,
		    c->out_len - c->out_sent, 0);
		if (w > 0) {
			c->out_sent += (size_t)w;
			continue;
		}
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		if (w < 0 && errno == EINTR)
			continue;
		close_socket(c);
		push_state(c, BD_NET_ERROR, strerror(errno));
		return;
	}
	if (c->out_sent == c->out_len)
		c->out_sent = c->out_len = 0;
}

static void
do_read(struct net_ctx *c)
{
	for (;;) {
		unsigned char buf[CHUNK];
		size_t room = bd_ring_write_avail(c->n->rx);
		size_t want;
		ssize_t r;

		if (room <= sizeof(uint8_t) + sizeof(uint32_t)) {
			c->rx_blocked = 1;      /* no room for even a 1-byte frame */
			return;
		}
		want = room - 5;
		if (want > sizeof buf)
			want = sizeof buf;

		r = recv(c->fd, buf, want, 0);
		if (r > 0) {
			tn_feed(c, buf, (int)r);
			continue;
		}
		if (r == 0) {
			close_socket(c);
			push_state(c, BD_NET_CLOSED,
			    "connection closed by peer");
			return;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		if (errno == EINTR)
			continue;
		close_socket(c);
		push_state(c, BD_NET_ERROR, strerror(errno));
		return;
	}
}

static void
sock_cb(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
	struct net_ctx *c = arg;
	(void)loop;
	(void)fd;

	if (c->connecting) {
		int err = 0;
		socklen_t el = sizeof err;
		if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) {
			close_socket(c);
			push_state(c, BD_NET_ERROR,
			    err ? strerror(err) : "connect failed");
			return;
		}
		c->connecting = 0;
		update_interest(c);
		push_state(c, BD_NET_CONNECTED, NULL);
		return;
	}

	if (events & IOX_WRITE)
		flush_out(c);
	if (c->fd >= 0 && (events & IOX_READ))
		do_read(c);
	if (c->fd >= 0)
		update_interest(c);
}

/* Drain the wake pipe, apply queued commands, and relieve read backpressure. */
static void
wake_cb(struct iox_loop *loop, int fd, unsigned events, void *arg)
{
	struct net_ctx *c = arg;
	unsigned char drain[64];
	uint8_t type;
	unsigned char buf[CHUNK];
	uint32_t len;
	(void)loop;
	(void)events;

	while (read(fd, drain, sizeof drain) > 0)
		;

	while (ring_get(c->n->tx, &type, buf, sizeof buf, &len)) {
		switch (type) {
		case MSG_CONNECT:
			do_connect(c, buf, len);
			break;
		case MSG_SEND:
			do_send(c, buf, len);
			break;
		case MSG_CLOSE:
			if (c->fd >= 0) {
				close_socket(c);
				push_state(c, BD_NET_CLOSED, NULL);
			}
			break;
		}
	}

	if (atomic_load(&c->n->shutdown)) {
		c->quit = 1;
		return;
	}

	/* The UI may have drained rx; re-enable reads if we had paused them. */
	if (c->rx_blocked &&
	    bd_ring_write_avail(c->n->rx) > sizeof(uint8_t) + sizeof(uint32_t)) {
		c->rx_blocked = 0;
		update_interest(c);
	}
}

static void *
net_thread_main(void *arg)
{
	bd_net *n = arg;
	struct net_ctx c;

	memset(&c, 0, sizeof c);
	c.n = n;
	c.fd = -1;
	c.ts = TS_DATA;

	c.loop = iox_loop_new();
	if (!c.loop)
		return NULL;
	iox_fd_add(c.loop, n->wake_r, IOX_READ, wake_cb, &c);
	iox_loop_start(c.loop);

	while (!c.quit) {
		if (iox_loop_poll(c.loop) < 0 && errno != EINTR)
			break;
	}

	if (c.fd >= 0)
		close(c.fd);
	free(c.out);
	iox_loop_free(c.loop);
	return NULL;
}

/* ---- UI thread: public API ------------------------------------------- */

static int
make_pipe(int *rfd, int *wfd)
{
	int fds[2];
	if (pipe(fds) < 0)
		return -1;
	set_nonblock(fds[0]);
	set_nonblock(fds[1]);
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);
	*rfd = fds[0];
	*wfd = fds[1];
	return 0;
}

bd_net *
bd_net_new(bd_net_data_cb on_data, bd_net_state_cb on_state, void *arg)
{
	bd_net *n = calloc(1, sizeof *n);
	if (!n)
		return NULL;
	n->on_data = on_data;
	n->on_state = on_state;
	n->arg = arg;
	n->wake_r = n->wake_w = -1;
	atomic_init(&n->state, BD_NET_IDLE);
	atomic_init(&n->shutdown, 0);

	n->rx = bd_ring_new(RX_CAP);
	n->tx = bd_ring_new(TX_CAP);
	if (!n->rx || !n->tx || make_pipe(&n->wake_r, &n->wake_w) < 0)
		goto fail;
	if (pthread_create(&n->thread, NULL, net_thread_main, n) != 0)
		goto fail;
	n->have_thread = 1;
	return n;

fail:
	if (n->wake_r >= 0)
		close(n->wake_r);
	if (n->wake_w >= 0)
		close(n->wake_w);
	bd_ring_free(n->rx);
	bd_ring_free(n->tx);
	free(n);
	return NULL;
}

void
bd_net_free(bd_net *n)
{
	if (!n)
		return;
	if (n->have_thread) {
		atomic_store(&n->shutdown, 1);
		wake(n);
		pthread_join(n->thread, NULL);
	}
	if (n->wake_r >= 0)
		close(n->wake_r);
	if (n->wake_w >= 0)
		close(n->wake_w);
	bd_ring_free(n->rx);
	bd_ring_free(n->tx);
	free(n);
}

int
bd_net_connect(bd_net *n, const char *host, const char *port)
{
	unsigned char payload[512];
	size_t hl, pl;

	if (!n || !host || !port)
		return -1;
	hl = strlen(host);
	pl = strlen(port);
	if (hl + pl + 2 > sizeof payload)
		return -1;
	memcpy(payload, host, hl + 1);
	memcpy(payload + hl + 1, port, pl + 1);

	if (ring_put(n->tx, MSG_CONNECT, payload, (uint32_t)(hl + pl + 2)) < 0)
		return -1;
	wake(n);
	return 0;
}

int
bd_net_send(bd_net *n, const void *data, int len)
{
	const unsigned char *p = data;
	int left = len;

	if (!n || len < 0)
		return -1;
	if (atomic_load(&n->state) != BD_NET_CONNECTED)
		return -1;

	while (left > 0) {
		uint32_t chunk = left > CHUNK ? CHUNK : (uint32_t)left;
		if (ring_put(n->tx, MSG_SEND, p, chunk) < 0) {
			wake(n);
			return len - left;      /* tx full: partial accept */
		}
		p += chunk;
		left -= (int)chunk;
	}
	wake(n);
	return len;
}

void
bd_net_close(bd_net *n)
{
	if (!n)
		return;
	if (ring_put(n->tx, MSG_CLOSE, NULL, 0) == 0)
		wake(n);
}

void
bd_net_poll(bd_net *n)
{
	uint8_t type;
	unsigned char buf[CHUNK];
	uint32_t len;
	int got_data = 0;

	if (!n)
		return;

	while (ring_get(n->rx, &type, buf, sizeof buf, &len)) {
		if (type == MSG_DATA) {
			if (n->on_data)
				n->on_data((const char *)buf, (int)len, n->arg);
			got_data = 1;
		} else if (type == MSG_STATE) {
			bd_net_state st = (bd_net_state)buf[0];
			char detail[201];
			const char *msg = NULL;
			if (len > 1) {
				uint32_t m = len - 1;
				if (m > sizeof detail - 1)
					m = sizeof detail - 1;
				memcpy(detail, buf + 1, m);
				detail[m] = '\0';
				msg = detail;
			}
			if (n->on_state)
				n->on_state(st, msg, n->arg);
		}
	}

	if (got_data)
		wake(n);        /* nudge the net thread to lift backpressure */
}

bd_net_state
bd_net_state_get(const bd_net *n)
{
	if (!n)
		return BD_NET_IDLE;
	return (bd_net_state)atomic_load((_Atomic int *)&n->state);
}
