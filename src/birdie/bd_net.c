/*
 * bd_net -- birdie's network connection layer.
 *
 * A single TCP/telnet connection to a MUD. The socket lives on a dedicated
 * network thread (a libiox poll loop); resolution, connect, recv/send, and
 * telnet option negotiation (bd_telopt) all happen there so the UI thread
 * never blocks on the network. Two lock-free SPSC rings carry framed messages:
 *
 *   tx (UI -> net):  CONNECT / SEND / CLOSE / WINSIZE / TERMTYPE
 *   rx (net -> UI):  DATA / STATE / PROMPT / ECHO
 *
 * The UI wakes the net thread with a self-pipe byte; bd_net_poll() drains rx
 * and fires the callbacks, all on the UI thread.
 *
 * Inbound bytes go through bd_telopt, whose output (clean data, plus prompt
 * and echo events) is framed into a net-thread staging buffer and streamed
 * into the rx ring. Streaming, rather than one ring_put per record, keeps
 * backpressure exact even when one recv yields several records: the ring's
 * framed reader already waits for a record to arrive in full.
 *
 * Deferred (doc/network.md): TLS (mbedTLS), the MTH extension set
 * (GMCP/MSDP/MSSP/MCCP), Happy Eyeballs, reconnect, and a Winsock path.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "bd_net.h"
#include "bd_ring.h"
#include "bd_telopt.h"
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

#define TN_IAC  255

/* tx message types (UI -> net) */
enum {
	MSG_CONNECT,    /* host '\0' port '\0' */
	MSG_SEND,       /* bytes to transmit */
	MSG_CLOSE,
	MSG_WINSIZE,    /* u16 cols, u16 rows (native order) */
	MSG_TERMTYPE    /* terminal type string */
};

/* rx message types (net -> UI) */
enum {
	MSG_DATA,       /* clean received bytes */
	MSG_STATE,      /* [u8 state][detail string] */
	MSG_PROMPT,     /* no payload */
	MSG_ECHO        /* [u8 suppress] */
};

#define RX_CAP   (1u << 20)     /* 1 MiB inbound buffer */
#define TX_CAP   (1u << 16)     /* 64 KiB command buffer */
#define CHUNK    4096

struct bd_net {
	bd_net_data_cb on_data;
	bd_net_state_cb on_state;
	bd_net_echo_cb on_echo;
	bd_net_prompt_cb on_prompt;
	void *arg;

	_Atomic int state;
	_Atomic int shutdown;

	bd_ring *rx;            /* net -> UI */
	bd_ring *tx;            /* UI -> net */
	int wake_r, wake_w;

	pthread_t thread;
	int have_thread;
};

/* Net-thread-only working state. */
struct net_ctx {
	bd_net *n;
	struct iox_loop *loop;
	bd_telopt *telopt;
	int fd;
	int connecting;
	unsigned want;
	int rx_blocked;         /* rx ring full: reads paused */
	int quit;

	unsigned char *out;     /* bytes owed to the socket */
	size_t out_len, out_cap, out_sent;

	unsigned char *pend;    /* framed rx records not yet in the ring */
	size_t pend_len, pend_cap, pend_sent;
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
		return 0;

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
	(void)rc;
}

/* ---- net thread: staging buffer -> rx ring ---------------------------- */

/* Append a framed record to the pending staging buffer. */
static void
pend_put(struct net_ctx *c, uint8_t type, const void *payload, uint32_t len)
{
	size_t need = c->pend_len + 5 + len;

	if (c->pend_sent == c->pend_len)
		c->pend_sent = c->pend_len = 0;
	if (need > c->pend_cap) {
		size_t cap = c->pend_cap ? c->pend_cap * 2 : 8192;
		unsigned char *nb;
		while (cap < need)
			cap *= 2;
		nb = realloc(c->pend, cap);
		if (!nb)
			return;         /* drop on OOM */
		c->pend = nb;
		c->pend_cap = cap;
	}
	c->pend[c->pend_len++] = type;
	memcpy(c->pend + c->pend_len, &len, 4);
	c->pend_len += 4;
	if (len) {
		memcpy(c->pend + c->pend_len, payload, len);
		c->pend_len += len;
	}
}

/* Stream as many staged bytes into the rx ring as fit. Partial records are
 * fine: the consumer waits until a record is complete. */
static void
drain_pend(struct net_ctx *c)
{
	size_t avail = bd_ring_write_avail(c->n->rx);
	size_t rem = c->pend_len - c->pend_sent;
	size_t k = rem < avail ? rem : avail;

	if (k) {
		bd_ring_write(c->n->rx, c->pend + c->pend_sent, k);
		c->pend_sent += k;
	}
	if (c->pend_sent == c->pend_len) {
		c->pend_sent = c->pend_len = 0;
		c->rx_blocked = 0;
	} else {
		c->rx_blocked = 1;
	}
}

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
	pend_put(c, MSG_STATE, frame, len);
	drain_pend(c);
}

/* ---- net thread: socket out buffer ------------------------------------ */

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

/* ---- bd_telopt callbacks (run on the net thread) ---------------------- */

static void
telopt_data(const unsigned char *p, size_t len, void *arg)
{
	struct net_ctx *c = arg;
	pend_put(c, MSG_DATA, p, (uint32_t)len);
}

static void
telopt_xmit(const unsigned char *p, size_t len, void *arg)
{
	struct net_ctx *c = arg;
	out_push(c, p, len);
}

static void
telopt_prompt(void *arg)
{
	struct net_ctx *c = arg;
	pend_put(c, MSG_PROMPT, NULL, 0);
}

static void
telopt_echo(int suppress, void *arg)
{
	struct net_ctx *c = arg;
	unsigned char b = suppress ? 1 : 0;
	pend_put(c, MSG_ECHO, &b, 1);
}

/* ---- net thread: I/O -------------------------------------------------- */

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
	bd_telopt_reset(c->telopt);
	c->pend_len = c->pend_sent = 0;

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
			return;
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
		return;

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
		ssize_t r;

		/* flush staged records before pulling more off the socket */
		if (c->pend_sent < c->pend_len) {
			drain_pend(c);
			if (c->pend_sent < c->pend_len)
				return;         /* rx still full */
		}
		if (bd_ring_write_avail(c->n->rx) == 0) {
			c->rx_blocked = 1;
			return;
		}

		r = recv(c->fd, buf, sizeof buf, 0);
		if (r > 0) {
			bd_telopt_recv(c->telopt, buf, (size_t)r);
			drain_pend(c);
			if (c->pend_sent < c->pend_len)
				return;         /* rx filled mid-record */
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
		case MSG_WINSIZE:
			if (len >= 4) {
				uint16_t cols, rows;
				memcpy(&cols, buf, 2);
				memcpy(&rows, buf + 2, 2);
				bd_telopt_set_winsize(c->telopt, cols, rows);
				update_interest(c);    /* NAWS may have queued out */
			}
			break;
		case MSG_TERMTYPE:
			buf[len < sizeof buf ? len : sizeof buf - 1] = '\0';
			bd_telopt_set_termtype(c->telopt, (char *)buf);
			break;
		}
	}

	if (atomic_load(&c->n->shutdown)) {
		c->quit = 1;
		return;
	}

	/* UI may have drained rx: flush staged records and resume reading */
	if (c->rx_blocked) {
		drain_pend(c);
		if (!c->rx_blocked)
			update_interest(c);
	}
}

static void
net_thread_main_inner(bd_net *n, struct net_ctx *c)
{
	bd_telopt_cb tcb;

	memset(c, 0, sizeof *c);
	c->n = n;
	c->fd = -1;

	tcb.data = telopt_data;
	tcb.xmit = telopt_xmit;
	tcb.prompt = telopt_prompt;
	tcb.echo = telopt_echo;
	tcb.arg = c;

	c->loop = iox_loop_new();
	if (!c->loop)
		return;
	c->telopt = bd_telopt_new(&tcb);
	if (!c->telopt) {
		iox_loop_free(c->loop);
		c->loop = NULL;
		return;
	}
	iox_fd_add(c->loop, n->wake_r, IOX_READ, wake_cb, c);
	iox_loop_start(c->loop);

	while (!c->quit) {
		if (iox_loop_poll(c->loop) < 0 && errno != EINTR)
			break;
	}
}

static void *
net_thread_main(void *arg)
{
	bd_net *n = arg;
	struct net_ctx c;

	net_thread_main_inner(n, &c);

	if (c.fd >= 0)
		close(c.fd);
	free(c.out);
	free(c.pend);
	bd_telopt_free(c.telopt);
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

void
bd_net_set_echo_cb(bd_net *n, bd_net_echo_cb cb)
{
	if (n)
		n->on_echo = cb;
}

void
bd_net_set_prompt_cb(bd_net *n, bd_net_prompt_cb cb)
{
	if (n)
		n->on_prompt = cb;
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
			return len - left;
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
bd_net_set_termtype(bd_net *n, const char *type)
{
	size_t l;

	if (!n || !type)
		return;
	l = strlen(type);
	if (l > 63)
		l = 63;
	if (ring_put(n->tx, MSG_TERMTYPE, type, (uint32_t)l) == 0)
		wake(n);
}

void
bd_net_set_winsize(bd_net *n, int cols, int rows)
{
	unsigned char p[4];
	uint16_t c = (uint16_t)cols, r = (uint16_t)rows;

	if (!n || cols <= 0 || rows <= 0)
		return;
	memcpy(p, &c, 2);
	memcpy(p + 2, &r, 2);
	if (ring_put(n->tx, MSG_WINSIZE, p, sizeof p) == 0)
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
		switch (type) {
		case MSG_DATA:
			if (n->on_data)
				n->on_data((const char *)buf, (int)len, n->arg);
			got_data = 1;
			break;
		case MSG_STATE: {
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
			break;
		}
		case MSG_PROMPT:
			if (n->on_prompt)
				n->on_prompt(n->arg);
			break;
		case MSG_ECHO:
			if (n->on_echo)
				n->on_echo(len >= 1 && buf[0], n->arg);
			break;
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
